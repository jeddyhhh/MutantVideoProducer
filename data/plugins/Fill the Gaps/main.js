// Fill the Gaps: every empty stretch of one video track gets random pieces of the videos in Project Media.

const args = mvp.args;

// A small random number generator that gives the same numbers for the same seed (Math.random can't be seeded).
function randomFrom(seed) {
    let a = (seed * 2654435761) >>> 0;
    return function () {
        a = (a + 0x6D2B79F5) >>> 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

// Places a piece of a file at `at` on `track`. It goes in past the end of everything first, where it can't cut into
// other events, is trimmed there, then moved into place (its sound, if kept, moves with it: they're grouped).
function placeClip(mediaId, at, track, sourceIn, length, keepSound) {
    const parking = mvp.call("get_project").duration + 60;
    const ids = mvp.call("add_media_to_timeline", { media_id: mediaId, at: parking, track: track }).event_ids;
    const types = {}, trackOf = {};
    const tracks = mvp.call("get_project").tracks;
    for (const t of tracks)
        for (const e of t.events) {
            types[e.event_id] = t.type;
            trackOf[e.event_id] = t;
        }
    const video = ids.filter(id => types[id] !== "audio");
    const audio = ids.filter(id => types[id] === "audio");
    // Its sound would land on whatever is on its audio track there: then it goes without.
    const busy = audio.some(id => trackOf[id].events.some(e => ids.indexOf(e.event_id) < 0 && e.start < at + length && e.end > at));
    if ((!keepSound || busy) && video.length && audio.length) {
        mvp.call("group", { action: "ungroup", event_ids: ids });
        mvp.call("delete_events", { event_ids: audio });
    }
    const main = video.length ? video[0] : audio[0];
    mvp.call("edit_event", { event_id: main, source_in: sourceIn, length: length });
    mvp.call("edit_event", { event_id: main, start: at });
    return main;
}

const project = mvp.call("get_project");
const videoTracks = project.tracks.filter(t => t.type === "video");
const track = videoTracks[args.track - 1];
if (!track)
    throw new Error("there's no video track " + args.track + " (the edit has " + videoTracks.length + ")");
if (track.locked)
    throw new Error("video track " + args.track + " is locked");
const footage = project.media.filter(m => m.online && m.width && !m.still && m.seconds >= 1);
if (!footage.length)
    throw new Error("there are no videos in Project Media to fill with");

// The gaps: from 0 to the end of the whole edit, wherever this track has nothing.
const gaps = [];
let at = 0;
for (const e of track.events.slice().sort((a, b) => a.start - b.start)) {
    if (e.start - at >= args.shortest)
        gaps.push([at, e.start]);
    at = Math.max(at, e.end);
}
if (project.duration - at >= args.shortest)
    gaps.push([at, project.duration]);
if (!gaps.length)
    throw new Error("video track " + args.track + " has no gaps of " + args.shortest + " seconds or more");

const random = randomFrom(args.seed);
let pieces = 0;
for (const [from, to] of gaps)
    for (let t = from; to - t > 0.04;) {
        const m = footage[Math.floor(random() * footage.length)];
        const length = Math.min(to - t, args.longest, m.seconds);
        const sourceIn = random() * (m.seconds - length);
        placeClip(m.media_id, t, track.index, sourceIn, length, args.sound);
        t += length;
        pieces++;
    }
mvp.log("Filled " + gaps.length + " gaps with " + pieces + " pieces");
