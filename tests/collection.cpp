#include "core/Project.h"
#include "core/ProjectCollection.h"
#include "core/TimelineOps.h"
#include "core/Compositor.h"
#include "core/AudioMixer.h"
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QDataStream>
#include <QCryptographicHash>
#include <QTextStream>
#include <QDirIterator>
#include <QSet>
#include <cmath>

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    int failures = 0;
    auto check = [&](bool ok, const QString& label) {
        QTextStream(stdout) << (ok ? "PASS " : "FAIL ") << label << Qt::endl;
        failures += !ok;
    };
    QTemporaryDir temp;
    check(temp.isValid(), "isolated collection fixture directory");
    if (!temp.isValid())
        return 1;
    const QString sources = temp.path() + "/sources";
    QDir().mkpath(sources + "/one");
    QDir().mkpath(sources + "/two");
    auto bytes = [&](const QString& path, QByteArray data) {
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
    };
    auto image = [&](const QString& path, QColor colour) {
        QImage im(64, 36, QImage::Format_ARGB32);
        im.fill(colour);
        return im.save(path);
    };
    const QString a = sources + "/one/same.png", b = sources + "/two/same.png", c = sources + "/keyed.png",
                  unused = sources + "/unused.png", wave = sources + "/speech-00000.wav", lut = sources + "/grade.cube",
                  model = sources + "/voice.pth", index = sources + "/voice.index";
    check(image(a, Qt::red) && image(b, Qt::blue) && image(c, Qt::green) && image(unused, Qt::yellow), "duplicate-basename image fixtures");
    QByteArray audio;
    QDataStream wav(&audio, QIODevice::WriteOnly);
    wav.setByteOrder(QDataStream::LittleEndian);
    wav.writeRawData("RIFF", 4);
    wav << quint32(36 + 48000 * 2);
    wav.writeRawData("WAVEfmt ", 8);
    wav << quint32(16) << quint16(1) << quint16(1) << quint32(48000) << quint32(96000) << quint16(2) << quint16(16);
    wav.writeRawData("data", 4);
    wav << quint32(96000);
    for (int i = 0; i < 48000; ++i)
        wav << qint16(12000 * std::sin(i * .08));
    check(bytes(wave, audio) && bytes(lut, "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n")
              && bytes(model, "test weights") && bytes(index, "test index"),
          "audio, LUT and model fixtures");
    Project project;
    ProjectSettings settings;
    settings.width = 64;
    settings.height = 36;
    project.clear(settings);
    int aid = project.addMedia(probeMedia(a)), bid = project.addMedia(probeMedia(b)), wid = project.addMedia(probeMedia(wave));
    project.addMedia(probeMedia(unused));
    MediaInfo offline;
    offline.path = sources + "/missing.wav";
    project.addMedia(offline);
    auto tl = project.timeline();
    auto ids = ops::insertMedia(tl, *project.mediaItem(aid), 0, -1, settings.fps, true);
    Timeline child = defaultTimeline();
    ops::insertMedia(child, *project.mediaItem(bid), 0, -1, settings.fps, true);
    Clip nested;
    nested.id = tl.allocId();
    nested.length = kUsPerSecond;
    nested.sequence.value = std::make_shared<const Timeline>(child);
    tl.tracks[0].clips.push_back(nested);
    auto lutFx = makeEffectOfType("lut_cube", tl.allocId());
    lutFx.params["file"].text = lut;
    tl.outputEffects.push_back(lutFx);
    auto plugin = makeEffectOfType("a_vst3", tl.allocId());
    plugin.params["plugin"].text = sources + "/External.vst3";
    plugin.enabled = false;
    tl.audioMasterEffects.push_back(plugin);
    auto sound = ops::insertMedia(tl, *project.mediaItem(wid), 0, -1, settings.fps, true);
    QJsonObject rvc{{"engine", "bundled"}, {"model", model}, {"index", index}};
    QJsonObject take{{"media", wid}, {"path", wave}, {"rvc", rvc}};
    for (int id : sound)
        if (auto* clip = tl.findClip(id))
            clip->narration = QJsonObject{{"rvc", rvc}, {"takes", QJsonArray{take}}};
    int overlay = ops::insertGenerated(tl, *findGeneratorPreset("solid_white"), 0, -1, {});
    auto* graphic = tl.findClip(overlay);
    graphic->generator.type = "graphics_overlay";
    graphic->generator.data = QJsonObject{
        {"version", 2},
        {"variables", QJsonObject{{"picture", a}}},
        {"layers",
         QJsonArray{QJsonObject{{"id", "image"},
                                {"type", "image"},
                                {"path", QJsonObject{{"keyframes", QJsonArray{QJsonArray{0, a}, QJsonArray{1, c}}}}},
                                {"follow", QJsonObject{{"path", "road"}}}},
                    QJsonObject{{"id", "text"}, {"type", "text"}, {"text", "Do not rewrite " + a}, {"font", "Arial"}},
                    QJsonObject{{"id", "var"}, {"type", "image"}, {"path", QJsonObject{{"variable", "picture"}}}},
                    QJsonObject{{"id", "road"}, {"type", "path"}, {"commands", QJsonArray{QJsonArray{"M", 0, 0}, QJsonArray{"L", 1, 1}}}},
                    QJsonObject{{"id", "instance"}, {"type", "instance"}, {"symbol", "badge"}, {"variables", QJsonObject{{"brand", c}}}}}},
        {"symbols",
         QJsonObject{
             {"badge",
              QJsonObject{
                  {"variables", QJsonObject{{"brand", b}}},
                  {"layers", QJsonArray{QJsonObject{{"id", "logo"}, {"type", "image"}, {"path", QJsonObject{{"variable", "brand"}}}}}}}}}}};
    LiveOverlay live;
    live.image = c;
    tl.liveOverlays.push_back(live);
    tl.liveScenes[0].used = true;
    tl.liveScenes[0].overlays.push_back(live);
    int waveform = ops::insertGenerated(tl, *findGeneratorPreset("solid_white"), 0, -1, {});
    tl.findClip(waveform)->generator.type = "audio_waveform";
    tl.findClip(waveform)->generator.data = QJsonObject{{"source_path", wave}};
    project.applyEdit(tl, "unsaved fixture");
    QString error;
    const auto before = project.snapshot();
    const auto oldPath = project.filePath();
    int undo = project.undoStack()->count();
    bool dirty = project.isModified();
    QImage expectedPicture;
    {
        Compositor originalRenderer;
        expectedPicture = originalRenderer.render(*before, 250000, {64, 36});
    }
    auto document = project.collectionDocument();
    check(document["format"] == "mutant-video-producer-project" && document["version"] == 1,
          "collection snapshot uses the new explicit project header");
    const QString refused = temp.path() + "/refused";
    auto obsolete = document;
    obsolete["format"] = "montage-project";
    check(!collection::collect(obsolete, sources, refused, "obsolete", {}).ok && !QFileInfo::exists(refused),
          "collection rejects the retired project identifier without creating a folder");
    auto unversioned = document;
    unversioned.remove("version");
    check(!collection::collect(unversioned, sources, refused, "unversioned", {}).ok && !QFileInfo::exists(refused),
          "collection rejects a missing project version without creating a folder");
    QJsonObject unrelated{
        {"media", QJsonArray{1, 2, 3}}, {"rvc", QJsonValue::Null}, {"params", 42}, {"follow", QJsonObject{{"path", "layer-id"}}}};
    const auto unchanged = unrelated;
    collection::rewriteFiles(unrelated, [](const QString&, collection::Role) { return QString("rewritten"); });
    check(unrelated == unchanged, "file traversal preserves unrelated numeric arrays, null values and layer IDs");
    QJsonObject defaultGraphic{{"type", "graphics_overlay"}, {"data", QJsonObject{}}};
    const auto defaultBefore = defaultGraphic;
    collection::rewriteFiles(defaultGraphic, [](const QString& path, collection::Role) { return path; });
    check(defaultGraphic == defaultBefore, "default graphics presets are not replaced with an empty scene");
    {
        const QString cwd = QDir::currentPath();
        QDir::setCurrent(sources);
        Project relative;
        auto edit = defaultTimeline();
        int id = ops::insertGenerated(edit, *findGeneratorPreset("solid_white"), 0, -1, {});
        edit.findClip(id)->generator.type = "graphics_overlay";
        edit.findClip(id)->generator.data = QJsonObject{{"layers", QJsonArray{QJsonObject{{"type", "image"}, {"path", "one/same.png"}}}}};
        relative.applyEdit(edit, "relative graphic");
        relative.save(temp.path() + "/Outside.mvp", nullptr);
        auto captured = relative.collectionDocument();
        QDir::setCurrent(cwd);
        QString capturedPath;
        collection::rewriteFiles(captured, [&](const QString& path, collection::Role role) {
            if (role == collection::Role::Asset)
                capturedPath = path;
            return path;
        });
        check(capturedPath == a, "runtime-relative assets are captured independently of the project save folder");
    }
    const auto full = collection::collect(document, sources, temp.path() + "/full", "CON.mvp");
    check(full.ok && full.copiedFiles == 8 && full.missing.size() == 1,
          "collect bins, nested media, takes, keyed graphics, LUT and RVC assets once");
    check(project.snapshot() == before && project.filePath() == oldPath && project.isModified() == dirty
              && project.undoStack()->count() == undo,
          "collection preserves unsaved edit, session, path and undo history");
    check(QFileInfo(full.projectPath).fileName() == "_CON.mvp", "portable project name avoids reserved Windows device names");
    QFile reportFile(full.reportPath);
    reportFile.open(QIODevice::ReadOnly);
    auto report = QJsonDocument::fromJson(reportFile.readAll()).object();
    reportFile.close();
    check(report["plugins"].toArray().size() == 1 && !report["fonts"].toArray().isEmpty(), "report identifies external plugins and fonts");
    bool hashes = true, relative = true;
    QSet<QString> names;
    for (auto v : report["files"].toArray()) {
        auto item = v.toObject();
        QString path = item["collected"].toString();
        relative &= QDir::isRelativePath(path);
        if (item["status"] == "copied") {
            QFile f(temp.path() + "/full/" + path);
            hashes &= f.open(QIODevice::ReadOnly)
                && QString::fromLatin1(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex())
                    == item["sha256"].toString();
            names.insert(path);
        }
    }
    check(hashes && relative && names.size() == 8, "report hashes match copied bytes and duplicate basenames stay separate");
    auto reduced = collection::collect(document, sources, temp.path() + "/reduced", "Edit.mvp", {false, false});
    check(reduced.ok && reduced.missing.isEmpty() && reduced.copiedFiles == 5,
          "used-only option retains nested clips and alternate takes, excluding unused bins and models");
    QFile reducedReport(reduced.reportPath);
    reducedReport.open(QIODevice::ReadOnly);
    check(QJsonDocument::fromJson(reducedReport.readAll()).object()["excludedVoiceModels"].toArray().size() == 2,
          "excluded regeneration models are reported");
    auto existing = collection::collect(document, sources, temp.path() + "/full", "Other.mvp");
    check(!existing.ok && !existing.error.isEmpty() && QFileInfo::exists(full.projectPath), "existing collection never overwritten");
    auto cancelled = collection::collect(document, sources, temp.path() + "/cancelled", "Edit.mvp", {},
                                         [](qint64 done, qint64, const QString&) { return done == 0; });
    check(cancelled.cancelled && !QFileInfo::exists(temp.path() + "/cancelled"), "mid-copy cancellation publishes no partial collection");
    check(QDir(temp.path()).entryList({".mvp-collect-*"}, QDir::Dirs | QDir::Hidden).isEmpty(),
          "cancelled collection staging files cleaned up");
    auto changed =
        collection::collect(document, sources, temp.path() + "/changed", "Edit.mvp", {}, [&](qint64 done, qint64, const QString& current) {
            if (done > 0 && !current.isEmpty())
                bytes(current, "changed during copy");
            return true;
        });
    check(!changed.ok && !changed.error.isEmpty() && !QFileInfo::exists(temp.path() + "/changed"),
          "changing source detected without publishing a damaged collection");
    // Move the collection and hide all original source paths; loading must use the new folder.
    check(QDir().rename(sources, sources + "-hidden") && QDir().rename(temp.path() + "/full", temp.path() + "/moved"),
          "fixture moved independently of originals");
    Project moved;
    QStringList missing;
    const QString movedPath = temp.path() + "/moved/_CON.mvp";
    const bool loaded = moved.load(movedPath, &error, &missing);
    check(loaded && missing.size() == 1, "relocated project loads and reports only the originally missing media: " + error);
    if (!loaded)
        return failures;
    bool paths = true;
    for (const auto& [id, item] : moved.media())
        paths &= item->path.startsWith(temp.path() + "/moved/");
    check(paths, "relocated media resolves against project folder, not working directory");
    auto movedGraphic = moved.timeline().findClip(overlay)->generator.data;
    auto layers = movedGraphic["layers"].toArray();
    check(
        layers[0].toObject()["path"].toObject()["keyframes"].toArray()[0].toArray()[1].toString().startsWith(temp.path() + "/moved/Assets/")
            || layers[0].toObject()["path"].toObject()["keyframes"].toArray()[0].toArray()[1].toString().startsWith(temp.path()
                                                                                                                    + "/moved/Media/"),
        "animated image paths remap after relocation");
    check(layers[1].toObject()["text"] == "Do not rewrite " + a && layers[0].toObject()["follow"].toObject()["path"] == "road",
          "captions and path-following layer IDs are not rewritten");
    check(QFileInfo::exists(movedGraphic["variables"].toObject()["picture"].toString())
              && QFileInfo::exists(movedGraphic["symbols"].toObject()["badge"].toObject()["variables"].toObject()["brand"].toString()),
          "variable-bound images and reusable symbol images remain available");
    check(QFileInfo::exists(layers[4].toObject()["variables"].toObject()["brand"].toString()), "reusable symbol image overrides relocate");
    {
        Compositor movedRenderer;
        check(!expectedPicture.isNull() && movedRenderer.render(*moved.snapshot(), 250000, {64, 36}) == expectedPicture,
              "relocated nested media and composed graphics render the same picture");
    }
    check(QFileInfo::exists(moved.timeline().outputEffects[0].params.at("file").text)
              && QFileInfo::exists(moved.timeline().liveScenes[0].overlays[0].image),
          "LUT and saved Live scene image references relocate");
    {
        AudioMixer mixer(48000);
        std::vector<float> samples(2048);
        mixer.mix(*moved.snapshot(), 0, 1024, samples.data());
        double energy = 0;
        for (float sample : samples)
            energy += sample * sample;
        check(energy > .01, "generated narration audio plays with originals unavailable");
    }
    check(moved.save(movedPath, &error), "collected project can be edited and saved");
    QFile saved(movedPath);
    saved.open(QIODevice::ReadOnly);
    auto savedRoot = QJsonDocument::fromJson(saved.readAll()).object();
    check(QDir::isRelativePath(savedRoot["media"].toArray()[0].toObject()["path"].toString()),
          "saving a collected project preserves relative media paths");
    saved.close();
    check(QDir().rename(temp.path() + "/moved", temp.path() + "/moved-again"), "saved collection moved a second time");
    Project again;
    check(again.load(temp.path() + "/moved-again/_CON.mvp", &error) && QFileInfo::exists(again.mediaItem(aid)->path),
          "saved collection remains portable on subsequent moves");
    check(collection::collect(again.collectionDocument(), temp.path() + "/moved-again", temp.path() + "/recollected", "Again.mvp").ok,
          "already portable collection can be collected again");
    {
        // A collection collected again, with a file from elsewhere called b.png met first: its copy would be named
        // Media/0001-b.png, which the project already names for a different file (that must not be taken for the copy).
        const QString re = temp.path() + "/re";
        QDir().mkpath(re + "/Media");
        QDir().mkpath(re + "/elsewhere");
        check(image(re + "/Media/0001-b.png", Qt::red) && image(re + "/elsewhere/b.png", Qt::blue), "renumbered collection fixtures");
        Project numbered;
        numbered.clear(settings);
        const int blue = numbered.addMedia(probeMedia(re + "/elsewhere/b.png")), red = numbered.addMedia(probeMedia(re + "/Media/0001-b.png"));
        auto ntl = numbered.timeline();
        ops::insertMedia(ntl, *numbered.mediaItem(blue), 0, -1, settings.fps, true);
        ops::insertMedia(ntl, *numbered.mediaItem(red), 10 * kUsPerSecond, -1, settings.fps, true);
        numbered.applyEdit(ntl, "test");
        QJsonObject doc = numbered.collectionDocument();
        QJsonArray media = doc["media"].toArray();
        for (int i = 0; i < media.size(); ++i) {
            QJsonObject m = media[i].toObject();
            m["path"] = QDir(re).relativeFilePath(m["path"].toString());
            media[i] = m;
        }
        doc["media"] = media;
        const auto result = collection::collect(doc, re, temp.path() + "/re-collected", "Re.mvp");
        Project out;
        const bool opened = result.ok && out.load(result.projectPath, &error) && out.mediaItem(blue) && out.mediaItem(red);
        check(opened && out.mediaItem(blue)->path != out.mediaItem(red)->path
                  && QImage(out.mediaItem(blue)->path).pixelColor(0, 0) == QColor(Qt::blue)
                  && QImage(out.mediaItem(red)->path).pixelColor(0, 0) == QColor(Qt::red),
              "a collection collected again keeps each file its own copy, whatever order it meets them in");
    }
    return failures;
}
