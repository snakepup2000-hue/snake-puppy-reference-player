# Architecture — Snake Puppy Reference Player

High-level module map, threading model, and the key data flows.

## Modules

```
Source/
├── Core/
│   ├── Types.h                  Track, playlist, analysis, signal-path, health value types
│   ├── DspParams.h/.cpp         Plain-data DSP parameter model (the shared truth)
│   ├── Serialization.h/.cpp     Canonical JSON <-> model (stable key order)
│   ├── ApplicationPaths.*       App container paths (Application Support)
│   ├── AppSettings.*            PropertiesFile: engine options, layout, shortcuts, device profiles
│   └── AppContext.*             Coordinator: owns services, wires track->profile->DSP,
│                                session persistence, A/B, reference mode, actions
├── Audio/
│   ├── DecodeSource.*           Format-agnostic decoder interface + factory
│   ├── DecodeBackends.h         JUCE reader backend, AudioToolbox (M4A/AAC/ALAC/CAF)
│   ├── AudioToolboxDecodeSource.*
│   ├── DsdDecodeSource.*        Native DSF/DFF DSD64/128/256 -> PCM (FIR decimation)
│   ├── DecodeUtils.*            Sample conversion, downmix, seek helpers
│   ├── PlaybackPipeline.*       Gapless queue preloading + resampling + memory playback
│   ├── LockFreeRingBuffer.h     SPSC sample ring
│   ├── Dither.h                 TPDF at device LSB
│   ├── SignalPath.*             Honest format-chain classification
│   ├── DeviceManager.*          Core Audio enumeration, rates, hog mode, bit depth
│   └── AudioEngine.*            Device callback, decode thread, transport, status
├── DSP/
│   ├── Biquad.h                 Double RBJ biquads + Butterworth cascades
│   ├── Equalizers.*             Graphic (15/31), parametric (12), bass, HP/LP
│   ├── Dynamics.*               True-peak detector + look-ahead limiter
│   ├── StereoProcessor.*        Balance, width/mono safeguard, mono bass, crossfeed
│   ├── DspChain.*               Chain ordering, smoothing, auto headroom, convolution, status
│   ├── Resampler.*              Streaming polyphase windowed-sinc
│   └── AnalyzerHub.*            Off-thread metering: peaks, LUFS, correlation, FFT, spectrogram
├── Library/
│   ├── Sqlite.*                 Thin system-SQLite wrapper (JUCE 8 has no SQLite)
│   ├── LibraryDatabase.*        Shared DB handle, schema, migrations
│   ├── LibraryStore.*           Row mapping / SQL
│   ├── LibraryScanner.*         File probing, tags, artwork cache, content hash
│   ├── LibraryService.*         Scanner thread, watcher, immutable snapshots, health
│   └── Tags.*                   ID3v2/Vorbis/MP4/RIFF/AIFF/DSF tag + artwork parsers/writers
├── Playlist/
│   ├── PlaylistManager.*        SQLite playlists, snapshots, smart playlists
│   ├── PlaylistIO.*             M3U/M3U8/PLS codec
│   ├── PlaylistResolver.*       Entry -> track resolution
│   └── SmartPlaylist.*          Rule evaluation, sorting, limits
├── Profile/
│   ├── ProfileEngine.*          11 genre profiles, decisions with reasons, learning, overrides
│   └── TrackAnalyzer.*          Gated LUFS, true peak, crest, spectral bands
├── Network/
│   └── NetworkOutput.*          SSDP discovery, DLNA SOAP control, HTTP range server, self-test
└── UI/
    ├── LookAndFeel.*            Charcoal/silver/gold theme
    ├── Widgets.*                Knobs, faders, buttons, meters, section headers
    ├── MainWindow / MainComponent / SidebarPanel / LibraryBrowser /
    │   TrackTableComponent / ArtworkPanel / TransportBar / PlaylistPanel /
    │   QueuePanel / HealthPanel
    ├── EqCurveComponent / GraphicEqEditor / ParametricEqEditor /
    │   BassControlPanel / ProfilePanel / DspChainPanel
    ├── MetersPanel / SpectrumComponent / SpectrogramComponent / SignalPathPanel
    └── DspStudioWindow / SettingsWindow / NetworkPanel
```

## Threading model

| Thread | Responsibilities | Rules |
|---|---|---|
| Message (UI) thread | Painting, user input, coordinator calls, DB reads through snapshots, scanner/job dispatch | Never blocks on decode or scan work |
| Audio callback (Core Audio realtime) | Ring-buffer read, DSP chain process, volume, dither, device write, meter FIFO pushes | No allocation, locks, file/network I/O, logging; preallocated buffers; atomics only |
| Decode thread | Opens decoders, reads PCM, resamples to device rate, gapless preloading, memory playback | May do file I/O; never touches the audio callback directly except through lock-free ring |
| Analyzer thread | Consumes pre/post-DSP FIFOs; computes peaks/true peak/RMS/LUFS/FFT/spectrogram | Reads lock-free, publishes atomics/double-buffered arrays |
| Library scanner thread | Directory walk, decode probes, tag/art parsing, artwork caching, hashing, DB writes | Publishes immutable snapshots; listener callbacks marshalled to message thread |
| Library watcher timer | Polls watched folder mtimes | Cheap; triggers incremental scans |
| Network discovery thread | SSDP M-SEARCH + description fetch | Updates endpoint list under lock; notifies via ChangeBroadcaster |
| HTTP media server thread | Serves files with byte ranges to DLNA renderers | Path validation, timeouts |
| Analysis worker pool | Genre-intelligence track analysis (decode + LUFS/spectrum) | Results cached in DB, re-applied on message thread |

## Key data flows

### Playback
```
UI selection -> AppContext::playTrack -> AudioEngine queue
AudioEngine decode thread -> DecodeSource (native rate) -> Resampler (if needed)
   -> lock-free ring buffer
Audio callback -> DspChain (double) -> volume -> dither -> float -> Core Audio
   -> AnalyzerHub FIFOs                                  -> UI meters
```

### Track change / intelligent EQ
```
trackChanged (engine listener, message thread)
  -> AppContext::resolveProfileForTrack
       ProfileEngine::resolveParams(track, cached Analysis, manual params)
         precedence: song override > album override > learned > profile > manual
       -> engine.setDspParams(params, rampMs)   (click-free smoothing)
       -> ReplayGain selection -> engine
       -> if Intelligent mode and no cached analysis: worker analyses track,
          saves to DB, re-resolves on message thread and publishes decision
  -> ProfileDecision (reasons) -> UI explanation
```

### Library
```
addLocation/importFolder -> scanner job -> probe/tags/artwork -> SQLite rows
  -> publish shared_ptr<const LibrarySnapshot> -> UI bubbles change
```
UI reads only snapshots; playlist operations read the same snapshot and never
trigger rescans.

### Network output
```
UI -> NetworkOutput.startDiscovery -> SSDP -> device description -> endpoints
UI -> connect + playUri -> AppContext starts embedded HTTP server
  -> SetAVTransportURI(file URL) + Play (SOAP) -> renderer fetches file over HTTP
```
Local DSP does not apply to network playback (documented in UI and limitations).

## Persistence

- `library.db` (SQLite/WAL): tracks, locations, analysis cache, playlists, items,
  EQ profiles, song/album overrides, learned preferences.
- `settings.xml`: engine options, manual/default DSP, EQ mode, learn scope,
  device DSP profiles, keyboard shortcuts, window bounds/layout, session
  (queue/position/volume).
- `artwork/`: content-hash named embedded artwork files.
- `presets/`: user DSP presets (JSON).

## Real-time-safety invariants

- The audio callback does not allocate, lock, or perform I/O.
- DSP parameter changes are handed over non-blockingly and smoothed per sample
  (or block) to avoid zipper noise and clicks.
- Metering is computed off the audio thread from lock-free FIFOs.
- Signal-path labels are derived from actual engine state (source decoder,
  resampling, active DSP steps, device format), never optimistic.
