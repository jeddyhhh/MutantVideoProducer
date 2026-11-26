// Brand Kit: intro at the start, outro at the end, logo in a corner in between.

const args = mvp.args;
if (!args.logo && !args.intro && !args.outro)
    throw new Error("choose a logo, an intro or an outro");

// The media id of a file (importing it, if it isn't in Project Media yet).
function mediaOf(path) {
    const id = mvp.call("import_media", { paths: [path] }).media_ids[0];
    return mvp.call("get_project").media.find(m => m.media_id === id);
}

// Places a file at `at` on `track` for `length` seconds, with its sound. Appending at the very end of the edit, nothing
// can be in its way; elsewhere, make room first (cut_time insert).
function place(media, at, track, length) {
    const ids = mvp.call("add_media_to_timeline", { media_id: media.media_id, at: at, track: track }).event_ids;
    mvp.call("edit_event", { event_id: ids[0], length: length });
    return ids[0];
}

const firstVideo = () => mvp.call("get_project").tracks.find(t => t.type === "video").index;
let introLength = 0, outroLength = 0;

if (args.intro) {
    const intro = mediaOf(args.intro);
    introLength = intro.seconds;
    // Room at the start: everything (on every unlocked track) moves along by the intro's length.
    mvp.call("cut_time", { action: "insert", at: 0, length: introLength });
    place(intro, 0, firstVideo(), introLength);
}
if (args.outro) {
    const outro = mediaOf(args.outro);
    outroLength = outro.seconds;
    place(outro, mvp.call("get_project").duration, firstVideo(), outroLength);
}
if (args.logo) {
    const project = mvp.call("get_project");
    const logo = mediaOf(args.logo);
    const from = introLength, to = project.duration - outroLength;
    if (to - from < 0.5)
        throw new Error("there's no edit between the intro and the outro to put the logo on");
    const track = mvp.call("add_track", { type: "video" }).index; // (on top)
    mvp.call("edit_track", { track: track, name: "Logo" });
    const event = place(logo, from, track, to - from);
    // A picture fills the frame's height (fit); Transform scales it and moves it by fractions of the frame.
    const scale = args.size / 100;
    const aspect = (logo.width && logo.height) ? logo.width / logo.height : 1;
    const halfWidth = scale * aspect * project.height / project.width / 2, halfHeight = scale / 2, margin = 0.04;
    const right = args.corner.indexOf("right") >= 0, bottom = args.corner.indexOf("bottom") >= 0;
    mvp.call("add_effect", { target: "event", id: event, effect: "transform", params: {
        scale: scale,
        x: (right ? 1 : -1) * (0.5 - halfWidth - margin * project.height / project.width),
        y: (bottom ? 1 : -1) * (0.5 - halfHeight - margin),
        opacity: args.opacity } });
}
mvp.log("Brand kit: " + [args.intro ? "intro" : "", args.outro ? "outro" : "", args.logo ? "logo" : ""].filter(x => x).join(", "));
