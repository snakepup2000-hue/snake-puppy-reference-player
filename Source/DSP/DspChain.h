/*
    Snake Puppy Reference Player
    Real-time 64-bit DSP chain interface.

    Threading contract:
      prepare/reset/setParams/loadImpulseResponse are called from the message
      or decode thread while audio is running. Implementations must keep the
      audio callback lock-free: parameter changes are handed off through a
      non-blocking mechanism and applied with smoothing.
      process() is called on the audio thread only, must not allocate or lock
      for any meaningful time.
*/
#pragma once

#include "Core/DspParams.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace sp
{

struct DspChainStatus
{
    double sampleRate = 0.0;
    int blockSize = 0;
    int channels = 2;
    bool anyProcessing = false;
    std::vector<juce::String> activeSteps;
    double maxPositiveBoostDb = 0.0;
    float autoHeadroomDb = 0.0f;        // negative value applied by auto headroom
    float effectivePreampDb = 0.0f;     // auto + manual
    float limiterReductionDb = 0.0f;    // current gain reduction (>= 0)
    bool limiterEngaged = false;
    bool convolutionLoaded = false;
    juce::String convolutionError;
};

class DspChain
{
public:
    DspChain();
    ~DspChain();

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    /** Hand new parameters to the chain. rampMs > 0 smooths every gain/frequency
        change to avoid clicks; the target params are visible immediately. */
    void setParams (const DspParams& newParams, int rampMs = 60);
    void setParamsImmediate (const DspParams& newParams);

    /** Thread-safe copy of the target (post-setParams, pre-smoothing) params. */
    DspParams getParams() const;

    /** ReplayGain for the currently loaded track. Called from the decode thread
        when a track is loaded; ramp smoothing applies. hasGain=false means the
        track has no ReplayGain tags (fallback gain used). */
    void setTrackReplayGain (float gainDb, float peakLinear, bool hasGain);

    /** In-place 64-bit processing, non-interleaved. */
    void process (juce::AudioBuffer<double>& buffer) noexcept;

    /** Magnitude response of the EQ + bass + filters sections for the graph.
        Frequencies in Hz, returns dB gain. */
    std::vector<float> getMagnitudeResponseDb (const std::vector<double>& freqsHz) const;

    DspChainStatus getStatus() const;

    //== Convolution ===========================================================
    void loadImpulseResponse (const juce::File& file, juce::String& errorOut);
    void clearImpulseResponse();
    bool isImpulseResponseLoaded() const;

    //== Metering taps (post-chain) ============================================
    float getLastOutputPeakDb() const;
    float getLimiterGainReductionDb() const;
    bool getLimiterEngaged() const;

    /** Total fixed processing latency in samples at the prepared sample rate.
        This is the look-ahead limiter delay when the limiter is enabled in the
        current params plus the convolution engine latency (zero for JUCE's
        default zero-latency convolution). Zero when fully bypassed. */
    int getLatencySamples() const;

    /** Number of frames whose pre-limiter true peak exceeded the ceiling since
        the last reset. Real measurement, never simulated. */
    std::int64_t getClippedSampleCount() const;
    void resetClippingCount();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DspChain)
};

} // namespace sp
