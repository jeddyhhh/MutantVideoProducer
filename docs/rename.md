# Rename compatibility

Mutant Video Producer was previously named Montage. The source, releases, issue tracker
and update checker now use [MutantVideoProducer on GitHub](https://github.com/jeddyhhh/MutantVideoProducer).

The Windows program is `MutantVideoProducer.exe`; Linux uses `MutantVideoProducer`.
MCP advertises the server as `mvp`. Update any MCP client's executable path to the new
file; its tools, arguments and HTTP port are unchanged. The configuration label is yours
to choose, so an existing client label also works.

Projects retain the `.mtg` extension and `montage-project` disk identifier. Saved effect
presets retain `montage-preset`, and shared profile libraries retain `montage-library.json`.
These are compatibility identifiers, not the application's displayed name. Existing
projects, collected folders and profile homes do not need conversion.

On the first normal start, missing preferences are imported from the previous installation.
New preferences take precedence, and the previous settings are left intact. This preserves
profile IDs and home folders, recent projects, layouts, voice settings and MCP options.
Test and screenshot sessions remain isolated from this migration.

Existing application data, caches, downloaded speech models, presets and recovery files
are used in place when their previous directories exist. Existing default Live, YouTube,
Voice, Graphics and Examples folders are also retained. New installations use the new
name. Selected profile homes and absolute project media paths remain unchanged.

New development environment variables use the `MVP_` prefix. The application's GUI and
stdio bridge accept old `MONTAGE_` variables as aliases; an explicitly set `MVP_` value
takes precedence. CMake options and test executable names use `MVP_` and `mvp_` respectively.
The build can reuse portable voice workers built before the rename and deploy them with
the new worker filenames.

The installer retains its upgrade identity, so it recognizes an existing installation.
Its product name, shortcuts and `.mtg` association point to Mutant Video Producer.
The internal Windows association identifier is retained so existing default-app choices
continue to open the renamed executable.
