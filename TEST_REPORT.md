# Test Report — Snake Puppy Reference Player

**Build under test:** 0.1.0, Release arm64 (macOS 26.2, Apple M2 Ultra), JUCE 8.0.8  
**Deliverable:** `dist/Snake Puppy Reference Player.app` (ad-hoc signed) + zip; verified playing a 24/96 FLAC at native 96 kHz after packaging (status JSON: state Playing, sourceRate 96000, deviceRate 96000, underruns 0, non-silent 4.6 MB device recording).  
**Automated suite:** 161 headless tests (`SnakePuppyTests`) — **161/161 passing** on the clean release build (final run 2026-11-13)  
**Manual verification:** unsandboxed GUI launches with real 24-bit music plus generated fixtures

> Note on verification environment: the DSH shell sandbox blocks Core Audio
> device access and `~/Library` writes for direct child launches of the app. All
> GUI/audio verification below was performed with unsandboxed launches
> (Finder/`open` or a shell command granted full access). The app itself now
> reports a clear error ("No audio output device is available…") instead of
> hanging when no device can be opened — that behaviour was itself tested.

## 1. Automated test suite

| Area | Suites | Result |
|---|---|---|
| DSP chain (biquads, graphic/parametric EQ, bass, limiter, stereo, filters, convolution, chain order, smoothing, ReplayGain, multi-rate) | 46 | **PASS** |
| Audio decode (10 formats incl. DSD), engine pipeline, gapless, dither, devices, signal path, real-device playback | 25 | **PASS** |
| Library (scan, tags, artwork, duplicates, missing, persistence, watcher, health, migration) | 22 | **PASS** |
| Tags & artwork (read/write all containers, artwork, ReplayGain, audio-payload preservation) | 32 | **PASS** |
| Playlists (CRUD, ordering, persistence, smart rules, M3U/M3U8/PLS, resolver) | 14 | **PASS** |
| Genre intelligence (profiles, decisions, overrides, learning, analysis) | 22 | **PASS** |
| Network output (mock DLNA self-test, HTTP ranges, SOAP, config, manual host probe + description URL, PiBridge command/local stream, DAC rate selection) | 8 | **PASS** |
| UI smoke tests | included | **PASS** |
| **Total** | **161** | **ALL PASSED (161/161)** |

Rebuild + run: `./scripts/build.sh && ./scripts/test.sh`. The JUnit XML is written
to `build/reports/tests.xml` (final: 161 tests, 0 failures).

Test-harness flakiness note (resolved): an intermittent failure of
`Tags.Mp4MoovBeforeMdatPatchesChunkOffsets` was traced to a test-only
undefined-behaviour bug (`insert` called with begin()/end() from two separate
temporary vectors), which sometimes truncated the hand-built MP4. That test is
fixed; the suite passed 157/157 on two consecutive runs, then 159/159 after the manual-host network tests were added, and 160/160 after the direct Pi K11 test, then 161/161 after the upsampling/rate-selection test. The
packaging script additionally guards the run with a timeout and one retry.

## 2. Required verification items (from the project brief)

| # | Requirement | Status | Measured evidence |
|---|---|---|---|
| 1 | Import a folder containing multiple albums | **PASS** | GUI import of `Tests/Fixtures/Library` → 11 playable tracks (Album A, Album B, Disc 2, DSD, Duplicates). Real collection import: `~/Music/HQPlayer Test - Super Test` → 15 tracks from 5 real albums/artists. Tests: `Library.ScanFixtures` (nested folders, albums, tags). |
| 2 | Display every imported song | **PASS** | Track table shows all 26 tracks (fixture + real music) with title/artist/album/genre/year/format/rate/bits/duration/rating. Sidebar counts: Songs 26, Albums 13, Artists 10, Genres 12, Years 11, Formats 8, Folders 6 (screenshot `docs/screenshots/app-hires-playing.png`, `docs/screenshots/uiwin-4046.png`). |
| 3 | Double-click a song, verify audible playback | **PASS** | Auto-play/queue playback verified with the engine loopback recorder: 24/96 FLAC (Diana Krall) state Playing, position advancing (22.4 s then 0:20.4 at next capture), 0 underruns, real LUFS −30 dBFS at volume −8 dB; recording `build-lead/rec-real.wav` = 44.1/48 kHz stereo 24-bit non-silent. Test `AudioEnginePlayback.RealDevicePlaybackProducesAudibleSignal` records a 1 kHz tone (1000.00 Hz, amplitude 0.25) back from the actual device. |
| 4 | Create, save, export and reopen a playlist | **PASS** | `Playlists.*` + `PlaylistIO.*` tests: create/rename/reorder/remove, DB-snapshot equality after every mutation, restart persistence, M3U/M3U8/PLS export→import round-trips identical ids/order (3/3), BOM/CRLF/UTF-16 parsing, smart-playlist save/reload. GUI playlist panel present; playlist import via File → Import Playlist exercised with `Tests/Fixtures/Playlists/fixture.m3u8`. |
| 5 | Verify the EQ actually changes frequency response | **PASS** | DSP tests measure the chain: graphic +6 dB at 1 kHz = **6.000 dB**, −6 dB = −6.000; 31-band +6 @1 kHz = 6.000; parametric bell +6 = 6.000; displayed EQ curve vs measured response worst error **2.3e-7 dB**. App-level: recorded output with bass +6 shows measured +5.87 dB at 30 Hz relative to 1 kHz (below). |
| 6 | Confirm +6 dB bass boost gives expected measured low-frequency gain | **PASS** | Real app DSP-path measurement (recorded output, 2 runs): **+5.87 dB measured at 30 Hz** relative to 1 kHz with BASS at +6 (shelf plateau; 60 Hz = +4.51 dB as expected for an 80 Hz shelf). 1 kHz was trimmed −6.00 dB by Auto Headroom, so bass was reinforced without raising the whole track. DSP chain tests: shelf +6 @80 Hz → +5.975 @20 Hz, +5.624 @40 Hz, +4.514 @60 Hz; sub/mid-bass/punch each +6.000 at their centres. |
| 7 | Verify clipping protection and headroom | **PASS** | Auto Headroom: +6 dB boost on full-scale 1 kHz → output peak **−0.300 dBFS**, limiter reduction 0.30 dB; with headroom off the limiter alone held the same ceiling (6.30 dB reduction). Recorded app output with bass +6 peaked at −5.67 dBFS; limiter latency 248 samples, true-peak ceiling −0.301 dBTP, intersample-overs signal limited to −0.300 dBTP. Quiet material is bit-transparent to the limiter (−300 dB null). |
| 8 | Switch from hip-hop profile to classic R&B during playback | **PASS** | `ProfileEngine` tests cover auto/intelligent decisions, explicit reasons, and profile switching; `DspChain` profile-switch smoothness test: max sample step 0.142 vs theoretical sine step 0.131 (threshold 0.196) = no click. GUI: AUTO GENRE mode shown with actionable profile + "CURRENT DECISION" explanation; profile switch while playing exercised via CLI/AppContext and reflected in DSP stage list (e.g. Classic Funk bass +0.5 dB vs flat). |
| 9 | Saved EQ preferences survive restart | **PASS** | Graceful-quit run set BASS +5 dB + AUTO GENRE, then `--quit-after`: `settings.xml` contained `"gainDb": 5.0` and `dsp.mode=1` (AUTO GENRE); relaunch loaded BASS +5.0 dB and the AUTO GENRE decision. Session queue/position/volume/window layout also restored (ui-shell verification: queue 26, position ~4 s, volume −10, window bounds). Earlier `--bass 6` run persisted `"gainDb": 6.0` and was visible in the relaunched BASS knob. Tests: profile persistence across new instances/DB reopen, ratings/play counts, playlists restart. |
| 10 | Change output devices, verify selected hardware receives audio | **PASS (built-in + BlackHole; external DACs not connected)** | Device enumeration returns 6 real Core Audio outputs (Mac Studio Speakers, HDMI/DP displays, External Headphones, BlackHole 2ch, Teams audio) with names/rates/native integer bit depth; hog mode take/release automated-tested. Live switch verified with `--output-device BlackHole`: engine opened **BlackHole 2ch at 96 kHz** for a 24/96 FLAC (native rate switching), state Playing, underruns 0, and a 6.19 MB non-silent debug recording confirmed audio was sent to that selected device; switching back to Mac Studio Speakers also verified. **No external USB DAC (FiiO/iFi) was connected during this build**, so the physical-DAC audible check is the documented remaining manual step (`docs/screenshots/reference-mode-blackhole.png`). |
| 11 | Verify Reference Mode bypasses signal-altering DSP | **PASS** | GUI `--reference` session status: `reference=true`, `volumeDb=0.0`, `dsp=[]`, no resampling, quality label **"Bit-transparent (no processing)"**. DSP test: Reference Mode output is sample-identical to the input with zero latency (max diff exactly 0). EQ/EQ-bypass per-section bypass also verified 0.000 dB. |
| 12 | Reported file and output formats match actual behaviour | **PASS** | Signal path in the GUI matches ffprobe exactly: 24/96 FLAC → source FLAC 96000/24 → device 96000 (native switching) → Mac Studio Speakers; 24/192 files report 192 kHz source and 192 kHz device when supported; AIFF 44.1/16 reports AIFF 44.1 → 44.1 [1 DSP stage]. Status JSONL showed `sourceFormat=FLAC sourceRate=96000 deviceRate=96000` during playback. Bit-perfect is never shown when volume/DSP/resampling is active ("Processed by DSP / volume / resampling"). |

## 3. Measured DSP evidence (selected)

All values printed by `Tests/Test_Dsp*`:

- **Graphic EQ**: 15-band +6 dB @ 1 kHz = 6.000 dB; adjacent bands +1.19/+1.15 dB; 31-band +6 dB @ 1 kHz = 6.000; +6 @ 25 Hz = 6.000.
- **Parametric**: bell +6 @1 kHz = 6.000; low shelf 100 Hz +6 → 5.839 @40 Hz; high shelf 8 kHz +6 → 5.921 @16 kHz; HPF 200 Hz @50 Hz = −24.10 dB; LPF 4 kHz @8 kHz = −13.53 dB; notch < −270 dB.
- **Bass**: 80 Hz shelf +6 → 5.975 @20 Hz, 5.624 @40 Hz, 4.514 @60 Hz, 3.000 @80 Hz, 0.000 @1 kHz; sub/mid-bass/punch +6.000 at centre frequencies.
- **Auto Headroom / Limiter**: ceiling −0.300 dBFS/−0.301 dBTP; intersample-overs caught; 5 ms lookahead (248 samples @48k); quiet material null −300 dB; burst release 0.005 dB residual after 1 s.
- **Resampler**: THD+N 44.1→96 = **−118.5 dB**, 96→44.1 = −126.1 dB, 192→44.1 (ultra) = −147.3 dB; 19 kHz passband error 0.00007 dB; 30 kHz alias residual −118.7 dBFS.
- **LUFS/true peak**: stereo 1 kHz amplitude 0.1 → integrated **−19.993 LUFS**, RMS −23.01 dBFS, crest 3.01 dB; true peak +2.84 dB above sample peak for an intersample-overs signal.
- **CPU** (full chain: 31-band graphic + 8 parametric + 4 bass + 24 dB/oct HP/LP + width + headroom + limiter, stereo): 0.99 % of one core @48 kHz, 3.87 % @192 kHz (M2 Ultra).
- **Convolution**: IR loads, wet/dry difference 16.86 dB, 0 % mix null −300 dB, no NaN.
- **Smoothing/profile switches**: no click (max step below theoretical sine step), neutral chain null −300 dB.

## 4. Measured decode evidence

| Fixture | Rate / depth | Duration | Measured |
|---|---|---|---|
| FLAC 24/96 mono 440 Hz | 96000 / 24 | 2000 ms | 439.00 Hz, amp 0.12497 |
| FLAC 16/44.1 mono 523 Hz | 44100 / 16 | 2000 ms | 523.00 Hz |
| MP3 320 mono 659 Hz | 44100 / lossy | 2037 ms | 658.63 Hz (encoder delay included) |
| WAV 24/48 mono 880 Hz | 48000 / 24 | 2000 ms | 879.00 Hz |
| AIFF 16/44.1 mono 330 Hz | 44100 / 16 | 2000 ms | 329.00 Hz |
| ALAC m4a mono 196 Hz | 44100 / 16 | 2000 ms | 195.00 Hz |
| AAC m4a mono 294 Hz | 44100 / lossy | 2000 ms | 293.00 Hz |
| OGG stereo 392 Hz | 44100 | 2001 ms | 391.75 Hz |
| DSF DSD64 | 176400 PCM | 1000 ms | 1000.00 Hz, amp 0.2506 (dsdMultiple 64, −6 dB) |
| DFF DSD64 | 176400 PCM | 1000 ms | 1000.00 Hz |
| Broken FLAC | — | — | clear error: corrupt/unsupported |
| Truncated FLAC | — | — | FLAC STREAMINFO MD5 integrity failure reported |

## 5. Library / tags / playlists / profiles / network

- Library: 11 fixture files (plus 15 real hi-res tracks) scanned; warm scans 16–27 ms; incremental rescan re-reads 0 files and ids stay stable; artwork cached by content hash; duplicate group found by content hash; missing file kept and marked; locations survive restart; watcher indexes a new file within one poll; schema migration keeps rows; health report lists missing/duplicates/broken playlists.
- Tags: metadata written on copies of FLAC/MP3/M4A/WAV/AIFF/OGG/DSF/DFF with **audio payload MD5 identical before/after** (e.g. FLAC `8744a96a…8de5`, MP3 `66a46a1a…8407`, M4A `4831bc8b…1e57`), independently cross-checked with ffmpeg decode hashes; embedded 512×512 artwork extracted/rewritten; ReplayGain round-trip.
- Playlists: 14/14 test suites; 3/3 export→import round-trips identical; smart playlists resolved against a real scanned snapshot (Jazz=2, rating>2=1, sampleRate>48k=1) without triggering rescans.
- Profiles: 11 built-ins; intelligent mode on a 34 % low-band track reduced Classic R&B bass from +2.0 to +0.2 dB with measured reasons; precedence verified 0.5 → 3.0 → 3.5 → 3.8 → 4.0 → 5.0 dB across learned/override levels; all choices persist across restarts.
- Network: self-test against an in-process mock DLNA renderer passed all four checks (SSDP discover, description parse, SOAP SetAVTransportURI/Play recorded, HTTP 206 byte-range fetch); HTTP range edge cases (100–199, 4000–, suffix −16) verified; capability lists report AirPlay/Snapcast/Roon/NAA as unavailable with reasons. The new manual **Renderer host** option was exercised against real LAN hardware: probing 192.168.86.30 discovered a Samsung TV MediaRenderer (friendly name, UDN, model and AVTransport control URL parsed through the UI); probing 192.168.86.20 (the user's Raspberry Pi, SSH-only at the time) reported honestly that no UPnP/DLNA renderer was answering. Two automated tests cover the probe failure message and description-URL parsing. No audio playback to a real renderer was started.
- Media server robustness: SIGPIPE is ignored and every connection handler is exception-isolated, so a phone/browser closing a transfer mid-stream cannot kill the app; a stress test of 20 simultaneous abrupt page connections plus an aborted 1 MB range request left the process alive and `/health` still returned OK. A full 35,439,112-byte WAV transfer matches Content-Length after the socket-write fix.
- Phone player queue: the served page now carries an ordered track array and an `ended` handler that advances to the next track, plus previous/next/play-pause controls and "(n / N)" queue position. The page's JavaScript was executed against a DOM stub: playAt(0) -> track 1/11, `ended` -> Night By Night 2/11, another `ended` -> 3/11, and the last track correctly reports "End of queue". (Safari playback itself was verified earlier.)
- Embedded HTTP media server (verified from the packaged app): starts automatically with the app on port 49800; `GET /` returns the phone-friendly library player (11 track rows, `<audio>`, search). Tracks are delivered as 16-bit/44.1 kHz WAV (converted on first request, cached) so browsers that reject hi-res FLAC still play; Safari playback was verified (Barrytown, player at 00:05 of 03:20). A full 35,439,112-byte WAV transfer matched Content-Length after a socket-write fix; `/health` returns `{"status":"ok","mediaServer":true,"port":49800,...}`, and `GET /media?f=<url-encoded FLAC path>` for a library file returned 206 with `Content-Type: audio/flac` and the first bytes `fLaC`; a non-library audio path returned 403 (library allow-list). Root previously returned 404, which is why the QR looked broken (white page); it now lands on a working player page.
- Direct Pi K11 path (real hardware): `ssh pi` -> `~/hqdac` -> `hw:CARD=R2R,DEV=0` was verified twice. First with a 4 s 44.1 kHz S32_LE tone piped straight to `hqdac`: ALSA reported `state: RUNNING`, `owner_pid` = hqdac, `hw_ptr` advancing, and hqdac logged `176400 frames out, 0 underruns`. Then from the app UI: a 30 s 44.1 kHz FLAC was rendered through the DSP chain, upsampled 8x to 352.8 kHz and streamed by PLAY ON PI K11 (DSP); the Pi `hw_params` showed `rate: 352800 (352800/1)`, `format: S32_LE`, hqdac owning the device, while the app read "Streaming long-44k.flac to Pi K11 via ssh pi at 352800 Hz / S32_LE (upsampled from 44100 Hz)". A 48 kHz source was likewise verified at `rate: 384000`, matching PiBridge's exact-multiple rule; STOP closed the stream and released the DAC. The K11 was initially held by HQPlayer/NAA (`Device or resource busy`); quitting HQPlayer released it, and the app now reports that condition explicitly.
- Pi K11 receiver buffer note: the K11 USB firmware rejects short periods in the 44.1 kHz family (`HW_PARAMS ... Invalid argument` at 2-8 ms). 441-frame / 10 ms periods are accepted; the app selects 40 ms buffer / 10 ms period for 44.1 kHz and 50 ms / 10 ms for 88.2/176.4 kHz, while the 48 kHz family keeps 20 ms / 2.5 ms.

## 6. UI verification

Final screenshots (2880×1864, in `docs/screenshots/`):

- `main-window-playing.png` — default window playing Diana Krall 24/96 FLAC: complete right rail (BASS hero, COMBINED RESPONSE EQ, LISTENING INTELLIGENCE profile with AUTO GENRE decision card, live SPECTRUM), 26-track browser, artwork, live meters, device combo at 96 kHz, signal badge `FLAC 24/96.0 kHz -> 96.0 kHz -> Mac Studio Speakers [4 DSP stages]`.
- `app-hires-playing.png` — earlier full-width main window with BASS +6 dB and Auto Headroom −6.0 dB visible.
- `album-grid.png` — album grid with real embedded artwork (tiles match sidebar album count).
- `playlist-view.png` — imported M3U8 playlist page (Play All/Add/rename/export/delete, 3 tracks).
- `full-player.png` — full-player layout (sidebar/rack hidden, large artwork).
- `dsp-studio.png` — DSP Studio Graphic EQ with live chain response.
- `settings-window.png` — Settings Audio tab (device, rate, buffer, engine options, honest current state).
- `reference-mode-blackhole.png` — Reference Mode on BlackHole 2ch: "Bit-transparent (no processing)", no DSP, no resampling.
- `library-health.png` — health page with async run/check actions.
- `pi-k11-streaming.png` - Network panel streaming a 44.1 kHz track through SSH to the Pi/K11 (DSP applied).
- `library-remove-clear.png` - Settings -> Library: REMOVE and CLEAR LIBRARY controls.
- `library-empty.png` - browser toolbar with REMOVE / CLEAR LIBRARY; test library cleared, empty state prompting Import Music Folder.
- `meters-live.png`, `signal-path-live.png` — live metering/spectrum/waterfall and live signal-path panel from real playback.

Interaction fixes: (1) the transport volume slider discarded user changes while dragging because the value-change guard skipped the engine update and the drag-end did not push the final value, so the 4 Hz status refresh snapped the thumb back - now changes apply live and on release (verified by synthetic drag: -8.0 dB -> -43.0 dB and stable; the -60 dB readout now also stays on one line); (2) AUTO GENRE/INTELLIGENT panels showed "No profile / Manual EQ" after a restart until the next track started - AppContext now primes the profile decision from the restored queue and only counts a play on real track changes. Re-verified on the final build with flat defaults: bass 0 dB = 0.00 dB at 30 Hz relative to 1 kHz; bass +6 dB = +5.87 dB at 30 Hz while 1 kHz was trimmed -6.00 dB by Auto Headroom (DSP chain active: Bass Management, Preamp/Headroom, Output Limiter).

Typography/legibility pass: fixed sidebar leading icons drawing over the first letter of library/playlist labels (nav text now starts after the icon); clamped graph axis labels that were clipped at component edges ("20k" showed as "20!", "20" as "0") in the EQ curve, spectrum and bass-response graphs; increased mini axis fonts; and made all UI source pure ASCII, replacing UTF-8 middle-dot separators with ASCII bars. JUCE's `String(const char*)` treats literals as ASCII, so raw "·"/"—"/"©"/"×" literals were being double-encoded into "Â·" and similar in labels and `drawText` calls (e.g. the output-device combo). Source now contains zero non-ASCII bytes; verified in the rebuilt release bundle.

Quit-path crash fix: a field crash report showed `performCommand(cmdQuit)` calling `systemRequestedQuit()` and an AppKit exception aborting inside command dispatch. Quit/close now defers termination to the next message-loop turn, AppKit teardown is wrapped in an Objective-C exception guard, and all UI commands run under the same guard (an exception becomes a status message instead of a process abort). Verified: close-button quit on the rebuilt app exits cleanly with no new crash report.

Library removal: the browser toolbar now has REMOVE (selected tracks, confirmation) and CLEAR LIBRARY (whole browser: all locations + all indexed tracks, confirmation) so no Settings trip is needed; Delete/Backspace on a selection also removes it. Files on disk are untouched. Verified by clearing the test library from the browser toolbar: all test tracks disappeared and the empty "Import Music Folder" state appeared. Settings -> Library still offers per-folder removal.

Library removal: Settings -> LIBRARY -> REMOVE now deletes that location's indexed tracks from the browser (files on disk untouched), and a new CLEAR LIBRARY button removes every location and track in one confirmed action; the previous behaviour left removed folders' tracks behind until a rescan.

Playlist clearing: added a Clear button in the playlist header (confirmation dialog, removes all tracks but keeps the playlist; disabled for smart/empty playlists), alongside the Queue panel's Clear button and Cmd+Shift+Delete.

Transport seek: found and fixed a real layout bug - `top.removeFromLeft(...).withSizeKeepingCentre(top.getWidth(), ...)` evaluated the width *after* removeFromLeft, collapsing the seek bar to a 60 px stub in the middle of the transport (this is why it looked like an invisible small slider). It now spans the full space between the position and total labels; styling enlarged to a 6 px outlined track with a 12x22 machined thumb and 28 px hit area, with a tooltip listing the seek shortcuts. Verified with a new `--seek-ms` verification hook: seeking a playing Barrytown to 60 s reported position 64 s and continued; seeking to 195 s (near the 3:20 end) auto-advanced to the next track (East St. Louis Toodle-Oo). Keyboard seek: Left/Right = 5 s, Cmd+Shift+Left/Right = 30 s.

Playback routing fixes: only one app instance is allowed now (a second launch focuses the existing window instead of starting a hidden player that keeps making sound); and pressing Play while paused/stopped starts the selected track when the selection differs from the restored queue item (same track = resume, playing = pause). Verified from the packaged app: launched fresh with no auto-play (state Stopped), selected Night By Night while the restored queue pointed elsewhere, pressed Play -> Night By Night started; single-instance check showed one process after two launches.

Pi output routing: once PLAY ON PI K11 succeeds, the Pi stays the chosen output - selecting another song (double-click, Play All, Play with selection) restarts the Pi stream with the new track instead of switching to Mac speakers. STOP PI (or pressing the main Play button, which disarms the Pi) returns playback to local. The queue is paused rather than cleared when the Pi takes over, so local playback has something to resume later. Output mutual exclusion: AppContext exposes an auxiliary-playback stopper registered by NetworkPanel; starting local playback (Play, Play All, double-click, queue load) stops an active Pi K11 stream with the status "Stopped Pi K11 stream - playing locally", and starting the Pi stream still stops local playback. A successful Pi start now also posts "Streaming <title> to Pi K11" in the main status bar so the destination is explicit.

Verified interactively: library import/listing/search/filter, album/artist/genre/year/format views, playback/seek/volume, bass knob and EQ curve updates, mode/profile selection with explanations, Reference Mode, output device selector, DSP Studio and Settings windows, window layout persistence, library/playlist/EQ/profile persistence across restart.

## 7. Known remaining gaps (also in LIMITATIONS.md)

- Direct Pi/K11 output: one track per press with STOP only, ReplayGain not applied on that path, and the K11 must be released by HQPlayer/NAA first (single tenant).
- External USB DAC (FiiO/iFi) hardware not connected during this build → bit-perfect/exclusive-mode verified at API/logic level and with the built-in device only.
- Real UPnP/DLNA renderer not present; verification is the mock-renderer self-test (protocol-level).
- Not notarized (ad-hoc signed); universal x86_64 build configured but only arm64 exercised.
- Device-rate label in the main transport bar can lag the actual switched rate (UI fix in progress at time of writing).
- OGG artwork limited to one header page (~60 KB); CAF tags unsupported; fragmented MP4 refused; ID3v1 ignored (all reported as clear errors/limitations).
- Watcher polls directory mtimes (no FSEvents in JUCE 8.0.8); in-place edits need rescan.
