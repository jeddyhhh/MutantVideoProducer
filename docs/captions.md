# Captions and Subtitles

Everything is under **Tools > Captions**.

## Auto Captions

**Auto Captions (Speech to Text)** (**Ctrl+Shift+T**) listens to the timeline on this computer with
[Whisper](https://github.com/ggml-org/whisper.cpp) and puts what is said on a new **Captions** track.
Nothing is sent anywhere.

1. Optionally select a stretch of time. Without a selection, the whole timeline is captioned.
2. Choose **Tools > Captions > Auto Captions**.
3. Set the options:
   - **Speech model**:
     - *Tiny* (78 MB, fastest),
     - *Base* (148 MB, good),
     - *Small* (488 MB, best, slower),
     - *Small English, marks who is speaking* (488 MB): English only; the second speaker's captions are yellow. A long
       turn is cut on words into captions no longer than the chosen caption length, each timed by its share of the words.

     A model is downloaded from huggingface.co the first time you use it, after asking. The offline
     release already has them all.
   - **Language**: *Detect it*, or one of 17 languages (English, Spanish, French, German, Italian,
     Portuguese, Dutch, Polish, Swedish, Russian, Ukrainian, Turkish, Arabic, Hindi, Japanese,
     Korean, Chinese).
   - **Style**: *Subtitles* (a line at the bottom), or *Kinetic Text* (the words pop on, paced to the speech).
   - **Caption length**: up to 32, 42, 60 or 80 characters.
   - **Translate into English**: captions in English, whatever the language spoken.
4. Click **Add Captions**.

Each caption starts and ends where the voice does. The captions are ordinary text events: edit the
words, restyle them, move or trim them like any title (see [Titles and Generators](titles-and-generators.md)).

**Speed**: Whisper runs on the graphics card (Vulkan) where there is one, and on the CPU otherwise.
The first run takes a little longer while the graphics driver prepares. As a guide, the Small
model captions a minute of speech in about 3 seconds on a recent graphics card, and about 15
seconds on the CPU.

## Generate Voice from Captions

**Tools > Captions > Generate Voice from Captions...** turns text and kinetic captions
into AI speech on a new **Voice** audio track. Generation runs locally on the CPU;
caption text is never uploaded. In the standard Windows build, first install
**Speech Generation** through **Tools > AI Components...** (also available from
Generate Voice). The app downloads its dependencies and private runtime from upstream sources; no separate Python
installation is needed. See [optional AI components](ai-components.md).

1. Choose the captions track, or **Selected captions only**. You can also limit it
   to captions starting inside the timeline time selection.
2. Choose a voice in **Speech & Voices**. The library has 28 English US and UK
   Kokoro voices, with creator, language and license information. **Download Selected
   Voice + Model** downloads the shared 92 MB model and a roughly 510 KB voice from
   a pinned Hugging Face revision and verifies both with SHA-256. Offline builds
   already include them. **Preview Voice** speaks a short sample.
3. Set speech rate and timing. **Fit caption timing** speeds long phrases up by at
   most 1.35× while preserving pitch. Short speech keeps its natural duration and
   leaves the remaining gap silent. **Natural duration** does not apply timing compression.
4. **Join adjacent short captions into sentences** avoids a separate robotic phrase
   for every few words. It stops at sentence punctuation, long pauses, overlaps,
   speaker changes and long passages.
5. Click **Generate Voice**. Progress and cancellation are available. **Ctrl+Z**
   removes the new track and its timing markers in one step.

Speech that still exceeds its caption window is marked **[Timing]**, with a timeline
marker. Later phrases move forward to prevent automatic crossfades from hiding
words. Review these markers and adjust captions or voice events as needed; generation
does not rewrite the original captions. Saving the project retains all generated audio.
Audio is stored in a `<project-name>-voice` folder beside saved projects, or in
`Documents/Mutant Video Producer Voice` for an unsaved project. Keep these files with your project;
undo removes the timeline edit but leaves generated media available in Project Media.

### RVC conversion

The **RVC Conversion** tab optionally converts Kokoro speech into an imported RVC
voice. RVC is a voice converter, so it still needs Kokoro to speak the caption text.

Install **RVC Conversion** through **Tools > AI Components...**. The app downloads
the RVC dependencies and private runtime from upstream sources, with CPU/CUDA choices; no separate
software installation is needed. **Download RVC Support Models** fetches the shared
HuBERT/ContentVec and RMVPE models (370 MB total) once, verifying their SHA-256
checksums. The offline build includes these models. Conversion then works offline;
CPU conversion can take longer than speech generation.

Use **Import RVC Voice...** to copy a `.pth` model and optional matching `.index`
into the local voice library. Import models from sources you trust. Set **Index
retrieval** to zero when no index is supplied. Controls include pitch in semitones,
retrieval strength, consonant protection, speaker ID, and RMVPE or Praat pitch
detection. RVC failures leave the timeline unchanged and show its diagnostic log.
The downloadable catalog currently contains Kokoro voices; online RVC catalogs and
Qwen speech are not part of this version.

## Generate dialogue from a script

**Tools > Captions > Generate Dialogue from a Script...** speaks a whole conversation at once. Type (or paste) the script
with one speech per line:

```
# the cabin, night
Gus: Did you hear that?
Mo: It was just the wind.
Gus: The wind doesn't laugh, Mo.
```

A line without `Name:` continues the speech above it; lines starting with `#` are notes and aren't spoken.
`(pause 2)` on a line of its own leaves two seconds of silence before the next speech (`(pause)`: one second). A
treatment in brackets at the start of a speech gives it a sound, and isn't spoken: `Gus: [radio] Come in, base.` The
treatments are `radio`, `phone`, `megaphone`, `echo`, `hall`, `dream` (reverb, pitched down a little) and `backwards`
(played in reverse; lip sync follows it). Several go together: `[radio, echo]`. Every
character in the script gets a row: choose their voice (different voices are picked for you, installed ones first),
their speed, and a pitch in semitones (a Pitch Shift on their track: +4 for a child, -4 for a giant). Set where the
dialogue starts (the cursor by default) and the pause between lines, then press **Generate Dialogue**.
**Exact mouth timing** (on when a Whisper model is installed) finds when each word is said in every line, so an animated
character's mouth shapes land on the words instead of being spread evenly over the line.

Each character gets their own audio track named after them, and the lines follow each other in script order with
natural timing. Missing voices are downloaded first. One **Undo** removes the whole dialogue. The voices you chose
are remembered for each name, so the next scene with the same characters sounds the same. For lip sync, bind each
character's track in Compose Graphics (see [Tutorial 17](tutorials/17-animated-short.md)).

## Import and export subtitle files

- **Import Subtitles (SRT / VTT)...** reads an SRT or WebVTT file onto a Captions track as text events.
- **Export Subtitles (SRT / VTT)...** writes the text events of a track as SRT or WebVTT: the track
  of the selected event, else the top track with text on it.

## Subtitle tracks in renders

Burned-in captions are part of the picture. To let viewers turn them on and off instead, tick
**Add the captions as a subtitle track** in [Render As](rendering.md). The captions are written as:

- timed text in MP4 and MOV,
- SRT in MKV,
- WebVTT in WebM.

## Try it

The [Captions and Subtitles](tutorials/08-captions.md) tutorial captions the narration in the
*audio-mixing* example project.
