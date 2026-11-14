/*
    Snake Puppy Reference Player
    Signal path honesty tests: the engine must only claim bit-perfect when every
    exclusive/integer/unprocessed condition holds.
*/
#include "TestFramework.h"
#include "Test_AudioHelpers.h"
#include "Audio/SignalPath.h"

using namespace sp;

namespace
{
using namespace spatest;

SignalPathInputs perfectInputs()
{
    SignalPathInputs in;
    in.hasTrack = true;
    in.sourceFile = "/music/track.flac";
    in.sourceFormat = "FLAC";
    in.decoderName = "JUCE FLAC reader";
    in.sourceSampleRate = 96000;
    in.sourceBitDepth = 24;
    in.sourceChannels = 2;
    in.sourceIsIntegerPcm = true;
    in.dspBypassed = true;
    in.volumeDb = 0.0f;
    in.outputDeviceName = "Test DAC";
    in.outputDeviceType = "Core Audio";
    in.outputSampleRate = 96000;
    in.outputBitDepth = 24;
    in.outputChannels = 2;
    in.hogModeActive = true;
    in.integerOutputFormat = true;
    return in;
}

SignalPathInfo evaluate (const SignalPathInputs& in)
{
    return evaluateSignalPath (in);
}

void expectQuality (const SignalPathInputs& in, SignalPathInfo::Quality expected, const char* label)
{
    const auto info = evaluate (in);
    std::cout << "        " << label << " -> " << info.qualityLabel()
              << " (" << (int) info.quality << ")\n";
    SP_REQUIRE_MSG (info.quality == expected,
                    juce::String (label) + ": quality " + juce::String ((int) info.quality)
                    + " != expected " + juce::String ((int) expected));
}

} // namespace

SP_TEST (AudioSignalPath, BitPerfectOnlyWhenEveryExclusiveConditionHolds)
{
    auto in = perfectInputs();
    expectQuality (in, SignalPathInfo::Quality::bitPerfect, "perfect 24/96 integer + hog");

    auto noHog = in;
    noHog.hogModeActive = false;
    expectQuality (noHog, SignalPathInfo::Quality::bitTransparent, "no hog mode");

    auto floatDevice = in;
    floatDevice.integerOutputFormat = false;
    floatDevice.outputBitDepth = 0;
    expectQuality (floatDevice, SignalPathInfo::Quality::bitTransparent, "float device format");

    auto lossy = in;
    lossy.sourceIsIntegerPcm = false;
    lossy.sourceBitDepth = 0;
    expectQuality (lossy, SignalPathInfo::Quality::bitTransparent, "lossy source");

    auto floatSource = in;
    floatSource.sourceIsIntegerPcm = false;
    expectQuality (floatSource, SignalPathInfo::Quality::bitTransparent, "float PCM source");

    auto dsd = in;
    dsd.sourceIsDsd = true;
    dsd.dsdMultiple = 64;
    dsd.sourceBitDepth = 1;
    dsd.sourceSampleRate = 176400;
    dsd.outputSampleRate = 176400;
    expectQuality (dsd, SignalPathInfo::Quality::processed, "DSD source (PCM conversion)");

    auto rateMismatch = in;
    rateMismatch.outputSampleRate = 48000;
    rateMismatch.resampling = true;
    rateMismatch.resampleFrom = 96000;
    rateMismatch.resampleTo = 48000;
    expectQuality (rateMismatch, SignalPathInfo::Quality::processed, "resampled");

    auto volume = in;
    volume.volumeDb = -6.0f;
    expectQuality (volume, SignalPathInfo::Quality::processed, "volume -6 dB");

    auto muted = in;
    muted.muted = true;
    expectQuality (muted, SignalPathInfo::Quality::processed, "muted");

    auto dsp = in;
    dsp.dspBypassed = false;
    dsp.dspProcessing = true;
    dsp.activeDspSteps = { "Graphic EQ", "Bass" };
    expectQuality (dsp, SignalPathInfo::Quality::processed, "DSP active");

    auto rg = in;
    rg.replayGainApplied = true;
    rg.replayGainDb = -3.0f;
    expectQuality (rg, SignalPathInfo::Quality::processed, "ReplayGain applied");

    auto dither = in;
    dither.ditherApplied = true;
    dither.ditherName = "TPDF 16-bit";
    expectQuality (dither, SignalPathInfo::Quality::processed, "dither active");

    auto depthReduction = in;
    depthReduction.outputBitDepth = 16;
    expectQuality (depthReduction, SignalPathInfo::Quality::bitTransparent,
                   "device 16-bit vs 24-bit source (not bit-perfect)");
}

SP_TEST (AudioSignalPath, NoTrackIsUnknown)
{
    SignalPathInputs in;
    expectQuality (in, SignalPathInfo::Quality::unknown, "no track");

    auto in2 = perfectInputs();
    in2.hasTrack = false;
    expectQuality (in2, SignalPathInfo::Quality::unknown, "hasTrack=false");
}

SP_TEST (AudioSignalPath, InfoFieldsAreFilledForThePanel)
{
    auto in = perfectInputs();
    in.ditherApplied = true;
    in.ditherName = "TPDF 24-bit";
    in.resampling = true;
    in.resampleFrom = 96000;
    in.resampleTo = 48000;
    in.outputSampleRate = 48000;
    in.bufferMs = 5.3;
    in.activeDspSteps = { "Graphic EQ" };
    in.dspBypassed = false;
    in.dspProcessing = true;
    in.volumeDb = -3.0f;
    in.replayGainApplied = true;

    const auto info = evaluate (in);
    SP_REQUIRE_MSG (info.decoderName == in.decoderName, "decoder name");
    SP_REQUIRE_MSG (info.sourceFormat == "FLAC", "source format");
    SP_REQUIRE (info.resampling);
    SP_REQUIRE_NEAR (info.resampleFrom, 96000.0, 0.001);
    SP_REQUIRE_NEAR (info.resampleTo, 48000.0, 0.001);
    SP_REQUIRE_NEAR (info.bufferMs, 5.3, 0.001);
    SP_REQUIRE (info.ditherApplied);
    SP_REQUIRE_MSG (info.ditherName == "TPDF 24-bit", info.ditherName);
    SP_REQUIRE (info.replayGainApplied);
    SP_REQUIRE (! info.volumeIsUnity);
    SP_REQUIRE (! info.dspBypassed);
    SP_REQUIRE_MSG (info.activeDspSteps.size() == 1, "dsp steps");
    SP_REQUIRE_MSG (info.summary().isNotEmpty(), "summary() must not be empty");
    std::cout << "        summary: " << info.summary() << "\n";
}
