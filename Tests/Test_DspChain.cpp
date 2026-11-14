/*
    DSP tests: chain transparency, click-free smoothing, ReplayGain, configurable
    chain order, Reference Mode bypass, status honesty and convolution.
*/
#include "Test_DspUtils.h"

#include "DSP/DspChain.h"

#include <cmath>
#include <vector>

using namespace sptest;
using namespace sptest::dsp;
using namespace sp;

//==============================================================================
SP_TEST (Dsp, ChainTrueBypassIsBitExact)
{
    const double fs = 48000.0;
    auto p = makeNeutralParams();
    p.bypassed = true;

    DspChain chain;
    chain.prepare (fs, 512, 2);
    chain.setParamsImmediate (p);

    auto input = makeSine (fs, 1.0, 997.0, 0.75, 2);
    auto output = input;
    processThroughChain (chain, output, 512);

    double maxDifference = 0.0;
    for (int ch = 0; ch < 2; ++ch)
        for (int n = 0; n < output.getNumSamples(); ++n)
            maxDifference = std::max (maxDifference, std::abs (output.getReadPointer (ch)[n]
                                                               - input.getReadPointer (ch)[n]));

    const auto status = chain.getStatus();
    printValue ("Reference Mode max sample difference", maxDifference);
    printValue ("Reference Mode latency (samples)", (double) chain.getLatencySamples());
    printValue ("Reference Mode active steps", (double) status.activeSteps.size());

    SP_REQUIRE (maxDifference == 0.0);
    SP_REQUIRE (status.activeSteps.empty());
    SP_REQUIRE (! status.anyProcessing);
    SP_REQUIRE (chain.getLatencySamples() == 0);
}

//==============================================================================
SP_TEST (Dsp, ChainTransparentWhenNoProcessing)
{
    const double fs = 48000.0;
    auto p = makeNeutralParams();

    DspChain chain;
    chain.prepare (fs, 512, 2);
    chain.setParamsImmediate (p);

    auto input = makeSine (fs, 1.0, 1000.0, 0.5, 2);
    auto output = input;
    processThroughChain (chain, output, 512);

    const double nullDb = nullTestDb (output, input, 0, 0, input.getNumSamples());
    const auto status = chain.getStatus();
    printValue ("neutral chain null test (dB)", nullDb);
    printValue ("neutral chain active steps", (double) status.activeSteps.size());
    SP_REQUIRE_LT (nullDb, -150.0);
    SP_REQUIRE (status.activeSteps.empty());
}

//==============================================================================
SP_TEST (Dsp, ChainLimiterDelayedNullTest)
{
    const double fs = 48000.0;
    auto p = makeNeutralParams();
    p.preamp.limiterEnabled = true;

    DspChain chain;
    chain.prepare (fs, 512, 2);
    chain.setParamsImmediate (p);

    const int latency = chain.getLatencySamples();
    auto input = makeSine (fs, 1.5, 440.0, 0.2, 2);
    auto output = input;
    processThroughChain (chain, output, 512);

    const int compare = input.getNumSamples() - latency;
    const double nullDb = nullTestDb (output, input, latency, 0, compare);
    printValue ("limiter-enabled below-ceiling null test vs delayed input (dB)", nullDb);
    SP_REQUIRE_LT (nullDb, -100.0);
}

//==============================================================================
SP_TEST (Dsp, ChainParameterSmoothingClickFree)
{
    const double fs = 48000.0;
    DspChain chain;
    chain.prepare (fs, 512, 2);

    const int totalFrames = (int) (5.0 * fs);
    const int changeFrame = (int) (1.0 * fs);
    const int profileFrame = (int) (2.5 * fs);

    juce::AudioBuffer<double> buffer (2, totalFrames);
    fillSine (buffer, 1000.0, 0.5, fs);

    auto p0 = makeNeutralParams();
    auto p1 = makeNeutralParams();
    p1.bass.gainDb = 6.0f;

    // A completely different profile: graphic band-set switch, parametric
    // bands, a high pass and a stereo width change all appear at once.
    auto p2 = makeNeutralParams();
    p2.graphic.bandSet = GraphicEqParams::BandSet::bands31;
    p2.graphic.gains31.fill (0.0f);
    p2.graphic.gains31[10] = -6.0f;
    p2.graphic.gains31[17] = 6.0f;
    p2.parametric.bands[0].enabled = true;
    p2.parametric.bands[0].type = BandType::bell;
    p2.parametric.bands[0].freqHz = 400.0;
    p2.parametric.bands[0].gainDb = 4.0;
    p2.parametric.bands[0].q = 1.0;
    p2.bass.gainDb = -4.0f;
    p2.filters.highPassEnabled = true;
    p2.filters.highPassHz = 40.0;
    p2.stereo.widthPercent = 130.0f;

    chain.setParamsImmediate (p0);

    for (int start = 0; start < totalFrames; start += 512)
    {
        if (start >= changeFrame && start - 512 < changeFrame)
            chain.setParams (p1, 200);   // 200 ms ramp, mid playback
        if (start >= profileFrame && start - 512 < profileFrame)
            chain.setParams (p2, 100);   // profile switch, mid playback

        const int n = std::min (512, totalFrames - start);
        juce::AudioBuffer<double> view (buffer.getArrayOfWritePointers(), 2, start, n);
        chain.process (view);
    }

    const int beforeStart = (int) (0.4 * fs);
    const int beforeLength = (int) (0.4 * fs);
    const int transitionStart = (int) (0.95 * fs);
    const int transitionLength = (int) (0.6 * fs);
    const int profileStart = (int) (2.45 * fs);
    const int profileLength = (int) (0.8 * fs);

    const double steadyStep = maxSampleStep (buffer, 0, beforeStart, beforeLength);
    const double transitionStep = maxSampleStep (buffer, 0, transitionStart, transitionLength);
    const double profileStep = maxSampleStep (buffer, 0, profileStart, profileLength);

    // A 1 kHz sine at amplitude A has a maximum first difference of
    // 2 * A * sin(pi * f / fs). The bass ramp can momentarily double the
    // amplitude (0.5 -> 1.0), so 0.5 * 2 = 1.0 is the worst-case amplitude.
    const double theoretical = 2.0 * 1.0 * std::sin (pi() * 1000.0 / fs);
    const double threshold = 1.5 * theoretical;

    printValue ("steady-state max sample step", steadyStep);
    printValue ("bass-ramp transition max sample step", transitionStep);
    printValue ("full profile switch max sample step (graphic set 15 -> 31, param, HPF, width)", profileStep);
    printValue ("theoretical 1 kHz sine max step at amplitude 1.0", theoretical);
    printValue ("click threshold", threshold);

    SP_REQUIRE_LT (transitionStep, threshold);
    SP_REQUIRE_LT (profileStep, threshold);
    SP_REQUIRE (! containsNonFinite (buffer));
    SP_REQUIRE_LT (std::abs (transitionStep - steadyStep), 0.15);
    SP_REQUIRE_LT (std::abs (profileStep - steadyStep), 0.15);
}

//==============================================================================
SP_TEST (Dsp, ChainReplayGain)
{
    const double fs = 48000.0;

    auto measureWithRg = [&] (float trackGainDb, float peakLinear, bool hasGain, bool preventClipping,
                              float fallbackDb) -> double
    {
        auto p = makeNeutralParams();
        p.replayGain.enabled = true;
        p.replayGain.preventClipping = preventClipping;
        p.replayGain.fallbackGainDb = fallbackDb;

        DspChain chain;
        chain.prepare (fs, 512, 2);
        chain.setParamsImmediate (p);
        chain.setTrackReplayGain (trackGainDb, peakLinear, hasGain);

        auto input = makeSine (fs, 1.5, 1000.0, 0.25, 2);
        auto output = input;
        processThroughChain (chain, output, 512);

        const int warmup = (int) (0.3 * fs);
        const auto m = measureTone (output.getReadPointer (0), warmup,
                                    output.getNumSamples() - warmup, 1000.0, fs);
        return toDb (m.amplitude / 0.25);
    };

    const double track = measureWithRg (-6.0f, 0.0f, true, false, 0.0f);
    printValue ("ReplayGain -6 dB applied (dB)", track);
    SP_REQUIRE_NEAR (track, -6.0, 0.1);

    const double fallback = measureWithRg (0.0f, 0.0f, false, false, -3.0f);
    printValue ("ReplayGain fallback -3 dB with no tags (dB)", fallback);
    SP_REQUIRE_NEAR (fallback, -3.0, 0.1);

    // Requested +12 dB, but peak 0.5 means +6.02 dB is the largest boost that
    // cannot clip: -20*log10(0.5) = +6.0206 dB.
    const double clipped = measureWithRg (12.0f, 0.5f, true, true, 0.0f);
    printValue ("ReplayGain +12 dB with peak 0.5 and clipping prevention (dB)", clipped);
    SP_REQUIRE_NEAR (clipped, 6.0206, 0.15);

    const double preamp = measureWithRg (-6.0f, 0.0f, true, false, 0.0f);
    printValue ("ReplayGain repeatability (dB)", preamp);
    SP_REQUIRE_NEAR (preamp, track, 1.0e-6);
}

//==============================================================================
SP_TEST (Dsp, ChainOrderIsHonoured)
{
    const double fs = 48000.0;
    auto makeOrder = [] (bool limiterFirst)
    {
        DspParams p;
        p.reset();
        p.replayGain.enabled = false;
        p.graphic.enabled = false;
        p.parametric.enabled = false;
        p.bass.enabled = false;
        p.preamp.autoHeadroom = false;
        p.preamp.limiterEnabled = true;
        p.preamp.manualPreampDb = 6.0f;

        std::array<DspSlot, 11> order {};
        if (limiterFirst)
            order = { DspSlot::limiter, DspSlot::preamp, DspSlot::replayGain, DspSlot::graphicEq,
                      DspSlot::parametricEq, DspSlot::bass, DspSlot::filters, DspSlot::convolution,
                      DspSlot::crossfeed, DspSlot::stereoWidth, DspSlot::balance };
        else
            order = { DspSlot::preamp, DspSlot::limiter, DspSlot::replayGain, DspSlot::graphicEq,
                      DspSlot::parametricEq, DspSlot::bass, DspSlot::filters, DspSlot::convolution,
                      DspSlot::crossfeed, DspSlot::stereoWidth, DspSlot::balance };
        p.chainOrder = order;
        return p;
    };

    auto measurePeak = [&] (const DspParams& p, std::vector<juce::String>& activeSteps)
    {
        DspChain chain;
        chain.prepare (fs, 512, 2);
        chain.setParamsImmediate (p);

        auto input = makeSine (fs, 2.0, 1000.0, 0.5, 2);
        auto output = input;
        processThroughChain (chain, output, 512);

        const int latency = chain.getLatencySamples();
        const int warmup = latency + (int) (0.3 * fs);
        const auto status = chain.getStatus();
        activeSteps = status.activeSteps;
        return toDb (peakAbs (output, 0, warmup, output.getNumSamples() - warmup));
    };

    std::vector<juce::String> stepsDefault;
    const double peakDefault = measurePeak (makeOrder (false), stepsDefault);
    std::vector<juce::String> stepsReversed;
    const double peakReversed = measurePeak (makeOrder (true), stepsReversed);

    printValue ("preamp -> limiter order: output peak (dBFS)", peakDefault);
    printValue ("limiter -> preamp order: output peak (dBFS)", peakReversed);

    std::cout << "        preamp -> limiter active steps:";
    for (auto& s : stepsDefault) std::cout << " [" << s << "]";
    std::cout << "\n        limiter -> preamp active steps:";
    for (auto& s : stepsReversed) std::cout << " [" << s << "]";
    std::cout << "\n";

    SP_REQUIRE_LE (peakDefault, -0.3 + 0.2);
    SP_REQUIRE_GT (peakReversed, -0.1);
    SP_REQUIRE (stepsReversed.size() >= 2);
    SP_REQUIRE (stepsReversed[0] == dspSlotName (DspSlot::limiter));
    SP_REQUIRE (stepsReversed[1] == dspSlotName (DspSlot::preamp));
    SP_REQUIRE (stepsDefault[0] == dspSlotName (DspSlot::preamp));
    SP_REQUIRE (stepsDefault[1] == dspSlotName (DspSlot::limiter));
}

//==============================================================================
SP_TEST (Dsp, ChainStatusHonesty)
{
    const double fs = 48000.0;
    auto p = makeNeutralParams();
    p.graphic.gains15.fill (0.0f);
    p.graphic.gains15[8] = 6.0f;
    p.preamp.autoHeadroom = true;
    p.preamp.limiterEnabled = true;

    DspChain chain;
    chain.prepare (fs, 512, 2);
    chain.setParamsImmediate (p);

    auto status = chain.getStatus();
    std::cout << "        active steps:";
    for (auto& s : status.activeSteps) std::cout << " [" << s << "]";
    std::cout << "\n";
    printValue ("maxPositiveBoostDb", status.maxPositiveBoostDb);
    printValue ("autoHeadroomDb", status.autoHeadroomDb);
    printValue ("effectivePreampDb", status.effectivePreampDb);
    printValue ("anyProcessing", status.anyProcessing);

    SP_REQUIRE (status.anyProcessing);
    SP_REQUIRE_NEAR (status.maxPositiveBoostDb, 6.0, 0.2);
    SP_REQUIRE_NEAR (status.autoHeadroomDb, -6.0, 0.2);
    SP_REQUIRE_NEAR (status.effectivePreampDb, -6.0, 0.2);

    bool sawGraphic = false, sawLimiter = false, sawPreamp = false;
    for (auto& s : status.activeSteps)
    {
        if (s == dspSlotName (DspSlot::graphicEq)) sawGraphic = true;
        if (s == dspSlotName (DspSlot::limiter)) sawLimiter = true;
        if (s == dspSlotName (DspSlot::preamp)) sawPreamp = true;
    }
    SP_REQUIRE (sawGraphic);
    SP_REQUIRE (sawLimiter);
    SP_REQUIRE (sawPreamp);

    // eqBypass removes the EQ steps but not the limiter.
    p.eqBypass = true;
    chain.setParamsImmediate (p);
    status = chain.getStatus();
    bool graphicAfterBypass = false;
    for (auto& s : status.activeSteps)
        if (s == dspSlotName (DspSlot::graphicEq))
            graphicAfterBypass = true;
    SP_REQUIRE (! graphicAfterBypass);
    printValue ("eqBypass active step count", (double) status.activeSteps.size());
}

//==============================================================================
SP_TEST (Dsp, ChainConvolution)
{
    const double fs = 48000.0;
    const juce::File irFile (juce::String (SP_SOURCE_DIR) + "/Tests/Fixtures/ir-short.wav");
    SP_REQUIRE_MSG (irFile.existsAsFile(), irFile.getFullPathName());

    auto p = makeNeutralParams();
    p.convolution.enabled = true;
    p.convolution.mixPercent = 100.0f;

    DspChain chain;
    chain.prepare (fs, 512, 2);

    juce::String error;
    chain.loadImpulseResponse (irFile, error);
    printValue ("convolution load ok", error.isEmpty() ? 1.0 : 0.0);
    if (error.isNotEmpty())
        std::cout << "        convolution error: " << error << "\n";
    SP_REQUIRE_MSG (error.isEmpty(), error);
    SP_REQUIRE (chain.isImpulseResponseLoaded());

    chain.setParamsImmediate (p);

    // Give JUCE's background IR installation a moment, then run the test signal.
    auto silence = makeSilence (fs, 0.3, 2);
    for (int i = 0; i < 8; ++i)
    {
        processThroughChain (chain, silence, 512);
        juce::Thread::sleep (10);
    }

    auto input = makeSilence (fs, 0.5, 2);
    // Impulse plus noise so the IR effect is clearly measurable.
    for (int ch = 0; ch < 2; ++ch)
    {
        auto* data = input.getWritePointer (ch);
        data[100] = 1.0;
        for (int n = 200; n < input.getNumSamples(); ++n)
            data[n] = 0.2 * std::sin (2.0 * pi() * 500.0 * (double) n / fs);
    }

    auto output = input;
    processThroughChain (chain, output, 512);

    const double differenceDb = nullTestDb (output, input, 0, 0, input.getNumSamples());
    printValue ("convolution wet vs dry difference (dB)", differenceDb);
    printValue ("convolution output contains non-finite", containsNonFinite (output) ? 1.0 : 0.0);

    SP_REQUIRE_GT (differenceDb, -6.0);   // convolution must audibly change the signal
    SP_REQUIRE (! containsNonFinite (output));

    auto status = chain.getStatus();
    printValue ("convolution loaded status", status.convolutionLoaded ? 1.0 : 0.0);
    SP_REQUIRE (status.convolutionLoaded);

    // Mix 0% must leave the signal exactly untouched.
    p.convolution.mixPercent = 0.0f;
    chain.setParamsImmediate (p);
    auto output2 = input;
    processThroughChain (chain, output2, 512);
    const double dryDb = nullTestDb (output2, input, 0, 0, input.getNumSamples());
    printValue ("convolution mix 0% null test (dB)", dryDb);
    SP_REQUIRE_LT (dryDb, -150.0);

    chain.clearImpulseResponse();
    SP_REQUIRE (! chain.isImpulseResponseLoaded());
}

//==============================================================================
SP_TEST (Dsp, ChainPreparesAtAllRates)
{
    for (double fs : { 44100.0, 96000.0, 192000.0, 384000.0 })
    {
        auto p = makeNeutralParams();
        p.graphic.gains15.fill (0.0f);
        p.graphic.gains15[8] = 6.0f;             // +6 dB at 1 kHz
        p.preamp.limiterEnabled = true;
        p.preamp.outputCeilingDb = -0.3f;

        DspChain chain;
        chain.prepare (fs, 256, 2);
        chain.setParams (p, 60);                 // ramped transition, as at runtime

        const int total = (int) std::lround (1.5 * fs);
        juce::AudioBuffer<double> buffer (2, total);
        fillSine (buffer, 1000.0, 0.25, fs);

        const int blockSize = 256;
        for (int start = 0; start < total; start += blockSize)
        {
            const int n = std::min (blockSize, total - start);
            juce::AudioBuffer<double> view (buffer.getArrayOfWritePointers(), 2, start, n);
            chain.process (view);
        }

        const int warmup = (int) std::lround (0.5 * fs);
        const int count = (int) std::lround (0.2 * fs);   // 200 cycles at 1 kHz
        const auto measurement = measureTone (buffer.getReadPointer (0), warmup, count, 1000.0, fs);
        const double gainDb = toDb (measurement.amplitude / 0.25);

        std::cout << "        " << fs << " Hz prepare: measured +6 dB graphic gain = " << gainDb
                  << " dB, latency " << chain.getLatencySamples() << " samples, finite "
                  << (containsNonFinite (buffer) ? 0 : 1) << "\n";

        SP_REQUIRE_NEAR (gainDb, 6.0, 0.6);
        SP_REQUIRE (! containsNonFinite (buffer));
        SP_REQUIRE_GT (chain.getLatencySamples(), 0);
        SP_REQUIRE_NEAR (chain.getStatus().sampleRate, fs, 1.0e-9);
    }
}

//==============================================================================
SP_TEST (Dsp, ChainRealtimePerformance)
{
    struct Result { double fs; double cpuPercent; double realtimeFactor; };
    std::vector<Result> results;

    for (double fs : { 48000.0, 192000.0 })
    {
        auto p = makeNeutralParams();
        p.graphic.bandSet = GraphicEqParams::BandSet::bands31;
        for (int i = 0; i < GraphicEqParams::kNumBands31; ++i)
            p.graphic.gains31[(size_t) i] = (i % 3 == 0) ? 3.0f : ((i % 3 == 1) ? -2.0f : 1.0f);
        for (int i = 0; i < ParametricEqParams::kNumBands; ++i)
        {
            p.parametric.bands[(size_t) i].enabled = i < 8;
            p.parametric.bands[(size_t) i].type = (i % 4 == 0) ? BandType::bell
                                              : (i % 4 == 1) ? BandType::lowShelf
                                              : (i % 4 == 2) ? BandType::highShelf : BandType::bell;
            p.parametric.bands[(size_t) i].freqHz = 80.0 * std::pow (1.7, i);
            p.parametric.bands[(size_t) i].gainDb = (i % 2 == 0) ? 3.0 : -3.0;
            p.parametric.bands[(size_t) i].q = 1.2;
        }
        p.bass.gainDb = 4.0f;
        p.bass.subEnabled = true;
        p.bass.subGainDb = 3.0f;
        p.bass.midBassEnabled = true;
        p.bass.midBassGainDb = 2.0f;
        p.bass.punchEnabled = true;
        p.bass.punchGainDb = 2.0f;
        p.filters.highPassEnabled = true;
        p.filters.highPassHz = 25.0;
        p.filters.highPassSlopeDbPerOct = 24;
        p.filters.lowPassEnabled = true;
        p.filters.lowPassHz = 0.45 * fs;
        p.filters.lowPassSlopeDbPerOct = 24;
        p.stereo.widthPercent = 120.0f;
        p.preamp.autoHeadroom = true;
        p.preamp.limiterEnabled = true;

        DspChain chain;
        chain.prepare (fs, 512, 2);
        chain.setParamsImmediate (p);

        const double seconds = 2.0;
        const int frames = (int) std::lround (seconds * fs);
        juce::AudioBuffer<double> buffer (2, frames);
        fillSine (buffer, 1000.0, 0.3, fs);
        // Add some high-frequency content so the filters do real work.
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int n = 0; n < frames; ++n)
                data[n] += 0.1 * std::sin (2.0 * pi() * 9000.0 * (double) n / fs);
        }

        const auto startTime = juce::Time::getMillisecondCounterHiRes();
        const int blockSize = 512;
        for (int start = 0; start < frames; start += blockSize)
        {
            const int n = std::min (blockSize, frames - start);
            juce::AudioBuffer<double> view (buffer.getArrayOfWritePointers(), 2, start, n);
            chain.process (view);
        }
        const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - startTime;
        const double audioMs = seconds * 1000.0;
        const double cpuPercent = 100.0 * elapsedMs / audioMs;
        const double realtimeFactor = elapsedMs / audioMs;

        std::cout << "        " << fs << " Hz stereo full chain: " << (double) frames << " frames processed in "
                  << elapsedMs << " ms (" << cpuPercent << "% of one core, " << (1.0 / realtimeFactor)
                  << "x real time)\n";

        results.push_back ({ fs, cpuPercent, realtimeFactor });
        SP_REQUIRE (! containsNonFinite (buffer));
        SP_REQUIRE_LT (realtimeFactor, 1.0);   // must be faster than real time on this machine
    }

    SP_REQUIRE (results.size() == 2);
}
