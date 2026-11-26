// Podcast Clean-up: normalise the voices, duck the music under them, take out pauses, set the loudness.
// The voice tracks are every audio track with events except the music track.

const args = mvp.args;
let project = mvp.call("get_project");

let music = null;
if (args.music > 0) {
    music = project.tracks[args.music - 1];
    if (!music || music.type !== "audio")
        throw new Error("track " + args.music + " isn't an audio track");
}
const voices = project.tracks.filter(t => t.type === "audio" && (!music || t.index !== music.index) && t.events.length && !t.locked);
if (!voices.length)
    throw new Error("there are no voice tracks (audio tracks with events, other than the music)");

// 1. Every voice event's loudest moment to the same level (a Gain effect at the front of its chain).
const voiceEvents = [];
for (const t of voices)
    for (const e of t.events)
        voiceEvents.push(e.event_id);
mvp.call("normalize_audio", { event_ids: voiceEvents });

// 2. The music ducks (turns down) while the first voice track has sound.
if (music) {
    mvp.call("edit_track", { track: music.index, duck_under: voices[0].index, duck_amount: args.duck });
    if (voices.length > 1)
        mvp.log("The music ducks under \"" + voices[0].name + "\" only (a track ducks under one other)");
}

// 3. Pauses out of the whole edit (every unlocked track, so pictures stay in step).
let cut = 0;
if (args.silences) {
    const r = mvp.call("cut_time", { action: "silences", min_length: args.pause });
    cut = r.removed_seconds || r.seconds || 0;
}

// 4. The whole mix to the loudness target, with the true peaks kept under -1.5 dB.
const finish = mvp.call("audio_finishing", { action: "apply_gain", lufs: Number(args.target), true_peak: -1.5 });
const gain = finish.applied_gain_db !== undefined ? finish.applied_gain_db : 0;
mvp.log("Podcast cleaned: " + voiceEvents.length + " voice events evened out" + (music ? ", music ducked" : "") +
        (args.silences ? ", pauses out" + (cut ? " (" + cut.toFixed(1) + " s)" : "") : "") + ", master gain " + gain.toFixed(1) + " dB");
