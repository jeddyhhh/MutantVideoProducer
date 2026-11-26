// Photo Slideshow: the folder's pictures on two new tracks, alternating, so each crossfades into the next.
// The upper track's pictures fade in and out over the lower track's: that makes every change a crossfade.

const args = mvp.args;
const pictures = mvp.listFolder(args.folder, ["*.jpg", "*.jpeg", "*.png", "*.webp", "*.bmp", "*.tif", "*.tiff", "*.gif"]);
if (!pictures.length)
    throw new Error("there are no pictures in " + args.folder);

const ids = mvp.call("import_media", { paths: pictures }).media_ids;
const start = mvp.call("get_project").duration;

// Two new video tracks on top. A new track goes in at index 0 and pushes the others down, so tracks are looked up by
// name each time they're needed.
mvp.call("edit_track", { track: mvp.call("add_track", { type: "video" }).index, name: "Slideshow B" });
mvp.call("edit_track", { track: mvp.call("add_track", { type: "video" }).index, name: "Slideshow A" });
const trackNamed = name => mvp.call("get_project").tracks.find(t => t.name === name).index;

const fade = Math.min(args.fade, args.seconds / 2);
for (let i = 0; i < ids.length; i++) {
    const upper = i % 2 === 1;
    const at = start + i * args.seconds;
    const length = args.seconds + (i + 1 < ids.length ? fade : 0); // (overlapping the next one by the crossfade)
    const event = mvp.call("add_media_to_timeline", { media_id: ids[i], at: at, track: trackNamed(upper ? "Slideshow A" : "Slideshow B") }).event_ids[0];
    const edit = { event_id: event, length: length };
    if (upper && fade > 0) {
        edit.fade_in = fade;
        if (i + 1 < ids.length)
            edit.fade_out = fade;
    }
    if (args.zoom) // (in and out by turns, a little off centre)
        edit.pan_crop = { mode: "Fill", zoom: { keyframes: i % 2 ? [[0, 1.12], [length, 1.0]] : [[0, 1.0], [length, 1.12]] },
                          centerX: 0.5 + (i % 3 - 1) * 0.04, centerY: 0.5 };
    mvp.call("edit_event", edit);
}
const total = ids.length * args.seconds;

if (args.music) {
    const song = mvp.call("import_media", { paths: [args.music] }).media_ids[0];
    const events = mvp.call("add_media_to_timeline", { media_id: song, at: start }).event_ids;
    mvp.call("edit_event", { event_id: events[0], length: total, loop: true, fade_out: Math.min(3, total / 4) });
}
mvp.log("Slideshow: " + ids.length + " pictures, " + total.toFixed(0) + " seconds");
