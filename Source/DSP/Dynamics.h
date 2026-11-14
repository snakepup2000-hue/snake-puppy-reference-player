/*
    Snake Puppy Reference Player
    True-peak detection (4x windowed-sinc oversampling) and a stereo-linked
    look-ahead true-peak limiter.

    The limiter:
      * delays the signal by (lookahead + detector delay) samples and reports
        that latency (DspChain exposes it);
      * computes the required gain for every input frame from the 4x oversampled
        peak, keeps a sliding minimum over the look-ahead window (instant attack,
        so transients cannot overshoot) and releases exponentially (default
        150 ms) so sustained material is not pumped;
      * is channel linked (the channel needing the most reduction drives both);
      * counts every frame whose pre-limiter peak exceeded the ceiling.

    Everything is prepared up front; process() is allocation and lock free.
*/
#pragma once

#include "DSP/DspSmoothing.h"
#include <array>
#include <cstdint>
#include <vector>

namespace sp
{

//==============================================================================
/** 4x oversampling true-peak detector using a Kaiser-windowed sinc polyphase
    interpolator (16 taps per phase). One frame is pushed per audio sample; the
    returned peak corresponds to the oversampled waveform roughly 7.5 input
    samples behind the newest sample (see latencySamples()). */
class TruePeakDetector
{
public:
    static constexpr int kMaxChannels = 8;
    static constexpr int kTapsPerPhase = 16;
    static constexpr int kPhases = 4;
    static constexpr int kTotalTaps = kTapsPerPhase * kPhases;

    TruePeakDetector();

    void prepare (int numChannels);
    void reset();

    /** Push one sample for one channel of the current frame (all channels before
        advanceFrame()). */
    inline void push (int channel, double sample) noexcept
    {
        history[(size_t) channel][(size_t) pos] = sample;
    }

    /** Peak of the 4 oversampled values for the frame just pushed. */
    double peak (int channel) const noexcept;

    void advanceFrame() noexcept { pos = (pos + 1) % kTapsPerPhase; }

    /** Delay of the estimate in input samples (used to size the limiter delay). */
    static constexpr int latencySamples()
    {
        return (kTotalTaps + 6) / 8;   // ceil((4 * taps - 1) / 8)
    }

private:
    std::array<std::array<double, kTapsPerPhase>, kMaxChannels> history {};
    std::array<std::array<double, kTapsPerPhase>, kPhases> coeffs {};
    int pos = 0;
};

//==============================================================================
/** Stereo-linked look-ahead true-peak limiter. */
class Limiter
{
public:
    static constexpr int kMaxChannels = 8;

    Limiter();

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    /** Ceiling with a smooth ramp (dBTP). */
    void setCeilingDb (double db, int rampSamples);
    void setReleaseMs (double ms);

    void process (double* const* channels, int numChannels, int numFrames) noexcept;

    int latencySamples() const noexcept { return totalDelay; }
    double currentGainReductionDb() const noexcept;
    bool isEngaged() const noexcept { return currentGain < 0.999999; }
    std::int64_t clipCount() const noexcept { return clippedFrames; }
    void resetClipCount() noexcept { clippedFrames = 0; }

private:
    double sampleRate = 48000.0;
    int maxBlock = 512;
    int numChannelsPrepared = 2;
    int lookaheadSamples = 240;
    int detectorDelay = 8;
    int totalDelay = 248;
    int delayCapacity = 1024;
    int writePos = 0;

    TruePeakDetector detector;

    std::vector<double> delayLine;          // kMaxChannels * delayCapacity
    std::vector<double> minDequeGain;
    std::vector<std::int64_t> minDequeLabel;
    int dequeHead = 0;
    int dequeCount = 0;
    std::int64_t labelCounter = 0;

    SampleRamp ceilingGain { 1.0 };
    double releaseCoeff = 0.9999;
    double attackCoeff = 0.1;
    double currentGain = 1.0;
    std::int64_t clippedFrames = 0;
    bool prepared = false;
};

} // namespace sp
