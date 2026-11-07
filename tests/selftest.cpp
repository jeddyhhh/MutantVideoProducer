// Headless checks for the media layer.
//
//   mvp_selftest probe <file>...            probe + decode test for each file
//   mvp_selftest export <media> <outdir>    render media through every export
//                                               preset, then re-import the result
//   mvp_selftest effects <media> <outdir>   render every effect and transition,
//                                               check keyframes, save/load, export
//   mvp_selftest generators <outdir>        render every text/background preset
//   mvp_selftest speed <media> <outdir>     playback speed, reverse, frame blending,
//                                               pan/crop and audio time-stretch checks
//   mvp_selftest gpu <testmedia-dir> <outdir>  GPU renderer vs CPU renderer on many
//                                               scenarios, hardware decoding, benchmarks
//   mvp_selftest audiofx <testmedia-dir> <outdir>  every audio effect, measured filter/
//                                               dynamics behaviour, tails, master FX, export
//   mvp_selftest envelopes <testmedia-dir> <outdir>  volume/pan/composite envelopes
//   mvp_selftest ramps <testmedia-dir> <outdir>  speed ramps (velocity envelopes)
//   mvp_selftest racks <testmedia-dir> <outdir>  effect racks (parallel chains), chain editing
//   mvp_selftest beats                      beat tracker on synthetic drum loops
//   mvp_selftest editing <testmedia-dir> <outdir>  ripple ops, markers, track blend modes,
//                                               autosave copies / recovery, relinking
//   mvp_selftest presets <testmedia-dir> <outdir>  factory/user effect presets
//   mvp_selftest formats                    list counts of supported formats
//
// Exit code is the number of failures.

#include "core/AudioDecoder.h"
#include "core/AudioEffects.h"
#include "core/AudioMixer.h"
#include "core/Captions.h"
#include "core/Segmentation.h"
#include "core/Vst3Host.h"
#include "core/YouTube.h"
#include "core/EffectMask.h"
#include "core/Multicam.h"
#include "core/PanCrop.h"
#include "core/Compositor.h"
#include "core/NodeGraph.h"
#include "core/ColorMatch.h"
#include "core/PreviewCache.h"
#include "core/Spectrogram.h"
#include <QDataStream>
#include "core/Dither.h"
#include "core/ConsoleEngine.h"
#include "core/BeatTracker.h"
#include "core/RandomChain.h"
#include "core/CubeLut.h"
#include "core/LiveAudio.h"
#include "core/LiveCapture.h"
#include "core/LiveModulation.h"
#include "core/MidiInput.h"
#include "core/BeatMarkers.h"
#include "core/MotionTracker.h"
#include "core/Stabilizer.h"
#include "core/Proxy.h"
#include "core/Scopes.h"
#include "core/VideoShare.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef near
#undef far
#endif
#include <atomic>
#include <thread>
#include "core/LiveRecorder.h"
#include "core/Effects.h"
#include "core/Exporter.h"
#include "core/FFmpeg.h"
#include "core/FormatRegistry.h"
#include "core/FrameConverter.h"
#include "core/Generators.h"
#include "core/GpuCompositor.h"
#include "core/Hardware.h"
#include "core/ImageOps.h"
#include "core/MediaInfo.h"
#include "core/Presets.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "core/SceneDetect.h"
#include "core/Cuts.h"
#include "core/Reframe.h"
#include "core/VideoDecoder.h"

#include <QDir>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include "core/MotionGraphics.h"
#include <QJsonObject>
#include <QOffscreenSurface>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QThread>
#include <QRegularExpression>
#include <QProcess>
#include <QCoreApplication>
#include <cmath>
#include <numbers>
#include <map>
#include <functional>
#include <cstdio>
#include <mutex>
#include <set>
#include <tuple>

namespace {

void print(const QString& s)
{
    std::fputs(s.toUtf8().constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Returns an error description, or empty on success.
QString checkMedia(const QString& path, bool verbose)
{
    const MediaInfo info = probeMedia(path);
    if (verbose)
        print(info.summary());
    if (!info.ok())
        return info.error.isEmpty() ? QStringLiteral("no usable streams") : info.error;

    if (info.hasVideo()) {
        const VideoStreamInfo& vs = info.video.front();
        VideoDecoder dec;
        QString err;
        if (!dec.open(path, vs.index, &err))
            return QStringLiteral("video open: %1").arg(err);
        FrameConverter conv;
        const TimeUs d = info.duration;
        // Forward, far jump, backwards: exercises sequential decode and seeking.
        const TimeUs times[] = {0, d / 2, d * 9 / 10, d / 10, d / 10 + d / 50, 0};
        for (TimeUs t : times) {
            const AVFrame* f = dec.frameAt(t);
            if (!f)
                return QStringLiteral("no video frame at %1").arg(formatDuration(t));
            const QImage img = conv.convert(f, QSize(160, 90), false);
            if (img.isNull())
                return QStringLiteral("frame conversion failed (pix fmt %1)")
                    .arg(QString::fromUtf8(av_get_pix_fmt_name(AVPixelFormat(f->format))));
            if (!info.isStill && std::llabs(dec.currentPts() - t) > 2 * kUsPerSecond && t < d - kUsPerSecond)
                return QStringLiteral("seek landed at %1 for target %2")
                    .arg(formatDuration(dec.currentPts()), formatDuration(t));
        }
        // Frame-stepping must advance one frame at a time.
        if (!info.isStill && d > kUsPerSecond) {
            const FrameRate fr = vs.frameRate;
            const int64_t f0 = fr.frameAt(d / 3);
            TimeUs last = -1;
            int distinct = 0;
            for (int i = 0; i < 10; ++i) {
                dec.frameAt(fr.timeOf(f0 + i));
                if (dec.currentPts() != last)
                    ++distinct;
                last = dec.currentPts();
            }
            if (distinct < 8)
                return QStringLiteral("frame stepping returned only %1 distinct frames of 10").arg(distinct);
        }
    }

    if (info.hasAudio()) {
        AudioDecoder dec;
        QString err;
        if (!dec.open(path, info.audio.front().index, 48000, &err))
            return QStringLiteral("audio open: %1").arg(err);
        std::vector<float> buf(48000 * 2);
        double energy = 0;
        const int64_t starts[] = {0, usToSamples(info.duration / 2, 48000)};
        for (int64_t s : starts) {
            const int n = int(std::min<int64_t>(48000, std::max<int64_t>(1, usToSamples(info.duration, 48000) - s)));
            dec.read(s, n, buf.data());
            for (int i = 0; i < n * 2; ++i)
                energy += double(buf[i]) * buf[i];
        }
        if (verbose)
            print(QStringLiteral("  audio energy: %1").arg(energy, 0, 'g', 4));
        if (!(energy > 0))
            return QStringLiteral("audio decoded as silence");
    }
    return {};
}

int cmdProbe(const QStringList& files)
{
    int failures = 0;
    for (const QString& f : files) {
        QElapsedTimer timer;
        timer.start();
        const QString err = checkMedia(f, files.size() == 1);
        const QString name = QFileInfo(f).fileName();
        if (err.isEmpty())
            print(QStringLiteral("PASS  %1  (%2 ms)").arg(name).arg(timer.elapsed()));
        else {
            print(QStringLiteral("FAIL  %1  %2").arg(name, err));
            ++failures;
        }
    }
    return failures;
}

int cmdExport(const QString& mediaPath, const QString& outDir)
{
    const MediaInfo info = probeMedia(mediaPath);
    if (!info.ok()) {
        print(QStringLiteral("FAIL  cannot import %1: %2").arg(mediaPath, info.error));
        return 1;
    }
    auto item = std::make_shared<MediaItem>();
    item->id = 1;
    item->path = mediaPath;
    item->info = info;

    ProjectSnapshot snap;
    snap.settings.width = 1280;
    snap.settings.height = 720;
    snap.settings.fps = FrameRate{30, 1};
    snap.media[1] = item;
    snap.timeline = defaultTimeline();
    ops::insertMedia(snap.timeline, *item, 0, -1, snap.settings.fps);
    // Fade in/out to exercise the envelopes.
    for (auto& t : snap.timeline.tracks)
        for (auto& c : t.clips) {
            c.fadeIn = 300000;
            c.fadeOut = 300000;
        }
    const TimeUs length = std::min<TimeUs>(snap.timeline.duration(), 3 * kUsPerSecond);

    QDir().mkpath(outDir);
    int failures = 0;
    int index = 0;
    // Render through the GPU when available (the editor does the same).
    const auto gpuSurface = createGpuSurface();
    for (const ExportPreset& preset : exportPresets()) {
        ++index;
        if (!presetAvailable(preset)) {
            print(QStringLiteral("SKIP  %1 (encoder not available on this machine)").arg(preset.name));
            continue;
        }
        ExportSettings s = settingsFromPreset(preset, snap.settings);
        s.path = QDir(outDir).filePath(QStringLiteral("preset%1.%2").arg(index, 2, 10, QLatin1Char('0')).arg(preset.extension));
        s.rangeStart = 0;
        s.rangeEnd = length;
        QElapsedTimer timer;
        timer.start();
        Exporter exporter;
        if (!exporter.run(snap, s, {}, gpuSurface.get())) {
            print(QStringLiteral("FAIL  %1: %2").arg(preset.name, exporter.error()));
            ++failures;
            continue;
        }
        const qint64 ms = timer.elapsed();
        QString checkPath = s.path;
        qint64 bytes = QFileInfo(s.path).size();
        if (s.muxer == QLatin1String("image2")) { // numbered stills: count them, re-import the first
            const QString pattern = imageSequencePattern(s.path);
            const QFileInfo first(QString::asprintf(pattern.toUtf8().constData(), 1));
            const QStringList files = QDir(first.absolutePath())
                                          .entryList({QFileInfo(s.path).completeBaseName() + QStringLiteral("_*.") + preset.extension}, QDir::Files);
            bytes = 0;
            for (const QString& f : files)
                bytes += QFileInfo(QDir(first.absolutePath()).filePath(f)).size();
            const int expected = int(s.fps.frameAt(length - 1) + 1);
            if (int(files.size()) != expected) {
                print(QStringLiteral("FAIL  %1: %2 images written, %3 expected").arg(preset.name).arg(files.size()).arg(expected));
                ++failures;
                continue;
            }
            checkPath = first.absoluteFilePath();
        }
        QString err;
        if (s.muxer == QLatin1String("webp")) { // FFmpeg reads only still WebP: check the animation chunks instead
            QFile f(s.path);
            const QByteArray head = f.open(QIODevice::ReadOnly) ? f.read(4096) : QByteArray();
            if (!head.startsWith("RIFF") || head.mid(8, 4) != "WEBP" || !head.contains("ANIM") || !head.contains("ANMF"))
                err = QStringLiteral("not an animated WebP");
        } else if (s.muxer == QLatin1String("dash")) { // the DASH reader needs libxml2: check the files instead
            const QStringList segs = QDir(QFileInfo(s.path).absolutePath()).entryList({QStringLiteral("*.m4s")}, QDir::Files);
            if (bytes <= 0 || segs.isEmpty())
                err = QStringLiteral("no manifest / segments");
        } else {
            err = checkMedia(checkPath, false);
        }
        if (!err.isEmpty()) {
            print(QStringLiteral("FAIL  %1: exported file does not re-import: %2").arg(preset.name, err));
            ++failures;
            continue;
        }
        // The picture has the template's size, and size-limited files fit.
        if (!s.videoEncoder.isEmpty() && s.muxer != QLatin1String("dash") && s.muxer != QLatin1String("webp")) {
            const MediaInfo out = probeMedia(checkPath);
            const int w = out.hasVideo() ? out.video.front().width : 0, h = out.hasVideo() ? out.video.front().height : 0;
            if (std::abs(w - s.width) > 1 || std::abs(h - s.height) > 1) {
                print(QStringLiteral("FAIL  %1: picture is %2x%3, the template asks for %4x%5").arg(preset.name).arg(w).arg(h).arg(s.width).arg(s.height));
                ++failures;
                continue;
            }
        }
        if (s.targetSizeBytes > 0 && bytes > s.targetSizeBytes) {
            print(QStringLiteral("FAIL  %1: %2 bytes is over the %3 byte limit").arg(preset.name).arg(bytes).arg(s.targetSizeBytes));
            ++failures;
            continue;
        }
        print(QStringLiteral("PASS  %1 [%2]  (%3 ms, %4 KB, %5)")
                  .arg(preset.name, preset.category)
                  .arg(ms)
                  .arg(bytes / 1024)
                  .arg(exporter.rendererName().isEmpty() ? QStringLiteral("audio only") : exporter.rendererName()));
    }
    // ---- Engine features used by the templates -------------------------------------
    auto exportWith = [&](const QString& name, ExportSettings st) -> QString {
        st.path = QDir(outDir).filePath(name);
        st.rangeStart = 0;
        st.rangeEnd = length;
        Exporter exporter;
        return exporter.run(snap, st, {}, gpuSurface.get()) ? st.path : QString();
    };
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        failures += ok ? 0 : 1;
    };
    const ExportPreset* mp4 = nullptr;
    const ExportPreset* tiktok = nullptr;
    const ExportPreset* hevc = nullptr;
    for (const ExportPreset& preset : exportPresets()) {
        if (preset.name.startsWith(QLatin1String("MP4 - H.264 + AAC")))
            mp4 = &preset;
        if (preset.name.startsWith(QLatin1String("TikTok (")))
            tiktok = &preset;
        if (preset.name == QLatin1String("MP4 - HEVC (H.265) + AAC"))
            hevc = &preset;
    }
    // Fitting a size: 3 s into 400 KB.
    if (mp4) {
        ExportSettings st = settingsFromPreset(*mp4, snap.settings);
        st.targetSizeBytes = 400 * 1000;
        st.quality = -1; // bitrate chosen to fill the size
        QString path = exportWith(QStringLiteral("fit400k.mp4"), st);
        qint64 size = path.isEmpty() ? 0 : QFileInfo(path).size();
        check(size > 300 * 1000 && size <= 400 * 1000, QStringLiteral("filling a file size: 3 s into 400 KB gives %1 KB").arg(size / 1000));
        st = settingsFromPreset(*mp4, snap.settings); // constant quality: the size is a ceiling
        st.quality = 5;                               // (near-lossless would be far bigger)
        st.targetSizeBytes = 400 * 1000;
        path = exportWith(QStringLiteral("cap400k.mp4"), st);
        size = path.isEmpty() ? 0 : QFileInfo(path).size();
        check(size > 0 && size <= 400 * 1000, QStringLiteral("capping a file size: near-lossless 3 s capped at 400 KB gives %1 KB").arg(size / 1000));
    }
    // Framing a 16:9 project into 9:16: bars, blurred bars, crop.
    if (tiktok) {
        auto frameAt = [&](const QString& path) {
            QImage img;
            const MediaInfo info = probeMedia(path);
            VideoDecoder dec;
            FrameConverter conv;
            if (info.hasVideo() && dec.open(path, info.video.front().index, nullptr, false))
                if (const AVFrame* f = dec.frameAt(kUsPerSecond))
                    img = conv.convert(f, QSize(f->width, f->height), true);
            return img;
        };
        auto brightness = [](const QImage& img, QRect r) {
            double sum = 0;
            int n = 0;
            for (int y = r.top(); y < r.bottom(); y += 9)
                for (int x = r.left(); x < r.right(); x += 9, ++n)
                    sum += qGray(img.pixel(x, y));
            return n ? sum / n : 0.0;
        };
        for (Framing f : {Framing::Fit, Framing::BlurFit, Framing::Fill, Framing::Stretch}) {
            ExportSettings st = settingsFromPreset(*tiktok, snap.settings);
            st.framing = f;
            const QString path = exportWith(QStringLiteral("framing%1.mp4").arg(int(f)), st);
            const QImage img = path.isEmpty() ? QImage() : frameAt(path);
            if (img.isNull()) {
                check(false, QStringLiteral("framing %1 renders").arg(int(f)));
                continue;
            }
            const double top = brightness(img, QRect(0, 0, img.width(), img.height() / 5));
            const double middle = brightness(img, QRect(0, img.height() * 2 / 5, img.width(), img.height() / 5));
            const QString names[] = {QStringLiteral("Fit"), QStringLiteral("Fill"), QStringLiteral("Stretch"), QStringLiteral("Blurred fit")};
            const bool ok = f == Framing::Fit ? top < 3 && middle > 10 : top > 5 && middle > 10;
            check(ok && img.size() == QSize(1080, 1920),
                  QStringLiteral("framing %1 for 9:16 (top %2, middle %3)").arg(names[int(f)]).arg(top, 0, 'f', 1).arg(middle, 0, 'f', 1));
        }
    }
    // The same template on the GPU encoder.
    if (mp4) {
        QString used;
        const ExportSettings st = withGpuEncoder(settingsFromPreset(*mp4, snap.settings), &used);
        if (used.isEmpty()) {
            print(QStringLiteral("SKIP  GPU encoding: no hardware encoder on this machine"));
        } else {
            const QString path = exportWith(QStringLiteral("gpu.mp4"), st);
            check(!path.isEmpty() && checkMedia(path, false).isEmpty(), QStringLiteral("template re-targeted to the GPU encoder (%1)").arg(used));
        }
    }
    // A 10-bit template on the GPU: P010 for HEVC / AV1 encoders; H.264 ones can't, so it stays in software.
    for (const ExportPreset& preset : exportPresets())
        if (preset.pixelFormat.contains(QLatin1String("10")) && (preset.videoEncoder == QLatin1String("libx265") || preset.videoEncoder == QLatin1String("libx264"))) {
            QString used;
            const ExportSettings st = withGpuEncoder(settingsFromPreset(preset, snap.settings), &used);
            if (used.isEmpty())
                continue;
            const QString path = exportWith(QStringLiteral("gpu10_%1.%2").arg(preset.videoEncoder, preset.extension), st);
            check(!path.isEmpty() && checkMedia(path, false).isEmpty() && st.pixelFormat == QLatin1String("p010le"),
                  QStringLiteral("10-bit template \"%1\" on the GPU encoder (%2, %3)").arg(preset.name, used, st.pixelFormat));
        }
    // 4:2:2 (H.264 all-intra) stays in software: NVENC's H.264 refused yuv422p.
    for (const ExportPreset& preset : exportPresets())
        if (preset.pixelFormat.contains(QLatin1String("422")) && preset.videoEncoder == QLatin1String("libx264")) {
            QString used;
            const ExportSettings st = withGpuEncoder(settingsFromPreset(preset, snap.settings), &used);
            check(used.isEmpty() && st.videoEncoder == QLatin1String("libx264"),
                  QStringLiteral("4:2:2 template \"%1\" stays on the software encoder (%2)").arg(preset.name, st.videoEncoder));
        }
    // A fit-to-size template over a short render: the bitrate stays sane (500 MB over seconds asked NVENC for gigabits).
    for (const ExportPreset& preset : exportPresets())
        if (preset.name == QLatin1String("Discord Nitro (500 MB)")) {
            QString used;
            const QString path = exportWith(QStringLiteral("fit_short.mp4"), withGpuEncoder(settingsFromPreset(preset, snap.settings), &used));
            check(!path.isEmpty() && checkMedia(path, false).isEmpty(),
                  QStringLiteral("a 500 MB fit-to-size template over a %1 s render (%2)").arg(double(length) / kUsPerSecond).arg(used.isEmpty() ? QStringLiteral("software") : used));
        }
    // HEVC is tagged hvc1 (Apple devices refuse hev1).
    if (hevc) {
        const QString path = exportWith(QStringLiteral("hevc_tag.mp4"), settingsFromPreset(*hevc, snap.settings));
        uint32_t tag = 0;
        AVFormatContext* fc = nullptr;
        if (!path.isEmpty() && avformat_open_input(&fc, path.toUtf8().constData(), nullptr, nullptr) == 0) {
            if (avformat_find_stream_info(fc, nullptr) >= 0)
                for (unsigned i = 0; i < fc->nb_streams; ++i)
                    if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                        tag = fc->streams[i]->codecpar->codec_tag;
            avformat_close_input(&fc);
        }
        check(tag == MKTAG('h', 'v', 'c', '1'), QStringLiteral("HEVC MP4 is tagged hvc1"));
    }
    int templates = 0;
    std::set<QString> categories;
    for (const ExportPreset& preset : exportPresets()) {
        ++templates;
        categories.insert(preset.category);
    }
    check(templates >= 140, QStringLiteral("%1 templates in %2 categories").arg(templates).arg(categories.size()));
    return failures;
}

// Non-default parameter values that make each effect visibly do something.
std::map<QString, double> testValues(const QString& effect)
{
    static const std::map<QString, std::map<QString, double>> table = {
        {QStringLiteral("brightness_contrast"), {{QStringLiteral("brightness"), 0.2}, {QStringLiteral("contrast"), 0.4}}},
        {QStringLiteral("levels"), {{QStringLiteral("gamma"), 2.0}, {QStringLiteral("inWhite"), 0.8}}},
        {QStringLiteral("hsl"), {{QStringLiteral("hue"), 90}, {QStringLiteral("saturation"), -0.5}}},
        {QStringLiteral("color_balance"), {{QStringLiteral("shadowsBlue"), 0.8}, {QStringLiteral("highlightsRed"), -0.8}}},
        {QStringLiteral("transform"),
         {{QStringLiteral("scale"), 0.5}, {QStringLiteral("x"), 0.2}, {QStringLiteral("rotation"), 12},
          {QStringLiteral("border"), 10}, {QStringLiteral("cropLeft"), 0.1}}},
        {QStringLiteral("chroma_key"), {{QStringLiteral("tolerance"), 0.2}}},
        {QStringLiteral("stutter"), {{QStringLiteral("fps"), 2.5}}}, // 1 s is not on a held-frame boundary
        {QStringLiteral("curves"), {{QStringLiteral("darks"), -0.1}, {QStringLiteral("lights"), 0.1}, {QStringLiteral("blueShadows"), 0.1}}},
        {QStringLiteral("color_wheels"),
         {{QStringLiteral("liftHue"), 190}, {QStringLiteral("liftAmount"), 0.5}, {QStringLiteral("gainHue"), 30}, {QStringLiteral("gainAmount"), 0.5}}},
        {QStringLiteral("color_match"), // (as Match Colour would set it: warmer, the blues pulled down)
         {{QStringLiteral("red0"), 0.15}, {QStringLiteral("red4"), 0.6}, {QStringLiteral("blue4"), 0.4}, {QStringLiteral("blue8"), 0.75}}},
        {QStringLiteral("white_balance"),
         {{QStringLiteral("neutral"), double(0x7080a0)}, {QStringLiteral("blackR"), 0.05}, {QStringLiteral("whiteB"), 0.9}}},
        {QStringLiteral("corner_pin"),
         {{QStringLiteral("tlX"), 0.1}, {QStringLiteral("tlY"), 0.05}, {QStringLiteral("trX"), 0.85}, {QStringLiteral("brX"), 0.95},
          {QStringLiteral("brY"), 0.9}, {QStringLiteral("blY"), 0.8}}},
    };
    auto it = table.find(effect);
    return it == table.end() ? std::map<QString, double>{} : it->second;
}

// Text parameters for effects that need them (the LUT effect gets a warm test LUT).
void applyTestText(EffectInstance& e)
{
    if (e.type != QLatin1String("lut_cube"))
        return;
    const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("mvp_selftest_warm.cube"));
    if (!QFileInfo::exists(path)) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QByteArray text = "LUT_3D_SIZE 9\n";
            for (int b = 0; b < 9; ++b)
                for (int g = 0; g < 9; ++g)
                    for (int r = 0; r < 9; ++r)
                        text += QByteArray::number(std::min(1.0, r / 8.0 * 1.1 + 0.03), 'f', 5) + ' ' + QByteArray::number(g / 8.0, 'f', 5) + ' '
                                + QByteArray::number(b / 8.0 * 0.8, 'f', 5) + '\n';
            f.write(text);
        }
    }
    e.params[QStringLiteral("file")].text = path;
}

// Mean absolute difference per channel (0..255), sampled.
double imageDifference(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.isNull())
        return 1e9;
    double sum = 0;
    int64_t n = 0;
    for (int y = 0; y < a.height(); ++y) { // checkerboard: every row, half the pixels
        const auto* ra = reinterpret_cast<const uint32_t*>(a.constScanLine(y));
        const auto* rb = reinterpret_cast<const uint32_t*>(b.constScanLine(y));
        for (int x = y & 1; x < a.width(); x += 2, ++n)
            for (int s : {0, 8, 16})
                sum += std::abs(int((ra[x] >> s) & 0xff) - int((rb[x] >> s) & 0xff));
    }
    return sum / double(std::max<int64_t>(1, n) * 3);
}

QImage contactSheet(const std::vector<QImage>& images)
{
    const int cols = 4, tw = 320, th = 180;
    const int rows = int((images.size() + cols - 1) / cols);
    QImage sheet(cols * tw, std::max(1, rows) * th, QImage::Format_ARGB32_Premultiplied);
    sheet.fill(Qt::darkGray);
    QPainter p(&sheet);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    for (size_t i = 0; i < images.size(); ++i)
        p.drawImage(QRect(int(i % cols) * tw + 2, int(i / cols) * th + 2, tw - 4, th - 4), images[i]);
    return sheet;
}

int cmdEffects(const QString& mediaPath, const QString& outDir)
{
    const MediaInfo info = probeMedia(mediaPath);
    if (!info.ok() || !info.hasVideo()) {
        print(QStringLiteral("FAIL  need a video file: %1").arg(info.error));
        return 1;
    }
    QDir().mkpath(outDir);
    auto item = std::make_shared<MediaItem>();
    item->id = 1;
    item->path = mediaPath;
    item->info = info;
    const int vstream = info.video.front().index;
    const QSize size(1280, 720);
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };

    // Two video tracks: the tested event on top, the same media from a later
    // point underneath so keying and transforms visibly reveal it.
    auto baseSnap = [&] {
        ProjectSnapshot s;
        s.settings.width = size.width();
        s.settings.height = size.height();
        s.settings.fps = FrameRate{30, 1};
        s.media[1] = item;
        Track top, bottom;
        top.id = 1;
        bottom.id = 2;
        Clip c;
        c.mediaId = 1;
        c.streamIndex = vstream;
        c.length = 3 * kUsPerSecond;
        c.id = 10;
        top.clips.push_back(c);
        c.id = 11;
        c.sourceIn = info.duration / 2;
        bottom.clips.push_back(c);
        s.timeline.tracks = {top, bottom};
        s.timeline.nextId = 100;
        return s;
    };

    // ---- Glitch options: every new option visibly changes the picture --------------
    {
        struct Option {
            const char* effect;
            std::vector<std::pair<const char*, double>> base, changed;
            int frames; // rendered in order up to 1 s (time-based effects)
        };
        const std::vector<Option> options = {
            {"chromatic_aberration", {{"amount", 30}}, {{"amount", 30}, {"centerX", 0.1}}, 1},
            {"chromatic_aberration", {{"amount", 30}}, {{"amount", 30}, {"falloff", 3}}, 1},
            {"chromatic_aberration", {{"amount", 30}}, {{"amount", 30}, {"greenShift", 0.8}}, 1},
            {"chromatic_aberration", {{"amount", 30}}, {{"amount", 30}, {"wobble", 1}, {"wobbleSpeed", 3}}, 1},
            {"rgb_split", {}, {{"jitter", 40}}, 1},
            {"rgb_split", {{"redX", -150}}, {{"redX", -150}, {"edge", 1}}, 1},
            {"rgb_split", {{"redX", -150}}, {{"redX", -150}, {"edge", 3}}, 1},
            {"pixel_sort", {}, {{"thresholdBy", 1}}, 1},
            {"pixel_sort", {}, {{"breaks", 0.05}}, 1},
            {"datamosh", {}, {{"motionScale", 3}}, 8},
            {"datamosh", {}, {{"autoClean", 0.1}}, 8},
            {"block_glitch", {{"amount", 0.8}}, {{"amount", 0.8}, {"aspect", 0.5}}, 1},
            {"block_glitch", {{"amount", 0.8}, {"colorGlitch", 1}}, {{"amount", 0.8}, {"colorGlitch", 1}, {"colorMode", 2}}, 1},
            {"block_glitch", {{"amount", 0.8}}, {{"amount", 0.8}, {"noise", 0.5}}, 1},
            {"slice_glitch", {{"amount", 0.9}}, {{"amount", 0.9}, {"direction", 1}}, 1},
            {"slice_glitch", {{"amount", 0.9}}, {{"amount", 0.9}, {"colorGlitch", 1}}, 1},
            {"jpeg_glitch", {}, {{"chromaQuality", 2}}, 1},
            {"jpeg_glitch", {}, {{"blockScale", 1}}, 1},
            {"jpeg_glitch", {{"corruption", 0.5}}, {{"corruption", 0.5}, {"corruptWhat", 1}}, 1},
            {"databend", {}, {{"mode", 4}}, 1},
            {"databend", {}, {{"mode", 5}}, 1},
            {"databend", {{"mode", 3}}, {{"mode", 6}}, 1},
            {"bitcrush", {}, {{"fold", 1}}, 1},
            {"bitcrush", {}, {{"separate", 1}, {"bitsB", 1}}, 1},
            {"pixel_stretch", {}, {{"length", 0.3}}, 1},
            {"pixel_stretch", {}, {{"jitter", 60}}, 1},
            {"pixel_stretch", {}, {{"sweep", 1}, {"sweepSpeed", 0.3}}, 1},
            {"channel_swap", {}, {{"invertR", 1}}, 1},
            {"time_split", {{"delay", 3}}, {{"delay", 3}, {"order", 5}}, 10},
            {"time_split", {{"delay", 3}}, {{"delay", 3}, {"curve", 2}}, 14},
            {"stutter", {{"fps", 10}}, {{"fps", 10}, {"freeze", 1}, {"maxFreeze", 30}}, 25},
            {"strobe", {{"rate", 1}, {"duty", 0.9}}, {{"rate", 1}, {"duty", 0.9}, {"opacity", 0.4}}, 1},
            {"strobe", {{"rate", 1}, {"duty", 0.9}}, {{"rate", 1}, {"duty", 0.9}, {"mode", 5}}, 1},
            {"strobe", {{"rate", 1}, {"duty", 0.9}}, {{"rate", 1}, {"duty", 0.9}, {"mode", 6}}, 1},
            {"block_glitch", {{"amount", 0.8}}, {{"amount", 0.8}, {"fxMix", 0.5}}, 1},
            {"block_glitch", {{"amount", 0.8}}, {{"amount", 0.8}, {"fxMask", 1}}, 1},
        };
        auto renderWith = [&](const Option& o, const std::vector<std::pair<const char*, double>>& values) {
            ProjectSnapshot s = baseSnap();
            EffectInstance e = makeEffect(*findEffect(QString::fromLatin1(o.effect)), 60);
            for (const auto& [k, v] : values)
                e.params[QString::fromLatin1(k)].value = v;
            s.timeline.tracks[0].clips[0].effects.push_back(e);
            Compositor comp(true);
            QImage img;
            for (int f = 30 - o.frames + 1; f <= 30; ++f)
                img = comp.render(s, FrameRate{30, 1}.timeOf(f), size);
            return img;
        };
        int changed = 0, total = 0;
        QStringList unchanged;
        for (const Option& o : options) {
            if (!findEffect(QString::fromLatin1(o.effect)))
                continue;
            ++total;
            const double d = imageDifference(renderWith(o, o.base), renderWith(o, o.changed));
            if (d > 0.3)
                ++changed;
            else
                unchanged << QStringLiteral("%1 (%2)").arg(QString::fromLatin1(o.effect)).arg(d, 0, 'f', 2);
        }
        check(changed == total, QStringLiteral("glitch options change the picture (%1 of %2)%3").arg(changed).arg(total)
                                    .arg(unchanged.isEmpty() ? QString() : QStringLiteral(": unchanged ") + unchanged.join(QStringLiteral(", "))));
        // The "Apply to" box keeps the effect inside it: outside, the picture is untouched.
        Option boxed{"block_glitch", {}, {{"amount", 1.0}, {"maxShift", 400}, {"fxMask", 1}, {"fxFeather", 0}}, 1};
        const QImage clean = renderWith(boxed, {{"amount", 0.0}}), masked = renderWith(boxed, boxed.changed);
        const QRect inside(size.width() / 4 + 8, size.height() / 4 + 8, size.width() / 2 - 16, size.height() / 2 - 16);
        double out = 0, in = 0;
        int nOut = 0, nIn = 0;
        for (int y = 0; y < size.height(); y += 5)
            for (int x = 0; x < size.width(); x += 5) {
                const double d = std::abs(qGray(clean.pixel(x, y)) - qGray(masked.pixel(x, y)));
                if (inside.contains(x, y)) {
                    in += d;
                    ++nIn;
                } else if (!inside.adjusted(-16, -16, 16, 16).contains(x, y)) {
                    out += d;
                    ++nOut;
                }
            }
        check(nIn && nOut && out / nOut < 0.01 && in / nIn > 5,
              QStringLiteral("\"Apply to: inside box\" keeps a glitch inside the box (difference inside %1, outside %2)")
                  .arg(nIn ? in / nIn : 0, 0, 'f', 2).arg(nOut ? out / nOut : 0, 0, 'f', 3));
    }

    // ---- Effects ---------------------------------------------------------------
    std::vector<QImage> sheet;
    {
        Compositor refComp(true);
        const QImage reference = refComp.render(baseSnap(), kUsPerSecond, size);
        sheet.push_back(reference);
        for (const EffectDef& def : videoEffects()) {
            ProjectSnapshot s = baseSnap();
            EffectInstance e = makeEffect(def, 50);
            for (const auto& [id, v] : testValues(def.id))
                e.params[id].value = v;
            applyTestText(e);
            s.timeline.tracks[0].clips[0].effects.push_back(e);
            Compositor comp(true);
            QElapsedTimer timer;
            timer.start();
            comp.render(s, kUsPerSecond - 5 * 33334, size); // warm up the decoder
            timer.restart();
            for (int i = 4; i >= 0; --i)
                comp.render(s, kUsPerSecond - i * 33334, size);
            const double ms = timer.elapsed() / 5.0;
            const QImage img = comp.render(s, kUsPerSecond, size);
            const double diff = imageDifference(img, reference);
            sheet.push_back(img);
            if (def.id == QLatin1String("remove_bg") && !segmentationAvailable()) { // (a build without ONNX Runtime: the picture as it is)
                print(QStringLiteral("SKIP  effect %1: no ONNX Runtime in this build").arg(def.name));
                continue;
            }
            check(diff >= 0.5, QStringLiteral("effect %1  (difference %2, %3 ms/frame at 720p incl. decoding)")
                                   .arg(def.name)
                                   .arg(diff, 0, 'f', 1)
                                   .arg(ms, 0, 'f', 1));
        }
        contactSheet(sheet).save(QDir(outDir).filePath(QStringLiteral("effects.png")));
    }

    // ---- Transitions -------------------------------------------------------------
    sheet.clear();
    {
        // Two overlapping events on the top track: 0-3 s and 2-5 s.
        auto overlapSnap = [&] {
            ProjectSnapshot s = baseSnap();
            Clip b = s.timeline.tracks[0].clips[0];
            b.id = 12;
            b.start = 2 * kUsPerSecond;
            b.sourceIn = info.duration / 3;
            // Inverted, so the shape of each transition is easy to see.
            b.effects.push_back(makeEffect(*findEffect(QStringLiteral("invert")), 40));
            s.timeline.tracks[0].clips.push_back(b);
            s.timeline.tracks[1].clips.clear();
            return s;
        };
        for (const TransitionDef& def : transitions()) {
            ProjectSnapshot s = overlapSnap();
            s.timeline.tracks[0].clips[1].transitionIn = makeTransition(def);
            Compositor comp(true);
            const QImage before = comp.render(s, kUsPerSecond * 19 / 10, size);
            QElapsedTimer timer;
            timer.start();
            const QImage mid = comp.render(s, 2 * kUsPerSecond + kUsPerSecond * 4 / 10, size);
            const qint64 ms = timer.elapsed();
            sheet.push_back(mid);
            check(imageDifference(mid, before) >= 0.5, QStringLiteral("transition %1  (%2 ms at 720p)").arg(def.name).arg(ms));
        }
        // A transition on a plain fade-in (from nothing to the event).
        ProjectSnapshot s = baseSnap();
        s.timeline.tracks[1].clips.clear();
        Clip& c0 = s.timeline.tracks[0].clips[0];
        c0.fadeIn = kUsPerSecond;
        c0.transitionIn = makeTransition(*findTransition(QStringLiteral("iris")));
        Compositor comp(true);
        const QImage fadeMid = comp.render(s, kUsPerSecond / 2, size);
        sheet.push_back(fadeMid);
        check(imageDifference(fadeMid, comp.render(s, kUsPerSecond * 3 / 2, size)) >= 0.5,
              QStringLiteral("fade-in with Iris transition"));
        contactSheet(sheet).save(QDir(outDir).filePath(QStringLiteral("transitions.png")));

        // Every transition starts exactly on the first picture and ends on the second.
        QImage from = comp.render(baseSnap(), kUsPerSecond, size), to = from;
        to.invertPixels();
        QStringList wrong;
        for (const TransitionDef& def : transitions()) {
            const Params prm = resolveParams(def.params, defaultParams(def.params), 0);
            RenderContext rc;
            rc.scale = size.width() / 1920.0;
            QImage start(size, QImage::Format_ARGB32_Premultiplied), end(size, QImage::Format_ARGB32_Premultiplied);
            def.apply(start, &from, &to, 0.0, prm, rc);
            def.apply(end, &from, &to, 1.0, prm, rc);
            const double ds = imageDifference(start, from), de = imageDifference(end, to);
            if (ds > 0.5 || de > 0.5)
                wrong << QStringLiteral("%1 (%2 / %3)").arg(def.name).arg(ds, 0, 'f', 2).arg(de, 0, 'f', 2);
        }
        check(transitions().size() >= 27 && wrong.isEmpty(),
              QStringLiteral("all %1 transitions start on the first picture and end on the second %2")
                  .arg(transitions().size()).arg(wrong.join(QStringLiteral(", "))));
    }

    // ---- Keyframes ------------------------------------------------------------------
    {
        ProjectSnapshot s = baseSnap();
        EffectInstance e = makeEffect(*findEffect(QStringLiteral("transform")), 60);
        e.params[QStringLiteral("scale")].addKey(0, 0.2);
        e.params[QStringLiteral("scale")].addKey(2 * kUsPerSecond, 1.0);
        s.timeline.tracks[0].clips[0].effects.push_back(e);
        const double v = e.params[QStringLiteral("scale")].at(kUsPerSecond, ParamType::Number);
        Compositor comp(true);
        const double d = imageDifference(comp.render(s, kUsPerSecond / 2, size), comp.render(s, kUsPerSecond * 3 / 2, size));
        check(std::abs(v - 0.6) < 1e-6 && d >= 0.5, QStringLiteral("keyframed transform (midpoint value %1)").arg(v));
    }

    // ---- Track and output FX -----------------------------------------------------------
    {
        ProjectSnapshot s = baseSnap();
        Compositor comp(true);
        const QImage plain = comp.render(s, kUsPerSecond, size);
        s.timeline.tracks[0].effects.push_back(makeEffect(*findEffect(QStringLiteral("black_white")), 70));
        const QImage trackFx = comp.render(s, kUsPerSecond, size);
        s.timeline.outputEffects.push_back(makeEffect(*findEffect(QStringLiteral("vignette")), 71));
        const QImage outFx = comp.render(s, kUsPerSecond, size);
        check(imageDifference(plain, trackFx) >= 0.5 && imageDifference(trackFx, outFx) >= 0.5,
              QStringLiteral("track FX and video output FX"));
    }

    // ---- Save / load round trip, then export ---------------------------------------------
    {
        Project proj;
        const int mediaId = proj.addMedia(info);
        Timeline tl = proj.timeline();
        ops::insertMedia(tl, *proj.mediaItem(mediaId), 0, -1, proj.settings().fps);
        Clip& clip = tl.tracks[0].clips[0];
        EffectInstance e = makeEffect(*findEffect(QStringLiteral("transform")), tl.allocId());
        e.params[QStringLiteral("x")].addKey(0, -0.5);
        e.params[QStringLiteral("x")].addKey(kUsPerSecond, 0.5);
        e.params[QStringLiteral("x")].keys[0].interp = Keyframe::Interp::Smooth;
        clip.effects.push_back(e);
        clip.effects.push_back(makeEffect(*findEffect(QStringLiteral("ffmpeg_filter")), tl.allocId()));
        clip.fadeIn = kUsPerSecond;
        clip.transitionIn = makeTransition(*findTransition(QStringLiteral("clock")));
        tl.tracks[0].effects.push_back(makeEffect(*findEffect(QStringLiteral("sepia")), tl.allocId()));
        tl.outputEffects.push_back(makeEffect(*findEffect(QStringLiteral("noise")), tl.allocId()));
        proj.applyEdit(tl, QStringLiteral("test"));
        const QString path = QDir(outDir).filePath(QStringLiteral("roundtrip.mvp"));
        QString err;
        Project loaded;
        const bool ok = proj.save(path, &err) && loaded.load(path, &err) && loaded.timeline() == proj.timeline();
        check(ok, QStringLiteral("project save/load keeps effects, keyframes and transitions %1").arg(err));

        ExportSettings es = settingsFromPreset(exportPresets().front(), loaded.settings());
        es.width = 640;
        es.height = 360;
        es.path = QDir(outDir).filePath(QStringLiteral("effects_export.mp4"));
        es.rangeStart = 0;
        es.rangeEnd = std::min<TimeUs>(2 * kUsPerSecond, loaded.timeline().duration());
        Exporter exporter;
        const bool exported = exporter.run(*loaded.snapshot(), es);
        check(exported, QStringLiteral("export with effects and transitions %1").arg(exporter.error()));
    }

    // Dither / Retro Palette: every method on every palette. Every pixel must be one of the
    // palette's colours (adaptive: at most that many), and the picture must not be flat.
    {
        ProjectSnapshot s;
        s.settings.width = 320;
        s.settings.height = 180;
        s.timeline = defaultTimeline();
        ops::insertGenerated(s.timeline, *findGeneratorPreset(QStringLiteral("bars_smpte")), 0, 0, s.settings.fps); // every hue, light to dark
        Clip* clip = nullptr;
        for (Track& t : s.timeline.tracks)
            if (!t.clips.empty())
                clip = &t.clips.front();
        Compositor comp(true);
        QStringList wrong;
        int renders = 0, flat = 0;
        QElapsedTimer clock;
        clock.start();
        const auto& pals = dither::palettes();
        for (int m = 0; m < int(dither::methods().size()); ++m)
            for (int pi = 0; pi < int(pals.size()); ++pi) {
                const dither::Palette& pal = pals[size_t(pi)];
                EffectInstance e = makeEffect(*findEffect(QStringLiteral("dither")), 700);
                e.params[QStringLiteral("method")].value = m;
                e.params[QStringLiteral("palette")].value = pi;
                e.params[QStringLiteral("pixelSize")].value = 2;
                e.params[QStringLiteral("match")].value = (m + pi) % 2; // both ways of matching
                clip->effects = {e};
                const QImage img = comp.render(s, kUsPerSecond, QSize(160, 90)).convertToFormat(QImage::Format_RGB32);
                ++renders;
                std::set<uint32_t> seen;
                bool ok = true;
                for (int y = 0; y < img.height() && ok; ++y)
                    for (int x = 0; x < img.width(); ++x) {
                        const uint32_t c = img.pixel(x, y) & 0xffffff;
                        seen.insert(c);
                        if (pal.levels > 0) {
                            auto onLevel = [&](uint32_t v) {
                                for (int q = 0; q < pal.levels; ++q)
                                    if (uint32_t((q * 255 * 2 + (pal.levels - 1)) / (2 * (pal.levels - 1))) == v)
                                        return true;
                                return false;
                            };
                            ok = onLevel((c >> 16) & 0xff) && onLevel((c >> 8) & 0xff) && onLevel(c & 0xff);
                        } else if (!pal.adaptive) {
                            ok = std::find(pal.colors.begin(), pal.colors.end(), c) != pal.colors.end();
                        }
                        if (!ok)
                            break;
                    }
                if (pal.adaptive && int(seen.size()) > pal.adaptive)
                    ok = false;
                if (seen.size() < 2 && ++flat <= 6)
                    print(QStringLiteral("INFO  flat: %1 on %2 (%3)").arg(QString::fromUtf8(dither::methods()[size_t(m)].name), QString::fromUtf8(pal.name))
                              .arg(seen.empty() ? 0u : *seen.begin(), 6, 16, QLatin1Char('0')));
                if (!ok && wrong.size() < 8)
                    wrong << QStringLiteral("%1 on %2").arg(QString::fromUtf8(dither::methods()[size_t(m)].name), QString::fromUtf8(pal.name));
            }
        check(wrong.isEmpty() && flat == 0,
              QStringLiteral("Dither: %1 methods x %2 palettes (%3 renders, %4 ms): every pixel is a palette colour, none flat%5")
                  .arg(dither::methods().size()).arg(pals.size()).arg(renders).arg(clock.elapsed())
                  .arg(wrong.isEmpty() ? (flat ? QStringLiteral(", but %1 flat").arg(flat) : QString()) : QStringLiteral(" (not: %1)").arg(wrong.join(QStringLiteral("; ")))));
        // The threshold maps: each a permutation of the ranks (every level reached once).
        bool maps = true;
        for (int id = 0; id < 12; ++id) {
            const dither::ThresholdMap& map = dither::thresholdMap(id);
            std::vector<float> v = map.values;
            std::sort(v.begin(), v.end());
            for (size_t i = 0; i < v.size(); ++i)
                maps = maps && std::abs(v[i] - (float(i) + 0.5f) / float(v.size())) < 1e-6f;
        }
        check(maps, QStringLiteral("Dither: the 12 threshold maps (Bayer 2-16, clustered dots, halftone, lines, crosshatch, 64x64 blue noise) each use every level once"));
    }
    return failures;
}

// Fraction of pixels that are not fully transparent.
double coverage(const QImage& img)
{
    int64_t n = 0, hit = 0;
    for (int y = 0; y < img.height(); y += 2) {
        const auto* row = reinterpret_cast<const uint32_t*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x += 2, ++n)
            hit += (row[x] >> 24) != 0;
    }
    return n ? double(hit) / double(n) : 0.0;
}

int cmdGenerators(const QString& outDir)
{
    QDir().mkpath(outDir);
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    const QSize size(1280, 720);
    const FrameRate fps{30, 1};
    std::vector<QImage> sheet;

    auto fixture = [](const GeneratorPreset& preset) {
        auto spec = makeGenerator(preset);
        if (spec.type == QLatin1String("audio_waveform") || preset.id == QLatin1String("animation_audio")) {
            QJsonArray peaks;
            for (int i=0;i<64;++i) peaks.append(.2+.7*std::abs(std::sin(i*.3)));
            QJsonObject envelope{{"version",1},{"duration",10},{"left",peaks},{"right",peaks}};
            if (spec.type == QLatin1String("audio_waveform")) spec.data=envelope;
            else spec.data["audio"]=QJsonObject{{"source",envelope}};
        }
        return spec;
    };

    for (const GeneratorPreset& preset : generatorPresets()) {
        const GeneratorSpec spec = fixture(preset);
        if (!findGenerator(spec.type)) {
            check(false, QStringLiteral("preset %1 refers to an unknown generator").arg(preset.name));
            continue;
        }
        QElapsedTimer timer;
        timer.start();
        const QImage mid = renderGenerator(spec, size, 1280.0 / 1920.0, preset.length / 2, preset.length, fps);
        const qint64 ms = timer.elapsed();
        // Show on grey so transparent areas are visible in the contact sheet.
        QImage tile(size, QImage::Format_ARGB32_Premultiplied);
        tile.fill(QColor(70, 70, 80));
        QPainter(&tile).drawImage(0, 0, mid);
        sheet.push_back(tile);
        const double cov = coverage(mid);
        const bool blank = preset.id == QLatin1String("graphics_blank") || preset.id == QLatin1String("animation_blank")
                           || preset.id == QLatin1String("adjustment"); // (no picture of its own)
        check(!mid.isNull() && (blank ? cov == 0 : cov > 0.002), QStringLiteral("generator preset %1  (%2% of frame drawn, %3 ms at 720p)")
                               .arg(preset.name)
                               .arg(cov * 100, 0, 'f', 1)
                               .arg(ms));
    }
    contactSheet(sheet).save(QDir(outDir).filePath(QStringLiteral("generators.png")));

    // Animations: a sliding title differs between start and middle, credits
    // move, the typewriter shows fewer characters early on.
    auto renderAt = [&](const char* presetId, double seconds) {
        const GeneratorPreset* p = findGeneratorPreset(QString::fromLatin1(presetId));
        return renderGenerator(fixture(*p), size, 1280.0 / 1920.0, TimeUs(seconds * kUsPerSecond), p->length, fps);
    };
    check(imageDifference(renderAt("lower_third", 0.1), renderAt("lower_third", 2.5)) > 0.5, QStringLiteral("lower third slides in"));
    check(imageDifference(renderAt("credits", 4), renderAt("credits", 8)) > 0.5, QStringLiteral("credits roll scrolls"));
    check(coverage(renderAt("typewriter", 0.5)) < coverage(renderAt("typewriter", 2.5)), QStringLiteral("typewriter reveals text"));
    check(imageDifference(renderAt("timecode", 1), renderAt("timecode", 2)) > 0.01, QStringLiteral("timecode changes over time"));

    // Animated generators move, and draw the same frame the same way every time
    // (they are worked out from the time alone, so any frame renders on its own).
    {
        QStringList still, unstable;
        int animated = 0;
        for (const GeneratorPreset& preset : generatorPresets()) {
            const GeneratorSpec spec = fixture(preset);
            const GeneratorDef* def = findGenerator(spec.type);
            if (!def || !def->isAnimating)
                continue;
            GeneratorContext gc;
            gc.time = kUsPerSecond;
            gc.length = preset.length;
            gc.data = &spec.data;
            if (!def->isAnimating(resolveParams(def->params, spec.params, gc.time), gc))
                continue;
            ++animated;
            // Some presets finish their entrance before 1 s or intentionally hold.
            // Sample the entrance and later frames, rather than demanding motion
            // between two arbitrarily chosen points in a hold.
            const QImage a = renderGenerator(spec, size, 1280.0 / 1920.0, 0, preset.length, fps);
            bool changed=false, repeatable=true;
            for (TimeUs at : {kUsPerSecond/10, kUsPerSecond, kUsPerSecond*3/2, preset.length/2}) {
                const QImage b=renderGenerator(spec,size,1280.0/1920.0,at,preset.length,fps);
                changed |= imageDifference(a,b)>0;
                repeatable &= imageDifference(b,renderGenerator(spec,size,1280.0/1920.0,at,preset.length,fps))==0;
            }
            if (!changed) still << preset.name;
            if (!repeatable) unstable << preset.name;
        }
        check(animated >= 15 && still.isEmpty() && unstable.isEmpty(),
              QStringLiteral("%1 animated generator presets change across their duration and repeat exactly (still: %2; not repeatable: %3)")
                  .arg(animated).arg(still.join(QStringLiteral(", ")), unstable.join(QStringLiteral(", "))));
    }
    check(coverage(renderAt("kinetic_text", 0.2)) < coverage(renderAt("kinetic_text", 2.5)) * 0.5,
          QStringLiteral("kinetic text puts its words on one by one"));
    {
        // Half way along the bar: still the track early on, the red bar later.
        const QPoint mid(size.width() / 2, size.height() - 2);
        const QRgb early = renderAt("progress_bar", 3).pixel(mid), late = renderAt("progress_bar", 27).pixel(mid);
        check(qRed(late) > 200 && qGreen(late) < 100 && qGreen(early) > 20 && qAlpha(early) < 128,
              QStringLiteral("progress bar fills up over the event"));
    }
    check(coverage(renderAt("letterbox", 0.1)) < coverage(renderAt("letterbox", 5)) * 0.8 && coverage(renderAt("letterbox", 5)) > 0.2,
          QStringLiteral("letterbox bars slide in to cover a quarter of a 16:9 frame (%1%)").arg(coverage(renderAt("letterbox", 5)) * 100, 0, 'f', 1));
    {
        // Colour bars: the first SMPTE bar is 75% grey, the last of the top row 75% blue.
        const QImage bars = renderAt("bars_smpte", 1);
        const QRgb grey = bars.pixel(size.width() / 14, size.height() / 4), blue = bars.pixel(size.width() * 13 / 14, size.height() / 4);
        check(qRed(grey) == 191 && qGreen(grey) == 191 && qBlue(blue) == 191 && qRed(blue) == 0,
              QStringLiteral("SMPTE colour bars at 75% (grey %1, blue %2)").arg(qRed(grey)).arg(qBlue(blue)));
    }

    // Through the compositor and the project file, with a keyframed position.
    {
        Project proj;
        Timeline tl = proj.timeline();
        const int id = ops::insertGenerated(tl, *findGeneratorPreset(QStringLiteral("title")), 0, 0, proj.settings().fps);
        // Background on its own video track below the title.
        const int bgTrack = ops::addTrack(tl, TrackType::Video, 1);
        ops::insertGenerated(tl, *findGeneratorPreset(QStringLiteral("gradient")), 0, bgTrack, proj.settings().fps);
        Clip* title = tl.findClip(id);
        title->generator.params[QStringLiteral("x")].addKey(kUsPerSecond, 0.3);
        title->generator.params[QStringLiteral("x")].addKey(3 * kUsPerSecond, 0.7);
        proj.applyEdit(tl, QStringLiteral("test"));

        Compositor comp(true);
        const QImage a = comp.render(*proj.snapshot(), kUsPerSecond, size);
        const QImage b = comp.render(*proj.snapshot(), 3 * kUsPerSecond, size);
        check(imageDifference(a, b) > 0.3, QStringLiteral("keyframed title moves over a gradient background"));
        a.save(QDir(outDir).filePath(QStringLiteral("title_composited.png")));

        const QString path = QDir(outDir).filePath(QStringLiteral("generators.mvp"));
        QString err;
        Project loaded;
        check(proj.save(path, &err) && loaded.load(path, &err) && loaded.timeline() == proj.timeline(),
              QStringLiteral("generated media survives save/load %1").arg(err));

        ExportSettings es = settingsFromPreset(exportPresets().front(), loaded.settings());
        es.width = 640;
        es.height = 360;
        es.path = QDir(outDir).filePath(QStringLiteral("title_export.mp4"));
        es.audioEncoder.clear(); // no audio events: a silent track would fail the re-import check
        es.rangeEnd = 2 * kUsPerSecond;
        Exporter exporter;
        const bool ok = exporter.run(*loaded.snapshot(), es);
        check(ok, QStringLiteral("export of a title over a background %1").arg(exporter.error()));
        if (ok) {
            const QString e = checkMedia(es.path, false);
            check(e.isEmpty(), QStringLiteral("exported title video re-imports %1").arg(e));
        }
    }
    return failures;
}

// Estimated frequency of the left channel from zero crossings.
double estimateFrequency(const std::vector<float>& stereo, int rate)
{
    int crossings = 0;
    const size_t n = stereo.size() / 2;
    for (size_t i = 1; i < n; ++i)
        if ((stereo[(i - 1) * 2] < 0) != (stereo[i * 2] < 0))
            ++crossings;
    return n ? crossings / 2.0 / (double(n) / rate) : 0.0;
}

QRgb pixelAt(const QImage& img, double fx, double fy)
{
    return img.pixel(int(fx * (img.width() - 1)), int(fy * (img.height() - 1)));
}

int cmdSpeed(const QString& mediaPath, const QString& outDir)
{
    const MediaInfo info = probeMedia(mediaPath);
    if (!info.ok() || !info.hasVideo() || !info.hasAudio()) {
        print(QStringLiteral("FAIL  need a file with video and audio: %1").arg(info.error));
        return 1;
    }
    QDir().mkpath(outDir);
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    auto item = std::make_shared<MediaItem>();
    item->id = 1;
    item->path = mediaPath;
    item->info = info;
    const QSize size(1280, 720);
    const FrameRate fps{30, 1};
    const TimeUs frame = fps.frameDuration();

    auto snapWith = [&](const std::function<void(Clip&)>& setup) {
        ProjectSnapshot s;
        s.settings.width = size.width();
        s.settings.height = size.height();
        s.settings.fps = fps;
        s.media[1] = item;
        s.timeline = defaultTimeline();
        ops::insertMedia(s.timeline, *item, 0, -1, fps);
        for (auto& t : s.timeline.tracks)
            for (auto& c : t.clips)
                setup(c);
        return s;
    };
    const ProjectSnapshot plain = snapWith([](Clip&) {});
    Compositor plainComp(true);
    auto plainAt = [&](TimeUs t) { return plainComp.render(plain, t, size); };
    const TimeUs dur = plain.timeline.duration();

    // ---- Video speed ---------------------------------------------------------------
    {
        ProjectSnapshot s = snapWith([&](Clip& c) { ops::setClipSpeed(c, item.get(), 2.0, true, fps); });
        Compositor comp(true);
        const double d = imageDifference(comp.render(s, kUsPerSecond, size), plainAt(2 * kUsPerSecond));
        check(d < 1.0 && std::llabs(s.timeline.duration() - dur / 2) <= frame,
              QStringLiteral("200% speed: half the length, shows the frame from twice as far (diff %1)").arg(d, 0, 'f', 2));
    }
    {
        ProjectSnapshot s = snapWith([](Clip& c) { c.reverse = true; });
        Compositor comp(true);
        const TimeUs t = kUsPerSecond / 2;
        const Clip& c = s.timeline.tracks[0].clips[0];
        const double d = imageDifference(comp.render(s, t, size), plainAt(c.sourceTimeAt(t)));
        check(d < 1.0, QStringLiteral("reverse shows the mirrored source time (diff %1)").arg(d, 0, 'f', 2));
        // Reverse playback speed: consecutive frames run backwards through the source.
        QElapsedTimer timer;
        timer.start();
        const int frames = 60;
        for (int i = 0; i < frames; ++i)
            comp.render(s, kUsPerSecond + fps.timeOf(i), size);
        const double ms = double(timer.elapsed()) / frames;
        check(ms < 40.0, QStringLiteral("reverse playback %1 ms/frame at 720p (decoded-frame history)").arg(ms, 0, 'f', 1));
    }
    {
        ProjectSnapshot s = snapWith([&](Clip& c) {
            ops::setClipSpeed(c, item.get(), 0.25, true, fps);
            c.frameBlend = true;
        });
        Compositor comp(true);
        // At 25% each source frame spans 4 output frames; the 2nd is a blend.
        const TimeUs t = fps.timeOf(60 * 4 + 2);
        const QImage blended = comp.render(s, t, size);
        const Clip& c = s.timeline.tracks[0].clips[0];
        const TimeUs src = c.sourceTimeAt(t);
        const double dPrev = imageDifference(blended, plainAt(fps.timeOf(fps.frameAt(src))));
        const double dNext = imageDifference(blended, plainAt(fps.timeOf(fps.frameAt(src) + 1)));
        check(dPrev > 0.2 && dNext > 0.2, QStringLiteral("frame blending mixes neighbouring frames in slow motion"));
    }
    {
        // Splitting a sped-up reversed event must not change what is shown.
        ProjectSnapshot s = snapWith([&](Clip& c) {
            ops::setClipSpeed(c, item.get(), 1.5, true, fps);
            c.reverse = true;
        });
        Compositor before(true), after(true);
        const TimeUs cut = fps.snap(s.timeline.duration() / 2);
        const QImage a1 = before.render(s, cut - 10 * frame, size), b1 = before.render(s, cut + 10 * frame, size);
        ops::splitClips(s.timeline, {}, cut);
        const QImage a2 = after.render(s, cut - 10 * frame, size), b2 = after.render(s, cut + 10 * frame, size);
        check(imageDifference(a1, a2) < 0.5 && imageDifference(b1, b2) < 0.5, QStringLiteral("split keeps speed + reverse mapping"));
    }

    // ---- Pan/Crop --------------------------------------------------------------------
    {
        const QImage ref = plainAt(kUsPerSecond);
        ProjectSnapshot s = snapWith([](Clip& c) { c.panCrop[QStringLiteral("zoom")].value = 2.0; });
        Compositor comp(true);
        const QImage zoomed = comp.render(s, kUsPerSecond, size);
        zoomed.save(QDir(outDir).filePath(QStringLiteral("pancrop_zoom2.png")));
        // 2x about the centre: output point 0.5 + d shows source point 0.5 + d / 2.
        const QRgb c0 = pixelAt(ref, 0.55, 0.45), c1 = pixelAt(zoomed, 0.6, 0.4);
        const bool mapped = std::abs(qRed(c0) - qRed(c1)) + std::abs(qGreen(c0) - qGreen(c1)) + std::abs(qBlue(c0) - qBlue(c1)) < 40;
        check(mapped && imageDifference(ref, zoomed) > 5, QStringLiteral("pan/crop zoom 2x maps points about the centre"));

        ProjectSnapshot r = snapWith([](Clip& c) { c.panCrop[QStringLiteral("rotation")].value = 90; });
        const QImage rotated = comp.render(r, kUsPerSecond, size);
        rotated.save(QDir(outDir).filePath(QStringLiteral("pancrop_rot90.png")));
        check(imageDifference(ref, rotated) > 5, QStringLiteral("pan/crop rotation"));

        // Ken Burns: keyframed zoom and pan.
        ProjectSnapshot k = snapWith([](Clip& c) {
            c.panCrop[QStringLiteral("zoom")].addKey(0, 1.0);
            c.panCrop[QStringLiteral("zoom")].addKey(3 * kUsPerSecond, 1.8);
            c.panCrop[QStringLiteral("centerX")].addKey(0, 0.5);
            c.panCrop[QStringLiteral("centerX")].addKey(3 * kUsPerSecond, 0.3);
        });
        check(imageDifference(comp.render(k, kUsPerSecond / 2, size), comp.render(k, kUsPerSecond * 5 / 2, size)) > 5,
              QStringLiteral("keyframed pan/crop (Ken Burns) animates"));

        // Fill mode on a 4:3 picture in a 16:9 frame leaves no black bars.
        auto item43 = std::make_shared<MediaItem>(*item);
        for (auto& v : item43->info.video)
            v.sarNum = 3, v.sarDen = 4; // squeeze 16:9 pixels into 4:3 display
        ProjectSnapshot f = snapWith([](Clip& c) { c.panCrop[QStringLiteral("mode")].value = 1; });
        f.media[1] = item43;
        ProjectSnapshot fit = f;
        for (auto& t : fit.timeline.tracks)
            for (auto& c : t.clips)
                c.panCrop.clear();
        const QImage filled = comp.render(f, kUsPerSecond, size), fitted = comp.render(fit, kUsPerSecond, size);
        check(qGray(pixelAt(fitted, 0.02, 0.5)) < 5 && qGray(pixelAt(filled, 0.02, 0.5)) > 5,
              QStringLiteral("Fill framing removes pillarbox bars that Fit leaves"));
    }

    // ---- Audio speed ---------------------------------------------------------------------
    {
        auto mixSecond = [&](const ProjectSnapshot& s, TimeUs at) {
            AudioMixer mixer(48000);
            std::vector<float> out(48000 * 2), chunk(1024 * 2);
            const int64_t start = usToSamples(at, 48000);
            for (int i = 0; i < 48000; i += 1024) {
                const int n = std::min(1024, 48000 - i);
                mixer.mix(s, start + i, n, chunk.data());
                std::copy_n(chunk.begin(), n * 2, out.begin() + i * 2);
            }
            return out;
        };
        const double base = estimateFrequency(mixSecond(plain, kUsPerSecond / 2), 48000);
        ProjectSnapshot keep = snapWith([&](Clip& c) { ops::setClipSpeed(c, item.get(), 2.0, true, fps); });
        ProjectSnapshot tape = snapWith([&](Clip& c) {
            ops::setClipSpeed(c, item.get(), 2.0, true, fps);
            c.preservePitch = false;
        });
        ProjectSnapshot rev = snapWith([](Clip& c) { c.reverse = true; });
        const double fKeep = estimateFrequency(mixSecond(keep, kUsPerSecond / 4), 48000);
        const double fTape = estimateFrequency(mixSecond(tape, kUsPerSecond / 4), 48000);
        const double fRev = estimateFrequency(mixSecond(rev, kUsPerSecond / 2), 48000);
        check(std::abs(fKeep - base) < base * 0.05, QStringLiteral("2x with preserved pitch: %1 Hz (source %2 Hz)").arg(fKeep, 0, 'f', 0).arg(base, 0, 'f', 0));
        check(std::abs(fTape - 2 * base) < base * 0.1, QStringLiteral("2x tape-style: %1 Hz (expected about %2 Hz)").arg(fTape, 0, 'f', 0).arg(2 * base, 0, 'f', 0));
        check(std::abs(fRev - base) < base * 0.05, QStringLiteral("reversed audio plays: %1 Hz").arg(fRev, 0, 'f', 0));
    }

    // ---- Save / load ------------------------------------------------------------------------
    {
        Project proj;
        const int id = proj.addMedia(info);
        Timeline tl = proj.timeline();
        ops::insertMedia(tl, *proj.mediaItem(id), 0, -1, proj.settings().fps);
        Clip& c = tl.tracks[0].clips[0];
        ops::setClipSpeed(c, proj.mediaItem(id).get(), 0.5, true, proj.settings().fps);
        c.reverse = true;
        c.frameBlend = true;
        c.preservePitch = false;
        c.panCrop[QStringLiteral("zoom")].addKey(0, 1.2);
        c.panCrop[QStringLiteral("mode")].value = 1;
        proj.applyEdit(tl, QStringLiteral("test"));
        const QString path = QDir(outDir).filePath(QStringLiteral("speed.mvp"));
        QString err;
        Project loaded;
        check(proj.save(path, &err) && loaded.load(path, &err) && loaded.timeline() == proj.timeline(),
              QStringLiteral("speed, reverse and pan/crop survive save/load %1").arg(err));
    }
    return failures;
}

std::shared_ptr<MediaItem> loadItem(const QString& path, int id)
{
    auto item = std::make_shared<MediaItem>();
    item->id = id;
    item->path = path;
    item->info = probeMedia(path);
    return item->info.ok() ? item : nullptr;
}

int cmdGpu(const QString& mediaDir, const QString& outDir)
{
    QDir().mkpath(outDir);
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    auto surface = createGpuSurface();
    QString err;
    std::unique_ptr<GpuCompositor> gpu = surface ? GpuCompositor::create(surface.get(), true, &err) : nullptr;
    if (!gpu) {
        print(QStringLiteral("SKIP  no usable GPU: %1").arg(err));
        return 0;
    }
    print(QStringLiteral("INFO  renderer: %1, hardware decoder: %2").arg(gpu->name(), hw::decodeDeviceName()));

    const QDir dir(mediaDir);
    auto main = loadItem(dir.filePath(QStringLiteral("h264_aac.mp4")), 1);
    if (!main) {
        print(QStringLiteral("FAIL  h264_aac.mp4 not found in %1").arg(mediaDir));
        return 1;
    }
    const QSize size(1280, 720);
    const FrameRate fps{30, 1};
    Compositor cpu(true);
    std::vector<QImage> sheet;

    auto baseSnap = [&](const std::shared_ptr<MediaItem>& item) {
        ProjectSnapshot s;
        s.settings.width = 1920;
        s.settings.height = 1080;
        s.settings.fps = fps;
        s.media[item->id] = item;
        s.timeline = defaultTimeline();
        ops::insertMedia(s.timeline, *item, 0, 0, fps);
        return s;
    };
    // Lower track with the main clip so transparency and transforms show something.
    auto withBackground = [&](ProjectSnapshot s) {
        s.media[main->id] = main;
        const int t = ops::addTrack(s.timeline, TrackType::Video, 1);
        Clip bg;
        bg.id = s.timeline.allocId();
        bg.mediaId = main->id;
        bg.streamIndex = main->info.video.front().index;
        bg.sourceIn = main->info.duration / 2;
        bg.length = 3 * kUsPerSecond;
        s.timeline.tracks[size_t(t)].clips.push_back(bg);
        return s;
    };
    auto compare = [&](const QString& name, const ProjectSnapshot& s, TimeUs t, double tolerance) {
        const QImage a = cpu.render(s, t, size);
        const QImage b = gpu->render(s, t, size);
        sheet.push_back(b);
        const double d = imageDifference(a, b);
        check(d <= tolerance, QStringLiteral("GPU matches CPU: %1  (mean difference %2)").arg(name).arg(d, 0, 'f', 2));
        if (d > tolerance) {
            a.save(QDir(outDir).filePath(name + QStringLiteral("_cpu.png")));
            b.save(QDir(outDir).filePath(name + QStringLiteral("_gpu.png")));
        }
    };

    compare(QStringLiteral("plain"), baseSnap(main), kUsPerSecond, 1.5);
    {
        // An adjustment layer: its effect (black and white, at 60%) on everything below it, the same on the GPU.
        ProjectSnapshot s = withBackground(baseSnap(main));
        const int at = ops::addTrack(s.timeline, TrackType::Video, 0);
        const int id = ops::insertGenerated(s.timeline, *findGeneratorPreset(QStringLiteral("adjustment")), 0, at, fps);
        Clip* adj = s.timeline.findClip(id);
        check(adj && isAdjustment(*adj), QStringLiteral("Insert > Adjustment Layer makes an adjustment event"));
        if (adj) {
            adj->effects.push_back(makeEffect(*findEffect(QStringLiteral("black_white")), s.timeline.allocId()));
            adj->gain = 0.6f;
            adj->length = 3 * kUsPerSecond;
        }
        compare(QStringLiteral("adjustment_layer"), s, kUsPerSecond, 1.5);
        const QImage plainCpu = cpu.render(withBackground(baseSnap(main)), kUsPerSecond, size), adjusted = cpu.render(s, kUsPerSecond, size);
        check(imageDifference(plainCpu, adjusted) > 2.0, QStringLiteral("the adjustment layer changes the picture below it (mean difference %1)")
                                                            .arg(imageDifference(plainCpu, adjusted), 0, 'f', 2));
        // The clip on top covers the frame: the compositors start there. Not while it fades.
        ProjectSnapshot cover = withBackground(baseSnap(main));
        const int top = coveringTrack(cover, kUsPerSecond);
        cover.timeline.tracks[0].clips[0].fadeIn = 2 * kUsPerSecond;
        check(top == 0 && coveringTrack(cover, kUsPerSecond) == 1, QStringLiteral("the opaque clip on top hides the tracks below (%1); fading, it doesn't")
                                                                      .arg(top));
    }
    {
        // A track matte: the title on top isn't shown; the clip below it shows only through its letters, over the
        // background (inverted, so it differs from the clip everywhere). The same on the GPU, and the other way round
        // with an inverted luma matte.
        auto inverted = [&] {
            ProjectSnapshot s = withBackground(baseSnap(main));
            s.timeline.tracks[1].clips[0].effects.push_back(makeEffect(*findEffect(QStringLiteral("invert")), s.timeline.allocId()));
            return s;
        };
        ProjectSnapshot s = inverted();
        const int at = ops::addTrack(s.timeline, TrackType::Video, 0);
        ops::insertGenerated(s.timeline, *findGeneratorPreset(QStringLiteral("title")), 0, at, fps);
        s.timeline.tracks[size_t(at)].matte = MatteAlpha;
        compare(QStringLiteral("track_matte"), s, kUsPerSecond, 1.5);
        ProjectSnapshot bgOnly = inverted();
        bgOnly.timeline.tracks[0].muted = true;
        const QImage matted = cpu.render(s, kUsPerSecond, size), plain = cpu.render(inverted(), kUsPerSecond, size),
                     bg = cpu.render(bgOnly, kUsPerSecond, size);
        const bool corner = matted.pixel(8, 8) == bg.pixel(8, 8) && plain.pixel(8, 8) != bg.pixel(8, 8);
        check(corner && imageDifference(matted, bg) > 1.0 && imageDifference(matted, plain) > 2.0,
              QStringLiteral("an alpha track matte: the background around the letters, the clip in them (%1 from the background, %2 from the clip)")
                  .arg(imageDifference(matted, bg), 0, 'f', 2)
                  .arg(imageDifference(matted, plain), 0, 'f', 2));
        s.timeline.tracks[size_t(at)].matte = MatteLumaInverted;
        compare(QStringLiteral("track_matte_luma_inverted"), s, kUsPerSecond, 1.5);
    }
    {
        // Color Match, the same on the GPU; Match Colour brings a darkened, tinted copy of a shot back to it.
        ProjectSnapshot s = baseSnap(main);
        Clip& a = s.timeline.tracks[0].clips[0];
        a.length = 3 * kUsPerSecond;
        Clip b = a;
        b.id = s.timeline.allocId();
        b.groupId = 0;
        b.start = 3 * kUsPerSecond;
        EffectInstance dim = makeEffect(*findEffect(QStringLiteral("brightness_contrast")), s.timeline.allocId());
        dim.params[QStringLiteral("brightness")].value = -0.2;
        dim.params[QStringLiteral("contrast")].value = -0.3;
        EffectInstance tint = makeEffect(*findEffect(QStringLiteral("tint")), s.timeline.allocId());
        tint.params[QStringLiteral("color")].value = double(0x3060ff);
        tint.params[QStringLiteral("amount")].value = 0.35;
        b.effects = {dim, tint};
        s.timeline.tracks[0].clips.push_back(b);
        Timeline tl = s.timeline;
        QString why;
        const bool refused = colormatch::match(tl, s, {b.id}, 4 * kUsPerSecond, &why) == 0;
        const int n = colormatch::match(tl, s, {b.id}, kUsPerSecond, &why);
        ProjectSnapshot fixed = s;
        fixed.timeline = tl;
        compare(QStringLiteral("color_match"), fixed, 4 * kUsPerSecond, 1.5);
        const QImage ref = cpu.render(s, kUsPerSecond, size), off = cpu.render(s, 4 * kUsPerSecond, size), on = cpu.render(fixed, 4 * kUsPerSecond, size);
        const double before = imageDifference(off, ref), after = imageDifference(on, ref);
        {
            // Auto Colour: a dull, blue picture (greys from 40 to 190, blue raised) comes out stretched to black and white
            // with its greys grey; the White Balance effect's curves show it (and it is a LUT: the GPU's the same).
            QImage dull(64, 64, QImage::Format_ARGB32_Premultiplied);
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) {
                    const int v = 40 + (x + y) * 150 / 126;
                    dull.setPixel(x, y, qRgb(int(v * 0.85), int(v * 0.92), std::min(255, v + 25)));
                }
            EffectInstance wb = makeEffect(*findEffect(QStringLiteral("white_balance")), 1);
            colormatch::autoColour(dull, wb);
            Params wp;
            for (const auto& [id, pv] : wb.params)
                wp.numbers[id] = pv.value;
            ChannelLuts l;
            const bool changes = findEffect(QStringLiteral("white_balance"))->luts(wp, l);
            auto out = [&](QRgb c) { return std::array<int, 3>{l[0][size_t(qRed(c))], l[1][size_t(qGreen(c))], l[2][size_t(qBlue(c))]}; };
            const auto mid = out(dull.pixel(32, 31)), low = out(dull.pixel(0, 0)), high = out(dull.pixel(63, 63));
            const int spread = std::max({mid[0], mid[1], mid[2]}) - std::min({mid[0], mid[1], mid[2]});
            check(changes && spread <= 6 && std::max({low[0], low[1], low[2]}) < 20 && std::min({high[0], high[1], high[2]}) > 235,
                  QStringLiteral("Auto Colour: a dull blue picture's middle grey comes out grey (%1, %2, %3), its ends near black and white")
                      .arg(mid[0]).arg(mid[1]).arg(mid[2]));
        }
        {
            // Corner Pin: the picture's corners put on the top left quarter's: it fills that quarter, nothing outside it.
            QImage pic(200, 100, QImage::Format_ARGB32_Premultiplied);
            pic.fill(QColor(220, 30, 30));
            Params pp;
            for (const ParamDef& d : findEffect(QStringLiteral("corner_pin"))->params)
                pp.numbers[d.id] = d.def;
            pp.numbers[QStringLiteral("trX")] = 0.5;
            pp.numbers[QStringLiteral("brX")] = 0.5;
            pp.numbers[QStringLiteral("brY")] = 0.5;
            pp.numbers[QStringLiteral("blY")] = 0.5;
            RenderContext rc;
            rc.contentRect = QRectF(pic.rect());
            std::unique_ptr<EffectState> state;
            QImage pinned = pic;
            findEffect(QStringLiteral("corner_pin"))->apply(pinned, pp, rc, state);
            check(qAlpha(pinned.pixel(150, 75)) == 0 && qAlpha(pinned.pixel(150, 20)) == 0 && pinned.pixel(50, 25) == pic.pixel(50, 25),
                  QStringLiteral("Corner Pin puts the picture's corners where they are set (a quarter of the frame, nothing outside it)"));
        }
        check(refused && n == 1 && after < before * 0.4, QStringLiteral("Match Colour brings a darkened, tinted shot back to the reference "
                                                                         "(mean difference %1 -> %2); the cursor in it is refused")
                                                             .arg(before, 0, 'f', 1)
                                                             .arg(after, 0, 'f', 1));
    }
    {
        // Noise reduction (with the frames either side), the same on the GPU.
        ProjectSnapshot s = baseSnap(main);
        s.timeline.tracks[0].clips[0].denoise = 1.0;
        compare(QStringLiteral("denoise"), s, kUsPerSecond, 2.0);
        const double changed = imageDifference(cpu.render(baseSnap(main), kUsPerSecond, size), cpu.render(s, kUsPerSecond, size));
        check(changed > 0.1 && changed < 6.0, QStringLiteral("noise reduction smooths the picture a little (mean difference %1)").arg(changed, 0, 'f', 2));
    }

    // Every effect (tolerances cover sampling differences of blurs/pixelate).
    const std::map<QString, double> effectTolerance = {
        {QStringLiteral("gaussian_blur"), 3.0}, {QStringLiteral("sharpen"), 4.0}, {QStringLiteral("glow"), 4.0},
        {QStringLiteral("pixelate"), 6.0},      {QStringLiteral("noise"), 3.0},   {QStringLiteral("transform"), 4.0}};
    for (const EffectDef& def : videoEffects()) {
        ProjectSnapshot s = withBackground(baseSnap(main));
        EffectInstance e = makeEffect(def, 500);
        for (const auto& [id, v] : testValues(def.id))
            e.params[id].value = v;
        applyTestText(e);
        s.timeline.tracks[0].clips[0].effects.push_back(e);
        auto it = effectTolerance.find(def.id);
        const int fallbacks = gpu->cpuFallbacks();
        compare(QStringLiteral("effect_") + def.id, s, kUsPerSecond, it == effectTolerance.end() ? 2.0 : it->second);
        // Everything runs on the GPU except effects that are inherently sequential or external.
        const bool cpuOnly = def.id == QLatin1String("ffmpeg_filter");
        if (!cpuOnly && gpu->cpuFallbacks() != fallbacks)
            check(false, QStringLiteral("effect %1 fell back to the CPU on the GPU renderer").arg(def.id));
    }

    // Remove Background on a real person (MVP_TEST_PORTRAIT), each background mode, and its speed at 1080p.
    if (const QString portrait = qEnvironmentVariable("MVP_TEST_PORTRAIT"); !portrait.isEmpty()) {
        if (auto photo = loadItem(portrait, 900)) {
            for (int mode = 0; mode < 4; ++mode) {
                ProjectSnapshot s = withBackground(baseSnap(photo));
                EffectInstance e = makeEffect(*findEffect(QStringLiteral("remove_bg")), s.timeline.allocId());
                e.params[QStringLiteral("mode")].value = mode;
                e.params[QStringLiteral("steady")].value = 0;
                s.timeline.tracks[0].clips[0].effects.push_back(e);
                compare(QStringLiteral("remove_bg_photo_mode%1").arg(mode), s, kUsPerSecond / 2, 3.0);
                if (mode == 0) { // and the Quality model
                    s.timeline.tracks[0].clips[0].effects.back().params[QStringLiteral("model")].value = 1;
                    compare(QStringLiteral("remove_bg_photo_quality"), s, kUsPerSecond / 2, 3.0);
                    s.timeline.tracks[0].clips[0].effects.back().params[QStringLiteral("model")].value = 0;
                }
                if (mode == 1) {
                    const QSize hd(1920, 1080);
                    for (FrameRenderer* r : {static_cast<FrameRenderer*>(&cpu), static_cast<FrameRenderer*>(gpu.get())}) {
                        r->render(s, kUsPerSecond / 2, hd);
                        QElapsedTimer t;
                        t.start();
                        for (int i = 0; i < 10; ++i)
                            r->render(s, kUsPerSecond / 2 + i * 1000, hd);
                        print(QStringLiteral("INFO  Remove Background (blurred) over a background at 1080p, %1: %2 ms/frame")
                                  .arg(r->isGpu() ? QStringLiteral("GPU") : QStringLiteral("CPU")).arg(t.nsecsElapsed() / 1e7, 0, 'f', 1));
                    }
                }
            }
        }
    }

    // Mode variants and time-based effects over a run of frames: GPU only, same result as the CPU.
    {
        struct Case {
            const char* id;
            std::vector<std::pair<const char*, double>> values;
            int frames;
            double tolerance;
        };
        const std::vector<Case> cases = {
            {"trails", {}, 6, 2.0}, {"trails", {{"mode", 1}}, 6, 2.0}, {"trails", {{"mode", 2}}, 6, 2.0}, {"trails", {{"mode", 3}}, 6, 2.0},
            {"video_feedback", {}, 6, 2.5}, {"video_feedback", {{"mode", 0}}, 6, 2.5}, {"datamosh", {}, 6, 2.0},
            {"datamosh", {{"mode", 1}}, 6, 2.0}, {"slit_scan", {{"frames", 6}}, 8, 2.0}, {"slit_scan", {{"direction", 4}, {"frames", 5}}, 8, 2.0},
            {"time_split", {{"delay", 2}}, 6, 2.0}, {"interlace", {}, 4, 2.0}, {"stutter", {{"fps", 2.5}}, 6, 2.0},
            {"databend", {}, 1, 2.0}, {"databend", {{"delay", 1}, {"feedback", 0.9}}, 1, 2.0}, {"databend", {{"mode", 1}}, 1, 2.0},
            // Error diffusion is chaotic (a one-level decoding difference re-routes the dot
            // pattern), so on video compare the average colour (1/8 scale); exactness is
            // checked on identical input below. Negative tolerance = compare at 1/8 scale.
            {"dither", {{"method", 3}}, 1, -10.0} /* Floyd-Steinberg worms on flat colour */, {"dither", {{"method", 4}, {"palette", 5}, {"pixelSize", 1}}, 1, -3.0}, {"databend", {{"mode", 2}}, 1, 5.0} /* bit rotation amplifies 1-level decode differences */, {"databend", {{"mode", 3}}, 1, 2.0},
            {"jpeg_glitch", {{"corruption", 0.3}}, 1, 2.5}, {"jpeg_glitch", {{"subsample", 0}, {"quality", 30}}, 1, 2.5},
            {"dither", {{"method", 0}}, 1, 2.0}, {"dither", {{"method", 1}, {"palette", 4}}, 1, 2.0}, {"halftone", {{"mode", 1}}, 1, 2.0},
            {"halftone", {{"mode", 2}}, 1, 2.0}, {"neon_edges", {{"colorMode", 0}}, 1, 2.5}, {"neon_edges", {{"colorMode", 2}, {"glow", 0}}, 1, 2.0},
            {"strobe", {{"rate", 1}, {"duty", 0.9}}, 1, 2.0}, {"strobe", {{"rate", 1}, {"duty", 0.9}, {"mode", 1}}, 1, 2.0},
            {"ascii", {{"colorMode", 1}, {"charset", 2}}, 1, 2.0}, {"emboss", {{"keepColor", 1}}, 1, 2.0}, {"gradient_map", {{"cycle", 0.5}}, 1, 2.0},
            {"tv_static", {{"colorNoise", 1}, {"signalLoss", 0.6}}, 1, 2.0}, {"vhs_tracking", {{"position", 0.5}, {"height", 0.6}}, 1, 2.0},
            {"pixel_stretch", {{"direction", 2}}, 1, 2.0}, {"vhs", {{"bleed", 20}, {"jitter", 8}}, 1, 2.0}, {"ntsc", {{"bleed", 15}}, 1, 2.0},
            {"block_glitch", {{"amount", 0.7}}, 1, 2.0}, {"slice_glitch", {{"amount", 0.9}}, 1, 2.0},
            // Pixel sort amplifies the tiny differences between CPU and GPU video decoding (a
            // one-level change moves run edges and reorders pixels); it is checked exactly on a still below.
            {"pixel_sort", {{"direction", 1}}, 1, 4.0}, {"pixel_sort", {{"sortBy", 1}, {"reverse", 1}}, 1, 4.0},
            {"pixel_sort", {{"sortBy", 2}, {"maxLength", 60}}, 1, 4.0}, {"pixel_sort", {{"invert", 1}, {"sortBy", 3}}, 1, 4.0},
            {"pixel_sort", {{"lower", 0.0}, {"upper", 1.0}, {"direction", 1}, {"sortBy", 5}}, 1, 4.0},
            // Glitch options (CPU and GPU must agree on every one).
            {"chromatic_aberration", {{"centerX", 0.3}, {"falloff", 2.0}, {"greenShift", 0.5}, {"wobble", 0.5}}, 1, 2.0},
            {"chromatic_aberration", {{"mode", 1}, {"greenShift", -0.5}, {"amount", 25}}, 1, 2.0},
            {"rgb_split", {{"jitter", 30}, {"edge", 1}}, 1, 2.0}, {"rgb_split", {{"edge", 2}, {"redX", -80}}, 1, 2.0},
            {"rgb_split", {{"edge", 3}, {"blueY", 60}}, 1, 2.0},
            {"pixel_sort", {{"thresholdBy", 1}, {"breaks", 0.02}}, 1, 4.0},
            {"pixel_sort", {{"thresholdBy", 2}, {"breaks", 0.05}, {"animated", 1}, {"direction", 1}}, 1, 4.0},
            {"datamosh", {{"motionScale", 2.5}}, 6, 2.0}, {"datamosh", {{"autoClean", 0.1}}, 6, 2.0},
            {"block_glitch", {{"amount", 0.7}, {"aspect", 0.5}, {"verticalShift", 1}, {"colorGlitch", 0.8}, {"colorMode", 3}, {"noise", 0.3}}, 1, 2.0},
            {"slice_glitch", {{"amount", 0.9}, {"direction", 1}, {"edge", 2}, {"colorGlitch", 0.7}}, 1, 2.0},
            {"slice_glitch", {{"amount", 0.9}, {"edge", 1}, {"rgbSplit", 0}}, 1, 2.0},
            {"jpeg_glitch", {{"chromaQuality", 3}, {"blockScale", 1}, {"corruption", 0.3}, {"corruptWhat", 1}}, 1, 2.5},
            {"jpeg_glitch", {{"corruption", 0.5}, {"corruptWhat", 2}, {"subsample", 0}}, 1, 2.5},
            {"databend", {{"mode", 4}, {"delay", 5001}}, 1, 2.0}, {"databend", {{"mode", 5}, {"stride", -17}}, 1, 2.0},
            {"databend", {{"mode", 6}, {"feedback", 0.6}}, 1, 2.0},
            {"bitcrush", {{"fold", 1}, {"offset", 0.2}, {"separate", 1}, {"bitsB", 1}}, 1, 2.0},
            {"pixel_stretch", {{"length", 0.4}, {"sweep", 0.6}, {"jitter", 80}}, 1, 2.0},
            {"pixel_stretch", {{"direction", 3}, {"jitter", 40}, {"length", 0.7}}, 1, 2.0},
            {"channel_swap", {{"order", 5}, {"invertG", 1}}, 1, 2.0},
            {"time_split", {{"delay", 2}, {"order", 4}, {"curve", 2}}, 9, 2.0},
            {"stutter", {{"fps", 10}, {"freeze", 0.6}, {"maxFreeze", 4}}, 8, 2.0},
            {"strobe", {{"rate", 1}, {"duty", 0.9}, {"mode", 5}, {"opacity", 0.5}}, 1, 2.0},
            {"strobe", {{"rate", 1}, {"duty", 0.9}, {"mode", 6}}, 1, 2.0}, {"strobe", {{"rate", 1}, {"duty", 0.9}, {"mode", 4}}, 1, 2.0},
            // Shared Mix / Apply to.
            {"block_glitch", {{"amount", 0.8}, {"fxMask", 1}, {"fxFeather", 0.1}}, 1, 2.0},
            {"rgb_split", {{"fxMask", 3}, {"fxThreshold", 0.4}, {"fxMix", 0.7}}, 1, 2.0},
            {"pixel_sort", {{"fxMask", 2}, {"fxFeather", 0}}, 1, 4.0}, {"time_split", {{"fxMask", 4}, {"delay", 2}}, 6, 2.0},
            {"bitcrush", {{"fxMix", 0.5}}, 1, 2.0},
        };
        int id = 700;
        for (const Case& c : cases) {
            ProjectSnapshot s = withBackground(baseSnap(main));
            EffectInstance e = makeEffect(*findEffect(QString::fromLatin1(c.id)), ++id); // fresh state per case
            QString name = QString::fromLatin1(c.id);
            for (const auto& [k, v] : c.values) {
                e.params[QString::fromLatin1(k)].value = v;
                name += QStringLiteral(" %1=%2").arg(QString::fromLatin1(k)).arg(v);
            }
            s.timeline.tracks[0].clips[0].effects.push_back(e);
            const int fallbacks = gpu->cpuFallbacks();
            const int last = fps.frameAt(kUsPerSecond);
            QImage a, b;
            for (int f = last - c.frames + 1; f <= last; ++f) {
                a = cpu.render(s, fps.timeOf(f), size);
                b = gpu->render(s, fps.timeOf(f), size);
            }
            const bool coarse = c.tolerance < 0;
            const double d = coarse ? imageDifference(a.scaled(size / 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
                                                      b.scaled(size / 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation))
                                    : imageDifference(a, b);
            const double tolerance = std::abs(c.tolerance);
            const bool onGpu = gpu->cpuFallbacks() == fallbacks;
            check(d <= tolerance && onGpu, QStringLiteral("GPU matches CPU over %1 frame(s): %2  (mean difference %3%4)")
                                                 .arg(c.frames).arg(name).arg(d, 0, 'f', 2).arg(onGpu ? QString() : QStringLiteral(", CPU fallback!")));
            if (d > tolerance) {
                a.save(QDir(outDir).filePath(QStringLiteral("seq_%1_%2_cpu.png").arg(QString::fromLatin1(c.id)).arg(id)));
                b.save(QDir(outDir).filePath(QStringLiteral("seq_%1_%2_gpu.png").arg(QString::fromLatin1(c.id)).arg(id)));
            }
        }
    }

    // Sequential effects (pixel sort, error-diffusion dither, databend echo) on
    // generated pictures, which both renderers draw identically: the GPU
    // versions must reproduce the CPU exactly.
    {
        struct Variant {
            const char* effect;
            std::vector<std::pair<const char*, double>> values;
        };
        const std::vector<Variant> variants = {
            {"pixel_sort", {}}, {"pixel_sort", {{"direction", 1}}}, {"pixel_sort", {{"sortBy", 1}, {"reverse", 1}}},
            {"pixel_sort", {{"sortBy", 2}, {"maxLength", 60}}}, {"pixel_sort", {{"invert", 1}, {"sortBy", 3}}},
            {"pixel_sort", {{"lower", 0.0}, {"upper", 1.0}, {"direction", 1}, {"sortBy", 5}}},
            {"dither", {{"method", 3}}}, {"dither", {{"method", 4}}}, {"dither", {{"method", 3}, {"palette", 0}, {"pixelSize", 1}}},
            {"dither", {{"method", 4}, {"palette", 5}, {"pixelSize", 7}}},
            {"databend", {}}, {"databend", {{"delay", 1}, {"feedback", 0.9}}}, {"databend", {{"delay", 50000}, {"feedback", 0.3}, {"mix", 0.6}}}};
        double worst = 0, base = 0;
        QString worstName;
        int id = 800;
        for (const char* preset : {"gradient", "vignette_gradient", "checkerboard"})
            for (const auto& v : variants) {
                ProjectSnapshot s;
                s.settings.width = 1920;
                s.settings.height = 1080;
                s.settings.fps = fps;
                s.timeline = defaultTimeline();
                const int clip = ops::insertGenerated(s.timeline, *findGeneratorPreset(QString::fromLatin1(preset)), 0, 0, fps);
                base = std::max(base, imageDifference(cpu.render(s, kUsPerSecond, size), gpu->render(s, kUsPerSecond, size)));
                EffectInstance e = makeEffect(*findEffect(QString::fromLatin1(v.effect)), ++id);
                for (const auto& [k, val] : v.values)
                    e.params[QString::fromLatin1(k)].value = val;
                s.timeline.findClip(clip)->effects.push_back(e);
                const QImage a = cpu.render(s, kUsPerSecond, size), b = gpu->render(s, kUsPerSecond, size);
                const double d = imageDifference(a, b);
                if (d > worst) {
                    worst = d;
                    worstName = QStringLiteral("%1 on %2").arg(QString::fromLatin1(v.effect), QString::fromLatin1(preset));
                    a.save(QDir(outDir).filePath(QStringLiteral("pixelsort_exact_cpu.png")));
                    b.save(QDir(outDir).filePath(QStringLiteral("pixelsort_exact_gpu.png")));
                }
            }
        // Every dithering method on a spread of palettes (list, levels, adaptive, perceptual,
        // animated noise): error diffusion and threshold maps exactly, the rest within rounding.
        {
            double ditherWorst = 0;
            QString ditherWorstName;
            int compared = 0;
            for (int m = 0; m < int(dither::methods().size()); ++m) {
                if (dither::methods()[size_t(m)].kind == dither::Kind::Riemersma)
                    continue;
                for (int pi : {0, 1, 3, 14, 27, 36, 37, 41, 50})
                    for (int variant = 0; variant < 2; ++variant) {
                        ProjectSnapshot s;
                        s.settings.width = 1920;
                        s.settings.height = 1080;
                        s.settings.fps = fps;
                        s.timeline = defaultTimeline();
                        const int clip = ops::insertGenerated(s.timeline, *findGeneratorPreset(QStringLiteral("vignette_gradient")), 0, 0, fps);
                        EffectInstance e = makeEffect(*findEffect(QStringLiteral("dither")), ++id);
                        e.params[QStringLiteral("method")].value = m;
                        e.params[QStringLiteral("palette")].value = pi;
                        e.params[QStringLiteral("pixelSize")].value = variant ? 1 : 4;
                        e.params[QStringLiteral("strength")].value = variant ? 0.7 : 1.0;
                        e.params[QStringLiteral("match")].value = variant;
                        e.params[QStringLiteral("animate")].value = variant;
                        s.timeline.findClip(clip)->effects.push_back(e);
                        const QImage a = cpu.render(s, kUsPerSecond, size), b = gpu->render(s, kUsPerSecond, size);
                        const double d = imageDifference(a, b);
                        ++compared;
                        if (d > ditherWorst) {
                            ditherWorst = d;
                            ditherWorstName = QStringLiteral("%1, %2%3").arg(QString::fromUtf8(dither::methods()[size_t(m)].name),
                                                                            QString::fromUtf8(dither::palettes()[size_t(pi)].name),
                                                                            variant ? QStringLiteral(" (1 px, 70%, OKLab, animated)") : QString());
                            a.save(QDir(outDir).filePath(QStringLiteral("dither_worst_cpu.png")));
                            b.save(QDir(outDir).filePath(QStringLiteral("dither_worst_gpu.png")));
                        }
                    }
            }
            check(ditherWorst <= base + 0.5, QStringLiteral("GPU dither: %1 methods and palettes match the CPU (worst difference %2: %3)")
                                                 .arg(compared).arg(ditherWorst, 0, 'f', 3).arg(ditherWorstName));
        }
        check(worst <= base + 0.05, QStringLiteral("GPU pixel sort, error-diffusion dither and databend echo are exact on identical input "
                                                   "(39 cases, worst difference %1 %2, without the effects %3)")
                                        .arg(worst, 0, 'f', 3).arg(worstName).arg(base, 0, 'f', 3));
    }
    // Speed of the sequential effects at 1080p (the frame changes each render, so nothing is cached).
    for (const auto& [effect, label, values] : std::vector<std::tuple<const char*, const char*, std::vector<std::pair<const char*, double>>>>{
             {"pixel_sort", "pixel sort", {}},
             {"dither", "Floyd-Steinberg dither (3 px)", {{"method", 3}}},
             {"dither", "Floyd-Steinberg dither (1 px)", {{"method", 3}, {"pixelSize", 1}}},
             {"dither", "Jarvis dither (1 px)", {{"method", 19}, {"pixelSize", 1}}},
             {"dither", "Pattern (Knoll) dither (3 px, PICO-8)", {{"method", 17}, {"palette", 5}}},
             {"dither", "blue noise dither (1 px, OKLab, 32 colours)", {{"method", 14}, {"palette", 41}, {"pixelSize", 1}, {"match", 1}}},
             {"dither", "Riemersma dither (3 px)", {{"method", 27}}},
             {"databend", "databend echo", {}}}) {
        ProjectSnapshot s = baseSnap(main);
        EffectInstance e = makeEffect(*findEffect(QString::fromLatin1(effect)), 900);
        for (const auto& [k, v] : values)
            e.params[QString::fromLatin1(k)].value = v;
        s.timeline.tracks[0].clips[0].effects.push_back(e);
        const QSize hd(1920, 1080);
        auto timeIt = [&](FrameRenderer& r) {
            r.render(s, 0, hd);
            QElapsedTimer timer;
            timer.start();
            for (int i = 1; i <= 10; ++i)
                r.render(s, fps.timeOf(i), hd);
            return timer.elapsed() / 10.0;
        };
        const double cpuMs = timeIt(cpu), gpuMs = timeIt(*gpu);
        print(QStringLiteral("INFO  %1 at 1080p incl. decoding: CPU %2 ms/frame, GPU %3 ms/frame").arg(QString::fromLatin1(label)).arg(cpuMs, 0, 'f', 1).arg(gpuMs, 0, 'f', 1));
    }

    // Every transition at 40%, between the main clip and an inverted copy.
    for (const TransitionDef& def : transitions()) {
        ProjectSnapshot s = baseSnap(main);
        Clip& a = s.timeline.tracks[0].clips[0];
        a.length = 3 * kUsPerSecond;
        Clip b = a;
        b.id = s.timeline.allocId();
        b.start = 2 * kUsPerSecond;
        b.effects.push_back(makeEffect(*findEffect(QStringLiteral("invert")), s.timeline.allocId()));
        b.transitionIn = makeTransition(def);
        s.timeline.tracks[0].clips.push_back(b);
        compare(QStringLiteral("transition_") + def.id, s, 2 * kUsPerSecond + 4 * kUsPerSecond / 10, 3.0);
    }

    // Framing, speed, generators, track/output FX.
    {
        ProjectSnapshot s = withBackground(baseSnap(main));
        Clip& c = s.timeline.tracks[0].clips[0];
        c.panCrop[QStringLiteral("zoom")].value = 0.6;
        c.panCrop[QStringLiteral("rotation")].value = 15;
        c.panCrop[QStringLiteral("centerX")].value = 0.4;
        compare(QStringLiteral("pancrop_rotate"), s, kUsPerSecond, 3.0);
    }
    {
        ProjectSnapshot s = baseSnap(main);
        s.timeline.tracks[0].clips[0].panCrop[QStringLiteral("mode")].value = 1;
        s.timeline.tracks[0].clips[0].panCrop[QStringLiteral("zoom")].value = 2.5;
        compare(QStringLiteral("pancrop_fill_zoom"), s, kUsPerSecond, 2.5);
    }
    {
        // A freehand mask: the GPU reads the shape the CPU drew.
        ProjectSnapshot s = baseSnap(main);
        EffectInstance inv = makeEffect(*findEffect(QStringLiteral("invert")), s.timeline.allocId());
        inv.params[QStringLiteral("mask.shape")].value = 3;
        inv.params[QStringLiteral("mask.feather")].value = 4;
        inv.params[QStringLiteral("mask.p0y")].value = -0.9; // a spike up
        s.timeline.tracks[0].clips[0].effects.push_back(inv);
        compare(QStringLiteral("mask_freehand"), s, kUsPerSecond, 2.0);
    }
    {
        ProjectSnapshot s = baseSnap(main);
        Clip& c = s.timeline.tracks[0].clips[0];
        ops::setClipSpeed(c, main.get(), 0.25, true, fps);
        c.frameBlend = true;
        c.reverse = true;
        compare(QStringLiteral("slowmo_blend_reverse"), s, fps.timeOf(122), 2.0);
    }
    {
        ProjectSnapshot s = baseSnap(main);
        Clip& c = s.timeline.tracks[0].clips[0];
        ops::setClipSpeed(c, main.get(), 0.25, true, fps);
        c.opticalFlow = true;
        compare(QStringLiteral("slowmo_optical_flow"), s, fps.timeOf(122), 3.0);
    }
    {
        ProjectSnapshot s = withBackground(baseSnap(main));
        s.timeline.tracks[0].clips.clear();
        ops::insertGenerated(s.timeline, *findGeneratorPreset(QStringLiteral("lower_third")), 0, 0, fps);
        compare(QStringLiteral("generator_lower_third"), s, 2 * kUsPerSecond, 1.5);
    }
    // The animated backgrounds and overlays the GPU draws itself (shaders for plasma and aurora, its painter for the
    // rest, twice as big where edges must come out smooth).
    for (const char* id : {"plasma_neon", "plasma_fire", "aurora", "bokeh_warm", "digital_rain", "confetti", "progress_bar", "letterbox",
                           "shape_heart", "kinetic_text", "countdown", "hyperspace", "retro_grid", "bars_smpte"}) {
        ProjectSnapshot s = withBackground(baseSnap(main));
        s.timeline.tracks[0].clips.clear();
        ops::insertGenerated(s.timeline, *findGeneratorPreset(QLatin1String(id)), 0, 0, fps);
        compare(QStringLiteral("generator_") + QLatin1String(id), s, 2 * kUsPerSecond + 400000, 3.0);
    }
    // Animation scenes: motion blur (the scene's and a layer's), soft layers (blur, depth of field), a layer's own effects
    // and groups seen through as one, all painted on the GPU (no picture of theirs made on the CPU).
    {
        const char* bg = R"({"type": "rect", "id": "bg", "x": 0, "y": 0, "width": 1, "height": 1, "fill": "#204060"})";
        const struct {
            const char* name;
            QByteArray scene;
        } scenes[] = {
            {"scene_motion_blur", QByteArray(R"({"version": 2, "motion_blur": {"samples": 6, "shutter": 0.1}, "layers": [)") + bg
                                      + R"(, {"type": "ellipse", "id": "ball", "x": {"keyframes": [[0, 0], [2, 0.8]]}, "y": 0.3, "width": 0.2,
                                             "height": 0.35, "fill": "#ffd60a"}]})"},
            {"scene_layer_motion_blur", QByteArray(R"({"version": 2, "layers": [)") + bg
                                            + R"(, {"type": "rect", "id": "box", "x": {"keyframes": [[0, 0.1], [2, 0.7]]}, "y": 0.4, "width": 0.2,
                                                   "height": 0.2, "fill": "#ff006e", "motion_blur": {"samples": 5, "shutter": 0.12}}]})"},
            {"scene_soft_layers", QByteArray(R"({"version": 2, "camera": {"x": 0.5, "y": 0.5, "zoom": 1.5, "depth_of_field": 3}, "layers": [)") + bg
                                      + R"(, {"type": "rect", "id": "far", "x": 0.15, "y": 0.2, "width": 0.3, "height": 0.4, "fill": "#ff006e", "depth": 1},
                                             {"type": "ellipse", "id": "soft", "x": 0.55, "y": 0.3, "width": 0.25, "height": 0.4, "fill": "#06d6a0",
                                              "blur": 0.01}]})"},
            {"scene_layer_effects", QByteArray(R"({"version": 2, "layers": [)") + bg
                                        + R"(, {"type": "rect", "id": "tv", "x": 0.2, "y": 0.2, "width": 0.5, "height": 0.5, "fill": "#06d6a0",
                                               "effects": [{"type": "vignette", "params": {"amount": 0.8}}, {"type": "crt"}]}]})"},
            {"scene_groups", QByteArray(R"({"version": 2, "layers": [)") + bg
                                 + R"(, {"type": "group", "id": "g", "x": 0, "y": 0, "width": 1, "height": 1, "opacity": 0.5},
                                        {"type": "rect", "id": "a", "parent": "g", "x": 0.1, "y": 0.2, "width": 0.4, "height": 0.4, "fill": "#ffd60a"},
                                        {"type": "rect", "id": "b", "parent": "g", "x": 0.3, "y": 0.3, "width": 0.4, "height": 0.4, "fill": "#ff006e"},
                                        {"type": "group", "id": "m", "x": 0, "y": 0, "width": 1, "height": 1, "blend": "multiply"},
                                        {"type": "ellipse", "id": "c", "parent": "m", "x": 0.55, "y": 0.1, "width": 0.3, "height": 0.5,
                                         "fill": "#8ecae6"}]})"},
        };
        for (const auto& sc : scenes) {
            QJsonParseError parsed;
            const QJsonObject scene = QJsonDocument::fromJson(sc.scene, &parsed).object();
            ProjectSnapshot s = withBackground(baseSnap(main));
            s.timeline.tracks[0].clips.clear();
            const int id = ops::insertGenerated(s.timeline, *findGeneratorPreset(QStringLiteral("animation_blank")), 0, 0, fps);
            if (Clip* c = s.timeline.findClip(id))
                c->generator.data = scene;
            const QString name = QString::fromLatin1(sc.name);
            check(parsed.error == QJsonParseError::NoError && motion::validate(scene).isEmpty(),
                  QStringLiteral("%1 is a valid scene %2").arg(name, motion::validate(scene)));
            const int before = motion::cpuPictures();
            compare(name, s, kUsPerSecond, 2.0);
            check(motion::cpuPictures() == before, QStringLiteral("%1: its pictures stay on the GPU (%2 made on the CPU)").arg(name).arg(motion::cpuPictures() - before));
        }
    }
    {
        ProjectSnapshot s = withBackground(baseSnap(main));
        s.timeline.tracks[0].clips[0].gain = 0.6f;
        s.timeline.tracks[0].effects.push_back(makeEffect(*findEffect(QStringLiteral("sepia")), 600));
        s.timeline.tracks[0].level = 0.8f;
        s.timeline.outputEffects.push_back(makeEffect(*findEffect(QStringLiteral("vignette")), 601));
        compare(QStringLiteral("track_and_output_fx"), s, kUsPerSecond, 2.0);
    }
    // Different sources: letterboxing, rotation metadata, 10-bit, alpha.
    const struct {
        const char* file;
        const char* name;
        bool background;
    } sources[] = {{"3ds_stereo_mjpeg.avi", "source_3ds_mjpeg", false}, {"phone_rotated.mp4", "source_rotated", false},
                   {"hevc_10bit.mp4", "source_hevc_10bit", false},      {"h264_10bit_422.mkv", "source_h264_422_10bit", false},
                   {"png_alpha.mov", "source_png_alpha_over_video", true}, {"quicktime_cinepak.mov", "source_cinepak", false},
                   {"dv_ntsc.dv", "source_dv_anamorphic", false}};
    for (const auto& src : sources) {
        auto item = loadItem(dir.filePath(QString::fromLatin1(src.file)), 7);
        if (!item) {
            print(QStringLiteral("SKIP  %1 missing").arg(QString::fromLatin1(src.file)));
            continue;
        }
        ProjectSnapshot s = baseSnap(item);
        if (src.background)
            s = withBackground(s);
        compare(QString::fromLatin1(src.name), s, kUsPerSecond, 2.0);
    }
    contactSheet(sheet).save(QDir(outDir).filePath(QStringLiteral("gpu.png")));

    // ---- Hardware decoding gives the same pictures as software decoding ----------
    for (const char* file : {"h264_aac.mp4", "hevc_10bit.mp4", "uhd_hevc10.mp4", "vp9_vorbis.webm", "av1_flac.mkv"}) {
        auto item = loadItem(dir.filePath(QString::fromLatin1(file)), 9);
        if (!item)
            continue;
        VideoDecoder hwDec, swDec;
        hwDec.open(item->path, item->info.video.front().index, nullptr, true);
        swDec.open(item->path, item->info.video.front().index, nullptr, false);
        FrameConverter c1, c2;
        double worst = 0;
        for (TimeUs t : {TimeUs(0), item->info.duration / 2, item->info.duration / 3}) {
            const AVFrame* a = hwDec.frameAt(t);
            const QImage ia = a ? c1.convert(a, QSize(640, 360), true) : QImage();
            const AVFrame* b = swDec.frameAt(t);
            const QImage ib = b ? c2.convert(b, QSize(640, 360), true) : QImage();
            worst = std::max(worst, imageDifference(ia, ib));
        }
        check(worst < 1.5, QStringLiteral("%1 decoding of %2 matches software (difference %3)")
                               .arg(hwDec.usingHardware() ? QStringLiteral("hardware") : QStringLiteral("(software fallback)"),
                                    QString::fromLatin1(file))
                               .arg(worst, 0, 'f', 2));
    }

    // ---- Benchmarks -------------------------------------------------------------------
    auto bench = [&](const QString& label, const ProjectSnapshot& s, QSize outSize, int frames) {
        auto timeIt = [&](FrameRenderer& r, bool hwDecode) {
            hw::settings().hardwareDecoding = hwDecode;
            r.render(s, 0, outSize); // warm up: decoder open, caches
            QElapsedTimer timer;
            timer.start();
            for (int i = 1; i <= frames; ++i)
                r.render(s, fps.timeOf(i), outSize);
            return double(timer.elapsed()) / frames;
        };
        Compositor c(true);
        const double cpuMs = timeIt(c, false);
        auto g = GpuCompositor::create(surface.get(), true, &err);
        const double gpuMs = timeIt(*g, true);
        hw::settings().hardwareDecoding = true;
        print(QStringLiteral("BENCH %1: CPU %2 ms/frame (%3 fps), GPU %4 ms/frame (%5 fps), %6x faster")
                  .arg(label)
                  .arg(cpuMs, 0, 'f', 1)
                  .arg(1000.0 / cpuMs, 0, 'f', 1)
                  .arg(gpuMs, 0, 'f', 1)
                  .arg(1000.0 / gpuMs, 0, 'f', 1)
                  .arg(cpuMs / gpuMs, 0, 'f', 1));
    };
    {
        ProjectSnapshot s = withBackground(baseSnap(main));
        Clip& c = s.timeline.tracks[0].clips[0];
        c.panCrop[QStringLiteral("zoom")].value = 0.7;
        for (const char* fx : {"gaussian_blur", "hsl", "vignette"})
            c.effects.push_back(makeEffect(*findEffect(QString::fromLatin1(fx)), s.timeline.allocId()));
        bench(QStringLiteral("1080p, 2 tracks, PiP + blur + colour + vignette"), s, QSize(1920, 1080), 60);
    }
    {
        ProjectSnapshot s = baseSnap(main);
        s.timeline.tracks[0].clips.clear();
        const int id = ops::insertGenerated(s.timeline, *findGeneratorPreset(QStringLiteral("animation_characters")), 0, 0, fps);
        if (Clip* c = s.timeline.findClip(id)) {
            c->generator.data[QStringLiteral("motion_blur")] = QJsonObject{{QStringLiteral("samples"), 4}, {QStringLiteral("shutter"), 0.025}};
            c->generator.data[QStringLiteral("camera")] = QJsonObject{{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}, {QStringLiteral("zoom"), 1.3},
                                                                       {QStringLiteral("depth_of_field"), 2}};
            // (MVP_BENCH_SCENE: a scene file of one's own instead, such as one from a real project)
            if (QFile f(qEnvironmentVariable("MVP_BENCH_SCENE")); f.open(QIODevice::ReadOnly))
                c->generator.data = QJsonDocument::fromJson(f.readAll()).object();
        }
        bench(QStringLiteral("1080p animation: two characters, motion blur, depth of field"), s, QSize(1920, 1080), 30);
        // The preview (draft quality, frames in a row): the GPU painting each frame, or the CPU's look-ahead.
        for (const bool ahead : {false, true}) {
            if (ahead)
                qputenv("MVP_CPU_LOOKAHEAD", "1");
            auto preview = GpuCompositor::create(surface.get(), false, &err);
            preview->render(s, 0, QSize(960, 540));
            QElapsedTimer timer;
            timer.start();
            for (int i = 1; i <= 48; ++i)
                preview->render(s, fps.timeOf(i), QSize(960, 540));
            print(QStringLiteral("BENCH preview of the animation at 960x540: %1 ms/frame (%2)")
                      .arg(timer.elapsed() / 48.0, 0, 'f', 1)
                      .arg(ahead ? QStringLiteral("CPU look-ahead") : QStringLiteral("GPU painting")));
        }
        qunsetenv("MVP_CPU_LOOKAHEAD");
    }
    // The preview recreates its GPU renderer on its own thread (GPU Acceleration,
    // Hardware Decoding): titles must still draw with each new context.
    {
        ProjectSnapshot s = baseSnap(main);
        ops::insertGenerated(s.timeline, *findGeneratorPreset(QStringLiteral("title")), 0, 0, fps);
        const QImage want = cpu.render(s, kUsPerSecond, size);
        double worst = 0;
        std::thread([&] {
            for (int i = 0; i < 3; ++i) {
                auto g = GpuCompositor::create(surface.get(), false, &err);
                worst = std::max(worst, g ? imageDifference(want, g->render(s, kUsPerSecond, size)) : 255.0);
            }
        }).join();
        check(worst <= 3, QStringLiteral("a title still draws after the GPU renderer is recreated on its thread (mean difference %1)").arg(worst, 0, 'f', 2));
    }
    for (const char* file : {"uhd_h264.mp4", "uhd_hevc10.mp4"}) {
        auto item = loadItem(dir.filePath(QString::fromLatin1(file)), 11);
        if (item)
            bench(QStringLiteral("4K %1 at 3840x2160").arg(QString::fromLatin1(file)), baseSnap(item), QSize(3840, 2160), 60);
    }
    return failures;
}

struct Levels {
    double rmsDb = -200;
    double peakDb = -200;
    bool finite = true;
};

Levels measure(const std::vector<float>& s, size_t from = 0)
{
    Levels l;
    double sum = 0, peak = 0;
    size_t n = 0;
    for (size_t i = from * 2; i < s.size(); ++i, ++n) {
        if (!std::isfinite(s[i]))
            l.finite = false;
        sum += double(s[i]) * s[i];
        peak = std::max(peak, double(std::abs(s[i])));
    }
    if (n) {
        l.rmsDb = 10 * std::log10(std::max(sum / double(n), 1e-20));
        l.peakDb = 20 * std::log10(std::max(peak, 1e-10));
    }
    return l;
}

int cmdAudioCache(const QString& dir)
{
    int failures = 0;
    auto a = loadItem(QDir(dir).filePath("a/speech-00000.wav"), 1);
    auto b = loadItem(QDir(dir).filePath("b/speech-00000.wav"), 1);
    if (!a || !b) return 1;
    auto snapshot = [&](const std::shared_ptr<MediaItem>& item, double speed, bool pitch, bool ramp) {
        ProjectSnapshot s;
        s.media[item->id] = item;
        s.timeline = defaultTimeline();
        ops::insertMedia(s.timeline, *item, 0, -1, FrameRate{30, 1});
        for (auto& t : s.timeline.tracks) for (auto& c : t.clips) {
            c.speed = speed; c.preservePitch = pitch;
            c.velocity.active = ramp; c.velocity.value = 1.25;
        }
        return s;
    };
    for (int mode = 0; mode < 4; ++mode) {
        auto first = snapshot(a, mode ? 1.5 : 1.0, mode == 2, mode == 3);
        auto second = snapshot(b, mode ? 1.5 : 1.0, mode == 2, mode == 3);
        AudioMixer reused(48000), fresh(48000);
        std::vector<float> warm(2048), actual(2048), expected(2048);
        reused.mix(first, 0, 1024, warm.data());
        reused.mix(second, 1024, 1024, actual.data());
        fresh.mix(second, 1024, 1024, expected.data());
        double error = 0, energy = 0;
        for (size_t i = 0; i < actual.size(); ++i) {
            error = std::max(error, double(std::abs(actual[i] - expected[i])));
            energy += expected[i] * expected[i];
        }
        const bool ok = error < 1e-5 && energy > 0.01;
        print(QStringLiteral("%1 audio cache source replacement mode %2 (error %3)")
              .arg(ok ? "PASS" : "FAIL").arg(mode).arg(error));
        if (!ok) ++failures;
    }
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1 %2").arg(ok ? "PASS" : "FAIL", what));
        if (!ok) ++failures;
    };
    {
        auto missing = std::make_shared<MediaItem>(*a);
        missing->path = QDir(dir).filePath("missing.wav");
        auto first = snapshot(missing, 1.0, false, false);
        auto second = snapshot(b, 1.0, false, false);
        AudioMixer reused(48000), fresh(48000);
        std::vector<float> actual(2048), expected(2048);
        reused.mix(first, 0, 1024, actual.data());
        reused.mix(second, 0, 1024, actual.data());
        fresh.mix(second, 0, 1024, expected.data());
        check(actual == expected && measure(expected).peakDb > -30,
              "a failed decoder does not silence a different source with the same ID");
    }
    {
        Project p;
        auto first = p.snapshot();
        p.setSettings(p.settings());
        check(first->session == p.snapshot()->session, "ordinary edits preserve the project session");
        p.clear();
        check(first->session != p.snapshot()->session, "new project changes the playback session");
        const QString path = QDir(dir).filePath("session.mvp");
        QString error;
        const bool saved = p.save(path, &error);
        auto before = p.snapshot();
        const bool loaded = p.load(path, &error);
        check(saved && loaded && before->session != p.snapshot()->session,
              "opening even the same project starts a fresh playback session");
        before = p.snapshot();
        check(!p.load(QDir(dir).filePath("absent.mvp"), &error) && before->session == p.snapshot()->session,
              "failed project opening preserves the current session");
        SequenceSnapshotCache cache;
        NestedTimeline nested; nested.value = std::make_shared<const Timeline>(defaultTimeline());
        check(cache.get(*p.snapshot(), nested).session == p.snapshot()->session,
              "compound sequences inherit project playback identity");
    }
    return failures;
}

int cmdAudioFx(const QString& mediaDir, const QString& outDir)
{
    QDir().mkpath(outDir);
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    const QDir dir(mediaDir);
    auto sine = loadItem(dir.filePath(QStringLiteral("audio_sine_loud.wav")), 1);
    auto noise = loadItem(dir.filePath(QStringLiteral("audio_noise.wav")), 2);
    auto mixed = loadItem(dir.filePath(QStringLiteral("audio_sine_noise.wav")), 3);
    if (!sine || !noise || !mixed) {
        print(QStringLiteral("FAIL  audio test signals missing (run tests/make_test_media.sh)"));
        return 1;
    }
    const int rate = 48000;
    const FrameRate fps{30, 1};

    auto snapFor = [&](const std::shared_ptr<MediaItem>& item, TimeUs length = 0) {
        ProjectSnapshot s;
        s.settings.fps = fps;
        s.media[item->id] = item;
        s.timeline = defaultTimeline();
        ops::insertMedia(s.timeline, *item, 0, -1, fps);
        if (length > 0)
            for (auto& t : s.timeline.tracks)
                for (auto& c : t.clips)
                    c.length = length;
        return s;
    };
    auto audioClip = [](ProjectSnapshot& s) -> Clip& {
        for (auto& t : s.timeline.tracks)
            if (t.type == TrackType::Audio && !t.clips.empty())
                return t.clips.front();
        return s.timeline.tracks.back().clips.front();
    };
    auto audioTrack = [](ProjectSnapshot& s) -> Track& {
        for (auto& t : s.timeline.tracks)
            if (t.type == TrackType::Audio)
                return t;
        return s.timeline.tracks.back();
    };
    auto mixRange = [&](const ProjectSnapshot& s, double from, double seconds) {
        AudioMixer mixer(rate);
        const int total = int(seconds * rate);
        std::vector<float> out(size_t(total) * 2), chunk(1024 * 2);
        const int64_t start = int64_t(from * rate);
        for (int i = 0; i < total; i += 1024) {
            const int n = std::min(1024, total - i);
            mixer.mix(s, start + i, n, chunk.data());
            std::copy_n(chunk.begin(), n * 2, out.begin() + i * 2);
        }
        return out;
    };
    auto withEffect = [&](const std::shared_ptr<MediaItem>& item, const QString& type, const std::map<QString, double>& values) {
        ProjectSnapshot s = snapFor(item);
        EffectInstance e = makeEffectOfType(type, 900);
        for (const auto& [id, v] : values)
            e.params[id].value = v;
        audioClip(s).effects.push_back(e);
        return s;
    };

    // ---- Every effect runs, changes the sound, stays finite, and is fast ----
    const std::map<QString, std::map<QString, double>> testValues = {
        {QStringLiteral("a_gain"), {{QStringLiteral("gain"), -6}}},
        {QStringLiteral("a_eq"), {{QStringLiteral("mid1Gain"), 9}}},
        {QStringLiteral("a_filter"), {{QStringLiteral("frequency"), 200}}},
        {QStringLiteral("a_compressor"), {{QStringLiteral("threshold"), -30}}},
        {QStringLiteral("a_limiter"), {{QStringLiteral("ceiling"), -12}}},
        {QStringLiteral("a_gate"), {{QStringLiteral("threshold"), -10}}},
        {QStringLiteral("a_stereo"), {{QStringLiteral("balance"), 0.5}}},
        {QStringLiteral("a_pitch"), {{QStringLiteral("semitones"), 5}}},
    };
    const std::vector<float> dry = mixRange(snapFor(mixed), 0, 3.0);
    for (const AudioEffectDef& def : audioEffects()) {
        if (def.id == QLatin1String("a_vst3"))
            continue; // (no plugin chosen: the sound goes through; see the vst3 test)
        auto it = testValues.find(def.id);
        ProjectSnapshot s = withEffect(mixed, def.id, it == testValues.end() ? std::map<QString, double>{} : it->second);
        QElapsedTimer timer;
        timer.start();
        const std::vector<float> wet = mixRange(s, 0, 3.0);
        const double ms = double(timer.nsecsElapsed()) / 1e6;
        double diff = 0;
        for (size_t i = 0; i < wet.size(); ++i)
            diff += std::abs(double(wet[i]) - dry[i]);
        diff /= double(wet.size());
        const Levels l = measure(wet);
        // (The gate test value closes it on purpose, so silence is expected there.)
        check(l.finite && diff > 1e-4 && (l.rmsDb > -80 || def.id == QLatin1String("a_gate")),
              QStringLiteral("audio effect %1  (mean change %2, %3x real time incl. decoding)")
                  .arg(def.name)
                  .arg(diff, 0, 'f', 4)
                  .arg(3000.0 / std::max(0.01, ms), 0, 'f', 0));
    }

    // ---- Measured behaviour --------------------------------------------------------------
    const double sineRms = measure(mixRange(snapFor(sine), 0.5, 1.0)).rmsDb;
    {
        const double lp = measure(mixRange(withEffect(sine, QStringLiteral("a_filter"),
                                                      {{QStringLiteral("frequency"), 100}, {QStringLiteral("slope"), 1}}), 0.5, 1.0)).rmsDb;
        const double hp = measure(mixRange(withEffect(sine, QStringLiteral("a_filter"),
                                                      {{QStringLiteral("type"), 1}, {QStringLiteral("frequency"), 3000}, {QStringLiteral("slope"), 1}}), 0.5, 1.0)).rmsDb;
        check(sineRms - lp > 20 && sineRms - hp > 20,
              QStringLiteral("filters cut a 440 Hz tone: low-pass 100 Hz by %1 dB, high-pass 3 kHz by %2 dB").arg(sineRms - lp, 0, 'f', 1).arg(sineRms - hp, 0, 'f', 1));
        const double eq = measure(mixRange(withEffect(sine, QStringLiteral("a_eq"),
                                                      {{QStringLiteral("mid1Freq"), 440}, {QStringLiteral("mid1Gain"), 12}, {QStringLiteral("mid1Q"), 1}}), 0.5, 1.0)).rmsDb;
        // +12 dB on a -6 dBFS tone clips at full scale, so check a quieter boost too.
        const double eq6 = measure(mixRange(withEffect(sine, QStringLiteral("a_eq"),
                                                       {{QStringLiteral("mid1Freq"), 440}, {QStringLiteral("mid1Gain"), 5}}), 0.5, 1.0)).rmsDb;
        check(std::abs((eq6 - sineRms) - 5.0) < 0.5 && eq > eq6,
              QStringLiteral("EQ bell at 440 Hz: +5 dB setting gives %1 dB").arg(eq6 - sineRms, 0, 'f', 2));
        // The device chain's pictures of them draw what is heard: the EQ's and the filter's curves at 440 Hz, and the
        // compressor's, gate's and limiter's transfer curves.
        auto settings = [](const QString& type, const std::map<QString, double>& set) {
            Params p;
            for (const ParamDef& d : findAudioEffect(type)->params)
                p.numbers[d.id] = d.def;
            for (const auto& [k, v] : set)
                p.numbers[k] = v;
            return p;
        };
        const double eqDrawn = audioEffectResponseDb(QStringLiteral("a_eq"), settings(QStringLiteral("a_eq"), {{QStringLiteral("mid1Freq"), 440}, {QStringLiteral("mid1Gain"), 5}}), 440);
        const double lpDrawn = audioEffectResponseDb(QStringLiteral("a_filter"), settings(QStringLiteral("a_filter"), {{QStringLiteral("frequency"), 200}}), 440);
        const double lpHeard = measure(mixRange(withEffect(sine, QStringLiteral("a_filter"), {{QStringLiteral("frequency"), 200}}), 0.5, 1.0)).rmsDb - sineRms;
        const Params comp = settings(QStringLiteral("a_compressor"), {{QStringLiteral("threshold"), -20}, {QStringLiteral("ratio"), 4}, {QStringLiteral("knee"), 0}});
        const Params gate = settings(QStringLiteral("a_gate"), {{QStringLiteral("threshold"), -40}, {QStringLiteral("range"), -60}});
        const Params lim = settings(QStringLiteral("a_limiter"), {{QStringLiteral("ceiling"), -1}});
        check(std::abs(eqDrawn - (eq6 - sineRms)) < 0.5 && std::abs(lpDrawn - lpHeard) < 0.5
                  && std::abs(audioEffectTransferDb(QStringLiteral("a_compressor"), comp, -6) + 16.5) < 0.01
                  && audioEffectTransferDb(QStringLiteral("a_gate"), gate, -50) == -110 && audioEffectTransferDb(QStringLiteral("a_gate"), gate, -30) == -30
                  && audioEffectTransferDb(QStringLiteral("a_limiter"), lim, 6) == -1 && std::isnan(audioEffectResponseDb(QStringLiteral("a_gain"), {}, 440)),
              QStringLiteral("the device pictures draw what is heard: the EQ at 440 Hz %1 dB (heard %2), a 200 Hz low-pass %3 dB (heard %4), "
                             "the dynamics' transfer curves")
                  .arg(eqDrawn, 0, 'f', 2).arg(eq6 - sineRms, 0, 'f', 2).arg(lpDrawn, 0, 'f', 2).arg(lpHeard, 0, 'f', 2));
    }
    {
        // -6 dBFS peak sine, threshold -20, ratio 4, hard knee: about -16.5 dBFS peak.
        const Levels c = measure(mixRange(withEffect(sine, QStringLiteral("a_compressor"),
                                                     {{QStringLiteral("threshold"), -20}, {QStringLiteral("ratio"), 4}, {QStringLiteral("knee"), 0},
                                                      {QStringLiteral("attack"), 1}, {QStringLiteral("release"), 300}}), 0.5, 1.0),
                                 size_t(rate) / 10); // skip the first 100 ms while it settles
        check(std::abs(c.peakDb - (-16.5)) < 1.5, QStringLiteral("compressor 4:1 at -20 dB: -6 dBFS peak in, %1 dBFS out (expected about -16.5)").arg(c.peakDb, 0, 'f', 1));
    }
    {
        ProjectSnapshot s = withEffect(sine, QStringLiteral("a_gain"), {{QStringLiteral("gain"), 12}});
        EffectInstance lim = makeEffectOfType(QStringLiteral("a_limiter"), 901);
        lim.params[QStringLiteral("ceiling")].value = -1.0;
        audioClip(s).effects.push_back(lim);
        const Levels l = measure(mixRange(s, 0.0, 2.0));
        check(l.peakDb <= -0.95 && l.rmsDb > sineRms + 3,
              QStringLiteral("limiter: +12 dB into a -1 dB ceiling peaks at %1 dBFS, louder by %2 dB").arg(l.peakDb, 0, 'f', 2).arg(l.rmsDb - sineRms, 0, 'f', 1));
    }
    {
        const double noiseRms = measure(mixRange(snapFor(noise), 0.5, 1.0)).rmsDb;
        const double gated = measure(mixRange(withEffect(noise, QStringLiteral("a_gate"), {{QStringLiteral("threshold"), -6}}), 0.5, 1.0)).rmsDb;
        check(noiseRms - gated > 30, QStringLiteral("noise gate closes on -30 dB noise: %1 dB quieter").arg(noiseRms - gated, 0, 'f', 1));
        const double denoised = measure(mixRange(withEffect(noise, QStringLiteral("a_denoise"),
                                                            {{QStringLiteral("amount"), 20}, {QStringLiteral("floor"), -30}}), 0.5, 2.0),
                                        size_t(rate)).rmsDb;
        check(noiseRms - denoised > 6, QStringLiteral("noise reduction removes %1 dB of white noise").arg(noiseRms - denoised, 0, 'f', 1));
    }
    {
        const double f = estimateFrequency(mixRange(withEffect(sine, QStringLiteral("a_pitch"), {{QStringLiteral("semitones"), 12}}), 0.5, 1.0), rate);
        check(std::abs(f - 880) < 880 * 0.05, QStringLiteral("pitch shift +12 semitones: 440 Hz -> %1 Hz").arg(f, 0, 'f', 0));
        // Latency compensation: effect output must stay aligned with the
        // picture. Measured as the lag with the best correlation to the dry
        // signal (pitch shift by 1 cent so it stays comparable).
        auto fresh = [&](const std::shared_ptr<MediaItem>& item) {
            ProjectSnapshot s = snapFor(item);
            for (auto& t : s.timeline.tracks)
                for (auto& c : t.clips)
                    c.start = kUsPerSecond;
            return s;
        };
        // White noise: no repeating pattern, so the correlation peak is unique.
        const std::vector<float> dryOut = mixRange(fresh(noise), 1.0, 2.0);
        for (const char* fx : {"a_pitch", "a_denoise", "a_limiter", "a_ffmpeg", "a_dereverb", "a_declick", "a_dehum"}) {
            ProjectSnapshot s = fresh(noise);
            EffectInstance e = makeEffectOfType(QString::fromLatin1(fx), 905);
            if (QLatin1String(fx) == QLatin1String("a_pitch"))
                e.params[QStringLiteral("cents")].value = 1;
            audioClip(s).effects.push_back(e);
            const std::vector<float> wet = mixRange(s, 1.0, 2.0);
            int bestLag = 0;
            double best = -1e30;
            const int maxLag = rate / 10; // +-100 ms
            for (int lag = -maxLag; lag <= maxLag; lag += 1) {
                double sum = 0;
                for (int i = rate / 2; i < rate; i += 4) // middle of the event
                    sum += double(dryOut[size_t(i) * 2]) * wet[size_t(i + lag) * 2];
                if (sum > best) {
                    best = sum;
                    bestLag = lag;
                }
            }
            const double ms = bestLag * 1000.0 / rate;
            check(std::abs(ms) <= 2.0, QStringLiteral("%1 stays in sync with the picture (offset %2 ms)")
                                           .arg(QString::fromLatin1(fx)).arg(ms, 0, 'f', 1));
        }
    }
    {
        // Track FX pass silence by once their output has died away, and start afresh when sound comes back: a line after
        // a long gap sounds as the same line at the start (and the gap costs nothing).
        ProjectSnapshot first = snapFor(sine, kUsPerSecond);
        // (a filter: the same sound wherever it starts; a pitch shift's output also depends on where its frames fall)
        audioTrack(first).effects.push_back(makeEffectOfType(QStringLiteral("a_filter"), 910));
        ProjectSnapshot later = first;
        audioClip(later).start = 8 * kUsPerSecond;
        const std::vector<float> a = mixRange(first, 0, 2.5), all = mixRange(later, 0, 10.5);
        double worst = 0, peak = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            worst = std::max(worst, double(std::abs(a[i] - all[size_t(8 * rate) * 2 + i])));
            peak = std::max(peak, double(std::abs(a[i])));
        }
        check(peak > 0.01 && worst < 1e-4, QStringLiteral("track FX after a long silence sound as at the start (difference %1)").arg(worst, 0, 'g', 3));
    }
    {
        // Track reverb / delay ring on after a 1 s event ends; event FX don't.
        for (const char* fx : {"a_reverb", "a_delay"}) {
            ProjectSnapshot s = snapFor(sine, kUsPerSecond);
            audioTrack(s).effects.push_back(makeEffectOfType(QString::fromLatin1(fx), 902));
            const std::vector<float> out = mixRange(s, 0, 1.5);
            const Levels tail = measure(out, size_t(rate) * 11 / 10);
            check(tail.rmsDb > -60, QStringLiteral("%1 on a track rings out after the event ends (tail %2 dB)")
                                        .arg(QString::fromLatin1(fx)).arg(tail.rmsDb, 0, 'f', 1));
        }
    }
    {
        ProjectSnapshot s = snapFor(sine);
        EffectInstance e = makeEffectOfType(QStringLiteral("a_gain"), 903);
        e.params[QStringLiteral("gain")].addKey(0, -40);
        e.params[QStringLiteral("gain")].addKey(2 * kUsPerSecond, 0);
        audioClip(s).effects.push_back(e);
        const double early = measure(mixRange(s, 0.05, 0.1)).rmsDb, late = measure(mixRange(s, 1.9, 0.1)).rmsDb;
        check(late - early > 30, QStringLiteral("keyframed gain fades in (%1 dB -> %2 dB)").arg(early, 0, 'f', 1).arg(late, 0, 'f', 1));
    }
    {
        ProjectSnapshot s = snapFor(sine);
        const float peak = measureClipPeak(*sine, audioClip(s));
        check(std::abs(peak - 0.5f) < 0.01f, QStringLiteral("normalize measures the -6 dBFS test tone's peak as %1").arg(peak, 0, 'f', 3));
    }
    {
        // Master limiter applies to the whole mix and to exports.
        ProjectSnapshot s = snapFor(sine);
        EffectInstance lim = makeEffectOfType(QStringLiteral("a_limiter"), 904);
        lim.params[QStringLiteral("ceiling")].value = -12;
        s.timeline.audioMasterEffects.push_back(lim);
        const Levels l = measure(mixRange(s, 0.5, 1.0));
        check(l.peakDb <= -11.95, QStringLiteral("master limiter holds the mix at %1 dBFS").arg(l.peakDb, 0, 'f', 2));

        ExportSettings es;
        es.path = QDir(outDir).filePath(QStringLiteral("master_fx.wav"));
        es.muxer = QStringLiteral("wav");
        es.audioEncoder = QStringLiteral("pcm_s16le");
        es.rangeEnd = 2 * kUsPerSecond;
        Exporter exporter;
        bool ok = exporter.run(s, es);
        double exportedPeak = 0;
        if (ok) {
            auto out = loadItem(es.path, 50);
            ProjectSnapshot back = out ? snapFor(out) : ProjectSnapshot{};
            exportedPeak = out ? measure(mixRange(back, 0.5, 1.0)).peakDb : 0;
        }
        check(ok && exportedPeak <= -11.9, QStringLiteral("exported WAV carries the master FX (peak %1 dBFS)").arg(exportedPeak, 0, 'f', 2));
    }
    {
        Project proj;
        const int id = proj.addMedia(mixed->info);
        Timeline tl = proj.timeline();
        ops::insertMedia(tl, *proj.mediaItem(id), 0, -1, proj.settings().fps);
        for (auto& t : tl.tracks)
            if (t.type == TrackType::Audio) {
                t.clips.front().effects.push_back(makeEffectOfType(QStringLiteral("a_eq"), tl.allocId()));
                t.effects.push_back(makeEffectOfType(QStringLiteral("a_reverb"), tl.allocId()));
            }
        tl.audioMasterEffects.push_back(makeEffectOfType(QStringLiteral("a_limiter"), tl.allocId()));
        proj.applyEdit(tl, QStringLiteral("test"));
        const QString path = QDir(outDir).filePath(QStringLiteral("audiofx.mvp"));
        QString err;
        Project loaded;
        check(proj.save(path, &err) && loaded.load(path, &err) && loaded.timeline() == proj.timeline(),
              QStringLiteral("audio event, track and master FX survive save/load %1").arg(err));
    }
    {
        // Restoration: De-hum takes 50 Hz hum (and its harmonics) out and leaves a 1 kHz tone; De-click takes the
        // clicks out of a tone; the spectrogram shows a tone at its row; an event's sound comes out for it.
        auto writeWav = [&](const QString& path, const std::function<float(int)>& sample, int n) {
            QFile f(path);
            f.open(QIODevice::WriteOnly);
            QDataStream st(&f);
            st.setByteOrder(QDataStream::LittleEndian);
            st.setFloatingPointPrecision(QDataStream::SinglePrecision);
            f.write("RIFF", 4);
            st << quint32(36 + n * 8);
            f.write("WAVEfmt ", 8);
            st << quint32(16) << quint16(3) << quint16(2) << quint32(rate) << quint32(rate * 8) << quint16(8) << quint16(32);
            f.write("data", 4);
            st << quint32(n * 8);
            for (int i = 0; i < n; ++i) {
                const float x = sample(i);
                st << x << x;
            }
        };
        const double pi = 3.14159265358979;
        const QString humPath = QDir(outDir).filePath(QStringLiteral("hum.wav")), clickPath = QDir(outDir).filePath(QStringLiteral("clicks.wav"));
        writeWav(humPath, [&](int i) { return float(0.25 * std::sin(2 * pi * 50 * i / rate) + 0.08 * std::sin(2 * pi * 150 * i / rate)
                                                    + 0.25 * std::sin(2 * pi * 1000 * i / rate)); }, rate * 3);
        writeWav(clickPath, [&](int i) { return float(0.1 * std::sin(2 * pi * 440 * i / rate) + (i % (rate / 4) == rate / 8 ? 0.9 : 0.0)); }, rate * 3);
        auto hum = loadItem(humPath, 40), clicks = loadItem(clickPath, 41);
        // A tone's level in the mix (Goertzel), dB.
        auto toneDb = [&](const std::vector<float>& stereo, double hz) {
            const size_t n = stereo.size() / 2;
            double s1 = 0, s2 = 0;
            const double k = 2 * std::cos(2 * pi * hz / rate);
            for (size_t i = 0; i < n; ++i) {
                const double s0 = stereo[i * 2] + k * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            return 20 * std::log10(std::max(1e-12, std::sqrt(s1 * s1 + s2 * s2 - k * s1 * s2) / (n / 2.0)));
        };
        if (hum && clicks) {
            // (From the start, the last 1.5 s: the notches settled, as when playing.)
            auto settled = [&](const ProjectSnapshot& s) {
                const std::vector<float> all = mixRange(s, 0.0, 3.0);
                return std::vector<float>(all.begin() + rate * 3, all.end());
            };
            const std::vector<float> raw = settled(snapFor(hum));
            ProjectSnapshot fixed = snapFor(hum);
            audioClip(fixed).effects.push_back(makeEffectOfType(QStringLiteral("a_dehum"), fixed.timeline.allocId()));
            const std::vector<float> clean = settled(fixed);
            const double humCut = toneDb(raw, 50) - toneDb(clean, 50), thirdCut = toneDb(raw, 150) - toneDb(clean, 150);
            const double toneKept = toneDb(clean, 1000) - toneDb(raw, 1000);
            check(humCut > 30 && thirdCut > 30 && std::abs(toneKept) < 1,
                  QStringLiteral("De-hum takes 50 Hz hum down %1 dB (its 3rd harmonic %2 dB) and leaves a 1 kHz tone (%3 dB)")
                      .arg(humCut, 0, 'f', 1).arg(thirdCut, 0, 'f', 1).arg(toneKept, 0, 'f', 2));
            ProjectSnapshot declicked = snapFor(clicks);
            audioClip(declicked).effects.push_back(makeEffectOfType(QStringLiteral("a_declick"), declicked.timeline.allocId()));
            const Levels before = measure(mixRange(snapFor(clicks), 0.5, 2.0)), after = measure(mixRange(declicked, 0.5, 2.0));
            check(before.peakDb > -2 && after.peakDb < before.peakDb - 6,
                  QStringLiteral("De-click takes clicks out of a tone (peaks %1 dBFS -> %2)").arg(before.peakDb, 0, 'f', 1).arg(after.peakDb, 0, 'f', 1));
            // The spectrogram: the 1 kHz tone is the brightest row near 1 kHz's (and the hum at 50 Hz's).
            const std::vector<float> mono = spectrogram::eventSound(snapFor(hum), audioClip(fixed).id, true, rate);
            const QImage pic = spectrogram::render(mono, rate, QSize(200, 300));
            auto brightest = [&](int from, int to) {
                int best = from;
                double most = -1;
                for (int y = from; y < to; ++y) {
                    const double v = qGray(pic.pixel(100, y));
                    if (v > most) {
                        most = v;
                        best = y;
                    }
                }
                return best;
            };
            const int toneRow = int(std::lround(spectrogram::rowOf(1000, rate, 300))), humRow = int(std::lround(spectrogram::rowOf(50, rate, 300)));
            check(mono.size() == size_t(rate * 3) && std::abs(brightest(toneRow - 20, toneRow + 20) - toneRow) <= 2
                      && std::abs(brightest(humRow - 12, std::min(300, humRow + 12)) - humRow) <= 4, // (within 4 Hz at 50)
                  QStringLiteral("the spectrogram shows a 1 kHz tone and 50 Hz hum at their rows (the event's sound: %1 s)").arg(double(mono.size()) / rate, 0, 'f', 1));
        } else {
            check(false, QStringLiteral("the hum and click test files were made"));
        }
    }
    {
        // Pre-rendering's stretches: an edit changes its own stretch's key only; a colour label changes none; frames on
        // disk come back.
        ProjectSnapshot s = snapFor(sine, 3 * kUsPerSecond);
        for (auto& t : s.timeline.tracks)
            if (!t.clips.empty()) {
                Clip c = t.clips.front();
                c.id = s.timeline.allocId();
                c.start = 5 * kUsPerSecond;
                t.clips.push_back(c);
            }
        const auto before = precache::stretches(s, QSize(320, 180), false);
        ProjectSnapshot edited = s;
        audioClip(edited).gain = 0.5f; // (the first event: 0 - 3 s)
        ProjectSnapshot labelled = s;
        for (auto& t : labelled.timeline.tracks)
            for (auto& c : t.clips)
                c.label = 3;
        const auto after = precache::stretches(edited, QSize(320, 180), false), same = precache::stretches(labelled, QSize(320, 180), false);
        bool firstChanged = false, othersKept = before.size() == after.size(), labelKept = before.size() == same.size();
        for (size_t i = 0; i < before.size() && i < after.size(); ++i) {
            if (before[i].end <= 3 * kUsPerSecond)
                firstChanged = firstChanged || before[i].key != after[i].key;
            else
                othersKept = othersKept && before[i].key == after[i].key;
            labelKept = labelKept && i < same.size() && same[i].key == before[i].key;
        }
        const QString dir = QDir(outDir).filePath(QStringLiteral("prerender-test"));
        precache::Store store(dir);
        store.clear();
        QImage frame(64, 36, QImage::Format_ARGB32_Premultiplied);
        frame.fill(QColor(30, 160, 90));
        store.save(before.front().key, 7, frame);
        precache::Store again(dir); // (as after a restart: found on disk)
        const QImage back = again.load(before.front().key, 7);
        again.trim(0);
        // (Trimmed by another MVP sharing the folder: the first store finds it gone and makes it again.)
        const bool goneSeen = store.has(before.front().key, 7) && store.load(before.front().key, 7).isNull() && !store.has(before.front().key, 7);
        check(before.size() == 3 && firstChanged && othersKept && labelKept && !back.isNull() && qGreen(back.pixel(10, 10)) > 150
                  && !precache::Store(dir).has(before.front().key, 7) && goneSeen && precache::stretchAt(before, 6 * kUsPerSecond) == &before[2],
              QStringLiteral("pre-rendering: an edit makes only its own stretch new (%1 stretches), a colour label none; frames come back "
                             "from disk and are trimmed, and one trimmed by another MVP is made again (%2)").arg(before.size()).arg(goneSeen ? "yes" : "no"));
    }
    {
        // A realistic voice chain on one track: EQ + compressor + reverb + limiter.
        ProjectSnapshot s = snapFor(mixed);
        for (const char* fx : {"a_eq", "a_compressor", "a_denoise"})
            audioClip(s).effects.push_back(makeEffectOfType(QString::fromLatin1(fx), s.timeline.allocId()));
        audioTrack(s).effects.push_back(makeEffectOfType(QStringLiteral("a_reverb"), s.timeline.allocId()));
        s.timeline.audioMasterEffects.push_back(makeEffectOfType(QStringLiteral("a_limiter"), s.timeline.allocId()));
        QElapsedTimer timer;
        timer.start();
        mixRange(s, 0, 4.0);
        const double factor = 4000.0 / std::max(0.01, double(timer.nsecsElapsed()) / 1e6);
        // (about 50x alone; it fell to 17x with the other tests running beside it)
        check(factor > 5, QStringLiteral("EQ + compressor + noise reduction + reverb + limiter run at %1x real time").arg(factor, 0, 'f', 0));
    }
    return failures;
}

int cmdEnvelopes(const QString& mediaDir, const QString& outDir)
{
    QDir().mkpath(outDir);
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    const QDir dir(mediaDir);
    auto sine = loadItem(dir.filePath(QStringLiteral("audio_sine_loud.wav")), 1);
    auto video = loadItem(dir.filePath(QStringLiteral("h264_aac.mp4")), 2);
    if (!sine || !video) {
        print(QStringLiteral("FAIL  test media missing"));
        return 1;
    }
    const int rate = 48000;
    const FrameRate fps{30, 1};
    auto snapFor = [&](const std::shared_ptr<MediaItem>& item) {
        ProjectSnapshot s;
        s.settings.fps = fps;
        s.settings.width = 1280;
        s.settings.height = 720;
        s.media[item->id] = item;
        s.timeline = defaultTimeline();
        ops::insertMedia(s.timeline, *item, 0, -1, fps);
        return s;
    };
    auto audioTrack = [](ProjectSnapshot& s) -> Track& {
        for (auto& t : s.timeline.tracks)
            if (t.type == TrackType::Audio)
                return t;
        return s.timeline.tracks.back();
    };
    auto mixRange = [&](const ProjectSnapshot& s, double from, double seconds) {
        AudioMixer mixer(rate);
        const int total = int(seconds * rate);
        std::vector<float> out(size_t(total) * 2), chunk(1024 * 2);
        for (int i = 0; i < total; i += 1024) {
            const int n = std::min(1024, total - i);
            mixer.mix(s, int64_t(from * rate) + i, n, chunk.data());
            std::copy_n(chunk.begin(), n * 2, out.begin() + i * 2);
        }
        return out;
    };
    auto channelDb = [](const std::vector<float>& s, int ch) {
        double sum = 0;
        for (size_t i = size_t(ch); i < s.size(); i += 2)
            sum += double(s[i]) * s[i];
        return 10 * std::log10(std::max(sum / double(s.size() / 2), 1e-20));
    };
    auto key = [](TimeUs t, double v, Keyframe::Interp interp = Keyframe::Interp::Linear) {
        Keyframe k;
        k.time = t;
        k.value = v;
        k.interp = interp;
        return k;
    };
    const double unity = channelDb(mixRange(snapFor(sine), 0.5, 0.5), 0);

    {
        ProjectSnapshot s = snapFor(sine);
        Envelope& v = audioTrack(s).volume;
        v.active = true;
        v.points = {key(0, 0.0), key(2 * kUsPerSecond, 1.0)};
        const double early = channelDb(mixRange(s, 0.02, 0.05), 0);
        const double mid = channelDb(mixRange(s, 0.98, 0.04), 0);
        const double late = channelDb(mixRange(s, 1.95, 0.05), 0);
        check(late - early > 25 && std::abs((mid - unity) - (-6.02)) < 0.6,
              QStringLiteral("volume envelope ramp: %1 dB -> %2 dB -> %3 dB (halfway = -6 dB)")
                  .arg(early - unity, 0, 'f', 1).arg(mid - unity, 0, 'f', 1).arg(late - unity, 0, 'f', 1));

        // No zipper steps: the gain change between neighbouring samples stays tiny.
        const std::vector<float> ramp = mixRange(s, 0.5, 1.0);
        double worst = 0;
        for (size_t i = 1; i + 1 < ramp.size() / 2; ++i) {
            // Compare successive peaks of |x| scaled by the expected sine shape is overkill;
            // a step would show up as a jump far larger than the sine's own slope.
            worst = std::max(worst, double(std::abs(ramp[i * 2] - ramp[(i - 1) * 2])));
        }
        const double sineSlope = 0.5 * 2 * 3.14159265 * 440 / rate; // max per-sample change of the dry tone
        check(worst < sineSlope * 1.05, QStringLiteral("volume ramp is smooth (largest sample step %1, dry tone %2)").arg(worst, 0, 'f', 4).arg(sineSlope, 0, 'f', 4));
    }
    {
        ProjectSnapshot s = snapFor(sine);
        Envelope& p = audioTrack(s).panEnvelope;
        p.active = true;
        p.points = {key(0, -1.0), key(2 * kUsPerSecond, 1.0)};
        const std::vector<float> a = mixRange(s, 0.05, 0.1), m = mixRange(s, 0.95, 0.1), b = mixRange(s, 1.85, 0.1);
        check(channelDb(a, 0) - channelDb(a, 1) > 15 && std::abs(channelDb(m, 0) - channelDb(m, 1)) < 1.0
                  && channelDb(b, 1) - channelDb(b, 0) > 15,
              QStringLiteral("pan envelope sweeps left -> centre -> right"));
    }
    {
        ProjectSnapshot s = snapFor(sine);
        Envelope& v = audioTrack(s).volume;
        v.active = true;
        v.points = {key(0, 1.0, Keyframe::Interp::Hold), key(kUsPerSecond, 0.25)};
        const double before = channelDb(mixRange(s, 0.85, 0.1), 0), after = channelDb(mixRange(s, 1.2, 0.1), 0);
        check(std::abs(before - unity) < 0.2 && std::abs((after - unity) + 12.04) < 0.3,
              QStringLiteral("hold curve keeps the level until the next point (%1 dB, then %2 dB)").arg(before - unity, 0, 'f', 2).arg(after - unity, 0, 'f', 2));
    }
    {
        ProjectSnapshot s = snapFor(sine);
        Track& t = audioTrack(s);
        t.level = 0.5f;
        t.volume.active = true;
        t.volume.value = 0.5; // flat line at -6 dB
        const double db = channelDb(mixRange(s, 0.5, 0.5), 0);
        check(std::abs((db - unity) + 12.04) < 0.3, QStringLiteral("envelope combines with the track fader (-6 dB x -6 dB = %1 dB)").arg(db - unity, 0, 'f', 2));
        t.volume.active = false; // hidden envelopes have no effect
        const double off = channelDb(mixRange(s, 0.5, 0.5), 0);
        check(std::abs((off - unity) + 6.02) < 0.3, QStringLiteral("hidden envelope is ignored (%1 dB)").arg(off - unity, 0, 'f', 2));
    }
    {
        // Composite level envelope fades the video track in over 2 s.
        ProjectSnapshot s = snapFor(video);
        Envelope& c = s.timeline.tracks[0].composite;
        c.active = true;
        c.points = {key(0, 0.0), key(2 * kUsPerSecond, 1.0)};
        ProjectSnapshot plain = snapFor(video);
        Compositor cpu(true);
        const QSize size(640, 360);
        const QImage a = cpu.render(s, 0, size), half = cpu.render(s, kUsPerSecond, size), full = cpu.render(s, 2 * kUsPerSecond, size);
        const QImage ref = cpu.render(plain, kUsPerSecond, size);
        const QRgb pm = half.pixel(64, 180), pr = ref.pixel(64, 180); // a saturated colour bar
        const bool halfway = std::abs(qRed(pm) - qRed(pr) / 2) <= 4 && std::abs(qGreen(pm) - qGreen(pr) / 2) <= 4;
        check(qGray(a.pixel(64, 180)) < 3 && halfway && imageDifference(full, cpu.render(plain, 2 * kUsPerSecond, size)) < 0.5,
              QStringLiteral("composite level envelope fades the picture in (50% at the midpoint)"));
        auto surface = createGpuSurface();
        QString err;
        if (auto gpu = surface ? GpuCompositor::create(surface.get(), true, &err) : nullptr) {
            const double d = imageDifference(half, gpu->render(s, kUsPerSecond, size));
            check(d < 1.5, QStringLiteral("GPU renders the composite envelope like the CPU (difference %1)").arg(d, 0, 'f', 2));
        }
    }
    {
        Project proj;
        const int id = proj.addMedia(sine->info);
        Timeline tl = proj.timeline();
        ops::insertMedia(tl, *proj.mediaItem(id), 0, -1, proj.settings().fps);
        for (auto& t : tl.tracks) {
            if (t.type == TrackType::Audio) {
                t.volume.active = true;
                t.volume.points = {key(0, 1.0, Keyframe::Interp::Smooth), key(kUsPerSecond, 0.3)};
                t.panEnvelope.active = true;
                t.panEnvelope.value = -0.4;
            } else {
                t.composite.active = false; // hidden but keeps its points
                t.composite.points = {key(0, 0.2)};
            }
        }
        proj.applyEdit(tl, QStringLiteral("test"));
        const QString path = QDir(outDir).filePath(QStringLiteral("envelopes.mvp"));
        QString err;
        Project loaded;
        check(proj.save(path, &err) && loaded.load(path, &err) && loaded.timeline() == proj.timeline(),
              QStringLiteral("envelopes survive save/load %1").arg(err));

        ExportSettings es;
        es.path = QDir(outDir).filePath(QStringLiteral("envelope.wav"));
        es.muxer = QStringLiteral("wav");
        es.audioEncoder = QStringLiteral("pcm_s16le");
        es.rangeEnd = 2 * kUsPerSecond;
        Exporter exporter;
        const bool ok = exporter.run(*loaded.snapshot(), es);
        double startDb = 0, endDb = 0;
        if (ok)
            if (auto back = loadItem(es.path, 60)) {
                const ProjectSnapshot bs = snapFor(back);
                startDb = channelDb(mixRange(bs, 0.0, 0.05), 0);
                endDb = channelDb(mixRange(bs, 1.5, 0.2), 0);
            }
        check(ok && startDb - endDb > 8, QStringLiteral("exported audio follows the envelope (%1 dB drop)").arg(startDb - endDb, 0, 'f', 1));
    }
    return failures;
}

// Writes interleaved stereo float samples as a 16-bit WAV file.
bool writeWav(const QString& path, const std::vector<float>& stereo, int rate)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    const uint32_t dataBytes = uint32_t(stereo.size() * 2);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36 + dataBytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(uint32_t(rate));
    u32(uint32_t(rate) * 4);
    u16(4);
    u16(16);
    f.write("data", 4);
    u32(dataBytes);
    std::vector<int16_t> pcm(stereo.size());
    for (size_t i = 0; i < stereo.size(); ++i)
        pcm[i] = int16_t(std::lround(std::clamp(stereo[i], -1.0f, 1.0f) * 32767.0f));
    f.write(reinterpret_cast<const char*>(pcm.data()), qint64(pcm.size() * 2));
    return true;
}

int cmdRamps(const QString& mediaDir, const QString& outDir)
{
    QDir().mkpath(outDir);
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    const QDir dir(mediaDir);
    auto video = loadItem(dir.filePath(QStringLiteral("h264_aac.mp4")), 1);
    auto sine = loadItem(dir.filePath(QStringLiteral("audio_sine_loud.wav")), 2);
    if (!video || !sine) {
        print(QStringLiteral("FAIL  test media missing"));
        return 1;
    }
    const int rate = 48000;
    const FrameRate fps{30, 1};
    const QSize size(640, 360);
    auto key = [](TimeUs t, double v, Keyframe::Interp interp = Keyframe::Interp::Linear) {
        Keyframe k;
        k.time = t;
        k.value = v;
        k.interp = interp;
        return k;
    };
    auto snapFor = [&](const std::shared_ptr<MediaItem>& item, const std::function<void(Clip&)>& setup) {
        ProjectSnapshot s;
        s.settings.fps = fps;
        s.settings.width = size.width();
        s.settings.height = size.height();
        s.media[item->id] = item;
        s.timeline = defaultTimeline();
        ops::insertMedia(s.timeline, *item, 0, -1, fps);
        for (auto& t : s.timeline.tracks)
            for (auto& c : t.clips)
                setup(c);
        return s;
    };
    auto mixRange = [&](const ProjectSnapshot& s, double from, double seconds) {
        AudioMixer mixer(rate);
        const int total = int(seconds * rate);
        std::vector<float> out(size_t(total) * 2), chunk(1024 * 2);
        for (int i = 0; i < total; i += 1024) {
            const int n = std::min(1024, total - i);
            mixer.mix(s, int64_t(from * rate) + i, n, chunk.data());
            std::copy_n(chunk.begin(), n * 2, out.begin() + i * 2);
        }
        return out;
    };
    auto rms = [](const std::vector<float>& s, size_t from, size_t frames) {
        double sum = 0;
        for (size_t i = from; i < from + frames && i * 2 < s.size(); ++i)
            sum += double(s[i * 2]) * s[i * 2];
        return std::sqrt(sum / double(std::max<size_t>(1, frames)));
    };
    // Linear ramp 100% -> 200% over 2 s, then 200%.
    auto rampUp = [&](Clip& c) {
        c.velocity.active = true;
        c.velocity.points = {key(0, 1.0), key(2 * kUsPerSecond, 2.0)};
    };

    // ---- Maths --------------------------------------------------------------------------
    {
        Clip c;
        c.length = 2 * kUsPerSecond;
        c.velocity.active = true;
        c.velocity.points = {key(0, 1.0), key(2 * kUsPerSecond, 3.0)};
        const double lin = c.rampOffset(double(kUsPerSecond)), linEnd = c.rampOffset(2.0 * kUsPerSecond);
        c.velocity.points[0].interp = Keyframe::Interp::Smooth;
        const double smooth = c.rampOffset(double(kUsPerSecond)), smoothEnd = c.rampOffset(2.0 * kUsPerSecond);
        c.velocity.points[0].interp = Keyframe::Interp::Hold;
        const double hold = c.rampOffset(2.0 * kUsPerSecond);
        check(std::abs(lin - 1.5e6) < 2 && std::abs(linEnd - 4e6) < 2 && std::abs(smooth - 1.375e6) < 2
                  && std::abs(smoothEnd - 4e6) < 2 && std::abs(hold - 2e6) < 2,
              QStringLiteral("ramp integrals: linear 1.5 s / 4 s, smooth 1.375 s / 4 s, hold 2 s (got %1 %2 %3 %4 %5)")
                  .arg(lin / 1e6).arg(linEnd / 1e6).arg(smooth / 1e6).arg(smoothEnd / 1e6).arg(hold / 1e6));
        // Forwards then backwards: the span is the furthest point reached.
        c.velocity.points = {key(0, 1.0), key(kUsPerSecond, 1.0), key(kUsPerSecond + 1, -1.0)};
        c.length = 3 * kUsPerSecond / 2;
        check(std::llabs(c.sourceSpan() - kUsPerSecond) < 5 && std::llabs(c.sourceTimeAt(c.start + c.length) - kUsPerSecond / 2) < 5,
              QStringLiteral("backwards ramp: span %1 s, ends at %2 s").arg(c.sourceSpan() / 1e6).arg(c.sourceTimeAt(c.length) / 1e6));
    }

    // ---- Video follows the ramp ---------------------------------------------------------------
    const ProjectSnapshot plain = snapFor(video, [](Clip&) {});
    Compositor plainComp(true);
    auto plainAt = [&](TimeUs t) { return plainComp.render(plain, t, size); };
    {
        ProjectSnapshot s = snapFor(video, rampUp);
        Compositor comp(true);
        const double d1 = imageDifference(comp.render(s, kUsPerSecond, size), plainAt(1250000));
        const double d2 = imageDifference(comp.render(s, 1800000, size), plainAt(2610000));
        const double dWrong = imageDifference(comp.render(s, 1800000, size), plainAt(1800000));
        check(d1 < 1.0 && d2 < 1.0 && dWrong > 2.0,
              QStringLiteral("video follows the ramp: 1.0 s shows 1.25 s, 1.8 s shows 2.61 s (diff %1, %2)").arg(d1, 0, 'f', 2).arg(d2, 0, 'f', 2));

        auto surface = createGpuSurface();
        QString err;
        if (auto gpu = surface ? GpuCompositor::create(surface.get(), true, &err) : nullptr) {
            const double g = imageDifference(comp.render(s, 1800000, size), gpu->render(s, 1800000, size));
            check(g < 1.5, QStringLiteral("GPU renders the ramp like the CPU (difference %1)").arg(g, 0, 'f', 2));
        }
    }
    {
        // Freeze: a hold at 0% shows the same frame throughout.
        ProjectSnapshot s = snapFor(video, [&](Clip& c) {
            c.velocity.active = true;
            c.velocity.points = {key(0, 1.0, Keyframe::Interp::Hold), key(kUsPerSecond, 0.0, Keyframe::Interp::Hold),
                                 key(2 * kUsPerSecond, 1.0)};
        });
        Compositor comp(true);
        const QImage a = comp.render(s, 1100000, size), b = comp.render(s, 1900000, size), after = comp.render(s, 2500000, size);
        check(imageDifference(a, b) < 0.01 && imageDifference(a, plainAt(kUsPerSecond)) < 1.0 && imageDifference(after, plainAt(1500000)) < 1.0,
              QStringLiteral("0% hold freezes the frame, then playback resumes from there"));
    }
    {
        // Backwards: -100% after 1 s runs back through the footage.
        ProjectSnapshot s = snapFor(video, [&](Clip& c) {
            c.velocity.active = true;
            c.velocity.points = {key(0, 1.0, Keyframe::Interp::Hold), key(kUsPerSecond, -1.0)};
            c.length = 3 * kUsPerSecond / 2;
        });
        Compositor comp(true);
        const double d = imageDifference(comp.render(s, 1400000, size), plainAt(600000));
        check(d < 1.0, QStringLiteral("-100% plays backwards (1.4 s shows 0.6 s, diff %1)").arg(d, 0, 'f', 2));
    }
    {
        // Presets keep the footage and change the length.
        for (auto preset : {ops::RampPreset::SlowMiddle, ops::RampPreset::SpeedUp, ops::RampPreset::FreezeMiddle}) {
            ProjectSnapshot s = snapFor(video, [](Clip&) {});
            Clip& c = s.timeline.tracks[0].clips[0];
            const TimeUs span = c.sourceSpan(), len = c.length;
            ops::applyRampPreset(c, video.get(), preset, fps);
            check(c.hasRamp() && std::llabs(c.sourceSpan() - span) <= fps.frameDuration() * 3 && c.length != len,
                  QStringLiteral("preset '%1' keeps the footage: span %2 -> %3 s, length %4 -> %5 s")
                      .arg(ops::rampPresetName(preset)).arg(span / 1e6, 0, 'f', 2).arg(c.sourceSpan() / 1e6, 0, 'f', 2)
                      .arg(len / 1e6, 0, 'f', 2).arg(c.length / 1e6, 0, 'f', 2));
        }
    }
    {
        // Split and trim keep what the ramp shows (forwards and reversed).
        for (bool rev : {false, true}) {
            ProjectSnapshot s = snapFor(video, [&](Clip& c) {
                c.velocity.active = true;
                c.velocity.points = {key(0, 0.5, Keyframe::Interp::Smooth), key(2 * kUsPerSecond, 1.5)};
                c.length = 3 * kUsPerSecond;
                c.reverse = rev;
            });
            Compositor before(true), after(true);
            const TimeUs cut = fps.snap(1300000);
            const QImage a1 = before.render(s, cut - 200000, size), b1 = before.render(s, cut + 300000, size);
            ProjectSnapshot split = s;
            ops::splitClips(split.timeline, {}, cut);
            const QImage a2 = after.render(split, cut - 200000, size), b2 = after.render(split, cut + 300000, size);
            const bool splitOk = imageDifference(a1, a2) < 0.5 && imageDifference(b1, b2) < 0.5;

            ProjectSnapshot trimmed = s;
            Clip& c = trimmed.timeline.tracks[0].clips[0];
            const auto [lo, hi] = ops::trimRange(c, video.get(), true);
            ops::trimClipStart(c, 700000);
            ops::trimClipEnd(c, -400000);
            Compositor tc(true);
            const bool trimOk = imageDifference(b1, tc.render(trimmed, cut + 300000, size)) < 0.5 && lo <= 0 && hi > 0;
            check(splitOk && trimOk, QStringLiteral("split and trim keep the ramp's picture%1").arg(rev ? QStringLiteral(" (reversed)") : QString()));
        }
        // Can't trim the end past the media: the ramp uses 4 s of footage by 2.5 s.
        Clip c = snapFor(video, rampUp).timeline.tracks[0].clips[0];
        const TimeUs maxLen = ops::maxClipLength(video.get(), c);
        check(std::abs(c.rampOffset(double(maxLen)) - double(video->info.duration)) < 5e3,
              QStringLiteral("longest ramped event ends at the end of the media (%1 s)").arg(maxLen / 1e6, 0, 'f', 3));
    }

    // ---- Audio --------------------------------------------------------------------------------
    {
        // Clicks every 0.25 s in the source; the ramp must play each one when its
        // source time comes up, with and without pitch preservation.
        std::vector<float> clicks(size_t(rate) * 6 * 2, 0.0f);
        std::vector<double> clickTimes;
        for (double t = 0.25; t < 5.8; t += 0.25) {
            clickTimes.push_back(t);
            const size_t at = size_t(t * rate);
            for (size_t i = 0; i < size_t(rate / 100); ++i) // 10 ms 2 kHz burst
                clicks[(at + i) * 2] = clicks[(at + i) * 2 + 1] = 0.8f * float(std::sin(2 * 3.14159265 * 2000 * double(i) / rate));
        }
        const QString clickPath = QDir(outDir).filePath(QStringLiteral("clicks.wav"));
        writeWav(clickPath, clicks, rate);
        auto clickItem = loadItem(clickPath, 3);
        for (bool keepPitch : {true, false}) {
            ProjectSnapshot s = snapFor(clickItem, [&](Clip& c) {
                rampUp(c);
                c.preservePitch = keepPitch;
                c.length = 3 * kUsPerSecond;
            });
            const Clip& c = s.timeline.tracks.back().clips[0];
            const std::vector<float> out = mixRange(s, 0.0, 3.0);
            double worst = 0;
            int found = 0, expected = 0;
            for (double src : clickTimes) {
                const TimeUs want = ops::rampTimeForOffset(c, src * kUsPerSecond, 0, c.length);
                if (want >= c.length - 100000)
                    break;
                ++expected;
                // First sample above the threshold near the expected time.
                const int64_t w = usToSamples(want, rate);
                int64_t hit = -1;
                for (int64_t i = std::max<int64_t>(0, w - rate / 16); i < std::min<int64_t>(int64_t(out.size() / 2), w + rate / 16); ++i)
                    if (std::abs(out[size_t(i) * 2]) > 0.2f) {
                        hit = i;
                        break;
                    }
                if (hit >= 0) {
                    ++found;
                    worst = std::max(worst, std::abs(double(hit - w)) / rate * 1000.0);
                }
            }
            check(found == expected && expected >= 8 && worst < 20,
                  QStringLiteral("%1: %2/%3 clicks in sync along the ramp (worst %4 ms)")
                      .arg(keepPitch ? QStringLiteral("pitch-preserved ramp") : QStringLiteral("tape-style ramp"))
                      .arg(found).arg(expected).arg(worst, 0, 'f', 1));
        }
    }
    {
        // Pitch: around 1 s the ramp plays at 150%.
        ProjectSnapshot keep = snapFor(sine, [&](Clip& c) { rampUp(c); });
        ProjectSnapshot tape = snapFor(sine, [&](Clip& c) {
            rampUp(c);
            c.preservePitch = false;
        });
        const double base = estimateFrequency(mixRange(snapFor(sine, [](Clip&) {}), 0.9, 0.2), rate);
        const double fKeep = estimateFrequency(mixRange(keep, 0.9, 0.2), rate);
        const double fTape = estimateFrequency(mixRange(tape, 0.9, 0.2), rate);
        check(std::abs(fKeep - base) < base * 0.05 && std::abs(fTape - 1.5 * base) < base * 0.06,
              QStringLiteral("at 150%: pitch-preserved %1 Hz, tape-style %2 Hz (source %3 Hz)").arg(fKeep, 0, 'f', 0).arg(fTape, 0, 'f', 0).arg(base, 0, 'f', 0));

        // No dropouts while the tempo keeps changing.
        const std::vector<float> out = mixRange(keep, 0.0, 2.0);
        const double nominal = rms(out, size_t(rate / 2), size_t(rate / 10));
        double lowest = 1e9;
        for (size_t i = size_t(rate / 10); i + rate / 100 < size_t(rate) * 2; i += size_t(rate / 100))
            lowest = std::min(lowest, rms(out, i, size_t(rate / 100)));
        check(lowest > nominal * 0.6, QStringLiteral("continuous audio through the ramp (quietest 10 ms window %1 of nominal)").arg(lowest / nominal, 0, 'f', 2));

        // Freeze: silent while the picture holds.
        ProjectSnapshot frozen = snapFor(sine, [&](Clip& c) {
            c.velocity.active = true;
            c.velocity.points = {key(0, 1.0, Keyframe::Interp::Hold), key(kUsPerSecond, 0.0, Keyframe::Interp::Hold),
                                 key(2 * kUsPerSecond, 1.0)};
        });
        const std::vector<float> f = mixRange(frozen, 0.0, 3.0);
        const double during = rms(f, size_t(rate * 1.3), size_t(rate / 2)), before = rms(f, size_t(rate / 2), size_t(rate / 4)),
                     after = rms(f, size_t(rate * 2.4), size_t(rate / 4));
        check(during < before * 0.05 && after > before * 0.6,
              QStringLiteral("audio is silent during a freeze and resumes after (%1 / %2 / %3)").arg(before, 0, 'f', 3).arg(during, 0, 'f', 4).arg(after, 0, 'f', 3));
    }

    // ---- Groups, save/load, export ---------------------------------------------------------------
    {
        Project proj;
        const int id = proj.addMedia(video->info);
        Timeline tl = proj.timeline();
        ops::insertMedia(tl, *proj.mediaItem(id), 0, -1, proj.settings().fps);
        Clip& v = tl.tracks[0].clips[0];
        ops::applyRampPreset(v, proj.mediaItem(id).get(), ops::RampPreset::SlowMiddle, proj.settings().fps);
        ops::syncGroupRamps(tl, v.id);
        const Clip& a = tl.tracks.back().clips[0];
        check(a.velocity == tl.tracks[0].clips[0].velocity && a.length == tl.tracks[0].clips[0].length,
              QStringLiteral("grouped audio event gets the same ramp"));
        proj.applyEdit(tl, QStringLiteral("test"));
        const QString path = QDir(outDir).filePath(QStringLiteral("ramps.mvp"));
        QString err;
        Project loaded;
        check(proj.save(path, &err) && loaded.load(path, &err) && loaded.timeline() == proj.timeline(),
              QStringLiteral("speed ramps survive save/load %1").arg(err));

        ExportSettings es = settingsFromPreset(exportPresets().front(), loaded.settings());
        es.width = 640;
        es.height = 360;
        es.path = QDir(outDir).filePath(QStringLiteral("ramp.mp4"));
        es.rangeEnd = loaded.timeline().duration();
        Exporter exporter;
        const bool ok = exporter.run(*loaded.snapshot(), es);
        const MediaInfo back = ok ? probeMedia(es.path) : MediaInfo{};
        check(ok && back.ok() && std::llabs(back.duration - loaded.timeline().duration()) < 100000,
              QStringLiteral("export with a ramp: %1 s (timeline %2 s) %3")
                  .arg(back.duration / 1e6, 0, 'f', 2).arg(loaded.timeline().duration() / 1e6, 0, 'f', 2).arg(exporter.error()));
    }
    return failures;
}

int cmdRacks(const QString& mediaDir, const QString& outDir)
{
    QDir().mkpath(outDir);
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    const QDir dir(mediaDir);
    auto video = loadItem(dir.filePath(QStringLiteral("h264_aac.mp4")), 1);
    auto sine = loadItem(dir.filePath(QStringLiteral("audio_sine_loud.wav")), 2);
    if (!video || !sine) {
        print(QStringLiteral("FAIL  test media missing"));
        return 1;
    }
    int nextId = 1000;
    auto alloc = [&] { return nextId++; };
    auto fx = [&](const char* type) { return makeEffectOfType(QString::fromLatin1(type), alloc()); };
    auto chainOf = [&](bool audio, EffectChain effects, double level, int blend = 0) {
        RackChain c = makeRackChain(audio, QStringLiteral("c"), std::move(effects));
        c.params[QStringLiteral("level")].value = level;
        if (!audio)
            c.params[QStringLiteral("blend")].value = blend;
        return c;
    };
    auto rackOf = [&](std::vector<RackChain> chains, double mix = 100) {
        EffectInstance r = makeRack(alloc());
        r.chains = std::move(chains);
        r.params[QStringLiteral("mix")].value = mix;
        return r;
    };

    // ---- Chain editing -------------------------------------------------------------------------
    {
        EffectChain root{fx("invert"), fx("sepia"), fx("gaussian_blur"), fx("vignette")};
        auto types = [](const EffectChain& c) {
            QStringList t;
            for (const auto& e : c)
                t << (e.isRack() ? QStringLiteral("[%1]").arg(e.chains.empty() ? QString() : [&] {
                    QStringList in;
                    for (const auto& x : e.chains.front().effects)
                        in << x.type;
                    return in.join(QLatin1Char(','));
                }()) : e.type);
            return t.join(QLatin1Char(' '));
        };
        EffectChain c = root;
        moveEffect(c, {}, 3, {}, 0, false, alloc); // vignette to the front
        const bool reorder = types(c) == QStringLiteral("vignette invert sepia gaussian_blur");
        moveEffect(c, {}, 0, {}, 3, false, alloc); // ...and behind blur ("in front of index 3" before the move)
        const bool reorder2 = types(c) == QStringLiteral("invert sepia vignette gaussian_blur");
        const int rack = groupIntoRack(c, {}, {1, 2}, alloc(), false);
        const bool grouped = rack == 1 && types(c) == QStringLiteral("invert [sepia,vignette] gaussian_blur");
        // Move blur into the rack's chain, between sepia and vignette.
        moveEffect(c, {}, 2, ChainPath{}.child(1, 0), 1, false, alloc);
        const bool intoRack = types(c) == QStringLiteral("invert [sepia,gaussian_blur,vignette]");
        // A rack can't go inside itself.
        const bool refused = !moveEffect(c, {}, 1, ChainPath{}.child(1, 0), 0, false, alloc);
        // Copy out of the rack: new ids.
        moveEffect(c, ChainPath{}.child(1, 0), 0, {}, 0, true, alloc);
        std::set<int> ids;
        int count = 0;
        forEachEffect(c, [&](const EffectInstance& e) {
            ids.insert(e.id);
            ++count;
        });
        const bool copied = types(c) == QStringLiteral("sepia invert [sepia,gaussian_blur,vignette]") && int(ids.size()) == count;
        const bool ungrouped = ungroupRack(c, {}, 2) && types(c) == QStringLiteral("sepia invert sepia gaussian_blur vignette");
        check(reorder && reorder2 && grouped && intoRack && refused && copied && ungrouped,
              QStringLiteral("chain editing: reorder, group into a rack, move into / copy out of a rack, ungroup (%1)").arg(types(c)));
    }
    {
        // Nested racks survive save/load; split gives nested effects fresh ids.
        Project proj;
        const int id = proj.addMedia(video->info);
        Timeline tl = proj.timeline();
        ops::insertMedia(tl, *proj.mediaItem(id), 0, -1, proj.settings().fps);
        Clip& c = tl.tracks[0].clips[0];
        EffectInstance inner = rackOf({chainOf(false, {fx("invert")}, 60, 2), chainOf(false, {}, 100)}, 70);
        inner.chains[0].solo = true;
        c.effects.push_back(rackOf({chainOf(false, {fx("sepia"), inner}, 100), chainOf(false, {fx("gaussian_blur")}, 40, 3)}));
        c.effects.front().chains[1].params[QStringLiteral("level")].addKey(kUsPerSecond, 80);
        tl.nextId = nextId; // ids came from the test's allocator
        proj.applyEdit(tl, QStringLiteral("test"));
        const QString path = QDir(outDir).filePath(QStringLiteral("racks.mvp"));
        QString err;
        Project loaded;
        check(proj.save(path, &err) && loaded.load(path, &err) && loaded.timeline() == proj.timeline(),
              QStringLiteral("nested racks survive save/load %1").arg(err));
        Timeline split = proj.timeline();
        ops::splitClips(split, {}, 2 * kUsPerSecond);
        std::set<int> ids;
        int count = 0;
        for (const auto& clip : split.tracks[0].clips)
            forEachEffect(clip.effects, [&](const EffectInstance& e) {
                ids.insert(e.id);
                ++count;
            });
        const auto& right = split.tracks[0].clips[1].effects.front();
        check(int(ids.size()) == count && count == 10 && right.chains[1].params.at(QStringLiteral("level")).keys.front().time == -kUsPerSecond,
              QStringLiteral("split: nested effects get fresh ids, rack keyframes move with the event"));
    }
    {
        // Pan/Crop keyframes count from the event's start: a split piece and a multicam piece keep the motion in place.
        const TimeUs S = kUsPerSecond;
        Timeline tl = defaultTimeline();
        for (int i = 0; i < 2; ++i) {
            Clip c;
            c.id = tl.allocId();
            c.length = 10 * S;
            c.generator.type = QStringLiteral("solid");
            c.panCrop[QStringLiteral("zoom")].addKey(4 * S, 2.0);
            c.effects.push_back(fx("gaussian_blur"));
            c.effects.back().id = tl.allocId();
            c.effects.back().params[QStringLiteral("radius")].addKey(4 * S, 5.0);
            tl.tracks[0].clips.push_back(c);
        }
        const int a1 = tl.tracks[0].clips[0].id, a2 = tl.tracks[0].clips[1].id;
        Timeline split = tl;
        ops::splitClips(split, {a1}, 1 * S);
        const Clip* right = nullptr;
        for (const Clip& c : split.tracks[0].clips)
            if (c.start == 1 * S)
                right = &c;
        const bool splitOk = right && right->panCrop.at(QStringLiteral("zoom")).keys.front().time == 3 * S;
        QString err;
        bool cutOk = multicam::create(tl, {}, {a1, a2}, &err) && multicam::cut(tl, {}, 2, 3 * S, &err);
        const int mi = multicam::trackIndex(tl);
        const Clip* piece = mi >= 0 && tl.tracks[size_t(mi)].clips.size() == 2 ? &tl.tracks[size_t(mi)].clips[1] : nullptr;
        cutOk = cutOk && piece && piece->panCrop.at(QStringLiteral("zoom")).keys.front().time == 1 * S
                && piece->effects.front().params.at(QStringLiteral("radius")).keys.front().time == 1 * S;
        check(splitOk && cutOk, QStringLiteral("split and multicam pieces keep Pan/Crop and effect keyframes in place (split %1, multicam %2 %3)")
                                    .arg(splitOk).arg(cutOk).arg(err));
    }

    // ---- Video -----------------------------------------------------------------------------------
    const QSize size(640, 360);
    const FrameRate fps{30, 1};
    auto snapWith = [&](const std::shared_ptr<MediaItem>& item, EffectChain effects) {
        ProjectSnapshot s;
        s.settings.fps = fps;
        s.settings.width = size.width();
        s.settings.height = size.height();
        s.media[item->id] = item;
        s.timeline = defaultTimeline();
        ops::insertMedia(s.timeline, *item, 0, -1, fps);
        s.timeline.tracks[0].clips[0].effects = std::move(effects);
        return s;
    };
    {
        Compositor cpu(true);
        const TimeUs t = kUsPerSecond;
        const QImage dry = cpu.render(snapWith(video, {}), t, size);
        const QImage inverted = cpu.render(snapWith(video, {fx("invert")}), t, size);
        const QImage same = cpu.render(snapWith(video, {rackOf({chainOf(false, {fx("invert")}, 100)})}), t, size);
        check(imageDifference(same, inverted) < 0.01, QStringLiteral("a one-chain rack equals its effects in series"));

        QImage half(size, QImage::Format_ARGB32_Premultiplied);
        imageops::mix(half, &dry, &inverted, 0.5f);
        const QImage opacity = cpu.render(snapWith(video, {rackOf({chainOf(false, {fx("invert")}, 50)})}), t, size);
        const QImage drywet = cpu.render(snapWith(video, {rackOf({chainOf(false, {fx("invert")}, 100)}, 50)}), t, size);
        check(imageDifference(opacity, half) < 1.5 && imageDifference(drywet, half) < 1.5,
              QStringLiteral("chain opacity 50% and rack dry/wet 50% both give half the effect (%1, %2)")
                  .arg(imageDifference(opacity, half), 0, 'f', 2).arg(imageDifference(drywet, half), 0, 'f', 2));

        // Two chains: the second is layered over the first with its blend mode.
        const QRgb d = dry.pixel(100, 180), inv = inverted.pixel(100, 180);
        const QImage multiply = cpu.render(snapWith(video, {rackOf({chainOf(false, {}, 100), chainOf(false, {fx("invert")}, 100, 3)})}), t, size);
        const QRgb m = multiply.pixel(100, 180);
        const bool mulOk = std::abs(qRed(m) - qRed(d) * qRed(inv) / 255) <= 2 && std::abs(qGreen(m) - qGreen(d) * qGreen(inv) / 255) <= 2;
        const QImage soloed = [&] {
            ProjectSnapshot s = snapWith(video, {rackOf({chainOf(false, {fx("sepia")}, 100), chainOf(false, {fx("invert")}, 100)})});
            s.timeline.tracks[0].clips[0].effects[0].chains[1].solo = true;
            return cpu.render(s, t, size);
        }();
        const QImage off = [&] {
            ProjectSnapshot s = snapWith(video, {rackOf({chainOf(false, {fx("invert")}, 100)})});
            s.timeline.tracks[0].clips[0].effects[0].chains[0].enabled = false;
            return cpu.render(s, t, size);
        }();
        check(mulOk && imageDifference(soloed, inverted) < 0.01 && imageDifference(off, dry) < 0.01,
              QStringLiteral("parallel chains: multiply blend, solo, chain bypass"));

        // GPU renders every blend mode, nesting and dry/wet like the CPU.
        auto surface = createGpuSurface();
        QString err;
        if (auto gpu = surface ? GpuCompositor::create(surface.get(), true, &err) : nullptr) {
            double worst = 0;
            for (int mode = 0; mode < 8; ++mode) {
                const ProjectSnapshot s = snapWith(video, {rackOf({chainOf(false, {fx("sepia")}, 100),
                                                                   chainOf(false, {fx("invert"), fx("vignette")}, 70, mode)}, 80)});
                worst = std::max(worst, imageDifference(cpu.render(s, t, size), gpu->render(s, t, size)));
            }
            const EffectInstance inner = rackOf({chainOf(false, {fx("invert")}, 60, 2), chainOf(false, {fx("black_white")}, 100, 7)}, 70);
            const ProjectSnapshot nested = snapWith(video, {fx("sepia"), rackOf({chainOf(false, {inner}, 100), chainOf(false, {fx("gaussian_blur")}, 40)})});
            worst = std::max(worst, imageDifference(cpu.render(nested, t, size), gpu->render(nested, t, size)));
            check(worst < 1.5, QStringLiteral("GPU racks match the CPU (8 blend modes + nesting, worst difference %1)").arg(worst, 0, 'f', 2));
        }
    }

    // ---- Audio -------------------------------------------------------------------------------------
    const int rate = 48000;
    auto mixRange = [&](const ProjectSnapshot& s, double from, double seconds) {
        AudioMixer mixer(rate);
        const int total = int(seconds * rate);
        std::vector<float> out(size_t(total) * 2), chunk(1024 * 2);
        for (int i = 0; i < total; i += 1024) {
            const int n = std::min(1024, total - i);
            mixer.mix(s, int64_t(from * rate) + i, n, chunk.data());
            std::copy_n(chunk.begin(), n * 2, out.begin() + i * 2);
        }
        return out;
    };
    auto rmsDb = [](const std::vector<float>& s) {
        double sum = 0;
        for (size_t i = 0; i < s.size(); i += 2)
            sum += double(s[i]) * s[i];
        return 10 * std::log10(std::max(sum / double(s.size() / 2), 1e-20));
    };
    auto audioSnap = [&](const std::shared_ptr<MediaItem>& item, EffectChain effects) {
        ProjectSnapshot s;
        s.settings.fps = fps;
        s.media[item->id] = item;
        s.timeline = defaultTimeline();
        ops::insertMedia(s.timeline, *item, 0, -1, fps);
        s.timeline.tracks.back().clips[0].effects = std::move(effects);
        return s;
    };
    {
        const double unity = rmsDb(mixRange(audioSnap(sine, {}), 0.5, 0.5));
        const double two = rmsDb(mixRange(audioSnap(sine, {rackOf({chainOf(true, {}, 0), chainOf(true, {}, 0)})}), 0.5, 0.5));
        const double halfEach = rmsDb(mixRange(audioSnap(sine, {rackOf({chainOf(true, {}, -6.0206), chainOf(true, {}, -6.0206)})}), 0.5, 0.5));
        const double dryWet = rmsDb(mixRange(audioSnap(sine, {rackOf({chainOf(true, {}, -60)}, 50)}), 0.5, 0.5));
        check(std::abs(two - unity - 6.02) < 0.1 && std::abs(halfEach - unity) < 0.1 && std::abs(dryWet - unity + 6.02) < 0.1,
              QStringLiteral("audio chains are summed: 2 x 0 dB = %1 dB, 2 x -6 dB = %2 dB, dry/wet 50% of silence = %3 dB")
                  .arg(two - unity, 0, 'f', 2).arg(halfEach - unity, 0, 'f', 2).arg(dryWet - unity, 0, 'f', 2));

        // A chain with lookahead (limiter) and an empty one stay lined up:
        // a click comes out once, on time, at double height.
        std::vector<float> click(size_t(rate) * 2 * 2, 0.0f);
        for (int i = 0; i < 48; ++i)
            click[size_t(rate + i) * 2] = click[size_t(rate + i) * 2 + 1] = 0.3f;
        const QString clickPath = QDir(outDir).filePath(QStringLiteral("click.wav"));
        writeWav(clickPath, click, rate);
        auto clickItem = loadItem(clickPath, 3);
        EffectInstance limiter = fx("a_limiter");
        const std::vector<float> out = mixRange(audioSnap(clickItem, {rackOf({chainOf(true, {limiter}, 0), chainOf(true, {}, 0)})}), 0.0, 1.5);
        int first = -1;
        float peak = 0;
        for (size_t i = 0; i < out.size() / 2; ++i) {
            if (first < 0 && std::abs(out[i * 2]) > 0.05f)
                first = int(i);
            peak = std::max(peak, std::abs(out[i * 2]));
        }
        check(first == rate && std::abs(peak - 0.6f) < 0.02f,
              QStringLiteral("rack delay compensation: limiter chain + dry chain give one click at %1 ms, peak %2")
                  .arg(first * 1000.0 / rate, 0, 'f', 2).arg(peak, 0, 'f', 3));
    }
    return failures;
}

int cmdEditing(const QString& mediaDir, const QString& outDir)
{
    QDir().mkpath(outDir);
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    const QDir dir(mediaDir);
    auto video = loadItem(dir.filePath(QStringLiteral("h264_aac.mp4")), 1);
    if (!video) {
        print(QStringLiteral("FAIL  test media missing"));
        return 1;
    }
    const FrameRate fps{30, 1};
    const TimeUs S = kUsPerSecond;
    {
        Timeline tl = defaultTimeline();
        ops::insertMedia(tl, *video, 0, 0, fps);
        const Timeline original = tl;
        const auto first = ops::insertMedia(tl, *video, S, 1, fps, true);
        const auto second = ops::insertMedia(tl, *video, 2 * S, 0, fps, true);
        bool kept = tl.tracks.size() == original.tracks.size() + 4;
        for (const Track& track : original.tracks) {
            const int index = tl.trackIndex(track.id);
            kept &= index >= 0 && tl.tracks[size_t(index)] == track;
        }
        check(kept && first.size() == 2 && second.size() == 2,
              QStringLiteral("new-track video placement keeps overlapping existing events and creates separate tracks for each file"));
        const Clip* a = first.empty() ? nullptr : tl.findClip(first.front());
        const Clip* b = second.empty() ? nullptr : tl.findClip(second.front());
        check(a && b && a->start == S && b->start == 2 * S && a->groupId && b->groupId && a->groupId != b->groupId,
              QStringLiteral("each new-track video has its own video/audio group at the requested time"));
        check(tl.tracks[0].name == QStringLiteral("Clip3-Video") && tl.tracks[1].name == QStringLiteral("Clip3-Audio")
                  && tl.tracks[2].name == QStringLiteral("Clip2-Video") && tl.tracks[3].name == QStringLiteral("Clip2-Audio")
                  && tl.tracks[1].type == TrackType::Audio && tl.tracks[3].type == TrackType::Audio,
              QStringLiteral("new video/audio pairs stay adjacent with matching names even when video and audio track counts differ"));
        MediaItem silent = *video;
        silent.info.audio.clear();
        const size_t count = tl.tracks.size();
        const auto silentIds = ops::insertMedia(tl, silent, 3 * S, -1, fps, true);
        check(silentIds.size() == 1 && tl.tracks.size() == count + 1 && tl.tracks.front().type == TrackType::Video
                  && tl.tracks.front().name == QStringLiteral("Clip4-Video")
                  && tl.tracks.front().clips.front().groupId == 0 && timelineProblem(tl).isEmpty(),
              QStringLiteral("a video without audio creates only a new video track"));
        tl.tracks[2].name = QStringLiteral("Clip10-Audio");
        ops::insertMedia(tl, silent, 4 * S, -1, fps, true);
        check(tl.tracks.front().name == QStringLiteral("Clip11-Video"),
              QStringLiteral("clip numbering continues past saved track names, including an audio track without its video partner"));
    }
    {
        const QSize size(64, 48);
        const QColor color(0xe0, 0x50, 0x20);
        QImage image(size, QImage::Format_RGB32);
        image.fill(color);
        const QString imagePath = QDir(outDir).filePath(QStringLiteral("image.png"));
        image.save(imagePath);
        Project project;
        ProjectSettings settings;
        settings.width = size.width();
        settings.height = size.height();
        project.setSettings(settings);
        const int imageId = project.addMedia(probeMedia(imagePath));
        const int videoId = project.addMedia(video->info);
        Timeline timeline = project.timeline();
        ops::insertMedia(timeline, *project.mediaItem(videoId), 0, 0, fps);
        const size_t originalCount = timeline.tracks.size();
        const auto first = ops::insertMedia(timeline, *project.mediaItem(imageId), 0, 0, fps, true);
        ops::insertMedia(timeline, *project.mediaItem(imageId), S, 0, fps, true);
        check(timeline.tracks.size() == originalCount + 2 && timeline.tracks[0].type == TrackType::Image
                  && timeline.tracks[0].name == QStringLiteral("Image 2") && timeline.tracks[1].type == TrackType::Image
                  && timeline.tracks[1].name == QStringLiteral("Image 1") && timelineProblem(timeline).isEmpty(),
              QStringLiteral("each still image gets a distinct Image track and no extra audio track"));
        project.applyEdit(timeline, QStringLiteral("Image tracks"));
        const QString projectPath = QDir(outDir).filePath(QStringLiteral("image-tracks.mvp"));
        QString error;
        Project loaded;
        check(project.save(projectPath, &error) && loaded.load(projectPath, &error) && loaded.timeline() == timeline,
              QStringLiteral("Image track types and names survive project save/load %1").arg(error));
        ProjectSnapshot snapshot = *loaded.snapshot();
        Compositor cpu;
        const QImage frame = cpu.render(snapshot, S / 2, size);
        check(!frame.isNull() && frame.pixelColor(32, 24) == color,
              QStringLiteral("Image tracks render over existing video tracks"));
        if (!first.empty()) {
            if (Clip* clip = snapshot.timeline.findClip(first.front()))
                clip->effects.push_back(makeEffectOfType(QStringLiteral("invert"), snapshot.timeline.allocId()));
        }
        const QImage effectFrame = cpu.render(snapshot, S / 2, size);
        check(imageDifference(frame, effectFrame) > 50,
              QStringLiteral("Image track events process visual effects"));
        const auto surface = createGpuSurface();
        auto gpu = createFrameRenderer(true, surface.get());
        if (gpu->isGpu())
            check(imageDifference(effectFrame, gpu->render(snapshot, S / 2, size)) < 1.5,
                  QStringLiteral("GPU and CPU rendering agree on Image tracks with effects"));
        ExportSettings exportSettings = settingsFromPreset(exportPresets().front(), settings);
        exportSettings.width = size.width();
        exportSettings.height = size.height();
        exportSettings.path = QDir(outDir).filePath(QStringLiteral("image-track-export.mp4"));
        exportSettings.rangeStart = S / 2;
        exportSettings.rangeEnd = S;
        Exporter exporter;
        const bool exported = exporter.run(*loaded.snapshot(), exportSettings, {}, surface.get());
        auto exportedMedia = exported ? loadItem(exportSettings.path, 99) : nullptr;
        Compositor checkExport;
        ProjectSnapshot exportedSnapshot;
        exportedSnapshot.settings = settings;
        exportedSnapshot.timeline = defaultTimeline();
        if (exportedMedia) {
            exportedSnapshot.media[exportedMedia->id] = exportedMedia;
            ops::insertMedia(exportedSnapshot.timeline, *exportedMedia, 0, 0, fps);
        }
        const QImage exportedFrame = exportedMedia ? checkExport.render(exportedSnapshot, 0, size) : QImage{};
        check(exported && !exportedFrame.isNull() && imageDifference(frame, exportedFrame) < 5,
              QStringLiteral("exported video includes Image tracks %1").arg(exporter.error()));
    }
    // ---- Scene changes: black, then moving test video, then blue -------------------------------
    {
        ProjectSnapshot snap;
        snap.settings.width = 640;
        snap.settings.height = 360;
        snap.settings.fps = fps;
        snap.timeline = defaultTimeline();
        snap.media[video->id] = video;
        auto solid = [&](const char* preset, TimeUs at) {
            Clip c;
            c.id = snap.timeline.allocId();
            c.start = at;
            c.length = S;
            c.generator = makeGenerator(*findGeneratorPreset(QString::fromLatin1(preset)));
            snap.timeline.tracks[0].clips.push_back(c);
        };
        solid("solid_black", 0);
        for (int id : ops::insertMedia(snap.timeline, *video, S, 0, fps))
            snap.timeline.findClip(id)->length = 2 * S;
        solid("solid_blue", 3 * S);
        snap.timeline.sortClips();
        ExportSettings s = settingsFromPreset(exportPresets().front(), snap.settings);
        s.path = QDir(outDir).filePath(QStringLiteral("scenes.mp4"));
        s.rangeStart = 0;
        s.rangeEnd = 4 * S;
        Exporter exporter;
        const bool exported = exporter.run(snap, s, {});
        const auto shots = exported ? loadItem(s.path, 98) : nullptr;
        std::vector<TimeUs> cuts;
        if (shots)
            cuts = scenes::detect(*shots, shots->info.video.front().index, 0, shots->info.duration, 0.5);
        QStringList found;
        for (TimeUs t : cuts)
            found << QString::number(double(t) / S, 'f', 3);
        const TimeUs frame = fps.frameDuration();
        check(cuts.size() == 2 && std::llabs(cuts[0] - S) <= frame && std::llabs(cuts[1] - 3 * S) <= frame,
              QStringLiteral("scene detection finds the two cuts and nothing in the moving shot (%1) %2").arg(found.join(QStringLiteral(", ")), exporter.error()));
    }
    auto starts = [](const Track& t) {
        QStringList out;
        for (const Clip& c : t.clips)
            out << QString::number(double(c.start) / kUsPerSecond, 'f', 1);
        return out.join(QLatin1Char(' '));
    };
    // ---- Cutting time out: silences, struck words, ranges ----------------------------------------
    {
        // 1 s tone, 1.2 s silence, 1 s tone, 0.3 s pause, 1 s tone (16 kHz), starting at 10 s.
        std::vector<float> mono;
        auto tone = [&](double s) { for (int i = 0; i < int(s * 16000); ++i) mono.push_back(float(0.3 * std::sin(i * 0.2))); };
        auto quiet = [&](double s) { mono.insert(mono.end(), size_t(s * 16000), 0.0005f); };
        tone(1); quiet(1.2); tone(1); quiet(0.3); tone(1);
        const auto gaps = cuts::silences(mono, 16000, 10 * S, -40, S / 2, S / 10);
        check(gaps.size() == 1 && std::llabs(gaps[0].from - (11 * S + S / 10)) <= 20000 && std::llabs(gaps[0].to - (12 * S + S / 5 - S / 10)) <= 20000,
              QStringLiteral("silences: the 1.2 s gap (less 0.1 s each side) and not the 0.3 s pause"));
        const std::vector<Caption> words{{0, S / 2, QStringLiteral("so"), 0}, {S / 2, S, QStringLiteral("um"), 0},
                                         {S + S / 5, 2 * S, QStringLiteral("like"), 0}, {2 * S, 3 * S, QStringLiteral("hello"), 0}};
        check(cuts::wordCuts(words, {false, true, true, false}) == std::vector<cuts::Range>{{S / 2, 2 * S}}
                  && cuts::wordCuts(words, {false, false, false, true}) == std::vector<cuts::Range>{{2 * S, 3 * S}},
              QStringLiteral("struck words cut from the first one to the next kept word, or to the last one's end"));
        Timeline tl = defaultTimeline();
        tl.nextId = 100;
        auto event = [&](int track, TimeUs start, TimeUs length) {
            Clip c;
            c.id = tl.allocId();
            c.start = start;
            c.length = length;
            c.generator = makeGenerator(*findGeneratorPreset(QStringLiteral("solid_black")));
            tl.tracks[size_t(track)].clips.push_back(c);
        };
        event(0, 0, 10 * S);
        for (bool locked : {false, true}) { // a second video track, and a locked one
            tl.tracks.push_back(tl.tracks[0]);
            tl.tracks.back().id = tl.allocId();
            tl.tracks.back().clips.front().id = tl.allocId();
            tl.tracks.back().locked = locked;
        }
        tl.markers.push_back(Marker{tl.allocId(), 3 * S, 0, QStringLiteral("inside"), 0});
        tl.markers.push_back(Marker{tl.allocId(), 8 * S, 0, QStringLiteral("after"), 0});
        tl.chapters.append(QJsonObject{{"id", 1}, {"start", "0"}, {"narration", QString::number(3 * S)}, {"listening", QString::number(2 * S)},
                                       {"clips", QJsonArray{tl.tracks[0].clips[0].id}}});
        tl.chapters.append(QJsonObject{{"id", 2}, {"start", QString::number(5 * S)}, {"narration", QString::number(3 * S)}, {"listening", QString::number(2 * S)}});
        const TimeUs removed = cuts::removeRanges(tl, {{6 * S, 7 * S}, {2 * S, 4 * S}, {3 * S, 5 * S}}, fps);
        auto chapter = [&](int i, const char* key) { return tl.chapters[i].toObject().value(QLatin1String(key)).toString().toLongLong(); };
        check(chapter(0, "start") == 0 && chapter(0, "narration") == 2 * S && chapter(0, "listening") == 0 && chapter(1, "start") == 2 * S
                  && chapter(1, "narration") == 2 * S && chapter(1, "listening") == 2 * S,
              QStringLiteral("cut time moves story chapters back and shrinks the ones it cuts into"));
        check(tl.chapters[0].toObject()["clips"].toArray().size() == 3,
              QStringLiteral("the pieces cut from a chapter's event stay in that chapter (%1)").arg(tl.chapters[0].toObject()["clips"].toArray().size()));
        check(removed == 4 * S && starts(tl.tracks[0]) == QStringLiteral("0.0 2.0 3.0") && tl.tracks[0].clips.back().end() == 6 * S
                  && starts(tl.tracks[2]) == QStringLiteral("0.0 2.0 3.0") && tl.tracks.back().clips.size() == 1 && tl.duration() == 10 * S
                  && tl.markers[0].time == 2 * S
                  && tl.markers[1].time == 4 * S && timelineProblem(tl).isEmpty(),
              QStringLiteral("removing ranges merges overlaps, closes the gaps on unlocked tracks and moves markers (%1)").arg(starts(tl.tracks[0])));
    }
    {
        // Insert Time: 2 s of empty time at 3 s on the unlocked tracks; markers, a region across it and chapters move or grow.
        Timeline tl = defaultTimeline();
        tl.nextId = 100;
        Clip c;
        c.id = tl.allocId();
        c.length = 10 * S;
        c.generator = makeGenerator(*findGeneratorPreset(QStringLiteral("solid_black")));
        tl.tracks[0].clips.push_back(c);
        tl.tracks.push_back(tl.tracks[0]);
        tl.tracks.back().id = tl.allocId();
        tl.tracks.back().clips.front().id = tl.allocId();
        tl.tracks.back().locked = true;
        tl.markers.push_back(Marker{tl.allocId(), 1 * S, 0, QStringLiteral("before"), 0});
        tl.markers.push_back(Marker{tl.allocId(), 5 * S, 0, QStringLiteral("after"), 0});
        tl.markers.push_back(Marker{tl.allocId(), 2 * S, 2 * S, QStringLiteral("across"), 0});
        tl.chapters.append(QJsonObject{{"id", 1}, {"start", "0"}, {"narration", QString::number(4 * S)}, {"listening", QString::number(1 * S)},
                                       {"clips", QJsonArray{tl.tracks[0].clips[0].id}}});
        tl.chapters.append(QJsonObject{{"id", 2}, {"start", QString::number(6 * S)}, {"narration", QString::number(2 * S)}, {"listening", "0"}});
        const TimeUs added = cuts::insertTime(tl, 3 * S, 2 * S, fps);
        const Marker* across = nullptr;
        for (const Marker& m : tl.markers)
            if (m.name == QLatin1String("across"))
                across = &m;
        auto chapter = [&](int i, const char* key) { return tl.chapters[i].toObject().value(QLatin1String(key)).toString().toLongLong(); };
        check(added == 2 * S && starts(tl.tracks[0]) == QStringLiteral("0.0 5.0") && tl.tracks[0].clips.back().end() == 12 * S
                  && tl.tracks.back().clips.size() == 1 && tl.tracks.back().clips[0].end() == 10 * S && tl.markers.front().time == 1 * S
                  && across && across->time == 2 * S && across->length == 4 * S && tl.markers.back().time == 7 * S
                  && chapter(0, "narration") == 6 * S && chapter(1, "start") == 8 * S
                  && tl.chapters[0].toObject()["clips"].toArray().size() == 2 && timelineProblem(tl).isEmpty(),
              QStringLiteral("insert time opens a gap on unlocked tracks; markers, a region across it and chapters move or grow (%1)")
                  .arg(starts(tl.tracks[0])));
    }
    // ---- Auto Reframe: finding the subject, and the crop's path ----------------------------------
    {
        auto square = [](double x) {
            QImage img(256, 144, QImage::Format_RGB32);
            img.fill(Qt::black);
            QPainter p(&img);
            p.fillRect(QRectF(x * 256 - 12, 60, 24, 24), Qt::white);
            return img;
        };
        const reframe::Sample moving = reframe::locate(square(0.8), square(0.7), {});
        QImage mask(256, 144, QImage::Format_Grayscale8);
        mask.fill(0);
        QPainter(&mask).fillRect(QRect(60, 20, 30, 120), Qt::white);
        const reframe::Sample person = reframe::locate(square(0.8), square(0.7), mask);
        const reframe::Sample still = reframe::locate(square(0.5), square(0.5), {});
        check(moving.weight > 0 && moving.x > 0.68 && moving.x < 0.82 && std::abs(person.x - 75.0 / 256) < 0.01 && still.weight == 0,
              QStringLiteral("reframe finds the person, else what moves (%1, %2)").arg(moving.x, 0, 'f', 3).arg(person.x, 0, 'f', 3));
        const double visible = reframe::visibleWidth(16.0 / 9, 9.0 / 16);
        std::vector<reframe::Sample> walk, stand;
        for (int i = 0; i <= 32; ++i) {
            walk.push_back({i * S / 8, 0.15 + 0.7 * i / 32.0, 1});
            stand.push_back({i * S / 8, 0.5 + (i % 2 ? 0.02 : -0.02), 1});
        }
        const auto walking = reframe::path(walk, visible), standing = reframe::path(stand, visible);
        bool inside = true, onward = true;
        for (size_t i = 0; i < walking.size(); ++i) {
            inside = inside && walking[i].value >= visible / 2 - 1e-9 && walking[i].value <= 1 - visible / 2 + 1e-9;
            onward = onward && (i == 0 || walking[i].value >= walking[i - 1].value - 1e-9);
        }
        check(std::abs(visible - 0.31640625) < 1e-6 && inside && onward && walking.back().value > 0.6 && standing.size() <= 2
                  && std::abs(standing.front().value - 0.5) < 0.01,
              QStringLiteral("the crop follows a walk across the picture inside its edges, and holds still for a subject standing still (%1 keys)")
                  .arg(walking.size()));
        {
            Timeline tl = defaultTimeline();
            const auto placed = ops::insertMedia(tl, *video, 0, 0, fps);
            Clip* c = tl.findClip(placed.front());
            c->length = 2 * S;
            c->reverse = true;
            MediaMap media{{video->id, video}};
            const int framed = reframe::apply(tl, media, {c->id}, QSize(1080, 1920));
            const ParamValue& x = tl.findClip(placed.front())->panCrop[QStringLiteral("centerX")];
            check(framed == 1 && x.keys.empty() && x.value > 0.15 && x.value < 0.85,
                  QStringLiteral("a reversed event gets a still crop instead of being skipped (centre %1)").arg(x.value, 0, 'f', 3));
        }
        const auto followed = reframe::follow(*video, video->info.video.front().index, 0, 2 * S, S / 4);
        check(followed.size() == 8 && std::all_of(followed.begin(), followed.end(), [](const reframe::Sample& f) { return f.x >= 0 && f.x <= 1; }),
              QStringLiteral("reframe samples a real video every quarter second (%1 samples)").arg(followed.size()));
    }
    // Three 2 s events on a video track at 0, 3 and 6 s, and on an audio track at 1 s.
    auto layout = [&] {
        Timeline tl = defaultTimeline();
        for (TimeUs at : {TimeUs(0), 3 * S, 6 * S}) {
            Clip c;
            c.id = tl.allocId();
            c.mediaId = video->id;
            c.streamIndex = video->info.video.front().index;
            c.start = at;
            c.length = 2 * S;
            tl.tracks[0].clips.push_back(c);
        }
        Clip a;
        a.id = tl.allocId();
        a.mediaId = video->id;
        a.streamIndex = video->info.audio.front().index;
        a.start = S;
        a.length = 4 * S;
        tl.tracks[1].clips.push_back(a);
        tl.markers.push_back(Marker{tl.allocId(), 7 * S, 0, QStringLiteral("end"), 1});
        return tl;
    };

    // ---- Ripple operations ----
    {
        Timeline tl = layout();
        ops::rippleShift(tl, 3 * S, -S, {tl.tracks[0].id}, {}, false);
        const bool shift = starts(tl.tracks[0]) == QStringLiteral("0.0 2.0 5.0") && starts(tl.tracks[1]) == QStringLiteral("1.0")
                           && tl.markers[0].time == 7 * S;
        Timeline all = layout();
        ops::rippleShift(all, 3 * S, S, {}, {all.tracks[0].clips[1].id}, true);
        const bool allTracks = starts(all.tracks[0]) == QStringLiteral("0.0 3.0 7.0") && all.markers[0].time == 8 * S;
        check(shift && allTracks, QStringLiteral("rippleShift moves events after a point on chosen tracks, or all tracks + markers, except excluded ones"));

        Timeline ins = layout();
        ops::rippleInsert(ins, 2 * S, 2 * S, {}, {}, true);
        // The audio event (1-5 s) is split at 2 s; its right part moves to 4 s.
        check(starts(ins.tracks[0]) == QStringLiteral("0.0 5.0 8.0") && starts(ins.tracks[1]) == QStringLiteral("1.0 4.0")
                  && ins.tracks[1].clips[0].length == S && ins.markers[0].time == 9 * S,
              QStringLiteral("rippleInsert opens a gap, splitting what crosses it (%1 | %2)").arg(starts(ins.tracks[0]), starts(ins.tracks[1])));

        Timeline del = layout();
        ops::rippleRemoveClipsAll(del, {del.tracks[0].clips[1].id}, true);
        check(starts(del.tracks[0]) == QStringLiteral("0.0 4.0") && starts(del.tracks[1]) == QStringLiteral("1.0") && del.markers[0].time == 5 * S,
              QStringLiteral("ripple delete on all tracks closes the 2 s on every track and moves markers (%1)").arg(starts(del.tracks[0])));

        Timeline inside = layout(); // the sound at 4-5 s starts in the deleted 3-5 s; one more at 6 s
        inside.tracks[1].clips[0].start = 4 * S;
        inside.tracks[1].clips[0].length = S;
        Clip after = inside.tracks[1].clips[0];
        after.id = inside.allocId();
        after.start = 6 * S;
        inside.tracks[1].clips.push_back(after);
        ops::rippleRemoveClipsAll(inside, {inside.tracks[0].clips[1].id}, true);
        check(starts(inside.tracks[1]) == QStringLiteral("3.0 4.0"),
              QStringLiteral("ripple delete on all tracks: an event starting in the gap moves to its start, not under the next (%1)")
                  .arg(starts(inside.tracks[1])));

        Timeline xf = layout(); // the 3 s event runs to 6.5 s, over the 6 s one (a crossfade); one more at 9 s
        xf.tracks[0].clips[1].length = 3 * S + S / 2;
        Clip last = xf.tracks[0].clips[2];
        last.id = xf.allocId();
        last.start = 9 * S;
        xf.tracks[0].clips.push_back(last);
        ops::rippleRemoveClips(xf, {xf.tracks[0].clips[1].id, xf.tracks[0].clips[2].id});
        check(starts(xf.tracks[0]) == QStringLiteral("0.0 4.0"),
              QStringLiteral("ripple delete of crossfaded events closes the 5 s they cover, the overlap once (%1)").arg(starts(xf.tracks[0])));

        Timeline locked = layout();
        locked.tracks[1].locked = true; // the audio event, 1-5 s
        ops::rippleRemoveClipsAll(locked, {locked.tracks[0].clips[1].id}, true);
        ops::rippleInsert(locked, 0, S, {}, {}, false);
        const bool notSplit = ops::splitClips(locked, {}, 4 * S).empty();
        check(notSplit && starts(locked.tracks[0]) == QStringLiteral("1.0 5.0") && starts(locked.tracks[1]) == QStringLiteral("1.0")
                  && nestedTimelineFromJson(nestedTimelineToJson(locked)).tracks[1].locked,
              QStringLiteral("a locked track keeps its events through ripples and Split, and stays locked when saved (%1 | %2)")
                  .arg(starts(locked.tracks[0]), starts(locked.tracks[1])));

        // A still asked onto a video track goes there (not onto a new Image track at the top, which moved every index).
        MediaItem still;
        still.id = 99;
        still.info.isStill = true;
        still.info.duration = 5 * S;
        still.info.video.push_back(VideoStreamInfo{});
        Timeline pic = layout();
        const size_t tracksBefore = pic.tracks.size();
        const auto placed = ops::insertMedia(pic, still, 9 * S, 0, FrameRate{30, 1});
        const Clip* onVideo = pic.tracks[0].clips.empty() ? nullptr : &pic.tracks[0].clips.back();
        check(placed.size() == 1 && pic.tracks.size() == tracksBefore && onVideo && onVideo->id == placed[0],
              QStringLiteral("a still placed on a chosen video track stays on it (%1 tracks, was %2)").arg(pic.tracks.size()).arg(tracksBefore));

        // Removing transitions: a fade-out, and a crossfade seen from either event.
        Timeline tr = layout();
        tr.tracks[0].clips[2].fadeOut = S / 2;
        tr.tracks[0].clips[2].transitionOut = TransitionSpec{QStringLiteral("iris"), {}};
        tr.tracks[0].clips[1].start = 3 * S / 2; // overlaps the first event by 0.5 s
        const int second = tr.tracks[0].clips[1].id, first = tr.tracks[0].clips[0].id, third = tr.tracks[0].clips[2].id;
        const bool before = ops::hasTransition(tr, first, false) && ops::hasTransition(tr, second, true) && !ops::hasTransition(tr, second, false);
        const bool out = ops::removeTransition(tr, third, false) && tr.tracks[0].clips[2].fadeOut == 0 && tr.tracks[0].clips[2].transitionOut.type.isEmpty();
        const bool cross = ops::removeTransition(tr, first, false) && tr.tracks[0].clips[0].end() == 3 * S / 2 && tr.tracks[0].clips[1].length == 2 * S;
        const bool none = !ops::removeTransition(tr, second, true) && !ops::hasTransition(tr, first, false);
        check(before && out && cross && none,
              QStringLiteral("removeTransition: a fade goes; a crossfade (from either side) cuts the earlier event where the later starts"));

        Timeline mv = layout();
        const int audioId = mv.tracks[1].id;
        ops::moveTrack(mv, 1, 0);
        check(mv.tracks[0].id == audioId && mv.tracks[1].type == TrackType::Video, QStringLiteral("moveTrack reorders tracks"));
    }

    // ---- Save / load: markers, regions, blend modes ----
    const QString projectPath = QDir(outDir).filePath(QStringLiteral("editing.mvp"));
    {
        Project proj;
        const int id = proj.addMedia(video->info);
        Timeline tl = proj.timeline();
        ops::insertMedia(tl, *proj.mediaItem(id), 0, -1, fps);
        tl.markers = {Marker{tl.allocId(), S, 0, {}, 0}, Marker{tl.allocId(), 2 * S, 3 * S, QStringLiteral("Chorus"), 2}};
        tl.tracks[0].blend = 2;
        tl.tracks[0].matte = MatteLuma;
        tl.tracks[0].clips[0].denoise = 0.65;
        proj.applyEdit(tl, QStringLiteral("t"));
        QString error;
        proj.save(projectPath, &error);
        Project back;
        back.load(projectPath, &error);
        const auto& m = back.timeline().markers;
        check(m.size() == 2 && m[1].isRegion() && m[1].name == QStringLiteral("Chorus") && m[1].length == 3 * S && m[1].color == 2
                  && back.timeline().tracks[0].blend == 2 && back.timeline().nextId > m[1].id && back.timeline().tracks[0].matte == MatteLuma
                  && back.timeline().tracks[0].clips[0].denoise == 0.65,
              QStringLiteral("markers, regions, track blend modes, track mattes and noise reduction are saved with the project"));

        // Autosave copy + recovery: the copy keeps the project's file name.
        const QString backup = QDir(outDir).filePath(QStringLiteral("autosave-copy.mvp"));
        Timeline more = back.timeline();
        more.markers.push_back(Marker{more.allocId(), 9 * S, 0, QStringLiteral("unsaved"), 0});
        more.sortMarkers();
        back.applyEdit(more, QStringLiteral("t"));
        back.saveBackup(backup, &error);
        const bool unchanged = back.filePath() == projectPath && back.isModified();
        Project rec;
        const bool ok = rec.recover(backup, &error);
        check(unchanged && ok && rec.filePath() == projectPath && rec.isModified() && rec.timeline().markers.size() == 3
                  && rec.mediaItem(id) && rec.mediaItem(id)->info.ok(),
              QStringLiteral("saveBackup leaves the project as it was; recover reopens it as editing.mvp, unsaved"));
    }

    // ---- Relink ----
    {
        const QString moved = QDir(outDir).filePath(QStringLiteral("moved.mp4"));
        QFile::remove(moved);
        QFile::copy(video->path, moved);
        Project proj;
        MediaInfo info = video->info;
        const int id = proj.addMedia(info);
        Timeline tl = proj.timeline();
        ops::insertMedia(tl, *proj.mediaItem(id), 0, -1, fps);
        for (auto& t : tl.tracks)
            for (auto& c : t.clips)
                c.streamIndex = 77; // a stream the new file doesn't have
        proj.applyEdit(tl, QStringLiteral("t"));
        const int revision = proj.mediaItem(id)->revision;
        QString error;
        const bool bad = !proj.relinkMedia(id, QDir(outDir).filePath(QStringLiteral("nothing.mp4")), &error);
        const bool ok = proj.relinkMedia(id, moved, &error);
        bool streams = true;
        for (const auto& t : proj.timeline().tracks)
            for (const auto& c : t.clips)
                streams &= t.type == TrackType::Video ? proj.mediaItem(id)->info.videoStream(c.streamIndex) != nullptr
                                                      : proj.mediaItem(id)->info.audioStream(c.streamIndex) != nullptr;
        check(bad && ok && proj.mediaItem(id)->path == moved && proj.mediaItem(id)->revision == revision + 1 && streams,
              QStringLiteral("relinkMedia: refuses a missing file, points events at the new one, fixes their streams"));
        // A renderer that had the old file open shows the new one.
        Compositor cpu(true);
        const QSize size(320, 180);
        const QImage first = cpu.render(*proj.snapshot(), S, size);
        QImage black(size, QImage::Format_ARGB32);
        black.fill(Qt::black);
        check(imageDifference(first, black) > 1.0, QStringLiteral("relinked media renders"));
    }

    // ---- Fade curves ----
    {
        const bool shapes = fadeShape(0, 0.25) == 0.25 && std::abs(fadeShape(4, 0.5) - std::sqrt(0.5)) < 1e-9
                            && fadeShape(3, 0.5) < 0.5 && fadeShape(2, 0.5) > 0.5 && std::abs(fadeShape(1, 0.5) - 0.5) < 1e-9;
        // Constant power: the two sides of a crossfade sum to the same power all the way through.
        double worstPower = 0;
        for (double x = 0; x <= 1.0; x += 0.05)
            worstPower = std::max(worstPower, std::abs(fadeShape(4, x) * fadeShape(4, x) + fadeShape(4, 1 - x) * fadeShape(4, 1 - x) - 1.0));
        Clip c;
        c.start = 0;
        c.length = 4 * S;
        c.fadeIn = 2 * S;
        c.fadeInCurve = 3; // slow
        const double g = envelopeGain(c, ClipEnvelope{c.fadeIn, 0}, S);
        check(shapes && worstPower < 1e-9 && std::abs(g - 0.25) < 1e-9,
              QStringLiteral("fade curves: linear, smooth, fast, slow, constant power (equal power through a crossfade); a slow fade is at 25% half way"));
    }

    // ---- Auto ducking and per-track meters ----
    {
        const int rate = 48000;
        std::vector<float> voice(size_t(rate) * 4 * 2, 0.0f), music(size_t(rate) * 4 * 2);
        uint32_t seed = 1;
        for (int i = rate; i < 2 * rate; ++i) // a "voice" from 1 to 2 s
            voice[size_t(i) * 2] = voice[size_t(i) * 2 + 1] = 0.3f * (float((seed = seed * 1664525u + 1013904223u) >> 8) / 16777216.0f - 0.5f) * 2.0f;
        for (int i = 0; i < 4 * rate; ++i)
            music[size_t(i) * 2] = music[size_t(i) * 2 + 1] = 0.25f * float(std::sin(2 * std::numbers::pi * 440.0 * i / rate));
        writeWav(QDir(outDir).filePath(QStringLiteral("voice.wav")), voice, rate);
        writeWav(QDir(outDir).filePath(QStringLiteral("music.wav")), music, rate);
        auto voiceItem = loadItem(QDir(outDir).filePath(QStringLiteral("voice.wav")), 11);
        auto musicItem = loadItem(QDir(outDir).filePath(QStringLiteral("music.wav")), 12);
        ProjectSnapshot snap;
        snap.settings.fps = fps;
        snap.media[11] = voiceItem;
        snap.media[12] = musicItem;
        snap.timeline = defaultTimeline();
        const int voiceTrack = ops::addTrack(snap.timeline, TrackType::Audio);
        ops::insertMedia(snap.timeline, *voiceItem, 0, voiceTrack, fps);
        const int musicTrack = 1; // the default audio track
        ops::insertMedia(snap.timeline, *musicItem, 0, musicTrack, fps);
        auto musicLevel = [&](const ProjectSnapshot& sn, double from, double len, std::map<int, std::pair<float, float>>* peaks = nullptr) {
            // The music track alone (voice muted in the mix but still driving the duck is not possible), so
            // measure the music track's own peaks while both play.
            AudioMixer mixer(rate);
            const int total = int(4.0 * rate);
            std::vector<float> chunk(1024 * 2);
            float peak = 0;
            for (int i = 0; i < total; i += 1024) {
                mixer.mix(sn, i, 1024, chunk.data());
                const auto tp = mixer.takeTrackPeaks();
                const double t = double(i) / rate;
                if (t >= from && t + 1024.0 / rate <= from + len)
                    if (auto it = tp.find(sn.timeline.tracks[size_t(musicTrack)].id); it != tp.end())
                        peak = std::max(peak, it->second.first);
                if (peaks && t >= 1.2 && t < 1.8)
                    for (const auto& [id, pk] : tp)
                        (*peaks)[id].first = std::max((*peaks)[id].first, pk.first);
            }
            return 20.0 * std::log10(std::max(1e-6f, peak));
        };
        std::map<int, std::pair<float, float>> peaks;
        const double plain = musicLevel(snap, 1.2, 0.6, &peaks);
        const bool metered = peaks.size() == 2 && peaks[snap.timeline.tracks[size_t(voiceTrack)].id].first > 0.2f;
        ProjectSnapshot ducked = snap;
        ducked.timeline.tracks[size_t(musicTrack)].duckUnder = snap.timeline.tracks[size_t(voiceTrack)].id;
        ducked.timeline.tracks[size_t(musicTrack)].duckAmount = -12;
        const double before = musicLevel(ducked, 0.2, 0.6), during = musicLevel(ducked, 1.2, 0.6), after = musicLevel(ducked, 3.0, 0.8);
        check(metered && std::abs(during - plain + 12) < 1.0 && std::abs(before - plain) < 0.3 && std::abs(after - plain) < 0.5,
              QStringLiteral("auto duck: music %1 dB before, %2 dB under the voice, %3 dB after (target -12); per-track meters")
                  .arg(before - plain, 0, 'f', 1).arg(during - plain, 0, 'f', 1).arg(after - plain, 0, 'f', 1));
    }

    // ---- Freeze frame (a 0% velocity event) ----
    {
        ProjectSnapshot s;
        s.settings.fps = fps;
        s.settings.width = 320;
        s.settings.height = 180;
        s.media[video->id] = video;
        s.timeline = defaultTimeline();
        ops::insertMedia(s.timeline, *video, 0, 0, fps);
        Clip& c = s.timeline.tracks[0].clips[0];
        c.sourceIn = S;
        c.length = 3 * S;
        c.velocity = Envelope{true, 0.0, {}};
        Compositor cpu(true);
        const QImage a = cpu.render(s, S / 2, QSize(320, 180)), b = cpu.render(s, 5 * S / 2, QSize(320, 180));
        ProjectSnapshot plainSnap = s;
        plainSnap.timeline.tracks[0].clips[0].velocity = Envelope{false, 1.0, {}};
        const QImage ref = cpu.render(plainSnap, 0, QSize(320, 180)); // the frame at sourceIn
        check(imageDifference(a, b) < 0.01 && imageDifference(a, ref) < 0.5,
              QStringLiteral("a freeze frame holds the picture at its source time (%1)").arg(imageDifference(a, ref), 0, 'f', 2));
    }

    // ---- Colour: curves, wheels, .cube LUTs ----
    {
        const QString cube = QDir(outDir).filePath(QStringLiteral("invert.cube"));
        {
            QFile f(cube);
            f.open(QIODevice::WriteOnly | QIODevice::Text);
            QByteArray text = "TITLE \"invert\"\nLUT_3D_SIZE 17\n";
            for (int b = 0; b < 17; ++b)
                for (int g = 0; g < 17; ++g)
                    for (int r = 0; r < 17; ++r)
                        text += QByteArray::number(1.0 - r / 16.0, 'f', 6) + ' ' + QByteArray::number(1.0 - g / 16.0, 'f', 6) + ' '
                                + QByteArray::number(1.0 - b / 16.0, 'f', 6) + '\n';
            f.write(text);
        }
        QString err;
        const auto lut = loadCubeLut(cube, &err);
        const auto oneD = parseCubeLut("LUT_1D_SIZE 3\n0 0 0\n0.25 0.5 0.75\n1 1 1\n", &err);
        float out[3];
        const float mid[3] = {0.5f, 0.5f, 0.5f};
        if (oneD)
            oneD->lookup(mid, out);
        check(lut && lut->size == 17 && lut->title == QStringLiteral("invert") && oneD && std::abs(out[0] - 0.25f) < 0.01f
                  && std::abs(out[2] - 0.75f) < 0.01f,
              QStringLiteral(".cube files: 3D tables and 1D curves load (%1)").arg(err));
        auto snapFx = [&](EffectChain fx) {
            ProjectSnapshot s;
            s.settings.fps = fps;
            s.settings.width = 640;
            s.settings.height = 360;
            s.media[video->id] = video;
            s.timeline = defaultTimeline();
            ops::insertMedia(s.timeline, *video, 0, 0, fps);
            s.timeline.tracks[0].clips[0].effects = std::move(fx);
            return s;
        };
        int nextFx = 5000;
        auto effect = [&](const char* type, std::vector<std::pair<const char*, double>> values, QString text = {}) {
            EffectInstance e = makeEffectOfType(QString::fromLatin1(type), nextFx++);
            for (const auto& [k, v] : values)
                e.params[QString::fromLatin1(k)].value = v;
            if (!text.isEmpty())
                e.params[QStringLiteral("file")].text = text;
            return e;
        };
        Compositor cpu(true);
        const QSize size(640, 360);
        const QImage plain = cpu.render(snapFx({}), S, size);
        const QImage identity = cpu.render(snapFx({effect("curves", {}), effect("color_wheels", {}), effect("lut_cube", {{"strength", 0}}, cube)}), S, size);
        const QImage inverted = cpu.render(snapFx({effect("lut_cube", {}, cube)}), S, size);
        const QRgb pp = plain.pixel(100, 90), ip = inverted.pixel(100, 90);
        const bool invertOk = std::abs(qRed(ip) - (255 - qRed(pp))) <= 2 && std::abs(qBlue(ip) - (255 - qBlue(pp))) <= 2;
        const QImage sCurve = cpu.render(snapFx({effect("curves", {{"darks", -0.1}, {"lights", 0.1}})}), S, size);
        const QImage warm = cpu.render(snapFx({effect("color_wheels", {{"gainHue", 30}, {"gainAmount", 0.6}})}), S, size);
        check(imageDifference(identity, plain) < 0.01 && invertOk && imageDifference(sCurve, plain) > 1 && imageDifference(warm, plain) > 1,
              QStringLiteral("Curves, Color Wheels and LUT: neutral settings change nothing; an invert LUT inverts; a curve and a wheel push change the picture"));
        auto surface = createGpuSurface();
        if (auto gpu = surface ? GpuCompositor::create(surface.get(), true, &err) : nullptr) {
            double worst = 0;
            for (const EffectChain& fx : {EffectChain{effect("lut_cube", {}, cube)}, EffectChain{effect("lut_cube", {{"strength", 0.5}}, cube)},
                                          EffectChain{effect("curves", {{"darks", -0.1}, {"lights", 0.1}, {"blueShadows", 0.1}})},
                                          EffectChain{effect("color_wheels", {{"lift", 0.1}, {"liftHue", 190}, {"liftAmount", 0.5}, {"gamma", -0.2}, {"gainHue", 30}, {"gainAmount", 0.4}})}}) {
                const ProjectSnapshot s = snapFx(fx);
                worst = std::max(worst, imageDifference(cpu.render(s, S, size), gpu->render(s, S, size)));
            }
            check(worst < 1.5 && gpu->cpuFallbacks() == 0,
                  QStringLiteral("GPU Curves, Color Wheels and LUTs match the CPU (worst difference %1, %2 CPU fallbacks)").arg(worst, 0, 'f', 2).arg(gpu->cpuFallbacks()));
        }
    }

    // ---- Saved: fade curves and ducking ----
    {
        Project proj;
        const int id = proj.addMedia(video->info);
        Timeline tl = proj.timeline();
        ops::insertMedia(tl, *proj.mediaItem(id), 0, -1, fps);
        tl.tracks[0].clips[0].fadeInCurve = 4;
        tl.tracks[0].clips[0].fadeOutCurve = 2;
        Track voice;
        voice.id = tl.allocId(); voice.type = TrackType::Audio; voice.name = QStringLiteral("Voice");
        tl.tracks.push_back(voice);
        tl.tracks[1].duckUnder = voice.id;
        tl.tracks[1].duckAmount = -18;
        proj.applyEdit(tl, QStringLiteral("t"));
        const QString path = QDir(outDir).filePath(QStringLiteral("curves.mvp"));
        QString error;
        proj.save(path, &error);
        Project back;
        back.load(path, &error);
        const Timeline& b = back.timeline();
        check(b.tracks[0].clips[0].fadeInCurve == 4 && b.tracks[0].clips[0].fadeOutCurve == 2 && b.tracks[1].duckUnder == voice.id
                  && b.tracks[1].duckAmount == -18.0f,
              QStringLiteral("fade curves and auto duck settings are saved with the project"));
    }

    // ---- Track blend modes on the CPU and GPU ----
    {
        // Top track: inverted footage at 80% in each blend mode, over the footage.
        auto snapWith = [&](int mode, double level) {
            ProjectSnapshot s;
            s.settings.fps = fps;
            s.settings.width = 640; // rendered at its own size: no scaling differences
            s.settings.height = 360;
            s.media[video->id] = video;
            s.timeline = defaultTimeline();
            ops::addTrack(s.timeline, TrackType::Video, 0);
            ops::insertMedia(s.timeline, *video, 0, 1, fps); // bottom video track
            Clip top = s.timeline.tracks[1].clips.front();
            top.id = s.timeline.allocId();
            top.groupId = 0;
            top.effects = {makeEffectOfType(QStringLiteral("invert"), s.timeline.allocId())};
            s.timeline.tracks[0].clips.push_back(top);
            s.timeline.tracks[0].blend = mode;
            s.timeline.tracks[0].level = float(level);
            return s;
        };
        Compositor cpu(true);
        const QSize size(640, 360);
        const TimeUs t = S;
        const QImage normal = cpu.render(snapWith(0, 1.0), t, size);
        const QImage multiply = cpu.render(snapWith(3, 1.0), t, size);
        const QImage plain = [&] {
            ProjectSnapshot s = snapWith(0, 1.0);
            s.timeline.tracks[0].muted = true;
            return cpu.render(s, t, size);
        }();
        const QRgb d = plain.pixel(100, 90), n = normal.pixel(100, 90), m = multiply.pixel(100, 90);
        const bool mulOk = std::abs(qRed(m) - qRed(d) * (255 - qRed(d)) / 255) <= 3 && std::abs(qGreen(m) - qGreen(d) * (255 - qGreen(d)) / 255) <= 3;
        const bool normalOk = std::abs(qRed(n) - (255 - qRed(d))) <= 3;
        check(mulOk && normalOk, QStringLiteral("track Multiply mode multiplies with the tracks below; Normal covers them"));
        auto surface = createGpuSurface();
        QString err;
        if (auto gpu = surface ? GpuCompositor::create(surface.get(), true, &err) : nullptr) {
            double worst = 0;
            for (int mode = 0; mode < 8; ++mode)
                for (double level : {1.0, 0.6}) {
                    const ProjectSnapshot s = snapWith(mode, level);
                    worst = std::max(worst, imageDifference(cpu.render(s, t, size), gpu->render(s, t, size)));
                }
            check(worst < 1.5, QStringLiteral("GPU track blend modes match the CPU (8 modes x 2 levels, worst difference %1)").arg(worst, 0, 'f', 2));
        } else {
            print(QStringLiteral("SKIP  GPU track blend modes (no OpenGL: %1)").arg(err));
        }
    }
    // ---- Looping past the end of the file; cutting instead of crossfading ----
    {
        // The test file (4 s) looped over 10 s: the picture and the sound start again at 4 s.
        ProjectSnapshot snap;
        snap.settings.width = 320;
        snap.settings.height = 180;
        snap.media[1] = video;
        Track vt;
        vt.id = 1;
        vt.type = TrackType::Video;
        Track at;
        at.id = 2;
        at.type = TrackType::Audio;
        Clip c;
        c.id = 3;
        c.mediaId = 1;
        c.streamIndex = video->info.video.front().index;
        c.length = 10 * S;
        c.loopLength = video->info.duration;
        vt.clips.push_back(c);
        Clip a = c;
        a.id = 4;
        a.streamIndex = video->info.audio.empty() ? -1 : video->info.audio.front().index;
        at.clips.push_back(a);
        snap.timeline.tracks = {vt, at};
        const TimeUs dur = video->info.duration;
        Compositor comp(true);
        const double sameFrame = imageDifference(comp.render(snap, S / 2, QSize(320, 180)), comp.render(snap, dur + S / 2, QSize(320, 180)));
        const double laterFrame = imageDifference(comp.render(snap, S / 2, QSize(320, 180)), comp.render(snap, S + S / 2, QSize(320, 180)));
        // Sound right after the end of the file (it would be silence without the loop), like the start.
        AudioMixer mixer(48000);
        std::vector<float> start(4800 * 2), wrapped(4800 * 2);
        mixer.mix(snap, usToSamples(S / 10, 48000), 4800, start.data());
        AudioMixer mixer2(48000);
        mixer2.mix(snap, usToSamples(dur + S / 10, 48000), 4800, wrapped.data());
        double peakStart = 0, peakWrapped = 0;
        for (size_t i = 0; i < start.size(); ++i) {
            peakStart = std::max(peakStart, double(std::abs(start[i])));
            peakWrapped = std::max(peakWrapped, double(std::abs(wrapped[i])));
        }
        Timeline back;
        {
            Project proj;
            Timeline round;
            round.tracks = {vt};
            round.tracks[0].clips[0].mediaId = proj.addMedia(video->info);
            round.nextId = 50;
            proj.restoreTimeline(round);
            const QString file = QDir(outDir).filePath(QStringLiteral("loop.mvp"));
            QString err;
            Project loaded;
            if (proj.save(file, &err) && loaded.load(file, &err))
                back = loaded.timeline();
        }
        check(sameFrame < 1 && laterFrame > 1 && (a.streamIndex < 0 || (peakStart > 0.01 && std::abs(peakWrapped - peakStart) < 0.05))
                  && !back.tracks.empty() && back.tracks[0].clips[0].loopLength == dur,
              QStringLiteral("an event looping past the end of its file: the picture at %1 s is the one at 0.5 s again (difference %2, "
                             "%3 for a second later), the sound comes back (peak %4 vs %5), and the loop is saved")
                  .arg(double(dur + S / 2) / S, 0, 'f', 2).arg(sameFrame, 0, 'f', 2).arg(laterFrame, 0, 'f', 2)
                  .arg(peakWrapped, 0, 'f', 3).arg(peakStart, 0, 'f', 3));
        // Automatic crossfades off: what an edited event lands on is cut.
        auto events = [&](std::vector<std::pair<TimeUs, TimeUs>> spans) {
            Timeline t;
            t.nextId = 100;
            Track v;
            v.id = t.allocId();
            v.type = TrackType::Video;
            for (auto [s, e] : spans) {
                Clip x;
                x.id = t.allocId();
                x.mediaId = 1;
                x.streamIndex = c.streamIndex;
                x.start = s;
                x.length = e - s;
                x.sourceIn = 0;
                v.clips.push_back(x);
            }
            t.tracks.push_back(v);
            t.sortClips();
            return t;
        };
        auto spansOf = [](const Timeline& t) {
            QStringList out;
            for (const Clip& x : t.tracks[0].clips)
                out << QStringLiteral("%1-%2").arg(double(x.start) / kUsPerSecond).arg(double(x.end()) / kUsPerSecond);
            return out.join(QLatin1Char(' '));
        };
        // The edited event (id 102) over: the end of one, the start of another, one wholly under it, and inside a longer one.
        Timeline o1 = events({{0, 4 * S}, {3 * S, 6 * S}, {5 * S, 9 * S}});
        ops::overwriteOverlaps(o1, {102});
        Timeline o2 = events({{0, 10 * S}, {3 * S, 5 * S}});
        ops::overwriteOverlaps(o2, {102});
        Timeline o3 = events({{4 * S, 5 * S}, {3 * S, 7 * S}});
        ops::overwriteOverlaps(o3, {102});
        check(spansOf(o1) == QStringLiteral("0-3 3-6 6-9") && spansOf(o2) == QStringLiteral("0-3 3-5 5-10") && spansOf(o3) == QStringLiteral("3-7"),
              QStringLiteral("with Automatic Crossfades off an edited event cuts what it lands on: the end before it and the start "
                             "after it (%1), the longer one around it (%2), one wholly under it (%3)")
                  .arg(spansOf(o1), spansOf(o2), spansOf(o3)));
    }
    // ---- Generated media and events inside other events ----
    {
        // A long event on video track 0 (0-20 s), from 2 s into its file.
        auto withLong = [&] {
            Timeline t;
            t.nextId = 100;
            Track v;
            v.id = t.allocId();
            v.type = TrackType::Video;
            Clip c;
            c.id = t.allocId();
            c.mediaId = 1;
            c.streamIndex = video->info.video.front().index;
            c.start = 0;
            c.length = 20 * S;
            c.sourceIn = 2 * S;
            v.clips.push_back(c);
            t.tracks.push_back(v);
            return t;
        };
        const GeneratorPreset* title = findGeneratorPreset(QStringLiteral("title"));
        // Dropped inside it: on a new track above, over the picture (no crossfade over all of it).
        Timeline a = withLong();
        const int t1 = ops::insertGenerated(a, *title, 5 * S, 0, fps);
        int t1Track = -1;
        a.findClip(t1, &t1Track);
        const bool above = a.tracks.size() == 2 && t1Track == 0 && a.tracks[1].clips.size() == 1 && a.tracks[1].clips[0].length == 20 * S;
        // A free video track above is used rather than a new one.
        Timeline b = withLong();
        ops::addTrack(b, TrackType::Video, 0);
        const int t2 = ops::insertGenerated(b, *title, 5 * S, 1, fps);
        int t2Track = -1;
        b.findClip(t2, &t2Track);
        const bool reused = b.tracks.size() == 2 && t2Track == 0;
        // On a free stretch (after it): where it was dropped.
        Timeline d = withLong();
        const int t3 = ops::insertGenerated(d, *title, 25 * S, 0, fps);
        int t3Track = -1;
        d.findClip(t3, &t3Track);
        const bool same = d.tracks.size() == 1 && t3Track == 0;
        check(above && reused && same,
              QStringLiteral("a title dropped inside a long event goes on the track above (a new one, or a free one there); "
                             "on a free stretch it stays on the track it was dropped on"));
        // A title already inside it on the same track (older projects): the crossfade's x cuts the long one around it.
        Timeline e = withLong();
        Clip inner;
        inner.id = e.allocId();
        inner.start = 5 * S;
        inner.length = 5 * S;
        inner.generator = makeGenerator(*title);
        e.tracks[0].clips.push_back(inner);
        e.sortClips();
        const int outerId = e.tracks[0].clips[0].id;
        const bool removed = ops::removeTransition(e, inner.id, true);
        const auto& cs = e.tracks[0].clips;
        bool cut = removed && cs.size() == 3;
        if (cut) {
            cut = cs[0].id == outerId && cs[0].start == 0 && cs[0].length == 5 * S && cs[1].id == inner.id && cs[1].start == 5 * S
                  && cs[2].start == 10 * S && cs[2].end() == 20 * S && cs[2].sourceIn == 12 * S && !cs[2].isGenerated();
        }
        check(cut, QStringLiteral("removing the crossfade over a title inside a long event cuts that event around it: before "
                                  "(0-5 s), the title (5-10 s), the rest after it (10-20 s, from 12 s in its file)"));
    }
    return failures;
}

// A synthetic drum loop: kick on every beat (some missing), snare on 2 and 4,
// hi-hats on eighths, a pumping bass and a pad; hits are nudged by up to
// +-`jitterMs`. Tempo changes from `bpm` to `bpm2` at `changeAt` seconds.
std::vector<float> drumLoop(double bpm, double seconds, int rate, double jitterMs = 4, double bpm2 = 0, double changeAt = 1e9,
                            double silenceFrom = 1e9)
{
    std::vector<float> out(size_t(seconds * rate) * 2, 0.0f);
    uint32_t seed = 12345;
    auto rnd = [&] { return float((seed = seed * 1664525u + 1013904223u) >> 8) / 16777216.0f; };
    auto add = [&](double t, double len, const std::function<float(double)>& fn) {
        const int a = int(t * rate), b = std::min(int(out.size() / 2), int((t + len) * rate));
        for (int i = std::max(0, a); i < b; ++i) {
            const float v = fn(double(i - a) / rate);
            out[size_t(i) * 2] += v;
            out[size_t(i) * 2 + 1] += v;
        }
    };
    double t = 0.5;
    int beat = 0;
    while (t < std::min(seconds, silenceFrom)) {
        const double period = 60.0 / (t < changeAt ? bpm : bpm2);
        const double j = (rnd() - 0.5) * 2 * jitterMs / 1000.0;
        if (beat % 7 != 6) // every 7th kick is missing
            add(t + j, 0.25, [](double x) { // kick: a falling sine
                const double f = 50 + 70 * std::exp(-x * 30);
                return float(0.8 * std::exp(-x * 14) * std::sin(2 * std::numbers::pi * f * x));
            });
        if (beat % 2 == 1)
            add(t + j, 0.15, [&](double x) { return float(0.35 * std::exp(-x * 25) * (rnd() * 2 - 1)); }); // snare
        for (int h = 0; h < 2; ++h) { // hats: high-passed noise (difference of samples)
            float prev = 0;
            add(t + h * period / 2 + j, 0.04, [&](double x) {
                const float n = rnd() * 2 - 1, v = n - prev;
                prev = n;
                return float(0.12 * std::exp(-x * 120) * v);
            });
        }
        add(t, period, [&](double x) { // bass, ducked under the kick
            return float(0.18 * std::min(1.0, x * 8) * std::sin(2 * std::numbers::pi * 55 * (t + x)));
        });
        t += period;
        ++beat;
    }
    for (size_t i = 0; i < out.size() / 2 && double(i) / rate < silenceFrom; ++i) { // a quiet pad
        const double x = double(i) / rate;
        const float v = float(0.05 * (std::sin(2 * std::numbers::pi * 220 * x) + std::sin(2 * std::numbers::pi * 277.2 * x)));
        out[i * 2] += v;
        out[i * 2 + 1] += v;
    }
    return out;
}

// Song-like test material for the beat tracker: arranged sections that make tempo
// detection hard (breakdowns without drums, dotted-eighth arps, half-time, swing,
// syncopated kicks, triplet fills, chopped vocals), at a constant tempo.
enum class SongStyle { House, BoomBap, Trap, Dembow, Rock, DnB, Ballad };

std::vector<float> songAudio(SongStyle style, double bpm, double seconds, int rate, uint32_t seed = 777)
{
    std::vector<float> out(size_t(seconds * rate) * 2, 0.0f);
    auto rnd = [&] { return float((seed = seed * 1664525u + 1013904223u) >> 8) / 16777216.0f; };
    const double pi = std::numbers::pi;
    auto add = [&](double t, double len, const std::function<float(double)>& fn) {
        const int a = int(t * rate), b = std::min(int(out.size() / 2), int((t + len) * rate));
        for (int i = std::max(0, a); i < b; ++i) {
            const float v = fn(double(i - a) / rate);
            out[size_t(i) * 2] += v;
            out[size_t(i) * 2 + 1] += v;
        }
    };
    auto kick = [&](double t, double amp) {
        add(t, 0.3, [=](double x) {
            const double f = 48 + 80 * std::exp(-x * 28);
            return float(amp * std::exp(-x * 11) * std::sin(2 * pi * f * x));
        });
    };
    auto snare = [&](double t, double amp) {
        add(t, 0.2, [&, amp](double x) {
            return float(amp * (0.6 * std::exp(-x * 22) * (rnd() * 2 - 1) + 0.4 * std::exp(-x * 30) * std::sin(2 * pi * 190 * x)));
        });
    };
    auto hat = [&](double t, double amp, double decay = 110) {
        float prev = 0;
        add(t, 0.06, [&, amp, decay](double x) {
            const float n = rnd() * 2 - 1, v = n - prev;
            prev = n;
            return float(amp * std::exp(-x * decay) * v);
        });
    };
    auto pluck = [&](double t, double f, double amp, double len = 0.25) {
        add(t, len, [=](double x) {
            const double e = std::exp(-x * 9) * std::min(1.0, x * 400);
            return float(amp * e * (std::sin(2 * pi * f * x) + 0.5 * std::sin(4 * pi * f * x) + 0.25 * std::sin(6 * pi * f * x)));
        });
    };
    auto tone = [&](double t, double len, double f, double amp, double attack) { // pads, 808s, chords
        add(t, len, [=](double x) {
            const double env = std::min(1.0, x / attack) * std::min(1.0, (len - x) / 0.05);
            return float(amp * env * std::sin(2 * pi * f * x));
        });
    };
    auto vocal = [&](double t, double len, double f, double amp) {
        add(t, len, [=](double x) {
            const double env = std::min(1.0, x / 0.03) * std::min(1.0, (len - x) / 0.04);
            const double ff = f * (1 + 0.01 * std::sin(2 * pi * 5.5 * x));
            double v = 0;
            for (int h = 1; h <= 8; ++h)
                v += std::sin(2 * pi * ff * h * x) * (h == 3 || h == 4 ? 0.8 : 0.3) / h;
            return float(amp * env * v);
        });
    };
    const double P = 60.0 / bpm; // a beat
    const double s16 = P / 4;
    const double roots[4] = {110, 87.3, 130.8, 98};
    // Sections, in bars: each style has its own arrangement, repeated to fill the time.
    enum Sec { Groove, Break, Arp, Build, Sparse, Fill };
    std::vector<std::pair<Sec, int>> plan;
    switch (style) {
    case SongStyle::House: plan = {{Groove, 8}, {Arp, 8}, {Break, 8}, {Build, 2}, {Groove, 8}, {Arp, 8}}; break;
    case SongStyle::BoomBap: plan = {{Groove, 8}, {Sparse, 4}, {Groove, 8}, {Fill, 1}, {Groove, 4}, {Break, 4}}; break;
    case SongStyle::Trap: plan = {{Groove, 8}, {Break, 4}, {Groove, 8}, {Sparse, 4}, {Fill, 1}, {Groove, 8}}; break;
    case SongStyle::Dembow: plan = {{Groove, 8}, {Sparse, 4}, {Groove, 8}, {Arp, 4}, {Groove, 8}}; break;
    case SongStyle::Rock: plan = {{Groove, 7}, {Fill, 1}, {Groove, 7}, {Fill, 1}, {Sparse, 4}, {Groove, 8}}; break;
    case SongStyle::DnB: plan = {{Groove, 8}, {Break, 8}, {Groove, 8}, {Arp, 4}, {Groove, 8}}; break;
    case SongStyle::Ballad: plan = {{Groove, 8}, {Sparse, 4}, {Groove, 7}, {Fill, 1}, {Groove, 8}}; break;
    }
    double t = 0.3;
    int bar = 0;
    for (size_t p = 0; t < seconds; p = (p + 1) % plan.size()) {
        const auto [sec, bars] = plan[p];
        for (int b = 0; b < bars && t < seconds; ++b, ++bar, t += 4 * P) {
            auto j = [&] { return (rnd() - 0.5) * 0.008; }; // +-4 ms human timing
            const double root = roots[bar % 4];
            tone(t, 4 * P, root * 2, 0.04, 0.3); // pad: a chord a bar
            tone(t, 4 * P, root * 2.52, 0.03, 0.3);
            if (sec == Arp || (style == SongStyle::House && sec == Break)) // dotted-eighth arp (4:3 against the beat)
                for (double a = 0; a < 4 * P - 1e-9; a += 3 * s16)
                    pluck(t + a + std::fmod(bar * 4 * P, 3 * s16), root * 4 * (int(a / (3 * s16)) % 2 ? 1.5 : 1), 0.12);
            if (sec == Break)
                continue;
            for (int k = 0; k < 16; ++k) {
                const double at = t + k * s16 + j();
                const double sw = (k % 2 == 1 && (style == SongStyle::BoomBap)) ? s16 * 0.33 : 0; // swing
                switch (style) {
                case SongStyle::House:
                    if (sec == Build) {
                        if (k % (b == 0 ? 2 : 1) == 0)
                            snare(at, 0.15 + 0.25 * (b * 16 + k) / 32.0);
                        break;
                    }
                    if (k % 4 == 0)
                        kick(at, 0.8);
                    if (k % 4 == 2)
                        hat(at, 0.2, 40), pluck(at, root * 3, 0.06); // offbeat open hat + stab
                    if (k % 2 == 1)
                        hat(at, 0.06);
                    if (k == 4 || k == 12)
                        snare(at, 0.18);
                    break;
                case SongStyle::BoomBap:
                    if (sec == Fill) {
                        if (k % 2 == 0)
                            snare(at, 0.3);
                        break;
                    }
                    if ((k == 0 || k == 7 || k == 10) && (sec != Sparse || k == 0))
                        kick(at + sw, 0.8);
                    if (k == 4 || k == 12)
                        snare(at, sec == Sparse ? 0.15 : 0.4);
                    if (k % 2 == 0 && sec != Sparse)
                        hat(at + (k % 4 == 2 ? s16 * 0.66 : 0), 0.1);
                    if (rnd() < (sec == Sparse ? 0.35 : 0.15)) // chopped vocal sample on odd places
                        vocal(at + sw, 0.12 + 0.2 * rnd(), 180 + 60 * std::floor(rnd() * 3), 0.12);
                    break;
                case SongStyle::Trap: // half-time: snare on 3
                    if (sec == Fill) {
                        for (int r = 0; r < 3 && k % 4 == 0; ++r) // triplet tom roll
                            tone(at + r * P / 3, 0.18, 140 - 15 * (k / 4 * 3 + r), 0.3, 0.003);
                        break;
                    }
                    if (k == 0 || k == 6 || k == 11) {
                        kick(at, 0.6);
                        tone(at, 3 * s16, root / 2, 0.25, 0.005); // 808
                    }
                    if (k == 8)
                        snare(at, 0.45);
                    if (sec != Sparse) {
                        if (b % 2 == 1 && k >= 12) { // hat roll in triplets
                            if (k == 12)
                                for (int r = 0; r < 6; ++r)
                                    hat(at + r * P / 6, 0.09);
                        } else if (k % 2 == 0) {
                            hat(at, 0.09);
                        }
                    }
                    if (sec == Sparse && rnd() < 0.3)
                        vocal(at, 0.2 + 0.3 * rnd(), 200 + 40 * std::floor(rnd() * 4), 0.1);
                    break;
                case SongStyle::Dembow:
                    if (k % 4 == 0 && sec != Sparse)
                        kick(at, 0.7);
                    if (k == 3 || k == 6 || k == 11 || k == 14)
                        snare(at, 0.3);
                    if (k % 2 == 0)
                        hat(at, 0.05);
                    if (sec == Sparse && rnd() < 0.3)
                        vocal(at, 0.15 + 0.2 * rnd(), 220, 0.12);
                    break;
                case SongStyle::Rock:
                    if (sec == Fill) {
                        if (k % 4 == 0)
                            for (int r = 0; r < 3; ++r) // triplet fill on the toms
                                tone(at + r * P / 3, 0.2, 180 - 12 * (k / 4 * 3 + r), 0.35, 0.002);
                        break;
                    }
                    if (sec == Sparse) { // quiet verse: guitar chords on the beat, vocals
                        if (k % 4 == 0)
                            pluck(at, root * 2, 0.1, 0.4);
                        if (rnd() < 0.25)
                            vocal(at, 0.2 + 0.2 * rnd(), 160 + 30 * std::floor(rnd() * 4), 0.12);
                        break;
                    }
                    if (k == 0 || k == 10 || (k == 8 && b % 2))
                        kick(at, 0.7);
                    if (k == 4 || k == 12)
                        snare(at, 0.5);
                    if (k % 2 == 0)
                        hat(at, 0.08), pluck(at, root * 2, 0.07, 0.15); // eighth-note guitar
                    break;
                case SongStyle::Ballad: // slow: busy eighth / sixteenth hats that can pass for double time
                    if (sec == Fill) {
                        tone(at, 0.15, 160 - 5 * k, 0.25, 0.002); // sixteenth-note tom run
                        break;
                    }
                    if (k % 4 == 0)
                        pluck(at, root * 2, 0.12, 0.6), pluck(at, root * 2.52, 0.08, 0.6); // piano chords on the beat
                    if (sec == Sparse) {
                        if (rnd() < 0.25)
                            vocal(at, 0.3 + 0.3 * rnd(), 200 + 30 * std::floor(rnd() * 4), 0.1);
                        break;
                    }
                    if (k == 0 || k == 8 || (k == 10 && b % 2))
                        kick(at, 0.7);
                    if (k == 4 || k == 12)
                        snare(at, 0.5);
                    hat(at, k % 2 == 0 ? 0.1 : 0.05); // sixteenths
                    break;
                case SongStyle::DnB:
                    if (sec == Arp) {
                        if (k % 8 == 0)
                            snare(at, 0.2);
                        break;
                    }
                    if (k == 0 || k == 10)
                        kick(at, 0.7);
                    if (k == 4 || k == 12)
                        snare(at, 0.45);
                    if (k % 2 == 0)
                        hat(at, 0.07);
                    if (k == 0)
                        tone(at, 6 * s16, root / 2, 0.2, 0.01); // reese-ish bass
                    break;
                }
            }
        }
    }
    return out;
}

// How well the tracker holds a song's tempo: the share of time (after it first
// finds one, plus a settling time) it shows the right tempo (or the octave of it
// that the range asks for), and the longest stretch it is wrong.
struct TempoHold {
    double correct = 0, longestWrong = 0, firstFound = -1, confidence = 0;
    double exact = -1;          // share of the time it is the tempo itself, not another octave (-1: not in the range)
    double beatError = 0;       // median distance of the beats fired from the song's beats (s)
    double beatsClose = 0;      // share of them within 25 ms
    double beatBias = 0;        // median signed distance (s): + late, - early
    QString wrongValues;
};

// A sung line over a song: syllables that start off the beat grid (0.15-0.6 s long),
// gliding between notes with vibrato, formants that change with each vowel, and
// consonant hisses: a lead vocal as loud as the drums.
void addVocals(std::vector<float>& audio, int rate, double level = 0.3, uint32_t seed = 4242)
{
    auto rnd = [&] { return float((seed = seed * 1664525u + 1013904223u) >> 8) / 16777216.0f; };
    const double pi = std::numbers::pi;
    const double notes[] = {196, 220, 246.9, 261.6, 293.7, 329.6, 392};
    const size_t frames = audio.size() / 2;
    double t = 1.0, f0 = 220;
    while (t < double(frames) / rate - 1) {
        const double len = 0.15 + 0.45 * rnd();
        const double target = notes[size_t(rnd() * 7) % 7];
        const double f1 = 300 + 500 * rnd(), f2 = 900 + 1500 * rnd(); // the vowel
        const double from = f0;
        const bool hiss = rnd() < 0.4;
        const size_t a = size_t(t * rate), b = std::min(frames, size_t((t + len) * rate));
        double phase = 0;
        for (size_t i = a; i < b; ++i) {
            const double x = double(i - a) / rate;
            const double glide = std::min(1.0, x / 0.06);
            const double vib = x > 0.15 ? 1 + 0.015 * std::sin(2 * pi * 5.5 * x) : 1.0;
            const double f = (from + (target - from) * glide) * vib;
            phase += 2 * pi * f / rate;
            double v = 0;
            for (int h = 1; h * f < 5000; ++h) {
                const double fh = h * f;
                const double formant = std::exp(-std::pow((fh - f1) / 150, 2)) + 0.6 * std::exp(-std::pow((fh - f2) / 250, 2)) + 0.05;
                v += formant * std::sin(phase * h) / std::sqrt(double(h));
            }
            const double env = std::min(1.0, x / 0.03) * std::min(1.0, (len - x) / 0.06);
            double s = level * 0.35 * env * v;
            if (hiss && x < 0.04) // "s", "t", "ch"
                s += level * 0.25 * (1 - x / 0.04) * (rnd() * 2 - 1) * (i % 2 ? 1 : -1);
            audio[i * 2] += float(s);
            audio[i * 2 + 1] += float(s);
        }
        f0 = target;
        t += len + (rnd() < 0.3 ? 0.4 + rnd() : 0.05 + 0.15 * rnd()); // breaths and phrases
    }
}

TempoHold holdTempo(const std::vector<float>& audio, int rate, const BeatSettings& settings, double truth, double firstBeat = 0.3)
{
    BeatTracker tracker(rate);
    tracker.setSettings(settings);
    TempoHold h;
    // The truth as the range wants it: octaves folded in.
    std::vector<double> accept;
    for (double m : {0.25, 0.5, 1.0, 2.0, 4.0})
        if (truth * m >= settings.minBpm - 0.5 && truth * m <= settings.maxBpm + 0.5)
            accept.push_back(truth * m);
    if (accept.empty())
        accept = {truth / 2, truth, truth * 2};
    const int chunk = 480;
    double wrongSince = -1, good = 0, all = 0, confidence = 0, exact = 0;
    const bool truthInRange = truth >= settings.minBpm && truth <= settings.maxBpm;
    std::map<int, int> wrongs;
    std::vector<double> beatErrors, signedErrors;
    for (size_t i = 0; i + chunk <= audio.size() / 2; i += chunk) {
        tracker.process(audio.data() + i * 2, chunk, double(i) / rate);
        const double now = double(i) / rate;
        for (double b : tracker.takeBeats())
            if (h.firstFound >= 0 && b > h.firstFound + 6 && tracker.bpm() > 0) {
                // Against the song's beats at the octave shown (eighths when shown at double time).
                const double shown = truth * std::pow(2.0, std::round(std::log2(tracker.bpm() / truth)));
                const double period = 60.0 / std::max(truth, shown);
                double e = std::fmod(b - firstBeat, period);
                if (e < 0)
                    e += period;
                if (e > period / 2)
                    e -= period;
                beatErrors.push_back(std::abs(e));
                signedErrors.push_back(e);
            }
        const double bpm = tracker.bpm();
        if (h.firstFound < 0 && bpm > 0)
            h.firstFound = now;
        if (h.firstFound < 0 || now < h.firstFound + 6 || i % (chunk * 25) != 0)
            continue;
        bool ok = false;
        for (double a : accept)
            ok |= std::abs(bpm / a - 1) < 0.02;
        all += 1;
        good += ok;
        exact += std::abs(bpm / truth - 1) < 0.02;
        confidence += tracker.confidence();
        if (!ok) {
            ++wrongs[int(std::lround(bpm))];
            if (wrongSince < 0)
                wrongSince = now;
            h.longestWrong = std::max(h.longestWrong, now - wrongSince);
        } else {
            wrongSince = -1;
        }
    }
    h.correct = all > 0 ? good / all : 0;
    h.confidence = all > 0 ? confidence / all : 0;
    h.exact = truthInRange && all > 0 ? exact / all : -1;
    if (!beatErrors.empty()) {
        std::sort(beatErrors.begin(), beatErrors.end());
        h.beatError = beatErrors[beatErrors.size() / 2];
        h.beatsClose = double(std::count_if(beatErrors.begin(), beatErrors.end(), [](double e) { return e <= 0.025; })) / double(beatErrors.size());
        std::sort(signedErrors.begin(), signedErrors.end());
        h.beatBias = signedErrors[signedErrors.size() / 2];
    }
    for (const auto& [v, n] : wrongs)
        h.wrongValues += QStringLiteral("%1%2x%3").arg(h.wrongValues.isEmpty() ? "" : " ").arg(v).arg(n);
    return h;
}

int cmdBeatStress()
{
    const int rate = 48000;
    struct Case {
        const char* name;
        SongStyle style;
        double bpm;
    };
    const Case cases[] = {
        {"house", SongStyle::House, 124},  {"house", SongStyle::House, 128},   {"boom bap", SongStyle::BoomBap, 90},
        {"boom bap", SongStyle::BoomBap, 96}, {"trap", SongStyle::Trap, 140},   {"trap", SongStyle::Trap, 150},
        {"dembow", SongStyle::Dembow, 95}, {"rock", SongStyle::Rock, 120},      {"rock", SongStyle::Rock, 104},
        {"dnb", SongStyle::DnB, 174},      {"ballad", SongStyle::Ballad, 64}, {"ballad", SongStyle::Ballad, 72},
    };
    struct Range {
        const char* name;
        double lo, hi;
    };
    const Range ranges[] = {{"default 70-180", 70, 180}, {"hip-hop 65-110", 65, 110}, {"house 100-140", 100, 140}, {"wide 50-200", 50, 200}};
    double total = 0, exact = 0, beatError = 0, close = 0, bias = 0;
    int n = 0, exactN = 0;
    for (const Case& c : cases) {
        const auto audio = songAudio(c.style, c.bpm, 120, rate);
        for (const Range& r : ranges) {
            for (int band : {int(BeatSettings::Full), int(BeatSettings::Bass)}) {
                BeatSettings s;
                s.minBpm = r.lo;
                s.maxBpm = r.hi;
                s.band = band;
                s.latencyMs = 0;
                const TempoHold h = holdTempo(audio, rate, s, c.bpm);
                total += h.correct;
                beatError += h.beatError;
                close += h.beatsClose;
                ++n;
                if (h.exact >= 0) {
                    exact += h.exact;
                    ++exactN;
                }
                print(QStringLiteral("%1  %2 %3 BPM, %4, %5: right %6%, longest wrong %7 s, found at %8 s, %10, beats %11 ms off (%12% within 25 ms, bias %13 ms)  [%9]")
                          .arg(h.correct > 0.97 ? "GOOD" : h.correct > 0.85 ? "OKAY" : "POOR")
                          .arg(QString::fromLatin1(c.name)).arg(c.bpm).arg(QString::fromLatin1(r.name))
                          .arg(band == BeatSettings::Full ? "full" : "kick")
                          .arg(h.correct * 100, 0, 'f', 0).arg(h.longestWrong, 0, 'f', 1).arg(h.firstFound, 0, 'f', 1)
                          .arg(h.wrongValues)
                          .arg(h.exact < 0 ? QStringLiteral("not in range") : QStringLiteral("exact %1%").arg(h.exact * 100, 0, 'f', 0))
                          .arg(h.beatError * 1000, 0, 'f', 1).arg(h.beatsClose * 100, 0, 'f', 0).arg(h.beatBias * 1000, 0, 'f', 1));
                bias += h.beatBias;
            }
        }
    }
    print(QStringLiteral("MEAN  %1% right").arg(total / n * 100, 0, 'f', 1));
    print(QStringLiteral("OCTAVE  %1% the tempo itself where it is in the range").arg(exactN ? exact / exactN * 100 : 0, 0, 'f', 1));
    print(QStringLiteral("GRID  beats %1 ms off the song's (median), %2% within 25 ms, bias %3 ms").arg(beatError / n * 1000, 0, 'f', 1).arg(close / n * 100, 0, 'f', 1).arg(bias / n * 1000, 0, 'f', 1));
    // Song changes: how long until the new tempo is shown.
    struct Change {
        const char* name;
        SongStyle a;
        double bpmA;
        SongStyle b;
        double bpmB;
    };
    const Change changes[] = {
        {"house 128 -> boom bap 96 (3:4)", SongStyle::House, 128, SongStyle::BoomBap, 96},
        {"house 124 -> dnb 174", SongStyle::House, 124, SongStyle::DnB, 174},
        {"rock 120 -> dembow 95", SongStyle::Rock, 120, SongStyle::Dembow, 95},
        {"boom bap 90 -> house 126", SongStyle::BoomBap, 90, SongStyle::House, 126},
    };
    double worst = 0;
    for (const Change& c : changes) {
        std::vector<float> audio = songAudio(c.a, c.bpmA, 60, rate);
        const std::vector<float> second = songAudio(c.b, c.bpmB, 60, rate, 99);
        audio.insert(audio.end(), second.begin(), second.end());
        BeatSettings s;
        s.latencyMs = 0;
        BeatTracker tracker(rate);
        tracker.setSettings(s);
        double since = -1, at = -1;
        for (size_t i = 0; i + 480 <= audio.size() / 2; i += 480) {
            tracker.process(audio.data() + i * 2, 480, double(i) / rate);
            const double now = double(i) / rate;
            if (now < 60)
                continue;
            bool ok = false;
            for (double m : {0.5, 1.0, 2.0})
                ok |= std::abs(tracker.bpm() / (c.bpmB * m) - 1) < 0.02;
            if (ok && since < 0)
                since = now;
            if (!ok)
                since = -1;
            if (since >= 0 && now - since >= 5 && at < 0)
                at = since - 60; // and stays for 5 s
        }
        worst = std::max(worst, at < 0 ? 60.0 : at);
        print(QStringLiteral("%1  %2: new tempo after %3 s (now %4 BPM)")
                  .arg(at >= 0 && at < 15 ? "GOOD" : at >= 0 && at < 30 ? "OKAY" : "POOR")
                  .arg(QString::fromLatin1(c.name))
                  .arg(at < 0 ? QStringLiteral("never") : QString::number(at, 'f', 1))
                  .arg(tracker.bpm(), 0, 'f', 1));
    }
    print(QStringLiteral("SWITCH  worst %1 s").arg(worst, 0, 'f', 1));
    // Vocals: the same songs with a lead vocal on top: the tempo and how sure it is.
    double drop = 0, vocalRight = 0;
    int vn = 0;
    for (const Case& c : cases) {
        BeatSettings s;
        s.latencyMs = 0;
        s.band = BeatSettings::Full;
        std::vector<float> audio = songAudio(c.style, c.bpm, 120, rate);
        const TempoHold dry = holdTempo(audio, rate, s, c.bpm);
        addVocals(audio, rate);
        const TempoHold sung = holdTempo(audio, rate, s, c.bpm);
        drop += sung.confidence / std::max(1e-6, dry.confidence);
        vocalRight += sung.correct;
        ++vn;
        print(QStringLiteral("%1  %2 %3 BPM with vocals: right %4% (without %5%), confidence %6 (without %7)  [%8]")
                  .arg(sung.correct > 0.97 ? "GOOD" : sung.correct > 0.85 ? "OKAY" : "POOR")
                  .arg(QString::fromLatin1(c.name)).arg(c.bpm).arg(sung.correct * 100, 0, 'f', 0).arg(dry.correct * 100, 0, 'f', 0)
                  .arg(sung.confidence, 0, 'f', 2).arg(dry.confidence, 0, 'f', 2).arg(sung.wrongValues));
    }
    print(QStringLiteral("VOCALS  %1% right, confidence kept %2%").arg(vocalRight / vn * 100, 0, 'f', 1).arg(drop / vn * 100, 0, 'f', 0));
    return 0;
}

int cmdBeats()
{
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    const int rate = 48000;
    struct Result {
        double bpm = 0;
        std::vector<double> beats;
    };
    auto run = [&](const std::vector<float>& audio, BeatSettings settings, double* bpmAt = nullptr, double at = 0) {
        BeatTracker tracker(rate);
        tracker.setSettings(settings);
        Result r;
        const int chunk = 480; // 10 ms deliveries, like a capture device
        for (size_t i = 0; i + chunk <= audio.size() / 2; i += chunk) {
            tracker.process(audio.data() + i * 2, chunk, double(i) / rate);
            for (double b : tracker.takeBeats())
                r.beats.push_back(b);
            if (bpmAt && std::abs(double(i) / rate - at) < 0.005)
                *bpmAt = tracker.bpm();
        }
        r.bpm = tracker.bpm();
        return r;
    };
    // Phase error of beats after `from` seconds against the true grid (median, ms).
    auto phaseError = [](const std::vector<double>& beats, double bpm, double from) {
        const double period = 60.0 / bpm;
        std::vector<double> errs;
        for (double b : beats)
            if (b >= from) {
                double e = std::fmod(b - 0.5, period);
                if (e > period / 2)
                    e -= period;
                errs.push_back(std::abs(e) * 1000);
            }
        if (errs.empty())
            return 1e9;
        std::nth_element(errs.begin(), errs.begin() + std::ptrdiff_t(errs.size() / 2), errs.end());
        return errs[errs.size() / 2];
    };
    BeatSettings base;
    base.latencyMs = 0;
    for (double bpm : {90.0, 128.0, 140.0, 174.0}) {
        const auto audio = drumLoop(bpm, 20, rate);
        const Result r = run(audio, base);
        const double err = phaseError(r.beats, bpm, 8);
        int late = 0; // beats after 8 s: the grid keeps every beat (missing kicks too)
        for (double b : r.beats)
            late += b >= 8 && b < 19;
        const int expected = int(11 * bpm / 60);
        check(std::abs(r.bpm - bpm) < 0.35 && err < 8 && std::abs(late - expected) <= 2,
              QStringLiteral("%1 BPM drum loop (jittered, missing kicks): heard %2 BPM, beats %3 ms off the grid, %4 of %5 beats")
                  .arg(bpm).arg(r.bpm, 0, 'f', 2).arg(err, 0, 'f', 1).arg(late).arg(expected));
    }
    {
        // Every hit instead of the grid: one beat per kick played (the missing ones stay missing).
        BeatSettings hits = base;
        hits.locked = false;
        const auto audio = drumLoop(120, 12, rate, 0);
        const Result r = run(audio, hits);
        int kicks = 0, beats = 0;
        for (int beat = 0; 0.5 + beat * 0.5 < 12; ++beat) {
            kicks += beat % 7 != 6;
            ++beats;
        }
        // (Where a kick is missing the bass note still starts on the beat: that counts as a hit too.)
        check(int(r.beats.size()) >= kicks - 1 && int(r.beats.size()) <= beats + 1 && phaseError(r.beats, 120, 0) < 12,
              QStringLiteral("every-hit mode: %1 hits for %2 kicks on %3 beats, on time").arg(r.beats.size()).arg(kicks).arg(beats));
    }
    {
        // The tempo changes from 120 to 128 half way; it is followed within a few seconds.
        double before = 0;
        const auto audio = drumLoop(120, 24, rate, 3, 128, 12);
        const Result r = run(audio, base, &before, 11.5);
        check(std::abs(before - 120) < 0.8 && std::abs(r.bpm - 128) < 0.8,
              QStringLiteral("tempo change 120 -> 128: %1 then %2 BPM").arg(before, 0, 'f', 2).arg(r.bpm, 0, 'f', 2));
    }
    {
        // The tempo range folds octaves: a 174 loop read as 87 in a 60-100 range.
        BeatSettings slow = base;
        slow.minBpm = 60;
        slow.maxBpm = 100;
        const Result r = run(drumLoop(174, 16, rate), slow);
        check(std::abs(r.bpm - 87) < 0.6, QStringLiteral("tempo range 60-100 reads a 174 BPM loop as %1").arg(r.bpm, 0, 'f', 2));
    }
    {
        // Listening to the hi-hats finds eighth notes (2x) when the range allows it.
        BeatSettings hats = base;
        hats.band = BeatSettings::Highs;
        hats.minBpm = 150;
        hats.maxBpm = 260;
        const Result r = run(drumLoop(110, 16, rate, 0), hats);
        check(std::abs(r.bpm - 220) < 1.0, QStringLiteral("listening to the hi-hats (range 150-260): %1 BPM (eighths of 110)").arg(r.bpm, 0, 'f', 2));
    }
    {
        // Latency: locked beats come early by the latency setting.
        BeatSettings early = base;
        early.latencyMs = 80;
        const Result r = run(drumLoop(120, 16, rate, 0), early);
        double sum = 0;
        int n = 0;
        for (double b : r.beats)
            if (b > 8) {
                double e = std::fmod(b - 0.5, 0.5);
                if (e > 0.25)
                    e -= 0.5;
                sum += e;
                ++n;
            }
        // (Beat times are the grid's; what changes is when they are fired: check via the
        // tracker's own timing instead.)
        BeatTracker tracker(rate);
        tracker.setSettings(early);
        const auto audio = drumLoop(120, 16, rate, 0);
        double firstFiredLead = 0;
        int leads = 0;
        for (size_t i = 0; i + 480 <= audio.size() / 2; i += 480) {
            tracker.process(audio.data() + i * 2, 480, double(i) / rate);
            const double now = double(i + 480) / rate;
            for (double b : tracker.takeBeats())
                if (b > 8) {
                    firstFiredLead += b - now;
                    ++leads;
                }
        }
        const double lead = leads ? firstFiredLead / leads * 1000 : 0;
        check(n > 10 && std::abs(sum / n) < 0.012 && lead > 60 && lead < 95,
              QStringLiteral("latency 80 ms: grid beats are fired %1 ms before the audio reaches them").arg(lead, 0, 'f', 1));
    }
    {
        // Silence: locked beats stop a couple of seconds after the music does.
        const Result r = run(drumLoop(120, 16, rate, 0, 0, 1e9, 8), base);
        double last = 0;
        for (double b : r.beats)
            last = std::max(last, b);
        check(last > 7 && last < 10.5, QStringLiteral("beats stop when the music does (last beat %1 s, music ends at 8 s)").arg(last, 0, 'f', 2));
    }
    {
        // Lock BPM: 120 heard and locked, then the music goes to 128: the tempo stays,
        // beats keep coming (on the hits), a reset (new input) keeps it, and unlocking
        // finds 128.
        std::vector<float> audio = drumLoop(120, 12, rate, 0);
        const std::vector<float> faster = drumLoop(128, 40, rate, 0);
        BeatTracker tracker(rate);
        tracker.setSettings(base);
        auto feed = [&](const std::vector<float>& a, double from, double to, double offset, std::vector<double>* beats) {
            for (size_t i = size_t(from * rate); i + 480 <= std::min(a.size() / 2, size_t(to * rate)); i += 480) {
                tracker.process(a.data() + i * 2, 480, offset + double(i) / rate);
                for (double b : tracker.takeBeats())
                    if (beats)
                        beats->push_back(b);
            }
        };
        feed(audio, 0, 12, 0, nullptr);
        const double heard = tracker.bpm();
        tracker.setTempoLocked(true);
        std::vector<double> beats;
        feed(faster, 0, 15, 12, &beats);
        const double whileLocked = tracker.bpm();
        tracker.reset();
        const double afterReset = tracker.bpm();
        int recent = 0;
        for (double b : beats)
            recent += b > 22;
        check(std::abs(heard - 120) < 0.35 && whileLocked == heard && afterReset == heard && tracker.tempoLocked() && recent >= 8,
              QStringLiteral("Lock BPM: %1 heard and locked, still %2 with 128 BPM music (%3 beats in its last 5 s), %4 after a reset")
                  .arg(heard, 0, 'f', 2).arg(whileLocked, 0, 'f', 2).arg(recent).arg(afterReset, 0, 'f', 2));
        tracker.setTempoLocked(true, 125);
        const double typed = tracker.bpm();
        tracker.setTempoLocked(false);
        feed(faster, 15, 40, 27, nullptr);
        check(typed == 125 && !tracker.tempoLocked() && std::abs(tracker.bpm() - 128) < 0.5,
              QStringLiteral("a typed tempo is locked as is (%1); unlocked, the tracker finds the music's again (%2)").arg(typed).arg(tracker.bpm(), 0, 'f', 2));
    }
    {
        // /2: a 120 BPM loop held at 60 even though the range is 70-180, through more music;
        // Set 1 on an off-beat moves the beats there, on a beat it leaves them; Find Tempo Again
        // (forget) goes back to the range's octave.
        const std::vector<float> audio = drumLoop(120, 40, rate, 0);
        BeatTracker tracker(rate);
        tracker.setSettings(base);
        std::vector<double> beats;
        size_t pos = 0;
        auto feedTo = [&](double t) {
            for (; pos + 480 <= std::min(audio.size() / 2, size_t(t * rate)); pos += 480) {
                tracker.process(audio.data() + pos * 2, 480, double(pos) / rate);
                for (double b : tracker.takeBeats())
                    beats.push_back(b);
            }
        };
        feedTo(10);
        const double before = tracker.bpm();
        const double halved = tracker.scaleTempo(0.5);
        feedTo(20);
        const double held = tracker.bpm();
        check(std::abs(before - 120) < 0.35 && std::abs(halved - before / 2) < 1e-9 && std::abs(held - 60) < 0.3,
              QStringLiteral("/2: %1 heard, halved to %2, still %3 ten seconds later (range 70-180)").arg(before, 0, 'f', 2).arg(halved, 0, 'f', 2).arg(held, 0, 'f', 2));
        tracker.scaleTempo(2.0);
        feedTo(20.25); // the kicks are on 0.5 + k * 0.5 s: 20.25 is half way between two
        const bool movedOff = tracker.beatIsNow();
        beats.clear();
        feedTo(23);
        double offset = 0;
        for (double b : beats) {
            double e = std::fmod(b - 0.25, 0.5);
            offset += std::min(e, 0.5 - e);
        }
        offset = beats.empty() ? 1 : offset / double(beats.size());
        feedTo(23.51);
        const bool movedOn = tracker.beatIsNow(); // the grid is on the off-beats now: 23.5 is a kick, so it moves back...
        feedTo(24.01);
        const bool stays = !tracker.beatIsNow(); // ...and then it is on the beat
        check(movedOff && offset < 0.03 && movedOn && stays,
              QStringLiteral("Set 1 between beats moves them there (%1 ms off it after), on a beat leaves them").arg(offset * 1000, 0, 'f', 1));
        tracker.forget();
        feedTo(40);
        check(std::abs(tracker.bpm() - 120) < 0.35, QStringLiteral("Find Tempo Again forgets the octave set by hand: %1").arg(tracker.bpm(), 0, 'f', 2));
    }
    {
        // Vocals: a lead vocal on top of a song hardly changes how sure the tempo is.
        BeatSettings s = base;
        s.band = BeatSettings::Full;
        std::vector<float> audio = songAudio(SongStyle::BoomBap, 92, 60, rate);
        const TempoHold dry = holdTempo(audio, rate, s, 92);
        addVocals(audio, rate);
        const TempoHold sung = holdTempo(audio, rate, s, 92);
        check(sung.correct > 0.97 && sung.confidence > 0.85 * dry.confidence,
              QStringLiteral("a lead vocal over boom bap: right %1% of the time, confidence %2 (%3 without)")
                  .arg(sung.correct * 100, 0, 'f', 0).arg(sung.confidence, 0, 'f', 2).arg(dry.confidence, 0, 'f', 2));
    }
    {
        // Songs that used to fool the tempo: a breakdown with only a dotted-eighth arp,
        // swung boom bap with chopped vocals, half-time trap, drum & bass. The tempo
        // (or the octave of it in the range) is held nearly all the time.
        struct Song {
            const char* name;
            SongStyle style;
            double bpm, minBpm, maxBpm;
        };
        const Song songs[] = {{"house with a breakdown and a dotted-eighth arp", SongStyle::House, 128, 70, 180},
                              {"boom bap (swing, chopped vocals)", SongStyle::BoomBap, 90, 65, 110},
                              {"half-time trap", SongStyle::Trap, 140, 70, 180},
                              {"drum & bass", SongStyle::DnB, 174, 70, 180}};
        for (const Song& song : songs) {
            BeatSettings s = base;
            s.band = BeatSettings::Full;
            s.minBpm = song.minBpm;
            s.maxBpm = song.maxBpm;
            const TempoHold h = holdTempo(songAudio(song.style, song.bpm, 120, rate), rate, s, song.bpm);
            check(h.correct > 0.97 && h.longestWrong < 3 && h.beatsClose > 0.6,
                  QStringLiteral("%1 at %2 BPM: right %3% of the time, longest wrong %4 s, beats %5 ms off the song's (%6% within 25 ms) [%7]")
                      .arg(QString::fromLatin1(song.name)).arg(song.bpm).arg(h.correct * 100, 0, 'f', 0).arg(h.longestWrong, 0, 'f', 1)
                      .arg(h.beatError * 1000, 0, 'f', 1).arg(h.beatsClose * 100, 0, 'f', 0).arg(h.wrongValues));
        }
    }
    return failures;
}

int cmdPresets(const QString& mediaDir, const QString& outDir)
{
    QDir().mkpath(outDir);
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    const auto& factory = factoryPresets();
    int singles = 0, chains = 0;
    for (const Preset& p : factory)
        (p.isChain() ? chains : singles)++;
    print(QStringLiteral("INFO  %1 factory presets: %2 single-device, %3 chains").arg(factory.size()).arg(singles).arg(chains));

    // ---- Integrity: unique ids, known effects and parameters, valid FFmpeg chains.
    {
        std::set<QString> ids;
        QStringList problems;
        std::function<void(const EffectChain&, bool, const QString&)> walk = [&](const EffectChain& chain, bool audio, const QString& where) {
            for (const EffectInstance& e : chain) {
                const std::vector<ParamDef>* defs = nullptr;
                if (e.isRack()) {
                    defs = &rackParams();
                    for (const RackChain& c : e.chains) {
                        for (const auto& [id, v] : c.params)
                            if (std::none_of(rackChainParams(audio).begin(), rackChainParams(audio).end(), [&](const ParamDef& d) { return d.id == id; }))
                                problems << QStringLiteral("%1: rack chain parameter %2").arg(where, id);
                        walk(c.effects, audio, where);
                    }
                } else if (auto meta = effectMeta(e.type)) {
                    defs = meta->params;
                    if (meta->audio != audio)
                        problems << QStringLiteral("%1: %2 is the wrong kind").arg(where, e.type);
                } else {
                    problems << QStringLiteral("%1: unknown effect %2").arg(where, e.type);
                    continue;
                }
                for (const auto& [id, v] : e.params) {
                    auto d = std::find_if(defs->begin(), defs->end(), [&](const ParamDef& pd) { return pd.id == id; });
                    if (d == defs->end())
                        problems << QStringLiteral("%1: %2 has no parameter %3").arg(where, e.type, id);
                    else if (d->type == ParamType::Number && (v.value < d->min || v.value > d->max))
                        problems << QStringLiteral("%1: %2.%3 = %4 out of range").arg(where, e.type, id).arg(v.value);
                }
                if (e.type == QLatin1String("a_ffmpeg")) {
                    const QString spec = e.params.count(QStringLiteral("filter")) ? e.params.at(QStringLiteral("filter")).text : QString();
                    if (!audioFilterSpecValid(spec))
                        problems << QStringLiteral("%1: FFmpeg chain does not build: %2").arg(where, spec);
                }
            }
        };
        for (const Preset& p : factory) {
            if (!ids.insert(p.id).second)
                problems << QStringLiteral("duplicate id %1").arg(p.id);
            if (!p.isChain() && (p.chain.size() != 1 || p.chain.front().type != p.effectType))
                problems << QStringLiteral("%1: single preset holds %2 devices").arg(p.id).arg(p.chain.size());
            walk(p.chain, p.audio, p.id);
        }
        for (const QString& pr : problems)
            print(QStringLiteral("      %1").arg(pr));
        check(problems.isEmpty(), QStringLiteral("factory presets use known effects, valid parameters and working FFmpeg chains (%1 problems)").arg(problems.size()));
    }

    // ---- Every audio preset plays: sound comes out, finite and not exploding.
    {
        auto sine = loadItem(QDir(mediaDir).filePath(QStringLiteral("audio_sine_noise.wav")), 1);
        if (!sine) {
            check(false, QStringLiteral("audio_sine_noise.wav missing"));
            return failures;
        }
        const FrameRate fps{30, 1};
        auto rms = [](const std::vector<float>& s) {
            double sum = 0;
            for (float v : s)
                sum += double(v) * v;
            return 10 * std::log10(std::max(sum / double(std::max<size_t>(1, s.size())), 1e-20));
        };
        QStringList bad;
        int played = 0;
        for (const Preset& p : factory) {
            if (!p.audio)
                continue;
            ProjectSnapshot s;
            s.settings.fps = fps;
            s.media[sine->id] = sine;
            s.timeline = defaultTimeline();
            ops::insertMedia(s.timeline, *sine, 0, -1, fps);
            int next = 100;
            s.timeline.tracks.back().clips[0].effects = instantiatePreset(p, [&] { return next++; });
            AudioMixer mixer(48000);
            std::vector<float> out(size_t(48000) * 2), chunk(1024 * 2);
            for (int i = 0; i < 48000; i += 1024) {
                const int n = std::min(1024, 48000 - i);
                mixer.mix(s, i + 48000 / 2, n, chunk.data());
                std::copy_n(chunk.begin(), n * 2, out.begin() + i * 2);
            }
            float peak = 0;
            bool finite = true;
            for (float v : out) {
                finite &= std::isfinite(v);
                peak = std::max(peak, std::abs(v));
            }
            const double level = rms(out);
            ++played;
            if (qEnvironmentVariableIsSet("PRESET_DEBUG") && level < -60) {
                AudioMixer m2(48000);
                QStringList secs;
                for (int sec = 0; sec < 8; ++sec) {
                    std::vector<float> o(size_t(48000) * 2);
                    for (int i = 0; i < 48000; i += 1024) {
                        const int n = std::min(1024, 48000 - i);
                        m2.mix(s, sec * 48000 + i, n, chunk.data());
                        std::copy_n(chunk.begin(), n * 2, o.begin() + i * 2);
                    }
                    secs << QString::number(rms(o), 'f', 1);
                }
                print(QStringLiteral("      %1 per second: %2").arg(p.name, secs.join(QLatin1Char(' '))));
            }
            if (!finite || peak > 4.0f || level < -60)
                bad << QStringLiteral("%1 (peak %2, %3 dB)").arg(p.name).arg(peak, 0, 'f', 2).arg(level, 0, 'f', 1);
        }
        for (const QString& b : bad)
            print(QStringLiteral("      %1").arg(b));
        check(bad.isEmpty() && played > 50, QStringLiteral("all %1 audio presets play sound (finite, no blow-ups, not silent)").arg(played));
    }

    // ---- Delay compensation on events, tracks and the master bus (look-ahead
    // limiter: 2 ms; loudness normaliser: about 3 s): a click stays on time.
    {
        const int rate = 48000;
        std::vector<float> click(size_t(rate) * 6 * 2, 0.0f);
        for (int i = 0; i < 48; ++i)
            click[size_t(rate * 2 + i) * 2] = click[size_t(rate * 2 + i) * 2 + 1] = 0.5f;
        const QString clickPath = QDir(outDir).filePath(QStringLiteral("click6.wav"));
        writeWav(clickPath, click, rate);
        auto clickItem = loadItem(clickPath, 5);
        const FrameRate fps{30, 1};
        enum class Where { Event, Track, Master };
        auto onset = [&](const char* type, const char* filter, Where where) {
            ProjectSnapshot s;
            s.settings.fps = fps;
            s.media[clickItem->id] = clickItem;
            s.timeline = defaultTimeline();
            ops::insertMedia(s.timeline, *clickItem, 0, -1, fps);
            EffectInstance e = makeEffectOfType(QString::fromLatin1(type), 900);
            if (filter)
                e.params[QStringLiteral("filter")].text = QString::fromLatin1(filter);
            Track& audio = s.timeline.tracks.back();
            (where == Where::Event ? audio.clips[0].effects : where == Where::Track ? audio.effects : s.timeline.audioMasterEffects).push_back(e);
            AudioMixer mixer(rate);
            std::vector<float> chunk(1024 * 2);
            for (int64_t pos = 0; pos < int64_t(rate) * 4; pos += 1024) {
                mixer.mix(s, pos, 1024, chunk.data());
                for (int i = 0; i < 1024; ++i)
                    if (std::abs(chunk[size_t(i) * 2]) > 0.05f)
                        return double(pos + i - rate * 2) * 1000.0 / rate; // ms early (-) / late (+)
            }
            return 1e9;
        };
        QStringList report;
        bool ok = true;
        for (auto [label, where] : {std::pair{"event", Where::Event}, std::pair{"track", Where::Track}, std::pair{"master", Where::Master}}) {
            const double lim = onset("a_limiter", nullptr, where);
            const double loud = onset("a_ffmpeg", "loudnorm=I=-16:TP=-1.5:LRA=11", where);
            report << QStringLiteral("%1: limiter %2 ms, loudnorm %3 ms").arg(QString::fromLatin1(label)).arg(lim, 0, 'f', 1).arg(loud, 0, 'f', 1);
            ok &= std::abs(lim) < 0.05 && std::abs(loud) < 5.0;
        }
        check(ok, QStringLiteral("delay compensation keeps a click on time (%1)").arg(report.join(QStringLiteral("; "))));
    }

    // ---- The preview's mixer lives as long as the project: after it has played, edits like a generated episode's
    // (Insert Time, effects added to tracks, events moved) must leave it in time with a fresh one.
    {
        const int rate = 48000, voiceRate = 24000;
        std::vector<float> clicks(size_t(voiceRate) * 10 * 2, 0.0f);
        for (int s = 1; s < 9; ++s)
            for (int i = 0; i < 24; ++i)
                clicks[size_t(voiceRate * s + i) * 2] = clicks[size_t(voiceRate * s + i) * 2 + 1] = 0.5f;
        const QString path = QDir(outDir).filePath(QStringLiteral("clicks24k.wav"));
        writeWav(path, clicks, voiceRate);
        auto item = loadItem(path, 7);
        const FrameRate fps{24, 1};
        ProjectSnapshot s;
        s.settings.fps = fps;
        s.media[item->id] = item;
        s.timeline = defaultTimeline();
        ops::insertMedia(s.timeline, *item, 0, -1, fps);
        Track& voice = s.timeline.tracks.back();
        EffectInstance pitch = makeEffectOfType(QStringLiteral("a_pitch"), 950);
        pitch.params[QStringLiteral("semitones")].value = 4;
        voice.effects.push_back(pitch);
        // The onsets heard in [from, from + seconds), in seconds.
        auto onsets = [&](AudioMixer& m, const ProjectSnapshot& snap, double from, double seconds) {
            std::vector<double> out;
            std::vector<float> chunk(1024 * 2);
            int64_t quiet = rate;
            for (int64_t p = int64_t(from * rate); p < int64_t((from + seconds) * rate); p += 1024) {
                m.mix(snap, p, 1024, chunk.data());
                for (int i = 0; i < 1024; ++i) {
                    if (std::abs(chunk[size_t(i) * 2]) > 0.05f) {
                        if (quiet > rate / 3)
                            out.push_back(double(p + i) / rate);
                        quiet = 0;
                    } else {
                        ++quiet;
                    }
                }
            }
            return out;
        };
        auto same = [](const std::vector<double>& a, const std::vector<double>& b) {
            if (a.size() != b.size() || a.empty())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::abs(a[i] - b[i]) > 0.002)
                    return false;
            return true;
        };
        auto show = [](const std::vector<double>& v) {
            QStringList l;
            for (double x : v)
                l << QString::number(x, 'f', 3);
            return l.join(QLatin1Char(' '));
        };
        AudioMixer live(rate);
        onsets(live, s, 0, 4); // played once
        QStringList report;
        bool ok = true;
        auto compare = [&](const QString& what, const ProjectSnapshot& edited, double from) {
            AudioMixer fresh(rate);
            const auto heard = onsets(live, edited, from, 6), expected = onsets(fresh, edited, from, 6);
            if (!same(heard, expected)) {
                ok = false;
                report << QStringLiteral("%1: %2 / fresh %3").arg(what, show(heard), show(expected));
            }
        };
        ProjectSnapshot inserted = s;
        cuts::insertTime(inserted.timeline, 2 * kUsPerSecond, 3 * kUsPerSecond / 2, fps);
        compare(QStringLiteral("after Insert Time, from the start"), inserted, 0);
        compare(QStringLiteral("after Insert Time, from the middle"), inserted, 2.5);
        ProjectSnapshot echoed = inserted;
        EffectInstance echo = makeEffectOfType(QStringLiteral("a_ffmpeg"), 951);
        echo.params[QStringLiteral("filter")].text = QStringLiteral("aecho=0.8:0.9:7:0.45");
        echoed.timeline.tracks.back().effects.push_back(echo);
        compare(QStringLiteral("after an effect is added"), echoed, 0);
        ProjectSnapshot moved = echoed;
        moved.timeline.tracks.back().clips.front().start += kUsPerSecond / 3;
        compare(QStringLiteral("after the event moves"), moved, 0);
        ProjectSnapshot repitched = moved;
        repitched.timeline.tracks.back().effects.front().params[QStringLiteral("semitones")].value = -6;
        compare(QStringLiteral("after the pitch changes"), repitched, 0);
        check(ok, QStringLiteral("a mixer that has played stays in time with a fresh one after edits %1").arg(report.join(QStringLiteral("; "))));
    }

    // ---- User presets: save, reload, overwrite, delete.
    {
        PresetLibrary& lib = PresetLibrary::instance();
        const QString dir = QDir(outDir).filePath(QStringLiteral("presets"));
        QDir(dir).removeRecursively();
        lib.setDirectory(dir);
        int next = 1;
        auto alloc = [&] { return next++; };
        EffectInstance eq = makeEffectOfType(QStringLiteral("a_eq"), alloc());
        eq.params[QStringLiteral("highGain")].value = 7.5;
        const QString single = lib.save(QStringLiteral("My Bright EQ"), true, QStringLiteral("a_eq"), {eq});
        EffectInstance rack = makeRack(alloc());
        rack.chains.push_back(makeRackChain(true, QStringLiteral("A"), {makeEffectOfType(QStringLiteral("a_reverb"), alloc())}));
        const QString chainId = lib.save(QStringLiteral("My Rack Chain"), true, QString(), {makeEffectOfType(QStringLiteral("a_gain"), alloc()), rack});
        const QString videoId = lib.save(QStringLiteral("My Look"), false, QString(), {makeEffectOfType(QStringLiteral("sepia"), alloc())});
        lib.setDirectory(dir); // reload from disk
        const auto s1 = lib.find(single), c1 = lib.find(chainId), v1 = lib.find(videoId);
        const bool loaded = s1 && c1 && v1 && s1->user && s1->chain.front().params.at(QStringLiteral("highGain")).value == 7.5
                            && c1->chain.size() == 2 && c1->chain[1].isRack() && c1->chain[1].chains.front().effects.front().type == QLatin1String("a_reverb");
        const auto eqPresets = lib.forEffect(QStringLiteral("a_eq"));
        const auto videoChains = lib.chains(false);
        const bool listed = std::any_of(eqPresets.begin(), eqPresets.end(), [&](const Preset& p) { return p.id == single; })
                            && std::none_of(videoChains.begin(), videoChains.end(), [&](const Preset& p) { return p.id == chainId; });
        check(loaded && listed, QStringLiteral("user presets (single device, chain with a rack, video) survive a reload and list in the right places"));

        // Instantiating gives fresh ids; applying a single preset keeps the device's id.
        int fresh = 500;
        const EffectChain inst = instantiatePreset(*c1, [&] { return fresh++; });
        std::set<int> idset;
        forEachEffect(inst, [&](const EffectInstance& e) { idset.insert(e.id); });
        EffectInstance target = makeEffectOfType(QStringLiteral("a_eq"), 42);
        const bool applied = applyPresetTo(*s1, target, [&] { return fresh++; }) && target.id == 42
                             && target.params.at(QStringLiteral("highGain")).value == 7.5;
        const bool wrongType = !applyPresetTo(*s1, rack, [&] { return fresh++; });
        check(idset.size() == 3 && *idset.begin() >= 500 && applied && wrongType,
              QStringLiteral("presets instantiate with fresh ids and load into devices of the same type only"));

        // Same name overwrites; corrupt files are ignored; delete removes.
        eq.params[QStringLiteral("highGain")].value = -3;
        const QString again = lib.save(QStringLiteral("my bright eq"), true, QStringLiteral("a_eq"), {eq});
        {
            QFile junk(QDir(dir).filePath(QStringLiteral("broken.json")));
            junk.open(QIODevice::WriteOnly);
            junk.write("{ not json");
        }
        lib.reload();
        const auto allAudio = lib.all(true);
        const int userCount = int(std::count_if(allAudio.begin(), allAudio.end(), [](const Preset& p) { return p.user; }));
        const bool overwritten = again == single && lib.find(single)->chain.front().params.at(QStringLiteral("highGain")).value == -3 && userCount == 2;
        const bool removed = lib.remove(single) && !lib.find(single) && lib.find(chainId);
        check(overwritten && removed, QStringLiteral("saving under an existing name overwrites it, broken files are skipped, delete works"));
        QDir(dir).removeRecursively();
    }
    return failures;
}

int cmdFormats()
{
    const auto o = formats::overview();
    print(QStringLiteral("demuxers: %1, video decoders: %2, audio decoders: %3, muxers: %4, video encoders: %5, audio encoders: %6")
              .arg(o.demuxers.size())
              .arg(o.videoDecoders.size())
              .arg(o.audioDecoders.size())
              .arg(formats::muxers().size())
              .arg(o.videoEncoders.size())
              .arg(o.audioEncoders.size()));
    return 0;
}

} // namespace

// "Broken VGS" (Broken Video Game Systems): every console effect on video (CPU against
// GPU over a run of frames, so freezes, ghosting and interlace are covered),
// exactness on generated pictures, and one labelled sheet per console.
int cmdConsoles(const QString& mediaDir, const QString& outDir)
{
    QDir().mkpath(outDir);
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    auto surface = createGpuSurface();
    QString err;
    std::unique_ptr<GpuCompositor> gpu = surface ? GpuCompositor::create(surface.get(), true, &err) : nullptr;
    if (!gpu)
        print(QStringLiteral("INFO  no usable GPU (%1): CPU only").arg(err));
    auto main = loadItem(QDir(mediaDir).filePath(QStringLiteral("h264_aac.mp4")), 1);
    if (!main) {
        print(QStringLiteral("FAIL  h264_aac.mp4 not found in %1").arg(mediaDir));
        return 1;
    }
    const QSize size(960, 540);
    const FrameRate fps{30, 1};
    auto baseSnap = [&]() {
        ProjectSnapshot s;
        s.settings.width = 1920;
        s.settings.height = 1080;
        s.settings.fps = fps;
        s.media[main->id] = main;
        s.timeline = defaultTimeline();
        ops::insertMedia(s.timeline, *main, 0, 0, fps);
        return s;
    };
    auto label = [](QImage img, const QString& text) {
        QPainter p(&img);
        QFont f = p.font();
        f.setPixelSize(18);
        f.setBold(true);
        p.setFont(f);
        p.fillRect(QRect(0, 0, img.width(), 26), QColor(0, 0, 0, 170));
        p.setPen(Qt::white);
        p.drawText(QRect(8, 0, img.width() - 16, 26), Qt::AlignVCenter | Qt::AlignLeft, text);
        return img;
    };
    Compositor cpu(true);
    const QImage plain = cpu.render(baseSnap(), kUsPerSecond, size);
    std::map<QString, std::vector<QImage>> sheets; // per console: rows of (CPU 1 s, GPU 1 s, CPU 2.3 s)
    std::vector<QString> order;
    int id = 3000, count = 0, gpuWorstAt = 0;
    double gpuWorst = 0;
    QString gpuWorstName;
    const QString root = vg::categoryRoot() + QLatin1Char('/');
    for (const EffectDef& def : videoEffects()) {
        if (!def.category.startsWith(root))
            continue;
        ++count;
        ProjectSnapshot s = baseSnap();
        s.timeline.tracks[0].clips[0].effects.push_back(makeEffect(def, ++id));
        const int last = fps.frameAt(kUsPerSecond);
        QImage a, b;
        const int fallbacks = gpu ? gpu->cpuFallbacks() : 0;
        for (int f = last - 5; f <= last; ++f) {
            a = cpu.render(s, fps.timeOf(f), size);
            if (gpu)
                b = gpu->render(s, fps.timeOf(f), size);
        }
        const QImage later = cpu.render(s, 2300000, size);
        const double changed = std::max(imageDifference(a, plain), imageDifference(later, plain));
        check(changed >= 0.5, QStringLiteral("%1 changes the picture (difference %2)").arg(def.name).arg(changed, 0, 'f', 1));
        if (gpu) {
            const bool mesh = def.id.contains(QLatin1String("explode")) || def.id.contains(QLatin1String("wobble"))
                              || def.id.contains(QLatin1String("cracks")) || def.id.contains(QLatin1String("texture"))
                              || def.id.contains(QLatin1String("warp")) || def.id.contains(QLatin1String("zbuffer"))
                              || def.id.contains(QLatin1String("tilt")) || def.id.contains(QLatin1String("jitter"));
            const double tolerance = mesh ? 4.0 : 2.5;
            const double d = imageDifference(a, b);
            const bool onGpu = gpu->cpuFallbacks() == fallbacks;
            check(d <= tolerance && onGpu, QStringLiteral("GPU matches CPU over 6 frames: %1  (mean difference %2%3)")
                                               .arg(def.name).arg(d, 0, 'f', 2).arg(onGpu ? QString() : QStringLiteral(", CPU fallback!")));
            if (d > tolerance) {
                a.save(QDir(outDir).filePath(def.id + QStringLiteral("_cpu.png")));
                b.save(QDir(outDir).filePath(def.id + QStringLiteral("_gpu.png")));
            }
        }
        const QString console = def.category.mid(root.size());
        if (!sheets.count(console))
            order.push_back(console);
        auto& sheet = sheets[console];
        sheet.push_back(label(a, def.name + QStringLiteral("  (CPU, 1 s)")));
        sheet.push_back(label(gpu ? b : a, def.name + QStringLiteral("  (GPU, 1 s)")));
        sheet.push_back(label(later, def.name + QStringLiteral("  (CPU, 2.3 s)")));
    }
    check(count >= 100, QStringLiteral("%1 console effects registered").arg(count));

    // Generated pictures draw identically on both renderers, so the console
    // engine itself must agree almost exactly (meshes differ only on edges).
    if (gpu) {
        for (const char* preset : {"gradient", "checkerboard"}) {
            ProjectSnapshot s;
            s.settings.width = 1920;
            s.settings.height = 1080;
            s.settings.fps = fps;
            s.timeline = defaultTimeline();
            const int clip = ops::insertGenerated(s.timeline, *findGeneratorPreset(QString::fromLatin1(preset)), 0, 0, fps);
            const double base = imageDifference(cpu.render(s, kUsPerSecond, size), gpu->render(s, kUsPerSecond, size));
            for (const EffectDef& def : videoEffects()) {
                if (!def.category.startsWith(root))
                    continue;
                ProjectSnapshot e = s;
                e.timeline.findClip(clip)->effects.push_back(makeEffect(def, ++id));
                const QImage ca = cpu.render(e, kUsPerSecond, size), cb = gpu->render(e, kUsPerSecond, size);
                const double d = imageDifference(ca, cb) - base;
                if (d > gpuWorst) {
                    gpuWorst = d;
                    gpuWorstName = QStringLiteral("%1 on %2").arg(def.name, QString::fromLatin1(preset));
                    ca.save(QDir(outDir).filePath(QStringLiteral("exact_worst_cpu.png")));
                    cb.save(QDir(outDir).filePath(QStringLiteral("exact_worst_gpu.png")));
                }
                if (d > 0.5)
                    ++gpuWorstAt;
                if (d > 0.2)
                    print(QStringLiteral("INFO  exactness: %1 on %2 differs by %3").arg(def.name, QString::fromLatin1(preset)).arg(d, 0, 'f', 3));
            }
        }
        check(gpuWorstAt == 0, QStringLiteral("console engine: GPU equals CPU on generated pictures (%1 effects x 2, worst %2 above the "
                                              "renderers' own difference: %3)")
                                   .arg(count).arg(gpuWorst, 0, 'f', 3).arg(gpuWorstName));
    }

    for (const QString& console : order) {
        const auto& imgs = sheets[console];
        const int cols = 3, tw = 480, th = 270;
        const int rows = int((imgs.size() + cols - 1) / cols);
        QImage sheet(cols * tw, rows * th, QImage::Format_ARGB32_Premultiplied);
        sheet.fill(Qt::darkGray);
        QPainter p(&sheet);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        for (size_t i = 0; i < imgs.size(); ++i)
            p.drawImage(QRect(int(i % cols) * tw + 1, int(i / cols) * th + 1, tw - 2, th - 2), imgs[i]);
        p.end();
        QString file = console;
        file.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9]+")), QStringLiteral("_"));
        sheet.save(QDir(outDir).filePath(QStringLiteral("console_%1.png").arg(file)));
    }
    print(QStringLiteral("%1  consoles: %2 failure(s)").arg(failures ? QStringLiteral("FAIL") : QStringLiteral("DONE")).arg(failures));
    return failures ? 1 : 0;
}

// Live capture: lists sources, captures a screen and a window, measures the
// capture rate, how fast capture stops, and live effects on both renderers.
int cmdLive(const QString& outDir)
{
    QDir().mkpath(outDir);
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    QElapsedTimer timer;
    timer.start();
    const std::vector<LiveSource> sources = listLiveSources();
    const qint64 listMs = timer.elapsed();
    int devices = 0, screens = 0, windows = 0;
    for (const auto& src : sources) {
        devices += src.kind == LiveSource::Kind::Device;
        screens += src.kind == LiveSource::Kind::Screen;
        windows += src.kind == LiveSource::Kind::Window;
        print(QStringLiteral("INFO  %1: %2").arg(src.kind == LiveSource::Kind::Device ? QStringLiteral("device")
                                                 : src.kind == LiveSource::Kind::Screen ? QStringLiteral("screen")
                                                                                       : QStringLiteral("window"),
                                                 src.name));
    }
    check(screens > 0, QStringLiteral("sources listed in %1 ms: %2 devices, %3 screens, %4 windows").arg(listMs).arg(devices).arg(screens).arg(windows));

    auto surface = createGpuSurface();
    QString err;
    std::unique_ptr<GpuCompositor> gpu = surface ? GpuCompositor::create(surface.get(), false, &err) : nullptr;
    Compositor cpu(false);

    auto tryCapture = [&](const LiveSource& src, const QString& label) {
        LiveCapture cap;
        QString error;
        timer.restart();
        const bool ok = cap.start(src, {}, QSize(1280, 720), &error);
        const qint64 openMs = timer.elapsed();
        check(ok, QStringLiteral("%1 opens in %2 ms (%3)%4").arg(label).arg(openMs).arg(cap.description(), ok ? QString() : QStringLiteral(": ") + error));
        if (!ok)
            return;
        uint64_t serial = 0;
        timer.restart();
        while (timer.elapsed() < 2500)
            QThread::msleep(50);
        const QImage img = cap.latest(&serial);
        check(!img.isNull() && serial > 0, QStringLiteral("%1 delivers pictures (%2 frames in 2.5 s, %3 fps measured, picture %4x%5 from %6x%7)")
                                               .arg(label).arg(serial).arg(cap.captureFps(), 0, 'f', 1)
                                               .arg(img.width()).arg(img.height()).arg(cap.sourceSize().width()).arg(cap.sourceSize().height()));
        if (!img.isNull()) {
            img.save(QDir(outDir).filePath(label + QStringLiteral(".png")));
            EffectChain chain;
            chain.push_back(makeEffect(*findEffect(QStringLiteral("vg_ps1_wobble")), 1));
            chain.push_back(makeEffect(*findEffect(QStringLiteral("chromatic_aberration")), 2));
            for (FrameRenderer* r : std::initializer_list<FrameRenderer*>{&cpu, gpu.get()}) {
                if (!r)
                    continue;
                QImage out;
                r->renderLive(img, chain, 0, 30);
                QElapsedTimer t;
                t.start();
                for (int i = 1; i <= 10; ++i)
                    out = r->renderLive(img, chain, TimeUs(i) * 33333, 30);
                check(!out.isNull() && out.size() == img.size(),
                      QStringLiteral("%1: live effects on the %2 renderer, %3 ms/frame at %4x%5")
                          .arg(label, r->name()).arg(double(t.elapsed()) / 10.0, 0, 'f', 1).arg(img.width()).arg(img.height()));
                if (!out.isNull())
                    out.save(QDir(outDir).filePath(label + QStringLiteral("_fx_") + (r->isGpu() ? QStringLiteral("gpu") : QStringLiteral("cpu")) + QStringLiteral(".png")));
            }
        }
        timer.restart();
        cap.stop();
        check(timer.elapsed() < 1500, QStringLiteral("%1 stops in %2 ms").arg(label).arg(timer.elapsed()));
    };
    for (const auto& src : sources)
        if (src.kind == LiveSource::Kind::Screen) {
            tryCapture(src, QStringLiteral("screen"));
            break;
        }
    for (const auto& src : sources)
        if (src.kind == LiveSource::Kind::Window) {
            tryCapture(src, QStringLiteral("window"));
            break;
        }
    // Recorder: synthetic moving colour bars, then read back.
    {
        const QString path = QDir(outDir).filePath(QStringLiteral("recorder.mp4"));
        QFile::remove(path);
        LiveRecorder rec;
        QString error;
        const bool ok = rec.start(path, QSize(1280, 720), 30, &error);
        check(ok, QStringLiteral("recorder opens (%1)%2").arg(rec.encoderName(), ok ? QString() : QStringLiteral(": ") + error));
        if (ok) {
            const QColor bars[] = {Qt::red, Qt::green, Qt::blue, Qt::white, Qt::black, Qt::yellow};
            const double t0 = rec.startTime(); // frames are placed by capture time: 30 fps
            for (int f = 0; f < 60; ++f) {
                QImage img(1280, 720, QImage::Format_ARGB32_Premultiplied);
                QPainter p(&img);
                for (int i = 0; i < 6; ++i)
                    p.fillRect(QRect((i * 1280 / 6 + f * 4) % 1280, 0, 1280 / 6 + 1, 720), bars[i]);
                p.end();
                while (!rec.addFrame(img, t0 + f / 30.0))
                    QThread::msleep(5);
            }
            QString finishError;
            const bool finished = rec.finish(&finishError);
            check(finished && rec.frames() == 60, QStringLiteral("recorder writes every frame (%1 of 60)%2").arg(rec.frames())
                                                                     .arg(finishError.isEmpty() ? QString() : QStringLiteral(": ") + finishError));
            const MediaInfo info = probeMedia(path);
            check(info.ok() && info.hasVideo() && std::abs(double(info.duration) / kUsPerSecond - 2.0) < 0.1
                      && info.video.front().width == 1280 && info.video.front().height == 720,
                  QStringLiteral("recording reads back: %1 s, %2x%3").arg(double(info.duration) / kUsPerSecond, 0, 'f', 2)
                      .arg(info.hasVideo() ? info.video.front().width : 0).arg(info.hasVideo() ? info.video.front().height : 0));
            check(!rec.thumbnail().isNull(), QStringLiteral("recorder keeps a thumbnail"));
            // Colours survive the encode (frame 0: the first bar is red at the left).
            if (info.ok() && info.hasVideo()) {
                VideoDecoder dec;
                FrameConverter conv;
                QImage first;
                if (dec.open(path, info.video.front().index, nullptr, false))
                    if (const AVFrame* f = dec.frameAt(0))
                        first = conv.convert(f, QSize(1280, 720), true);
                const QRgb c = first.isNull() ? 0 : first.pixel(100, 360);
                check(qRed(c) > 220 && qGreen(c) < 40 && qBlue(c) < 40,
                      QStringLiteral("colours survive encoding (red bar reads %1,%2,%3)").arg(qRed(c)).arg(qGreen(c)).arg(qBlue(c)));
            }
        }
    }
    // Audio inputs: list them, then capture a generated tone in real time
    // (FFmpeg's lavfi stands in for a microphone, so this runs anywhere).
    for (const auto& a : listLiveAudioSources())
        print(QStringLiteral("INFO  audio input: %1 (%2 %3)").arg(a.name, a.format, a.id));
    {
        LiveAudioCapture cap;
        std::mutex m;
        int64_t samples = 0;
        double firstTime = -1, nextTime = -1, worstJump = 0;
        QString error;
        const double began = liveClock();
        const bool ok = cap.start({QStringLiteral("Test tone"), QStringLiteral("lavfi"),
                                   QStringLiteral("sine=frequency=440:sample_rate=44100,arealtime")},
                                  [&](const float*, int frames, double time) {
                                      std::lock_guard lock(m);
                                      if (firstTime < 0)
                                          firstTime = time;
                                      if (nextTime >= 0)
                                          worstJump = std::max(worstJump, std::abs(time - nextTime));
                                      nextTime = time + double(frames) / LiveAudioCapture::kSampleRate;
                                      samples += frames;
                                  },
                                  &error);
        check(ok, QStringLiteral("audio input opens (%1)%2").arg(cap.description(), ok ? QString() : QStringLiteral(": ") + error));
        if (ok) {
            QThread::msleep(2000);
            const std::pair<float, float> peaks = cap.takePeaks();
            cap.stop();
            std::lock_guard lock(m);
            const double secs = double(samples) / LiveAudioCapture::kSampleRate;
            check(secs > 1.6 && secs < 2.2 && firstTime >= began - 0.1 && worstJump < 0.001,
                  QStringLiteral("audio input delivers sound in real time (%1 s in 2 s, resampled to 48 kHz, stamps continuous within %2 ms)")
                      .arg(secs, 0, 'f', 2).arg(worstJump * 1000, 0, 'f', 2));
            check(std::abs(peaks.first - 0.125f) < 0.02f && std::abs(peaks.second - 0.125f) < 0.02f,
                  QStringLiteral("audio input level reaches the meter (peaks %1 / %2)").arg(peaks.first, 0, 'f', 3).arg(peaks.second, 0, 'f', 3));
        }
    }
    // Recording with sound: flashes in the picture and clicks in the sound at
    // the same moments (1 s and 2 s) must line up in the file, even with
    // late pictures, a hole of missing pictures and sound that started early.
    {
        const QString path = QDir(outDir).filePath(QStringLiteral("recorder_audio.mp4"));
        QFile::remove(path);
        LiveRecorder rec;
        QString error;
        const bool ok = rec.start(path, QSize(640, 360), 30, &error, true);
        check(ok && rec.hasAudio(), QStringLiteral("recorder opens with a sound track%1").arg(ok ? QString() : QStringLiteral(": ") + error));
        if (ok) {
            const double t0 = rec.startTime();
            const int rate = LiveAudioCapture::kSampleRate;
            std::vector<float> chunk;
            // Sound from 0.05 s before the start to 3.1 s, in 20 ms chunks.
            for (int64_t pos = -rate / 20; pos < rate * 31 / 10; pos += rate / 50) {
                chunk.assign(size_t(rate / 50) * 2, 0.0f);
                for (int i = 0; i < rate / 50; ++i) {
                    const int64_t p = pos + i;
                    const bool click = (p >= rate && p < rate + rate / 100) || (p >= 2 * rate && p < 2 * rate + rate / 100);
                    chunk[size_t(i) * 2] = chunk[size_t(i) * 2 + 1] = click ? 0.8f * float(std::sin(2 * std::numbers::pi * 1000.0 * double(p) / rate)) : 0.0f;
                }
                rec.addAudio(chunk.data(), rate / 50, t0 + double(pos) / rate);
            }
            for (int f = 0; f < 90; ++f) {
                if (f >= 41 && f <= 43)
                    continue; // never rendered: the recorder fills the hole
                const double late = f % 7 == 3 ? 0.012 : 0.0; // some pictures come a little late
                QImage img(640, 360, QImage::Format_ARGB32_Premultiplied);
                img.fill(f == 30 || f == 60 ? Qt::white : QColor(20, 20, 20));
                while (!rec.addFrame(img, t0 + f / 30.0 + late))
                    QThread::msleep(5);
            }
            QString finishError;
            const bool finished = rec.finish(&finishError);
            check(finished && rec.frames() == 90,
                  QStringLiteral("recording with sound writes a picture for every slot (%1 of 90, the 3 missing ones filled)%2")
                      .arg(rec.frames()).arg(finishError.isEmpty() ? QString() : QStringLiteral(": ") + finishError));
            const MediaInfo info = probeMedia(path);
            check(info.ok() && info.hasVideo() && info.hasAudio() && std::abs(double(info.duration) / kUsPerSecond - 3.1) < 0.15,
                  QStringLiteral("recording with sound reads back: %1 s, %2 audio stream(s)")
                      .arg(double(info.duration) / kUsPerSecond, 0, 'f', 2).arg(info.audio.size()));
            if (info.hasVideo() && info.hasAudio()) {
                // Flash times from the picture.
                std::vector<double> flashes;
                VideoDecoder vdec;
                FrameConverter conv;
                if (vdec.open(path, info.video.front().index, nullptr, false))
                    for (int f = 0; f < 90; ++f)
                        if (const AVFrame* fr = vdec.frameAt(TimeUs(f) * kUsPerSecond / 30 + 1000)) {
                            const QImage img = conv.convert(fr, QSize(64, 36), true);
                            if (!img.isNull() && qGray(img.pixel(32, 18)) > 200)
                                flashes.push_back(f / 30.0);
                        }
                // Click onsets from the sound.
                std::vector<double> clicks;
                AudioDecoder adec;
                if (adec.open(path, info.audio.front().index, rate)) {
                    std::vector<float> buf(size_t(rate) * 2);
                    int64_t last = -rate;
                    for (int64_t pos = 0; pos < rate * 3; pos += rate) {
                        adec.read(pos, rate, buf.data());
                        for (int i = 0; i < rate; ++i)
                            if (std::abs(buf[size_t(i) * 2]) > 0.3f && pos + i - last > rate / 2) {
                                last = pos + i;
                                clicks.push_back(double(last) / rate);
                            }
                    }
                }
                QStringList pairs;
                double worst = flashes.size() == 2 && clicks.size() == 2 ? 0 : 1e9;
                for (size_t i = 0; i < std::min(flashes.size(), clicks.size()); ++i) {
                    worst = std::max(worst, std::abs(flashes[i] - clicks[i]));
                    pairs << QStringLiteral("picture %1 s / sound %2 s").arg(flashes[i], 0, 'f', 3).arg(clicks[i], 0, 'f', 3);
                }
                check(worst < 1.0 / 30 + 0.005, QStringLiteral("picture and sound are in sync (%1 flashes, %2 clicks: %3)")
                                                     .arg(flashes.size()).arg(clicks.size()).arg(pairs.join(QStringLiteral(", "))));
            }
        }
    }
    // ---- Modulation: the signals, a setting's value, chains (racks too), saving ----
    {
        ModInputs in;
        Modulation beat;
        beat.source = Modulation::Source::Beat;
        beat.beats = 1;
        in.beats = 3.0;
        const double onBeat = modulationSignal(beat, in);
        in.beats = 3.5;
        const double halfway = modulationSignal(beat, in);
        Modulation bar = beat;
        bar.source = Modulation::Source::Bar;
        in.beats = 8.0;
        in.barBeat = 0;
        const double onOne = modulationSignal(bar, in);
        in.barBeat = 2;
        const double onThree = modulationSignal(bar, in);
        check(std::abs(onBeat - 1) < 1e-9 && halfway < 0.1 && halfway > 0.05 && std::abs(onOne - 1) < 1e-9 && onThree < 0.01,
              QStringLiteral("beat pulse 1 on the beat, %1 half way; bar pulse 1 on the 1, %2 on the 3").arg(halfway, 0, 'f', 3).arg(onThree, 0, 'f', 4));
        Modulation lfo;
        lfo.source = Modulation::Source::Lfo;
        lfo.beats = 4;
        auto at = [&](const Modulation& m, double beats) {
            in.beats = beats;
            return modulationSignal(m, in);
        };
        lfo.shape = Modulation::Shape::Sine;
        const bool sine = at(lfo, 0) < 1e-9 && std::abs(at(lfo, 2) - 1) < 1e-9 && std::abs(at(lfo, 1) - 0.5) < 1e-9;
        lfo.shape = Modulation::Shape::Triangle;
        const bool triangle = std::abs(at(lfo, 1) - 0.5) < 1e-9 && std::abs(at(lfo, 3) - 0.5) < 1e-9;
        lfo.shape = Modulation::Shape::Square;
        const bool square = at(lfo, 1) == 1.0 && at(lfo, 3) == 0.0;
        lfo.shape = Modulation::Shape::Random;
        const bool steps = at(lfo, 4.1) == at(lfo, 7.9) && at(lfo, 4.1) != at(lfo, 8.1);
        check(sine && triangle && square && steps, QStringLiteral("LFO shapes over 4 beats: sine, triangle, square, random steps held for a cycle"));
        Modulation midi;
        midi.source = Modulation::Source::Midi;
        midi.channel = 2;
        midi.cc = 21;
        in.midi[2][21] = 127;
        ParamDef def;
        def.min = 0;
        def.max = 100;
        Modulation half = beat;
        half.amount = 0.5;
        in.beats = 1.0;
        const double up = modulatedValue(half, def, 80, in), full = modulatedValue(midi, def, 10, in);
        half.amount = -1;
        const double down = modulatedValue(half, def, 30, in);
        in.midi[2][21] = 0;
        const double zero = modulatedValue(midi, def, 10, in);
        check(up == 100 && full == 100 && down == 0 && zero == 0,
              QStringLiteral("values stay in the range (+50% from 80: %1, -100% from 30: %2); MIDI sets the whole range (127: %3, 0: %4)").arg(up).arg(down).arg(full).arg(zero));
        // A chain: an effect with a number setting, alone and inside a rack.
        const EffectDef* fx = nullptr;
        const ParamDef* num = nullptr;
        for (const EffectDef& d : videoEffects()) {
            for (const ParamDef& p : d.params)
                if (p.type == ParamType::Number && p.max > p.min) {
                    num = &p;
                    break;
                }
            if (num) {
                fx = &d;
                break;
            }
        }
        if (fx && num) {
            EffectChain chain{makeEffect(*fx, 1)};
            chain[0].params[num->id].value = num->def;
            chain[0].params[num->id].mod = midi;
            EffectInstance rack = makeRack(2);
            rack.chains.push_back(makeRackChain(false, QStringLiteral("A"), {chain[0]}));
            rack.chains[0].effects[0].id = 3;
            chain.push_back(rack);
            in.midi[2][21] = 127;
            const EffectChain now = applyModulation(chain, in);
            const ParamValue& top = now[0].params.at(num->id);
            const ParamValue& inner = now[1].chains[0].effects[0].params.at(num->id);
            const ParamMap saved = paramsFromJson(paramsToJson(chain[0].params));
            check(hasModulation(chain) && !hasModulation(now) && top.value == num->max && inner.value == num->max && saved.at(num->id).mod == midi,
                  QStringLiteral("a chain's modulated setting (%1 of %2), also inside a rack, takes its value; the modulation is saved")
                      .arg(num->name, fx->name));
        }
    }
    // ---- Video files as a source: played at their own speed, looped, a playlist ----
    {
        const QString a = QDir(QStringLiteral("testmedia")).absoluteFilePath(QStringLiteral("h264_aac.mp4"));
        const QString b = QDir(QStringLiteral("testmedia")).absoluteFilePath(QStringLiteral("vp8_vorbis.webm"));
        if (!QFileInfo::exists(a) || !QFileInfo::exists(b)) {
            print(QStringLiteral("SKIP  video files as a source: testmedia/h264_aac.mp4 and vp8_vorbis.webm are needed"));
        } else {
            const MediaInfo info = probeMedia(a);
            const double fps = info.hasVideo() ? info.video.front().frameRate.toDouble() : 25;
            LiveSource src;
            src.kind = LiveSource::Kind::File;
            src.name = QStringLiteral("test playlist");
            src.id = a + QLatin1Char('\n') + b;
            LiveCapture cap;
            QString error;
            const bool opened = cap.start(src, {}, QSize(640, 360), &error);
            uint64_t s0 = 0, s1 = 0;
            QThread::msleep(500);
            cap.latest(&s0);
            QThread::msleep(1500);
            cap.latest(&s1);
            const double rate = double(s1 - s0) / 1.5;
            const bool first = cap.currentFile() == a;
            cap.nextFile();
            QThread::msleep(400);
            const bool second = cap.currentFile() == b;
            check(opened && rate > fps * 0.7 && rate < fps * 1.3 && first && second,
                  QStringLiteral("a playlist plays at the video's own speed (%1 pictures/s for %2 fps) and Next goes on to the next file%3")
                      .arg(rate, 0, 'f', 1).arg(fps, 0, 'f', 1).arg(opened ? QString() : QStringLiteral(": ") + error));
            cap.stop();
            // One short file: it loops.
            src.id = a;
            LiveCapture loop;
            loop.start(src, {}, QSize(320, 180), &error);
            QThread::msleep(int(info.duration / 1000) + 1200);
            uint64_t l0 = 0, l1 = 0;
            loop.latest(&l0);
            QThread::msleep(500);
            loop.latest(&l1);
            check(l1 > l0 && loop.failure().isEmpty() && loop.currentFile() == a,
                  QStringLiteral("a %1 s video keeps playing after its end (looped: %2 new pictures in 0.5 s)").arg(info.duration / 1e6, 0, 'f', 1).arg(l1 - l0));
            loop.stop();
        }
    }
    // ---- Replay buffer: the last seconds, saved on demand while it carries on ----
    {
        LiveRecorder rec;
        QString error;
        const bool started = rec.startBuffer(QSize(320, 180), 30, 2.0, &error, true);
        const double t0 = rec.startTime();
        std::vector<float> tone(1600 * 2);
        auto feed = [&](int from, int to) {
            for (int i = from; i < to; ++i) {
                QImage img(320, 180, QImage::Format_ARGB32_Premultiplied);
                img.fill(QColor::fromHsv((i * 7) % 360, 200, 220));
                for (size_t k = 0; k < tone.size() / 2; ++k)
                    tone[k * 2] = tone[k * 2 + 1] = float(0.3 * std::sin(2 * std::numbers::pi * 440 * (i * 1600 + double(k)) / 48000));
                rec.addAudio(tone.data(), 1600, t0 + i / 30.0);
                while (!rec.addFrame(img, t0 + i / 30.0))
                    QThread::msleep(2); // the encoder is busy: wait (a live source would drop it)
            }
        };
        feed(0, 150); // 5 s
        QThread::msleep(300);
        const double buffered = rec.bufferedSeconds();
        const QString path = QDir(outDir).filePath(QStringLiteral("replay.mp4"));
        double secs = 0;
        const bool saved = started && rec.saveReplay(path, &error, &secs);
        feed(150, 180); // it carries on
        const QString path2 = QDir(outDir).filePath(QStringLiteral("replay2.mp4"));
        const bool savedAgain = rec.saveReplay(path2, &error);
        rec.finish();
        const MediaInfo m = probeMedia(path);
        const double duration = m.duration / 1e6;
        bool decodes = false;
        if (m.hasVideo()) {
            VideoDecoder dec;
            if (dec.open(path, m.video.front().index, nullptr, false))
                decodes = dec.frameAt(0) != nullptr;
        }
        check(saved && savedAgain && duration >= 1.9 && duration <= 3.4 && m.hasAudio() && decodes && buffered >= 1.9 && buffered <= 3.6, // (from a keyframe: up to a second more)
              QStringLiteral("replay buffer of 2 s after 5 s: holds %1 s, saves a %2 s clip with sound that starts on a picture, and again later%3")
                  .arg(buffered, 0, 'f', 2).arg(duration, 0, 'f', 2).arg(error.isEmpty() ? QString() : QStringLiteral(" (") + error + QLatin1Char(')')));
    }
    // ---- MIDI clock: the tempo from the ticks, a beat every 24, the bar from Start / song position ----
    {
        MidiClock clock;
        MidiMessage m;
        m.status = 0xFA; // Start
        clock.feed(m);
        const double tick = 60.0 / 125.0 / 24.0;
        std::vector<int> bars;
        double bpm = 0;
        for (int i = 0; i < 24 * 9; ++i) {
            m.status = 0xF8;
            m.time = 100.0 + i * tick + ((i * 7919) % 11 - 5) * 0.0002; // +-1 ms of jitter
            if (const auto beat = clock.feed(m)) {
                bars.push_back(beat->barBeat);
                bpm = beat->bpm;
            }
        }
        m.status = 0xF2; // song position: sixteenth 8 = beat 3 (the 3 of bar 1)
        m.data1 = 8;
        m.data2 = 0;
        clock.feed(m);
        m.status = 0xF8;
        m.time += tick;
        const auto third = clock.feed(m);
        const std::vector<int> expected{0, 1, 2, 3, 0, 1, 2, 3, 0};
        check(bars == expected && std::abs(bpm - 125) < 0.5 && third && third->barBeat == 2,
              QStringLiteral("MIDI clock: 9 beats from 216 ticks at %1 BPM (125 sent), counted from the 1 after Start; the song position moves the bar").arg(bpm, 0, 'f', 2));
    }
    // ---- Game controllers as MIDI: buttons are notes, triggers and stick directions are controllers (0 at rest) ----
    {
        GamePadState rest, pressed = rest;
        pressed.buttons = 1u << 0;      // A
        pressed.axes[1] = 127;          // the right trigger all the way
        pressed.axes[4] = 64;           // the left stick half way up
        const auto down = gamePadChanges(rest, pressed, 1.0);
        const auto up = gamePadChanges(pressed, rest, 2.0);
        const bool shape = down.size() == 3 && down[0].isNoteOn() && down[0].channel() == 15 && down[0].data1 == 60 && down[1].isController()
                           && down[1].data1 == 21 && down[1].data2 == 127 && down[2].data1 == 24 && down[2].data2 == 64;
        const bool released = up.size() == 3 && up[0].isNoteOff() && up[1].data2 == 0 && up[2].data2 == 0;
        MidiMessage other;
        other.status = 0x90; // a note on channel 1: a real MIDI controller's, not a game controller's
        other.data1 = 60;
        other.data2 = 100;
        check(shape && released && gamePadChanges(pressed, pressed, 3.0).empty() && gamePadControlName(down[0]) == QStringLiteral("A button")
                  && gamePadControlName(down[1]) == QStringLiteral("Right trigger") && gamePadControlName(down[2]) == QStringLiteral("Left stick up")
                  && gamePadControlName(other).isEmpty() && isGamePad(QStringLiteral("Xbox controller 1")) && !isGamePad(QStringLiteral("nanoPAD2")),
              QStringLiteral("an Xbox controller reads as MIDI: A is note 60 on channel 16 (on and off), the right trigger and a stick direction "
                             "are controllers, nothing when nothing moved, and each has its name"));
    }
    // ---- Scenes and the visualiser overlays are saved with the project ----
    {
        Timeline tl;
        tl.liveScenes[2].used = true;
        tl.liveScenes[2].name = QStringLiteral("Drop");
        tl.liveScenes[2].pad = 5;
        tl.liveScenes[2].layers[0].source = QStringLiteral("Camera");
        tl.liveScenes[2].layers[0].settings.mode = LiveLayerSettings::Mode::Full;
        tl.liveScenes[2].layers[0].settings.blend = 3;
        tl.liveScenes[2].layers[2].source = QStringLiteral("Logo loop");
        tl.liveScenes[2].layers[2].settings.size = 0.17;
        tl.liveScenes[2].layers[2].settings.noEffects = true;
        tl.liveScenes[2].layers[2].effects = {makeEffectOfType(QStringLiteral("hsl"), 8)};
        tl.liveScenes[2].liveEffects = {makeEffectOfType(QStringLiteral("invert"), 7)};
        LiveOverlay spectrum;
        spectrum.kind = LiveOverlay::Kind::Spectrum;
        spectrum.width = 42;
        tl.liveScenes[2].overlays = {spectrum};
        const LiveScenes back = liveScenesFromJson(liveScenesToJson(tl.liveScenes));
        const QJsonObject json = liveScenesToJson(tl.liveScenes)[2].toObject();
        check(back == tl.liveScenes && back[2].overlays[0].kind == LiveOverlay::Kind::Spectrum && back[2].overlays[0].width == 42
                  && json[QStringLiteral("layerSource")].toString() == QStringLiteral("Camera") && json.contains(QStringLiteral("layer3NoEffects"))
                  && !json.contains(QStringLiteral("layer2Source")),
              QStringLiteral("scenes (Live FX, pad, layers with No Effects, overlays) and a spectrum overlay come back from the project file "
                             "(layer 1 under the keys one layer had)"));
    }
    // ---- The Live pad sequencer's loop: which pad is on when, and saved with the project ----
    {
        LiveSequence s; // 4 bars of 4/4: 96 ticks a beat
        s.blocks = {{3, 0, 192}, {5, 96, 96}, {7, 1536 - 96, 96}};
        LiveSequence waltz = s;
        waltz.beatsPerBar = 3;
        waltz.beatUnit = 8;
        waltz.snap = -16;
        const bool ticks = s.beatTicks() == 96 && s.loopTicks() == 1536 && s.snapTicks() == 24 && waltz.beatTicks() == 48 && waltz.barTicks() == 144
                           && waltz.snapTicks() == 16;
        const bool pads = s.padAt(0) == 3 && s.padAt(100) == 5 && s.padAt(191) == 5 && s.padAt(192) == -1 && s.padAt(1500) == 7
                          && s.padAt(1536 + 10) == 3 && s.padAt(-10) == 7; // (the later start wins; the loop wraps)
        s.on = true;
        LiveSequence bad = liveSequenceFromJson(QJsonObject{{QStringLiteral("bars"), 900}, {QStringLiteral("beatUnit"), 7},
                                                            {QStringLiteral("snap"), 3}, {QStringLiteral("blocks"), QJsonArray{QJsonArray{20, 0, 96}, QJsonArray{1, 0, 0}, QJsonArray{2, 2147483000, 1000}}}});
        Timeline tl;
        tl.liveSequence = s;
        Project p;
        p.applyEdit(tl, QStringLiteral("Sequence"));
        const QString file = QDir::temp().filePath(QStringLiteral("mvp-sequence-test.mvp"));
        Project back;
        QString err;
        const bool saved = p.save(file, &err) && back.load(file, &err);
        QFile::remove(file);
        check(ticks && pads && liveSequenceFromJson(liveSequenceToJson(s)) == s && saved && back.timeline().liveSequence == s && bad.bars == 64
                  && bad.beatUnit == 4 && bad.snap == 16 && bad.blocks.empty(),
              QStringLiteral("the pad sequencer's loop: ticks for 4/4 and 3/8, the pad on at each tick (looping), saved with the project, "
                             "and nonsense in a file read as limits"));
    }
    // ---- Portrait / square output: a landscape picture framed into it ----
    {
        // 16:9, a red band down the middle between blue sides.
        QImage wide(1920, 1080, QImage::Format_ARGB32_Premultiplied);
        wide.fill(QColor(20, 40, 220));
        QPainter(&wide).fillRect(QRect(800, 0, 320, 1080), QColor(220, 30, 30));
        const QSize portrait(1080, 1920);
        const QImage crop = frameToCanvas(wide.scaled(coverSize(wide.size(), portrait)), portrait, LiveFraming::CropFill);
        const QImage fit = frameToCanvas(wide, portrait, LiveFraming::Fit);
        const QImage blur = frameToCanvas(wide, portrait, LiveFraming::FitBlur);
        auto isRed = [](QRgb c) { return qRed(c) > 150 && qBlue(c) < 90; };
        auto isBlue = [](QRgb c) { return qBlue(c) > 150 && qRed(c) < 90; };
        // Crop: the middle of the picture fills it (red down the centre, blue at the far edges).
        const bool cropOk = crop.size() == portrait && isRed(crop.pixel(540, 960)) && isRed(crop.pixel(540, 50)) && isBlue(crop.pixel(10, 960));
        // Fit: the whole picture across the middle, black above and below.
        const QRgb top = fit.pixel(540, 100);
        const bool fitOk = fit.size() == portrait && qRed(top) + qGreen(top) + qBlue(top) < 10 && isRed(fit.pixel(540, 960)) && isBlue(fit.pixel(20, 960));
        // Blurred background: not black above, darker than the picture itself.
        const QRgb behind = blur.pixel(540, 100);
        const bool blurOk = blur.size() == portrait && qRed(behind) + qGreen(behind) + qBlue(behind) > 60 && qRed(behind) < 200 && isRed(blur.pixel(540, 960));
        const QImage square = frameToCanvas(wide, QSize(1080, 1080), LiveFraming::CropFill);
        check(cropOk && fitOk && blurOk && square.size() == QSize(1080, 1080) && coverSize(QSize(1920, 1080), portrait).height() >= 1920,
              QStringLiteral("portrait output: a 16:9 picture cropped to fill 1080x1920 (the middle), fitted with black bars, or "
                             "fitted on a blurred copy of itself; a square one too"));
        // Pan and zoom: which part of the source fills the frame.
        LivePanZoom left;
        left.center = QPointF(0.2, 0.5);
        const QImage panned = frameToCanvas(wide, portrait, LiveFraming::CropFill, left);
        const bool panOk = isBlue(panned.pixel(540, 960)); // the blue left of the band, at the middle now
        LivePanZoom edge;
        edge.center = QPointF(0.0, 0.5);
        QPointF used;
        const QRectF r = sourceRectInCanvas(QSizeF(wide.size()), QSizeF(portrait), LiveFraming::CropFill, edge, &used);
        const bool clampOk = std::abs(r.x()) < 0.5 && used.x() > 0.1 && used.x() < 0.2; // as far left as it goes, no gap
        LivePanZoom twice;
        twice.zoom = 2;
        const QImage zoomed = frameToCanvas(wide.scaled(coverSize(wide.size(), portrait) * 2), portrait, LiveFraming::CropFill, twice);
        const bool zoomOk = zoomed.size() == portrait && isRed(zoomed.pixel(10, 960)) && isRed(zoomed.pixel(1070, 960)); // the band fills it
        LivePanZoom four;
        four.zoom = 4;
        const QImage digital = frameToCanvas(wide, QSize(1920, 1080), LiveFraming::CropFill, four); // a normal size: digital zoom
        const bool digitalOk = digital.size() == QSize(1920, 1080) && isRed(digital.pixel(400, 540)) && isBlue(wide.pixel(400, 540));
        const QImage fitZoom = frameToCanvas(wide, portrait, LiveFraming::Fit, twice);
        const QRgb fitTop = fitZoom.pixel(540, 100);
        const bool fitZoomOk = qRed(fitTop) + qGreen(fitTop) + qBlue(fitTop) < 10 && isRed(fitZoom.pixel(540, 960)) && isRed(fitZoom.pixel(400, 960));
        check(panOk && clampOk && zoomOk && digitalOk && fitZoomOk && frameToCanvas(wide, wide.size(), LiveFraming::CropFill, {}).cacheKey() == wide.cacheKey(),
              QStringLiteral("pan and zoom: the source moved left in a portrait frame, stopped at its edge (centre %1), zoomed 2x "
                             "and 4x (a digital zoom at a normal size), zoomed in a fit with bars; untouched by default").arg(used.x(), 0, 'f', 3));
    }
    // ---- Setlists are saved with the project ----
    {
        std::vector<LiveSong> songs(2);
        songs[0].name = QStringLiteral("Opener");
        songs[0].padSet = QStringLiteral("Warm");
        songs[0].bpm = 128.5;
        songs[0].scene = 3;
        songs[0].notes = QStringLiteral("capo 2");
        songs[1].name = QStringLiteral("Slow one");
        songs[1].lockBpm = false;
        check(liveSetlistFromJson(liveSetlistToJson(songs)) == songs,
              QStringLiteral("a setlist (pad set, tempo and its lock, scene, notes per song) comes back from the project file"));
    }
    // ---- Spout: a sender other apps find by name, with the picture in its shared texture ----
    if (SpoutSender::available()) {
        QImage pic(320, 180, QImage::Format_RGB32);
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x)
                pic.setPixel(x, y, qRgb(x * 255 / 319, y * 255 / 179, (x + y) & 255));
        SpoutSender a, b;
        QString error;
        const bool opened = a.open(QStringLiteral("Mutant Video Producer Test"), &error) && b.open(QStringLiteral("Mutant Video Producer Test"), &error);
        const QStringList listed = spout::senders();
        a.send(pic);
        const QImage got = spout::receive(a.name(), &error);
        bool same = got.size() == pic.size();
        for (int y = 0; same && y < 180; y += 3)
            for (int x = 0; same && x < 320; x += 3)
                same = (got.pixel(x, y) & 0xffffff) == (pic.pixel(x, y) & 0xffffff);
        // A new size follows.
        a.send(pic.scaled(640, 360));
        const QImage bigger = spout::receive(a.name());
        a.close();
        const QStringList after = spout::senders();
        check(opened && listed.contains(QStringLiteral("Mutant Video Producer Test")) && listed.contains(QStringLiteral("Mutant Video Producer Test_1"))
                  && b.name() == QStringLiteral("Mutant Video Producer Test_1") && same && bigger.size() == QSize(640, 360)
                  && !after.contains(QStringLiteral("Mutant Video Producer Test")) && after.contains(QStringLiteral("Mutant Video Producer Test_1")),
              QStringLiteral("Spout: senders \"%1\" and \"%2\" are listed; another app reads the picture exactly from the shared texture%3; "
                             "a new size follows; closing one takes it off the list")
                  .arg(a.name().isEmpty() ? QStringLiteral("Mutant Video Producer Test") : QStringLiteral("Mutant Video Producer Test"), b.name())
                  .arg(error.isEmpty() ? QString() : QStringLiteral(" (") + error + QLatin1Char(')')));
        b.close();
    } else {
        print(QStringLiteral("SKIP  Spout: no Direct3D 11 here"));
    }
    // ---- The OBS Virtual Camera: what a camera app sees through it ----
    if (VirtualCamera::installed()) {
        VirtualCamera cam;
        QString error;
        const bool opened = cam.open(QSize(640, 360), 30, &error);
        // Quadrants: white, black, red, blue.
        QImage pic(640, 360, QImage::Format_RGB32);
        {
            QPainter p(&pic);
            p.fillRect(0, 0, 320, 180, Qt::white);
            p.fillRect(320, 0, 320, 180, Qt::black);
            p.fillRect(0, 180, 320, 180, QColor(220, 30, 30));
            p.fillRect(320, 180, 320, 180, QColor(30, 30, 220));
        }
        std::atomic<bool> feeding{true};
        std::thread feeder([&] {
            while (feeding) {
                cam.send(pic);
                QThread::msleep(33);
            }
        });
        // Open it as a camera, the way Zoom or OBS would (DirectShow).
        LiveSource camSource;
        for (const LiveSource& s : listLiveSources())
            if (s.kind == LiveSource::Kind::Device && s.name.contains(VirtualCamera::cameraName()))
                camSource = s;
        QImage seen;
        QString capError;
        if (opened && !camSource.name.isEmpty()) {
            LiveCapture capture;
            if (capture.start(camSource, {}, {}, &capError)) {
                QElapsedTimer t;
                t.start();
                uint64_t serial = 0;
                while (t.elapsed() < 8000) {
                    const QImage img = capture.latest(&serial);
                    if (!img.isNull() && serial > 5) {
                        seen = img;
                        break;
                    }
                    QThread::msleep(30);
                }
                capture.stop();
            }
        }
        feeding = false;
        feeder.join();
        cam.close();
        auto near = [](QRgb a, QColor b) {
            return std::abs(qRed(a) - b.red()) < 30 && std::abs(qGreen(a) - b.green()) < 30 && std::abs(qBlue(a) - b.blue()) < 30;
        };
        const bool right = !seen.isNull() && near(seen.pixel(seen.width() / 4, seen.height() / 4), Qt::white)
                           && near(seen.pixel(seen.width() * 3 / 4, seen.height() / 4), Qt::black)
                           && near(seen.pixel(seen.width() / 4, seen.height() * 3 / 4), QColor(220, 30, 30))
                           && near(seen.pixel(seen.width() * 3 / 4, seen.height() * 3 / 4), QColor(30, 30, 220));
        check(opened && right,
              QStringLiteral("OBS Virtual Camera: a camera app opening \"%1\" sees Mutant Video Producer's picture (%2x%3, colours right)%4")
                  .arg(camSource.name)
                  .arg(seen.width())
                  .arg(seen.height())
                  .arg(!error.isEmpty() ? QStringLiteral(": ") + error
                       : !capError.isEmpty() ? QStringLiteral(": ") + capError
                       : camSource.name.isEmpty() ? QStringLiteral(": the camera isn't listed") : QString()));
        // Closed: free for OBS's own virtual camera (or the next session).
        VirtualCamera again;
        QElapsedTimer freeTimer;
        freeTimer.start();
        bool free = false;
        while (!(free = again.open(QSize(320, 180), 30, &error)) && freeTimer.elapsed() < 3000)
            QThread::msleep(50); // the camera app lets go of it
        check(free, QStringLiteral("the virtual camera is free again once closed"));
    } else {
        print(QStringLiteral("SKIP  OBS Virtual Camera: OBS Studio isn't installed"));
    }
    // ---- Streaming: to a local RTMP server (FFmpeg listening), as to Twitch or YouTube ----
    {
        const QString ffmpegExe = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../../../deps/ffmpeg/bin/ffmpeg.exe"));
        if (!QFileInfo::exists(ffmpegExe)) {
            print(QStringLiteral("SKIP  streaming: no ffmpeg.exe to act as the server"));
        } else {
            const QString received = QDir(outDir).filePath(QStringLiteral("stream_received.flv"));
            QFile::remove(received);
            QProcess server;
            server.start(ffmpegExe, {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y"),
                                     QStringLiteral("-listen"), QStringLiteral("1"), QStringLiteral("-i"),
                                     QStringLiteral("rtmp://127.0.0.1:19356/live/mvp-test"), QStringLiteral("-c"), QStringLiteral("copy"), received});
            server.waitForStarted(5000);
            // Wait until it listens (a test connection would use up its one -listen 1 connection); slow after a reboot.
            QElapsedTimer wait;
            wait.start();
            while (wait.elapsed() < 15000) {
                QProcess netstat;
                netstat.start(QStringLiteral("netstat"), {QStringLiteral("-an"), QStringLiteral("-p"), QStringLiteral("TCP")});
                netstat.waitForFinished(5000);
                if (QString::fromLocal8Bit(netstat.readAllStandardOutput()).contains(QRegularExpression(QStringLiteral("127\\.0\\.0\\.1:19356\\s.*LISTENING"))))
                    break;
                QThread::msleep(200);
            }
            LiveRecorder stream;
            QString error;
            const bool started = stream.startStream(QStringLiteral("rtmp://127.0.0.1:19356/live/mvp-test"), QSize(640, 360), 30, 2500, &error);
            const double t0 = liveClock();
            int frames = 0;
            QElapsedTimer clock;
            clock.start();
            while (clock.elapsed() < 5000) { // 5 s of a moving bar, in real time
                QImage f(640, 360, QImage::Format_RGB32);
                f.fill(QColor(30, 30, 60));
                QPainter(&f).fillRect(QRect(frames * 4 % 640, 100, 40, 160), QColor(240, 200, 40));
                stream.addFrame(f, t0 + frames / 30.0);
                ++frames;
                const qint64 next = qint64(frames * 1000 / 30);
                if (next > clock.elapsed())
                    QThread::msleep(unsigned(next - clock.elapsed()));
            }
            const bool connected = stream.isConnected();
            QElapsedTimer stopTimer;
            stopTimer.start();
            stream.finish(&error);
            const qint64 stopMs = stopTimer.elapsed();
            server.waitForFinished(8000);
            if (server.state() != QProcess::NotRunning)
                server.kill();
            // What arrived: H.264 and AAC, ~5 s, a keyframe every 2 s.
            ff::InputFormatPtr in;
            double seconds = 0;
            int videoPackets = 0, audioPackets = 0;
            std::vector<double> keys;
            bool h264 = false, aac = false;
            if (QFileInfo(received).size() > 0 && ff::openInput(received, in) >= 0) {
                for (unsigned i = 0; i < in->nb_streams; ++i) {
                    h264 |= in->streams[i]->codecpar->codec_id == AV_CODEC_ID_H264;
                    aac |= in->streams[i]->codecpar->codec_id == AV_CODEC_ID_AAC;
                }
                ff::PacketPtr pkt = ff::makePacket();
                while (av_read_frame(in.get(), pkt.get()) >= 0) {
                    const AVStream* st = in->streams[pkt->stream_index];
                    const double t = pkt->pts == AV_NOPTS_VALUE ? 0 : pkt->pts * av_q2d(st->time_base);
                    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                        ++videoPackets;
                        seconds = std::max(seconds, t);
                        if (pkt->flags & AV_PKT_FLAG_KEY)
                            keys.push_back(t);
                    } else {
                        ++audioPackets;
                    }
                    av_packet_unref(pkt.get());
                }
            }
            double worstGap = 0;
            for (size_t i = 1; i < keys.size(); ++i)
                worstGap = std::max(worstGap, keys[i] - keys[i - 1]);
            check(started && connected && h264 && aac && videoPackets >= 120 && audioPackets > 100 && seconds > 4 && keys.size() >= 2
                      && worstGap < 2.2 && stopMs < 4000,
                  QStringLiteral("streaming to an RTMP server (%1): it received %2 pictures (%3 s, H.264) and %4 sound packets (AAC, "
                                 "silence without an input), keyframes every %5 s at most; stopping took %6 ms%7")
                      .arg(stream.encoderName())
                      .arg(videoPackets)
                      .arg(seconds, 0, 'f', 1)
                      .arg(audioPackets)
                      .arg(worstGap, 0, 'f', 2)
                      .arg(stopMs)
                      .arg(error.isEmpty() ? QString() : QStringLiteral(" (") + error + QLatin1Char(')')));
            // Nobody listening: it says so, quickly, and the pictures just go.
            LiveRecorder nowhere;
            QString why;
            nowhere.startStream(QStringLiteral("rtmp://127.0.0.1:19357/live/nobody"), QSize(320, 180), 30, 1000, &why);
            QElapsedTimer failTimer;
            failTimer.start();
            while (nowhere.streamError().isEmpty() && failTimer.elapsed() < 15000) {
                QImage f(320, 180, QImage::Format_RGB32);
                f.fill(Qt::gray);
                nowhere.addFrame(f);
                QThread::msleep(33);
            }
            const QString failure = nowhere.streamError();
            const bool stoppedSending = !nowhere.isRecording();
            failTimer.restart();
            nowhere.finish();
            const qint64 finishMs = failTimer.elapsed();
            QString bad;
            LiveRecorder wrong;
            const bool refused = !wrong.startStream(QStringLiteral("http://example.com/live"), QSize(320, 180), 30, 1000, &bad);
            check(!failure.isEmpty() && stoppedSending && finishMs < 2000 && refused,
                  QStringLiteral("no server: \"%1\", the stream stops by itself and Stop is quick (%2 ms); a web address is refused (%3)")
                      .arg(failure).arg(finishMs).arg(bad));
            QFile::remove(received);
        }
    }
    print(QStringLiteral("%1  live: %2 failure(s)").arg(failures ? QStringLiteral("FAIL") : QStringLiteral("DONE")).arg(failures));
    return failures ? 1 : 0;
}

int cmdRandom(const QString& media = QString(), const QString& outDir = QString())
{
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    const QStringList unknown = randomChainUnknownParams();
    check(unknown.isEmpty(), QStringLiteral("every effect and parameter in the pools exists (%1)").arg(unknown.join(QStringLiteral(", "))));
    const int seeds = 30;
    const int sizes[4][2] = {{2, 2}, {3, 3}, {4, 5}, {6, 8}};
    double lastChange = 0;
    for (int t = 0; t < 4; ++t) {
        const RandomTier tier = RandomTier(t);
        int sizeOk = 0, idsOk = 0, rangeOk = 0, inBounds = 0, notFlat = 0, named = 0, unique = 0, light = 0;
        double change = 0, minDetail = 1e9;
        QSet<QString> chains, names;
        QStringList examples;
        for (int s = 0; s < seeds; ++s) {
            int next = 1000;
            const RandomChain c = makeRandomChain(tier, uint32_t(s * 7919 + t), [&] { return next++; });
            const int n = int(c.effects.size());
            sizeOk += n >= sizes[t][0] && n <= sizes[t][1];
            bool ids = true, ranges = true;
            QSet<QString> kinds;
            QStringList list;
            for (size_t i = 0; i < c.effects.size(); ++i) {
                const EffectInstance& e = c.effects[i];
                ids = ids && e.id == 1000 + int(i);
                kinds.insert(e.type);
                list << e.type;
                const EffectDef* def = findEffect(e.type);
                if (!def) {
                    ranges = false;
                    continue;
                }
                for (const ParamDef& pd : def->params)
                    if (pd.type == ParamType::Number && e.params.count(pd.id)) {
                        const double v = e.params.at(pd.id).value;
                        ranges = ranges && v >= pd.min - 1e-9 && v <= pd.max + 1e-9;
                    }
            }
            idsOk += ids;
            rangeOk += ranges;
            light += kinds.contains(QStringLiteral("pixel_sort")) + kinds.contains(QStringLiteral("jpeg_glitch"))
                         + kinds.contains(QStringLiteral("datamosh")) <= 1;
            unique += kinds.size() == n;
            named += !c.name.isEmpty();
            change += c.change / seeds;
            minDetail = std::min(minDetail, c.detail);
            notFlat += c.detail >= 8;
            const double lo[] = {3, 10, 18, 28}, hi[] = {40, 70, 110, 160};
            inBounds += c.change >= lo[t] && c.change <= hi[t];
            chains.insert(list.join(QLatin1Char(',')));
            names.insert(c.name);
            if (s < 2)
                examples << QStringLiteral("%1: %2").arg(c.name, list.join(QStringLiteral(" > ")));
        }
        const QString tn = randomTierName(tier);
        check(sizeOk == seeds, QStringLiteral("%1: %2/%3 chains have %4-%5 effects").arg(tn).arg(sizeOk).arg(seeds).arg(sizes[t][0]).arg(sizes[t][1]));
        check(idsOk == seeds && rangeOk == seeds && unique == seeds && named == seeds && light == seeds,
              QStringLiteral("%1: new ids %2, values in range %3, no repeats %4, named %5, one slow effect at most %6 (of %7)")
                  .arg(tn).arg(idsOk).arg(rangeOk).arg(unique).arg(named).arg(light).arg(seeds));
        check(inBounds >= seeds * 8 / 10 && notFlat == seeds,
              QStringLiteral("%1: %2/%3 change the picture as much as the tier should, mean change %4, least detail left %5 (%6)")
                  .arg(tn).arg(inBounds).arg(seeds).arg(change, 0, 'f', 1).arg(minDetail, 0, 'f', 1).arg(examples.join(QStringLiteral("; "))));
        check(names.size() >= seeds * 2 / 3, QStringLiteral("%1: %2 different names from %3 seeds").arg(tn).arg(names.size()).arg(seeds));
        // Low has only two effects from small pools, so the same pair can come up again (with other settings).
        check(chains.size() >= (t == 0 ? seeds / 2 : seeds * 2 / 3), QStringLiteral("%1: %2 different effect lists from %3 seeds").arg(tn).arg(chains.size()).arg(seeds));
        check(change > lastChange, QStringLiteral("%1 changes the picture more than the tier below (%2 > %3)").arg(tn).arg(change, 0, 'f', 1).arg(lastChange, 0, 'f', 1));
        lastChange = change;
    }
    int a = 1, b = 1;
    const RandomChain x = makeRandomChain(RandomTier::High, 42, [&] { return a++; });
    const RandomChain y = makeRandomChain(RandomTier::High, 42, [&] { return b++; });
    check(x.effects == y.effects && x.name == y.name, QStringLiteral("the same seed gives the same chain"));
    // A contact sheet of each tier on a real picture: rows Low..Insane.
    if (!media.isEmpty()) {
        const MediaInfo info = probeMedia(media);
        VideoDecoder dec;
        QString err;
        QImage frame;
        if (info.hasVideo() && dec.open(media, info.video.front().index, &err))
            if (const AVFrame* f = dec.frameAt(info.duration / 3))
                frame = FrameConverter().convert(f, QSize(320, 180), false).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (!frame.isNull()) {
            const int cols = 5;
            QImage sheet(cols * 324, 4 * 200, QImage::Format_RGB32);
            sheet.fill(QColor(30, 30, 32));
            QPainter p(&sheet);
            p.setPen(Qt::white);
            for (int t = 0; t < 4; ++t)
                for (int k = 0; k < cols; ++k) {
                    int id = 1;
                    const RandomChain c = makeRandomChain(RandomTier(t), uint32_t(100 + k * 31 + t), [&] { return id++; });
                    Compositor comp(false);
                    QImage out;
                    for (int f = 0; f < 6; ++f)
                        out = comp.renderLive(frame, c.effects, TimeUs(f) * 33333, 30);
                    p.drawImage(k * 324 + 2, t * 200 + 2, out);
                    p.drawText(QRect(k * 324 + 2, t * 200 + 182, 320, 18), Qt::AlignLeft | Qt::AlignVCenter,
                               QStringLiteral("%1: %2 (%3)").arg(randomTierName(RandomTier(t)), c.name).arg(c.effects.size()));
                }
            p.end();
            sheet.save(QDir(outDir).filePath(QStringLiteral("random_tiers.png")));
        }
        check(!frame.isNull(), QStringLiteral("a picture of the tiers is saved (%1)").arg(QDir(outDir).filePath(QStringLiteral("random_tiers.png"))));
    }
    print(QStringLiteral("%1  random: %2 failure(s)").arg(failures ? QStringLiteral("FAIL") : QStringLiteral("DONE")).arg(failures));
    return failures ? 1 : 0;
}

// Editor tools: beat markers, splitting at markers, beat montage, effect masks,
// stabilisation and motion tracking, on material made here with known answers.
// Auto captions: Whisper hears `speech` (a WAV of "Welcome to Mutant Video Producer. This
// video editor writes its own captions. The quick brown fox jumps over the lazy dog.").
// With `dialogue` (two voices taking turns) and a "tdrz" model, the speaker turns too.
int cmdCaptions(const QString& speech, const QString& model, const QString& dialogue, const QString& turnsModel)
{
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    auto item = loadItem(speech, 1);
    if (!item || item->info.audio.empty() || !whisperAvailable()) {
        print(QStringLiteral("FAIL  no speech file or no Whisper in this build"));
        return 1;
    }
    ProjectSnapshot s;
    s.media[1] = item;
    Track a;
    a.id = 2;
    a.type = TrackType::Audio;
    Clip c;
    c.id = 3;
    c.mediaId = 1;
    c.streamIndex = item->info.audio.front().index;
    c.start = 2 * kUsPerSecond; // two seconds of silence first
    c.length = item->info.duration;
    a.clips.push_back(c);
    s.timeline.tracks.push_back(a);
    const std::vector<float> audio = timelineSpeechAudio(s, 0, c.end());
    {
        // Music ducked under the voice: the speech is the voice alone (the music between lines isn't words).
        ProjectSnapshot m = s;
        Track music;
        music.id = 4;
        music.type = TrackType::Audio;
        Clip after = c;
        after.id = 5;
        after.start = c.end();
        after.length = std::min<TimeUs>(item->info.duration, 3 * kUsPerSecond);
        music.clips.push_back(after);
        m.timeline.tracks.push_back(music);
        auto energy = [&](const ProjectSnapshot& snap) {
            const std::vector<float> a = timelineSpeechAudio(snap, c.end(), after.end());
            double e = 0;
            for (float v : a)
                e += double(v) * v;
            return e;
        };
        const double plain = energy(m);
        m.timeline.tracks.back().duckUnder = 2;
        const double ducked = energy(m);
        check(plain > 1.0 && ducked < plain * 1e-3,
              QStringLiteral("speech for captions: music ducked under the voice is left out (%1 -> %2)").arg(plain, 0, 'g', 3).arg(ducked, 0, 'g', 3));
    }
    QElapsedTimer timer;
    timer.start();
    std::vector<Caption> caps;
    QString err;
    TranscribeOptions english;
    english.language = QStringLiteral("en");
    const bool ok = transcribe(model, audio, english, [](double) { return true; }, caps, &err);
    QStringList words;
    for (const Caption& cap : caps)
        words << cap.text;
    const QString all = words.join(QLatin1Char(' ')).toLower();
    int longest = 0;
    for (const Caption& cap : caps)
        longest = std::max(longest, int(cap.text.size()));
    check(ok && audio.size() == size_t(usToSamples(c.end(), 16000)) && all.contains(QStringLiteral("mutant video producer")) && all.contains(QStringLiteral("brown fox"))
              && all.contains(QStringLiteral("lazy dog")) && caps.size() >= 3 && caps.front().start >= 15 * kUsPerSecond / 10 && longest <= 50,
          QStringLiteral("Whisper (%7) captions the speech in %1 ms (%2 captions, the first at %3 s, longest %4 characters): \"%5\" %6")
              .arg(timer.elapsed()).arg(caps.size()).arg(caps.empty() ? 0.0 : double(caps.front().start) / kUsPerSecond, 0, 'f', 2)
              .arg(longest).arg(all, err, whisperOnGpu() ? QStringLiteral("GPU") : QStringLiteral("CPU")));
    std::vector<Caption> none;
    const bool cancelled = !transcribe(model, audio, english, [](double) { return false; }, none, &err);
    check(cancelled && none.empty(), QStringLiteral("captioning can be cancelled (%1)").arg(err));
    if (!dialogue.isEmpty()) {
        auto talk = loadItem(dialogue, 5);
        ProjectSnapshot d;
        d.media[5] = talk;
        Track t;
        t.id = 6;
        t.type = TrackType::Audio;
        Clip k;
        k.id = 7;
        k.mediaId = 5;
        k.streamIndex = talk->info.audio.front().index;
        k.length = talk->info.duration;
        t.clips.push_back(k);
        d.timeline.tracks.push_back(t);
        TranscribeOptions turns = english;
        turns.speakerTurns = true;
        std::vector<Caption> said;
        const bool heard = transcribe(turnsModel, timelineSpeechAudio(d, 0, k.end()), turns, [](double) { return true; }, said, &err);
        int changes = 0;
        QStringList lines;
        for (size_t i = 0; i < said.size(); ++i) {
            changes += i > 0 && said[i].speaker != said[i - 1].speaker;
            lines << QStringLiteral("[%1] %2").arg(said[i].speaker).arg(said[i].text);
        }
        // (The turn model learnt from real meetings: synthetic voices may give none, so this only reports them.)
        check(heard && said.size() >= 3, QStringLiteral("speaker-turn captioning runs on a dialogue (%1 captions)").arg(said.size()));
        print(QStringLiteral("INFO  speaker turns found: %1: %2").arg(changes).arg(lines.join(QStringLiteral(" | "))));
    }
    return failures ? 1 : 0;
}

// VST3: an installed effect plugin loads, changes the sound, keeps its state, and runs in the mixer.
int cmdVst3(const QString& path)
{
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    print(QStringLiteral("INFO  installed: %1").arg(vst3InstalledPlugins().join(QStringLiteral(", "))));
    QString err;
    const QStringList classes = vst3EffectClasses(path, &err);
    std::unique_ptr<Vst3Plugin> plugin = Vst3Plugin::open(path, QString(), &err);
    check(vst3Available() && !classes.isEmpty() && plugin && plugin->prepare(48000, 512),
          QStringLiteral("VST3 plugin %1 loads (%2) %3").arg(QFileInfo(path).fileName(), classes.join(QStringLiteral(", ")), err));
    if (!plugin)
        return 1;
    // A second of a 440 Hz tone through it.
    std::vector<float> tone(48000 * 2), out;
    for (size_t i = 0; i < 48000; ++i)
        tone[i * 2] = tone[i * 2 + 1] = 0.4f * float(std::sin(2 * 3.14159265 * 440 * double(i) / 48000));
    out = tone;
    for (int pos = 0; pos < 48000; pos += 1000)
        plugin->process(out.data() + size_t(pos) * 2, 1000);
    double diff = 0, energy = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        diff += std::abs(out[i] - tone[i]);
        energy += std::abs(out[i]);
    }
    check(energy > 100 && std::isfinite(diff), QStringLiteral("it processes a tone (level %1, changed by %2 on average)")
                                                    .arg(energy / out.size(), 0, 'f', 3).arg(diff / out.size(), 0, 'f', 4));
    const QByteArray state = plugin->state();
    check(!state.isEmpty() && plugin->setState(state) && plugin->state() == state,
          QStringLiteral("its state saves and loads back (%1 bytes)").arg(state.size()));
#ifdef Q_OS_WIN
    {
        // Its editor, in a native window that is never shown.
        HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 800, 600, nullptr, nullptr, nullptr, nullptr);
        const QSize size = plugin->hasEditor() ? plugin->openEditor(parent) : QSize();
        plugin->closeEditor();
        DestroyWindow(parent);
        check(size.width() > 50 && size.height() > 50, QStringLiteral("its editor opens in a window (%1 x %2)").arg(size.width()).arg(size.height()));
    }
#endif
    // Through the mixer as the a_vst3 effect on an audio track.
    ProjectSnapshot s;
    const QString wav = QDir::temp().filePath(QStringLiteral("mvp_vst3_tone.wav"));
    writeWav(wav, std::vector<float>(tone.begin(), tone.end()), 48000);
    auto item = loadItem(wav, 1);
    s.media[1] = item;
    Track a;
    a.id = 2;
    a.type = TrackType::Audio;
    Clip c;
    c.id = 3;
    c.mediaId = 1;
    c.streamIndex = item->info.audio.front().index;
    c.length = kUsPerSecond;
    a.clips.push_back(c);
    EffectInstance fx = makeEffectOfType(QStringLiteral("a_vst3"), 4);
    fx.params[QStringLiteral("plugin")].text = path;
    fx.params[QStringLiteral("state")].text = QString::fromLatin1(state.toBase64());
    a.effects.push_back(fx);
    s.timeline.tracks.push_back(a);
    AudioMixer mixer(48000);
    std::vector<float> mixed(9600 * 2);
    double level = 0;
    for (int block = 0; block < 4; ++block) {
        mixer.mix(s, int64_t(block) * 9600, 9600, mixed.data());
        for (float v : mixed)
            level += std::abs(v);
    }
    check(level / (4 * 9600 * 2) > 0.05, QStringLiteral("the VST3 Plugin effect plays it on a track (level %1)").arg(level / (4 * 9600 * 2), 0, 'f', 3));
    QFile::remove(wav);
    return failures ? 1 : 0;
}

// Create Clip from YouTube: `url` (a video over a minute long) at 360p with yt-dlp (`ytdlp`), picture and sound joined.
int cmdYouTubeOptions()
{
    int failures = 0;
    auto check = [&](bool ok, const QString& what) { print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what)); if (!ok) ++failures; };
    YouTubeOptions o; o.url = QStringLiteral("https://www.youtube.com/watch?v=test"); o.folder = QStringLiteral("C:/a folder/downloads");
    o.jsRuntime = QStringLiteral("deno:C:/a folder/deno.exe");
    auto args = youtubeArguments(QStringLiteral("C:/ffmpeg folder"), o);
    check(!args.contains(QStringLiteral("--cookies")) && !args.contains(QStringLiteral("--cookies-from-browser")), QStringLiteral("public downloads never use a browser session without selection"));
    check(args.contains(o.jsRuntime) && args.value(args.indexOf(QStringLiteral("--ffmpeg-location")) + 1) == QStringLiteral("C:/ffmpeg folder")
          && args.last() == o.url && args.at(args.size() - 2) == QLatin1String("--"), QStringLiteral("runtime, FFmpeg and URL are separate arguments, including paths with spaces"));
    o.cookiesBrowser = QStringLiteral("firefox"); args = youtubeArguments({}, o);
    check(args.value(args.indexOf(QStringLiteral("--cookies-from-browser")) + 1) == QLatin1String("firefox"), QStringLiteral("selected browser authentication reaches yt-dlp"));
    o.cookiesFile = QStringLiteral("C:/a folder/cookies.txt"); args = youtubeArguments({}, o);
    check(!args.contains(QStringLiteral("--cookies-from-browser")) && args.value(args.indexOf(QStringLiteral("--cookies")) + 1) == o.cookiesFile,
          QStringLiteral("cookies-file authentication is exclusive and preserves paths with spaces"));
    o.maxHeight = 0; args = youtubeArguments({}, o);
    check(args.value(args.indexOf(QStringLiteral("-f")) + 1) == QLatin1String("ba/b") && !args.contains(QStringLiteral("--merge-output-format")), QStringLiteral("audio-only downloads retain audio format selection"));
    check(youtubeFailure(QStringLiteral("ERROR: Sign in to confirm you're not a bot. Use cookies-from-browser")) == YouTubeFailure::Authentication
          && youtubeFailure(QStringLiteral("Failed to decrypt Chrome cookies using DPAPI")) == YouTubeFailure::Cookies,
          QStringLiteral("session verification and browser decryption failures have different recovery instructions"));
    check(youtubeFailure(QStringLiteral("HTTP Error 429: Too Many Requests")) == YouTubeFailure::RateLimit
          && youtubeFailure(QStringLiteral("No supported JavaScript runtime")) == YouTubeFailure::JavaScript
          && youtubeFailure(QStringLiteral("GVS PO Token is required")) == YouTubeFailure::Token
          && youtubeFailure(QStringLiteral("Video unavailable")) == YouTubeFailure::Unavailable,
          QStringLiteral("rate limits, JavaScript challenges, tokens and unavailable videos are classified"));
    check(youtubeFailureHelp(QStringLiteral("Sign in to confirm you're not a bot")).contains(QStringLiteral("Updating the downloader alone will not fix")),
          QStringLiteral("bot-check errors do not send the user into an update loop"));
    return failures;
}

int cmdYouTube(const QString& ytdlp, const QString& url)
{
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    check(parseClock(QStringLiteral("1:30")) == 90 * kUsPerSecond && parseClock(QStringLiteral("1:02:03.5")) == 3723500000
              && parseClock(QStringLiteral("12")) == 12 * kUsPerSecond && parseClock(QString()) == -1 && parseClock(QStringLiteral("a:b")) == -1,
          QStringLiteral("times like 1:30, 1:02:03.5 and 12 read"));
    const QString folder = QDir::temp().filePath(QStringLiteral("mvp_youtube_test"));
    QDir(folder).removeRecursively();
    YouTubeOptions o;
    o.url = url;
    o.maxHeight = 360;
    o.folder = folder;
    QString err;
    double lastProgress = -1;
    QElapsedTimer t;
    t.start();
    const QString path = youtubeDownload(ytdlp, ffmpegFolder(), o, [&](double p) {
        lastProgress = std::max(lastProgress, p);
        return true;
    }, &err);
    const MediaInfo info = path.isEmpty() ? MediaInfo() : probeMedia(path);
    check(!path.isEmpty() && info.hasVideo() && info.hasAudio() && info.duration > 60 * kUsPerSecond && info.video.front().height <= 360
              && lastProgress > 0.99,
          QStringLiteral("the video downloads with picture and sound in %1 s (%2 s, %3p, %4) %5")
              .arg(t.elapsed() / 1000.0, 0, 'f', 1).arg(double(info.duration) / kUsPerSecond, 0, 'f', 2)
              .arg(info.hasVideo() ? info.video.front().height : 0).arg(QFileInfo(path).fileName(), err));
    QString cancelError;
    const QString none = youtubeDownload(ytdlp, ffmpegFolder(), o, [](double) { return false; }, &cancelError);
    YouTubeOptions bad = o;
    bad.url = QStringLiteral("https://www.youtube.com/watch?v=this-is-not-a-video");
    QString badError;
    const QString nothing = youtubeDownload(ytdlp, ffmpegFolder(), bad, [](double) { return true; }, &badError);
    check(none.isEmpty() && cancelError == QStringLiteral("cancelled") && nothing.isEmpty() && !badError.isEmpty(),
          QStringLiteral("a download can be cancelled; a wrong link fails with yt-dlp's message (%1)").arg(badError.left(120)));
    QDir(folder).removeRecursively();
    return failures ? 1 : 0;
}

// The user guide's animations (docs/animations): short GIFs rendered from the
// example projects, with Track Motion run on the effects example first.
int cmdDocAnimations(const QString& examplesDir, const QString& outDir)
{
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    QDir().mkpath(outDir);
    const ExportPreset* gif = nullptr;
    for (const ExportPreset& preset : exportPresets())
        if (preset.name.startsWith(QLatin1String("GIF - small for chat")))
            gif = &preset;
    auto render = [&](const QString& example, const QString& name, double from, double to, const std::function<void(Project&)>& prepare = {}) {
        Project p;
        QString err;
        bool ok = gif && p.load(QDir(examplesDir).filePath(example + QStringLiteral(".mvp")), &err);
        if (ok && prepare)
            prepare(p);
        ExportSettings es = ok ? settingsFromPreset(*gif, p.settings()) : ExportSettings{};
        es.path = QDir(outDir).filePath(name + QStringLiteral(".gif"));
        es.rangeStart = TimeUs(from * kUsPerSecond);
        es.rangeEnd = TimeUs(to * kUsPerSecond);
        Exporter exporter;
        ok = ok && exporter.run(*p.snapshot(), es);
        check(ok, QStringLiteral("%1.gif: %2 s of %3 (%4 KB) %5").arg(name).arg(to - from).arg(example).arg(QFileInfo(es.path).size() / 1024)
                      .arg(err + exporter.error()));
    };
    // Glitch, Ripple, Swirl, Card Flip, Cube Spin.
    render(QStringLiteral("transitions"), QStringLiteral("transitions"), 23.5, 33.5);
    // The ramp: slow in the middle.
    render(QStringLiteral("speed-and-time"), QStringLiteral("speed-ramp"), 18, 24.4);
    // Kinetic text, then the typewriter.
    render(QStringLiteral("titles-and-generators"), QStringLiteral("titles"), 9, 17);
    // The masked Pixelate on the second event, after Track Motion (as its button does it).
    render(QStringLiteral("effects-and-keyframes"), QStringLiteral("track-motion"), 12.5, 20.5, [&](Project& p) {
        Timeline tl = p.timeline();
        for (Track& t : tl.tracks)
            for (Clip& c : t.clips)
                for (EffectInstance& e : c.effects) {
                    auto mask = [&](const char* k) { return e.params[QString::fromLatin1(k)].value; };
                    if (mask("mask.shape") == 0 || !c.mediaId)
                        continue;
                    const QRectF box(mask("mask.x") - mask("mask.w") / 2, mask("mask.y") - mask("mask.h") / 2, mask("mask.w"), mask("mask.h"));
                    for (const tracker::Point& pt : tracker::track(p.mediaItem(c.mediaId)->path, c.streamIndex, c.sourceIn, c.sourceIn + c.length, box)) {
                        e.params[QStringLiteral("mask.x")].keys.push_back({pt.srcT - c.sourceIn, pt.centre.x(), Keyframe::Interp::Linear});
                        e.params[QStringLiteral("mask.y")].keys.push_back({pt.srcT - c.sourceIn, pt.centre.y(), Keyframe::Interp::Linear});
                    }
                }
        p.applyEdit(tl, QStringLiteral("track"));
    });
    return failures;
}

// The user guide (docs/): every link and picture points at something that
// exists (a heading for "#anchor" links, an example project in `examplesDir`
// for .mvp links), every page is linked from another, and the tutorials index
// lists every tutorial. With `srcDir`, every page the program opens (Help
// buttons, Shift+F1, the welcome window) is there too.
int cmdDocs(const QString& docsDir, const QString& examplesDir, const QString& srcDir)
{
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    // A heading's anchor as GitHub makes it (MainWindowHelp.cpp follows the same rule).
    auto anchor = [](const QString& heading) {
        QString s;
        for (const QChar c : heading.trimmed().toLower())
            if (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_'))
                s += c;
            else if (c == QLatin1Char(' '))
                s += QLatin1Char('-');
        return s;
    };
    const QDir docs(docsDir);
    std::map<QString, QStringList> anchors; // page -> its headings' anchors
    std::map<QString, QString> text;
    QDirIterator it(docsDir, {QStringLiteral("*.md")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = QFileInfo(it.next()).canonicalFilePath();
        QFile f(path);
        f.open(QIODevice::ReadOnly);
        text[path] = QString::fromUtf8(f.readAll());
        bool code = false;
        for (const QString& line : text[path].split(QLatin1Char('\n'))) {
            code ^= line.trimmed().startsWith(QLatin1String("```"));
            if (!code && line.startsWith(QLatin1Char('#')))
                anchors[path] << anchor(QString(line).remove(QRegularExpression(QStringLiteral("^#+"))));
        }
    }
    static const QRegularExpression link(QStringLiteral("!?\\[[^\\]]*\\]\\(([^)\\s]+)\\)"));
    QStringList broken;
    std::set<QString> linked;
    int links = 0, examples = 0;
    for (const auto& [path, body] : text) {
        for (auto m = link.globalMatch(body); m.hasNext();) {
            const QString target = m.next().captured(1);
            if (target.startsWith(QLatin1String("http")) || target.startsWith(QLatin1String("mailto:")))
                continue;
            ++links;
            const QString file = target.section(QLatin1Char('#'), 0, 0), fragment = target.section(QLatin1Char('#'), 1);
            const QString where = docs.relativeFilePath(path) + QStringLiteral(": ") + target;
            if (file.endsWith(QLatin1String(".mvp"))) {
                ++examples;
                if (!QFileInfo::exists(QDir(examplesDir).filePath(QFileInfo(file).fileName())))
                    broken << where;
                continue;
            }
            const QString to = file.isEmpty() ? path : QFileInfo(QFileInfo(path).dir().filePath(file)).canonicalFilePath();
            if (to.isEmpty()) {
                broken << where;
                continue;
            }
            linked.insert(to);
            if (!fragment.isEmpty() && !anchors[to].contains(fragment))
                broken << where;
        }
    }
    check(broken.isEmpty() && links > 100, QStringLiteral("%1 links and pictures in %2 pages lead somewhere, %3 of them to example projects %4")
                                              .arg(links).arg(text.size()).arg(examples).arg(broken.join(QStringLiteral(", "))));
    QStringList orphans;
    for (const auto& [path, body] : text)
        if (!linked.count(path) && QFileInfo(path).fileName() != QLatin1String("README.md"))
            orphans << docs.relativeFilePath(path);
    check(orphans.isEmpty(), QStringLiteral("every page is linked from another %1").arg(orphans.join(QStringLiteral(", "))));
    const QString index = QFileInfo(docs.filePath(QStringLiteral("tutorials/README.md"))).canonicalFilePath();
    QStringList missing;
    const QStringList tutorials = QDir(docs.filePath(QStringLiteral("tutorials"))).entryList({QStringLiteral("[0-9]*.md")}, QDir::Files, QDir::Name);
    for (const QString& t : tutorials)
        if (!text[index].contains(QStringLiteral("(%1)").arg(t)))
            missing << t;
    check(tutorials.size() >= 16 && missing.isEmpty(),
          QStringLiteral("the tutorials index lists all %1 tutorials %2").arg(tutorials.size()).arg(missing.join(QStringLiteral(", "))));
    if (!srcDir.isEmpty()) {
        static const QRegularExpression page(QStringLiteral("\"((?:tutorials/)?[a-z0-9-]+\\.md)(?:#([a-z0-9-]+))?\""));
        QStringList wrong;
        int pages = 0;
        QDirIterator src(srcDir, {QStringLiteral("*.cpp")}, QDir::Files, QDirIterator::Subdirectories);
        while (src.hasNext()) {
            QFile f(src.next());
            f.open(QIODevice::ReadOnly);
            for (auto m = page.globalMatch(QString::fromUtf8(f.readAll())); m.hasNext();) {
                const auto hit = m.next();
                ++pages;
                const QString to = QFileInfo(docs.filePath(hit.captured(1))).canonicalFilePath();
                if (to.isEmpty() || (!hit.captured(2).isEmpty() && !anchors[to].contains(hit.captured(2))))
                    wrong << QFileInfo(f.fileName()).fileName() + QStringLiteral(": ") + hit.captured(0);
            }
        }
        check(pages >= 30 && wrong.isEmpty(), QStringLiteral("the %1 guide pages the program opens are there %2").arg(pages).arg(wrong.join(QStringLiteral(", "))));
    }
    return failures;
}

// The built-in sound effects library (data/sfx, the Sounds tab): every sound plays, is short (0.05 to 10 s), and is in CREDITS.md.
int cmdSfx(const QString& dir)
{
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    QFile creditsFile(QDir(dir).filePath(QStringLiteral("CREDITS.md")));
    check(creditsFile.open(QIODevice::ReadOnly), QStringLiteral("the sound library has its credits"));
    const QString credits = QString::fromUtf8(creditsFile.readAll());
    QStringList bad, uncredited;
    int count = 0;
    QDirIterator it(dir, {QStringLiteral("*.ogg")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo f(it.next());
        ++count;
        const MediaInfo m = probeMedia(f.filePath());
        if (!m.ok() || m.audio.empty() || !m.video.empty() || m.duration < 50000 || m.duration > 10 * kUsPerSecond)
            bad << f.fileName();
        if (!credits.contains(QStringLiteral("**%1**").arg(f.completeBaseName())))
            uncredited << f.fileName();
    }
    check(count >= 50, QStringLiteral("the library has %1 sounds").arg(count));
    check(bad.isEmpty(), QStringLiteral("every sound is audio from 0.05 to 10 seconds %1").arg(bad.join(QStringLiteral(", "))));
    check(uncredited.isEmpty(), QStringLiteral("every sound is credited %1").arg(uncredited.join(QStringLiteral(", "))));
    return failures;
}

// The example projects behind Help > Tutorials (data/examples): their media made
// here (music, a sample clip with a moving shape and a timecode, and
// `narration`, a recording of speech), each project built with the same model
// the editor edits, saved with paths relative to its folder, loaded back and
// rendered. Re-run after a change to the project format.
// The README's effects sheet: each "<effect id or node:Name>|<picture>|<label>" drawn (a few frames in, for the effects
// that move) as a tile, `cols` across.
int cmdFxShowcase(const QStringList& args)
{
    nodes::loadUserEffects();
    const QString out = args[0];
    const int cols = std::max(1, args[1].toInt()), tw = 640, th = 360, gap = 8;
    std::vector<QImage> tiles;
    for (int i = 2; i < args.size(); ++i) {
        const QStringList spec = args[i].split(QLatin1Char('|'));
        const QString id = spec[0].startsWith(QLatin1String("node:")) ? nodes::effectId(spec[0].mid(5)) : spec[0];
        const EffectDef* def = findEffect(id);
        if (!def)
            def = nodes::findUserEffect(id);
        const QImage in = QImage(spec.value(1)).convertToFormat(QImage::Format_ARGB32_Premultiplied).scaled(1280, 720, Qt::IgnoreAspectRatio,
                                                                                                           Qt::SmoothTransformation);
        if (!def || in.isNull()) {
            print(QStringLiteral("fxshowcase: no effect %1 or picture %2").arg(spec[0], spec.value(1)));
            return 1;
        }
        const EffectChain chain{makeEffect(*def, 1)};
        Compositor comp(false);
        QImage result;
        for (int f = 0; f < 15; ++f)
            result = comp.renderLive(in, chain, TimeUs(f) * 33333, 30.0);
        QImage tile = result.scaled(tw, th, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
        QPainter p(&tile);
        QFont f = p.font();
        f.setPixelSize(17);
        f.setBold(true);
        p.setFont(f);
        const QString label = spec.value(2, def->name);
        const QRect bar(0, th - 30, p.fontMetrics().horizontalAdvance(label) + 20, 30);
        p.fillRect(bar, QColor(12, 10, 20, 200));
        p.setPen(Qt::white);
        p.drawText(bar, Qt::AlignCenter, label);
        p.end();
        tiles.push_back(tile);
        print(QStringLiteral("%1: %2").arg(label, def->name));
    }
    const int rows = int((tiles.size() + size_t(cols) - 1) / size_t(cols));
    QImage sheet(cols * tw + (cols - 1) * gap, rows * th + (rows - 1) * gap, QImage::Format_RGB32);
    sheet.fill(QColor(18, 18, 24));
    QPainter p(&sheet);
    for (size_t k = 0; k < tiles.size(); ++k)
        p.drawImage(int(k % size_t(cols)) * (tw + gap), int(k / size_t(cols)) * (th + gap), tiles[k]);
    p.end();
    return sheet.save(out) ? 0 : 1;
}

int cmdExamples(const QString& outDir, const QString& narrationWav)
{
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    const QDir out(QDir(outDir).absolutePath()); // (relative paths in the projects are worked out from it)
    QDir().mkpath(out.filePath(QStringLiteral("media")));
    auto S = [](double s) { return TimeUs(std::llround(s * kUsPerSecond)); };
    QStringList unknown; // names (effects, settings, presets...) that don't exist

    // ---- Building blocks ----
    auto gen = [&](Timeline& tl, int track, const QString& preset, double start, double length,
                   const std::map<QString, QString>& texts = {}, const std::map<QString, double>& numbers = {}) -> Clip* {
        const GeneratorPreset* p = findGeneratorPreset(preset);
        if (!p) {
            unknown << preset;
            return nullptr;
        }
        Clip c;
        c.id = tl.allocId();
        c.start = S(start);
        c.length = S(length);
        c.generator = makeGenerator(*p);
        const GeneratorDef* def = findGenerator(c.generator.type);
        auto known = [&](const QString& k) { return std::any_of(def->params.begin(), def->params.end(), [&](const ParamDef& d) { return d.id == k; }); };
        for (const auto& [k, v] : texts) {
            if (!known(k))
                unknown << preset + QLatin1Char('.') + k;
            c.generator.params[k].text = v;
        }
        for (const auto& [k, v] : numbers) {
            if (!known(k))
                unknown << preset + QLatin1Char('.') + k;
            c.generator.params[k].value = v;
        }
        tl.tracks[size_t(track)].clips.push_back(c);
        tl.sortClips();
        return tl.findClip(c.id);
    };
    auto fx = [&](Timeline& tl, const QString& type, const std::map<QString, double>& numbers = {}) {
        EffectInstance e = makeEffectOfType(type, tl.allocId());
        const auto meta = effectMeta(type);
        if (!meta) {
            unknown << type;
            return e;
        }
        for (const auto& [k, v] : numbers) {
            auto has = [&](const std::vector<ParamDef>& defs) { return std::any_of(defs.begin(), defs.end(), [&](const ParamDef& d) { return d.id == k; }); };
            if (!has(*meta->params) && !has(effectMaskParams())) {
                QStringList ids;
                for (const ParamDef& d : *meta->params)
                    ids << d.id;
                unknown << QStringLiteral("%1.%2 (it has %3)").arg(type, k, ids.join(QLatin1Char(' ')));
            }
            e.params[k].value = v;
        }
        return e;
    };
    auto preset = [&](Timeline& tl, const QString& name) -> EffectChain {
        for (const Preset& p : factoryPresets())
            if (p.name == name)
                return instantiatePreset(p, [&] { return tl.allocId(); });
        unknown << name;
        return {};
    };
    auto transition = [&](const QString& id) {
        const TransitionDef* d = findTransition(id);
        if (!d) {
            unknown << id;
            return TransitionSpec{};
        }
        return makeTransition(*d);
    };
    auto track = [&](Timeline& tl, TrackType type, int index, const QString& name) {
        const int i = ops::addTrack(tl, type, index);
        tl.tracks[size_t(i)].name = name;
        return i;
    };
    // A file's picture and / or sound on the given tracks (grouped), from `from` in the file.
    auto place = [&](Project& p, Timeline& tl, int mediaId, int vTrack, int aTrack, double at, double length, double from = 0) {
        const auto m = p.mediaItem(mediaId);
        std::vector<int> ids;
        const int group = vTrack >= 0 && aTrack >= 0 ? tl.allocId() : 0;
        auto add = [&](int ti, int stream) {
            Clip c;
            c.id = tl.allocId();
            c.mediaId = mediaId;
            c.streamIndex = stream;
            c.start = S(at);
            c.length = S(length);
            c.sourceIn = S(from);
            c.groupId = group;
            tl.tracks[size_t(ti)].clips.push_back(c);
            ids.push_back(c.id);
        };
        if (vTrack >= 0 && !m->info.video.empty())
            add(vTrack, m->info.video.front().index);
        if (aTrack >= 0 && !m->info.audio.empty())
            add(aTrack, m->info.audio.front().index);
        tl.sortClips();
        std::vector<Clip*> placed;
        for (int id : ids)
            placed.push_back(tl.findClip(id));
        return placed;
    };
    auto label = [&](Timeline& tl, int track, const QString& text, double start, double length) {
        if (Clip* c = gen(tl, track, QStringLiteral("subtitle"), start, length, {{QStringLiteral("text"), text}}, {{QStringLiteral("y"), 0.08}})) { // (over the timecode)
            c->fadeIn = S(0.2);
            c->fadeOut = S(0.2);
        }
    };
    auto save = [&](Project& p, const Timeline& tl, const QString& name) {
        p.applyEdit(tl, QStringLiteral("example"));
        const QString path = out.filePath(name + QStringLiteral(".mvp"));
        QString err;
        bool ok = p.save(path, &err);
        // Only the paths relative to the project: nothing of this computer's folders.
        if (QFile f(path); ok && f.open(QIODevice::ReadOnly)) {
            QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            QJsonArray media = root[QStringLiteral("media")].toArray();
            for (auto v : media) {
                QJsonObject o = v.toObject();
                o[QStringLiteral("path")] = o[QStringLiteral("relativePath")];
                v = o;
            }
            root[QStringLiteral("media")] = media;
            ok = f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(QJsonDocument(root).toJson()) > 0;
        }
        Project loaded;
        QStringList missing;
        ok = ok && loaded.load(path, &err, &missing) && missing.isEmpty() && loaded.timeline() == tl;
        Compositor comp(true);
        // Eight frames along it, all drawn (a contact sheet of them with MVP_EXAMPLES_PREVIEW=<folder>).
        std::vector<QImage> frames;
        double least = 1;
        for (int i = 0; ok && i < 8; ++i) {
            frames.push_back(comp.render(*loaded.snapshot(), loaded.timeline().duration() * (2 * i + 1) / 16, QSize(480, 270)));
            least = std::min(least, coverage(frames.back()));
        }
        if (const QString sheets = qEnvironmentVariable("MVP_EXAMPLES_PREVIEW"); ok && !sheets.isEmpty()) {
            QDir().mkpath(sheets);
            contactSheet(frames).save(QDir(sheets).filePath(name + QStringLiteral(".png")));
        }
        for (const Track& t : tl.tracks) // every event inside its file (past the end it would show a frozen frame)
            for (const Clip& c : t.clips)
                if (const auto m = c.mediaId ? p.mediaItem(c.mediaId) : nullptr; m && c.sourceIn + c.sourceSpan() > m->info.duration + kUsPerSecond / 10)
                    unknown << QStringLiteral("%1: event %2 runs past the end of its file").arg(name).arg(c.id);
        check(ok && least > 0.5,
              QStringLiteral("%1.mvp: %2 s, %3 tracks, loads back and renders %4").arg(name).arg(double(tl.duration()) / kUsPerSecond, 0, 'f', 1)
                  .arg(tl.tracks.size()).arg(err + missing.join(QLatin1Char(' '))));
    };

    // ---- Media ----
    auto encode = [&](const ProjectSnapshot& snap, const QString& name, bool video, TimeUs length) {
        ExportSettings es;
        es.path = out.filePath(QStringLiteral("media/") + name);
        es.muxer = QStringLiteral("mp4");
        if (video)
            es.videoEncoder = QStringLiteral("libx264");
        es.audioEncoder = QStringLiteral("aac");
        es.width = snap.settings.width;
        es.height = snap.settings.height;
        es.fps = snap.settings.fps;
        es.quality = 28;
        es.encoderPreset = QStringLiteral("slow");
        es.audioBitrate = 128000;
        es.rangeEnd = length;
        Exporter ex;
        const bool ok = ex.run(snap, es);
        check(ok, QStringLiteral("media/%1 (%2 KB) %3").arg(name).arg(QFileInfo(es.path).size() / 1024).arg(ex.error()));
    };
    auto audioSnap = [&](const QString& wav, TimeUs length) {
        ProjectSnapshot snap;
        snap.timeline.nextId = 100;
        if (auto item = loadItem(wav, 1)) {
            snap.media[1] = item;
            Track a;
            a.id = 1;
            a.type = TrackType::Audio;
            Clip c;
            c.id = 2;
            c.mediaId = 1;
            c.streamIndex = item->info.audio.front().index;
            c.length = length > 0 ? length : item->info.duration;
            a.clips.push_back(c);
            snap.timeline.tracks.push_back(a);
        }
        return snap;
    };
    const int rate = 48000;
    const QString musicWav = out.filePath(QStringLiteral("music.wav"));
    writeWav(musicWav, songAudio(SongStyle::House, 120, 40, rate), rate);
    encode(audioSnap(musicWav, S(40)), QStringLiteral("music.m4a"), false, S(40));
    {
        const ProjectSnapshot narration = audioSnap(narrationWav, 0);
        check(!narration.timeline.tracks.empty(), QStringLiteral("the narration %1 reads").arg(narrationWav));
        if (!narration.timeline.tracks.empty())
            encode(narration, QStringLiteral("narration.m4a"), false, narration.timeline.duration());
    }
    {
        // 12 s: a yellow ball moving on a curve over a retro grid, with a timecode, and music.
        ProjectSnapshot snap = audioSnap(musicWav, S(12));
        snap.settings.width = 1280;
        snap.settings.height = 720;
        Timeline& tl = snap.timeline;
        for (int i = 0; i < 3; ++i) {
            Track v;
            v.id = tl.allocId();
            v.type = TrackType::Video;
            tl.tracks.insert(tl.tracks.begin(), v);
        }
        gen(tl, 0, QStringLiteral("timecode"), 0, 12);
        if (Clip* ball = gen(tl, 1, QStringLiteral("shape_heart"), 0, 12, {},
                             {{QStringLiteral("shape"), 2}, {QStringLiteral("width"), 30}, {QStringLiteral("height"), 30},
                              {QStringLiteral("color"), double(0xffd400)}, {QStringLiteral("outline"), 6}, {QStringLiteral("outlineColor"), 0.0}})) {
            for (const auto& [t, x, y] : {std::tuple{0.0, 0.2, 0.35}, {4.0, 0.62, 0.65}, {8.0, 0.8, 0.4}, {12.0, 0.3, 0.6}}) {
                ball->generator.params[QStringLiteral("x")].addKey(S(t), x);
                ball->generator.params[QStringLiteral("y")].addKey(S(t), y);
            }
            for (const QString k : {QStringLiteral("x"), QStringLiteral("y")})
                for (Keyframe& key : ball->generator.params[k].keys)
                    key.interp = Keyframe::Interp::Smooth;
        }
        gen(tl, 2, QStringLiteral("retro_grid"), 0, 12);
        encode(snap, QStringLiteral("sample.mp4"), true, S(12));
    }
    QFile::remove(musicWav);

    // ---- 1. First Edit: three backgrounds, a crossfade, a title, music ----
    {
        Project p;
        Timeline tl = p.timeline(); // Video (0), Audio (1)
        tl.tracks[0].name = QStringLiteral("Pictures");
        tl.tracks[1].name = QStringLiteral("Music");
        const int titles = track(tl, TrackType::Video, 0, QStringLiteral("Titles"));
        const int pics = titles + 1;
        gen(tl, pics, QStringLiteral("plasma_ocean"), 0, 5);
        gen(tl, pics, QStringLiteral("aurora"), 4, 5); // overlaps the first by 1 s: a crossfade
        gen(tl, pics, QStringLiteral("starfield"), 9, 5);
        if (Clip* t = gen(tl, titles, QStringLiteral("title"), 0.5, 4, {{QStringLiteral("text"), QStringLiteral("My First Edit")}})) {
            t->fadeIn = S(0.5);
            t->fadeOut = S(0.5);
        }
        gen(tl, titles, QStringLiteral("lower_third"), 9.5, 4, {{QStringLiteral("text"), QStringLiteral("Starfield\nA generated background")}});
        const int music = p.addMedia(probeMedia(out.filePath(QStringLiteral("media/music.m4a"))));
        if (auto c = place(p, tl, music, -1, pics + 1, 0, 14); !c.empty() && c.front())
            c.front()->fadeOut = S(2);
        save(p, tl, QStringLiteral("first-edit"));
    }

    // ---- 2. Titles and Generators ----
    {
        Project p;
        Timeline tl = p.timeline();
        tl.tracks[0].name = QStringLiteral("Backgrounds");
        const int text = track(tl, TrackType::Video, 0, QStringLiteral("Text"));
        const int overlays = track(tl, TrackType::Video, 0, QStringLiteral("Overlays"));
        const int bg = text + 1;
        gen(tl, bg, QStringLiteral("countdown"), 0, 5);
        gen(tl, bg, QStringLiteral("gradient"), 5, 7);
        gen(tl, bg, QStringLiteral("bokeh_warm"), 12, 7);
        gen(tl, bg, QStringLiteral("retro_grid"), 19, 7);
        gen(tl, bg, QStringLiteral("starfield"), 26, 10);
        gen(tl, text, QStringLiteral("title_subtitle"), 5, 4, {{QStringLiteral("text"), QStringLiteral("Titles and Generators\nEverything here is made by Mutant Video Producer")}});
        gen(tl, text, QStringLiteral("kinetic_text"), 9, 4, {{QStringLiteral("text"), QStringLiteral("Every word pops on in time")}});
        gen(tl, text, QStringLiteral("typewriter"), 13, 4, {{QStringLiteral("text"), QStringLiteral("Typed out one letter at a time")}});
        gen(tl, text, QStringLiteral("lower_third"), 17, 5, {{QStringLiteral("text"), QStringLiteral("Lower Third\nNames and roles")}});
        gen(tl, text, QStringLiteral("zoom_title"), 22, 4, {{QStringLiteral("text"), QStringLiteral("ZOOM")}});
        gen(tl, text, QStringLiteral("credits"), 26, 10,
            {{QStringLiteral("text"), QStringLiteral("# Made with\nMutant Video Producer\n\n# Tutorial\nTitles and Generators\n\n# Thanks for watching")}});
        if (Clip* heart = gen(tl, overlays, QStringLiteral("shape_heart"), 19, 3)) {
            heart->generator.params[QStringLiteral("x")].addKey(0, 0.15);
            heart->generator.params[QStringLiteral("x")].addKey(S(3), 0.85);
            heart->generator.params[QStringLiteral("angle")].addKey(0, 0);
            heart->generator.params[QStringLiteral("angle")].addKey(S(3), 360);
        }
        gen(tl, overlays, QStringLiteral("confetti_gold"), 22.5, 3.5);
        const int bar = track(tl, TrackType::Video, 0, QStringLiteral("Progress"));
        gen(tl, bar, QStringLiteral("progress_bar"), 0, 36);
        save(p, tl, QStringLiteral("titles-and-generators"));
    }

    // ---- 3. Transitions: every one, named ----
    {
        Project p;
        Timeline tl = p.timeline();
        tl.tracks[0].name = QStringLiteral("Pictures");
        const int labels = track(tl, TrackType::Video, 0, QStringLiteral("Names"));
        const int pics = labels + 1;
        const QString looks[] = {QStringLiteral("plasma_neon"), QStringLiteral("aurora"), QStringLiteral("retro_grid"), QStringLiteral("bokeh_warm"),
                                 QStringLiteral("plasma_fire"), QStringLiteral("digital_rain"), QStringLiteral("starfield"), QStringLiteral("gradient")};
        const auto& all = transitions();
        double at = 0;
        for (size_t i = 0; i <= all.size(); ++i) {
            Clip* c = gen(tl, pics, looks[i % std::size(looks)], at, 3);
            if (c && i > 0) {
                c->transitionIn = transition(all[i - 1].id);
                label(tl, labels, all[i - 1].name, at - 0.2, 1.4);
            }
            at += 2; // 1 s overlaps
        }
        save(p, tl, QStringLiteral("transitions"));
    }

    // ---- 4. Effects and Keyframes ----
    {
        Project p;
        Timeline tl = p.timeline();
        tl.tracks[0].name = QStringLiteral("Video");
        const int sample = p.addMedia(probeMedia(out.filePath(QStringLiteral("media/sample.mp4"))));
        if (auto a = place(p, tl, sample, 0, 1, 0, 12); a.size() == 2) {
            // A blur that clears over the first 2 seconds, then a rack: black and white beside a glow, screened together.
            EffectInstance blur = fx(tl, QStringLiteral("gaussian_blur"));
            for (const QString k : {QStringLiteral("horizontal"), QStringLiteral("vertical")}) {
                blur.params[k].addKey(0, 30);
                blur.params[k].addKey(S(2), 0);
            }
            EffectInstance rack = makeRack(tl.allocId());
            rack.chains.push_back(makeRackChain(false, QStringLiteral("Mono"), {fx(tl, QStringLiteral("black_white"))}));
            RackChain glow = makeRackChain(false, QStringLiteral("Glow"), {fx(tl, QStringLiteral("glow"), {{QStringLiteral("intensity"), 1.5}})});
            glow.params[QStringLiteral("blend")].value = double(BlendMode::Screen);
            rack.chains.push_back(glow);
            a[0]->effects = {blur, rack};
        }
        if (auto b = place(p, tl, sample, 0, 1, 12.5, 12); b.size() == 2) {
            // Pixelate inside an ellipse over the ball where it starts: Track Motion makes it follow.
            b[0]->effects = {fx(tl, QStringLiteral("pixelate"),
                                {{QStringLiteral("size"), 24}, {QStringLiteral("mask.shape"), 2}, {QStringLiteral("mask.x"), 0.2}, {QStringLiteral("mask.y"), 0.35},
                                 {QStringLiteral("mask.w"), 0.2}, {QStringLiteral("mask.h"), 0.36}, {QStringLiteral("mask.feather"), 2}})};
        }
        tl.tracks[0].effects = {fx(tl, QStringLiteral("noise"), {{QStringLiteral("amount"), 0.15}})};
        tl.outputEffects = {fx(tl, QStringLiteral("vignette"))};
        const int labels = track(tl, TrackType::Video, 0, QStringLiteral("Notes"));
        label(tl, labels, QStringLiteral("Event FX: a keyframed blur, then a rack"), 0.3, 4);
        label(tl, labels, QStringLiteral("A masked Pixelate: track it with Track Motion"), 12.8, 4);
        save(p, tl, QStringLiteral("effects-and-keyframes"));
    }

    // ---- 5. Colour Grading: the same shot five ways, colour bars first ----
    {
        Project p;
        Timeline tl = p.timeline();
        tl.tracks[0].name = QStringLiteral("Video");
        const int labels = track(tl, TrackType::Video, 0, QStringLiteral("Grades"));
        const int video = labels + 1;
        const int sample = p.addMedia(probeMedia(out.filePath(QStringLiteral("media/sample.mp4"))));
        gen(tl, video, QStringLiteral("bars_smpte"), 0, 3);
        label(tl, labels, QStringLiteral("Colour bars: open the Scopes"), 0.2, 2.6);
        const QString grades[] = {QString(), QStringLiteral("S-Curve Contrast"), QStringLiteral("Teal and Orange"), QStringLiteral("Faded Film"),
                                  QStringLiteral("Cool Moonlight"), QStringLiteral("Vintage Film")};
        double at = 3;
        for (const QString& grade : grades) {
            if (auto c = place(p, tl, sample, video, video + 1, at, 4, std::fmod(at - 3, 8.0)); !c.empty() && c.front() && !grade.isEmpty())
                c.front()->effects = preset(tl, grade);
            label(tl, labels, grade.isEmpty() ? QStringLiteral("Original") : grade, at + 0.2, 3.6);
            at += 4;
        }
        save(p, tl, QStringLiteral("colour-grading"));
    }

    // ---- 6. Glitch and Retro ----
    {
        Project p;
        Timeline tl = p.timeline();
        tl.tracks[0].name = QStringLiteral("Video");
        const int labels = track(tl, TrackType::Video, 0, QStringLiteral("Looks"));
        const int video = labels + 1;
        const int sample = p.addMedia(probeMedia(out.filePath(QStringLiteral("media/sample.mp4"))));
        QString console;
        for (const EffectDef& d : videoEffects())
            if (console.isEmpty() && d.id.startsWith(QLatin1String("vg_nes_")))
                console = d.id;
        if (console.isEmpty())
            unknown << QStringLiteral("a NES effect");
        const std::pair<QString, QString> looks[] = {
            {QStringLiteral("vhs"), QStringLiteral("VHS")}, {QStringLiteral("crt"), QStringLiteral("CRT Monitor")},
            {QStringLiteral("rgb_split"), QStringLiteral("RGB Split")}, {QStringLiteral("pixel_sort"), QStringLiteral("Pixel Sort")},
            {QStringLiteral("datamosh"), QStringLiteral("Datamosh")}, {console, QStringLiteral("Broken VGS: NES")},
            {QStringLiteral("old_film"), QStringLiteral("Old Film")}, {QStringLiteral("kaleidoscope"), QStringLiteral("Kaleidoscope")},
            {QString(), QStringLiteral("Preset chain: Broken Stream")}};
        double at = 0;
        for (const auto& [type, name] : looks) {
            if (auto c = place(p, tl, sample, video, video + 1, at, 4, std::fmod(at, 8)); !c.empty() && c.front())
                c.front()->effects = type.isEmpty() ? preset(tl, QStringLiteral("Broken Stream")) : EffectChain{fx(tl, type)};
            label(tl, labels, name, at + 0.2, 3.6);
            at += 4;
        }
        save(p, tl, QStringLiteral("glitch-and-retro"));
    }

    // ---- 7. Audio Mixing: narration over music, ducked, cleaned up, faded ----
    {
        Project p;
        Timeline tl = p.timeline();
        tl.tracks[0].name = QStringLiteral("Picture");
        tl.tracks[1].name = QStringLiteral("Narration");
        const int music = track(tl, TrackType::Audio, -1, QStringLiteral("Music"));
        const int narration = p.addMedia(probeMedia(out.filePath(QStringLiteral("media/narration.m4a"))));
        const int song = p.addMedia(probeMedia(out.filePath(QStringLiteral("media/music.m4a"))));
        const double talk = double(p.mediaItem(narration)->info.duration) / kUsPerSecond;
        const double length = std::ceil(talk) + 6;
        gen(tl, 0, QStringLiteral("aurora"), 0, length);
        place(p, tl, narration, -1, 1, 2, talk);
        place(p, tl, song, -1, music, 0, length);
        tl.tracks[1].effects = preset(tl, QStringLiteral("Podcast Voice"));
        Track& m = tl.tracks[size_t(music)];
        m.duckUnder = tl.tracks[1].id;
        m.duckAmount = -14;
        m.volume.active = true;
        m.volume.points = {{0, 0.0, Keyframe::Interp::Smooth}, {S(2), 0.8}, {S(length - 3), 0.8, Keyframe::Interp::Smooth}, {S(length), 0.0}};
        tl.audioMasterEffects = {fx(tl, QStringLiteral("a_limiter"))};
        const int labels = track(tl, TrackType::Video, 0, QStringLiteral("Title"));
        label(tl, labels, QStringLiteral("Audio Mixing: open the Mixer (Ctrl+Shift+M)"), 0, 3);
        save(p, tl, QStringLiteral("audio-mixing"));
    }

    // ---- 8. Cutting to the Beat: music, a background, a clip in Project Media ----
    {
        Project p;
        Timeline tl = p.timeline();
        tl.tracks[0].name = QStringLiteral("Background");
        tl.tracks[1].name = QStringLiteral("Music");
        const int song = p.addMedia(probeMedia(out.filePath(QStringLiteral("media/music.m4a"))));
        const int sample = p.addMedia(probeMedia(out.filePath(QStringLiteral("media/sample.mp4"))));
        p.setMediaDetails(sample, QStringLiteral("Clips"), 4, 0, 0);
        gen(tl, 0, QStringLiteral("plasma_neon"), 0, 32);
        place(p, tl, song, -1, 1, 0, 32);
        save(p, tl, QStringLiteral("cutting-to-the-beat"));
    }

    // ---- 9. Speed and Time: the clip at 100%, 50%, 200%, reversed and ramped ----
    {
        Project p;
        Timeline tl = p.timeline();
        tl.tracks[0].name = QStringLiteral("Video");
        const int labels = track(tl, TrackType::Video, 0, QStringLiteral("Speeds"));
        const int video = labels + 1;
        const int sample = p.addMedia(probeMedia(out.filePath(QStringLiteral("media/sample.mp4"))));
        const MediaItem* m = p.mediaItem(sample).get();
        const FrameRate fps = p.settings().fps;
        double at = 0;
        for (const auto& [speed, reverse, ramp, name] :
             {std::tuple{1.0, false, false, QStringLiteral("100%")}, {0.5, false, false, QStringLiteral("50%: slow motion")},
              {2.0, false, false, QStringLiteral("200%")}, {1.0, true, false, QStringLiteral("Reversed")},
              {1.0, false, true, QStringLiteral("Speed ramp: slow in the middle")}}) {
            auto c = place(p, tl, sample, video, video + 1, at, 4);
            double length = 4;
            for (Clip* x : c) {
                if (!x)
                    continue;
                ops::setClipSpeed(*x, m, speed, false, fps);
                x->reverse = reverse;
                if (ramp)
                    ops::applyRampPreset(*x, m, ops::RampPreset::SlowMiddle, fps);
                length = double(x->length) / kUsPerSecond;
            }
            label(tl, labels, name, at + 0.2, std::min(3.6, length - 0.4));
            at += length;
        }
        save(p, tl, QStringLiteral("speed-and-time"));
    }

    // ---- 10. Live Set: pads, a scene, overlays and a setlist ----
    {
        Project p;
        Timeline tl = p.timeline();
        p.addMedia(probeMedia(out.filePath(QStringLiteral("media/sample.mp4")))); // a video file to play as the Live source
        const int titles = track(tl, TrackType::Video, 0, QStringLiteral("Title"));
        gen(tl, titles, QStringLiteral("title_subtitle"), 0, 10,
            {{QStringLiteral("text"), QStringLiteral("Live Set\nOpen the Live tab:\nthe pads, scenes and setlist are ready")}});
        gen(tl, titles + 1, QStringLiteral("bokeh_party"), 0, 10);
        tl.liveEffects = {fx(tl, QStringLiteral("glow"), {{QStringLiteral("intensity"), 0.6}})};
        auto pad = [&](int i, const QString& name, EffectChain chain, bool momentary = false) {
            tl.livePads[size_t(i)] = LivePad{name, std::move(chain), false, momentary};
        };
        pad(0, QStringLiteral("Neon"), {fx(tl, QStringLiteral("neon_edges"))});
        pad(1, QStringLiteral("Kaleido"), {fx(tl, QStringLiteral("kaleidoscope"))});
        pad(2, QStringLiteral("Mirror"), {fx(tl, QStringLiteral("mirror_symmetry"))});
        pad(3, QStringLiteral("Psychedelic"), {fx(tl, QStringLiteral("psychedelic"))});
        pad(4, QStringLiteral("VHS"), {fx(tl, QStringLiteral("vhs"))});
        pad(5, QStringLiteral("CRT"), {fx(tl, QStringLiteral("crt"))});
        pad(6, QStringLiteral("Trails"), {fx(tl, QStringLiteral("trails"))});
        pad(7, QStringLiteral("Mono"), {fx(tl, QStringLiteral("black_white"))});
        pad(12, QStringLiteral("Strobe (hold)"), {fx(tl, QStringLiteral("strobe"))}, true);
        pad(13, QStringLiteral("RGB Burst (hold)"), {fx(tl, QStringLiteral("rgb_split"))}, true);
        for (int i = 8; i < 12; ++i) { // a random chain of each strength
            const RandomChain r = makeRandomChain(RandomTier(i - 8), uint32_t(2026 + i), [&] { return tl.allocId(); });
            pad(i, r.name, r.effects);
        }
        LiveOverlay title;
        title.text = QStringLiteral("MVP LIVE");
        title.anchor = 1;
        title.size = 6;
        title.pulse = true;
        LiveOverlay bars;
        bars.kind = LiveOverlay::Kind::Spectrum;
        bars.anchor = 7;
        bars.size = 14;
        bars.opacity = 0.8;
        tl.liveOverlays = {title, bars};
        tl.liveScenes[0] = LiveScene{QStringLiteral("Intro"), tl.liveEffects, 0, {}, {title}, true};
        tl.liveScenes[1] = LiveScene{QStringLiteral("Drop"), tl.liveEffects, 3, {}, {title, bars}, true};
        tl.liveSetlist = {LiveSong{QStringLiteral("Warm-up"), {}, 120, true, 0, QStringLiteral("Slow pads, let it breathe")},
                          LiveSong{QStringLiteral("Peak"), {}, 128, true, 1, QStringLiteral("Momentary pads on the drops")}};
        // A loop for the pad sequencer (Loop off: press it to play): four bars through the pads, the holds on the 4.
        tl.liveSequence.bars = 4;
        tl.liveSequence.blocks = {{0, 0, 192},    {1, 192, 96},   {2, 288, 96},   {3, 384, 288},  {12, 672, 96},
                                  {4, 768, 96},   {5, 864, 96},   {6, 960, 192},  {13, 1152, 48}, {13, 1248, 48},
                                  {8, 1344, 96},  {9, 1440, 48},  {10, 1488, 48}};
        save(p, tl, QStringLiteral("live-set"));
    }

    check(unknown.isEmpty(), QStringLiteral("every effect, setting, preset and generator named exists %1").arg(unknown.join(QStringLiteral(", "))));
    return failures;
}

int cmdTools(const QString& mediaDir, const QString& outDir)
{
    QDir().mkpath(outDir);
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        print(QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };
    const int rate = 48000;
    ProjectSnapshot snap;
    snap.settings.width = 640;
    snap.settings.height = 360;
    // ---- Beats from the timeline's music -> markers ----
    const QString loop = QDir(outDir).filePath(QStringLiteral("loop120.wav"));
    writeWav(loop, drumLoop(120, 20, rate, 3), rate);
    auto music = loadItem(loop, 1);
    auto video = loadItem(QDir(mediaDir).filePath(QStringLiteral("h264_aac.mp4")), 2);
    auto video2 = loadItem(QDir(mediaDir).filePath(QStringLiteral("vp8_vorbis.webm")), 3);
    if (!music || !video || !video2) {
        print(QStringLiteral("FAIL  test media missing"));
        return 1;
    }
    snap.media[1] = music;
    snap.media[2] = video;
    snap.media[3] = video2;
    {
        Track a;
        a.id = 10;
        a.type = TrackType::Audio;
        Clip c;
        c.id = 11;
        c.mediaId = 1;
        c.streamIndex = music->info.audio.front().index;
        c.start = 2 * kUsPerSecond; // the music starts 2 s in: beats at 2.5, 3.0, ...
        c.length = 18 * kUsPerSecond;
        a.clips.push_back(c);
        snap.timeline.tracks.push_back(a);
    }
    BeatSettings bs;
    bs.band = BeatSettings::Full;
    const BeatScan scan = scanBeats(snap, 2 * kUsPerSecond, 20 * kUsPerSecond, {}, bs);
    double worst = 0;
    for (TimeUs b : scan.beats) {
        double e = std::fmod(double(b - 2 * kUsPerSecond - kUsPerSecond / 2) / 1e6, 0.5);
        if (e < 0)
            e += 0.5;
        worst = std::max(worst, std::min(e, 0.5 - e));
    }
    check(std::abs(scan.bpm - 120) < 0.5 && scan.beats.size() >= 33 && worst < 0.03,
          QStringLiteral("Detect Beats on a 120 BPM loop placed 2 s in: %1 BPM, %2 beats (the grid carried back to the start), worst %3 ms off")
              .arg(scan.bpm, 0, 'f', 2).arg(scan.beats.size()).arg(worst * 1000, 0, 'f', 1));
    Timeline tl = snap.timeline;
    tl.nextId = 100;
    const int bars = addBeatMarkers(tl, scan.beats, 4, true, 2 * kUsPerSecond, 20 * kUsPerSecond);
    const bool named = !tl.markers.empty() && tl.markers.front().name == QLatin1String("1") && tl.markers[1].name == QLatin1String("2");
    const int again = addBeatMarkers(tl, scan.beats, 4, true, 2 * kUsPerSecond, 20 * kUsPerSecond);
    check(bars >= 8 && named && int(tl.markers.size()) == again,
          QStringLiteral("a marker on every bar, numbered (%1); running it again replaces them (%2 markers)").arg(bars).arg(tl.markers.size()));
    // Split a video event at the markers, and a montage cut to them.
    {
        Track v;
        v.id = 20;
        v.type = TrackType::Video;
        Clip c;
        c.id = tl.allocId();
        c.mediaId = 2;
        c.streamIndex = video->info.video.front().index;
        c.start = 2 * kUsPerSecond;
        c.length = 4 * kUsPerSecond;
        v.clips.push_back(c);
        tl.tracks.insert(tl.tracks.begin(), v);
        const int inside = int(std::count_if(tl.markers.begin(), tl.markers.end(), [&](const Marker& m) { return m.time > c.start && m.time < c.end(); }));
        Timeline split = tl;
        const std::vector<int> pieces = ops::splitAtMarkers(split, {c.id});
        bool onMarkers = true;
        for (const Clip& piece : split.tracks[0].clips)
            if (piece.start != c.start)
                onMarkers &= std::any_of(split.markers.begin(), split.markers.end(), [&](const Marker& m) { return m.time == piece.start; });
        check(int(pieces.size()) == inside && onMarkers && split.tracks[0].clips.size() == size_t(inside) + 1,
              QStringLiteral("Split at Markers cuts an event on the %1 markers inside it").arg(inside));
        std::vector<TimeUs> cuts;
        for (const Marker& m : tl.markers)
            cuts.push_back(m.time);
        Timeline mvp = tl;
        const std::vector<int> ids = ops::beatMontage(mvp, {video, video2}, cuts, false, 7);
        bool contiguous = true, inMedia = true;
        const Track& top = mvp.tracks.front();
        for (size_t i = 0; i < top.clips.size(); ++i) {
            const Clip& m = top.clips[i];
            contiguous &= m.start == cuts[i] && m.end() == cuts[i + 1];
            const auto& item = m.mediaId == 2 ? video : video2;
            inMedia &= m.sourceIn >= 0 && m.sourceIn + m.length <= item->info.duration && m.mediaId == (i % 2 ? 3 : 2);
        }
        check(ids.size() == cuts.size() - 1 && top.name == QLatin1String("Beat montage") && contiguous && inMedia,
              QStringLiteral("Beat Montage: %1 clips on a new top track, one per bar, files in turn, each from inside its file").arg(ids.size()));
        // A file with Mark In / Out: its clips come from the marked part.
        auto marked = std::make_shared<MediaItem>(*video2);
        marked->markIn = marked->info.duration / 4;
        marked->markOut = marked->info.duration / 2;
        Timeline mvp2 = tl;
        ops::beatMontage(mvp2, {marked}, cuts, false, 7);
        bool inMarks = !mvp2.tracks.front().clips.empty();
        for (const Clip& m : mvp2.tracks.front().clips)
            inMarks &= m.sourceIn >= marked->markIn && m.sourceIn + m.length <= marked->markOut;
        check(inMarks, QStringLiteral("Beat Montage takes a file's clips from its marked part"));
    }
    // ---- Effect masks: inside / outside, rotated, feathered; CPU and GPU the same ----
    {
        QImage grey(320, 180, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < grey.height(); ++y)
            for (int x = 0; x < grey.width(); ++x)
                grey.setPixel(x, y, qRgb(40 + x / 4, 90, 140 + y / 3));
        EffectInstance inv = makeEffectOfType(QStringLiteral("invert"), 1);
        inv.params[QStringLiteral("mask.shape")].value = 2;
        inv.params[QStringLiteral("mask.x")].value = 0.5;
        inv.params[QStringLiteral("mask.y")].value = 0.5;
        inv.params[QStringLiteral("mask.w")].value = 0.4;
        inv.params[QStringLiteral("mask.h")].value = 0.4;
        inv.params[QStringLiteral("mask.feather")].value = 3;
        Compositor cpu(false);
        const QImage in = cpu.renderLive(grey, {inv}, 0, 30);
        auto inverted = [&](const QImage& img, int x, int y) { return qRed(img.pixel(x, y)) == 255 - qRed(grey.pixel(x, y)); };
        const bool ellipse = inverted(in, 160, 90) && !inverted(in, 5, 5) && !inverted(in, 160 + 70, 90 + 30); // outside the ellipse's curve
        inv.params[QStringLiteral("mask.invert")].value = 1;
        const QImage out = cpu.renderLive(grey, {inv}, 0, 30);
        const bool outside = !inverted(out, 160, 90) && inverted(out, 5, 5);
        inv.params[QStringLiteral("mask.invert")].value = 0;
        inv.params[QStringLiteral("mask.shape")].value = 1;
        inv.params[QStringLiteral("mask.rotation")].value = 30;
        inv.params[QStringLiteral("mask.feather")].value = 6;
        const QImage rotated = cpu.renderLive(grey, {inv}, 0, 30);
        double gpuDiff = -1;
        auto surface = createGpuSurface();
        QString err;
        if (auto gpu = surface ? GpuCompositor::create(surface.get(), false, &err) : nullptr)
            gpuDiff = imageDifference(rotated, gpu->renderLive(grey, {inv}, 0, 30));
        check(ellipse && outside && (gpuDiff < 0 || gpuDiff < 1.0),
              QStringLiteral("an ellipse mask limits Invert to it, Outside flips that; a rotated feathered rectangle matches on the GPU (difference %1)")
                  .arg(gpuDiff < 0 ? QStringLiteral("no GPU") : QString::number(gpuDiff, 'f', 3)));
    }
    // ---- Stabilisation: a picture shaken by a known amount ----
    const QString shaky = QDir(outDir).filePath(QStringLiteral("shaky.mp4"));
    QImage scene(960, 540, QImage::Format_ARGB32_Premultiplied);
    {
        uint32_t seed = 99;
        QPainter p(&scene);
        p.fillRect(scene.rect(), QColor(30, 40, 50));
        for (int i = 0; i < 400; ++i) { // lots of texture to match on
            seed = seed * 1664525u + 1013904223u;
            const int x = int(seed % 960), y = int((seed >> 10) % 540), s = 6 + int((seed >> 20) % 30);
            p.fillRect(x, y, s, s, QColor::fromHsv(int((seed >> 5) % 360), 160, 120 + int((seed >> 15) % 120)));
        }
    }
    const int frames = 90;
    std::vector<QPointF> shake(frames);
    {
        LiveRecorder rec;
        QString error;
        rec.start(shaky, QSize(640, 360), 30, &error);
        const double t0 = rec.startTime();
        for (int i = 0; i < frames; ++i) {
            // A slow pan to the right with hand shake on top.
            shake[size_t(i)] = QPointF(i * 0.8 + 9 * std::sin(i * 1.7) + 5 * std::sin(i * 0.9), 7 * std::sin(i * 2.3) + 4 * std::cos(i * 1.1));
            QImage frame(640, 360, QImage::Format_ARGB32_Premultiplied);
            QPainter p(&frame);
            p.drawImage(QPointF(-160 - shake[size_t(i)].x(), -90 - shake[size_t(i)].y()), scene);
            p.end();
            while (!rec.addFrame(frame, t0 + i / 30.0))
                QThread::msleep(2);
        }
        rec.finish(&error);
    }
    auto shakyItem = loadItem(shaky, 4);
    if (!shakyItem || !shakyItem->info.hasVideo()) {
        check(false, QStringLiteral("the shaky test video was made"));
    } else {
        const int stream = shakyItem->info.video.front().index;
        const bool analysed = stabilize::analyze(shaky, stream, {});
        const stabilize::Motion m = stabilize::motion(shaky, stream);
        // Measured shifts against the known ones (scaled to the 320-wide analysis; content moves opposite to the camera).
        double err = 0;
        int n = 0;
        for (size_t i = 1; i < m.dx.size() && i < shake.size(); ++i) {
            const double scale = double(m.width) / 640.0;
            const QPointF truth = -(shake[i] - shake[i - 1]) * scale;
            err += std::hypot(m.dx[i] - truth.x(), m.dy[i] - truth.y());
            ++n;
        }
        err = n ? err / n : 99;
        // Rendered with and without it: how much consecutive frames jump.
        Timeline st;
        Track v;
        v.id = 1;
        v.type = TrackType::Video;
        Clip c;
        c.id = 2;
        c.mediaId = 4;
        c.streamIndex = stream;
        c.length = TimeUs(frames) * kUsPerSecond / 30;
        v.clips.push_back(c);
        st.tracks.push_back(v);
        ProjectSnapshot s2;
        s2.settings = snap.settings;
        s2.media[4] = shakyItem;
        auto jumpiness = [&](double stab) {
            s2.timeline = st;
            s2.timeline.tracks[0].clips[0].stabilize = stab;
            Compositor comp(false);
            double sum = 0;
            QImage prev;
            for (int i = 20; i < 70; ++i) {
                const QImage img = comp.render(s2, TimeUs(i) * kUsPerSecond / 30 + 1000, QSize(320, 180));
                if (!prev.isNull())
                    sum += imageDifference(prev, img);
                prev = img;
            }
            return sum / 49;
        };
        const double raw = jumpiness(0), steady = jumpiness(2.0);
        check(analysed && m.dx.size() >= size_t(frames) - 2 && err < 0.6 && steady < raw * 0.6,
              QStringLiteral("stabilisation measures the shake (%1 px off per frame) and steadies the picture (frame-to-frame change %2 -> %3)")
                  .arg(err, 0, 'f', 2).arg(raw, 0, 'f', 2).arg(steady, 0, 'f', 2));
    }
    // ---- Motion tracking: a square moving along a curve over texture ----
    const QString moving = QDir(outDir).filePath(QStringLiteral("moving.mp4"));
    std::vector<QPointF> path(frames);
    {
        LiveRecorder rec;
        QString error;
        rec.start(moving, QSize(640, 360), 30, &error);
        const double t0 = rec.startTime();
        for (int i = 0; i < frames; ++i) {
            path[size_t(i)] = QPointF(120 + i * 4.0, 180 + 80 * std::sin(i * 0.07));
            QImage frame(640, 360, QImage::Format_ARGB32_Premultiplied);
            QPainter p(&frame);
            p.drawImage(QPointF(-100, -60), scene);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(250, 240, 60));
            p.drawRect(QRectF(path[size_t(i)] - QPointF(22, 22), QSizeF(44, 44)));
            p.setBrush(QColor(20, 20, 160));
            p.drawEllipse(path[size_t(i)], 10, 10);
            p.end();
            while (!rec.addFrame(frame, t0 + i / 30.0))
                QThread::msleep(2);
        }
        rec.finish(&error);
    }
    if (auto item = loadItem(moving, 5); item && item->info.hasVideo()) {
        const QRectF box(QPointF((path[0].x() - 26) / 640, (path[0].y() - 26) / 360), QSizeF(52.0 / 640, 52.0 / 360));
        const std::vector<tracker::Point> pts = tracker::track(moving, item->info.video.front().index, 0, item->info.duration, box);
        double worst = 0;
        for (size_t i = 0; i < pts.size() && i < path.size(); ++i)
            worst = std::max(worst, std::hypot(pts[i].centre.x() * 640 - path[i].x(), pts[i].centre.y() * 360 - path[i].y()));
        check(pts.size() >= size_t(frames) - 2 && worst < 4,
              QStringLiteral("Track Motion follows a moving square for %1 frames (at worst %2 px off)").arg(pts.size()).arg(worst, 0, 'f', 2));
    } else {
        check(false, QStringLiteral("the moving test video was made"));
    }
    // ---- Proxies: smaller files with the same frames at the same times, read by the preview ----
    {
        qputenv("MVP_PROXY_DIR", QDir(outDir).filePath(QStringLiteral("proxies")).toLocal8Bit());
        proxy::removeAll();
        struct Case {
            const char* file;
            bool heavy;
        };
        const Case cases[] = {{"uhd_hevc10.mp4", true}, {"hevc_10bit.mp4", true}, {"phone_rotated.mp4", false},
                              {"h264_aac.mp4", false},  {"vp9_vorbis.webm", true}, {"uhd_h264.mp4", true}};
        auto allPts = [](const QString& path, int stream) {
            std::vector<TimeUs> pts;
            VideoDecoder d;
            if (!d.open(path, stream, nullptr, false))
                return pts;
            const AVFrame* f = d.frameAt(0);
            while (f) {
                const TimeUs p = d.currentPts();
                pts.push_back(p);
                if (!d.nextFrame())
                    break;
                f = d.frameAt(d.nextPts());
                if (d.currentPts() <= p)
                    break;
            }
            return pts;
        };
        int id = 20;
        for (const Case& c : cases) {
            const QString path = QDir(mediaDir).filePath(QString::fromLatin1(c.file));
            auto item = loadItem(path, ++id);
            if (!item || !item->info.hasVideo()) {
                check(false, QStringLiteral("proxy test media %1").arg(QString::fromLatin1(c.file)));
                continue;
            }
            const int stream = item->info.video.front().index;
            QElapsedTimer timer;
            timer.start();
            QString error;
            const bool made = proxy::make(path, stream, 540, {}, &error);
            const qint64 ms = timer.elapsed();
            const QString pf = proxy::file(path, stream);
            const MediaInfo pi = probeMedia(pf);
            const bool small = made && pi.hasVideo() && pi.video.front().height <= 540 && pi.audio.empty();
            // Every frame at the same time as in the original (the first may start at 0).
            std::vector<TimeUs> a = allPts(path, stream), b = allPts(pf, 0);
            if (!a.empty() && !b.empty())
                a.front() = b.front() = 0;
            const bool sameTimes = !a.empty() && a == b;
            // The preview (proxies on) and a render (proxies off) show the same picture, a little softer.
            ProjectSnapshot ps;
            ps.settings.width = 640;
            ps.settings.height = 360;
            ps.media[item->id] = item;
            Track vt;
            vt.id = 1;
            Clip clip;
            clip.id = 2;
            clip.mediaId = item->id;
            clip.streamIndex = stream;
            clip.length = std::max<TimeUs>(kUsPerSecond / 2, item->info.duration);
            vt.clips.push_back(clip);
            ps.timeline.tracks.push_back(vt);
            Compositor full(true), preview(true);
            preview.setUseProxies(true);
            double worst = 0;
            for (double f : {0.1, 0.5, 0.85}) {
                const TimeUs t = TimeUs(double(clip.length) * f);
                worst = std::max(worst, imageDifference(full.render(ps, t, QSize(640, 360)), preview.render(ps, t, QSize(640, 360))));
            }
            // Playing a second from the start, as the preview does.
            auto playMs = [&](Compositor& comp) {
                QElapsedTimer pt;
                pt.start();
                for (int i = 0; i < 30; ++i)
                    comp.render(ps, TimeUs(i) * kUsPerSecond / 30, QSize(640, 360));
                return pt.elapsed();
            };
            Compositor fullPlay(false), proxyPlay(false);
            proxyPlay.setUseProxies(true);
            const qint64 fullMs = playMs(fullPlay), proxyMs = playMs(proxyPlay);
            proxy::setEnabled(false);
            const bool offReadsOriginal = proxy::playbackPath(path, stream) == path;
            proxy::setEnabled(true);
            check(proxy::recommended(item->info) == c.heavy && small && sameTimes && worst < 4 && offReadsOriginal,
                  QStringLiteral("proxy of %1 (%2x%3 -> %4x%5, %6 KB, made in %7 ms): %8 frames at the same times as the file, preview %9 levels from a render; a second of preview %11 ms -> %12 ms%10")
                      .arg(QString::fromLatin1(c.file))
                      .arg(item->info.video.front().width)
                      .arg(item->info.video.front().height)
                      .arg(pi.hasVideo() ? pi.video.front().width : 0)
                      .arg(pi.hasVideo() ? pi.video.front().height : 0)
                      .arg(QFileInfo(pf).size() / 1024)
                      .arg(ms)
                      .arg(b.size())
                      .arg(worst, 0, 'f', 2)
                      .arg(made ? (sameTimes ? QString() : QStringLiteral(" (times differ: %1 vs %2 frames; first differing %3: %4 vs %5)").arg(a.size()).arg(b.size())
                                       .arg(std::mismatch(a.begin(), a.end(), b.begin(), b.end()).first - a.begin())
                                       .arg(std::mismatch(a.begin(), a.end(), b.begin(), b.end()).first == a.end() ? -1 : *std::mismatch(a.begin(), a.end(), b.begin(), b.end()).first)
                                       .arg(std::mismatch(a.begin(), a.end(), b.begin(), b.end()).second == b.end() ? -1 : *std::mismatch(a.begin(), a.end(), b.begin(), b.end()).second))
                                : QStringLiteral(" - FAILED: ") + error)
                      .arg(fullMs)
                      .arg(proxyMs));
        }
        // Not for transparency; cancelling leaves nothing behind.
        const QString alpha = QDir(mediaDir).filePath(QStringLiteral("png_alpha.mov"));
        QString error;
        const bool alphaRefused = !proxy::make(alpha, 0, 540, {}, &error) && error.contains(QStringLiteral("transparency"));
        const QString uhd = QDir(mediaDir).filePath(QStringLiteral("uhd_h264.mp4"));
        proxy::remove(uhd, 0);
        int calls = 0;
        const bool cancelled = !proxy::make(uhd, 0, 540, [&](double) { return ++calls < 3; }, &error);
        const bool clean = !QFileInfo::exists(proxy::file(uhd, 0)) && !QFileInfo::exists(proxy::file(uhd, 0) + QStringLiteral(".part"));
        check(alphaRefused && cancelled && clean && proxy::totalBytes() > 0,
              QStringLiteral("no proxy for pictures with transparency; a cancelled one leaves no file; %1 KB of proxies in the cache")
                  .arg(proxy::totalBytes() / 1024));
        // Deleted while the preview plays it (the file is open): out of use at once, gone once let go.
        {
            const QString hevc = QDir(mediaDir).filePath(QStringLiteral("hevc_10bit.mp4"));
            auto playing = std::make_unique<VideoDecoder>();
            const bool open = playing->open(proxy::file(hevc, 0), 0) && playing->frameAt(kUsPerSecond);
            const bool removed = proxy::remove(hevc, 0);
            const bool unused = !proxy::exists(hevc, 0) && proxy::playbackPath(hevc, 0) == hevc;
            playing.reset();
            proxy::purge();
            check(open && removed && unused && !QFileInfo::exists(proxy::file(hevc, 0)),
                  QStringLiteral("a proxy deleted while it plays goes out of use at once and its file goes when it is let go"));
        }
        proxy::removeAll();
        check(proxy::totalBytes() == 0, QStringLiteral("Delete All Proxies empties the cache"));
    }

    // ---- Video scopes ----
    {
        // 75% colour bars: on the vectorscope each lands in its target box.
        QImage bars(640, 360, QImage::Format_RGB32);
        const QRgb colours[] = {qRgb(191, 191, 191), qRgb(191, 191, 0), qRgb(0, 191, 191), qRgb(0, 191, 0),
                                qRgb(191, 0, 191),   qRgb(191, 0, 0),   qRgb(0, 0, 191)};
        {
            QPainter p(&bars);
            for (int i = 0; i < 7; ++i)
                p.fillRect(QRect(i * 640 / 7, 0, 640 / 7 + 1, 360), QColor(colours[i]));
        }
        const QPointF red = scopes::vectorPosition(qRgb(191, 0, 0)), yellow = scopes::vectorPosition(qRgb(191, 191, 0));
        const QPointF grey = scopes::vectorPosition(qRgb(128, 128, 128));
        const double redAngle = std::atan2(red.y(), red.x()) * 180 / std::numbers::pi;
        check(std::hypot(grey.x(), grey.y()) < 1e-9 && redAngle > 100 && redAngle < 106 && yellow.x() < -0.3 && yellow.y() > 0,
              QStringLiteral("vectorscope: grey in the centre, red at %1 degrees, yellow up and left").arg(redAngle, 0, 'f', 1));
        const QImage vs = scopes::render(bars, scopes::Kind::Vectorscope, QSize(300, 300));
        // The red bar's dot: bright, reddish, at its target.
        const double scale = (std::max(32, 300 - 12) / 2.0) * 0.95 / 0.5;
        const QPoint at(int(std::lround(150 + red.x() * scale)), int(std::lround(150 - red.y() * scale)));
        int bright = 0;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const QRgb px = vs.pixel(at + QPoint(dx, dy));
                bright = std::max(bright, qRed(px) - qBlue(px));
            }
        check(bright > 80, QStringLiteral("vectorscope draws the red bar on its target (redness %1 there)").arg(bright));
        // A left-to-right ramp: the waveform climbs from bottom left to top right.
        QImage ramp(256, 100, QImage::Format_RGB32);
        for (int y = 0; y < 100; ++y)
            for (int x = 0; x < 256; ++x)
                ramp.setPixel(x, y, qRgb(x, x, x));
        const QImage wf = scopes::render(ramp, scopes::Kind::Waveform, QSize(334, 268));
        auto litRow = [&](int x) { // the lit row in a column of the plot
            for (int y = 0; y < wf.height(); ++y)
                if (qGreen(wf.pixel(x, y)) > 150)
                    return y;
            return -1;
        };
        const int left = litRow(28 + 10), right = litRow(28 + 290);
        check(left > right && left > 200 && right >= 0 && right < 60,
              QStringLiteral("waveform of a black-to-white ramp rises from row %1 to row %2").arg(left).arg(right));
        const scopes::Levels lv = scopes::histogram(ramp);
        bool even = lv.samples == 256 * 100;
        for (int i = 0; i < 256; ++i)
            even = even && lv.bins[3][size_t(i)] == 100 && lv.bins[0][size_t(i)] == 100;
        check(even, QStringLiteral("histogram of the ramp: every level %1 times").arg(lv.bins[3][128]));
        // Fast enough for playback: a 1080p picture, every kind.
        QImage hd = bars.scaled(1920, 1080);
        QElapsedTimer timer;
        timer.start();
        for (int k = 0; k < 4; ++k)
            scopes::render(hd, scopes::Kind(k), QSize(400, 300));
        const double ms = timer.nsecsElapsed() / 1e6 / 4;
#ifdef NDEBUG
        const double limit = 30;
#else
        const double limit = 300; // (the checked Debug build: index checks, not speed)
#endif
        check(ms < limit, QStringLiteral("a scope of a 1080p picture takes %1 ms").arg(ms, 0, 'f', 1));
        const QImage parade = scopes::render(bars, scopes::Kind::Parade, QSize(400, 260));
        const QImage hist = scopes::render(bars, scopes::Kind::Histogram, QSize(400, 260));
        QImage sheet(820, 540, QImage::Format_RGB32);
        sheet.fill(Qt::black);
        {
            QPainter p(&sheet);
            p.drawImage(0, 0, scopes::render(bars, scopes::Kind::Waveform, QSize(400, 260)));
            p.drawImage(410, 0, parade);
            p.drawImage(0, 270, vs.scaled(260, 260));
            p.drawImage(410, 270, hist);
        }
        sheet.save(QDir(outDir).filePath(QStringLiteral("scopes.png")));
    }
    // ---- Follow Audio: loudness bands of the timeline's sound ----
    {
        ProjectSnapshot s;
        s.media[1] = music;
        Track a;
        a.id = 10;
        a.type = TrackType::Audio;
        Clip c;
        c.id = 11;
        c.mediaId = 1;
        c.streamIndex = music->info.audio.front().index;
        c.start = 2 * kUsPerSecond;
        c.length = 6 * kUsPerSecond;
        a.clips.push_back(c);
        s.timeline.tracks.push_back(a);
        const TimeUs step = kUsPerSecond / 20;
        const std::vector<double> level = audioBandEnvelope(s, 0, 8 * kUsPerSecond, step, BandFollower::Level);
        const std::vector<double> bass = audioBandEnvelope(s, 0, 8 * kUsPerSecond, step, BandFollower::Bass);
        const double silent = *std::max_element(level.begin(), level.begin() + 38); // before 1.9 s
        const auto [lo, hi] = std::minmax_element(bass.begin() + 50, bass.begin() + 150);
        check(level.size() == 160 && silent < 0.01 && *std::max_element(level.begin(), level.end()) > 0.99 && *hi - *lo > 0.5,
              QStringLiteral("Follow Audio: silent before the music (%1), the bass rises and falls with the kicks (%2..%3)")
                  .arg(silent, 0, 'f', 3).arg(*lo, 0, 'f', 2).arg(*hi, 0, 'f', 2));
    }
    // ---- Freehand masks ----
    {
        ParamMap p;
        p[QStringLiteral("mask.shape")].value = 3;
        p[QStringLiteral("mask.feather")].value = 0;
        p[QStringLiteral("mask.smooth")].value = 0;
        p[QStringLiteral("mask.points")].value = 4;
        // A diamond with its left point moved to the box's top-left corner.
        p[QStringLiteral("mask.p3x")].value = -0.5;
        p[QStringLiteral("mask.p3y")].value = -0.5;
        const EffectMask m = effectMask(p, 0, QSize(400, 400));
        const double mw = 0.35 * 400 / 2, mh = 0.45 * 400 / 2; // default half-size, pixels
        auto in = [&](double x, double y) { return maskWeight(m, float(200 + x * 2 * mw), float(200 + y * 2 * mh)); };
        check(m.points.size() == 4 && in(0, 0) == 1 && in(-0.42, -0.42) == 1 && in(-0.42, 0.2) == 0 && in(0.3, 0.3) == 0,
              QStringLiteral("freehand mask: inside the shape, outside it, and a moved point reshapes it"));
        ParamMap k = p;
        k[QStringLiteral("mask.p0y")].addKey(0, -0.5);
        k[QStringLiteral("mask.p0y")].addKey(kUsPerSecond, -1.0);
        const EffectMask a0 = effectMask(k, 0, QSize(400, 400)), a1 = effectMask(k, kUsPerSecond, QSize(400, 400));
        check(a1.points[0].y() < a0.points[0].y() - 30, QStringLiteral("freehand mask points can be keyframed (rotoscoping)"));
    }
    // ---- Remove Background: the person model ----
    {
        QString err;
        const bool loaded = segmentationAvailable(SegmentModel::Fast, &err) && segmentationAvailable(SegmentModel::Quality, &err);
        QImage plain(640, 360, QImage::Format_ARGB32_Premultiplied);
        plain.fill(QColor(200, 205, 210));
        const QImage empty = personMask(plain, &err);
        int most = 0;
        for (int y = 0; !empty.isNull() && y < empty.height(); ++y)
            for (int x = 0; x < empty.width(); ++x)
                most = std::max(most, int(empty.constScanLine(y)[x]));
        {
            // Where the time goes at 1080p: the model (with its scaling) and the whole effect (blurred background).
            QImage hd(1920, 1080, QImage::Format_ARGB32_Premultiplied);
            hd.fill(QColor(120, 90, 60));
            personMask(hd);
            QElapsedTimer t;
            t.start();
            for (int i = 0; i < 10; ++i)
                personMask(hd);
            const double maskMs = t.nsecsElapsed() / 1e7;
            const EffectDef* rb = findEffect(QStringLiteral("remove_bg"));
            EffectInstance e = makeEffect(*rb, 1);
            e.params[QStringLiteral("mode")].value = 1;
            const Params prm = resolveParams(rb->params, e.params, 0);
            std::unique_ptr<EffectState> st;
            t.restart();
            for (int i = 0; i < 10; ++i) {
                QImage img = hd.copy();
                rb->apply(img, prm, RenderContext(), st);
            }
            print(QStringLiteral("INFO  Remove Background at 1080p: model on %3 %1 ms, whole effect (blurred, CPU) %2 ms").arg(maskMs, 0, 'f', 1).arg(t.nsecsElapsed() / 1e7, 0, 'f', 1).arg(segmentationDevice()));
        }
        check(loaded && empty.size() == plain.size() && most < 64,
              QStringLiteral("Remove Background: the person model loads and finds nobody in an empty room (at most %1) %2").arg(most).arg(err));
        // A real person (MVP_TEST_PORTRAIT: a head-and-shoulders photo against a plain background).
        const QString portrait = qEnvironmentVariable("MVP_TEST_PORTRAIT");
        if (!portrait.isEmpty()) {
            const QImage pic = QImage(portrait).convertToFormat(QImage::Format_ARGB32_Premultiplied);
            const QImage mask = personMask(pic, &err);
            auto at = [&](double fx, double fy) { return mask.isNull() ? -1 : int(mask.constScanLine(int(fy * (mask.height() - 1)))[int(fx * (mask.width() - 1))]); };
            if (!mask.isNull())
                mask.save(QDir(outDir).filePath(QStringLiteral("portrait_mask.png")));
            const EffectDef* rb = findEffect(QStringLiteral("remove_bg"));
            QImage out = pic.copy();
            std::unique_ptr<EffectState> st;
            rb->apply(out, resolveParams(rb->params, defaultParams(rb->params), 0), RenderContext(), st);
            out.save(QDir(outDir).filePath(QStringLiteral("portrait_removed.png")));
            check(at(0.5, 0.45) > 200 && at(0.02, 0.02) < 40 && at(0.98, 0.02) < 40 && qAlpha(out.pixel(5, 5)) < 40
                      && qAlpha(out.pixel(pic.width() / 2, pic.height() * 45 / 100)) > 220,
                  QStringLiteral("Remove Background on a photo: the person kept (%1), the background gone (%2, %3)")
                      .arg(at(0.5, 0.45)).arg(at(0.02, 0.02)).arg(at(0.98, 0.02)));
            // Quality: the matte, twice (the second frame with the model's memory of the first).
            EffectInstance q = makeEffect(*rb, 2);
            q.params[QStringLiteral("model")].value = 1;
            q.params[QStringLiteral("steady")].value = 0;
            const Params qp = resolveParams(rb->params, q.params, 0);
            std::unique_ptr<EffectState> qs;
            QImage matte;
            QElapsedTimer t;
            for (int i = 0; i < 2; ++i) {
                matte = pic.copy();
                t.restart();
                rb->apply(matte, qp, RenderContext(), qs);
            }
            const qint64 qms = t.elapsed();
            matte.save(QDir(outDir).filePath(QStringLiteral("portrait_quality.png")));
            const int qBack = qAlpha(matte.pixel(5, 5)), qPerson = qAlpha(matte.pixel(pic.width() / 2, pic.height() * 45 / 100));
            check(qBack < 40 && qPerson > 220,
                  QStringLiteral("Remove Background, Quality: the person kept (%1), the background gone (%2), %3 ms a frame on %4")
                      .arg(qPerson).arg(qBack).arg(qms).arg(segmentationDevice()));
        }
    }
    // ---- Captions as a subtitle track in a render (MP4 timed text, Matroska SRT) ----
    {
        ProjectSnapshot s;
        s.settings.width = 320;
        s.settings.height = 180;
        s.media[2] = video;
        Track v;
        v.id = 20;
        v.type = TrackType::Video;
        Clip c;
        c.id = 21;
        c.mediaId = 2;
        c.streamIndex = video->info.video.front().index;
        c.length = 3 * kUsPerSecond;
        v.clips.push_back(c);
        s.timeline.tracks.push_back(v);
        QStringList found;
        for (const char* ext : {"mp4", "mkv"}) {
            ExportSettings es = settingsFromPreset(exportPresets().front(), s.settings);
            es.width = 320;
            es.height = 180;
            es.audioEncoder.clear();
            es.muxer.clear(); // (from the extension)
            es.path = QDir(outDir).filePath(QStringLiteral("subs.%1").arg(QLatin1String(ext)));
            es.rangeStart = kUsPerSecond / 2;
            es.rangeEnd = 3 * kUsPerSecond;
            es.captions = {{0, kUsPerSecond, QStringLiteral("First line")}, {kUsPerSecond / 5, kUsPerSecond, QStringLiteral("Also")}, // (both start at the range: one after the other)
                           {2 * kUsPerSecond, 5 * kUsPerSecond, QStringLiteral("Second")}};
            Exporter ex;
            const bool ok = ex.run(s, es);
            // Read the subtitle packets back.
            AVFormatContext* fc = nullptr;
            QStringList texts;
            if (ok && avformat_open_input(&fc, es.path.toUtf8().constData(), nullptr, nullptr) == 0) {
                avformat_find_stream_info(fc, nullptr);
                AVPacket* p = av_packet_alloc();
                while (av_read_frame(fc, p) >= 0) {
                    if (fc->streams[p->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                        const bool tx3g = fc->streams[p->stream_index]->codecpar->codec_id == AV_CODEC_ID_MOV_TEXT;
                        const double at = p->pts * av_q2d(fc->streams[p->stream_index]->time_base);
                        if (p->size > (tx3g ? 2 : 0)) // (MP4 fills gaps with empty samples)
                            texts << QStringLiteral("%1@%2").arg(QString::fromUtf8(reinterpret_cast<const char*>(p->data) + (tx3g ? 2 : 0), p->size - (tx3g ? 2 : 0))).arg(at, 0, 'f', 1);
                    }
                    av_packet_unref(p);
                }
                av_packet_free(&p);
                avformat_close_input(&fc);
            }
            found << QStringLiteral("%1: %2 %3").arg(QLatin1String(ext), texts.join(QStringLiteral(", ")), ex.error());
            check(ok && texts == QStringList{QStringLiteral("First line@0.0"), QStringLiteral("Also@0.0"), QStringLiteral("Second@1.5")},
                  QStringLiteral("captions become a subtitle track in a %1 render, timed from the range's start (%2)").arg(QLatin1String(ext), found.last()));
        }
    }
    // ---- Subtitles: SRT / VTT ----
    {
        const QString srt = QStringLiteral("1\r\n00:00:01,500 --> 00:00:03,250\r\n<i>Hello</i> there\r\nsecond line\r\n\r\n"
                                           "2\r\n00:01:02,000 --> 00:01:04,000\r\n{\\an8}Top\r\n");
        const std::vector<Caption> c = parseSubtitles(srt);
        const QString vtt = QStringLiteral("WEBVTT\n\nNOTE a comment\n\nid1\n00:01.500 --> 00:03.250 align:start\nHello there\nsecond line\n\n"
                                           "01:02.000 --> 01:04.000\nTop\n");
        const std::vector<Caption> v = parseSubtitles(vtt);
        check(c.size() == 2 && c[0].start == 1500000 && c[0].end == 3250000 && c[0].text == QStringLiteral("Hello there\nsecond line")
                  && c[1].start == 62 * kUsPerSecond && c[1].text == QStringLiteral("Top") && v == c && parseSubtitles(writeSrt(c)) == c
                  && parseSubtitles(writeVtt(c)) == c && writeVtt(c).startsWith(QStringLiteral("WEBVTT")),
              QStringLiteral("subtitles: SRT and WebVTT read (tags dropped, hours optional) and written back the same"));
        const std::vector<Caption> para{{0, kUsPerSecond, QStringLiteral("One\n\nTwo"), 0}, {kUsPerSecond, 2 * kUsPerSecond, QStringLiteral("Three"), 0}};
        const std::vector<Caption> back = parseSubtitles(writeSrt(para)), backVtt = parseSubtitles(writeVtt(para));
        check(back.size() == 2 && back[0].text == QStringLiteral("One\nTwo") && backVtt.size() == 2 && backVtt[0].text == QStringLiteral("One\nTwo"),
              QStringLiteral("subtitles: a blank line inside a caption does not end its cue (%1 read back)").arg(back.size()));
        const std::vector<Caption> marks{{0, kUsPerSecond, QStringLiteral("Tom & Jerry: 3 < 4 > 2 \"ok\""), 0}};
        const auto yt = parseSubtitles(QStringLiteral("WEBVTT\n\n00:00.000 --> 00:01.000\n&gt;&gt; Fish &amp; chips&nbsp;<c>now</c>\n"));
        check(parseSubtitles(writeVtt(marks)) == marks && !writeVtt(marks).contains(QLatin1String("3 < 4")) && yt.size() == 1
                  && yt[0].text == QStringLiteral(">> Fish & chips now"),
              QStringLiteral("subtitles: WebVTT & < > written and read as entities (%1)").arg(yt.empty() ? QString() : yt[0].text));
        const QByteArray ansi("1\r\n00:00:01,000 --> 00:00:02,000\r\nCaf\xe9\r\n");
        QByteArray utf16("\xff\xfe", 2);
        const QString wide = QStringLiteral("1\n00:00:01,000 --> 00:00:02,000\nCaf\u00e9\n");
        utf16.append(reinterpret_cast<const char*>(wide.utf16()), wide.size() * 2);
        const auto a = parseSubtitles(subtitleText(ansi)), w = parseSubtitles(subtitleText(utf16));
        check(a.size() == 1 && a[0].text == QStringLiteral("Caf\u00e9") && w.size() == 1 && w[0].text == QStringLiteral("Caf\u00e9")
                  && subtitleText(QByteArray("Caf\xc3\xa9")) == QStringLiteral("Caf\u00e9"),
              QStringLiteral("subtitles: Windows-1252 and UTF-16 files read (%1, %2)").arg(a.empty() ? QString() : a[0].text, w.empty() ? QString() : w[0].text));
    }
    {
        // ";" (drop-frame) only for NTSC rates; whole rates read with ":".
        const QString pal = formatTimecode(61 * kUsPerSecond + 500000, FrameRate{25, 1}), ntsc = formatTimecode(61 * kUsPerSecond, FrameRate{30000, 1001});
        check(pal == QStringLiteral("00:01:01:12") && ntsc == QStringLiteral("00:01:01;00"), QStringLiteral("timecode: %1 at 25 fps, %2 at 29.97").arg(pal, ntsc));
    }
    {
        // A speaker turn transcribed as one caption is cut on words, in order, each piece timed by its characters.
        Caption turn;
        turn.start = 1 * kUsPerSecond;
        turn.end = 13 * kUsPerSecond;
        turn.speaker = 1;
        turn.text = QStringLiteral("And I am Bolt. I paint cars. Mostly blue. Sometimes, if I am feeling brave, slightly bluer. Let's start with a big question.");
        std::vector<Caption> c{turn};
        splitLongCaptions(c, 42);
        QStringList texts;
        bool ok = c.size() > 2 && c.front().start == turn.start && c.back().end == turn.end;
        for (size_t i = 0; i < c.size(); ++i) {
            texts << c[i].text;
            ok = ok && c[i].text.size() <= 42 && c[i].speaker == 1 && c[i].end > c[i].start && (i == 0 || c[i].start >= c[i - 1].end);
        }
        check(ok && texts.join(QLatin1Char(' ')) == turn.text,
              QStringLiteral("subtitles: a long speaker turn is cut into captions of 42 characters or fewer (%1)").arg(texts.join(QStringLiteral(" | "))));
    }
    print(QStringLiteral("%1  tools: %2 failure(s)").arg(failures ? QStringLiteral("FAIL") : QStringLiteral("DONE")).arg(failures));
    return failures ? 1 : 0;
}

int cmdHarden(const QString& mediaDir, const QString& examplesDir, const QString& outDir, int iterations); // hardening.cpp

int main(int argc, char** argv)
{
    // Headless GUI application: text needs fonts and the GPU test needs
    // OpenGL. On Windows the native platform does both without showing any
    // window; elsewhere fall back to offscreen when there is no display.
#ifndef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY")
        && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    QGuiApplication app(argc, argv);
    ff::initialize();
    const QStringList args = app.arguments().mid(1);
    if (args.size() == 2 && args[0] == QLatin1String("render-status")) {
        QDir().mkpath(args[1]);
        ProjectSnapshot snap;
        ExportSettings es; es.path = QDir(args[1]).filePath("status.mp4");
        es.muxer = "mp4"; es.videoEncoder = "libx264"; es.audioEncoder = "aac";
        es.width = 160; es.height = 90; es.rangeEnd = kUsPerSecond;
        ExportStatus final; QStringList stages; bool successAfterClose = false;
        Exporter exporter;
        auto report = [&](const ExportStatus& value) {
            final = value;
            if (!stages.contains(value.stage)) stages.append(value.stage);
            if (value.stage == QLatin1String("Complete")) {
                QFile file(es.path); successAfterClose = file.open(QIODevice::ReadOnly) && file.size() > 0;
            }
        };
        bool ok = exporter.run(snap, es, {}, nullptr, report);
        bool passed = ok && final.stage == "Complete" && final.frames == 30 && final.samples == 48000
                      && final.videoPackets > 0 && final.audioPackets > 0 && final.fileBytes > 0 && successAfterClose
                      && stages.indexOf("Finalizing container") > stages.indexOf("Flushing video")
                      && stages.indexOf("Complete") > stages.indexOf("Closing output");
        es.path = QDir(args[1]).filePath("cancel.mp4");
        ok = exporter.run(snap, es, [](double){ return false; }, nullptr, report);
        passed = passed && !ok && final.stage == "Cancelled" && !QFileInfo::exists(es.path);
        es.videoEncoder = "not-an-encoder";
        ok = exporter.run(snap, es, {}, nullptr, report);
        passed = passed && !ok && final.stage == "Failed" && !final.detail.isEmpty();
        es.videoEncoder.clear(); es.audioEncoder = "pcm_s16le"; es.muxer = "wav";
        es.path = QDir(args[1]).filePath("audio.wav");
        ok = exporter.run(snap, es, {}, nullptr, report);
        passed = passed && ok && final.frames == 0 && final.samples == 48000 && final.audioPackets > 0;
        print(passed ? "PASS render status: video/audio counters, finalization, closed output, cancellation, failure, audio-only" : "FAIL render status");
        return passed ? 0 : 1;
    }
    if (args.size() >= 2 && args[0] == QLatin1String("probe"))
        return cmdProbe(args.mid(1));
    if (args.size() == 3 && args[0] == QLatin1String("export"))
        return cmdExport(args[1], args[2]);
    if (args.size() == 3 && args[0] == QLatin1String("effects"))
        return cmdEffects(args[1], args[2]);
    if (args.size() == 3 && args[0] == QLatin1String("presets"))
        return cmdPresets(args[1], args[2]);
    if (args.size() == 3 && args[0] == QLatin1String("racks"))
        return cmdRacks(args[1], args[2]);
    if (args.size() == 1 && args[0] == QLatin1String("beatstress"))
        return cmdBeatStress();
    if (args.size() == 1 && args[0] == QLatin1String("beats"))
        return cmdBeats();
    if (args.size() == 1 && args[0] == QLatin1String("random"))
        return cmdRandom();
    if (args.size() == 3 && args[0] == QLatin1String("random"))
        return cmdRandom(args[1], args[2]);
    if (args.size() == 3 && args[0] == QLatin1String("editing"))
        return cmdEditing(args[1], args[2]);
    if (args.size() == 3 && args[0] == QLatin1String("ramps"))
        return cmdRamps(args[1], args[2]);
    if (args.size() == 3 && args[0] == QLatin1String("envelopes"))
        return cmdEnvelopes(args[1], args[2]);
    if (args.size() == 2 && args[0] == QLatin1String("audio-cache"))
        return cmdAudioCache(args[1]);
    if (args.size() == 3 && args[0] == QLatin1String("audiofx"))
        return cmdAudioFx(args[1], args[2]);
    if (args.size() == 2 && args[0] == QLatin1String("live"))
        return cmdLive(args[1]);
    if (args.size() == 3 && args[0] == QLatin1String("consoles"))
        return cmdConsoles(args[1], args[2]);
    if (args.size() == 3 && args[0] == QLatin1String("gpu"))
        return cmdGpu(args[1], args[2]);
    if (args.size() == 3 && args[0] == QLatin1String("speed"))
        return cmdSpeed(args[1], args[2]);
    if (args.size() == 2 && args[0] == QLatin1String("generators"))
        return cmdGenerators(args[1]);
    if (args.size() == 3 && args[0] == QLatin1String("tools"))
        return cmdTools(args[1], args[2]);
    if (args.size() == 1 && args[0] == QLatin1String("formats"))
        return cmdFormats();
    if ((args.size() == 3 || args.size() == 5) && args[0] == QLatin1String("captions"))
        return cmdCaptions(args[1], args[2], args.value(3), args.value(4));
    if (args.size() == 2 && args[0] == QLatin1String("vst3"))
        return cmdVst3(args[1]);
    if (args.size() == 1 && args[0] == QLatin1String("youtube-options"))
        return cmdYouTubeOptions();
    if (args.size() == 3 && args[0] == QLatin1String("youtube"))
        return cmdYouTube(args[1], args[2]);
    if (args.size() == 3 && args[0] == QLatin1String("docanimations"))
        return cmdDocAnimations(args[1], args[2]);
    if ((args.size() == 3 || args.size() == 4) && args[0] == QLatin1String("docs"))
        return cmdDocs(args[1], args[2], args.value(3));
    if ((args.size() == 4 || args.size() == 5) && args[0] == QLatin1String("harden"))
        return cmdHarden(args[1], args[2], args[3], args.size() == 5 ? args[4].toInt() : 300);
    if (args.size() == 2 && args[0] == QLatin1String("sfx"))
        return cmdSfx(args[1]);
    if (args.size() >= 3 && args[0] == QLatin1String("fxshowcase"))
        return cmdFxShowcase(args.mid(1));
    if (args.size() == 3 && args[0] == QLatin1String("examples"))
        return cmdExamples(args[1], args[2]);
    print(QStringLiteral("usage: mvp_selftest probe <files...> | export <media> <outdir> | effects <media> <outdir> | generators <outdir> | speed <media> <outdir> | gpu <testmedia-dir> <outdir> | audiofx <testmedia-dir> <outdir> | envelopes <testmedia-dir> <outdir> | ramps <testmedia-dir> <outdir> | racks <testmedia-dir> <outdir> | editing <testmedia-dir> <outdir> | beats | random [<media> <outdir>] | presets <testmedia-dir> <outdir> | formats"));
    return 2;
}
