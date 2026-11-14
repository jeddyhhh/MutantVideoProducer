// Hardening: tries to break the core with what real users (and real disks)
// throw at it, and checks that nothing crashes, hangs or goes wrong quietly.
//   mvp_selftest harden <testmedia-dir> <examples-dir> <outdir> [iterations]
// Damaged and odd media, mangled project files, random editing with undo and
// redo, export edge cases, rendering on several threads, memory over time.
// Each fuzzed case prints its seed; a failing project file is kept in outdir.

#include "core/AudioMixer.h"
#include "core/Captions.h"
#include "core/CubeLut.h"
#include "core/Compositor.h"
#include "core/Cuts.h"
#include "core/Effects.h"
#include "core/AudioEffects.h"
#include "core/Exporter.h"
#include "core/Generators.h"
#include "core/Graphics.h"
#include "core/MotionGraphics.h"
#include "core/NodeGraph.h"
#include "core/MediaInfo.h"
#include "core/Multicam.h"
#include "core/Project.h"
#include "core/Story.h"
#include "core/TimelineOps.h"
#include "core/VideoDecoder.h"
#include "core/AudioDecoder.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#else
#include <execinfo.h>
#include <unistd.h>
#endif

namespace {

int g_failures = 0;

void print(const QString& s)
{
    std::fputs(s.toUtf8().constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void check(bool ok, const QString& what)
{
    print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
    if (!ok)
        ++g_failures;
}

// ---- Watchdog: a stage that takes far too long is a hang ----
std::mutex g_stageMutex;
QString g_stage;
std::atomic<int64_t> g_deadline{0};

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void stage(const QString& name, int seconds)
{
    std::lock_guard lock(g_stageMutex);
    g_stage = name;
    g_deadline = nowMs() + seconds * 1000LL;
}

// A crash names the stage it happened in (and its seed), then ends the run.
void crashed(const char* what)
{
    std::fprintf(stdout, "FAIL  CRASH (%s) in \"%s\"\n", what, g_stage.toUtf8().constData());
    std::fflush(stdout);
#ifndef Q_OS_WIN
    void* frames[48]; // where (addr2line -f -C -e mvp_selftest <offset> names them)
    backtrace_symbols_fd(frames, backtrace(frames, 48), 1);
#endif
    std::_Exit(4);
}

#ifdef Q_OS_WIN
LONG WINAPI onException(EXCEPTION_POINTERS* e)
{
    char what[64];
    std::snprintf(what, sizeof what, "exception 0x%08lx", e->ExceptionRecord->ExceptionCode);
    crashed(what);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void startWatchdog()
{
#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(onException);
    ULONG stack = 64 * 1024; // room to report a stack overflow
    SetThreadStackGuarantee(&stack);
#endif
    std::signal(SIGSEGV, [](int) { crashed("segmentation fault"); });
    std::signal(SIGABRT, [](int) { crashed("abort"); });
    std::signal(SIGFPE, [](int) { crashed("arithmetic error"); });
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (g_deadline && nowMs() > g_deadline) {
                std::lock_guard lock(g_stageMutex);
                print(QStringLiteral("FAIL  HANG: \"%1\" did not finish in time").arg(g_stage));
                std::_Exit(3);
            }
        }
    }).detach();
}

qint64 memoryBytes()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        return qint64(pmc.PrivateUsage);
    return 0;
#else
    QFile f(QStringLiteral("/proc/self/statm"));
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QList<QByteArray> parts = f.readAll().split(' ');
    return parts.size() > 1 ? parts[1].toLongLong() * sysconf(_SC_PAGESIZE) : 0;
#endif
}

QString ffmpegExe()
{
    const QString local = QDir(QCoreApplication::applicationDirPath())
                              .filePath(
#ifdef Q_OS_WIN
                                  QStringLiteral("ffmpeg.exe"));
#else
                                  QStringLiteral("ffmpeg"));
#endif
    if (QFileInfo::exists(local))
        return local;
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

bool runFfmpeg(const QStringList& args)
{
    QProcess p;
    p.start(ffmpegExe(),
            QStringList{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y")} + args);
    return p.waitForFinished(120000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

std::shared_ptr<MediaItem> mediaItem(int id, const QString& path)
{
    auto m = std::make_shared<MediaItem>();
    m->id = id;
    m->path = path;
    m->info = probeMedia(path);
    return m;
}

// A snapshot with each usable file placed one after the other.
ProjectSnapshot snapshotOf(const QStringList& paths, QSize size = {640, 360})
{
    ProjectSnapshot snap;
    snap.settings.width = size.width();
    snap.settings.height = size.height();
    snap.timeline = defaultTimeline();
    TimeUs at = 0;
    int id = 1;
    for (const QString& p : paths) {
        auto m = mediaItem(id++, p);
        snap.media[m->id] = m;
        if (!m->info.ok())
            continue;
        ops::insertMedia(snap.timeline, *m, at, -1, snap.settings.fps);
        at = snap.timeline.duration();
    }
    return snap;
}

QString settingsProblem(const ProjectSettings& s)
{
    if (s.width < 16 || s.height < 16 || s.width > 16384 || s.height > 16384)
        return QStringLiteral("size %1x%2").arg(s.width).arg(s.height);
    if (!s.fps.valid() || s.fps.toDouble() > 1000)
        return QStringLiteral("frame rate %1/%2").arg(s.fps.num).arg(s.fps.den);
    if (s.sampleRate < 8000 || s.sampleRate > 384000)
        return QStringLiteral("sample rate %1").arg(s.sampleRate);
    return {};
}

// Renders and mixes a few points of a snapshot (small, quick). False if a
// picture came back empty.
QString g_exerciseFailure; // what exercise() last found wrong

bool exercise(const ProjectSnapshot& snap, Compositor& comp, AudioMixer& mixer)
{
    g_exerciseFailure.clear();
    const TimeUs d = snap.timeline.duration();
    bool ok = true;
    std::vector<float> buf(2 * 2048);
    for (TimeUs t : {TimeUs(0), d / 3, d / 2, d - 1, d, d + kUsPerSecond}) {
        t = std::max<TimeUs>(0, t);
        const QImage img = comp.render(snap, t, QSize(160, 90));
        if (img.isNull() || img.size() != QSize(160, 90)) {
            ok = false;
            g_exerciseFailure = QStringLiteral("no picture at %1 s").arg(t / 1e6);
        }
        mixer.mix(snap, usToSamples(t, mixer.sampleRate()), 2048, buf.data());
        if (!std::all_of(buf.begin(), buf.end(), [](float f) { return std::isfinite(f); })) {
            ok = false;
            g_exerciseFailure = QStringLiteral("sound not a number at %1 s").arg(t / 1e6);
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// 1. Damaged files: cut short, scribbled over, empty, not what they claim.
// ---------------------------------------------------------------------------
void damagedMedia(const QString& mediaDir, const QString& outDir)
{
    const QString dir = QDir(outDir).filePath(QStringLiteral("damaged"));
    QDir().mkpath(dir);
    const QStringList sources = {QStringLiteral("h264_aac.mp4"),    QStringLiteral("vp9_vorbis.webm"), QStringLiteral("hevc_opus.mkv"),
                                 QStringLiteral("mpeg2_mp2.ts"),    QStringLiteral("audio.mp3"),       QStringLiteral("audio.wav"),
                                 QStringLiteral("still.png"),       QStringLiteral("still.jpg"),       QStringLiteral("animated.gif"),
                                 QStringLiteral("prores_pcm24.mov"), QStringLiteral("interlaced_mpeg2.ts"), QStringLiteral("hdr_pq.mkv"),
                                 QStringLiteral("hdr_hlg.mp4")};
    std::mt19937 rng(1234);
    int made = 0, opened = 0;
    QStringList failures;
    for (const QString& name : sources) {
        QFile f(QDir(mediaDir).filePath(name));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray data = f.readAll();
        const QString ext = QFileInfo(name).suffix();
        std::vector<std::pair<QString, QByteArray>> cases;
        cases.emplace_back(QStringLiteral("empty"), QByteArray());
        cases.emplace_back(QStringLiteral("one_byte"), data.left(1));
        for (double cut : {0.01, 0.1, 0.5, 0.9, 0.999})
            cases.emplace_back(QStringLiteral("cut%1").arg(int(cut * 1000)), data.left(qsizetype(data.size() * cut)));
        {
            QByteArray z = data; // a run of zeros in the middle (a bad sector)
            const qsizetype at = z.size() / 3, n = std::min<qsizetype>(z.size() / 5, 65536);
            z.replace(at, n, QByteArray(n, '\0'));
            cases.emplace_back(QStringLiteral("zeroed"), z);
        }
        for (int k = 0; k < 3; ++k) { // random bytes flipped all through
            QByteArray b = data;
            const int flips = 20 << (k * 3);
            for (int i = 0; i < flips && !b.isEmpty(); ++i)
                b[qsizetype(rng() % uint32_t(b.size()))] = char(rng());
            cases.emplace_back(QStringLiteral("flipped%1").arg(flips), b);
        }
        {
            QByteArray head = data.left(64); // the right header, then noise
            QByteArray noise(std::max<qsizetype>(4096, data.size() / 4), '\0');
            for (auto& c : noise)
                c = char(rng());
            cases.emplace_back(QStringLiteral("header_then_noise"), head + noise);
        }
        for (const auto& [label, bytes] : cases) {
            const QString path = QDir(dir).filePath(QStringLiteral("%1_%2.%3").arg(QFileInfo(name).baseName(), label, ext));
            QFile o(path);
            if (!o.open(QIODevice::WriteOnly))
                continue;
            o.write(bytes);
            o.close();
            ++made;
            stage(QStringLiteral("damaged file %1").arg(QFileInfo(path).fileName()), 60);
            ProjectSnapshot snap = snapshotOf({path});
            if (snap.media.begin()->second->info.ok())
                ++opened;
            Compositor comp;
            AudioMixer mixer(48000);
            if (!exercise(snap, comp, mixer))
                failures << QFileInfo(path).fileName();
            // Straight decoding to the end and past it, and seeking around.
            const MediaInfo& info = snap.media.begin()->second->info;
            if (info.hasVideo()) {
                VideoDecoder dec;
                if (dec.open(path, info.video.front().index, nullptr, false))
                    for (TimeUs t : {TimeUs(0), info.duration / 2, info.duration, info.duration * 2, TimeUs(0), info.duration - 1})
                        dec.frameAt(std::max<TimeUs>(0, t));
            }
            if (info.hasAudio()) {
                AudioDecoder dec;
                dec.open(path, info.audio.front().index, 48000);
            }
        }
    }
    check(failures.isEmpty(),
          QStringLiteral("damaged files: %1 made from %2 good ones (%3 still open); none crashed or hung, every picture came back%4")
              .arg(made)
              .arg(sources.size())
              .arg(opened)
              .arg(failures.isEmpty() ? QString() : QStringLiteral(" (not: %1)").arg(failures.join(QStringLiteral(", ")))));

    // A file that changes under us: probed, then cut short before rendering
    // (a recording still being written, a download that failed).
    const QString live = QDir(dir).filePath(QStringLiteral("shrinks.mp4"));
    QFile::remove(live);
    QFile::copy(QDir(mediaDir).filePath(QStringLiteral("h264_aac.mp4")), live);
    stage(QStringLiteral("a file cut short after it was probed"), 60);
    ProjectSnapshot snap = snapshotOf({live});
    {
        QFile f(live);
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        f.resize(f.size() / 3);
    }
    Compositor comp;
    AudioMixer mixer(48000);
    check(exercise(snap, comp, mixer),
          QStringLiteral("a file cut to a third after it was placed still renders (the rest is missing, nothing crashes)"));
    QFile::remove(live);
    stage(QStringLiteral("a file deleted after it was placed"), 60);
    Compositor comp2;
    AudioMixer mixer2(48000);
    check(exercise(snap, comp2, mixer2), QStringLiteral("a file deleted after it was placed renders as missing"));
}

// ---------------------------------------------------------------------------
// 2. Odd but valid media: tiny, huge, odd sizes, one frame, many channels,
//    variable frame rate, names in other scripts.
// ---------------------------------------------------------------------------
void oddMedia(const QString& outDir)
{
    if (ffmpegExe().isEmpty()) {
        print(QStringLiteral("SKIP  odd media: no ffmpeg to make it"));
        return;
    }
    const QString dir = QDir(outDir).filePath(QStringLiteral("odd"));
    QDir().mkpath(dir);
    auto p = [&](const QString& n) { return QDir(dir).filePath(n); };
    const QString lavfi = QStringLiteral("lavfi");
    struct Case {
        QString name;
        QStringList args;
    };
    const std::vector<Case> cases = {
        {QStringLiteral("1x1.mkv"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("color=c=orange:s=1x1:d=1"), QStringLiteral("-c:v"),
          QStringLiteral("png")}},
        {QStringLiteral("odd_3x7.mkv"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("color=c=orange:s=3x7:d=1"), QStringLiteral("-c:v"),
          QStringLiteral("png")}},
        {QStringLiteral("odd_641x479.mp4"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("testsrc2=s=641x479:d=1"), QStringLiteral("-c:v"),
          QStringLiteral("mpeg4")}},
        {QStringLiteral("wide_4000x16.mkv"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("testsrc2=s=4000x16:d=1"), QStringLiteral("-c:v"),
          QStringLiteral("ffv1")}},
        {QStringLiteral("tall_16x4000.mkv"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("testsrc2=s=16x4000:d=1"), QStringLiteral("-c:v"),
          QStringLiteral("ffv1")}},
        {QStringLiteral("one_frame.mp4"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("testsrc2=s=320x240:r=30"), QStringLiteral("-frames:v"),
          QStringLiteral("1"), QStringLiteral("-c:v"), QStringLiteral("mpeg4")}},
        {QStringLiteral("fps_1000.mkv"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("testsrc2=s=160x120:r=1000:d=0.5"), QStringLiteral("-c:v"),
          QStringLiteral("ffv1")}},
        {QStringLiteral("fps_1.mkv"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("testsrc2=s=160x120:r=1:d=3"), QStringLiteral("-c:v"),
          QStringLiteral("ffv1")}},
        {QStringLiteral("vfr.mkv"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("testsrc2=s=320x240:r=30:d=3"), QStringLiteral("-vf"),
          QStringLiteral("setpts='if(lt(N,30),N/30/TB,if(lt(N,60),(1+(N-30)/5)/TB,(7+(N-60)/60)/TB))'"), QStringLiteral("-fps_mode"),
          QStringLiteral("vfr"), QStringLiteral("-c:v"), QStringLiteral("ffv1")}},
        {QStringLiteral("starts_late.mp4"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("testsrc2=s=320x240:d=2"), QStringLiteral("-output_ts_offset"),
          QStringLiteral("10"), QStringLiteral("-c:v"), QStringLiteral("mpeg4")}},
        {QStringLiteral("audio_1sample.wav"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("sine=r=48000"), QStringLiteral("-frames:a"),
          QStringLiteral("1"), QStringLiteral("-af"), QStringLiteral("atrim=end_sample=1")}},
        {QStringLiteral("audio_8k_mono.wav"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("sine=r=8000:d=1"), QStringLiteral("-ac"),
          QStringLiteral("1")}},
        {QStringLiteral("audio_384k_8ch.wav"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("sine=r=384000:d=0.5"), QStringLiteral("-ac"),
          QStringLiteral("8")}},
        {QStringLiteral("audio_silence_long.flac"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("anullsrc=r=44100:cl=mono"), QStringLiteral("-t"),
          QStringLiteral("600")}},
        {QStringLiteral("video_no_audio_but_audio_stream_empty.mkv"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("testsrc2=s=320x240:d=2"), QStringLiteral("-f"), lavfi,
          QStringLiteral("-i"), QStringLiteral("anullsrc=r=48000"), QStringLiteral("-t"), QStringLiteral("2"), QStringLiteral("-c:v"),
          QStringLiteral("ffv1"), QStringLiteral("-c:a"), QStringLiteral("pcm_s16le"), QStringLiteral("-af"),
          QStringLiteral("atrim=end=0.001")}},
        {QStringLiteral("two_video_streams.mkv"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("testsrc2=s=320x240:d=1"), QStringLiteral("-f"), lavfi,
          QStringLiteral("-i"), QStringLiteral("testsrc=s=64x48:d=3"), QStringLiteral("-map"), QStringLiteral("0"), QStringLiteral("-map"),
          QStringLiteral("1"), QStringLiteral("-c:v"), QStringLiteral("ffv1")}},
        {QStringLiteral("huge_still.png"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("color=c=red:s=12000x8000"), QStringLiteral("-frames:v"),
          QStringLiteral("1")}},
        {QStringLiteral("rgba64.png"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("testsrc2=s=300x200"), QStringLiteral("-frames:v"),
          QStringLiteral("1"), QStringLiteral("-pix_fmt"), QStringLiteral("rgba64be")}},
        {QStringLiteral("gray16.tiff"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("testsrc2=s=300x200"), QStringLiteral("-frames:v"),
          QStringLiteral("1"), QStringLiteral("-pix_fmt"), QStringLiteral("gray16le")}},
        {QStringLiteral("Ünïcødé 日本語 ✓ clip.mp4"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("testsrc2=s=320x240:d=1"), QStringLiteral("-c:v"),
          QStringLiteral("mpeg4")}},
        {QStringLiteral("name with %20 & ; ' $HOME #1.mp4"),
         {QStringLiteral("-f"), lavfi, QStringLiteral("-i"), QStringLiteral("testsrc2=s=320x240:d=1"), QStringLiteral("-c:v"),
          QStringLiteral("mpeg4")}},
    };
    QStringList made, unusable, bad;
    for (const Case& c : cases) {
        stage(QStringLiteral("making %1").arg(c.name), 180);
        if (!runFfmpeg(c.args + QStringList{p(c.name)})) {
            print(QStringLiteral("SKIP  %1: ffmpeg could not make it").arg(c.name));
            continue;
        }
        made << c.name;
        stage(QStringLiteral("odd media %1").arg(c.name), 120);
        ProjectSnapshot snap = snapshotOf({p(c.name)});
        const auto& m = snap.media.begin()->second;
        if (!m->info.ok()) {
            unusable << QStringLiteral("%1 (%2)").arg(c.name, m->info.error);
            continue;
        }
        Compositor comp(true);
        AudioMixer mixer(48000);
        if (!exercise(snap, comp, mixer) || timelineProblem(snap.timeline).size())
            bad << c.name;
    }
    check(bad.isEmpty(),
          QStringLiteral("odd media: %1 files (1x1, 3x7, 4000x16, one frame, 1 and 1000 fps, variable rate, a late start, 1 audio sample, "
                         "384 kHz 8 channels, 10 min of silence, two video streams, a 12000x8000 still, 16-bit pictures, other scripts and "
                         "symbols in the name) render and mix%2")
              .arg(made.size())
              .arg(bad.isEmpty() ? QString() : QStringLiteral(": not %1").arg(bad.join(QStringLiteral(", ")))));
    if (!unusable.isEmpty())
        print(QStringLiteral("NOTE  refused at import (fine if the message is clear): %1").arg(unusable.join(QStringLiteral("; "))));
}

// ---------------------------------------------------------------------------
// 3. Mangled project files: values out of range or of the wrong type, keys
//    missing, the file cut short or scribbled over.
// ---------------------------------------------------------------------------
QJsonValue extremeValue(std::mt19937& rng)
{
    switch (rng() % 18) {
    case 0: return -1;
    case 1: return 0;
    case 2: return 1e18;
    case 3: return -1e18;
    case 4: return 2147483647;
    case 5: return -2147483648.0;
    case 6: return 1e308;
    case 7: return -1e308;
    case 8: return QStringLiteral("garbage");
    case 9: return QStringLiteral("-9223372036854775808");
    case 10: return QStringLiteral("99999999999999999999");
    case 11: return QJsonArray{1, 2, 3};
    case 12: return QJsonObject{{QStringLiteral("x"), 1}};
    case 13: return QJsonValue(QJsonValue::Null);
    case 14: return true;
    case 15: return 0.5;
    case 16: return QStringLiteral("");
    default: return QJsonValue(QJsonValue::Undefined); // removed
    }
}

int countNodes(const QJsonValue& v)
{
    int n = 1;
    if (v.isObject())
        for (const auto& c : v.toObject())
            n += countNodes(c);
    else if (v.isArray())
        for (const auto& c : v.toArray())
            n += countNodes(c);
    return n;
}

// Replaces node number `target` (pre-order) with `with`; Undefined removes it.
QJsonValue replaceNode(const QJsonValue& v, int& target, const QJsonValue& with)
{
    if (target-- == 0)
        return with;
    if (v.isObject()) {
        QJsonObject o = v.toObject();
        for (const QString& k : o.keys()) {
            if (target < 0)
                break;
            const QJsonValue r = replaceNode(o[k], target, with);
            if (r.isUndefined())
                o.remove(k);
            else
                o[k] = r;
        }
        return o;
    }
    if (v.isArray()) {
        QJsonArray a = v.toArray();
        for (qsizetype i = 0; i < a.size() && target >= 0; ++i) {
            const QJsonValue r = replaceNode(a[i], target, with);
            if (r.isUndefined())
                a.removeAt(i--);
            else
                a[i] = r;
        }
        return a;
    }
    return v;
}

// Loads one candidate file and puts the result through its paces. Returns
// a description of what went wrong (empty if nothing).
QString tryProject(const QString& path)
{
    Project project;
    QString error;
    if (!project.load(path, &error))
        return error.isEmpty() ? QStringLiteral("refused without saying why") : QString();
    if (const QString s = settingsProblem(project.settings()); !s.isEmpty())
        return QStringLiteral("loaded with a bad setting: %1").arg(s);
    if (const QString t = timelineProblem(project.timeline()); !t.isEmpty())
        return QStringLiteral("loaded with %1").arg(t);
    auto snap = project.snapshot();
    Compositor comp;
    AudioMixer mixer(48000);
    if (!exercise(*snap, comp, mixer))
        return g_exerciseFailure;
    // Edits on it: split everything in the middle, ripple out the first event.
    Timeline tl = project.timeline();
    const TimeUs mid = tl.duration() / 2;
    ops::splitClips(tl, {}, mid);
    for (const Track& t : tl.tracks)
        if (!t.clips.empty()) {
            ops::rippleRemoveClips(tl, {t.clips.front().id});
            break;
        }
    if (const QString t = timelineProblem(tl); !t.isEmpty())
        return QStringLiteral("after editing: %1").arg(t);
    project.applyEdit(tl, QStringLiteral("fuzz"));
    // It must save and come back the same.
    const QString again = path + QStringLiteral(".again.mvp");
    if (!project.save(again, &error))
        return QStringLiteral("could not save: %1").arg(error);
    Project back;
    if (!back.load(again, &error))
        return QStringLiteral("its own save does not load: %1").arg(error);
    if (!(back.timeline() == project.timeline()) || !(back.settings() == project.settings())) {
        // Say where: the first line that differs between the two saves.
        const QString twice = path + QStringLiteral(".twice.mvp");
        back.save(twice, &error);
        QFile a(again), b(twice);
        a.open(QIODevice::ReadOnly);
        b.open(QIODevice::ReadOnly);
        const QList<QByteArray> la = a.readAll().split('\n'), lb = b.readAll().split('\n');
        for (qsizetype i = 0; i < std::min(la.size(), lb.size()); ++i)
            if (la[i] != lb[i])
                return QStringLiteral("saved and loaded it is not the same: line %1 \"%2\" became \"%3\"")
                    .arg(i + 1)
                    .arg(QString::fromUtf8(la[i].trimmed()), QString::fromUtf8(lb[i].trimmed()));
        const Timeline& x = project.timeline();
        const Timeline& y = back.timeline();
        QString where = !(project.settings() == back.settings())                                       ? QStringLiteral("the settings")
            : !(x.markers == y.markers)                                                                ? QStringLiteral("the markers")
            : !(x.outputEffects == y.outputEffects) || !(x.audioMasterEffects == y.audioMasterEffects) ? QStringLiteral("the output FX")
            : !(x.liveEffects == y.liveEffects) || !(x.livePads == y.livePads) || !(x.liveLayerEffects == y.liveLayerEffects)
            ? QStringLiteral("the Live FX or pads")
            : !(x.liveOverlays == y.liveOverlays) || !(x.liveScenes == y.liveScenes) || !(x.liveSetlist == y.liveSetlist)
            ? QStringLiteral("the Live overlays, scenes or setlist")
            : x.nextId != y.nextId ? QStringLiteral("nextId %1 / %2").arg(x.nextId).arg(y.nextId)
                                   : QStringLiteral("not in the tracks");
        const auto& ta = project.timeline().tracks;
        const auto& tb = back.timeline().tracks;
        for (size_t i = 0; i < std::min(ta.size(), tb.size()) && where.startsWith(QLatin1String("not")); ++i) {
            if (!(ta[i].effects == tb[i].effects))
                where = QStringLiteral("track %1's effects").arg(i);
            else if (ta[i].id != tb[i].id || ta[i].level != tb[i].level || ta[i].pan != tb[i].pan || ta[i].duckUnder != tb[i].duckUnder
                     || ta[i].duckAmount != tb[i].duckAmount || ta[i].blend != tb[i].blend || ta[i].name != tb[i].name
                     || ta[i].clips.size() != tb[i].clips.size())
                where = QStringLiteral("track %1's settings (id %2/%3, level %4/%5, pan %6/%7, duck %8/%9, %10/%11 events)")
                            .arg(i)
                            .arg(ta[i].id)
                            .arg(tb[i].id)
                            .arg(ta[i].level)
                            .arg(tb[i].level)
                            .arg(ta[i].pan)
                            .arg(tb[i].pan)
                            .arg(ta[i].duckUnder)
                            .arg(tb[i].duckUnder)
                            .arg(ta[i].clips.size())
                            .arg(tb[i].clips.size());
            else if (!(ta[i].volume == tb[i].volume) || !(ta[i].panEnvelope == tb[i].panEnvelope) || !(ta[i].composite == tb[i].composite))
                where = QStringLiteral("track %1's envelopes").arg(i);
            for (size_t k = 0; k < std::min(ta[i].clips.size(), tb[i].clips.size()); ++k) {
                const Clip& a = ta[i].clips[k];
                const Clip& b = tb[i].clips[k];
                if (a == b)
                    continue;
                where = !(a.effects == b.effects)                                                  ? QStringLiteral("effects")
                    : !(a.generator == b.generator)                                                ? QStringLiteral("generator")
                    : !(a.panCrop == b.panCrop)                                                    ? QStringLiteral("pan/crop")
                    : !(a.velocity == b.velocity)                                                  ? QStringLiteral("speed ramp")
                    : !(a.transitionIn == b.transitionIn) || !(a.transitionOut == b.transitionOut) ? QStringLiteral("transition")
                                                                                                   : QStringLiteral("a plain field");
                where = QStringLiteral("event %1's %2").arg(a.id).arg(where);
                break;
            }
        }
        return QStringLiteral("saved and loaded it is not the same (the files match): %1").arg(where);
    }
    QFile::remove(again);
    return {};
}

void projectFuzz(const QString& examplesDir, const QString& outDir, int iterations)
{
    const QString dir = QDir(outDir).filePath(QStringLiteral("projects"));
    QDir().mkpath(dir);
    QStringList bases;
    for (const QFileInfo& fi : QDir(examplesDir).entryInfoList({QStringLiteral("*.mvp")}, QDir::Files, QDir::Name)) {
        if (fi.fileName().startsWith(QLatin1String("fuzz-"))) // left by an earlier run that stopped
            QFile::remove(fi.absoluteFilePath());
        else
            bases << fi.absoluteFilePath();
    }
    if (bases.isEmpty()) {
        check(false, QStringLiteral("project fuzz: no example projects in %1").arg(examplesDir));
        return;
    }
    QStringList problems;
    int loaded = 0;
    const uint32_t onlySeed = qEnvironmentVariableIntValue("MVP_FUZZ_SEED"); // to look at one case again
    for (int i = 0; i < iterations; ++i) {
        const uint32_t seed = onlySeed ? onlySeed : uint32_t(i) * 2654435761u + 7;
        if (onlySeed && i > 0)
            break;
        std::mt19937 rng(seed);
        const QString base = bases[int(rng() % uint32_t(bases.size()))];
        QFile f(base);
        f.open(QIODevice::ReadOnly);
        QByteArray bytes = f.readAll();
        const int kind = int(rng() % 10);
        if (kind < 7) { // 1-6 values replaced
            QJsonValue root = QJsonDocument::fromJson(bytes).object();
            const int changes = 1 + int(rng() % 6);
            for (int k = 0; k < changes; ++k) {
                int target = 1 + int(rng() % uint32_t(countNodes(root) - 1)); // never the root itself
                root = replaceNode(root, target, extremeValue(rng));
            }
            bytes = QJsonDocument(root.toObject()).toJson();
        } else if (kind == 7) { // cut short
            bytes.truncate(qsizetype(rng() % uint32_t(bytes.size())));
        } else if (kind == 8) { // bytes scribbled over
            for (int k = 0; k < 8; ++k)
                bytes[qsizetype(rng() % uint32_t(bytes.size()))] = char(rng());
        } else { // a valid file with an event duplicated (same id twice)
            QJsonObject root = QJsonDocument::fromJson(bytes).object();
            QJsonArray tracks = root[QStringLiteral("tracks")].toArray();
            for (qsizetype t = 0; t < tracks.size(); ++t) {
                QJsonObject to = tracks[t].toObject();
                QJsonArray clips = to[QStringLiteral("clips")].toArray();
                if (clips.isEmpty())
                    continue;
                clips.append(clips[0]);
                to[QStringLiteral("clips")] = clips;
                tracks[t] = to;
                break;
            }
            root[QStringLiteral("tracks")] = tracks;
            root[QStringLiteral("nextId")] = 1;
            bytes = QJsonDocument(root).toJson();
        }
        // Next to the example, so its media (relative paths) is found.
        const QString path = QDir(examplesDir).filePath(QStringLiteral("fuzz-%1.mvp").arg(seed));
        QFile o(path);
        o.open(QIODevice::WriteOnly);
        o.write(bytes);
        o.close();
        stage(QStringLiteral("project fuzz seed %1 (from %2)").arg(seed).arg(QFileInfo(base).fileName()), 120);
        const QString problem = tryProject(path);
        if (!problem.isEmpty()) {
            const QString keep = QDir(dir).filePath(QFileInfo(path).fileName());
            QFile::remove(keep);
            QFile::copy(path, keep);
            if (problems.size() < 12)
                problems << QStringLiteral("seed %1 (%2): %3").arg(seed).arg(QFileInfo(base).fileName(), problem);
            else if (problems.size() == 12)
                problems << QStringLiteral("...");
        } else {
            ++loaded;
        }
        for (const QString& p : {path, path + QStringLiteral(".again.mvp"), path + QStringLiteral(".twice.mvp")})
            QFile::remove(p);
    }
    check(problems.isEmpty(),
          QStringLiteral("project files: %1 mangled copies of the examples (values replaced or removed, cut short, scribbled over, ids "
                         "repeated) are refused with a message or load sound, render, edit and save back the same%2")
              .arg(iterations)
              .arg(problems.isEmpty() ? QString() : QStringLiteral(":\n      ") + problems.join(QStringLiteral("\n      "))));
}

// ---------------------------------------------------------------------------
// 4. Random editing: every kind of edit with odd arguments, checked after
//    each step; then all of it undone and redone.
// ---------------------------------------------------------------------------
void editFuzz(const QString& mediaDir, int iterations)
{
    Project project;
    QStringList files = {QStringLiteral("h264_aac.mp4"), QStringLiteral("audio.wav"), QStringLiteral("still.png"),
                         QStringLiteral("vp9_vorbis.webm")};
    std::vector<int> mediaIds;
    for (const QString& f : files) {
        const MediaInfo info = probeMedia(QDir(mediaDir).filePath(f));
        if (info.ok())
            mediaIds.push_back(project.addMedia(info));
    }
    if (mediaIds.empty()) {
        check(false, QStringLiteral("random editing: none of %1 in %2").arg(files.join(QStringLiteral(", ")), mediaDir));
        return;
    }
    const ProjectSettings settings = project.settings();
    const uint32_t seed = qEnvironmentVariableIntValue("MVP_EDIT_SEED"); // (to try other sequences of edits)
    std::mt19937 rng(seed ? seed : 99);
    auto anyTime = [&]() -> TimeUs {
        switch (rng() % 6) {
        case 0: return 0;
        case 1: return -TimeUs(rng() % 10000000);
        case 2: return TimeUs(rng() % 600) * kUsPerSecond * 60; // up to 10 h
        case 3: return TimeUs(1);
        default: return TimeUs(rng() % 30000000);
        }
    };
    auto someClips = [&](const Timeline& tl) {
        std::set<int> ids;
        for (const Track& t : tl.tracks)
            for (const Clip& c : t.clips)
                if (rng() % 3 == 0)
                    ids.insert(c.id);
        if (rng() % 8 == 0)
            ids.insert(99999); // one that doesn't exist
        return ids;
    };
    const auto& presets = generatorPresets();
    const auto& vfx = videoEffects();
    const auto& afx = audioEffects();
    std::vector<Timeline> history{project.timeline()};
    QString problem;
    int steps = 0;
    Compositor comp;
    AudioMixer mixer(48000);
    for (int i = 0; i < iterations && problem.isEmpty(); ++i) {
        Timeline tl = project.timeline();
        const int op = int(rng() % 36);
        const auto media = project.media();
        stage(QStringLiteral("editing step %1 (op %2)").arg(i).arg(op), 60);
        switch (op) {
        case 0:
        case 1:
        case 2: {
            const int id = mediaIds[rng() % mediaIds.size()];
            ops::insertMedia(tl, *media.at(id), std::max<TimeUs>(0, anyTime()),
                             tl.tracks.empty() ? -1 : int(rng() % (tl.tracks.size() + 1)) - 1, settings.fps);
            break;
        }
        case 3: ops::splitClips(tl, rng() % 2 ? std::set<int>{} : someClips(tl), anyTime()); break;
        case 4: ops::removeClips(tl, someClips(tl)); break;
        case 5: ops::rippleRemoveClips(tl, someClips(tl)); break;
        case 6: ops::rippleRemoveClipsAll(tl, someClips(tl), rng() % 2); break;
        case 7: ops::rippleShift(tl, anyTime(), anyTime() * (rng() % 2 ? 1 : -1), {}, {}, rng() % 2); break;
        case 8: ops::rippleInsert(tl, anyTime(), anyTime(), {}, {}, rng() % 2); break;
        case 9: ops::groupClips(tl, someClips(tl)); break;
        case 10: ops::ungroupClips(tl, someClips(tl)); break;
        case 11: ops::addTrack(tl, rng() % 2 ? TrackType::Video : TrackType::Audio, int(rng() % (tl.tracks.size() + 2)) - 1); break;
        case 12:
            if (!tl.tracks.empty())
                ops::removeTrack(tl, int(rng() % tl.tracks.size()));
            break;
        case 13:
            if (!tl.tracks.empty())
                ops::moveTrack(tl, int(rng() % tl.tracks.size()), int(rng() % tl.tracks.size()));
            break;
        case 14: ops::insertGenerated(tl, presets[rng() % presets.size()], std::max<TimeUs>(0, anyTime()), -1, settings.fps); break;
        case 15:
        case 16:
        case 17: { // per-event changes through the same calls the UI makes
            for (Track& t : tl.tracks)
                for (Clip& c : t.clips) {
                    if (rng() % 4)
                        continue;
                    const MediaItem* m = c.mediaId ? media.at(c.mediaId).get() : nullptr;
                    const int sub = int(rng() % 10);
                    const Clip was = c;
                    switch (sub) {
                    case 0: ops::setClipSpeed(c, m, std::pow(10.0, (int(rng() % 400) - 200) / 100.0), rng() % 2, settings.fps); break;
                    case 1: {
                        auto [lo, hi] = ops::trimRange(c, m, true);
                        lo = std::max(lo, -c.start);
                        hi = std::min(hi, c.length - 1); /* the UI's limits */
                        ops::trimClipStart(c, std::clamp<TimeUs>(anyTime() * (rng() % 2 ? 1 : -1), lo, hi));
                        break;
                    }
                    case 2: {
                        auto [lo, hi] = ops::trimRange(c, m, false);
                        lo = std::max(lo, 1 - c.length);
                        ops::trimClipEnd(c, std::clamp<TimeUs>(anyTime() * (rng() % 2 ? 1 : -1), lo, hi));
                        break;
                    }
                    case 3: ops::applyRampPreset(c, m, ops::RampPreset(rng() % 5), settings.fps); break;
                    case 4: c.reverse = !c.reverse; break;
                    case 5:
                        c.fadeIn = std::clamp<TimeUs>(anyTime(), 0, c.length);
                        c.fadeOut = std::clamp<TimeUs>(anyTime(), 0, c.length);
                        break;
                    case 6:
                        if (t.type == TrackType::Video)
                            c.effects.push_back(makeEffect(vfx[rng() % vfx.size()], tl.allocId()));
                        else
                            c.effects.push_back(makeEffectOfType(afx[rng() % afx.size()].id, tl.allocId()));
                        break;
                    case 7: c.loopLength = rng() % 2 ? 0 : TimeUs(1 + rng() % 3000000); break;
                    case 8:
                        c.frameBlend = !c.frameBlend;
                        c.stabilize = rng() % 2 ? 0.0 : 0.5;
                        break;
                    case 9: // a setting of one of its effects keyframed (later splits, trims and ripples move the keys)
                        if (!c.effects.empty()) {
                            EffectInstance& e = c.effects[rng() % c.effects.size()];
                            const auto meta = effectMeta(e.type);
                            if (!meta || !meta->params)
                                break;
                            for (const ParamDef& d : *meta->params)
                                if (d.type == ParamType::Number && rng() % 2 == 0) {
                                    ParamValue& v = e.params[d.id];
                                    v.keys.clear();
                                    std::set<TimeUs> times;
                                    for (int k = 0; k < 1 + int(rng() % 4); ++k)
                                        times.insert(TimeUs(rng() % uint64_t(std::max<TimeUs>(1, c.length))));
                                    for (TimeUs at : times) {
                                        Keyframe key;
                                        key.time = at;
                                        key.value = d.min + (d.max - d.min) * (rng() % 1001) / 1000.0;
                                        key.interp = Keyframe::Interp(rng() % 9);
                                        v.keys.push_back(key);
                                    }
                                    break;
                                }
                        }
                        break;
                    }
                    if (c.sourceIn < 0 || c.length <= 0 || c.start < 0)
                        print(QStringLiteral("NOTE  change %1 on event %2 (start %3, length %4, in %5, speed %6, reverse %7, ramp %8) gave "
                                             "start %9, length %10, in %11")
                                  .arg(sub)
                                  .arg(c.id)
                                  .arg(was.start)
                                  .arg(was.length)
                                  .arg(was.sourceIn)
                                  .arg(was.speed)
                                  .arg(was.reverse)
                                  .arg(was.hasRamp())
                                  .arg(c.start)
                                  .arg(c.length)
                                  .arg(c.sourceIn));
                }
            tl.sortClips();
            break;
        }
        case 18: { // events trimmed to nothing are what the UI removes; keep that rule here too
            for (const Track& t : tl.tracks)
                for (const Clip& c : t.clips)
                    if (rng() % 5 == 0) {
                        ops::overwriteContained(tl, c.id);
                        break;
                    }
            break;
        }
        case 19: ops::overwriteOverlaps(tl, someClips(tl)); break;
        case 20:
            for (const int id : someClips(tl)) {
                ops::removeTransition(tl, id, rng() % 2);
            }
            break;
        case 21: { // Paste Event Attributes from any event onto some others
            const std::set<int> ids = someClips(tl);
            for (const Track& t : tl.tracks)
                if (!t.clips.empty()) {
                    const Clip from = t.clips[rng() % t.clips.size()];
                    ops::pasteAttributes(tl, media, from, isVisualTrack(t.type), ids, unsigned(rng() % 256), settings.fps);
                    break;
                }
            break;
        }
        case 22: ops::replaceMedia(tl, media, someClips(tl), mediaIds[rng() % mediaIds.size()], settings.fps); break;
        case 24: { // story chapters: a new one (on a file, or a placeholder), or one made from what is there
            const TimeUs narration = TimeUs(rng() % 8) * kUsPerSecond / 2, listening = TimeUs(rng() % 8) * kUsPerSecond / 2;
            if (rng() % 2)
                story::addChapter(tl, QStringLiteral("C%1").arg(i), {}, narration, listening,
                                  rng() % 2 ? media.at(mediaIds[rng() % mediaIds.size()]).get() : nullptr, std::max<TimeUs>(0, anyTime()),
                                  settings.fps);
            else {
                const TimeUs from = std::max<TimeUs>(0, anyTime());
                story::adoptChapter(tl, QStringLiteral("A%1").arg(i), {}, from, from + narration + listening + 1, narration);
            }
            break;
        }
        case 25:
        case 26: { // a chapter changed, swapped, assigned clips or deleted
            if (tl.chapters.isEmpty())
                break;
            const int id = tl.chapters[qsizetype(rng() % uint32_t(tl.chapters.size()))].toObject()[QStringLiteral("id")].toInt();
            switch (rng() % 4) {
            case 0: story::updateChapter(tl, id, QStringLiteral("U%1").arg(i), {}, TimeUs(rng() % 8) * kUsPerSecond / 2,
                                         TimeUs(rng() % 8) * kUsPerSecond / 2, settings.fps); break;
            case 1: story::moveChapter(tl, id, rng() % 2 ? 1 : -1); break;
            case 2: story::bindChapter(tl, id, someClips(tl)); break;
            default: story::removeChapter(tl, id); break;
            }
            break;
        }
        case 27:
        case 28: { // multicam: made from some picture events, then cut and switched
            if (multicam::trackIndex(tl) < 0) {
                std::vector<int> angles;
                for (const Track& t : tl.tracks)
                    if (isVisualTrack(t.type))
                        for (const Clip& c : t.clips)
                            if (rng() % 3 == 0 && angles.size() < 3)
                                angles.push_back(c.id);
                multicam::create(tl, media, angles, nullptr);
            } else if (rng() % 2)
                multicam::cut(tl, media, 1 + int(rng() % 3), std::max<TimeUs>(0, anyTime()), nullptr);
            else
                multicam::switchAngle(tl, media, 1 + int(rng() % 3), std::max<TimeUs>(0, anyTime()), nullptr);
            break;
        }
        case 29: { // Remove Silences / Edit by Transcript: stretches cut out of every track
            std::vector<cuts::Range> ranges;
            for (int k = 0; k < 1 + int(rng() % 3); ++k) {
                const TimeUs from = std::max<TimeUs>(0, anyTime());
                ranges.push_back({from, from + TimeUs(rng() % 4000000)});
            }
            cuts::removeRanges(tl, ranges, settings.fps);
            break;
        }
        case 30: cuts::insertTime(tl, std::max<TimeUs>(0, anyTime()), TimeUs(rng() % 5000000), settings.fps); break;
        case 31: story::compound(tl, someClips(tl), QStringLiteral("S%1").arg(i)); break;
        case 32: { // a compound unpacked, or its content changed (Edit Compound Sequence's Save)
            std::vector<const Clip*> sequences;
            for (const Track& t : tl.tracks)
                for (const Clip& c : t.clips)
                    if (c.isSequence())
                        sequences.push_back(&c);
            if (sequences.empty())
                break;
            const Clip* c = sequences[rng() % sequences.size()];
            if (rng() % 2) {
                story::unpack(tl, c->id);
            } else {
                Timeline inner = *c->sequence.value;
                if (!inner.tracks.empty() && !inner.tracks[0].clips.empty()) {
                    Clip& first = inner.tracks[0].clips[0];
                    first.length = std::max<TimeUs>(settings.fps.frameDuration(), first.length + (rng() % 2 ? 1 : -1) * TimeUs(rng() % 2000000));
                    ops::clampFades(first);
                    inner.sortClips();
                }
                story::replaceCompound(tl, c->id, inner, rng() % 2);
            }
            break;
        }
        case 33: story::directorsDice(tl, someClips(tl), unsigned(rng()), int(rng() % 3), (rng() % 101) / 100.0); break;
        case 34:
        case 35: { // the Live tab's pads, FX, layers, scenes, overlays, setlist and sequencer (saved with the project)
            auto chain = [&] {
                EffectChain fx;
                for (int k = 0; k < int(rng() % 3); ++k)
                    fx.push_back(makeEffect(vfx[rng() % vfx.size()], tl.allocId()));
                return fx;
            };
            auto share = [&] { return (rng() % 1001) / 1000.0; };
            auto overlay = [&] {
                LiveOverlay o;
                o.kind = LiveOverlay::Kind(rng() % 7);
                o.enabled = rng() % 2;
                o.text = QStringLiteral("text %1").arg(rng() % 100);
                o.anchor = int(rng() % 9);
                o.size = 1 + int(rng() % 40);
                o.width = 10 + int(rng() % 90);
                o.color = int(rng() & 0xffffff);
                o.opacity = share();
                o.pulse = rng() % 2;
                return o;
            };
            switch (rng() % 6) {
            case 0: {
                LivePad& pad = tl.livePads[rng() % kLivePads];
                pad.name = rng() % 3 ? QStringLiteral("Pad %1").arg(i) : QString();
                pad.effects = chain();
                pad.momentary = rng() % 2;
                pad.locked = rng() % 4 == 0;
                break;
            }
            case 1:
                if (rng() % 2)
                    tl.liveEffects = chain();
                else
                    tl.liveLayerEffects[rng() % kLiveLayers] = chain();
                break;
            case 2: { // a scene saved (its chains are copies of the Live ones: their ids repeat on purpose)
                LiveScene& sc = tl.liveScenes[rng() % kLiveScenes];
                sc.name = QStringLiteral("Scene %1").arg(i);
                sc.liveEffects = tl.liveEffects;
                sc.pad = int(rng() % (kLivePads + 1)) - 1;
                for (int l = 0; l < kLiveLayers; ++l) {
                    LiveLayer& layer = sc.layers[size_t(l)];
                    layer.source = rng() % 2 ? QStringLiteral("Camera %1").arg(l) : QString();
                    layer.settings.mode = LiveLayerSettings::Mode(rng() % 2);
                    layer.settings.corner = int(rng() % 6);
                    layer.settings.x = layer.settings.corner == 5 ? share() : 0.5; // (only a dragged layer keeps x, y)
                    layer.settings.y = layer.settings.corner == 5 ? share() : 0.5;
                    layer.settings.size = 0.05 + share() * 0.95;
                    layer.settings.opacity = share();
                    layer.settings.blend = int(rng() % 4);
                    layer.settings.noEffects = rng() % 2;
                    layer.effects = tl.liveLayerEffects[size_t(l)];
                }
                sc.overlays.clear();
                for (int k = 0; k < int(rng() % 3); ++k)
                    sc.overlays.push_back(overlay());
                sc.used = true;
                break;
            }
            case 3:
                if (tl.liveOverlays.size() < 8 && rng() % 3)
                    tl.liveOverlays.push_back(overlay());
                else if (!tl.liveOverlays.empty())
                    tl.liveOverlays.erase(tl.liveOverlays.begin() + std::ptrdiff_t(rng() % tl.liveOverlays.size()));
                break;
            case 4: {
                if (tl.liveSetlist.size() >= 20 || rng() % 4 == 0) {
                    if (!tl.liveSetlist.empty())
                        tl.liveSetlist.erase(tl.liveSetlist.begin() + std::ptrdiff_t(rng() % tl.liveSetlist.size()));
                    break;
                }
                LiveSong song;
                song.name = QStringLiteral("Song %1").arg(i);
                song.padSet = rng() % 2 ? QStringLiteral("Set %1").arg(rng() % 4) : QString();
                song.bpm = rng() % 3 ? 60 + int(rng() % 140) : 0;
                song.lockBpm = rng() % 2;
                song.scene = int(rng() % (kLiveScenes + 1)) - 1;
                song.notes = rng() % 2 ? QStringLiteral("notes %1").arg(i) : QString();
                tl.liveSetlist.push_back(song);
                break;
            }
            default: { // the sequencer: its loop, time signature and grid, and blocks
                LiveSequence& q = tl.liveSequence;
                q.on = rng() % 2;
                q.bars = 1 + int(rng() % 8);
                q.beatsPerBar = 1 + int(rng() % 16);
                q.beatUnit = std::array{2, 4, 8, 16}[rng() % 4];
                q.snap = std::array{0, 1, 2, 4, 8, 16, 32, -8, -16}[rng() % 9];
                if (rng() % 3 == 0)
                    q.blocks.clear();
                for (int k = 0; k < int(rng() % 4); ++k)
                    q.blocks.push_back({int(rng() % kLivePads), int(rng() % uint32_t(q.loopTicks())), 1 + int(rng() % 384)});
                break;
            }
            }
            break;
        }
        default: { // a marker or region
            Marker m;
            m.id = tl.allocId();
            m.time = std::max<TimeUs>(0, anyTime());
            m.length = rng() % 2 ? 0 : std::max<TimeUs>(0, anyTime());
            tl.markers.push_back(m);
            tl.sortMarkers();
            break;
        }
        }
        // Drop what an edit trimmed to nothing (the UI does the same).
        for (Track& t : tl.tracks)
            std::erase_if(t.clips, [](const Clip& c) { return c.length <= 0; });
        if (const QString p = timelineProblem(tl); !p.isEmpty()) {
            problem = QStringLiteral("step %1 (op %2): %3").arg(i).arg(op).arg(p);
            break;
        }
        // Story chapters: not before 0, no negative parts, laid one after another without overlapping, each with its
        // region where it is.
        {
            std::vector<std::pair<TimeUs, TimeUs>> spans;
            for (const QJsonValue v : tl.chapters) {
                const QJsonObject c = v.toObject();
                const auto get = [&](const char* key) { return c.value(QLatin1String(key)).toString().toLongLong(); };
                const TimeUs start = get("start"), length = get("narration") + get("listening");
                if (start < 0 || get("narration") < 0 || get("listening") < 0)
                    problem = QStringLiteral("chapter %1 at %2 (%3 + %4)").arg(c[QStringLiteral("id")].toInt()).arg(start).arg(get("narration")).arg(get("listening"));
                if (const Marker* m = tl.findMarker(c[QStringLiteral("id")].toInt());
                    m && (m->time != start || m->length != std::max<TimeUs>(1, length))) // (a region cut to nothing is 1 us)
                    problem = QStringLiteral("chapter %1 at %2 for %3, its region at %4 for %5")
                                  .arg(c[QStringLiteral("id")].toInt()).arg(start).arg(length).arg(m->time).arg(m->length);
                spans.emplace_back(start, start + length);
            }
            std::sort(spans.begin(), spans.end());
            for (size_t k = 1; k < spans.size() && problem.isEmpty(); ++k)
                if (spans[k].first < spans[k - 1].second && spans[k - 1].second > spans[k - 1].first && spans[k].second > spans[k].first)
                    problem = QStringLiteral("chapters overlap: %1-%2 and %3-%4")
                                  .arg(spans[k - 1].first).arg(spans[k - 1].second).arg(spans[k].first).arg(spans[k].second);
            if (!problem.isEmpty()) {
                problem = QStringLiteral("step %1 (op %2): %3").arg(i).arg(op).arg(problem);
                break;
            }
        }
        if (tl == project.timeline())
            continue;
        project.applyEdit(tl, QStringLiteral("step %1").arg(i));
        history.push_back(project.timeline());
        ++steps;
        if (i % 10 == 0 && !exercise(*project.snapshot(), comp, mixer))
            problem = QStringLiteral("step %1 (op %2): a picture came back empty or the sound had NaN").arg(i).arg(op);
        // (a project file holds times up to about 11.6 days, so audio sample counts can't overflow: an edit the fuzz has
        // slowed past that isn't one a file can hold)
        const bool fileSized = std::all_of(tl.tracks.begin(), tl.tracks.end(), [](const Track& t) {
            return std::all_of(t.clips.begin(), t.clips.end(),
                               [](const Clip& c) { return c.end() <= 1'000'000'000'000 && c.generatorSpan <= 1'000'000'000'000; });
        });
        if (i % 50 == 0 && problem.isEmpty() && fileSized) { // saved and opened again: the same edit
            const QString file = QDir::temp().filePath(QStringLiteral("mvp-edit-fuzz-%1.mvp").arg(QCoreApplication::applicationPid()));
            Project back;
            QString why;
            if (!project.save(file, &why) || !back.load(file, &why))
                problem = QStringLiteral("step %1 (op %2): saving and opening again failed: %3").arg(i).arg(op).arg(why);
            else if (Timeline kept = project.timeline(); [&] {
                         // (opening drops what refers to events no longer there: chapters' clips, a deleted angle)
                         for (int k = 0; k < kept.chapters.size(); ++k) {
                             QJsonObject c = kept.chapters[k].toObject();
                             for (const char* key : {"clips", "stretch"})
                                 if (c.contains(QLatin1String(key))) {
                                     QJsonArray left;
                                     std::set<int> seen;
                                     for (const QJsonValue v : c[QLatin1String(key)].toArray())
                                         if (kept.findClip(v.toInt()) && seen.insert(v.toInt()).second)
                                             left.append(v);
                                     c[QLatin1String(key)] = left;
                                 }
                             kept.chapters[k] = c;
                         }
                         if (!kept.multicam.isEmpty()) {
                             QJsonArray angles;
                             for (const QJsonValue v : kept.multicam[QStringLiteral("angles")].toArray())
                                 angles.append(kept.findClip(v.toInt()) ? v.toInt() : 0);
                             kept.multicam[QStringLiteral("angles")] = angles;
                             if (kept.trackIndex(kept.multicam[QStringLiteral("track")].toInt()) < 0)
                                 kept.multicam[QStringLiteral("track")] = 0;
                         }
                         return !(back.timeline() == kept);
                     }()) {
                const QJsonObject a = nestedTimelineToJson(kept), b = nestedTimelineToJson(back.timeline());
                QStringList differ;
                for (auto it = a.begin(); it != a.end(); ++it)
                    if (b.value(it.key()) != it.value())
                        differ << it.key();
                const Timeline& was = kept;
                const Timeline& now = back.timeline();
                if (!(was.livePads == now.livePads)) differ << QStringLiteral("livePads");
                if (!(was.liveEffects == now.liveEffects)) differ << QStringLiteral("liveEffects");
                if (!(was.liveLayerEffects == now.liveLayerEffects)) differ << QStringLiteral("liveLayerEffects");
                if (!(was.liveOverlays == now.liveOverlays)) differ << QStringLiteral("liveOverlays");
                if (!(was.liveScenes == now.liveScenes)) differ << QStringLiteral("liveScenes");
                if (!(was.liveSetlist == now.liveSetlist)) differ << QStringLiteral("liveSetlist");
                if (!(was.liveSequence == now.liveSequence)) differ << QStringLiteral("liveSequence");
                problem = QStringLiteral("step %1 (op %2): opened again, the edit differs (%3)").arg(i).arg(op).arg(differ.join(QStringLiteral(", ")));
                if (qEnvironmentVariableIsSet("MVP_EDIT_DIFF"))
                    for (const QString& key : differ)
                        print(key + QStringLiteral("\n  was: ") + QString::fromUtf8(QJsonDocument(QJsonObject{{key, a.value(key)}}).toJson(QJsonDocument::Compact))
                              + QStringLiteral("\n  now: ") + QString::fromUtf8(QJsonDocument(QJsonObject{{key, b.value(key)}}).toJson(QJsonDocument::Compact)));
            }
            QFile::remove(file);
        }
    }
    check(problem.isEmpty(),
          QStringLiteral("random editing: %1 edits (insert, split, delete, ripple, group, tracks, titles, speed, trims, ramps, reverse, "
                         "fades, FX, loops, overwrite, transitions, markers, story chapters, multicam, Remove Silences, Insert Time, compounds, the Live tab) keep the "
                         "timeline sound%2")
              .arg(steps)
              .arg(problem.isEmpty() ? QString() : QStringLiteral(": ") + problem));
    // Undo to the start, checking each step, and redo to the end.
    stage(QStringLiteral("undo and redo of the random edits"), 300);
    bool undoOk = true, redoOk = true;
    QUndoStack* undo = project.undoStack();
    for (int k = int(history.size()) - 2; k >= 0 && undo->canUndo(); --k) {
        undo->undo();
        undoOk = undoOk && project.timeline() == history[size_t(k)];
    }
    for (size_t k = 1; k < history.size() && undo->canRedo(); ++k) {
        undo->redo();
        redoOk = redoOk && project.timeline() == history[k];
    }
    check(undoOk && redoOk && project.timeline() == history.back(),
          QStringLiteral("all %1 edits undo step by step back to the empty timeline and redo to the end").arg(history.size() - 1));
}

// ---------------------------------------------------------------------------
// 5. Export edge cases.
// ---------------------------------------------------------------------------
void exportEdges(const QString& mediaDir, const QString& outDir)
{
    const QString dir = QDir(outDir).filePath(QStringLiteral("export"));
    QDir().mkpath(dir);
    const ProjectSnapshot base = snapshotOf({QDir(mediaDir).filePath(QStringLiteral("h264_aac.mp4"))});
    auto settings = [&](const QString& name) {
        ExportSettings s;
        s.path = QDir(dir).filePath(name);
        s.videoEncoder = QStringLiteral("mpeg4");
        s.audioEncoder = QStringLiteral("aac");
        s.width = 320;
        s.height = 180;
        s.rangeStart = 0;
        s.rangeEnd = kUsPerSecond;
        return s;
    };
    struct Result {
        bool ok;
        QString error;
    };
    auto run = [&](const ProjectSnapshot& snap, const ExportSettings& s, const Exporter::Progress& progress = {}) {
        stage(QStringLiteral("export %1").arg(QFileInfo(s.path).fileName()), 120);
        Exporter e;
        const bool ok = e.run(snap, s, progress);
        return Result{ok, e.error()};
    };
    auto fileOk = [](const QString& path) {
        const MediaInfo i = probeMedia(path);
        return i.ok() && i.hasVideo();
    };

    {
        ProjectSnapshot empty = base;
        empty.timeline = defaultTimeline();
        ExportSettings s = settings(QStringLiteral("empty.mp4"));
        s.rangeEnd = 0;
        const Result r = run(empty, s);
        check(r.ok ? fileOk(s.path) : !r.error.isEmpty(),
              QStringLiteral("an empty timeline: %1")
                  .arg(r.ok ? QStringLiteral("exports a playable file") : QStringLiteral("refused: \"%1\"").arg(r.error)));
    }
    {
        ExportSettings s = settings(QStringLiteral("backwards.mp4"));
        s.rangeStart = 2 * kUsPerSecond;
        s.rangeEnd = kUsPerSecond;
        const Result r = run(base, s);
        check(r.ok ? fileOk(s.path) : !r.error.isEmpty(),
              QStringLiteral("a range that ends before it starts: %1")
                  .arg(r.ok ? QStringLiteral("exports") : QStringLiteral("refused: \"%1\"").arg(r.error)));
    }
    {
        ExportSettings s = settings(QStringLiteral("past_end.mp4"));
        s.rangeStart = base.timeline.duration() + 10 * kUsPerSecond;
        s.rangeEnd = s.rangeStart + kUsPerSecond;
        const Result r = run(base, s);
        check(r.ok ? fileOk(s.path) : !r.error.isEmpty(),
              QStringLiteral("a range after the end: %1")
                  .arg(r.ok ? QStringLiteral("exports black") : QStringLiteral("refused: \"%1\"").arg(r.error)));
    }
    for (QSize size : {QSize(2, 2), QSize(321, 181), QSize(1, 1), QSize(0, 0), QSize(-5, 100), QSize(9000, 9000)}) {
        ExportSettings s = settings(QStringLiteral("size_%1x%2.mp4").arg(size.width()).arg(size.height()));
        s.width = size.width();
        s.height = size.height();
        s.rangeEnd = kUsPerSecond / 5;
        const Result r = run(base, s);
        check(r.ok ? fileOk(s.path) : !r.error.isEmpty(),
              QStringLiteral("output size %1x%2: %3")
                  .arg(size.width())
                  .arg(size.height())
                  .arg(r.ok ? QStringLiteral("exports a playable file") : QStringLiteral("refused: \"%1\"").arg(r.error)));
    }
    for (FrameRate fps :
         {FrameRate{0, 1}, FrameRate{30, 0}, FrameRate{-30, 1}, FrameRate{24000, 1001}, FrameRate{1, 1}, FrameRate{240, 1}}) {
        ExportSettings s = settings(QStringLiteral("fps_%1_%2.mp4").arg(fps.num).arg(fps.den));
        s.fps = fps;
        const Result r = run(base, s);
        check(r.ok ? fileOk(s.path) : !r.error.isEmpty(),
              QStringLiteral("frame rate %1/%2: %3")
                  .arg(fps.num)
                  .arg(fps.den)
                  .arg(r.ok ? QStringLiteral("exports") : QStringLiteral("refused: \"%1\"").arg(r.error)));
    }
    {
        // A folder that isn't there yet is made; one that can't be (a file stands in the way) is an error.
        ExportSettings s = settings(QStringLiteral("no/such/folder/out.mp4"));
        QDir(QFileInfo(s.path).absolutePath()).removeRecursively();
        const Result r = run(base, s);
        check(r.ok && fileOk(s.path), QStringLiteral("into a folder that doesn't exist yet: %1")
                                          .arg(r.ok ? QStringLiteral("made it") : QStringLiteral("\"%1\"").arg(r.error)));
        ExportSettings blocked = settings(QStringLiteral("blocker/out.mp4"));
        const QString blocker = QFileInfo(QFileInfo(blocked.path).absolutePath()).absoluteFilePath();
        QDir(blocker).removeRecursively();
        QFile(blocker).open(QIODevice::WriteOnly);
        const Result rb = run(base, blocked);
        check(!rb.ok && !rb.error.isEmpty(),
              QStringLiteral("into a \"folder\" that is a file: %1")
                  .arg(rb.ok ? QStringLiteral("said it worked") : QStringLiteral("\"%1\"").arg(rb.error)));
    }
    {
        ExportSettings s = settings(QString());
        s.path = dir; // a folder, not a file
        const Result r = run(base, s);
        check(!r.ok && !r.error.isEmpty() && QFileInfo(dir).isDir(),
              QStringLiteral("onto a folder's name: %1")
                  .arg(r.ok ? QStringLiteral("said it worked") : QStringLiteral("\"%1\"").arg(r.error)));
    }
    {
        ExportSettings s = settings(QStringLiteral("nothing.mp4"));
        s.videoEncoder.clear();
        s.audioEncoder.clear();
        const Result r = run(base, s);
        check(!r.ok && !r.error.isEmpty(),
              QStringLiteral("with no video and no sound: %1")
                  .arg(r.ok ? QStringLiteral("said it worked") : QStringLiteral("\"%1\"").arg(r.error)));
    }
    {
        ExportSettings s = settings(QStringLiteral("bad_encoder.mp4"));
        s.videoEncoder = QStringLiteral("no_such_encoder");
        const Result r = run(base, s);
        check(!r.ok && !r.error.isEmpty(), QStringLiteral("an encoder that doesn't exist: \"%1\"").arg(r.error));
    }
    {
        ExportSettings s = settings(QStringLiteral("bad_options.mp4"));
        s.videoOptions = QStringLiteral("=:::=crf=abc:nonsense");
        s.muxerOptions = QStringLiteral("::==");
        const Result r = run(base, s);
        check(r.ok ? fileOk(s.path) : !r.error.isEmpty(),
              QStringLiteral("nonsense encoder options: %1")
                  .arg(r.ok ? QStringLiteral("ignored, exports") : QStringLiteral("refused: \"%1\"").arg(r.error)));
    }
    {
        ExportSettings s = settings(QStringLiteral("cancelled.mp4"));
        s.rangeEnd = 5 * kUsPerSecond;
        const Result r = run(base, s, [](double p) { return p < 0.3; });
        check(!r.ok && !QFileInfo::exists(s.path),
              QStringLiteral("cancelled at 30%: stops, and no half-written file is left (%1)").arg(r.error));
    }
    {
        // Over the source it reads from: must not destroy the source.
        const QString src = QDir(dir).filePath(QStringLiteral("self.mp4"));
        QFile::remove(src);
        QFile::copy(QDir(mediaDir).filePath(QStringLiteral("h264_aac.mp4")), src);
        const ProjectSnapshot snap = snapshotOf({src});
        ExportSettings s = settings(QString());
        s.path = src;
        const Result r = run(snap, s);
        const MediaInfo after = probeMedia(src);
        const TimeUs before = snap.media.begin()->second->info.duration;
        check(!r.ok && after.ok() && after.duration == before,
              QStringLiteral("over the file it reads from: %1; the file is untouched (%2 s, was %3 s)")
                  .arg(r.ok ? QStringLiteral("exported over it") : QStringLiteral("refused: \"%1\"").arg(r.error))
                  .arg(after.duration / 1e6, 0, 'f', 2)
                  .arg(before / 1e6, 0, 'f', 2));
    }
    {
        // Read-only target file.
        const QString ro = QDir(dir).filePath(QStringLiteral("readonly.mp4"));
        QFile::remove(ro);
        QFile(ro).open(QIODevice::WriteOnly);
        QFile(ro).setPermissions(QFile::ReadOwner);
        ExportSettings s = settings(QString());
        s.path = ro;
        const Result r = run(base, s);
        QFile(ro).setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        check(!r.ok && !r.error.isEmpty(),
              QStringLiteral("over a read-only file: %1")
                  .arg(r.ok ? QStringLiteral("said it worked") : QStringLiteral("\"%1\"").arg(r.error)));
    }
}

// ---------------------------------------------------------------------------
// 6. Several renderers at once (export, preview and thumbnails all render
//    while the user edits).
// ---------------------------------------------------------------------------
void parallelRendering(const QString& mediaDir)
{
    const ProjectSnapshot snap =
        snapshotOf({QDir(mediaDir).filePath(QStringLiteral("h264_aac.mp4")), QDir(mediaDir).filePath(QStringLiteral("vp9_vorbis.webm"))});
    stage(QStringLiteral("rendering on 6 threads at once"), 300);
    std::atomic<int> bad{0};
    std::vector<QImage> first(6);
    std::vector<std::thread> threads;
    for (int k = 0; k < 6; ++k)
        threads.emplace_back([&, k] {
            Compositor comp;
            AudioMixer mixer(48000);
            std::vector<float> buf(2 * 1024);
            std::mt19937 rng{uint32_t(k)};
            const TimeUs d = snap.timeline.duration();
            for (int i = 0; i < 40; ++i) {
                const TimeUs t = i == 0 ? d / 2 : TimeUs(rng() % uint64_t(std::max<TimeUs>(1, d)));
                const QImage img = comp.render(snap, t, QSize(320, 180));
                if (img.isNull())
                    ++bad;
                if (i == 0)
                    first[size_t(k)] = img;
                mixer.mix(snap, usToSamples(t, 48000), 1024, buf.data());
            }
        });
    for (auto& t : threads)
        t.join();
    bool same = true;
    for (const QImage& img : first)
        same = same && img == first[0];
    check(bad == 0 && same,
          QStringLiteral("6 threads rendering and mixing at random times at once: no crash, no empty picture, and the same frame gives the "
                         "same picture on each"));
}

// ---------------------------------------------------------------------------
// 7. Memory over time: playing on for a long while must not keep growing.
// ---------------------------------------------------------------------------
void memoryOverTime(const QString& mediaDir)
{
    ProjectSnapshot snap =
        snapshotOf({QDir(mediaDir).filePath(QStringLiteral("h264_aac.mp4")), QDir(mediaDir).filePath(QStringLiteral("audio.wav")),
                    QDir(mediaDir).filePath(QStringLiteral("still.png"))});
    // Some FX so their state is in play too.
    for (Track& t : snap.timeline.tracks)
        for (Clip& c : t.clips)
            if (t.type == TrackType::Video)
                c.effects.push_back(makeEffectOfType(QStringLiteral("blur"), snap.timeline.allocId()));
    stage(QStringLiteral("memory over 3000 frames"), 600);
    Compositor comp;
    AudioMixer mixer(48000);
    std::vector<float> buf(2 * 1600);
    const TimeUs d = std::max<TimeUs>(1, snap.timeline.duration());
    qint64 warm = 0;
    for (int i = 0; i < 3000; ++i) {
        const TimeUs t = TimeUs(i) * kUsPerSecond / 30 % d; // play on, looping
        comp.render(snap, t, QSize(640, 360));
        mixer.mix(snap, usToSamples(t, 48000), 1600, buf.data());
        if (i == 300)
            warm = memoryBytes();
    }
    const qint64 end = memoryBytes();
    const double growMB = (end - warm) / 1048576.0;
    check(growMB < 40,
          QStringLiteral("memory: 2700 more frames played (looping, with FX) grew it by %1 MB (%2 MB in all)")
              .arg(growMB, 0, 'f', 1)
              .arg(end / 1048576.0, 0, 'f', 0));
}

// ---------------------------------------------------------------------------
// 8. A big project: thousands of events with effects. Edits, undo memory,
//    saving and loading must stay quick and bounded.
// ---------------------------------------------------------------------------
void bigProject(const QString& mediaDir, const QString& outDir)
{
    stage(QStringLiteral("building a project of 3000 events"), 300);
    Project project;
    const int video = project.addMedia(probeMedia(QDir(mediaDir).filePath(QStringLiteral("h264_aac.mp4"))));
    Timeline tl = project.timeline();
    while (tl.tracks.size() < 6)
        ops::addTrack(tl, tl.tracks.size() % 2 ? TrackType::Audio : TrackType::Video);
    const auto media = project.media();
    for (int i = 0; i < 500; ++i)
        ops::insertMedia(tl, *media.at(video), TimeUs(i) * 2 * kUsPerSecond, int(i % 3) * 2, project.settings().fps);
    int events = 0;
    for (Track& t : tl.tracks)
        for (Clip& c : t.clips) {
            c.length = std::min<TimeUs>(c.length, 2 * kUsPerSecond);
            if (t.type == TrackType::Video) {
                c.effects.push_back(makeEffectOfType(QStringLiteral("blur"), tl.allocId()));
                c.effects.push_back(makeEffectOfType(QStringLiteral("brightness_contrast"), tl.allocId()));
            }
        }
    ops::splitClips(tl, {}, 7 * kUsPerSecond); // and some more pieces
    for (TimeUs t = kUsPerSecond / 2; t < 1000 * kUsPerSecond; t += 3 * kUsPerSecond)
        ops::splitClips(tl, {}, t);
    for (const Track& t : tl.tracks)
        events += int(t.clips.size());
    project.applyEdit(tl, QStringLiteral("big"));
    const qint64 before = memoryBytes();
    QElapsedTimer clock;
    clock.start();
    stage(QStringLiteral("200 edits on a project of %1 events").arg(events), 600);
    for (int i = 0; i < 200; ++i) {
        Timeline e = project.timeline();
        ops::splitClips(e, {}, TimeUs(i) * 4 * kUsPerSecond + 333333);
        project.applyEdit(e, QStringLiteral("split"));
    }
    const double perEdit = clock.elapsed() / 200.0;
    const double perStepMB = (memoryBytes() - before) / 200.0 / 1048576.0;
    // (the checked Debug build is several times slower; it is for index checks, not speed)
#ifdef NDEBUG
    check(perEdit < 100,
#else
    check(perEdit < 800,
#endif
          QStringLiteral("big project (%1 events, 2 effects on each video one): an edit takes %2 ms").arg(events).arg(perEdit, 0, 'f', 1));
    check(perStepMB < 2,
          QStringLiteral("big project: each undo step costs %1 MB of memory (500 edits: %2 MB)")
              .arg(perStepMB, 0, 'f', 2)
              .arg(perStepMB * 500, 0, 'f', 0));
    stage(QStringLiteral("saving and loading the big project"), 300);
    const QString path = QDir(outDir).filePath(QStringLiteral("big.mvp"));
    QString error;
    clock.restart();
    const bool saved = project.save(path, &error);
    const qint64 saveMs = clock.restart();
    Project back;
    const bool loaded = back.load(path, &error);
    const qint64 loadMs = clock.restart();
    // (the checked Debug build, run beside the other tests, took 4.9 s and 5.5 s: 0.6 s each on its own)
#ifdef NDEBUG
    constexpr qint64 kSlow = 1;
#else
    constexpr qint64 kSlow = 4;
#endif
    check(saved && loaded && back.timeline() == project.timeline() && saveMs < 3000 * kSlow && loadMs < 5000 * kSlow,
          QStringLiteral("big project: saved in %1 ms (%2 MB), loaded in %3 ms, the same")
              .arg(saveMs)
              .arg(QFileInfo(path).size() / 1048576.0, 0, 'f', 1)
              .arg(loadMs));
    stage(QStringLiteral("rendering the big project"), 300);
    Compositor comp;
    clock.restart();
    const auto snap = project.snapshot();
    for (int i = 0; i < 30; ++i)
        comp.render(*snap, TimeUs(i) * 7 * kUsPerSecond, QSize(640, 360));
    check(clock.elapsed() / 30.0 < 200,
          QStringLiteral("big project: a frame (seeking around) renders in %1 ms").arg(clock.elapsed() / 30.0, 0, 'f', 1));
}

// ---------------------------------------------------------------------------
// 9. Other files people open: LUTs and subtitles, mangled.
// ---------------------------------------------------------------------------
QByteArray mangleText(QByteArray text, std::mt19937& rng)
{
    static const char* tokens[] = {"nan",
                                   "inf",
                                   "-inf",
                                   "1e40",
                                   "-1e40",
                                   "-",
                                   "",
                                   "99999999999",
                                   "-5",
                                   "0",
                                   "2147483648",
                                   "\xff\xfe",
                                   "99:99:99,999",
                                   "-00:00:01,000",
                                   "999999999:00:00.000",
                                   "-->",
                                   "00:00:01,000 --> 00:00:00,500"};
    QList<QByteArray> words = text.split(' ');
    for (int k = 0, n = 1 + int(rng() % 3); k < n && !words.isEmpty(); ++k)
        words[qsizetype(rng() % uint32_t(words.size()))] = tokens[rng() % (sizeof tokens / sizeof *tokens)];
    text = words.join(' ');
    switch (rng() % 6) {
    case 0: text.truncate(qsizetype(rng() % uint32_t(text.size() + 1))); break;
    case 1:
        for (int k = 0; k < 6 && !text.isEmpty(); ++k)
            text[qsizetype(rng() % uint32_t(text.size()))] = char(rng());
        break;
    default: break;
    }
    return text;
}

void parserFuzz(const QString& examplesDir, int iterations)
{
    stage(QStringLiteral("mangled LUTs"), 300);
    QByteArray cube = "TITLE \"test\"\nLUT_3D_SIZE 4\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n";
    for (int b = 0; b < 4; ++b)
        for (int g = 0; g < 4; ++g)
            for (int r = 0; r < 4; ++r)
                cube += QByteArray::number(r / 3.0) + ' ' + QByteArray::number(g / 3.0) + ' ' + QByteArray::number(b / 3.0) + '\n';
    const QByteArray cube1d = "LUT_1D_SIZE 3\n0 0 0\n0.5 0.5 0.5\n1 1 1\n";
    std::mt19937 rng(77);
    int luts = 0;
    QString lutProblem;
    for (int i = 0; i < iterations && lutProblem.isEmpty(); ++i) {
        const QByteArray text = mangleText(i % 3 ? cube : cube1d, rng);
        QString error;
        const auto lut = parseCubeLut(text, &error);
        if (!lut) {
            if (error.isEmpty())
                lutProblem = QStringLiteral("refused without saying why: %1").arg(QString::fromLatin1(text.left(80)));
            continue;
        }
        ++luts;
        QImage img(32, 32, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                img.setPixel(x, y, qRgb(x * 8, y * 8, (x + y) * 4));
        applyCubeLut(img, *lut, 1.0f);
    }
    check(lutProblem.isEmpty(),
          QStringLiteral("LUT files: %1 mangled .cube files (nan, inf, sizes that don't fit, cut short, scribbled over): %2 load and "
                         "apply, the rest are refused with a reason%3")
              .arg(iterations)
              .arg(luts)
              .arg(lutProblem.isEmpty() ? QString() : QStringLiteral(": ") + lutProblem));

    stage(QStringLiteral("mangled subtitles"), 300);
    const QByteArray srt = "1\n00:00:01,000 --> 00:00:02,500\nHello <i>there</i>\n\n2\n00:00:03,000 --> 00:00:04,000\nSecond\nline\n\n";
    const QByteArray vtt =
        "WEBVTT\n\n00:01.000 --> 00:02.500 align:start\nHello\n\nNOTE a note\n\n00:00:03.000 --> 00:00:04.000\n- Second\n";
    int captions = 0;
    QString subProblem;
    for (int i = 0; i < iterations && subProblem.isEmpty(); ++i) {
        const QByteArray text = mangleText(i % 2 ? srt : vtt, rng);
        for (const Caption& c : parseSubtitles(QString::fromUtf8(text))) {
            ++captions;
            if (c.start < 0 || c.end < c.start || c.end > TimeUs(1000) * 3600 * kUsPerSecond)
                subProblem =
                    QStringLiteral("a caption from %1 to %2 us from: %3").arg(c.start).arg(c.end).arg(QString::fromUtf8(text.left(120)));
        }
    }
    check(subProblem.isEmpty(),
          QStringLiteral("subtitle files: %1 mangled SRT and WebVTT files give %2 captions, none starting before 0, ending before it "
                         "starts or absurdly late%3")
              .arg(iterations)
              .arg(captions)
              .arg(subProblem.isEmpty() ? QString() : QStringLiteral(": ") + subProblem));

    // Graphics scenes (from project files and the AI): arrays cut short, wrong types, keys gone.
    // Drawn whether or not they pass validation (a project file's scene isn't checked again).
    const std::function<QJsonValue(const QJsonValue&)> mangle = [&](const QJsonValue& v) -> QJsonValue {
        const uint32_t r = rng() % 100;
        if (v.isArray()) {
            const QJsonArray a = v.toArray();
            if (r < 4)
                return QJsonArray{};
            if (r < 8 && !a.isEmpty())
                return QJsonArray{mangle(a.at(0))};
            QJsonArray out;
            for (const QJsonValue& e : a)
                out.append(mangle(e));
            return out;
        }
        if (v.isObject()) {
            const QJsonObject o = v.toObject();
            QJsonObject out;
            for (auto it = o.begin(); it != o.end(); ++it)
                if (rng() % 100 >= 3)
                    out.insert(it.key(), mangle(it.value()));
            return out;
        }
        switch (r) {
        case 0: return QStringLiteral("x");
        case 1: return QJsonValue();
        case 2: return QJsonObject{};
        case 3: return QJsonArray{};
        case 4: return v.isDouble() ? QJsonValue(1e300) : v;
        case 5: return v.isDouble() ? QJsonValue(-1e300) : v;
        case 6: return v.isDouble() ? QJsonValue(int(rng() % 8)) : v; // (node links rewired, loops among them)
        default: return v;
        }
    };
    QStringList scenePresets = graphics::presets();
    scenePresets += motion::presets();
    int valid = 0;
    for (int i = 0; i < iterations; ++i) {
        const QString name = scenePresets[qsizetype(i) % scenePresets.size()];
        const QJsonObject scene = mangle(graphics::scene(name)).toObject();
        stage(QStringLiteral("mangled graphics scene %1 (%2)").arg(i).arg(name), 60);
        valid += graphics::validateScene(scene).isEmpty();
        QImage img(96, 54, QImage::Format_ARGB32_Premultiplied);
        for (double t : {0.0, 1.3, 1e9})
            graphics::drawScene(img, scene, t);
    }
    check(true, QStringLiteral("graphics scenes: %1 mangled scenes (%2 still valid) draw without crashing or hanging").arg(iterations).arg(valid));

    // Node effects (.mvpfx files people share): mangled and rewired, run for a few frames.
    std::vector<QJsonObject> graphs;
    QDirIterator files(QDir(examplesDir).filePath(QStringLiteral("../effects")), {QStringLiteral("*.mvpfx")}, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile f(files.next());
        if (f.open(QIODevice::ReadOnly))
            graphs.push_back(QJsonDocument::fromJson(f.readAll()).object());
    }
    int ran = 0;
    for (int i = 0; i < iterations && !graphs.empty(); ++i) {
        const nodes::Graph g = nodes::fromJson(mangle(graphs[size_t(i) % graphs.size()]).toObject());
        stage(QStringLiteral("mangled node effect %1").arg(i), 60);
        ran += nodes::problem(g).isEmpty();
        QImage source(64, 36, QImage::Format_ARGB32_Premultiplied);
        source.fill(qRgb(90, 140, 200));
        nodes::Evaluator eval;
        RenderContext ctx;
        ctx.fps = 30;
        for (int f = 0; f < 3; ++f) {
            ctx.time = ctx.timelineTime = TimeUs(f) * kUsPerSecond / 30;
            eval.run(g, source, ctx, f == 0);
        }
    }
    check(!graphs.empty(), QStringLiteral("node effects: %1 mangled and rewired graphs from %2 files (%3 still runnable) run without crashing or hanging")
                               .arg(iterations).arg(graphs.size()).arg(ran));
}

} // namespace

int cmdHarden(const QString& mediaDir, const QString& examplesDir, const QString& outDir, int iterations)
{
    QDir().mkpath(outDir);
    startWatchdog();
    QElapsedTimer clock;
    clock.start();
    const QString only = qEnvironmentVariable("MVP_HARDEN_ONLY"); // one part, e.g. "big"
    auto part = [&](const char* name) { return only.isEmpty() || only == QLatin1String(name); };
    if (part("damaged"))
        damagedMedia(mediaDir, outDir);
    if (part("odd"))
        oddMedia(outDir);
    if (part("projects"))
        projectFuzz(examplesDir, outDir, iterations);
    if (part("editing"))
        editFuzz(mediaDir, iterations);
    if (part("export"))
        exportEdges(mediaDir, outDir);
    if (part("parallel"))
        parallelRendering(mediaDir);
    if (part("memory"))
        memoryOverTime(mediaDir);
    if (part("big"))
        bigProject(mediaDir, outDir);
    if (part("parsers"))
        parserFuzz(examplesDir, iterations);
    g_deadline = 0;
    print(QStringLiteral("%1  harden: %2 failure(s) in %3 s")
              .arg(g_failures ? QStringLiteral("FAIL") : QStringLiteral("PASS"))
              .arg(g_failures)
              .arg(clock.elapsed() / 1000));
    return g_failures ? 1 : 0;
}
