# Profiles and home folders

Open **File > Profiles & Home Folders...**, choose **Create a new profile...**,
enter a name, select its home folder, and click **Save & Use Profile**. The same
dialog selects an existing profile or changes its name/home. The active profile
appears in the window title and is remembered next time Mutant Video Producer starts.

Each home contains:

| Folder | Content |
|---|---|
| Projects | New projects, named in the New Project dialog |
| YouTube | Videos/audio created by the YouTube downloader |
| Live | Live recordings, replays and screenshots |
| Voice | Generated caption speech and RVC audio |
| Media | Default folder for media imports and exported frames |
| Exports | Default destination for rendered videos |
| Voice Assets / Speech Models | Downloaded speech voices, RVC models and caption models |
| Pad Sets | Saved live pad sets |
| Autosave | Recovery copies for the profile |

**Profile Media** shows the profile's shared media library, alongside media from
the current project. **From YouTube** includes downloads used by other projects
in that profile; **From Live** browses its recordings. Drag a shared clip onto the
timeline, double-click it, or use **Add to Project Media** to import its reference.
Search, folder and rating filters also work on shared entries. Importing shared
media restores its origin, folder, rating and in/out marks.
Open an existing project while the profile is active to add its media references
to that profile's library.

Imported files are referenced in `montage-library.json` in the profile home;
Mutant Video Producer does not copy existing external files. They must remain available at their
original location. Removing a clip from a project does not delete it from the
shared library. Files that have disappeared are shown as offline.

Recent projects and remembered file-dialog locations are separate per profile.
Switching profiles resolves unsaved changes and starts an empty project. Finish
voice generation and stop recording before switching. Existing files are not moved
when changing a home folder: choose the old home to access its library again.
**Default (existing folders)** uses Mutant Video Producer's previous storage locations.

You can still explicitly open/save files outside the profile home. Executables,
plugins and temporary decoding/proxy caches remain application-wide.
