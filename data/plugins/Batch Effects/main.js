// Batch Effects: one effect or preset onto many events, or every effect off them.
// get_project marks the events selected on the timeline with "selected": true.

const args = mvp.args;
const project = mvp.call("get_project");

let events = [];
if (args.scope === "selected") {
    for (const t of project.tracks)
        for (const e of t.events)
            if (e.selected && !t.locked)
                events.push({ event: e, audio: t.type === "audio" });
    if (!events.length)
        throw new Error("no events are selected (click them on the timeline first, or choose a track)");
} else {
    const t = project.tracks[args.track - 1];
    if (!t)
        throw new Error("there's no track " + args.track + " (the edit has " + project.tracks.length + ")");
    if (t.locked)
        throw new Error("track " + args.track + " is locked");
    events = t.events.map(e => ({ event: e, audio: t.type === "audio" }));
    if (!events.length)
        throw new Error("track " + args.track + " has no events");
}

// The best match for a name: the same name (any case), else the first that contains it.
function pick(list, name, what) {
    const lower = name.trim().toLowerCase();
    const found = list.find(x => x.name.toLowerCase() === lower) || list.find(x => x.name.toLowerCase().indexOf(lower) >= 0);
    if (!found)
        throw new Error("no " + what + " called \"" + name + "\"");
    return found;
}

let changed = 0;
if (args.action === "strip") {
    for (const { event } of events)
        for (const fx of event.effects || []) {
            mvp.call("remove_effect", { effect_id: fx.effect_id });
            changed++;
        }
    mvp.log("Removed " + changed + " effects from " + events.length + " events");
} else if (args.action === "preset") {
    for (const audio of [false, true]) {
        const these = events.filter(x => x.audio === audio);
        if (!these.length)
            continue;
        const preset = pick(mvp.call("presets", { action: "list", search: args.name, audio: audio }).presets, args.name,
                            audio ? "audio preset" : "preset");
        for (const { event } of these) {
            mvp.call("presets", { action: "apply", preset: preset.id, target: "event", id: event.event_id });
            changed++;
        }
    }
    mvp.log("Applied \"" + args.name + "\" to " + changed + " events");
} else {
    for (const audio of [false, true]) {
        const these = events.filter(x => x.audio === audio);
        if (!these.length)
            continue;
        const effect = pick(mvp.call("list_effects", { kind: audio ? "audio" : "video", search: args.name }).effects, args.name,
                            audio ? "audio effect" : "video effect");
        for (const { event } of these) {
            mvp.call("add_effect", { target: "event", id: event.event_id, effect: effect.id });
            changed++;
        }
    }
    mvp.log("Added \"" + args.name + "\" to " + changed + " events");
}
