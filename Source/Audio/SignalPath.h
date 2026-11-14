/*
    Snake Puppy Reference Player
    Signal path evaluation: turns the live engine/device state into the honest
    SignalPathInfo shown in the UI and diagnostics.

    The classification rules (never overclaim):
      - bitPerfect requires: no DSP, no ReplayGain, unity volume, no dithering,
        no resampling, an integer PCM source, the device running at the source
        rate, exclusive (hog) device access and an integer device format whose
        depth is at least the source depth.
      - bitTransparent means the float pipeline is untouched (no DSP/volume/RG/
        resampling) and the rate matches, but the exclusive integer conditions
        are not all met (e.g. no hog mode, or a float device format, or a lossy/
        float/DSD source).
      - processed covers anything that alters the samples: DSP, volume, mute,
        ReplayGain, resampling, dithering or DSD-to-PCM conversion.

    Threading: pure value function, callable from any thread.
*/
#pragma once

#include "Core/Types.h"
#include <juce_core/juce_core.h>
#include <vector>

namespace sp
{

struct SignalPathInputs
{
    bool hasTrack = false;

    juce::String sourceFile;
    juce::String sourceFormat;
    juce::String decoderName;
    int sourceSampleRate = 0;
    int sourceBitDepth = 0;
    int sourceChannels = 0;
    bool sourceIsDsd = false;
    int dsdMultiple = 0;
    bool sourceIsIntegerPcm = false;       // integer PCM (not lossy/float/DSD)
    bool sourceDownmixed = false;

    bool resampling = false;
    double resampleFrom = 0.0;
    double resampleTo = 0.0;
    juce::String resamplerName;

    std::vector<juce::String> activeDspSteps;
    bool dspBypassed = true;
    bool dspProcessing = false;            // any active processor, independent of bypass flag
    float volumeDb = 0.0f;
    bool muted = false;
    bool replayGainApplied = false;
    float replayGainDb = 0.0f;

    bool ditherApplied = false;
    juce::String ditherName;

    juce::String outputDeviceName;
    juce::String outputDeviceType;
    int outputSampleRate = 0;
    int outputBitDepth = 0;                // integer bits when known, 0 = float/unknown
    int outputChannels = 0;
    bool hogModeActive = false;
    bool integerOutputFormat = false;
    double bufferMs = 0.0;
};

SignalPathInfo evaluateSignalPath (const SignalPathInputs& inputs);

/** Convenience predicate used by tests and the engine: are all bit-perfect
    conditions satisfied? */
bool isBitPerfect (const SignalPathInputs& inputs);

} // namespace sp
