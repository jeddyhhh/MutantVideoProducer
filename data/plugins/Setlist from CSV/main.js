// Setlist from CSV: song,bpm,scene,notes,pad set rows become the Live tab's setlist (live_setlist replaces it).

const args = mvp.args;

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

const songs = [];
for (const line of mvp.readText(args.csv).split(/\r?\n/)) {
    const c = cells(line);
    if (!line.trim() || !c[0])
        continue;
    const bpm = Number(c[1] || 0), scene = Number(c[2] || 0);
    if (isNaN(bpm) || isNaN(scene)) { // (a header row: "song,bpm,scene")
        if (songs.length)
            throw new Error("\"" + line + "\": the BPM and scene must be numbers");
        continue;
    }
    songs.push({ name: c[0], bpm: bpm, scene: scene, notes: c[3] || "", pad_set: c[4] || "", lock_bpm: args.lock });
}
if (!songs.length)
    throw new Error("no songs like \"Opener,128,1\" in " + args.csv);

const result = mvp.call("live_setlist", { songs: songs });
mvp.log("Setlist: " + result.songs.length + " songs (Page Down on the Live tab plays the next)");
