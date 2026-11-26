// Lyric Video: an .lrc file's lines as titles on a new track ("Lyrics"), each until the next line starts.
// LRC lines look like "[01:02.50] the words"; a line can carry several times ("[00:30.00][01:30.00] chorus"), and a
// time with no words ends the line before it (an instrumental break). Tags such as [ar:Artist] are skipped.

const args = mvp.args;
const lines = [];
for (const raw of mvp.readText(args.lrc).split(/\r?\n/)) {
    const words = raw.replace(/\[[^\]]*\]/g, "").trim();
    const stamp = /\[(\d+):(\d+(?:\.\d+)?)\]/g;
    for (let t = stamp.exec(raw); t; t = stamp.exec(raw))
        lines.push({ time: Number(t[1]) * 60 + Number(t[2]) + args.offset, words: words });
}
lines.sort((a, b) => a.time - b.time);
if (!lines.some(l => l.words))
    throw new Error("no timed lines in " + args.lrc + " (they look like [00:12.34] the words)");

// Onto the beat: Detect Beats puts a marker on every beat; those are taken, and the markers that were there before
// are put back as they were (regions are left alone throughout).
if (args.snap) {
    const before = mvp.call("get_project").markers.filter(m => !m.length);
    const seen = new Set(before.map(m => m.time));
    try {
        mvp.call("detect_beats", { every: 1 });
        const beats = mvp.call("get_project").markers.filter(m => !m.length && !seen.has(m.time)).map(m => m.time);
        mvp.call("remove_markers", {});
        for (const m of before)
            mvp.call("add_marker", m.name ? { time: m.time, name: m.name } : { time: m.time });
        const nearest = t => beats.reduce((best, b) => Math.abs(b - t) < Math.abs(best - t) ? b : best, t);
        if (beats.length)
            for (const l of lines)
                l.time = nearest(l.time);
    } catch (e) {
        mvp.log("No beat found (" + e.message + "): the lines keep their own times");
    }
}

const track = mvp.call("add_track", { type: "video" }).index;
mvp.call("edit_track", { track: track, name: "Lyrics" });
const params = args.font ? { font: args.font } : {};
let made = 0;
for (let i = 0; i < lines.length; i++) {
    const l = lines[i];
    if (!l.words || l.time < 0)
        continue;
    const next = i + 1 < lines.length ? lines[i + 1].time : l.time + 4;
    const length = Math.min(8, next - l.time);
    if (length < 0.2)
        continue;
    mvp.call("add_text", { preset: args.style, text: l.words, at: l.time, length: length, track: track, params: params });
    made++;
}
mvp.log("Lyric Video: " + made + " lines on the Lyrics track");
