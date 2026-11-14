/*
    Snake Puppy Reference Player
    Offline track analysis for the intelligent EQ mode. Measured from decoded
    audio only (never from metadata): integrated loudness, true peak, crest
    factor, spectral centroid and band energy ratios. Runs on a background
    thread; results are cached in the library database by LibraryService.

    Measured quantities (see TrackAnalyzer.cpp for the full method notes):
      integratedLufs  : BS.1770-4 style K-weighted integrated loudness with the
                        -70 LUFS absolute gate and -10 LU relative gate. The
                        K-weighting filters are re-derived from the analogue
                        prototypes at the measured sample rate (a documented
                        simplification; 400 ms blocks, 100 ms hop).
      loudnessRangeLu : proxy for EBU R128 LRA: 95th - 10th percentile of 3 s
                        short-term loudness (1 s hop) above the -70 LUFS gate.
      truePeakDb      : maximum absolute sample after 4x oversampling
                        (JUCE half-band FIR oversampler) = dBTP.
      samplePeakDb    : maximum absolute sample value = dBFS.
      crestFactorDb   : samplePeakDb - RMS(dbFS) of the full-band signal, i.e.
                        how much the peaks stand out above the average level.
      spectralCentroid: power-weighted mean frequency of a Welch-averaged FFT
                        (2048-point Hann window, 50 % overlap, 20 Hz-16 kHz).
      *EnergyRatio    : band power / total 20 Hz-16 kHz power, where band power
                        is calibrated so the summed spectrum equals the signal
                        mean square:
                          sub  20-60 Hz   low 20-120 Hz
                          mid 120-2000 Hz high 6000-16000 Hz
      lowBandDb / midBandDb / highBandDb : absolute band energy (dBFS RMS).

    analyzeFile decodes at most kMaxAnalysisSeconds of the file. Decoding order:
      1. AudioEngine::decodeFileToBuffer (the playback decoder, handles DSD gain)
      2. DecodeSource::create + readFrames (format-agnostic decoder factory)
      3. A direct JUCE AudioFormatManager read, so offline analysis still works
         when the playback engine is busy or unavailable (WAV/AIFF/FLAC/MP3/OGG).
    If none of them can decode the file a clear error is returned; truncated
    streams are detected and reported as errors rather than analysed as if
    complete.

    Results are deterministic: no randomness, no time dependence, and repeated
    runs on the same file produce identical numbers.
*/
#pragma once

#include "Core/Types.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace sp
{

class TrackAnalyzer
{
public:
    /** Maximum amount of audio analysed per file (seconds). */
    static constexpr int kMaxAnalysisSeconds = 120;
    /** Decoded sample budget (~208 MB for stereo doubles) so that very high
        sample-rate sources cannot exhaust memory; 44.1-192 kHz sources keep the
        full kMaxAnalysisSeconds window. */
    static constexpr std::int64_t kMaxDecodedFrames = 13000000;

    /** Analyzes up to maxSeconds of a file (0 = auto, capped internally at
        kMaxAnalysisSeconds). Returns true on success with `out.valid == true`. */
    static bool analyzeFile (const juce::File& file,
                             TrackAnalysis& out,
                             int maxSeconds,
                             juce::String& errorOut);

    /** Analyzes an in-memory buffer at the given sample rate. Returns false only
        when there is nothing analysable (empty buffer or invalid rate); digital
        silence is valid and reports the -144 dB floor. */
    static bool analyzeBuffer (const juce::AudioBuffer<double>& buffer,
                               double sampleRate,
                               TrackAnalysis& out);

    static constexpr int kAnalyzerVersion = TrackAnalysis::kVersion;
};

} // namespace sp
