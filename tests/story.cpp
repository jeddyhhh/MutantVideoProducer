#include "core/Story.h"
#include "core/TimelineOps.h"
#include "core/Compositor.h"
#include "core/AudioMixer.h"
#include "core/GpuCompositor.h"
#include "core/FrameRenderer.h"
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QTemporaryDir>
#include <QTextStream>
#include <cmath>
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        QTextStream(stdout) << (ok ? "PASS " : "FAIL ") << what << Qt::endl;
        if (!ok)
            ++failures;
    };
    if (argc < 2) {
        QTextStream(stderr) << "usage: mvp_story_test <video with sound, e.g. testmedia/h264_aac.mp4>" << Qt::endl;
        return 2;
    }
    Project p;
    const auto info = probeMedia(QString::fromLocal8Bit(argv[1]));
    check(info.ok() && info.hasAudio() && info.hasVideo(), "audio/video fixture");
    if (!info.ok())
        return 1;
    int media = p.addMedia(info);
    Timeline original = p.timeline();
    auto placed = ops::insertMedia(original, *p.mediaItem(media), 2 * kUsPerSecond, -1, p.settings().fps, true);
    std::set<int> ids(placed.begin(), placed.end());
    for (int id : ids)
        original.findClip(id)->length = 2 * kUsPerSecond;
    int video = -1, audio = -1;
    for (size_t i = 0; i < original.tracks.size(); ++i)
        if (!original.tracks[i].clips.empty())
            (isVisualTrack(original.tracks[i].type) ? video : audio) = int(i);
    original.tracks[size_t(audio)].level = .6f;
    original.tracks[size_t(audio)].effects.push_back(makeEffectOfType("a_gain", original.allocId()));
    original.tracks[size_t(audio)].effects.back().params["gain"].value = -3;
    p.applyEdit(original, "fixture");
    Compositor beforeRenderer(true);
    const auto frame = beforeRenderer.render(*p.snapshot(), 2500000, {640, 360});
    AudioMixer beforeMixer(48000);
    std::vector<float> samples(2048);
    beforeMixer.mix(*p.snapshot(), 120000, 1024, samples.data());
    Timeline packed = original;
    QString error;
    auto containers = story::compound(packed, ids, "Chapter one", &error);
    check(containers.size() == 2 && error.isEmpty(), "compound creates paired picture and sound");
    check(packed.tracks.size() == original.tracks.size(), "packing collapses selected source tracks");
    check(timelineProblem(packed).isEmpty(), "packed timeline invariants");
    p.applyEdit(packed, "pack");
    Compositor packedRenderer(true);
    const auto packedFrame = packedRenderer.render(*p.snapshot(), 2500000, {640, 360});
    check(packedFrame == frame, "compound CPU picture matches original");
    AudioMixer packedMixer(48000);
    std::vector<float> packedSamples(2048);
    packedMixer.mix(*p.snapshot(), 120000, 1024, packedSamples.data());
    double difference = 0, peak = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        difference = std::max(difference, double(std::abs(samples[i] - packedSamples[i])));
        peak = std::max(peak, double(std::abs(samples[i])));
    }
    check(peak > 1e-5 && difference < 1e-5, "compound retains real audio and track gain/effects");
    check(p.isMediaUsed(media) && !p.removeMedia(media), "nested source media protected from removal");
    p.undoStack()->undo();
    check(p.timeline() == original, "one undo restores source tracks");
    p.undoStack()->redo();
    check(p.timeline() == packed, "redo restores nested content");
    QTemporaryDir folder;
    QString path = folder.path() + "/nested.mvp";
    check(p.save(path, &error), "save compound project");
    Project loaded;
    check(loaded.load(path, &error) && loaded.timeline() == packed, "reload preserves embedded tracks and metadata");
    auto nestedJson = nestedTimelineToJson(*packed.findClip(containers.front())->sequence.value);
    check(nestedTimelineFromJson(nestedJson) == *packed.findClip(containers.front())->sequence.value, "embedded JSON round trip");
    Timeline unpacked = packed;
    check(story::unpack(unpacked, containers.front(), &error), "unpack full range");
    check(timelineProblem(unpacked).isEmpty(), "unpack remaps effect, track, event and group IDs");
    p.setTimelineLive(unpacked);
    Compositor unpackRenderer(true);
    check(unpackRenderer.render(*p.snapshot(), 2500000, {640, 360}) == frame, "unpacked picture stays identical");
    AudioMixer unpackMixer(48000);
    std::vector<float> unpackSamples(2048);
    unpackMixer.mix(*p.snapshot(), 120000, 1024, unpackSamples.data());
    difference = 0;
    for (size_t i = 0; i < samples.size(); ++i)
        difference = std::max(difference, double(std::abs(samples[i] - unpackSamples[i])));
    check(difference < 1e-5, "unpacked sound stays identical");
    auto surface = createGpuSurface();
    auto gpu = surface ? GpuCompositor::create(surface.get(), true, &error) : nullptr;
    check(bool(gpu), "GPU available for sequence check");
    if (gpu) {
        p.setTimelineLive(packed);
        const auto gpuFrame = gpu->render(*p.snapshot(), 2500000, {640, 360});
        double d = 0;
        auto a = frame.convertToFormat(QImage::Format_RGBA8888), b = gpuFrame.convertToFormat(QImage::Format_RGBA8888);
        if (a.size() == b.size()) {
            for (int y = 0; y < a.height(); ++y)
                for (int x = 0; x < a.width() * 4; ++x)
                    d += std::abs(int(a.constScanLine(y)[x]) - int(b.constScanLine(y)[x]));
            d /= a.width() * a.height() * 4;
        } else
            d = 255;
        check(d < 3, "GPU sequence picture agrees with CPU");
    }
    Timeline twice = packed;
    auto second = story::compound(twice, std::set<int>(containers.begin(), containers.end()), "Nested twice", &error);
    p.setTimelineLive(twice);
    Compositor twiceRenderer(true);
    check(second.size() == 2 && twiceRenderer.render(*p.snapshot(), 2500000, {640, 360}) == frame, "two nesting levels preserve picture");
    Clip sequence = *packed.findClip(containers.front());
    ops::setClipSpeed(sequence, nullptr, .5, false, p.settings().fps);
    check(sequence.length <= 4 * kUsPerSecond, "sequence speed constrained by source duration");
    const auto bounds = ops::trimRange(*packed.findClip(containers.front()), nullptr, false);
    check(bounds.second == 0, "sequence cannot trim past source end");
    Timeline storyTl;
    int first = story::addChapter(storyTl, "Origins", "Narration one", 2 * kUsPerSecond, 3 * kUsPerSecond, p.mediaItem(media).get(), 0,
                                  p.settings().fps, &error);
    int secondChapter =
        story::addChapter(storyTl, "Evolution", "Narration two", 3 * kUsPerSecond, 2 * kUsPerSecond, nullptr, 0, p.settings().fps, &error);
    check(first && secondChapter && storyTl.duration() == 10 * kUsPerSecond, "story chapters include separate listening time");
    check(
        story::updateChapter(storyTl, first, "Origins revised", "New script", 3 * kUsPerSecond, 4 * kUsPerSecond, p.settings().fps, &error),
        "change chapter duration");
    check(storyTl.chapters[1].toObject()["start"].toString().toLongLong() == 7 * kUsPerSecond && storyTl.duration() == 12 * kUsPerSecond,
          "duration change moves later chapters and clips together");
    check(story::moveChapter(storyTl, first, 1, &error) && storyTl.chapters[0].toObject()["id"].toInt() == secondChapter,
          "swap adjacent chapters");
    check(story::removeChapter(storyTl, first, &error) && storyTl.duration() == 5 * kUsPerSecond, "delete chapter ripples its full range");
    check(timelineProblem(storyTl).isEmpty(), "chapter edit invariants");
    {
        // An assigned clip dragged out of its chapter is no longer the chapter's to move or delete.
        Timeline tl;
        const auto fps = p.settings().fps;
        const int c0 = story::addChapter(tl, "Zero", "", 2 * kUsPerSecond, 3 * kUsPerSecond, nullptr, 0, fps, &error);
        const int a = story::addChapter(tl, "A", "", 2 * kUsPerSecond, 3 * kUsPerSecond, nullptr, 0, fps, &error);
        const int b = story::addChapter(tl, "B", "", 2 * kUsPerSecond, 3 * kUsPerSecond, nullptr, 0, fps, &error);
        auto clipOf = [&](int chapter) {
            for (const auto value : tl.chapters)
                if (value.toObject()["id"].toInt() == chapter)
                    return value.toObject()["clips"].toArray()[0].toInt();
            return 0;
        };
        const int strayB = clipOf(b), strayA = clipOf(a);
        check(c0 && a && b && strayA && strayB, "three chapters with a placeholder each");
        tl.findClip(strayB)->start = 30 * kUsPerSecond; // dragged well past the story
        tl.findClip(strayB)->length = kUsPerSecond;
        tl.sortClips();
        Timeline moved = tl;
        check(story::moveChapter(moved, a, 1, &error) && moved.findClip(strayB) && moved.findClip(strayB)->start == 30 * kUsPerSecond
                  && timelineProblem(moved).isEmpty(),
              "swapping chapters leaves a clip dragged out of one where it was put: " + error);
        Timeline removed = tl;
        check(story::removeChapter(removed, b, &error) && removed.findClip(strayB)
                  && removed.findClip(strayB)->start == 25 * kUsPerSecond,
              "deleting a chapter keeps a clip dragged out of it (it moves back with what follows): " + error);
        Timeline early = tl;
        early.findClip(strayB)->start = 30 * kUsPerSecond;
        early.findClip(strayA)->start = 40 * kUsPerSecond; // (A's too: then B's range is A's to swap with, both empty)
        early.sortClips();
        check(story::moveChapter(early, a, 1, &error) && early.findClip(strayA)->start == 40 * kUsPerSecond
                  && early.findClip(strayB)->start == 30 * kUsPerSecond && timelineProblem(early).isEmpty(),
              "swapping two emptied chapters moves neither stray clip: " + error);
    }
    {
        // A chapter's clip split (S on the timeline): both pieces are the chapter's.
        Timeline tl;
        const auto fps = p.settings().fps;
        const int a = story::addChapter(tl, "A", "", 2 * kUsPerSecond, 3 * kUsPerSecond, nullptr, 0, fps, &error);
        const int b = story::addChapter(tl, "B", "", 2 * kUsPerSecond, 3 * kUsPerSecond, nullptr, 0, fps, &error);
        const auto pieces = ops::splitClips(tl, {}, 2 * kUsPerSecond);
        error.clear();
        Timeline swapped = tl;
        const bool swap = story::moveChapter(swapped, a, 1, &error);
        check(a && b && pieces.size() == 1 && swap && swapped.findClip(pieces[0]) && swapped.findClip(pieces[0])->start == 7 * kUsPerSecond
                  && timelineProblem(swapped).isEmpty(),
              "a split chapter clip's pieces both move with the chapter: " + error);
        check(story::removeChapter(tl, a, &error) && !tl.findClip(pieces[0]) && tl.duration() == 5 * kUsPerSecond,
              "and both go when the chapter is deleted: " + error);
    }
    {
        // Ripple edits (Ctrl+Del, auto ripple on all tracks) move the chapters with their clips, as they move markers.
        Timeline tl;
        const auto fps = p.settings().fps;
        const int intro = ops::insertGenerated(tl, *findGeneratorPreset("solid_white"), 0, -1, fps);
        tl.findClip(intro)->length = 3 * kUsPerSecond;
        const int a = story::addChapter(tl, "A", "", 2 * kUsPerSecond, 3 * kUsPerSecond, nullptr, 0, fps, &error);
        const int b = story::addChapter(tl, "B", "", 2 * kUsPerSecond, 3 * kUsPerSecond, nullptr, 0, fps, &error);
        auto at = [&](int i, const char* key) { return tl.chapters[i].toObject()[key].toString().toLongLong(); };
        const bool laid = a && b && at(0, "start") == 3 * kUsPerSecond && at(1, "start") == 8 * kUsPerSecond;
        ops::rippleRemoveClipsAll(tl, {intro}, true);
        check(laid && at(0, "start") == 0 && at(1, "start") == 5 * kUsPerSecond && tl.findMarker(b)->time == 5 * kUsPerSecond,
              QStringLiteral("a ripple delete in front of the story moves its chapters back with their clips (%1, %2)")
                  .arg(at(0, "start")).arg(at(1, "start")));
        ops::rippleShift(tl, 5 * kUsPerSecond, 2 * kUsPerSecond, {}, {}, true); // (time put in between the chapters)
        check(at(0, "start") == 0 && at(0, "narration") + at(0, "listening") == 5 * kUsPerSecond && at(1, "start") == 7 * kUsPerSecond
                  && at(1, "narration") + at(1, "listening") == 5 * kUsPerSecond,
              QStringLiteral("time rippled in between two chapters moves the later one, and neither grows (%1, %2)")
                  .arg(at(0, "narration") + at(0, "listening")).arg(at(1, "start")));
        ops::rippleShift(tl, 7 * kUsPerSecond, -2 * kUsPerSecond, {}, {}, true);
        error.clear();
        check(at(1, "start") == 5 * kUsPerSecond && story::moveChapter(tl, a, 1, &error) && timelineProblem(tl).isEmpty()
                  && tl.chapters[0].toObject()["id"].toInt() == b && at(0, "start") == 0,
              "and the chapters can still be swapped afterwards: " + error);
    }
    Timeline adopted = original;
    int adoptedId = story::adoptChapter(adopted, "Existing edit", "Script", 2 * kUsPerSecond, 3 * kUsPerSecond, kUsPerSecond, &error);
    check(adoptedId && adopted.chapters[0].toObject()["clips"].toArray().size() == 2,
          "adopt existing range splits and assigns picture/sound");
    check(!story::adoptChapter(adopted, "Overlap", "", 2500000, 3500000, 500000, &error), "overlapping chapters rejected");
    Timeline variation = original, repeated = original;
    check(story::directorsDice(variation, ids, 42, 2, .8) == 1, "dice varies visual clips");
    story::directorsDice(repeated, ids, 42, 2, .8);
    check(variation == repeated, "dice seed reproduces the same edit");
    check(variation.tracks[size_t(audio)] == original.tracks[size_t(audio)], "dice leaves sound intact");
    const auto& old = original.tracks[size_t(video)].clips.front();
    const auto& changed = variation.tracks[size_t(video)].clips.front();
    check(old.start == changed.start && old.length == changed.length && old.sourceIn == changed.sourceIn && old.speed == changed.speed,
          "dice preserves timing/source sync");
    check(!changed.effects.empty() && changed.effects.back().params.at("fxMix").at(200000, ParamType::Number) == 0,
          "dice effect accent clears after entrance");
    Timeline markerTimeline = *packed.findClip(containers.front())->sequence.value;
    Marker marker;
    marker.id = markerTimeline.allocId();
    marker.time = 100000;
    marker.name = "Beat";
    markerTimeline.markers.push_back(marker);
    check(nestedTimelineFromJson(nestedTimelineToJson(markerTimeline)) == markerTimeline, "nested markers persist");
    {
        Timeline edited = packed;
        Timeline content = *packed.findClip(containers.front())->sequence.value;
        for (auto& t : content.tracks)
            for (auto& c : t.clips)
                c.length += kUsPerSecond;
        Track future;
        future.id = edited.allocId();
        Clip tail;
        tail.id = edited.allocId();
        tail.start = 6 * kUsPerSecond;
        tail.length = kUsPerSecond;
        tail.generator.type = "text";
        future.clips.push_back(tail);
        edited.tracks.push_back(future);
        Marker chapterMarker;
        chapterMarker.id = edited.allocId();
        chapterMarker.time = 2 * kUsPerSecond;
        chapterMarker.length = 2 * kUsPerSecond;
        edited.markers.push_back(chapterMarker);
        QJsonArray owned;
        for (int id : containers)
            owned.append(id);
        edited.chapters.append(QJsonObject{{"id", chapterMarker.id},
                                           {"name", "Compound chapter"},
                                           {"start", "2000000"},
                                           {"narration", "1000000"},
                                           {"listening", "1000000"},
                                           {"clips", owned}});
        edited.chapters.append(QJsonObject{
            {"id", edited.allocId()}, {"name", "Following chapter"}, {"start", "6000000"}, {"narration", "1000000"}, {"listening", "0"}});
        check(story::replaceCompound(edited, containers.front(), content, true, &error), "replace compound content with longer edit");
        check(edited.findClip(tail.id)->start == 7 * kUsPerSecond && edited.chapters[1].toObject()["start"].toString() == "7000000",
              "compound duration ripples clips and chapter metadata");
        check(edited.chapters[0].toObject()["listening"].toString() == "2000000"
                  && edited.findMarker(chapterMarker.id)->length == 3 * kUsPerSecond,
              "compound chapter retains narration window and updates listening span");
        check(edited.findClip(containers.back())->sequence == edited.findClip(containers.front())->sequence,
              "picture/audio containers share updated content");
    }
    {
        // 24 fps frame times round up, so summed chapter durations used to land 1 us past the grid and the next chapter's
        // clips started inside the previous chapter, failing later updates.
        const FrameRate fps{24, 1};
        Timeline tl;
        std::vector<int> chapters;
        const TimeUs lengths[] = {1370000, 2110000, 950000, 3330000, 1710000};
        for (int k = 0; k < 5; ++k)
            chapters.push_back(story::addChapter(tl, QString("Chapter %1").arg(k), "", lengths[k], lengths[(k + 2) % 5],
                                                 k % 2 ? nullptr : p.mediaItem(media).get(), 0, fps, &error));
        const int voice = ops::addTrack(tl, TrackType::Audio);
        auto chapter = [&](int id) {
            for (auto v : tl.chapters)
                if (v.toObject()["id"].toInt() == id)
                    return v.toObject();
            return QJsonObject();
        };
        for (int k = 0; k < 5; ++k) {
            Clip v;
            v.id = tl.allocId();
            v.start = chapter(chapters[size_t(k)])["start"].toString().toLongLong();
            v.length = fps.snap(lengths[k] / 2);
            v.narration["takes"] = QJsonArray();
            tl.tracks[size_t(voice)].clips.push_back(v);
            tl.sortClips();
            story::bindChapter(tl, chapters[size_t(k)], {v.id}, &error);
        }
        auto aligned = [&] {
            for (int j = 0; j < tl.chapters.size(); ++j) {
                const auto c = tl.chapters[j].toObject();
                const TimeUs start = c["start"].toString().toLongLong(),
                             end = start + c["narration"].toString().toLongLong() + c["listening"].toString().toLongLong();
                TimeUs first = std::numeric_limits<TimeUs>::max();
                for (auto v : c["clips"].toArray())
                    if (const auto* clip = tl.findClip(v.toInt()))
                        first = std::min(first, clip->start);
                if (first != start || (j + 1 < tl.chapters.size() && tl.chapters[j + 1].toObject()["start"].toString().toLongLong() != end))
                    return false;
            }
            return timelineProblem(tl).isEmpty();
        };
        check(aligned(), "new chapters abut exactly and start with their clips");
        const struct {
            int k;
            TimeUs narration, listening;
        } edits[] = {{0, 1500000, 2900000}, {1, 2500000, 710000}, {0, 1210000, 1000000},
                     {2, 900000, 3000000},  {1, 1100000, 400000}, {3, 1700000, 1250000}};
        bool ok = true;
        for (const auto& e : edits)
            ok = story::updateChapter(tl, chapters[size_t(e.k)], "Resized", "", e.narration, e.listening, fps, &error) && aligned() && ok;
        check(ok, "grow/shrink chapters keeps every chapter on its clips: " + error);
        // Projects saved before the fix: chapter end 1 us past the next chapter's start.
        auto drifted = tl.chapters[2].toObject();
        drifted["listening"] = QString::number(drifted["listening"].toString().toLongLong() + 1);
        tl.chapters[2] = drifted;
        check(story::updateChapter(tl, chapters[2], "Healed", "", 1000000, 3500000, fps, &error) && aligned(),
              "drifted chapter end is healed: " + error);
        check(story::updateChapter(tl, chapters[3], "After", "", 1700000, 2000000, fps, &error)
                  && story::updateChapter(tl, chapters[4], "Last", "", 1000000, 500000, fps, &error) && aligned(),
              "later chapters still update: " + error);
    }
    {
        // A track rack's chain settings are keyframed in timeline time: packed into a compound, they move with the clips.
        Timeline tl = original;
        Track& t = tl.tracks[size_t(audio)];
        EffectInstance rack = makeRack(tl.allocId());
        rack.chains.push_back(makeRackChain(true, "c"));
        rack.chains.back().params["level"].addKey(3 * kUsPerSecond, 50);
        t.effects.push_back(rack);
        QString why;
        const auto made = story::compound(tl, ids, "Rack", &why);
        const Clip* sound = nullptr;
        for (int id : made)
            if (const Clip* c = tl.findClip(id); c && !c->sequence.value->tracks.empty())
                for (const Track& inner : c->sequence.value->tracks)
                    if (!isVisualTrack(inner.type))
                        sound = c;
        TimeUs key = -1;
        if (sound)
            for (const Track& inner : sound->sequence.value->tracks)
                for (const EffectInstance& e : inner.effects)
                    if (e.isRack())
                        key = e.chains.front().params.at("level").keys.front().time;
        check(key == kUsPerSecond, QStringLiteral("compound: a track rack chain's keyframes move with the clips (%1 us) %2").arg(key).arg(why));
    }
    {
        // Remove from Project isn't undoable: undoing the delete before it brings the file back with its events.
        Project q;
        const int m = q.addMedia(info);
        Timeline tl = q.timeline();
        ops::insertMedia(tl, *q.mediaItem(m), 0, -1, q.settings().fps, true);
        q.applyEdit(tl, "insert");
        Timeline empty = q.timeline();
        for (auto& t : empty.tracks)
            t.clips.clear();
        q.applyEdit(empty, "delete");
        const bool removed = q.removeMedia(m) && !q.mediaItem(m);
        q.undoStack()->undo();
        check(removed && q.mediaItem(m) && q.isMediaUsed(m) && q.snapshot()->media.count(m),
              "undoing a delete brings back media removed from the project after it");
        q.undoStack()->redo();
        const bool again = q.removeMedia(m) && q.addMedia(info) == m && q.media().size() == 1;
        check(again, "importing a removed file again gives back the same item");
    }
    return failures;
}
