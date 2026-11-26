// Restyle Captions: the same text settings on every caption event of the chosen tracks.
// A caption is a text event (generator "text" or "kinetic_text"); edit_event's params change its settings.

const args = mvp.args;
const params = { color: args.color };
if (args.font)
    params.font = args.font;
if (args.size > 0)
    params.size = args.size;
if (args.box !== "keep")
    params.box = { none: 0, light: 0.45, dark: 0.85 }[args.box];
if (args.position !== "keep")
    params.y = args.position === "top" ? 0.1 : 0.9;

const want = args.tracks.trim().toLowerCase();
const tracks = mvp.call("get_project").tracks.filter(t => t.type !== "audio" && !t.locked && t.name.toLowerCase().indexOf(want) >= 0);
if (!tracks.length)
    throw new Error("no unlocked track has \"" + args.tracks + "\" in its name");

let changed = 0, skipped = 0;
for (const t of tracks)
    for (const e of t.events) {
        if (e.generator !== "text" && e.generator !== "kinetic_text") {
            skipped++;
            continue;
        }
        try {
            mvp.call("edit_event", { event_id: e.event_id, params: params });
            changed++;
        } catch (err) { // (a setting this kind of caption doesn't have)
            skipped++;
            mvp.log("Event " + e.event_id + ": " + err.message);
        }
    }
if (!changed)
    throw new Error("there were no captions to restyle on " + tracks.map(t => "\"" + t.name + "\"").join(", "));
mvp.log("Restyled " + changed + " captions" + (skipped ? " (" + skipped + " other events left alone)" : ""));
