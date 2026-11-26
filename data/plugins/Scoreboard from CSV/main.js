// Scoreboard from CSV: one title per row, from its time until the next row's (the last one until the end).

const args = mvp.args;

// "75", "1:15", "1:01:15" or "75.5" as seconds; NaN when it isn't a time.
function seconds(text) {
    const parts = String(text).trim().split(":").map(Number);
    if (!parts.length || parts.some(isNaN))
        return NaN;
    return parts.reduce((total, p) => total * 60 + p, 0);
}

// Splits a CSV line, with "quoted, fields" kept whole.
function cells(line) {
    const out = [];
    let cell = "", quoted = false;
    for (let i = 0; i < line.length; i++) {
        const ch = line[i];
        if (ch === '"' && quoted && line[i + 1] === '"') {
            cell += '"';
            i++;
        } else if (ch === '"')
            quoted = !quoted;
        else if (ch === "," && !quoted) {
            out.push(cell.trim());
            cell = "";
        } else
            cell += ch;
    }
    out.push(cell.trim());
    return out;
}

const rows = [];
for (const line of mvp.readText(args.csv).split(/\r?\n/)) {
    const c = cells(line);
    const t = seconds(c[0]);
    if (line.trim() && !isNaN(t) && c.length >= 3) // (a header row, or a blank one, has no time)
        rows.push({ time: t, home: c[1], away: c[2], note: c[3] || "" });
}
if (!rows.length)
    throw new Error("no rows like \"12:30,1,0,Goal!\" in " + args.csv);
rows.sort((a, b) => a.time - b.time);

const end = Math.max(mvp.call("get_project").duration, rows[rows.length - 1].time + 10);
const track = mvp.call("add_track", { type: "video" }).index;
mvp.call("edit_track", { track: track, name: "Scoreboard" });
for (let i = 0; i < rows.length; i++) {
    const r = rows[i];
    const until = i + 1 < rows.length ? rows[i + 1].time : end;
    if (until - r.time < 0.1)
        continue;
    const words = args.home + "  " + r.home + " - " + r.away + "  " + args.away + (r.note ? "\n" + r.note : "");
    mvp.call("add_text", { preset: "title", text: words, at: r.time, length: until - r.time, track: track,
                           params: { size: 4.5, y: args.position === "top" ? 0.09 : 0.88, color: args.color, box: 0.55 } });
}
mvp.log("Scoreboard: " + rows.length + " scores");
