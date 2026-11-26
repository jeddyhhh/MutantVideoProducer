// The editor test's plugin part (MutantVideoProducer --editor-test): every bundled plugin runs on a small project
// made for it, through the same path as Tools > Plugins (without its form), and what it made is checked; then the
// sandbox: what a script may and may not read, list and write, the watchdog, rollback and the refused commands.
#include "core/Vst3Host.h"
#include "core/Project.h"
#include "ui/MainWindow.h"
#include "ui/McpServer.h"
#include "ui/Plugins.h"
#include "ui/TimelineWidget.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUndoStack>

#include <algorithm>
#include <cmath>

void MainWindow::runPluginsTest(const std::function<void(bool, const QString&)>& check, const QDir& work, const QString& clip)
{
    auto call = [this](const QString& tool, const QJsonObject& args = {}) {
        const QJsonObject response = m_mcp->handle({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                                     {QStringLiteral("id"), 1},
                                                     {QStringLiteral("method"), QStringLiteral("tools/call")},
                                                     {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), tool}, {QStringLiteral("arguments"), args}}}});
        const QJsonObject result = response[QStringLiteral("result")].toObject();
        QString text;
        for (const QJsonValue c : result[QStringLiteral("content")].toArray())
            text += c[QStringLiteral("text")].toString();
        QJsonObject out = QJsonDocument::fromJson(text.toUtf8()).object();
        if (result[QStringLiteral("isError")].toBool())
            out[QStringLiteral("error")] = text;
        return out;
    };
    auto write = [&](const QString& name, const QByteArray& text) {
        const QString path = work.filePath(name);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(text);
        return path;
    };
    const QString media = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("examples/media"));
    const QString music = media + QStringLiteral("/music.m4a"), sample = media + QStringLiteral("/sample.mp4"); // 40 s and 12 s
    auto tracks = [&] { return call(QStringLiteral("get_project"))[QStringLiteral("tracks")].toArray(); };
    auto track = [&](const QString& name) {
        for (const QJsonValue t : tracks())
            if (t[QStringLiteral("name")].toString() == name)
                return t.toObject();
        return QJsonObject();
    };
    auto events = [&](const QString& name) { return track(name)[QStringLiteral("events")].toArray(); };
    // A fresh project: the test clip (4 s) at 0, the 12 s sample at 10 (a gap between), the 40 s song from 0.
    int clipEvent = -1, musicTrack = -1;
    auto fresh = [&] {
        call(QStringLiteral("new_project"), {{QStringLiteral("discard"), true}});
        const QJsonArray ids =
            call(QStringLiteral("import_media"), {{QStringLiteral("paths"), QJsonArray{clip, sample, music}}})[QStringLiteral("media_ids")].toArray();
        clipEvent = call(QStringLiteral("add_media_to_timeline"), {{QStringLiteral("media_id"), ids.at(0)}, {QStringLiteral("at"), 0}})
                        [QStringLiteral("event_ids")].toArray().at(0).toInt(-1);
        call(QStringLiteral("add_media_to_timeline"), {{QStringLiteral("media_id"), ids.at(1)}, {QStringLiteral("at"), 10}});
        const int own = call(QStringLiteral("add_track"), {{QStringLiteral("type"), QStringLiteral("audio")}})[QStringLiteral("index")].toInt();
        const QJsonArray song = call(QStringLiteral("add_media_to_timeline"),
                                     {{QStringLiteral("media_id"), ids.at(2)}, {QStringLiteral("at"), 0}, {QStringLiteral("track"), own}})[QStringLiteral("event_ids")].toArray();
        musicTrack = -1;
        for (const QJsonValue t : tracks())
            for (const QJsonValue e : t[QStringLiteral("events")].toArray())
                if (song.contains(e[QStringLiteral("event_id")]))
                    musicTrack = t[QStringLiteral("index")].toInt();
        m_project->undoStack()->clear();
    };
    auto run = [&](const QString& id, const QJsonObject& args, const plugins::Access& access, QString* log = nullptr) {
        QStringList lines;
        const QString error = runPluginForTests(id, 0, args, access, &lines);
        if (log)
            *log = lines.join(QStringLiteral(" | "));
        return error;
    };
    auto ok = [](const QString& error) { return error.isEmpty() ? QStringLiteral("ok") : error; };

    QStringList problems;
    const auto bundled = plugins::load(&problems);
    check(problems.isEmpty() && bundled.size() >= 17,
          QStringLiteral("the bundled plugins load (%1, problems: %2)").arg(bundled.size()).arg(problems.join(QStringLiteral("; "))));

    {   // Lyric Video, onto the beat of the song.
        fresh();
        const QString lrc = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("plugins/Lyric Video/example.lrc"));
        const QString error = run(QStringLiteral("lyric-video"),
                                  {{QStringLiteral("lrc"), lrc}, {QStringLiteral("style"), QStringLiteral("kinetic_text")}, {QStringLiteral("font"), QString()},
                                   {QStringLiteral("snap"), true}, {QStringLiteral("offset"), 0}},
                                  {{lrc}, {}, {}});
        const int markers = call(QStringLiteral("get_project"))[QStringLiteral("markers")].toArray().size();
        check(error.isEmpty() && events(QStringLiteral("Lyrics")).size() == 9 && markers == 0,
              QStringLiteral("Lyric Video on the beat: 9 lines (%1, %2), and the beat markers taken away again (%3 left)")
                  .arg(events(QStringLiteral("Lyrics")).size()).arg(ok(error)).arg(markers));
    }
    {   // Chapters from a List.
        fresh();
        const QString list = write(QStringLiteral("plugins/chapters.txt"), "0:00 Intro\nThe middle - 0:15\n[0:30] The end\nno time on this line\n");
        QString log;
        const QString error = run(QStringLiteral("chapters-from-list"), {{QStringLiteral("list"), list}, {QStringLiteral("replace"), false}}, {{list}, {}, {}}, &log);
        const QJsonArray markers = call(QStringLiteral("get_project"))[QStringLiteral("markers")].toArray();
        check(error.isEmpty() && markers.size() == 3 && markers.at(1)[QStringLiteral("name")].toString() == QLatin1String("The middle")
                  && markers.at(1)[QStringLiteral("time")].toDouble() == 15,
              QStringLiteral("Chapters from a List: 3 named markers from 4 lines (%1, %2: %3)").arg(markers.size()).arg(ok(error), log));
    }
    {   // Music Video in One Click.
        fresh();
        const int before = int(tracks().size());
        QString log;
        const QString error = run(QStringLiteral("music-video"),
                                  {{QStringLiteral("style"), QStringLiteral("riot")}, {QStringLiteral("every"), QStringLiteral("2")}, {QStringLiteral("from"), 0},
                                   {QStringLiteral("to"), 0}, {QStringLiteral("intensity"), 0.6}, {QStringLiteral("seed"), 3}},
                                  {}, &log);
        const QJsonArray top = tracks().at(0)[QStringLiteral("events")].toArray();
        const int markers = call(QStringLiteral("get_project"))[QStringLiteral("markers")].toArray().size();
        check(error.isEmpty() && int(tracks().size()) == before + 1 && top.size() >= 8 && markers == 0,
              QStringLiteral("Music Video in One Click: a montage track of %1 cuts, no markers left (%2) (%3: %4)")
                  .arg(top.size()).arg(markers).arg(ok(error), log));
        const int steps = m_project->undoStack()->index();
        m_project->undoStack()->undo();
        check(steps == 1 && int(tracks().size()) == before, QStringLiteral("one Ctrl+Z takes the whole music video away (%1 step)").arg(steps));
    }
    {   // Fill the Gaps, on the top video track (the clip at 0-4 and the sample at 10-22, the edit to 40).
        fresh();
        QString log;
        const QString error = run(QStringLiteral("fill-gaps"),
                                  {{QStringLiteral("track"), 1}, {QStringLiteral("shortest"), 0.5}, {QStringLiteral("longest"), 4},
                                   {QStringLiteral("sound"), false}, {QStringLiteral("seed"), 2}},
                                  {}, &log);
        QJsonObject video;
        for (const QJsonValue t : tracks())
            if (t[QStringLiteral("type")].toString() == QLatin1String("video")) {
                video = t.toObject();
                break;
            }
        double at = 0, longestGap = 0;
        std::vector<std::pair<double, double>> spans;
        for (const QJsonValue e : video[QStringLiteral("events")].toArray())
            spans.emplace_back(e[QStringLiteral("start")].toDouble(), e[QStringLiteral("end")].toDouble());
        std::sort(spans.begin(), spans.end());
        bool overlap = false;
        for (const auto& [s, e] : spans) {
            longestGap = std::max(longestGap, s - at);
            overlap |= s < at - 0.01;
            at = std::max(at, e);
        }
        const double end = call(QStringLiteral("get_project"))[QStringLiteral("duration")].toDouble();
        longestGap = std::max(longestGap, end - at);
        check(error.isEmpty() && longestGap < 0.05 && !overlap && spans.size() > 3,
              QStringLiteral("Fill the Gaps: the track is covered to the end (longest gap %1 s, %2 events, overlap %3) (%4: %5)")
                  .arg(longestGap, 0, 'f', 2).arg(spans.size()).arg(overlap ? "yes" : "no").arg(ok(error), log));
    }
    {   // Batch Effects: VHS on the selected event, then every effect off its track.
        fresh();
        m_timeline->selectClips({clipEvent});
        const QString error = run(QStringLiteral("batch-effects"),
                                  {{QStringLiteral("scope"), QStringLiteral("selected")}, {QStringLiteral("track"), 1},
                                   {QStringLiteral("action"), QStringLiteral("effect")}, {QStringLiteral("name"), QStringLiteral("VHS")}},
                                  {});
        int vhs = 0, index = -1;
        for (const QJsonValue t : tracks())
            for (const QJsonValue e : t[QStringLiteral("events")].toArray())
                for (const QJsonValue fx : e[QStringLiteral("effects")].toArray())
                    if (fx[QStringLiteral("type")].toString() == QLatin1String("vhs")) {
                        ++vhs;
                        index = t[QStringLiteral("index")].toInt();
                    }
        const QString strip = run(QStringLiteral("batch-effects"),
                                  {{QStringLiteral("scope"), QStringLiteral("track")}, {QStringLiteral("track"), index + 1},
                                   {QStringLiteral("action"), QStringLiteral("strip")}, {QStringLiteral("name"), QString()}},
                                  {});
        int left = 0;
        for (const QJsonValue e : tracks().at(std::max(0, index))[QStringLiteral("events")].toArray())
            left += e[QStringLiteral("effects")].toArray().size();
        m_timeline->selectClips({});
        const QString none = run(QStringLiteral("batch-effects"),
                                 {{QStringLiteral("scope"), QStringLiteral("selected")}, {QStringLiteral("track"), 1},
                                  {QStringLiteral("action"), QStringLiteral("effect")}, {QStringLiteral("name"), QStringLiteral("VHS")}},
                                 {});
        check(error.isEmpty() && vhs == 1 && strip.isEmpty() && left == 0 && none.contains(QStringLiteral("no events are selected")),
              QStringLiteral("Batch Effects: VHS on the one selected event (%1, %2), every effect off its track (%3 left, %4), and "
                             "nothing selected is refused (%5)")
                  .arg(vhs).arg(ok(error)).arg(left).arg(ok(strip), none));
    }
    {   // Countdown and Timer.
        fresh();
        const QString error = run(QStringLiteral("countdown-timer"),
                                  {{QStringLiteral("mode"), QStringLiteral("countdown")}, {QStringLiteral("at"), 1}, {QStringLiteral("seconds"), 5},
                                   {QStringLiteral("last"), QStringLiteral("GO!")}, {QStringLiteral("style"), QStringLiteral("title")},
                                   {QStringLiteral("color"), QStringLiteral("#ffcc00")}},
                                  {});
        const QJsonArray count = events(QStringLiteral("Countdown"));
        const QString timer = run(QStringLiteral("countdown-timer"),
                                  {{QStringLiteral("mode"), QStringLiteral("timer")}, {QStringLiteral("at"), 0}, {QStringLiteral("seconds"), 65},
                                   {QStringLiteral("last"), QString()}, {QStringLiteral("style"), QStringLiteral("subtitle")},
                                   {QStringLiteral("color"), QStringLiteral("#ffffff")}},
                                  {});
        const QJsonArray clock = events(QStringLiteral("Timer"));
        check(error.isEmpty() && count.size() == 6 && count.at(0)[QStringLiteral("text")].toString() == QLatin1String("5") && timer.isEmpty()
                  && clock.size() == 65 && clock.at(64)[QStringLiteral("text")].toString() == QLatin1String("01:04"),
              QStringLiteral("Countdown and Timer: 5 to 1 and GO! (%1 titles, %2), a clock to 01:04 (%3 titles, %4)")
                  .arg(count.size()).arg(ok(error)).arg(clock.size()).arg(ok(timer)));
    }
    {   // Podcast Clean-up: the clip's sound is the voice, the song the music.
        fresh();
        QString log;
        const QString error = run(QStringLiteral("podcast-cleanup"),
                                  {{QStringLiteral("music"), musicTrack + 1}, {QStringLiteral("duck"), -12}, {QStringLiteral("silences"), false},
                                   {QStringLiteral("pause"), 1.0}, {QStringLiteral("target"), QStringLiteral("-16")}},
                                  {}, &log);
        int gains = 0;
        for (const QJsonValue t : tracks())
            if (t[QStringLiteral("type")].toString() == QLatin1String("audio") && t[QStringLiteral("index")].toInt() != musicTrack)
                for (const QJsonValue e : t[QStringLiteral("events")].toArray())
                    gains += e[QStringLiteral("effects")].toArray().size();
        check(error.isEmpty() && gains >= 1 && log.contains(QStringLiteral("master gain")),
              QStringLiteral("Podcast Clean-up: voices normalised (%1 gain effects), loudness set (%2: %3)").arg(gains).arg(ok(error), log));
    }
    {   // Social Cut-downs: queued (not started), and the edit left as it was.
        fresh();
        const QJsonObject before = call(QStringLiteral("get_project"));
        const QString out = work.filePath(QStringLiteral("plugins/vertical.mp4"));
        const QString error = run(QStringLiteral("social-cutdowns"),
                                  {{QStringLiteral("start"), 0}, {QStringLiteral("seconds"), QStringLiteral("15")}, {QStringLiteral("output"), out},
                                   {QStringLiteral("template"), QStringLiteral("MP4 H.264")}, {QStringLiteral("now"), false}},
                                  {{}, {}, {out}});
        const QJsonObject after = call(QStringLiteral("get_project"));
        bool queued = false;
        for (const QJsonValue j : call(QStringLiteral("render_queue"), {{QStringLiteral("action"), QStringLiteral("status")}})[QStringLiteral("jobs")].toArray())
            queued |= QFileInfo(j[QStringLiteral("path")].toString()) == QFileInfo(out);
        check(error.isEmpty() && queued && after[QStringLiteral("width")] == before[QStringLiteral("width")]
                  && after[QStringLiteral("height")] == before[QStringLiteral("height")] && after[QStringLiteral("tracks")] == before[QStringLiteral("tracks")],
              QStringLiteral("Social Cut-downs: the vertical render is queued (%1) and the edit is as it was, frame %2x%3 (%4)")
                  .arg(queued ? "yes" : "no").arg(after[QStringLiteral("width")].toInt()).arg(after[QStringLiteral("height")].toInt()).arg(ok(error)));
    }
    {   // Brand Kit: the sample as the intro, the clip as the outro, a made-up logo.
        fresh();
        QImage logo(200, 100, QImage::Format_ARGB32);
        logo.fill(QColor(255, 40, 120, 200));
        const QString logoPath = work.filePath(QStringLiteral("plugins/logo.png"));
        logo.save(logoPath);
        const double before = call(QStringLiteral("get_project"))[QStringLiteral("duration")].toDouble();
        const QString error = run(QStringLiteral("brand-kit"),
                                  {{QStringLiteral("logo"), logoPath}, {QStringLiteral("corner"), QStringLiteral("top-right")}, {QStringLiteral("size"), 12},
                                   {QStringLiteral("opacity"), 0.85}, {QStringLiteral("intro"), sample}, {QStringLiteral("outro"), clip}},
                                  {{logoPath, sample, clip}, {}, {}});
        const QJsonObject p = call(QStringLiteral("get_project"));
        const QJsonArray logoEvents = events(QStringLiteral("Logo"));
        const bool transformed = !logoEvents.isEmpty()
                                 && logoEvents.at(0)[QStringLiteral("effects")].toArray().at(0)[QStringLiteral("type")].toString() == QLatin1String("transform");
        check(error.isEmpty() && std::abs(p[QStringLiteral("duration")].toDouble() - (before + 12 + 4)) < 0.1 && logoEvents.size() == 1 && transformed
                  && std::abs(logoEvents.at(0)[QStringLiteral("start")].toDouble() - 12) < 0.05,
              QStringLiteral("Brand Kit: intro and outro make the edit %1 s (from %2), the logo on its own track from the intro's end with a "
                             "Transform (%3) (%4)")
                  .arg(p[QStringLiteral("duration")].toDouble(), 0, 'f', 2).arg(before, 0, 'f', 2).arg(transformed ? "yes" : "no").arg(ok(error)));
    }
    {   // Scoreboard from CSV.
        fresh();
        const QString csv = write(QStringLiteral("plugins/scores.csv"), "time,home,away,note\n0,0,0\n0:02,1,0,\"Goal, a header\"\n5,1,1\n");
        const QString error = run(QStringLiteral("scoreboard-csv"),
                                  {{QStringLiteral("csv"), csv}, {QStringLiteral("home"), QStringLiteral("CITY")}, {QStringLiteral("away"), QStringLiteral("ROVERS")},
                                   {QStringLiteral("position"), QStringLiteral("top")}, {QStringLiteral("color"), QStringLiteral("#ffffff")}},
                                  {{csv}, {}, {}});
        const QJsonArray scores = events(QStringLiteral("Scoreboard"));
        check(error.isEmpty() && scores.size() == 3 && scores.at(1)[QStringLiteral("text")].toString().contains(QStringLiteral("Goal, a header")),
              QStringLiteral("Scoreboard from CSV: 3 scores from a CSV with a header and a quoted comma (%1, %2)").arg(scores.size()).arg(ok(error)));
    }
    {   // Edit List from CSV: the clip copied next to the CSV, named without a folder.
        fresh();
        QFile::remove(work.filePath(QStringLiteral("plugins/shot.mp4")));
        QFile::copy(clip, work.filePath(QStringLiteral("plugins/shot.mp4")));
        const QString csv = write(QStringLiteral("plugins/edit.csv"), "file,in,out,name\nshot.mp4,0.5,2,First\nshot.mp4,0:01,3.5\n");
        const double before = call(QStringLiteral("get_project"))[QStringLiteral("duration")].toDouble();
        const QString error = run(QStringLiteral("edit-list-csv"), {{QStringLiteral("csv"), csv}, {QStringLiteral("gap"), 0}}, {{csv}, {}, {}});
        QStringList placed;
        for (const QJsonValue t : tracks())
            if (t[QStringLiteral("type")].toString() == QLatin1String("video"))
                for (const QJsonValue e : t[QStringLiteral("events")].toArray())
                    if (e[QStringLiteral("start")].toDouble() >= before - 0.01)
                        placed << QStringLiteral("%1@%2+%3").arg(e[QStringLiteral("name")].toString()).arg(e[QStringLiteral("start")].toDouble() - before, 0, 'f', 1)
                                      .arg(e[QStringLiteral("length")].toDouble(), 0, 'f', 1);
        placed.sort();
        check(error.isEmpty() && placed.size() == 2 && placed.contains(QStringLiteral("First@0.0+1.5")),
              QStringLiteral("Edit List from CSV: two shots after the edit, trimmed and named (%1) (%2)").arg(placed.join(QStringLiteral(", ")), ok(error)));
    }
    {   // Restyle Captions, on captions imported from an SRT.
        fresh();
        const QString srt = write(QStringLiteral("plugins/captions.srt"),
                                  "1\n00:00:00,500 --> 00:00:02,000\nHello there\n\n2\n00:00:02,500 --> 00:00:04,000\nSecond line\n");
        call(QStringLiteral("captions"), {{QStringLiteral("action"), QStringLiteral("import")}, {QStringLiteral("path"), srt}});
        QString log;
        const QString error = run(QStringLiteral("restyle-captions"),
                                  {{QStringLiteral("tracks"), QStringLiteral("Caption")}, {QStringLiteral("font"), QStringLiteral("Arial")}, {QStringLiteral("size"), 6},
                                   {QStringLiteral("color"), QStringLiteral("#ffcc00")}, {QStringLiteral("box"), QStringLiteral("dark")},
                                   {QStringLiteral("position"), QStringLiteral("top")}},
                                  {}, &log);
        check(error.isEmpty() && log.contains(QStringLiteral("Restyled 2 captions")),
              QStringLiteral("Restyle Captions: both imported captions restyled (%1: %2)").arg(ok(error), log));
    }
    {   // Setlist from CSV (through live_setlist).
        fresh();
        const QString csv = write(QStringLiteral("plugins/setlist.csv"), "song,bpm,scene,notes\nOpener,128,1,Big start\nBallad,0,0\n");
        const QString error = run(QStringLiteral("setlist-csv"), {{QStringLiteral("csv"), csv}, {QStringLiteral("lock"), true}}, {{csv}, {}, {}});
        const QJsonArray songs = call(QStringLiteral("live_setlist"))[QStringLiteral("songs")].toArray();
        const QJsonObject bad = call(QStringLiteral("live_setlist"), {{QStringLiteral("songs"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("x")},
                                                                                                                   {QStringLiteral("scene"), 9}}}}});
        check(error.isEmpty() && songs.size() == 2 && songs.at(0)[QStringLiteral("bpm")].toDouble() == 128 && songs.at(0)[QStringLiteral("scene")].toInt() == 1
                  && bad.contains(QStringLiteral("error")) && call(QStringLiteral("live_setlist"))[QStringLiteral("songs")].toArray().size() == 2,
              QStringLiteral("Setlist from CSV: 2 songs, the first at 128 BPM with scene 1 (%1, %2); live_setlist refuses scene 9 and keeps the list")
                  .arg(songs.size()).arg(ok(error)));
    }
    {   // Genre Pads: every style's chains are real effects.
        fresh();
        QStringList failed;
        for (const QString& genre : {QStringLiteral("ambient"), QStringLiteral("techno"), QStringLiteral("punk"), QStringLiteral("retro"), QStringLiteral("psychedelic")}) {
            const QString error = run(QStringLiteral("genre-pads"),
                                      {{QStringLiteral("genre"), genre}, {QStringLiteral("first"), QStringLiteral("9")}, {QStringLiteral("momentary"), false}}, {});
            if (!error.isEmpty())
                failed << genre + QStringLiteral(": ") + error;
        }
        QStringList names;
        for (const QJsonValue pad : call(QStringLiteral("live_status"))[QStringLiteral("pads")].toArray())
            names << pad[QStringLiteral("name")].toString();
        check(failed.isEmpty() && names.contains(QStringLiteral("Kaleido")) && names.contains(QStringLiteral("Thermal")),
              QStringLiteral("Genre Pads: all five styles fill pads 9-16 (%1)").arg(failed.isEmpty() ? names.join(QStringLiteral(", ")) : failed.join(QStringLiteral("; "))));
    }
    {   // Photo Slideshow: three pictures in a folder (and a text file, left out).
        fresh();
        QDir().mkpath(work.filePath(QStringLiteral("plugins/photos")));
        for (int i = 0; i < 3; ++i) {
            QImage picture(320, 180, QImage::Format_RGB32);
            picture.fill(QColor::fromHsv(i * 100, 200, 220));
            picture.save(work.filePath(QStringLiteral("plugins/photos/p%1.png").arg(i)));
        }
        write(QStringLiteral("plugins/photos/notes.txt"), "not a picture");
        const QString folder = work.filePath(QStringLiteral("plugins/photos"));
        const QString error = run(QStringLiteral("photo-slideshow"),
                                  {{QStringLiteral("folder"), folder}, {QStringLiteral("seconds"), 3}, {QStringLiteral("fade"), 1}, {QStringLiteral("zoom"), true},
                                   {QStringLiteral("music"), QString()}},
                                  {{}, {folder}, {}});
        const int a = events(QStringLiteral("Slideshow A")).size(), b = events(QStringLiteral("Slideshow B")).size();
        const bool fades = a == 1 && events(QStringLiteral("Slideshow A")).at(0)[QStringLiteral("fade_in")].toDouble() == 1;
        check(error.isEmpty() && a + b == 3 && fades, QStringLiteral("Photo Slideshow: 3 pictures over two tracks, crossfading (%1 + %2, %3)").arg(a).arg(b).arg(ok(error)));
    }
    {   // Shot List Export.
        fresh();
        const QString out = work.filePath(QStringLiteral("plugins/shots.csv"));
        QFile::remove(out);
        const QString error = run(QStringLiteral("shot-list"),
                                  {{QStringLiteral("output"), out}, {QStringLiteral("kind"), QStringLiteral("all")}, {QStringLiteral("timecode"), true}},
                                  {{}, {}, {out}});
        QFile f(out);
        f.open(QIODevice::ReadOnly);
        const QStringList lines = QString::fromUtf8(f.readAll()).split(QStringLiteral("\r\n"), Qt::SkipEmptyParts);
        int count = 0;
        for (const QJsonValue t : tracks())
            count += t[QStringLiteral("events")].toArray().size();
        check(error.isEmpty() && lines.size() == count + 1 && lines.value(0).startsWith(QStringLiteral("track,kind,start")),
              QStringLiteral("Shot List Export: a header and a row for each of the %1 events (%2 lines, %3)").arg(count).arg(lines.size()).arg(ok(error)));
    }
    {   // Stream Highlights: the chat busiest near 3 s and 9 s of the 12 s sample.
        fresh();
        QByteArray chat = "[0:00:01] a: hi\n";
        for (int i = 0; i < 12; ++i)
            chat += "[0:00:03] fan: wow\n";
        for (int i = 0; i < 8; ++i)
            chat += "0:00:09 fan: again\n";
        const QString log = write(QStringLiteral("plugins/chat.txt"), chat);
        const double before = call(QStringLiteral("get_project"))[QStringLiteral("duration")].toDouble();
        const QString error = run(QStringLiteral("stream-highlights"),
                                  {{QStringLiteral("recording"), sample}, {QStringLiteral("chat"), log}, {QStringLiteral("count"), 2},
                                   {QStringLiteral("seconds"), 3}, {QStringLiteral("lead"), 1}, {QStringLiteral("offset"), 0}},
                                  {{sample, log}, {}, {}});
        QStringList cuts;
        for (const QJsonValue t : tracks())
            if (t[QStringLiteral("type")].toString() == QLatin1String("video"))
                for (const QJsonValue e : t[QStringLiteral("events")].toArray())
                    if (e[QStringLiteral("name")].toString().startsWith(QStringLiteral("Highlight")) && e[QStringLiteral("start")].toDouble() >= before - 0.01)
                        cuts << QString::number(e[QStringLiteral("source_in")].toDouble(), 'f', 1);
        cuts.sort();
        check(error.isEmpty() && cuts.size() == 2, QStringLiteral("Stream Highlights: 2 clips from the busiest chat (from %1 s) (%2)").arg(cuts.join(QStringLiteral(", ")), ok(error)));
    }

    // ---- The sandbox, with a plugin of the test's own in the plugins folder ----
    const QString folder = QDir(plugins::userFolder()).filePath(QStringLiteral("sandbox-test"));
    QDir(folder).removeRecursively();
    QDir().mkpath(folder);
    auto put = [](const QString& path, const QByteArray& text) {
        QFile f(path);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(text);
    };
    put(QDir(folder).filePath(QStringLiteral("plugin.json")),
        R"({"id": "sandbox-test", "name": "Sandbox", "commands": [{"label": "Try", "script": "main.js", "fields": [{"id": "what", "type": "text"}, {"id": "path", "type": "text"}]}]})");
    put(QDir(folder).filePath(QStringLiteral("main.js")), R"(
        const a = mvp.args;
        if (a.what === "read") mvp.readText(a.path);
        if (a.what === "list") mvp.listFolder(a.path);
        if (a.what === "write") mvp.writeText(a.path, "written");
        if (a.what === "loop") for (;;) {}
        if (a.what === "undo") mvp.call("undo");
        if (a.what === "open") mvp.call("open_project", { path: a.path });
        if (a.what === "rollback") {
            mvp.call("add_marker", { time: 1, name: "gone" });
            mvp.rollback();
            mvp.call("add_marker", { time: 2, name: "kept" });
        }
        if (a.what === "tools") mvp.log(String(mvp.tools().length));
        if (a.what === "throw") throw new Error("on purpose");
        if (a.what === "call") mvp.call(a.path.split(" ")[0], JSON.parse(a.path.slice(a.path.indexOf(" ") + 1)));
        if (a.what === "qobject") mvp.log([typeof mvp.deleteLater, typeof mvp.destroyed, typeof mvp.objectName, typeof mvp.call].join(","));
    )");
    QDir().mkpath(QDir(plugins::userFolder()).filePath(QStringLiteral("broken")));
    put(QDir(plugins::userFolder()).filePath(QStringLiteral("broken/plugin.json")), "{ not json");
    auto sandbox = [&](const QString& what, const QString& path, const plugins::Access& access, QString* log = nullptr) {
        return run(QStringLiteral("sandbox-test"), {{QStringLiteral("what"), what}, {QStringLiteral("path"), path}}, access, log);
    };
    fresh();
    const QString chosenDir = work.filePath(QStringLiteral("plugins/photos")), secret = write(QStringLiteral("plugins/secret.txt"), "secret");
    const QString inFolder = sandbox(QStringLiteral("read"), chosenDir + QStringLiteral("/notes.txt"), {{}, {chosenDir}, {}});
    const QString outside = sandbox(QStringLiteral("read"), secret, {{}, {chosenDir}, {}});
    const QString escape = sandbox(QStringLiteral("read"), chosenDir + QStringLiteral("/../secret.txt"), {{}, {chosenDir}, {}});
    const QString listOther = sandbox(QStringLiteral("list"), work.absolutePath(), {{}, {chosenDir}, {}});
    const QString target = work.filePath(QStringLiteral("plugins/out.txt"));
    QFile::remove(target);
    const QString writeOk = sandbox(QStringLiteral("write"), target, {{}, {}, {target}});
    const QString writeOther = sandbox(QStringLiteral("write"), secret, {{}, {}, {target}});
    QFile secretFile(secret);
    secretFile.open(QIODevice::ReadOnly);
    check(inFolder.isEmpty() && outside.contains(QStringLiteral("only a file chosen")) && escape.contains(QStringLiteral("only a file chosen"))
              && listOther.contains(QStringLiteral("only a folder chosen")) && writeOk.isEmpty() && QFileInfo::exists(target)
              && writeOther.contains(QStringLiteral("only the file a save field names")) && secretFile.readAll() == "secret",
          QStringLiteral("a script reads only chosen files and files in chosen folders (%1; outside: %2; ../: %3), lists only chosen folders (%4), "
                         "writes only the save field's file (%5; another: %6)")
              .arg(ok(inFolder), ok(outside), ok(escape), ok(listOther), ok(writeOk), ok(writeOther)));
    const QString undo = sandbox(QStringLiteral("undo"), {}, {});
    const QString open = sandbox(QStringLiteral("open"), work.filePath(QStringLiteral("x.mvp")), {});
    QString toolsLog;
    sandbox(QStringLiteral("tools"), {}, {}, &toolsLog);
    const QString thrown = sandbox(QStringLiteral("throw"), {}, {});
    check(undo.contains(QStringLiteral("isn't available to plugins")) && open.contains(QStringLiteral("isn't available to plugins"))
              && toolsLog.toInt() == int(McpServer::toolList().size()) && thrown.contains(QStringLiteral("main.js:")) && thrown.contains(QStringLiteral("on purpose")),
          QStringLiteral("plugins can't undo or open projects (%1; %2), mvp.tools() lists the %3 commands, a thrown error says where (%4)")
              .arg(undo, open, toolsLog, thrown));
    const int before = m_project->undoStack()->index();
    const QString rolled = sandbox(QStringLiteral("rollback"), {}, {});
    QStringList marks;
    for (const QJsonValue m : call(QStringLiteral("get_project"))[QStringLiteral("markers")].toArray())
        marks << m[QStringLiteral("name")].toString();
    m_project->undoStack()->undo();
    const int afterUndo = call(QStringLiteral("get_project"))[QStringLiteral("markers")].toArray().size();
    check(rolled.isEmpty() && marks == QStringList{QStringLiteral("kept")} && m_project->undoStack()->index() == before && afterUndo == 0,
          QStringLiteral("mvp.rollback() takes back what the run did before it (markers: %1), and the rest is still one undo step (%2 left after it)")
              .arg(marks.join(QStringLiteral(", ")))
              .arg(afterUndo));
    QElapsedTimer clock;
    clock.start();
    const QString loop = sandbox(QStringLiteral("loop"), {}, {});
    const qint64 ms = clock.elapsed();
    QStringList loadProblems;
    plugins::load(&loadProblems);
    check(loop.contains(QStringLiteral("stopped after")) && ms > 15000 && ms < 40000
              && loadProblems.join(QLatin1Char(' ')).contains(QStringLiteral("broken")),
          QStringLiteral("a script that never ends is stopped (%1 s: %2); a broken plugin.json is reported (%3)")
              .arg(ms / 1000.0, 0, 'f', 1).arg(loop, loadProblems.join(QStringLiteral("; "))));

    // ---- What a script passes to commands: the same limits as its own file tools ----
    auto refused = [&](const QString& tool, const QJsonObject& a, const plugins::Access& access = {}) {
        return sandbox(QStringLiteral("call"), tool + QLatin1Char(' ') + QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)), access);
    };
    const QString mayOpen = QStringLiteral("isn't a file this plugin may open"), saveOnly = QStringLiteral("only the file a save field names");
    const QString picture = work.filePath(QStringLiteral("plugins/picture.png"));
    QImage(64, 36, QImage::Format_RGB32).save(picture);
    const QString importOther = refused(QStringLiteral("import_media"), {{QStringLiteral("paths"), QJsonArray{picture}}});
    const QString importChosen = refused(QStringLiteral("import_media"), {{QStringLiteral("paths"), QJsonArray{picture}}}, {{picture}, {}, {}});
    const QString startup = work.filePath(QStringLiteral("plugins/startup.bat"));
    const QString captionsOut = refused(QStringLiteral("captions"), {{QStringLiteral("action"), QStringLiteral("export")}, {QStringLiteral("path"), startup}});
    const QString renderOut = refused(QStringLiteral("render"), {{QStringLiteral("path"), startup}, {QStringLiteral("overwrite"), true}});
    const QString frameOut = refused(QStringLiteral("get_frame"), {{QStringLiteral("save_to"), work.filePath(QStringLiteral("plugins/x.png"))}});
    const QString dll = QStringLiteral("C:/Windows/System32/kernel32.dll");
    const QString vst = refused(QStringLiteral("add_effect"), {{QStringLiteral("effect"), QStringLiteral("a_vst3")},
                                                              {QStringLiteral("params"), QJsonObject{{QStringLiteral("plugin"), dll}}}});
    QElapsedTimer netClock;
    netClock.start();
    const QString unc = refused(QStringLiteral("import_media"), {{QStringLiteral("paths"), QJsonArray{QStringLiteral("\\\\192.0.2.1\\share\\x.png")}}});
    const QString uncRead = sandbox(QStringLiteral("read"), QStringLiteral("//192.0.2.1/share/x.txt"), {});
    const qint64 netMs = netClock.elapsed();
    check(importOther.contains(mayOpen) && !importChosen.contains(mayOpen) && captionsOut.contains(saveOnly) && renderOut.contains(saveOnly)
              && frameOut.contains(saveOnly) && vst.contains(mayOpen) && unc.contains(mayOpen) && uncRead.contains(QStringLiteral("only a file chosen"))
              && netMs < 3000 && !QFileInfo::exists(startup),
          QStringLiteral("commands open only what the form chose (other: %1; chosen: %2), write only the save file (captions: %3; render: %4; "
                         "frame: %5), load no DLL (%6) and never touch a network path (%7; readText: %8; %9 ms)")
              .arg(ok(importOther), ok(importChosen), ok(captionsOut), ok(renderOut), ok(frameOut), ok(vst), ok(unc), ok(uncRead))
              .arg(netMs));
    QStringList closed;
    for (const auto& [tool, a] : std::vector<std::pair<QString, QJsonObject>>{
             {QStringLiteral("youtube_clip"), {{QStringLiteral("url"), QStringLiteral("https://example.com/?data=secret")}}},
             {QStringLiteral("ai_components"), {{QStringLiteral("action"), QStringLiteral("install")}}},
             {QStringLiteral("collect_project"), {{QStringLiteral("destination"), work.filePath(QStringLiteral("plugins/copy"))}}},
             {QStringLiteral("set_live_source"), {{QStringLiteral("video"), QStringLiteral("camera")}}},
             {QStringLiteral("live_action"), {{QStringLiteral("action"), QStringLiteral("record")}}},
             {QStringLiteral("live_switcher"), {{QStringLiteral("inputs"), QJsonArray{QStringLiteral("camera")}}}},
             {QStringLiteral("render_queue"), {{QStringLiteral("action"), QStringLiteral("add")}, {QStringLiteral("project_file"), picture}}},
             {QStringLiteral("deliver"), {{QStringLiteral("action"), QStringLiteral("outputs")}, {QStringLiteral("folder"), work.absolutePath()}}},
             {QStringLiteral("presets"), {{QStringLiteral("action"), QStringLiteral("remove")}, {QStringLiteral("preset"), QStringLiteral("x")}}},
             {QStringLiteral("node_effect"), {{QStringLiteral("action"), QStringLiteral("save")}, {QStringLiteral("overwrite"), true}}}})
        if (!refused(tool, a).contains(QStringLiteral("available to plugins")))
            closed << tool;
    QString seen;
    sandbox(QStringLiteral("qobject"), {}, {}, &seen);
    QString vstError;
    vst3EffectClasses(dll, &vstError);
    check(closed.isEmpty() && seen == QStringLiteral("undefined,undefined,undefined,function")
#ifdef _WIN32
              && vstError.contains(QStringLiteral("isn't in a VST3 folder"))
#endif
              ,
          QStringLiteral("plugins can't go online, copy the project out, turn on a camera, open other projects, delete presets or "
                         "overwrite saved effects (allowed: %1); mvp is a plain object (%2); a_vst3 loads only installed plugins (%3)")
              .arg(closed.join(QStringLiteral(", ")), seen, vstError));
    QDir(folder).removeRecursively();
    QDir(QDir(plugins::userFolder()).filePath(QStringLiteral("broken"))).removeRecursively();
    call(QStringLiteral("new_project"), {{QStringLiteral("discard"), true}});
}
