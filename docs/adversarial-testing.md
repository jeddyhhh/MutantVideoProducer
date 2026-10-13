# Adversarial hardening tests

These tests supplement the existing damaged-media fuzzing and editor checks with structured failures and reproducible seeds. They use generated fixtures, never the user's documentary projects.

## Core campaign

Build `mvp_adversarial_test`, then run:

```
build/Release/bin/mvp_adversarial_test.exe creative-test/adversarial/run 13371337 2000
```

Arguments are output directory, seed and iteration count (1–5000). Each case runs in a separate child process with a 120-second deadline. A crashed or stalled child is recorded and later cases continue. The directory retains the last fixture, per-case logs, and `summary.json`; repeat a single failure with `--case CASE DIRECTORY SEED ITERATIONS`.

Cases cover transactional rejection of unsupported versions and ambiguous media IDs; importing after loading offline media; malformed, duplicate and extreme keyframes; the exact nested-sequence boundary; preservation of ducking and caption ownership through ID repair; repeated load-like resets and undo/redo while retaining immutable snapshots; and cyclic animation variables followed by valid evaluation.

The media case also requires importing the same offline path twice to deduplicate correctly. The reference case repairs self-ducking and verifies the resulting project remains identical after saving and loading.

## Raw MCP transport campaign

In PowerShell:

```
$env:MVP_ADVERSARIAL_TRANSPORT_ONLY='1'
build/Release/bin/MutantVideoProducer.exe --editor-test creative-test/adversarial/transport.log
Remove-Item Env:MVP_ADVERSARIAL_TRANSPORT_ONLY
```

The existing editor-test mode uses its own preferences and autosave directory. The transport test selects an ephemeral localhost port and sends fragmented requests, conflicting lengths, unsupported transfer encodings, oversized headers, malformed request lines, seeded header mutations, pipelined messages, 300 abandoned requests and a 140-client saturation burst. It also checks request deadlines and recovery after invalid clients. Ordinary supported HTTP requests must still work after every mutation.

Run the normal editor, motion, story and original hardening suites after fixes to check valid projects and normal editing behaviour.

## Issues reproduced and fixed

- Unsupported or malformed project versions were accepted. Loading now rejects them before changing the current project or undo history; legacy files without a version remain supported.
- Duplicate, invalid and overflowing media IDs could alias sources or damage future allocation. They now fail loading with a diagnostic.
- Distinct missing media paths could compare equal and share a media entry. Import deduplication now uses normalized paths, with canonical paths for existing files.
- Malformed, repeated and extreme keyframes survived parsing. Parameter and track-envelope parsing now share bounded timestamp validation and deterministic duplicate handling.
- Nested-sequence loading and validation disagreed at the depth boundary, and deeper contents could be silently discarded. The supported sixteen-level boundary is consistent, and deeper project files are rejected transactionally.
- ID repair broke ducking and narration ownership references. Unambiguous references now follow repaired IDs; invalid references are cleared. Clearing ducking also normalizes its inactive gain so save/reload stays stable.
- MCP accepted malformed or ambiguous HTTP framing, overflowing lengths and complete oversized headers. Parsing now rejects these requests, bounds connections and buffers, expires stalled partial requests, and honors requested connection closure on error responses.

These campaigns exercise generated projects and localhost requests. They supplement the existing codec and editor tests; they do not prove absence of every possible driver, plugin or media-decoder failure.
