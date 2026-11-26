// Chapters from a List: "00:00 Intro" lines become named markers (Deliver turns markers into YouTube chapters).
// A line is a time (m:ss, mm:ss or h:mm:ss) and a name, in either order: "02:15 The build", "The build - 2:15",
// "[1:02:03] Part three". Lines without a time are skipped.

const timePattern = /\[?(?:(\d+):)?(\d{1,2}):(\d{2}(?:\.\d+)?)\]?/;

const chapters = [];
for (const line of mvp.readText(mvp.args.list).split(/\r?\n/)) {
    const m = timePattern.exec(line);
    if (!m)
        continue;
    const seconds = Number(m[1] || 0) * 3600 + Number(m[2]) * 60 + Number(m[3]);
    // The name is what's left, without the dashes or colons round it.
    const name = line.replace(m[0], "").replace(/^[\s\-–—:|]+|[\s\-–—:|]+$/g, "").trim();
    chapters.push({ time: seconds, name: name || "Chapter " + (chapters.length + 1) });
}
if (!chapters.length)
    throw new Error("no lines with a time in " + mvp.args.list + " (they look like \"02:15 The build\")");
chapters.sort((a, b) => a.time - b.time);

if (mvp.args.replace)
    mvp.call("remove_markers", {}); // (markers only: regions stay)
for (const c of chapters)
    mvp.call("add_marker", { time: c.time, name: c.name });

// YouTube shows chapters only when the first is at 0:00, there are at least three, and each lasts 10 seconds or more.
const notes = [];
if (chapters[0].time !== 0)
    notes.push("the first isn't at 0:00");
if (chapters.length < 3)
    notes.push("there are fewer than three");
if (chapters.some((c, i) => i > 0 && c.time - chapters[i - 1].time < 10))
    notes.push("some are under 10 seconds apart");
mvp.log(chapters.length + " chapter markers" + (notes.length ? " (YouTube won't show them: " + notes.join(", ") + ")" : ""));
