// Shot List Export: get_project as a CSV (writeText may write only the file the save field names).

const args = mvp.args;
const project = mvp.call("get_project");
const fps = project.fps || 30;

function time(seconds) {
    if (!args.timecode)
        return seconds.toFixed(3);
    const frames = Math.round(seconds * fps);
    const f = frames % Math.round(fps), s = Math.floor(frames / Math.round(fps));
    const two = n => (n < 10 ? "0" : "") + n;
    return Math.floor(s / 3600) + ":" + two(Math.floor(s / 60) % 60) + ":" + two(s % 60) + "." + two(f);
}
// A CSV cell: quoted when it has a comma, a quote or a line break.
const cell = v => /[",\n\r]/.test(String(v)) ? '"' + String(v).replace(/"/g, '""') + '"' : String(v);

const files = {};
for (const m of project.media)
    files[m.media_id] = m.path;

const rows = [["track", "kind", "start", "end", "length", "file", "file_in", "name_or_text", "effects", "event_id"]];
project.tracks.forEach((t, i) => {
    const audio = t.type === "audio";
    if ((args.kind === "video" && audio) || (args.kind === "audio" && !audio))
        return;
    for (const e of t.events)
        rows.push([(i + 1) + " " + t.name, t.type, time(e.start), time(e.end), time(e.length),
                   e.media_id !== undefined ? files[e.media_id] || "" : "(" + (e.generator || "generated") + ")",
                   e.source_in !== undefined ? time(e.source_in) : "", e.text || e.name || "",
                   (e.effects || []).map(fx => fx.name || fx.type).join("; "), e.event_id]);
});
let path = args.output;
if (!/\.csv$/i.test(path))
    throw new Error("save it as a .csv file");
mvp.writeText(path, rows.map(r => r.map(cell).join(",")).join("\r\n") + "\r\n");
mvp.log("Shot list: " + (rows.length - 1) + " events in " + path);
