// Stream Highlights: the chat's busiest windows of the recording, cut in time order at the end of the edit.

const args = mvp.args;

// Every chat message's time, in seconds from the start of the stream.
function chatTimes(text) {
    const trimmed = text.trim();
    if (trimmed.startsWith("{") || trimmed.startsWith("[{")) { // Twitch chat JSON
        const data = JSON.parse(trimmed);
        const comments = Array.isArray(data) ? data : data.comments || [];
        return comments.map(c => Number(c.content_offset_seconds)).filter(t => !isNaN(t));
    }
    const times = [];
    for (const line of text.split(/\r?\n/)) {
        const clock = /^\s*\[?(\d+):(\d{2})(?::(\d{2}))?(?:\.\d+)?\]?/.exec(line); // [1:02:03] or 1:02:03 or 02:03
        if (clock)
            times.push(clock[3] !== undefined ? Number(clock[1]) * 3600 + Number(clock[2]) * 60 + Number(clock[3])
                                              : Number(clock[1]) * 60 + Number(clock[2]));
        else {
            const first = Number(line.split(",")[0]); // CSV: seconds first
            if (line.trim() && !isNaN(first))
                times.push(first);
        }
    }
    return times;
}

const times = chatTimes(mvp.readText(args.chat)).map(t => t - args.offset).filter(t => t >= 0);
if (times.length < 2)
    throw new Error("no message times found in " + args.chat);

const recording = mvp.call("get_project").media.find(m => m.media_id === mvp.call("import_media", { paths: [args.recording] }).media_ids[0]);
const length = recording.seconds;

// Messages per second, then the busiest windows of `seconds`, none overlapping another.
const perSecond = new Array(Math.ceil(length) + 1).fill(0);
for (const t of times)
    if (t < length)
        perSecond[Math.floor(t)]++;
const windowSize = Math.round(args.seconds);
const score = [];
let sum = 0;
for (let s = 0; s < perSecond.length; s++) {
    sum += perSecond[s] - (s >= windowSize ? perSecond[s - windowSize] : 0);
    if (s >= windowSize - 1)
        score.push({ end: s + 1, messages: sum });
}
score.sort((a, b) => b.messages - a.messages);
const picked = [];
for (const w of score) {
    if (picked.length >= args.count || w.messages === 0)
        break;
    // The busiest moment is the middle of the window; the clip starts `lead` seconds before it.
    const peak = w.end - windowSize / 2;
    const from = Math.max(0, Math.min(length - args.seconds, peak - args.lead));
    if (picked.every(p => from + args.seconds <= p.from || from >= p.from + args.seconds))
        picked.push({ from: from, messages: w.messages });
}
if (!picked.length)
    throw new Error("none of the chat's times fall inside the recording (" + length.toFixed(0) + " seconds); try the offset");
picked.sort((a, b) => a.from - b.from);

// At the end of the edit nothing is in the way: each clip is placed there, trimmed, and the next goes after it.
let at = mvp.call("get_project").duration;
const firstVideo = mvp.call("get_project").tracks.findIndex(t => t.type === "video");
for (let i = 0; i < picked.length; i++) {
    const place = { media_id: recording.media_id, at: at };
    if (firstVideo >= 0)
        place.track = firstVideo;
    const event = mvp.call("add_media_to_timeline", place).event_ids[0];
    mvp.call("edit_event", { event_id: event, source_in: picked[i].from, length: args.seconds, name: "Highlight " + (i + 1),
                             fade_in: i ? 0.3 : 0, fade_out: 0.3 });
    at += args.seconds;
}
mvp.log("Stream highlights: " + picked.length + " clips (" + picked.map(p => p.messages).join(", ") + " messages)");
