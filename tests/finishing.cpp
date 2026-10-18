#include "core/AudioFinishing.h"
#include "core/GraphicsWorkflow.h"
#include "core/Graphics.h"
#include "core/MotionGraphics.h"
#include "core/AudioMixer.h"
#include "core/TimelineOps.h"
#include "core/YouTubeUpload.h"
#include "core/FFmpeg.h"
#include <QGuiApplication>
#include <QDataStream>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QTextStream>
#include <cmath>
#include <limits>

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv); // (the thumbnail draws text)
    ff::initialize();
    int failures = 0;
    auto check = [&](bool ok, QString message) {
        QTextStream(stdout) << (ok ? "PASS  " : "FAIL  ") << message << Qt::endl;
        failures += !ok;
    };
    constexpr int rate = 48000, frames = rate * 8;
    constexpr double pi = 3.141592653589793;
    auto tone = [&](int64_t pos, int n, float* out) {
        for (int i = 0; i < n; ++i)
            out[2 * i] = out[2 * i + 1] = float(.1 * std::sin(2 * pi * 1000 * (pos + i) / rate));
    };
    auto r = finishing::analyze(rate, frames, tone);
    check(r.error.isEmpty() && r.audible, "EBU scanner initializes and measures real stereo samples");
    check(std::abs(r.integrated + 20) < .3, "1 kHz stereo tone agrees with expected K-weighted loudness");
    check(std::abs(r.samplePeak + 20) < .05 && r.truePeak >= r.samplePeak, "sample and oversampled true peaks are distinct measurements");
    check(std::abs(r.correlation - 1) < .00001, "identical stereo channels correlate positively");
    check(std::abs(finishing::safeGain(r, {"Test", -16, -1.5}) - 4) < .3, "safe gain reaches loudness target when headroom permits");
    auto hot = finishing::analyze(rate, frames, [&](int64_t pos, int n, float* out) {
        for (int i = 0; i < n; ++i)
            out[2 * i] = out[2 * i + 1] = float(.9 * std::sin(2 * pi * 12000 * (pos + i) / rate + pi / 4));
    });
    check(hot.error.isEmpty() && hot.truePeak > hot.samplePeak + 1.5, "intersample peaks are detected beyond sample peaks");
    auto crest = finishing::analyze(rate, frames, [&](int64_t pos, int n, float* out) {
        for (int i = 0; i < n; ++i) {
            auto t = pos + i;
            out[2 * i] = out[2 * i + 1] =
                float(t % rate < 48 ? .9 * std::sin(2 * pi * 12000 * t / rate + pi / 4) : .03 * std::sin(2 * pi * 1000 * t / rate));
        }
    });
    auto gain = finishing::safeGain(crest, {"Test", -16, -2});
    check(gain + crest.truePeak <= -2 && gain < -16 - crest.integrated, "gain is limited by true-peak ceiling without crushing dynamics");
    {
        // With a master limiter the gain goes in front of it and reaches the loudness target; without one, at the end.
        Timeline tl;
        EffectInstance limiter;
        limiter.id = tl.nextId++;
        limiter.type = "a_limiter";
        tl.audioMasterEffects.push_back(limiter);
        QString error;
        const bool applied = finishing::applyGain(tl, crest, {"Test", -16, -2}, &error);
        Timeline plain;
        const bool plainApplied = finishing::applyGain(plain, crest, {"Test", -16, -2}, &error);
        check(applied && tl.audioMasterEffects.size() == 2 && tl.audioMasterEffects[0].type == "a_gain" && tl.audioMasterEffects[1].type == "a_limiter"
                  && std::abs(tl.audioMasterEffects[0].params["gain"].value - std::min(12., -16 - crest.integrated)) < 1e-6 && plainApplied
                  && plain.audioMasterEffects.back().type == "a_gain" && std::abs(plain.audioMasterEffects.back().params["gain"].value - gain) < 1e-6,
              "a master limiter gets the gain in front of it, up to the loudness target");
    }
    auto silence = finishing::analyze(rate, frames, [](int64_t, int n, float* out) { std::fill(out, out + 2 * n, 0.f); });
    check(silence.error.isEmpty() && !silence.audible && !std::isfinite(finishing::safeGain(silence, {})),
          "silence never receives an extreme normalization gain");
    check(silence.silenceSeconds == 8 && !finishing::warnings(silence, {}).isEmpty(), "long silent sections are flagged");
    auto phase = finishing::analyze(rate, frames, [&](int64_t pos, int n, float* out) {
        tone(pos, n, out);
        for (int i = 0; i < n; ++i)
            out[2 * i + 1] *= -1;
    });
    check(phase.correlation < -.99 && finishing::warnings(phase, {}).join(' ').contains("mono"),
          "out-of-phase stereo gets a mono compatibility warning");
    auto invalid = finishing::analyze(rate, frames, [&](int64_t pos, int n, float* out) {
        tone(pos, n, out);
        if (!pos) {
            out[0] = std::numeric_limits<float>::quiet_NaN();
            out[1] = std::numeric_limits<float>::infinity();
        }
    });
    check(invalid.invalidSamples == 2 && !std::isfinite(finishing::safeGain(invalid, {})),
          "NaN and infinity samples are counted and block gain application");
    auto cancelled = finishing::analyze(rate, frames, tone, [](double p) { return p < .2; });
    check(cancelled.cancelled && !std::isfinite(finishing::safeGain(cancelled, {})), "cancelled scans cannot be applied");
    check(!finishing::analyze(rate, 0, tone).error.isEmpty() && !finishing::analyze(1, 10, tone).error.isEmpty(),
          "invalid lengths and sample rates fail before allocation");
    auto shortMix = finishing::analyze(rate, rate / 5, tone);
    check(!shortMix.audible && !std::isfinite(finishing::safeGain(shortMix, {})),
          "sub-400 ms signals do not pretend to have programme loudness");
    check(!std::isfinite(finishing::safeGain(r, {"Bad", std::numeric_limits<double>::infinity(), -1})),
          "invalid target values are rejected");
    auto impossible = r;
    impossible.truePeak = 100;
    check(!std::isfinite(finishing::safeGain(impossible, {})), "unsupported attenuation cannot silently violate the true-peak ceiling");
    Timeline tl = defaultTimeline();
    auto previous = tl;
    QString error;
    check(!finishing::applyGain(tl, silence, {}, &error) && tl == previous, "failed finishing leaves the timeline unchanged");
    check(finishing::applyGain(tl, r, {}, &error) && tl.audioMasterEffects.size() == 1 && tl.audioMasterEffects[0].type == "a_gain",
          "finishing adds one master gain effect");
    QTemporaryDir tmp;
    auto wav = tmp.filePath("tone.wav");
    QFile file(wav);
    file.open(QIODevice::WriteOnly);
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    file.write("RIFF", 4);
    stream << quint32(36 + frames * 8);
    file.write("WAVEfmt ", 8);
    stream << quint32(16) << quint16(3) << quint16(2) << quint32(rate) << quint32(rate * 8) << quint16(8) << quint16(32);
    file.write("data", 4);
    stream << quint32(frames * 8);
    std::vector<float> samples(frames * 2);
    tone(0, frames, samples.data());
    for (float sample : samples)
        stream << sample;
    file.close();
    auto workingDirectory = QDir::currentPath();
    QDir::setCurrent(tmp.path());
    Project relativeProject;
    int relativeId = relativeProject.addMedia(probeMedia("tone.wav"));
    QDir().mkpath("saved");
    QString relativeError;
    Project reopened;
    bool relativeOk = relativeProject.save("saved/relative.mvp", &relativeError) && reopened.load("saved/relative.mvp", &relativeError);
    check(relativeOk && QDir::isAbsolutePath(relativeProject.mediaItem(relativeId)->path) && reopened.mediaItem(relativeId)->info.ok(),
          "relative media imports remain online when saved into a different folder");
    auto urlInfo = probeMedia(wav);
    urlInfo.path = "https://example.test/video.mp4";
    int urlId = relativeProject.addMedia(urlInfo);
    check(relativeProject.mediaItem(urlId)->path == urlInfo.path,
          "import path normalization preserves FFmpeg URLs without accessing the network");
    QDir::setCurrent(workingDirectory);
    Project project;
    int media = project.addMedia(probeMedia(wav));
    auto timeline = project.timeline();
    Track audio;
    audio.id = timeline.nextId++;
    audio.type = TrackType::Audio;
    audio.name = "Tone";
    Clip c;
    c.id = timeline.nextId++;
    c.mediaId = media;
    c.streamIndex = 0;
    c.length = 8 * kUsPerSecond;
    audio.clips.push_back(c);
    timeline.tracks = {audio};
    project.applyEdit(timeline, "Fixture");
    auto measured = finishing::analyze(*project.snapshot());
    auto normalized = project.timeline();
    finishing::applyGain(normalized, measured, {}, &error);
    project.applyEdit(normalized, "Finish");
    auto final = finishing::analyze(*project.snapshot());
    check(std::abs(final.integrated + 16) < .3 && final.truePeak <= -1.5, "offline scan and gain use the actual post-master mixer");
    project.undoStack()->undo();
    check(project.timeline() == timeline, "finishing gain can be undone exactly");
    auto snapshot = *project.snapshot();
    auto offline = std::make_shared<MediaItem>(*snapshot.media.at(media));
    offline->id = 9999;
    offline->path = tmp.filePath("missing.wav");
    snapshot.media[9999] = offline;
    check(!finishing::analyze(snapshot).warnings.join(' ').contains("Missing audio source"),
          "unused offline bin media does not produce a final-mix warning");
    snapshot.timeline.tracks[0].clips[0].mediaId = 9999;
    check(finishing::analyze(snapshot).warnings.join(' ').contains("Missing audio source"),
          "offline audio used by the timeline is reported");
    auto make = [](QString id, double x, double y, double w = .1, double h = .1) {
        return QJsonObject{{"type", "rect"}, {"id", id}, {"x", x}, {"y", y}, {"width", w}, {"height", h}, {"fill", "#ffffff"}};
    };
    QJsonObject scene{{"version", 2}, {"layers", QJsonArray{make("a", .1, .1), make("b", .4, .3, .2), make("c", .8, .5)}}};
    auto addressable = graphics_workflow::prepare(scene);
    check(graphics::validateScene(addressable).isEmpty() && !addressable["layers"].toArray()[0].toObject().contains("children"),
          "preparing flat scenes never inserts invalid null child arrays");
    auto aligned = graphics_workflow::layout(scene, {"a", "b"}, "left", "safe", 0, &error);
    check(!aligned.isEmpty() && std::abs(aligned["layers"].toArray()[0].toObject()["x"].toDouble() - .05) < 1e-6
              && std::abs(aligned["layers"].toArray()[1].toObject()["x"].toDouble() - .05) < 1e-6,
          "alignment uses safe margins and multiple selected layers");
    auto distributed = graphics_workflow::layout(scene, {"a", "b", "c"}, "distribute_h", "selection", 0, &error);
    auto layers = distributed["layers"].toArray();
    double leftGap = layers[1].toObject()["x"].toDouble() - .2, rightGap = .8 - (layers[1].toObject()["x"].toDouble() + .2);
    check(std::abs(leftGap - rightGap) < 1e-6, "distribution uses equal gaps for unequal layer widths");
    auto locked = graphics_workflow::setLocked(scene, {"a"}, true, &error);
    check(graphics_workflow::translate(locked, {"a"}, {.1, 0}, 0, &error).isEmpty(), "locked layers cannot be translated");
    check(graphics_workflow::layout(locked, {"a", "b"}, "left", "canvas", 0, &error).isEmpty(), "mixed locked selections fail atomically");
    auto unlocked = graphics_workflow::setLocked(locked, {"a"}, false, &error);
    check(!graphics_workflow::locked(unlocked, "a"), "locks survive data and can be removed");
    check(graphics_workflow::layout(scene, {"a", "b"}, "distribute_h", "canvas", 0, &error).isEmpty(),
          "distribution requires three layers");
    check(graphics_workflow::layout(scene, {"a", "a"}, "left", "canvas", 0, &error).isEmpty(), "duplicate selection IDs are rejected");
    auto parent = make("group", .1, .1, .5, .5);
    parent["type"] = "group";
    parent["rotation"] = 30;
    parent["scale_x"] = 1.5;
    auto child = make("child", .1, .2);
    child["parent"] = "group";
    QJsonObject nested{{"version", 2}, {"layers", QJsonArray{parent, child}}};
    auto before = motion::bounds(nested, "child", 0, {1600, 900});
    auto moved = graphics_workflow::translate(nested, {"child"}, {.1, .05}, 0, &error);
    auto after = motion::bounds(moved, "child", 0, {1600, 900});
    check(!moved.isEmpty() && std::abs(after.x() - before.x() - 160) < .01 && std::abs(after.y() - before.y() - 45) < .01,
          "layout offsets map through rotated, scaled parent coordinates");
    auto parentLocked = graphics_workflow::setLocked(nested, {"group"}, true, &error);
    check(graphics_workflow::locked(parentLocked, "child"), "group locks protect descendants");
    check(graphics_workflow::translate(nested, {"group", "child"}, {.1, 0}, 0, &error).isEmpty(),
          "parent and child selection cannot double-transform a layer");
    auto evaluated = motion::allBounds(nested, 0, {1600, 900});
    check(evaluated.value("child") == before && evaluated.value("group") == motion::bounds(nested, "group", 0, {1600, 900}),
          "bulk bounds match individual transformed group bounds");
    auto animated = make("animated", 0, .1);
    animated["x"] = QJsonObject{{"keyframes", QJsonArray{QJsonArray{0, .1, "ease_in"}, QJsonArray{2, .5}}}};
    QJsonObject animation{{"version", 2}, {"layers", QJsonArray{animated}}};
    auto arranged = graphics_workflow::layout(animation, {"animated"}, "left", "safe", 1, &error);
    auto pos = arranged["layers"].toArray()[0].toObject()["x"];
    check(pos.isObject() && std::abs(motion::evaluate(pos, 1, arranged).toDouble() - .05) < .00001,
          "arranging animated layers shifts keys without flattening motion");
    QVector<double> xs, ys;
    auto snapped = graphics_workflow::snap(scene, {"a"}, {.345, 0}, 0, .01, &xs, &ys);
    check(std::abs(snapped.x() - .35) < .00001 && !xs.isEmpty(), "snapping finds canvas centers and returns visible guide coordinates");
    QJsonObject brand{{"version", 1},           {"name", "Demo"},      {"font", "Arial"},         {"primary", "#ff0000"},
                      {"secondary", "#00ff00"}, {"accent", "#0000ff"}, {"background", "#101010"}, {"foreground", "#ffffff"}};
    auto tagged = scene;
    auto taggedLayers = tagged["layers"].toArray();
    auto a = taggedLayers[0].toObject();
    a["brand_role"] = "primary";
    taggedLayers[0] = a;
    tagged["layers"] = taggedLayers;
    auto branded = graphics_workflow::applyBrand(tagged, brand, &error);
    check(branded["layers"].toArray()[0].toObject()["fill"] == "#ff0000", "brand palette changes explicitly tagged fills");
    auto brandedLock = graphics_workflow::applyBrand(graphics_workflow::setLocked(tagged, {"a"}, true, &error), brand, &error);
    check(brandedLock["layers"].toArray()[0].toObject()["fill"] == "#ffffff", "brand kits preserve locked layer styles");
    auto symbolLayer = a;
    symbolLayer["id"] = "a";
    auto instance = make("instance", .2, .2);
    instance["type"] = "instance";
    instance["symbol"] = "card";
    auto symbolScene = tagged;
    symbolScene["layers"] = QJsonArray{instance};
    symbolScene["symbols"] = QJsonObject{{"card", QJsonObject{{"layers", QJsonArray{symbolLayer}}}}};
    auto symbolBrand = graphics_workflow::applyBrand(symbolScene, brand, &error);
    check(!symbolBrand.isEmpty()
              && symbolBrand["symbols"].toObject()["card"].toObject()["layers"].toArray()[0].toObject()["fill"] == "#ff0000",
          "brand kits apply inside reusable symbol definitions");
    auto protectedBrand =
        graphics_workflow::applyBrand(graphics_workflow::setLocked(symbolScene, {"instance"}, true, &error), brand, &error);
    check(!protectedBrand.isEmpty() && protectedBrand["symbols"] == symbolScene["symbols"],
          "brand kits preserve shared definitions used by locked instances");
    auto dynamic = a;
    dynamic["fill"] = QJsonObject{{"keyframes", QJsonArray{QJsonArray{0, "#ffffff"}, QJsonArray{1, "#000000"}}}};
    auto dynamicScene = tagged;
    dynamicScene["layers"] = QJsonArray{dynamic};
    auto dynamicBrand = graphics_workflow::applyBrand(dynamicScene, brand, &error);
    check(!dynamicBrand.isEmpty() && dynamicBrand["layers"].toArray()[0].toObject()["fill"] == dynamic["fill"],
          "brand kits do not flatten animated colours");
    brand["primary"] = "no colour";
    check(graphics_workflow::applyBrand(tagged, brand, &error).isEmpty(), "invalid brand kits fail before scene changes");
    QJsonObject legacy{{"version", 1},
                       {"layers", QJsonArray{QJsonObject{{"type", "rect"}, {"x", .1}, {"y", .1}, {"width", .1}, {"height", .1}}}}};
    auto prepared = graphics_workflow::prepare(legacy);
    check(!prepared["layers"].toArray()[0].toObject()["id"].toString().isEmpty() && prepared == graphics_workflow::prepare(prepared),
          "legacy scene layer IDs are stable and preparation is idempotent");
    {
        Project p;
        Timeline tl = p.timeline();
        const int track = ops::addTrack(tl, TrackType::Audio);
        const char* files[] = {"C:/Music/01. Beta Boyz.flac", "C:/Music/02 - Not A Chance.mp3", "C:/Music/2Pac Übergang.wav"};
        const TimeUs starts[] = {0, 259600000, 742 * kUsPerSecond};
        for (int i = 2; i >= 0; --i) {
            MediaInfo info;
            info.path = QString::fromUtf8(files[i]);
            info.duration = 300 * kUsPerSecond;
            const int id = p.addMedia(info);
            Clip c;
            c.id = tl.allocId();
            c.mediaId = id;
            c.start = starts[i];
            c.length = kUsPerSecond;
            c.name = p.mediaItem(id)->displayName();
            tl.tracks[size_t(track)].clips.push_back(c);
        }
        p.applyEdit(tl, "album");
        check(upload::trackListing(p, track)
                  == QString::fromUtf8("01. Beta Boyz - 00:00\n02. Not A Chance - 04:19\n03. 2Pac Übergang - 12:22"),
              "track listing numbers events by start, names them from files without track numbers (2Pac keeps its 2)");
        Clip renamed = tl.tracks[size_t(track)].clips[0];
        renamed.id = tl.allocId();
        renamed.start = 3725 * kUsPerSecond;
        renamed.name = "The Romper Stomp";
        tl.tracks[size_t(track)].clips.push_back(renamed);
        p.applyEdit(tl, "long album");
        check(upload::trackListing(p, track).endsWith("\n03. " + QString::fromUtf8("2Pac Übergang")
                                                         + " - 0:12:22\n04. The Romper Stomp - 1:02:05"),
              "track listing keeps renamed events and shows hours past an hour: " + upload::trackListing(p, track));
    }
    {
        Timeline tl = defaultTimeline();
        Clip long_;
        long_.id = tl.allocId();
        long_.length = 125 * kUsPerSecond;
        tl.tracks[0].clips.push_back(long_);
        tl.markers.push_back({tl.allocId(), 15 * kUsPerSecond, 0, "Lore", 0});
        tl.markers.push_back({tl.allocId(), 61 * kUsPerSecond, 20 * kUsPerSecond, "", 0});
        tl.markers.push_back({tl.allocId(), 65 * kUsPerSecond, 0, "Raids", 0});
        QStringList warnings;
        const QString chapters = upload::youtubeChapters(tl, &warnings);
        check(chapters == "00:00 Intro\n00:15 Lore\n01:01 Chapter 2\n01:05 Raids" && warnings.size() == 1 && warnings[0].contains("Chapter 2"),
              "YouTube chapters from markers start with Intro at 00:00 and flag ones under 10 s: " + chapters + " / " + warnings.join(' '));
        tl.chapters.append(QJsonObject{{"name", "Origins"}, {"start", "0"}});
        tl.chapters.append(QJsonObject{{"name", "Wrath"}, {"start", QString::number(40 * kUsPerSecond)}});
        check(upload::youtubeChapters(tl, &warnings) == "00:00 Origins\n00:40 Wrath" && warnings.size() == 1 && warnings[0].contains("at least 3"),
              "story chapters come first, and fewer than 3 is flagged");
        QImage frame(640, 480, QImage::Format_RGB32);
        frame.fill(QColor(30, 90, 200));
        const QImage plain = upload::thumbnail(frame, {}), titled = upload::thumbnail(frame, "Twenty Years of Azeroth");
        int white = 0;
        for (int y = 400; y < 720; y += 4)
            for (int x = 0; x < 1280; x += 4)
                white += titled.pixelColor(x, y).lightness() > 240;
        check(plain.size() == QSize(1280, 720) && plain.pixelColor(640, 360) == QColor(30, 90, 200) && plain.pixelColor(5, 5) == QColor(30, 90, 200)
                  && white > 400 && titled.pixelColor(640, 100) == QColor(30, 90, 200),
              QString("thumbnail is 1280x720, filled by the frame, with the title in white along the bottom (%1 white samples)").arg(white));
    }
    return failures;
}
