// Countdown and Timer: one title a second on a new track.

const args = mvp.args;
const start = args.at >= 0 ? args.at : mvp.call("get_project").cursor;
const seconds = Math.round(args.seconds);

// A new video track goes on top (index 0); every other track moves down one.
const track = mvp.call("add_track", { type: "video" }).index;
mvp.call("edit_track", { track: track, name: args.mode === "countdown" ? "Countdown" : "Timer" });

function title(words, at, length) {
    mvp.call("add_text", { preset: args.style, text: words, at: at, length: length, track: track, params: { color: args.color } });
}

const two = n => (n < 10 ? "0" : "") + n;
for (let i = 0; i < seconds; i++) {
    if (args.mode === "countdown")
        title(String(seconds - i), start + i, 1);
    else {
        const h = Math.floor(i / 3600), m = Math.floor(i / 60) % 60, s = i % 60;
        title((h ? h + ":" : "") + two(m) + ":" + two(s), start + i, 1);
    }
}
if (args.mode === "countdown" && args.last)
    title(args.last, start + seconds, 1.5);
mvp.log((args.mode === "countdown" ? "Countdown from " : "Clock for ") + seconds + " seconds at " + start.toFixed(2) + " s");
