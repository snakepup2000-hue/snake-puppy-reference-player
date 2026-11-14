# Contributing

Thanks for taking a look at Snake Puppy Reference Player.

## Before you start

- macOS 13+ with Xcode command line tools and CMake ≥ 3.22.
- The JUCE tree is vendored under `JUCE/`; do not replace it with a package
  manager fetch.
- Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the threading model and
  [LIMITATIONS.md](LIMITATIONS.md) for the intended capability boundaries. A
  change that quietly moves one of those boundaries is a bug.

## Build

```bash
./scripts/build.sh
open "build/SnakePuppyReferencePlayer_artefacts/Release/Snake Puppy Reference Player.app"
```

## Tests

The suite needs generated audio fixtures and `ffmpeg`:

```bash
brew install ffmpeg
./scripts/make-fixtures.sh
./scripts/test.sh
```

`scripts/test.sh` writes a JUnit report to `build/reports/tests.xml`. The
fixtures are reproducible and intentionally git-ignored; they are generated from
`ffmpeg` lavfi sources.

## Code style

- C++17, no exceptions on the audio path, no allocations or locks in the audio
  callback.
- Keep `Source/` free of non-ASCII literals; the UI stack renders ASCII safely.
- DSP changes need a measurement or a test, not just "it sounds fine".
- UI changes should keep every control connected to real behaviour. Stub
  controls are not accepted.

## Pull requests

- Describe what you measured or verified and how.
- Include the relevant test output (`scripts/test.sh`) and screenshots for UI
  changes.
- Update `TEST_REPORT.md` / `LIMITATIONS.md` when the verified behaviour
  changes. Do not overstate what was tested.

## Reporting issues

Please include the macOS version, CPU architecture, output device, source
format/rate, and the app log from
`~/Library/Application Support/Snake Puppy Reference Player/snake-puppy.log`.
