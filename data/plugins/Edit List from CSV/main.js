// Edit List from CSV: file,in,out[,name] rows laid out one after another at the end of the edit.

const args = mvp.args;
const folder = args.csv.replace(/[\\/][^\\/]*$/, ""); // (the CSV's own folder)

function seconds(text) {
    const parts = String(text).trim().split(":").map(Number);
    if (!parts.length || parts.some(isNaN))
        return NaN;
    return parts.reduce((total, p) => total * 60 + p, 0);
}

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

// A file named in the list: as it is when it has a folder (C:\..., /..., or with a slash), else next to the CSV.
function fullPath(name) {
    return /^([a-zA-Z]:)?[\\/]/.test(name) ? name : folder + "/" + name;
}
const same = (a, b) => a.replace(/\\/g, "/").toLowerCase() === b.replace(/\\/g, "/").toLowerCase();

const shots = [];
for (const line of mvp.readText(args.csv).split(/\r?\n/)) {
    const c = cells(line);
    const from = seconds(c[1]), to = seconds(c[2]);
    if (c.length >= 3 && c[0] && !isNaN(from) && !isNaN(to)) { // (a header row has no times)
        if (to <= from)
            throw new Error(c[0] + ": the out time (" + c[2] + ") must be after the in time (" + c[1] + ")");
        shots.push({ path: fullPath(c[0]), from: from, to: to, name: c[3] || "" });
    }
}
if (!shots.length)
    throw new Error("no rows like \"interview.mp4,12.5,20\" in " + args.csv);

// Every file into Project Media at once; then each one's id by its path.
const paths = shots.map(s => s.path).filter((p, i, all) => all.indexOf(p) === i);
mvp.call("import_media", { paths: paths }); // (throws when none of them open; problems lists the ones that didn't)
const media = mvp.call("get_project").media;
for (const s of shots) {
    s.media = media.find(m => same(m.path, s.path));
    if (!s.media)
        throw new Error(s.path + " couldn't be imported");
    if (s.to > s.media.seconds + 0.05)
        throw new Error(s.path + " is only " + s.media.seconds.toFixed(2) + " seconds long (out: " + s.to + ")");
}

// At the end of the edit nothing is in the way, so each shot is placed there and trimmed; the next goes after it.
const firstVideo = mvp.call("get_project").tracks.findIndex(t => t.type === "video");
const start = mvp.call("get_project").duration;
let at = start;
for (const s of shots) {
    const place = { media_id: s.media.media_id, at: at };
    if (firstVideo >= 0)
        place.track = firstVideo; // (no video track yet: one is made)
    const ids = mvp.call("add_media_to_timeline", place).event_ids;
    const edit = { event_id: ids[0], source_in: s.from, length: s.to - s.from };
    if (s.name)
        edit.name = s.name;
    mvp.call("edit_event", edit); // (its sound, grouped with it, is trimmed the same)
    at += s.to - s.from + args.gap;
}
mvp.log("Edit list: " + shots.length + " shots from " + start.toFixed(1) + " s, " + (at - args.gap - start).toFixed(1) + " s long");
