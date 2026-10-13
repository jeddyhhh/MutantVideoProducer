# Story, Narration and Sequences

Open the **Story** tab beside History in the Editor's lower panel. Add a chapter with a name, script, narration window and listening-only duration. Choose footage or music from the media pool, or start with a title placeholder. Source audio plays quietly under the planned narration and rises for the listening section. Short footage loops when required.

For an existing edit, select a timeline region and click **Chapter from region**. Mutant Video Producer splits clips at the boundaries and assigns the pieces to that chapter. **Assign selected clips** adds other material within the range, including linked video/audio. Draft scripts stay available when switching chapters; **Save changes** commits a draft to the project. Duration changes move following clips, chapters and markers together. **Move up/down** swaps adjacent chapters; **Delete** removes the assigned range and closes its gap. Assign all material in the affected ranges first.

**Generate chapter voice** opens Narration Studio for the script. It keeps the listening section free of narration; if speech will not fit, expand the narration window or shorten the script. Audio from an unsuccessful fit is retained in the profile's Voice folder.

## Narration Studio

Choose **Tools > Narration Studio**, with an existing narration or text event selected to work on that sentence. You can also type a new paragraph and choose its start/window. Generate another take without rebuilding the rest of the voice track. Earlier audio remains available under **Alternate takes**; audition it and choose **Use selected take**. Replacing a sentence and its captions is undoable.

The pronunciation dictionary accepts one `word = spoken form` per line, for example `Necrophagist = neck roff a gist`. Add `[pause=0.5]` in the script or set pauses before, after and between sentences. These create silence in the audio. Pronunciation and narration defaults are saved in the project.

**Align captions to the actual generated speech** uses an installed local Whisper model. Caption timing follows recognised words in the generated audio; review the text for unusual names. The word timings are retained with each take. Install speech models through the existing automatic captions controls if none is available.

Voices and RVC models still use **Tools > Captions > Generate Voice** for downloads/imports. Narration Studio can reuse the saved RVC voice for conversion. Processing runs locally through Mutant Video Producer's bundled workers.

## Compound sequences

Select related footage, music, captions and graphics, then choose **Tools > Create Compound Sequence** (`Ctrl+Shift+G`). A selected timeline region can also be packed. The tracks become a named picture/audio pair with editable content inside. The source media stays in the project pool.

Double-click a compound, or choose **Tools > Edit Compound Sequence**, to open its own preview and timeline. Split, trim, move, import, add effects, and open deeper compounds there. **Save** commits everything as one parent undo step; **Cancel** discards the embedded edits. When a full-range sequence changes duration, the ripple option moves following material and chapter starts. Trimmed or speed-adjusted containers retain their source range.

**Unpack Compound Sequence** restores the editable source tracks. Reset container transformations and track controls first; unpacking refuses changes that would lose those treatments. Include overlapping visual layers and both sides of audio ducking when packing; turn off solos and use Normal track blending. The program explains these requirements when they apply. To expand a packed chapter through Story, unpack it, change the chapter duration, and pack it again.

## Director's Dice

Select footage or image clips and choose **Tools > Director's Dice**. Pick **Measured**, **Restless** or **Riot**, adjust intensity, then **Roll / Preview**. Reroll to explore another direction. A seed reproduces the same variation for the same selected clips.

Dice adds camera motion and, in stronger styles, brief RGB accents and glitch or slice entrances. Sound, script and clip placement remain intact. **Cancel** restores the original preview; **Apply** records one undo step. Captions and generated titles are kept intact.

All of these features are also available through [AI control](ai-control.md).
