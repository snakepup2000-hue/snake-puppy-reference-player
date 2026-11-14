/*
    Snake Puppy Reference Player
    Stereo processing: balance, mid/side width with a mono-compatibility
    safeguard, optional mono-below-frequency and Bauer-style headphone
    crossfeed (low-pass + interaural delay).

    The three stages are exposed separately so the DSP chain can run them in the
    user-configured slot order:
      processBalance()   - DspSlot::balance
      processWidth()     - DspSlot::stereoWidth (includes mono bass + safeguard)
      processCrossfeed() - DspSlot::crossfeed

    All parameter changes are smoothed; there is no allocation in process().
*/
#pragma once

#include "Core/DspParams.h"
#include "DSP/Biquad.h"
#include "DSP/DspSmoothing.h"
#include <array>
#include <vector>

namespace sp
{

class StereoProcessor
{
public:
    static constexpr int kMaxChannels = 8;
    static constexpr int kCrossfeedDelaySize = 4096;

    StereoProcessor();

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    void setParams (const StereoParams& params, int rampSamples);

    void processBalance (double* const* channels, int numChannels, int numFrames) noexcept;
    void processWidth (double* const* channels, int numChannels, int numFrames) noexcept;
    void processCrossfeed (double* const* channels, int numChannels, int numFrames) noexcept;

    /** Current effective width in percent (0..200), after the mono safeguard. */
    double effectiveWidthPercent() const noexcept { return effectiveWidth * 100.0; }

private:
    void updateMonoBassCoefficients (int numFrames) noexcept;

    double sampleRate = 48000.0;
    int maxBlock = 512;
    int numChannelsPrepared = 2;
    int rampSamples = 0;

    // Balance
    SampleRamp balanceLeft { 1.0 };
    SampleRamp balanceRight { 1.0 };

    // Width / M-S
    SampleRamp width { 1.0 };
    double effectiveWidth = 1.0;
    bool protectMono = true;
    double midEnergy = 0.0;
    double sideEnergy = 0.0;
    double energyCoeff = 0.999;

    // Mono bass
    bool monoBassEnabled = false;
    LinearSmooth monoBassLog2Cutoff;
    SampleRamp monoBassMix { 0.0 };
    std::array<BiquadCoeffs, 2> monoBassCoeffs;
    bool monoBassCoeffValid = false;
    double monoBassDesignedCutoff = 0.0;
    std::array<BiquadState, 2> monoBassState;

    // Crossfeed
    bool crossfeedEnabled = false;
    LinearSmooth crossfeedLog2Cutoff;
    LinearSmooth crossfeedDelaySamples;
    SampleRamp crossfeedGain { 0.0 };
    BiquadCoeffs crossfeedCoeffs;
    bool crossfeedCoeffValid = false;
    double crossfeedDesignedCutoff = 0.0;
    std::array<BiquadState, 2> crossfeedState;   // 0: LP of delayed R, 1: LP of delayed L
    std::array<std::array<double, kCrossfeedDelaySize>, 2> crossfeedDelay {};
    int crossfeedWritePos = 0;
    bool crossfeedWasActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StereoProcessor)
};

} // namespace sp
