# Known limitations (honest capability boundaries)

This file is deliberately conservative. If something here is not claimed, it is
not implemented or not verified — the application does not pretend otherwise.

## Playback and formats

- **Bit-perfect playback is conditional.** The app never labels processing-active
  audio as bit-perfect. "Bit-perfect" is reported only when *all* of these hold:
  integer-PCM source, file rate equal to the device rate, no DSP (EQ, bass,
  filters, convolution, crossfeed, width, balance, ReplayGain, preamp, limiter),
  unity volume, no dithering, no resampling, an integer-format output device and
  exclusive (hog) mode. The built-in "Mac Studio Speakers" and similar Core Audio
  outputs report a float format (0 integer bits), so on those devices the app
  honestly reports **bit-transparent** (no processing, OS may mix) rather than
  bit-perfect. Most USB DACs run in hog mode with integer 24/32-bit formats and
  can reach the bit-perfect state; this was verified at the signal-path logic
  level and with a real device's exclusive access, but the specific DACs in the
  user's equipment list were not connected during this build.
- **DSD is decoded to PCM, not passed through.** DSF/DFF DSD64/128/256 are decoded
  with a two-stage FIR decimator to 176.4/352.8 kHz PCM with a −6 dB DSD gain
  (reported in the signal path). DoP (DSD over PCM) is **not** implemented, and
  no native DSD DAC handshake is attempted. DSD512 and multichannel DSD (>2 ch)
  are rejected with a clear error.
- **Gapless** is sample-accurate when consecutive tracks share the same sample
  rate. When the engine switches the output device rate between tracks, a short
  re-open gap is unavoidable (Core Audio rate changes are not sample-continuous);
  the app does not claim gapless across rate switches.
- **Native sample-rate switching** is applied per track (and in Reference Mode)
  when the device supports the file rate. Otherwise the engine resamples with its
  high-quality polyphase resampler on the decode thread and reports
  `resampling=true` plus both rates in the signal path.
- **Exclusive (hog) mode** is supported through Core Audio
  (`kAudioDevicePropertyHogMode`) on devices that implement it. It can fail if
  another application holds the device; the failure is shown, not hidden.
- **Multichannel files** (>2 channels) are downmixed to stereo; the downmix is
  reported. 5.1/7.1 playback to a multichannel device is not implemented.
- **CAF** files can be probed/decoded through AudioToolbox, but tag reading/writing
  for CAF is not implemented (clear error).
- **MP3/AAC/OGG are lossy**; the app reports lossy/lossless honestly and never
  presents decoded lossy audio as bit-perfect.
- **ReplayGain**: track/album gain and peak tags are read and applied; the app
  does not compute ReplayGain values itself for untagged files (a fallback gain
  is configurable). R128 fallback tags are read where present.
- **Dither** is TPDF at the device's integer LSB when the effective device bit
  depth is lower than the source; when Core Audio reports a float device, the
  final quantization is performed by the OS and dither is reported accordingly.

## DSP

- **Convolution** uses JUCE's partitioned convolution (float internals) with
  WAV/AIFF/FLAC impulse responses; the rest of the chain is 64-bit double. Very
  long/true-stereo IRs are bounded by the convolution engine's internal limits
  (errors are reported).
- **Limiter**: a lookahead true-peak limiter is the last stage. It is designed to
  catch peaks, not to compress program material. If a user disables the limiter
  and boosts heavily, clipping can be reached; Auto Headroom is on by default and
  is the recommended protection.
- **Reference Mode** bypasses *all* signal-altering DSP including the limiter,
  sets unity volume and disables ReplayGain. This is intentional but means there
  is no clipping protection while it is active.
- **A/B level matching** uses the measured short-term RMS difference between the
  pre-DSP input and post-DSP output to compensate loudness. It is an explicit
  measurement, not a perceptual loudness model.
- **Channel balance, stereo width and crossfeed** are implemented in the stereo
  domain with a mono-compatibility safeguard for width; DSP Studio exposes the
  options. This is not a full room-correction suite.

## Library

- **Missing/disconnected drives are never deleted** from the library; tracks are
  marked missing and stay in snapshots/playlists until the location returns or
  the user removes it.
- **Folder watching** polls directory modification times (JUCE 8.0.8 has no
  FileSystemWatcher). Adds/removes/renames are detected within a few seconds;
  in-place edits of an existing file require a rescan or the app's own metadata
  editor.
- **Metadata editing** rewrites only tag containers; the audio payload is proven
  byte-identical by tests for every supported container. Star ratings are written
  only where the format has a standard field (ID3 POPM); FLAC/OGG/MP4 have no
  standard star field, so ratings for those files live in the library database
  only.
- **Duplicate detection** uses content hashes computed lazily; the health report
  lists groups. It does not auto-delete duplicates.
- **First scan of very large hi-res libraries is I/O-bound** (measured ~0.4–1.1 s
  per 100–200 MB 24/192 FLAC on this machine). The UI serves the persisted
  snapshot immediately and reports scan progress.

## Network output

- **DLNA path**: UPnP/DLNA AVTransport only - SSDP discovery, device-description
  parsing, SOAP transport control and an embedded HTTP byte-range server that
  serves the source file to the renderer. Verified against the in-process mock
  renderer, and manual host probing was verified against a real DLNA
  MediaRenderer on the LAN (a Samsung TV). Audio playback to that TV was not
  started.
- **Local DSP does not apply to DLNA playback.** The renderer fetches and
  decodes the original file itself; EQ/bass/volume in this app do not affect what
  the renderer plays. The UI states this explicitly.
- **Direct Pi / K11 path (DSP applied)**: Network -> PLAY ON PI K11 (DSP)
  renders the currently loaded track through the local 64-bit DSP chain and
  streams S32_LE PCM over SSH to `~/hqdac` on the Pi, which writes it to the USB
  DAC. This path applies EQ, bass, limiter and volume. Verified on the real
  Raspberry Pi with a FiiO K11 R2R: the ALSA PCM status showed `hqdac` owning
  card 0 with `hw_ptr` advancing and 0 underruns; STOP closes the stream and
  releases the DAC.
- Rate policy matches PiBridge: the K11 maximum that is an exact integer
  multiple of the source is chosen (352.8 kHz for 44.1/88.2/176.4/352.8 kHz
  sources, 384 kHz for 48/96/192/320/384 kHz sources; DSD64/DSD128 -> 352.8 kHz;
  DSD256 is downsampled 2x to 352.8 kHz). The DAC therefore always runs at
  352.8 or 384 kHz; the source rate is never sent. The stream uses our
  high-quality polyphase resampler at 20 ms receiver buffer / 2.5 ms periods
  (the 44.1 kHz-family buffer quirk disappears because the DAC always runs at
  its maximum rate).
- Limits of the direct path: one track per press with STOP only (the receiver
  has no transport protocol), ReplayGain tags are not applied, and the K11 is
  single-tenant: HQPlayer/NAA or any other player must release the DAC first,
  and the app reports a busy DAC explicitly.
- **Manual Renderer host**: the Network panel's **Renderer host** field
  (enter your receiver's LAN address) plus PROBE HOST sends a unicast SSDP
  M-SEARCH to that host, or fetches a pasted device-description URL; DISCOVER
  also probes it. A host with no UPnP renderer is reported as such, never
  pretended to connect.
- **AirPlay sender, Snapcast, Roon and HQPlayer NAA are not implemented** and are
  listed as unavailable in the Network panel with reasons. macOS exposes no
  public API for sending a custom PCM stream to AirPlay, so no claim is made.
  (The direct Pi path uses PiBridge's `hqdac` receiver over SSH, not NAA.)

## Platform / packaging

- The delivered app is **ad-hoc signed**, not notarized. For distribution, sign
  with a Developer ID certificate and notarize.
- The release build is **arm64** by default; `SP_UNIVERSAL=1` builds arm64+x86_64
  (the code is architecture-neutral; only the arm64 build was exercised in this
  environment).
- **No sandboxing/App Store packaging** is configured. The app needs normal local
  file access to the user's music folders.
- **No plug-in hosting (VST/AU)**, no streaming services, no CD ripping, no
  video playback, no remote control app.
- **One library database per app container.** Multiple app instances are allowed
  for development but are not recommended for normal use (SQLite WAL handles it,
  but concurrent edits are last-writer-wins for settings).

## Verification context

Real music was used from this machine's local collection (Diana Krall 24/96,
Michael Jackson / Norah Jones / Simply Red / Steely Dan / The Supremes
24/176.4–192 FLAC) plus generated fixtures. The audio output device available
during verification was the built-in Mac Studio Speakers and virtual/HDMI
outputs; no external USB DAC (FiiO/iFi) was connected, so DAC-specific bit-perfect
and exclusive-mode behaviour was verified at the logic/API level and with the
platform's exclusive-mode API, not with those physical units.
