# Changelog

## 0.1.0 - 2026-11-13

First public source release.

### Player

- 64-bit DSP chain: 15/31-band graphic EQ, 12-band parametric EQ, bass
  management, stereo tools, crossfeed, convolution, ReplayGain, preamp/auto
  headroom and true-peak limiter.
- DSD64/128/256 decoding to PCM with a multistage decimator; native sample-rate
  switching; gapless queue playback; TPDF dither; memory playback.
- Reference Mode, device profiles, genre intelligence with explanation, smart
  playlists, library health, metadata editing and duplicate detection.
- Real metering and an honest Audio Signal Path display.
- Full-width seek bar, keyboard transport (Space, arrows, command shortcuts),
  single-instance app and playback routing between the Mac output and Pi K11.

### Network

- Embedded HTTP media server with phone player: queue auto-advance,
  previous/next/play-pause, scrub bar, and on-demand 16-bit/44.1 kHz WAV
  transcoding for hi-res sources.
- UPnP/DLNA discovery and AVTransport control point with mock-renderer
  self-test.
- PiBridge/K11 direct output: local DSP rendered to S32_LE over SSH, always
  upsampled to 352.8 kHz (44.1 family) or 384 kHz (48 family).
- Pi output is a routing choice: selecting another song restarts the stream on
  the Pi; local playback and Pi streaming never run different songs at once.
- Master volume and mute apply live to the Pi stream (smoothed per block), not
  only to local playback.
- Scrubbing while the Pi is the output restarts the stream at the requested
  position (the receiver has no seek protocol) and the transport clock/scrub bar
  follow the Pi position.
- DSP Studio -> Devices now shows Pi K11 as an explicit output and labels the
  local Core Audio device inactive while the Pi is selected; the transport
  signal badge reports the Pi stream format/rate instead of the Mac device.
- Filter/convolution UI is no longer decorative-looking: enabling high-pass
  jumps to an audible 40 Hz default, low-pass to 18 kHz, each with a status
  confirmation; convolution refuses to enable until an impulse response is
  loaded (LOAD IR), since an unloaded convolution stage is correctly silent.
- Auto Headroom now defaults off (the limiter still protects peaks). Previously
  raising BASS lowered the preamp by the boost amount, so cranking bass made the
  whole song quieter. An AUTO HEADROOM toggle is on the Bass page, and existing
  settings are migrated once so the level stays put.
- Pi K11 output now feeds the app's meters, spectrum and loudness analysers
  from the post-DSP stream (chunked into the analyzer FIFOs), and DSP changes
  made in the app are pushed live to the running Pi stream.
- Pi K11 is selectable as the player output in the transport device selector,
  alongside the Mac audio devices. Selecting it routes Play/Stop/Next/Previous,
  volume, mute and queue auto-advance to the Pi; selecting a Mac device returns
  playback there. Stop silences the stream but keeps Pi K11 as the output.
- Switching output while a song is playing pauses the Mac and reroutes that song
  to the Pi; if the Pi cannot start, the app reports the error and stays silent
  instead of falling back to Mac playback. Stop always silences both outputs.

### Quality

- 160+ headless tests covering decode, DSP, library, playlists, engine
  invariants, HTTP ranges and PiBridge rate selection.
- Packaging script produces a signed app bundle and a distributable zip.
