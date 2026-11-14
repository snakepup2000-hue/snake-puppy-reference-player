# Building Snake Puppy Reference Player

## Toolchain

| Requirement | Details |
|---|---|
| macOS | 13.0+ deployment target (verified on macOS 26.2, Apple Silicon) |
| Xcode | full Xcode with the macOS SDK (Command Line Tools alone may lack a complete SDK) |
| CMake | ≥ 3.22 (verified with CMake 4.4.0) |
| C++ | C++17 |
| JUCE | 8.0.8, vendored in `JUCE/` (shallow clone) — nothing to install |
| Optional | `ffmpeg`/`ffprobe` and `sox` (fixture generation and audio verification) |

No third-party audio libraries are required. The app links the system SQLite
(`-lsqlite3`), Core Audio / AudioToolbox frameworks through JUCE, and JUCE's
bundled FLAC/MP3/Ogg/Vorbis decoders.

## Standard build

```bash
cd snake-puppy-reference-player
./scripts/build.sh
```

- Build directory: `build/` (override with `SP_BUILD_DIR`).
- Configuration: Release (override with `SP_CONFIG=Debug`).
- Output app:
  `build/SnakePuppyReferencePlayer_artefacts/Release/Snake Puppy Reference Player.app`
- Build target selection: `SP_TARGET=SnakePuppyTests ./scripts/build.sh`.

This project uses `GLOB CONFIGURE_DEPENDS`, so new source files are picked up
automatically. Do not edit `CMakeLists.txt` just to add files.

### SDK note for this machine

The installed CommandLineTools SDK is incomplete with the current linker, so the
build scripts export `SDKROOT` to the newest Xcode-bundled macOS SDK
(`/Applications/Xcode*.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX*.sdk`).
CMakeLists also prefers that SDK before `project()`. On a normal Xcode setup this
is unnecessary but harmless.

## Tests

```bash
./scripts/test.sh                 # builds + runs the full headless suite
./scripts/test.sh --filter Dsp    # one area
./scripts/test.sh --list          # list tests
SP_BUILD_DIR=build-x ./scripts/test.sh --junit results.xml
```

The suite (153 tests) covers DSP measured responses, decoding of every supported
format (incl. DSD), the playback/gapless pipeline, library scanning/persistence,
metadata writing with audio-payload hashes, playlists and playlist IO, genre
profiles/analysis/learning, device/signal-path logic, dither, network output
(against an in-process mock DLNA renderer) and UI smoke tests.

### Audio fixtures

Some tests use generated audio fixtures:

```bash
./scripts/make-fixtures.sh
```

This writes `Tests/Fixtures/` (not committed): nested albums, FLAC 24/96, FLAC
16/44.1, MP3, WAV 24/48, AIFF, ALAC, AAC, OGG (all with tags and embedded
artwork), DSD64 DSF/DFF, duplicates, broken files, playlists, an impulse
response and test tones. `scripts/make_dsd_fixtures.py` generates the DSD files.
Tests copy fixtures before write operations; originals are never modified.

## Packaging

```bash
./scripts/package.sh              # release build + tests + ad-hoc signing + dist/
SP_UNIVERSAL=1 ./scripts/package.sh   # universal arm64 + x86_64
SP_SKIP_TESTS=1 ./scripts/package.sh  # skip the test run
```

Outputs:

```
dist/Snake Puppy Reference Player.app
dist/SnakePuppyReferencePlayer-<version>-macOS.zip
dist/SnakePuppyReferencePlayer-<version>-macOS.zip.sha256
dist/README.md, TEST_REPORT.md, LIMITATIONS.md
dist/test-results/tests.xml
```

The bundle is **ad-hoc signed** (`codesign --deep --sign -`), which is sufficient
to run locally (Gatekeeper may still require right-click → Open, and the app is
not notarized). For distribution to other machines, sign with a Developer ID and
notarize with `notarytool`.

## Developer / verification hooks

`Source/Main.cpp` (and the Lead's environment hooks in `AppContext`) accept
flags used by the automated visual/functional verification rigs. They call the
same public APIs as the UI; they are not required for normal use.

```
--import <folder>              import a folder on startup
--import-playlist <file>       import an M3U/M3U8/PLS playlist
--play                          play the first queued/library track when ready
--view songs|albums|queue|health|playlist
--volume <dB>                  set master volume
--bass <dB>                    set the dedicated bass control
--eq-mode manual|autogenre|intelligent
--profile <profileId>          select a profile
--reference                    start in Reference Mode
--output-device <substring>    select the first matching output device
--hog on|off                   exclusive (hog) device mode
--native-rate on|off           follow each file's sample rate
--buffer <frames>              device buffer size
--dither on|off                TPDF dither when reducing bit depth
--record <path.wav>            record the engine's post-volume output (verification)
--status-log <path.jsonl>      append a JSON engine status line every 500 ms
--dsp-studio / --settings / --full-player   open the respective window
```

Environment hooks (useful when launching the binary directly):
`SP_IMPORT_ON_START`, `SP_PLAY_ON_START`, `SP_DEBUG_RECORD`, `SP_STATUS_LOG`,
`SP_REFERENCE_MODE`, `SP_SET_EQ_MODE`, `SP_SET_BASS`, `SP_SET_PROFILE`.

**Important when verifying on this machine:** the DSH shell sandbox blocks the
GUI app's Core Audio device access and `~/Library` writes for direct child
launches. The app then honestly reports "No audio output device is available".
Launch unsandboxed (Finder/`open`, or a shell command granted full access) for
real playback verification.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `clang: error: linker command failed ... unknown architecture` | incomplete CommandLineTools SDK — the build scripts select Xcode's SDK; ensure Xcode is installed and `SDKROOT` points at its `MacOSX*.sdk` |
| App shows "No audio output device is available" | launched from a sandboxed shell; launch from Finder or approve a full-access shell launch |
| First scan of large 24/192 FLAC folders is slow | metadata reads are I/O-bound; the library snapshot is served from the DB while the background rescan runs |
| `open` reuses another instance | the app now allows multiple instances so verification copies can coexist; use the window of the instance you launched |
| Test fixture generation fails on `vorbis` | this machine's ffmpeg builds OGG Vorbis via its native encoder; the script handles that |

## Licensing note

JUCE 8 is dual-licensed (AGPLv3 or a commercial JUCE licence). This project
vendors JUCE 8.0.8 for local use; if you distribute binaries you are responsible
for complying with JUCE's licence terms. The rest of the source in this
repository is original work for this project.

## Clean rebuild

```bash
rm -rf build dist
./scripts/build.sh
./scripts/test.sh
```
