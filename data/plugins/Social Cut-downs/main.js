// Social Cut-downs: reframe part of the edit for a vertical frame, queue its render, then put the edit back.
// The Render Queue keeps a copy of the project as it is when the job is added, so the edit can be put back at once:
// mvp.rollback() takes back the reframing; the frame size isn't part of undo, so it's set back by hand.

const args = mvp.args;
const from = args.start, to = args.start + Number(args.seconds);
const before = mvp.call("get_project");
if (from >= before.duration)
    throw new Error("the edit is only " + before.duration.toFixed(1) + " seconds long");

const ids = [];
for (const t of before.tracks)
    if (t.type === "video" && !t.locked)
        for (const e of t.events)
            if (e.media_id !== undefined && e.start < to && e.end > from)
                ids.push(e.event_id);
if (!ids.length)
    throw new Error("there's no footage between " + from + " and " + to + " seconds");

let path = args.output;
if (!/\.mp4$/i.test(path))
    path += ".mp4";

try {
    mvp.call("auto_reframe", { event_ids: ids, portrait: true });
    mvp.call("render_queue", { action: "add", path: path, from: from, to: Math.min(to, before.duration), preset: args.template });
    if (args.now)
        mvp.call("render_queue", { action: "start" });
} finally {
    // (whatever happened above, the edit goes back as it was)
    mvp.rollback();
    mvp.call("set_project_settings", { width: before.width, height: before.height, fps: before.fps });
}
mvp.log("Vertical cut-down queued: " + path + (args.now ? " (rendering)" : " (File > Render Queue to start it)"));
