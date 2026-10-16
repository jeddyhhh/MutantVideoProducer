# Contributing

Thanks for helping with Mutant Video Producer. This page is the short version; the
[developer guides](docs/developer/) have the details.

## Build and test

1. Build as described in [Building](docs/developer/building.md) (`build.bat Release` on Windows,
   `./build.sh Release` on Linux).
2. Make the test media once: `tests/make_test_media.sh deps/ffmpeg/bin/ffmpeg.exe testmedia`.
3. Run the tests:

   ```sh
   ctest --test-dir build/Release -LE gui --output-on-failure
   ```

   `-L gui` adds the Editor tests. They drive a hidden window and play sound. See
   [Testing](docs/developer/testing.md) for the suites that need your own files (captions, VST3,
   the Live tab, the monkey test). A new suite is one `mvp_test(...)` line in `CMakeLists.txt`.

GitHub Actions builds only `main` and version tags, so run the tests before you send a change.

## Code

- C++20, Qt 6, FFmpeg. `src/core` has no UI, `src/engine` plays and caches, `src/ui` is the Qt
  interface ([Architecture](docs/developer/architecture.md)).
- Follow `.clang-format`. Format the files you change a lot of (`clang-format -i <file>`, version
  19); leave the rest, so diffs stay readable. `.editorconfig` sets indentation for other files.
- Build without new warnings (MSVC `/W4`, GCC `-Wall -Wextra`). `-DMVP_WARNINGS_AS_ERRORS=ON`
  makes them errors.
- Prefer the smallest change that fixes the cause. Add or extend a test that fails without it.

## Docs and changelog

- User-visible changes update the [User Guide](docs/README.md) page for the feature, and get an
  entry under a topic heading at the top of [CHANGELOG.md](CHANGELOG.md), above the latest version.
- Third-party code or downloads need their licence noted (see the `NOTICE.md` files and
  [Building](docs/developer/building.md#licences)).

## Licence

Mutant Video Producer is GPL-3.0 ([LICENSE](LICENSE)). Contributions are accepted under the same
licence.
