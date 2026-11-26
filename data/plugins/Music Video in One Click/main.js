// Music Video in One Click: Detect Beats, Make Beat Montage and Director's Dice in a row.
// Detect Beats leaves a marker on every beat; Beat Montage cuts on markers. The markers that were there before are
// put back afterwards, so the timeline ends with the user's own markers only.

const args = mvp.args;
const range = args.to > args.from ? { from: args.from, to: args.to } : { from: args.from };

const project = mvp.call("get_project");
if (!project.media.some(m => m.width && !m.still))
    throw new Error("there's no footage in Project Media to cut (import some videos first)");
const keep = project.markers.filter(m => !m.length); // (regions are never touched)

const beat = mvp.call("detect_beats", Object.assign({ every: 1 }, range)); // throws when there's no music there
const montage = mvp.call("beat_montage", Object.assign({ every: Number(args.every), seed: args.seed }, range));
mvp.call("directors_dice", { event_ids: montage.event_ids, style: args.style, seed: args.seed, intensity: args.intensity });

mvp.call("remove_markers", {});
for (const m of keep)
    mvp.call("add_marker", m.name ? { time: m.time, name: m.name } : { time: m.time });

mvp.log("Music video: " + montage.cuts + " cuts at " + Math.round(beat.bpm) + " BPM, " + args.style);
