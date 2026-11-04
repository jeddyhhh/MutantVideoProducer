#include "core/AudioEffects.h"
#include "core/AudioFinishing.h"
#include "core/GraphicsWorkflow.h"
#include "core/Graphics.h"
#include "core/MotionGraphics.h"
#include "core/AudioMixer.h"
#include "core/AudioSync.h"
#include "core/AudioDecoder.h"
#include "core/Exporter.h"
#include "core/Generators.h"
#include "core/Compositor.h"
#include "core/Multicam.h"
#include "core/StemSeparation.h"
#include "core/OpticalFlow.h"
#include "core/ColorMatch.h"
#include <random>
#include <QPainter>
#include "core/TimelineOps.h"
#include "core/YouTubeUpload.h"
#include "core/FFmpeg.h"
#include <QGuiApplication>
#include <QDataStream>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QTextStream>
#include <algorithm>
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
    finishing::Report onTarget = r;
    onTarget.integrated = -16.07;
    onTarget.truePeak = -1.42;
    check(!finishing::warnings(onTarget, {"Test", -16, -1.5}).join(' ').contains("dynamics control"),
          "a mix already at its loudness target is not told it needs a limiter");
    auto hot = finishing::analyze(rate, frames, [&](int64_t pos, int n, float* out) {
        for (int i = 0; i < n; ++i)
            out[2 * i] = out[2 * i + 1] = float(.9 * std::sin(2 * pi * 12000 * (pos + i) / rate + pi / 4));
    });
    check(hot.error.isEmpty() && hot.truePeak > hot.samplePeak + 1.5, "intersample peaks are detected beyond sample peaks");
    {
        // The limiter holds its ceiling as a true peak: the same hot signal through it, 3 dB down.
        const auto* def = findAudioEffect("a_limiter");
        auto limiter = def->create();
        limiter->reset(rate);
        ParamMap values;
        values["ceiling"].value = -3;
        const Params params = resolveParams(def->params, values, 0);
        auto limited = finishing::analyze(rate, frames, [&](int64_t pos, int n, float* out) {
            for (int i = 0; i < n; ++i)
                out[2 * i] = out[2 * i + 1] = float(.9 * std::sin(2 * pi * 12000 * (pos + i) / rate + pi / 4));
            limiter->process(out, n, params);
        });
        check(limited.error.isEmpty() && limited.truePeak <= -2.7 && limited.truePeak > -6,
              QStringLiteral("the limiter holds a -3 dB ceiling between samples too (true peak %1 dBTP)").arg(limited.truePeak, 0, 'f', 2));
        // And on a full mix's worth of high frequencies: loud noise with sharp clicks in it.
        auto noisy = def->create();
        noisy->reset(rate);
        uint32_t seed = 12345;
        auto limitedNoise = finishing::analyze(rate, frames, [&](int64_t pos, int n, float* out) {
            for (int i = 0; i < n; ++i) {
                seed = seed * 1664525u + 1013904223u;
                const double white = (double(seed >> 8) / double(1u << 24)) * 2 - 1;
                const double click = (pos + i) % 4801 < 3 ? ((pos + i) % 2 ? 1.0 : -1.0) : 0.0;
                out[2 * i] = out[2 * i + 1] = float(std::clamp(white * 0.7 + click * 0.9, -1.0, 1.0));
            }
            noisy->process(out, n, params);
        });
        check(limitedNoise.error.isEmpty() && limitedNoise.truePeak <= -2.85,
              QStringLiteral("the limiter holds a -3 dB ceiling on loud noise and clicks (true peak %1 dBTP)").arg(limitedNoise.truePeak, 0, 'f', 2));
    }
    {
        // Pitch Shift (Rubber Band) holds back more on real sound than on the silence its delay is measured with: a tone
        // that changes (a sweep under a moving level, like a voice) through it in the mixer's blocks never drops out.
        const auto* def = findAudioEffect("a_pitch");
        auto shift = def ? def->create() : nullptr;
        ParamMap values;
        values["semitones"].value = -0.5;
        const Params params = def ? resolveParams(def->params, values, 0) : Params();
        int dropouts = 0, lat = 0;
        if (shift) {
            shift->reset(rate);
            lat = shift->latency(params);
            const int len = rate * 12;
            std::vector<float> buf(size_t(len) * 2);
            double phase = 0;
            uint32_t seed = 99;
            for (int i = 0; i < len; ++i) { // syllables: a voiced tone and a breath of noise, 180 ms on, 60 ms off
                const double t = double(i) / rate, f = 120 + 60 * std::sin(2 * pi * 0.9 * t);
                phase += 2 * pi * f / rate;
                seed = seed * 1664525u + 1013904223u;
                const double noise = (double(seed >> 8) / double(1u << 24)) * 2 - 1;
                const double syllable = std::fmod(t, 0.24) < 0.18 ? std::sin(pi * std::fmod(t, 0.24) / 0.18) : 0.0;
                buf[size_t(i) * 2] = buf[size_t(i) * 2 + 1] = float(syllable * (0.35 * std::sin(phase) + 0.1 * noise));
            }
            for (int i = 0; i < len; i += 1024)
                shift->process(buf.data() + size_t(i) * 2, std::min(1024, len - i), params);
            // A dropout: a sample of exact silence between sounding ones (the FIFO short by a sample or two: a click).
            for (int i = lat + rate; i + 3 < len; ++i)
                if (buf[size_t(i) * 2] == 0.0f && std::abs(buf[size_t(i - 1) * 2]) > 0.01f
                    && (std::abs(buf[size_t(i + 1) * 2]) > 0.01f || std::abs(buf[size_t(i + 2) * 2]) > 0.01f))
                    ++dropouts;
        }
        check(shift && dropouts == 0, QStringLiteral("Pitch Shift never drops out on changing sound (%1 dropouts, %2 samples of delay)").arg(dropouts).arg(lat));
    }
    {
        // De-reverb: 1 kHz bursts in a room ringing for 0.8 s (decaying noise after each): the tail between them goes down
        // by more than the bursts themselves.
        const auto* def = findAudioEffect("a_dereverb");
        auto dry = def ? def->create() : nullptr;
        const int len = rate * 6, period = rate * 6 / 10, burst = rate / 10;
        std::vector<float> wet(size_t(len) * 2);
        uint32_t seed = 7;
        for (int i = 0; i < len; ++i) {
            const int t = i % period;
            double v = t < burst ? 0.5 * std::sin(2 * pi * 1000 * i / rate) : 0.0;
            seed = seed * 1664525u + 1013904223u;
            const double noise = (double(seed >> 8) / double(1u << 24)) * 2 - 1;
            v += 0.15 * noise * std::exp(-6.9 * double(t) / rate / 0.8);
            wet[size_t(i) * 2] = wet[size_t(i) * 2 + 1] = float(v);
        }
        std::vector<float> out = wet;
        ParamMap values;
        const Params params = def ? resolveParams(def->params, values, 0) : Params();
        int lat = 0;
        if (dry) {
            dry->reset(rate);
            lat = dry->latency(params);
            for (int i = 0; i < len; i += 512)
                dry->process(out.data() + size_t(i) * 2, std::min(512, len - i), params);
        }
        auto energy = [&](const std::vector<float>& x, int shift, int from, int to) {
            double e = 0;
            for (int k = 2; k < len / period; ++k) // (after the first bursts: the processor has settled)
                for (int t = from; t < to; ++t) {
                    const int i = k * period + t + shift;
                    if (i < len)
                        e += double(x[size_t(i) * 2]) * x[size_t(i) * 2];
                }
            return 10 * std::log10(e + 1e-12);
        };
        const double burstDrop = energy(wet, 0, burst / 5, burst) - energy(out, lat, burst / 5, burst);
        const double tailDrop = energy(wet, 0, burst + rate / 5, period) - energy(out, lat, burst + rate / 5, period);
        check(dry && tailDrop > 6 && burstDrop < 3,
              QStringLiteral("De-reverb: a room's tail down %1 dB, the sound itself %2 dB").arg(tailDrop, 0, 'f', 1).arg(burstDrop, 0, 'f', 1));
    }
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
    {
        // The master volume (the line on the Editor's meter) turns the whole mix down, and is saved with the project.
        auto quieter = project.timeline();
        quieter.audioMasterLevel = 0.5f;
        project.applyEdit(quieter, "Master Volume");
        const auto down = finishing::analyze(*project.snapshot());
        const QString path = tmp.filePath("master.mvp");
        Project again;
        QString saveError;
        check(std::abs(down.integrated - (measured.integrated - 6.02)) < .1 && project.save(path, &saveError) && again.load(path, &saveError)
                  && again.timeline().audioMasterLevel == 0.5f,
              QStringLiteral("the master volume turns the mix down (%1 to %2 LUFS) and is saved").arg(measured.integrated, 0, 'f', 2).arg(down.integrated, 0, 'f', 2));
        project.undoStack()->undo();
    }
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
    {
        // Recorded voice-overs: onto a "Voice-over" track above the other audio; an overlapping take gets its own track,
        // a later one goes back on the first.
        Timeline tl = defaultTimeline();
        MediaItem take;
        take.id = 7;
        take.info.duration = 3 * kUsPerSecond;
        take.info.audio.push_back({});
        const FrameRate fps{30, 1};
        const int a = ops::placeVoiceOver(tl, take, 2 * kUsPerSecond, fps), b = ops::placeVoiceOver(tl, take, 4 * kUsPerSecond, fps),
                  c = ops::placeVoiceOver(tl, take, 6 * kUsPerSecond, fps);
        auto on = [&](int clip) { int t = -1; tl.findClip(clip, &t); return t >= 0 ? tl.tracks[size_t(t)].name : QString(); };
        const auto firstAudio = std::find_if(tl.tracks.begin(), tl.tracks.end(), [](const Track& t) { return t.type == TrackType::Audio; });
        MediaItem silent = take;
        silent.info.audio.clear();
        check(a && b && c && on(a) == "Voice-over" && on(b) == "Voice-over 2" && on(c) == "Voice-over" && firstAudio->name.startsWith("Voice-over")
                  && tl.findClip(a)->start == 2 * kUsPerSecond && tl.findClip(a)->length == 3 * kUsPerSecond
                  && !ops::placeVoiceOver(tl, silent, 0, fps) && timelineProblem(tl).isEmpty(),
              QStringLiteral("voice-over takes find room on Voice-over tracks (%1, %2, %3)").arg(on(a), on(b), on(c)));
    }
    {
        // Sync by audio: two recordings of the same bursts (the second 1.234 s later, quieter, with hiss), placed anywhere,
        // end up with their sounds together.
        QTemporaryDir dir;
        auto writeWav = [&](const QString& path, double delay, float gain, float hiss) {
            QFile f(path);
            f.open(QIODevice::WriteOnly);
            QDataStream st(&f);
            st.setByteOrder(QDataStream::LittleEndian);
            st.setFloatingPointPrecision(QDataStream::SinglePrecision);
            const int n = rate * 24;
            f.write("RIFF", 4);
            st << quint32(36 + n * 8);
            f.write("WAVEfmt ", 8);
            st << quint32(16) << quint16(3) << quint16(2) << quint32(rate) << quint32(rate * 8) << quint16(8) << quint16(32);
            f.write("data", 4);
            st << quint32(n * 8);
            uint32_t seed = 99, noise = 7;
            std::vector<double> bursts; // (the same "events" in both: claps and words at irregular times)
            for (double t = 0.4; t < 30; t += 0.35 + double((seed = seed * 1664525u + 1013904223u) >> 8) / double(1u << 24) * 0.9)
                bursts.push_back(t);
            for (int i = 0; i < n; ++i) {
                const double t = double(i) / rate - delay;
                double v = 0;
                for (double b : bursts)
                    if (t >= b && t < b + 0.12)
                        v += std::exp(-(t - b) * 30) * std::sin(2 * pi * (300 + 40 * std::fmod(b * 7, 9)) * t);
                noise = noise * 1664525u + 1013904223u;
                const float x = float(v * 0.5) * gain + hiss * (float(noise >> 8) / float(1u << 24) * 2 - 1);
                st << x << x;
            }
        };
        const QString a = dir.filePath("camera.wav"), b = dir.filePath("recorder.wav");
        writeWav(a, 0.0, 1.0f, 0.0f);
        writeWav(b, 1.234, 0.5f, 0.02f);
        Project p;
        const int ma = p.addMedia(probeMedia(a)), mb = p.addMedia(probeMedia(b));
        Timeline tl = p.timeline();
        const int ta = ops::addTrack(tl, TrackType::Audio, 0), tb = ops::addTrack(tl, TrackType::Audio, 1);
        auto place = [&](int track, int mediaId, TimeUs at) {
            Clip c;
            c.id = tl.allocId();
            c.mediaId = mediaId;
            c.streamIndex = p.mediaItem(mediaId)->info.audio.front().index;
            c.start = at;
            c.length = 20 * kUsPerSecond;
            tl.tracks[size_t(track)].clips.push_back(c);
            return c.id;
        };
        const int ca = place(ta, ma, 10 * kUsPerSecond), cb = place(tb, mb, 3 * kUsPerSecond);
        const audiosync::Result r = audiosync::syncByAudio(tl, p.media(), {ca, cb});
        const TimeUs got = tl.findClip(cb)->start, want = 10 * kUsPerSecond - 1234000;
        check(r.error.isEmpty() && r.unmatched.empty() && tl.findClip(ca)->start == 10 * kUsPerSecond && std::llabs(got - want) <= 2000,
              QStringLiteral("sync by audio lines the second recording up with the first (at %1 s, %2 s expected)")
                  .arg(double(got) / kUsPerSecond, 0, 'f', 4).arg(double(want) / kUsPerSecond, 0, 'f', 4));
        // A recording of something else isn't moved.
        Timeline other = p.timeline();
        const int oa = ops::addTrack(other, TrackType::Audio, 0), ob = ops::addTrack(other, TrackType::Audio, 1);
        Clip x;
        x.id = other.allocId();
        x.mediaId = ma;
        x.streamIndex = p.mediaItem(ma)->info.audio.front().index;
        x.length = 20 * kUsPerSecond;
        other.tracks[size_t(oa)].clips.push_back(x);
        const int tone = p.addMedia(probeMedia(wav)); // (the steady 1 kHz tone from the start of this test)
        Clip y = x;
        y.id = other.allocId();
        y.mediaId = tone;
        y.streamIndex = p.mediaItem(tone)->info.audio.front().index;
        y.length = 8 * kUsPerSecond;
        y.start = 5 * kUsPerSecond;
        other.tracks[size_t(ob)].clips.push_back(y);
        const audiosync::Result none = audiosync::syncByAudio(other, p.media(), {x.id, y.id});
        check(none.moved.empty() && none.unmatched.size() == 1 && other.findClip(y.id)->start == 5 * kUsPerSecond,
              "sync by audio leaves a recording of something else where it was");
    }
    {
        // Multicam: two angles; cutting while playing switches from there on, switching when stopped changes the stretch.
        MediaMap media;
        for (int id : {1, 2}) {
            auto m = std::make_shared<MediaItem>();
            m->id = id;
            m->info.duration = 60 * kUsPerSecond;
            m->info.video.push_back({});
            media[id] = m;
        }
        Timeline tl = defaultTimeline();
        const int t1 = ops::addTrack(tl, TrackType::Video, 0), t2 = ops::addTrack(tl, TrackType::Video, 1);
        auto angle = [&](int track, int mediaId, TimeUs at, TimeUs in) {
            Clip c;
            c.id = tl.allocId();
            c.mediaId = mediaId;
            c.start = at;
            c.sourceIn = in;
            c.length = 30 * kUsPerSecond;
            c.effects.push_back(makeEffectOfType(QStringLiteral("invert"), tl.allocId())); // (each piece's copy needs its own id)
            tl.tracks[size_t(track)].clips.push_back(c);
            return c.id;
        };
        const int a1 = angle(t1, 1, 0, 0), a2 = angle(t2, 2, 2 * kUsPerSecond, 5 * kUsPerSecond);
        QString error;
        const bool made = multicam::create(tl, media, {a1, a2}, &error);
        const int mi = multicam::trackIndex(tl);
        const bool hidden = made && std::all_of(tl.tracks.begin(), tl.tracks.end(), [&](const Track& t) {
            return t.clips.empty() || &t == &tl.tracks[size_t(mi)] || t.muted;
        });
        const bool cut = multicam::cut(tl, media, 2, 10 * kUsPerSecond, &error);
        const auto& cuts = tl.tracks[size_t(mi)].clips;
        const bool pieces = cuts.size() == 2 && multicam::angleOf(cuts[0]) == 1 && cuts[0].end() == 10 * kUsPerSecond
                            && multicam::angleOf(cuts[1]) == 2 && cuts[1].sourceIn == 13 * kUsPerSecond && cuts[1].end() == 30 * kUsPerSecond;
        const bool switched = multicam::switchAngle(tl, media, 2, 5 * kUsPerSecond, &error);
        const auto& after = tl.tracks[size_t(mi)].clips;
        // Angle 2 starts at 2 s: switching the first stretch to it starts it there.
        const bool covered = switched && after.size() == 2 && multicam::angleOf(after[0]) == 2 && after[0].start == 2 * kUsPerSecond
                             && after[0].sourceIn == 5 * kUsPerSecond;
        const bool refused = !multicam::cut(tl, media, 3, 12 * kUsPerSecond, &error) && !multicam::create(tl, media, {a1}, &error);
        check(made && mi == 0 && hidden && cut && pieces && covered && refused && multicam::angles(tl, media).size() == 2 && timelineProblem(tl).isEmpty(),
              QStringLiteral("multicam: two angles under a Multicam track; a cut at 10 s, a switch, no angle 3 (%1)").arg(error));
        Project saved;
        saved.applyEdit(tl, "multicam");
        QTemporaryDir out;
        Project back;
        check(saved.save(out.filePath("mc.mvp"), &error) && back.load(out.filePath("mc.mvp"), &error) && back.timeline().multicam == tl.multicam,
              "the multicam is saved with the project");
    }
    {
        // Paste Event Attributes and Replace Media: event b gets a's effects (new ids), Pan/Crop, fade, level and label;
        // its grouped sound is left (a picture's attributes); then b shows a 10 s file: its sound follows, the trim that
        // went past the new file's end starts again from 0 and its length fits the file.
        MediaMap media;
        for (int id : {1, 2}) {
            auto m = std::make_shared<MediaItem>();
            m->id = id;
            m->info.duration = (id == 1 ? 60 : 10) * kUsPerSecond;
            m->info.video.push_back({});
            m->info.video.back().index = 0;
            m->info.audio.push_back({});
            m->info.audio.back().index = 1;
            media[id] = m;
        }
        Timeline tl = defaultTimeline();
        const int vt = ops::addTrack(tl, TrackType::Video, 0), at = ops::addTrack(tl, TrackType::Audio, -1);
        auto event = [&](int track, int stream, TimeUs start, TimeUs in) {
            Clip c;
            c.id = tl.allocId();
            c.mediaId = 1;
            c.streamIndex = stream;
            c.start = start;
            c.sourceIn = in;
            c.length = 8 * kUsPerSecond;
            tl.tracks[size_t(track)].clips.push_back(c);
            return c.id;
        };
        const int a = event(vt, 0, 0, 0), b = event(vt, 0, 10 * kUsPerSecond, 20 * kUsPerSecond), c = event(at, 1, 10 * kUsPerSecond, 20 * kUsPerSecond);
        tl.findClip(b)->groupId = tl.findClip(c)->groupId = tl.allocId();
        Clip* ca = tl.findClip(a);
        ca->effects.push_back(makeEffectOfType(QStringLiteral("invert"), tl.allocId()));
        ca->panCrop[QStringLiteral("zoom")].value = 1.5;
        ca->fadeIn = kUsPerSecond;
        ca->gain = 0.5f;
        ca->label = 3;
        const FrameRate fps{30, 1};
        const Clip from = *ca;
        const int pasted = ops::pasteAttributes(tl, media, from, true, {b, c}, ops::AttrAll, fps);
        const Clip& pb = *tl.findClip(b);
        const bool attributes = pasted == 1 && pb.effects.size() == 1 && pb.effects[0].type == QStringLiteral("invert")
                                && pb.effects[0].id != from.effects[0].id && pb.panCrop == from.panCrop && pb.fadeIn == kUsPerSecond
                                && pb.gain == 0.5f && pb.label == 3 && tl.findClip(c)->effects.empty() && tl.findClip(c)->gain == 1.0f;
        check(attributes, "Paste Event Attributes: effects (new ids), Pan/Crop, fade, level and label onto a picture, not its sound");
        QString why;
        const int replaced = ops::replaceMedia(tl, media, {b}, 2, fps, &why);
        const Clip& rb = *tl.findClip(b);
        const Clip& rc = *tl.findClip(c);
        check(replaced == 2 && rb.mediaId == 2 && rc.mediaId == 2 && rb.sourceIn == 0 && rb.length <= 10 * kUsPerSecond && rb.effects.size() == 1
                  && rb.start == 10 * kUsPerSecond && rc.streamIndex == 1 && timelineProblem(tl).isEmpty(),
              QStringLiteral("Replace Media: the event and its grouped sound show the new file, keeping place and effects (%1)").arg(why));
    }
    {
        // Separate Stems' two ends: the event's sound as the engine gets it (its span of the file, 44.1 kHz), and the
        // stems back on tracks right under it, grouped, the event muted.
        Project p;
        const int tone = p.addMedia(probeMedia(wav));
        Timeline tl = p.timeline();
        const int at = ops::addTrack(tl, TrackType::Audio);
        Clip c;
        c.id = tl.allocId();
        c.mediaId = tone;
        c.streamIndex = p.mediaItem(tone)->info.audio.front().index;
        c.start = 3 * kUsPerSecond;
        c.sourceIn = kUsPerSecond;
        c.length = 2 * kUsPerSecond;
        tl.tracks[size_t(at)].clips.push_back(c);
        p.applyEdit(tl, "event");
        QString why;
        const QString cut = tmp.filePath("event.wav");
        const bool written = stems::writeEventWav(*p.snapshot(), c, cut, &why);
        const MediaInfo info = written ? probeMedia(cut) : MediaInfo();
        check(written && info.ok() && info.audio.front().sampleRate == 44100 && std::abs(double(info.duration) - 2e6) < 30000,
              QStringLiteral("Separate Stems hands the engine the event's 2 s of sound at 44.1 kHz (%1)").arg(why));
        Timeline st = p.timeline();
        const int a = p.addMedia(info), b = p.addMedia(info);
        const std::vector<int> made = stems::place(st, p.media(), c.id, {a, b}, {"voice", "background"}, &why);
        int ti = -1;
        st.findClip(c.id, &ti);
        int t0 = -1, t1 = -1;
        const Clip* s0 = made.size() == 2 ? st.findClip(made[0], &t0) : nullptr;
        const Clip* s1 = made.size() == 2 ? st.findClip(made[1], &t1) : nullptr;
        check(s0 && s1 && t0 == ti + 1 && t1 == ti + 2 && s0->start == c.start && s1->length == c.length && s0->groupId && s0->groupId == s1->groupId
                  && st.findClip(c.id)->muted && st.tracks[size_t(t0)].name.endsWith(QLatin1String("voice")) && timelineProblem(st).isEmpty(),
              QStringLiteral("the stems go on tracks under the event, where it is, grouped, the event muted (%1)").arg(why));
    }
    {
        // Stems: a track's own file has only its sound (a 1 kHz tone and a 440 Hz one on two tracks).
        QTemporaryDir dir;
        const QString low = dir.filePath("low.wav");
        {
            QFile f(low);
            f.open(QIODevice::WriteOnly);
            QDataStream st(&f);
            st.setByteOrder(QDataStream::LittleEndian);
            st.setFloatingPointPrecision(QDataStream::SinglePrecision);
            f.write("RIFF", 4);
            st << quint32(36 + frames * 8);
            f.write("WAVEfmt ", 8);
            st << quint32(16) << quint16(3) << quint16(2) << quint32(rate) << quint32(rate * 8) << quint16(8) << quint16(32);
            f.write("data", 4);
            st << quint32(frames * 8);
            for (int i = 0; i < frames; ++i) {
                const float x = float(0.1 * std::sin(2 * pi * 440 * i / rate));
                st << x << x;
            }
        }
        Project p;
        const int hi = p.addMedia(probeMedia(wav)), lo = p.addMedia(probeMedia(low));
        Timeline tl = p.timeline();
        tl.tracks.clear();
        int trackIds[2] = {0, 0};
        for (int k = 0; k < 2; ++k) {
            const int ti = ops::addTrack(tl, TrackType::Audio);
            trackIds[k] = tl.tracks[size_t(ti)].id;
            Clip c;
            c.id = tl.allocId();
            c.mediaId = k == 0 ? hi : lo;
            c.streamIndex = p.mediaItem(c.mediaId)->info.audio.front().index;
            c.length = 2 * kUsPerSecond;
            tl.tracks[size_t(ti)].clips.push_back(c);
        }
        p.applyEdit(tl, "stems");
        ExportSettings s;
        s.path = dir.filePath("stem.wav");
        s.muxer = "wav";
        s.audioEncoder = "pcm_s24le";
        s.rangeEnd = 2 * kUsPerSecond;
        s.onlyAudioTrack = trackIds[1];
        Exporter ex;
        const bool ok = ex.run(*p.snapshot(), s);
        // How much of each tone the stem has (a Goertzel filter at each frequency over a second).
        AudioDecoder dec;
        std::vector<float> got(size_t(rate) * 2);
        const bool read = ok && dec.open(s.path, probeMedia(s.path).audio.front().index, rate);
        if (read)
            dec.read(rate / 2, rate, got.data());
        auto power = [&](double hz) {
            double s0 = 0, s1 = 0, s2 = 0;
            const double k = 2 * std::cos(2 * pi * hz / rate);
            for (int i = 0; i < rate; ++i) {
                s0 = got[size_t(i) * 2] + k * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            return s1 * s1 + s2 * s2 - k * s1 * s2;
        };
        const double p440 = power(440), p1000 = power(1000);
        check(read && p440 > 1000 * std::max(1e-9, p1000), QStringLiteral("a stem has only its track's sound (440 Hz %1x the other track's 1 kHz)")
                                                             .arg(p440 / std::max(1e-9, p1000), 0, 'g', 3));
    }
    {
        // Smart render: a file cut at 0.5-2 s and 2-3.5 s (2 s a keyframe: one a second) is copied, not encoded again: the
        // pictures are the file's own, the first cut starting mid-GOP (the MP4's edit list hides the frames before it).
        // A cut off a keyframe renders as usual and says why.
        QTemporaryDir sdir;
        Project gen;
        Timeline gt = gen.timeline();
        const int clock = ops::insertGenerated(gt, *findGeneratorPreset("timecode"), 0, -1, gen.settings().fps);
        gt.findClip(clock)->length = 4 * kUsPerSecond;
        gen.applyEdit(gt, "clock");
        ExportSettings make;
        make.path = sdir.filePath("source.mp4");
        make.videoEncoder = "libx264";
        make.audioEncoder = "aac";
        make.width = 640;
        make.height = 360;
        make.fps = gen.settings().fps;
        make.gopSeconds = 1;
        make.rangeEnd = 4 * kUsPerSecond;
        Exporter maker;
        const bool made = maker.run(*gen.snapshot(), make);
        auto frameOf = [&](const QString& path, TimeUs t) {
            Project q;
            const int id = q.addMedia(probeMedia(path));
            Timeline qt = q.timeline();
            ops::insertMedia(qt, *q.mediaItem(id), 0, -1, q.settings().fps);
            q.applyEdit(qt, "frame");
            Compositor cpu(true);
            return cpu.render(*q.snapshot(), t, QSize(320, 180));
        };
        auto differ = [](const QImage& a, const QImage& b) {
            if (a.size() != b.size() || a.isNull())
                return 999.0;
            double sum = 0;
            for (int y = 0; y < a.height(); y += 3)
                for (int x = 0; x < a.width(); x += 3)
                    sum += std::abs(qGray(a.pixel(x, y)) - qGray(b.pixel(x, y)));
            return sum / ((a.height() / 3 + 1) * (a.width() / 3 + 1));
        };
        auto cutAndRender = [&](TimeUs secondIn, QString* note) {
            Project p;
            const int id = p.addMedia(probeMedia(make.path));
            Timeline tl = p.timeline();
            int vt = -1;
            for (size_t i = 0; i < tl.tracks.size(); ++i)
                if (tl.tracks[i].type == TrackType::Video && vt < 0)
                    vt = int(i);
            for (const auto& [at, in] : {std::pair<TimeUs, TimeUs>{0, kUsPerSecond / 2}, {3 * kUsPerSecond / 2, secondIn}}) {
                Clip c;
                c.id = tl.allocId();
                c.mediaId = id;
                c.streamIndex = p.mediaItem(id)->info.video.front().index;
                c.start = at;
                c.sourceIn = in;
                c.length = 3 * kUsPerSecond / 2;
                tl.tracks[size_t(vt)].clips.push_back(c);
            }
            p.applyEdit(tl, "cuts");
            ExportSettings s = make;
            s.path = sdir.filePath(QStringLiteral("smart%1.mp4").arg(secondIn));
            s.rangeEnd = 3 * kUsPerSecond;
            s.smartRender = true;
            Exporter ex;
            const bool ok = ex.run(*p.snapshot(), s);
            *note = ex.smartRenderNote();
            return ok ? s.path : QString();
        };
        QString copiedNote, renderedNote;
        const QString copied = made ? cutAndRender(2 * kUsPerSecond, &copiedNote) : QString();
        const QString rendered = made ? cutAndRender(5 * kUsPerSecond / 2, &renderedNote) : QString();
        const MediaInfo out = copied.isEmpty() ? MediaInfo() : probeMedia(copied);
        const double first = copied.isEmpty() ? 999 : differ(frameOf(copied, kUsPerSecond / 10), frameOf(make.path, kUsPerSecond * 6 / 10));
        const double second = copied.isEmpty() ? 999 : differ(frameOf(copied, 2 * kUsPerSecond), frameOf(make.path, 5 * kUsPerSecond / 2));
        check(made && copiedNote.contains(QLatin1String("copied")) && out.ok() && std::abs(double(out.duration) / kUsPerSecond - 3.0) < 0.15
                  && first < 1.0 && second < 1.0,
              QStringLiteral("smart render: straight cuts of an H.264 file copied (%1; %2 s long; the first cut's picture %3 off, the "
                             "second's %4 off)")
                  .arg(copiedNote).arg(double(out.duration) / kUsPerSecond, 0, 'f', 2).arg(first, 0, 'f', 2).arg(second, 0, 'f', 2));
        check(!rendered.isEmpty() && renderedNote.contains(QLatin1String("keyframe")),
              QStringLiteral("smart render: a cut off a keyframe renders as usual and says so (%1)").arg(renderedNote));
    }
    {
        // Match Colour's curves: a picture with every level, against a copy with other levels in each channel, give
        // the copy's levels.
        QImage src(256, 32, QImage::Format_ARGB32), ref(256, 32, QImage::Format_ARGB32);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 256; ++x) {
                src.setPixel(x, y, qRgb(x, x, x));
                ref.setPixel(x, y, qRgb(int(51 + 0.6 * x), x * x / 255, int(0.9 * x)));
            }
        colormatch::Histogram hs, hr;
        hs.add(src);
        hr.add(ref);
        const colormatch::Curves cv = colormatch::curves(hs, hr);
        double worst = 0;
        for (int k = 1; k < colormatch::kPoints - 1; ++k) {
            const double x = double(k) / (colormatch::kPoints - 1);
            const double want[3] = {0.2 + 0.6 * x, x * x, 0.9 * x};
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::abs(cv[size_t(c)][size_t(k)] - want[c]));
        }
        check(worst < 0.02, QStringLiteral("Match Colour's curves take a picture's levels to the reference's (worst %1 off)").arg(worst, 0, 'f', 3));
    }
    {
        // Noise reduction: the grain on a flat picture (different in each of three frames) mostly goes; an edge in
        // it stays sharp.
        auto noisy = [](unsigned seed) {
            QImage img(320, 180, QImage::Format_ARGB32_Premultiplied);
            std::mt19937 rng(seed);
            std::normal_distribution<double> grain(0, 12);
            for (int y = 0; y < 180; ++y)
                for (int x = 0; x < 320; ++x) {
                    const int v = std::clamp(int((x < 160 ? 60 : 190) + grain(rng)), 0, 255);
                    img.setPixel(x, y, qRgb(v, v, v));
                }
            return img;
        };
        const QImage a = noisy(1), b = noisy(2), c = noisy(3);
        const flow::Field toA = flow::estimate(b, a), toC = flow::estimate(b, c);
        const QImage out = flow::denoise(b, &a, &toA, &c, &toC, 0.65);
        auto spread = [](const QImage& img) { // (in the flat left part)
            double s = 0, s2 = 0, n = 0;
            for (int y = 10; y < 170; ++y)
                for (int x = 10; x < 140; ++x) {
                    const double v = qRed(img.pixel(x, y));
                    s += v;
                    s2 += v * v;
                    ++n;
                }
            return std::sqrt(std::max(0.0, s2 / n - (s / n) * (s / n)));
        };
        double dark = 0, bright = 0;
        for (int y = 10; y < 170; ++y) {
            dark += qRed(out.pixel(158, y));
            bright += qRed(out.pixel(161, y));
        }
        const double edge = (bright - dark) / 160, before = spread(b), after = spread(out);
        check(after < before * 0.45 && edge > 110, QStringLiteral("noise reduction: grain %1 -> %2 levels, the edge kept (%3 levels across it)")
                                                        .arg(before, 0, 'f', 1)
                                                        .arg(after, 0, 'f', 1)
                                                        .arg(edge, 0, 'f', 0));
    }
    {
        // Optical flow: a square that moved 24 px right and 12 down is, halfway, one square halfway along (frame
        // blending would show two faint ones).
        auto frame = [](int x, int y) {
            QImage img(640, 360, QImage::Format_ARGB32_Premultiplied);
            img.fill(QColor(30, 40, 60));
            QPainter p(&img);
            for (int i = 0; i < 640; i += 40) // (a textured background, so the still parts have something to hold on to)
                for (int j = 0; j < 360; j += 40)
                    p.fillRect(i + (j / 40 % 2) * 20, j, 20, 20, QColor(40, 55, 80));
            p.fillRect(x, y, 80, 80, QColor(250, 220, 40));
            p.fillRect(x + 20, y + 20, 20, 20, QColor(200, 30, 30));
            return img;
        };
        const QImage a = frame(200, 120), b = frame(224, 132);
        const flow::Field f = flow::estimate(a, b);
        const QImage mid = flow::interpolate(a, b, f, 0.5f);
        auto yellow = [](QRgb c) { return qRed(c) > 200 && qGreen(c) > 170 && qBlue(c) < 120; };
        int fx = 0, fy = 0;
        if (!f.empty()) { // the motion at the square's middle, in pixels
            const int cx = int((240.0 + 12) / 640 * f.w), cy = int((160.0 + 6) / 360 * f.h);
            fx = int(std::lround(f.dx[size_t(cy) * size_t(f.w) + size_t(cx)] * 640));
            fy = int(std::lround(f.dy[size_t(cy) * size_t(f.w) + size_t(cx)] * 360));
        }
        // Halfway, the square spans 212..292: x 286 (only the second frame's square there) is yellow, x 206 (only the
        // first's) is background. Blending the two frames would make both half yellow.
        const bool inside = yellow(mid.pixel(286, 166));
        const bool outside = qRed(mid.pixel(206, 166)) < 90;
        check(!f.empty() && std::abs(fx - 24) <= 3 && std::abs(fy - 12) <= 3 && inside && outside,
              QStringLiteral("optical flow follows a square (%1, %2 px; 24, 12 moved) and the halfway frame has it halfway along, no ghost behind")
                  .arg(fx).arg(fy));
    }
    return failures;
}
