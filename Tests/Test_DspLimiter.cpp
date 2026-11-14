/*
    DSP tests: look-ahead true-peak limiter, measured through the real DspChain.
*/
#include "Test_DspUtils.h"

#include "DSP/Dynamics.h"

#include <cmath>
#include <vector>

using namespace sptest;
using namespace sptest::dsp;
using namespace sp;

namespace
{
double measureTruePeakDb (const juce::AudioBuffer<double>& buffer, int startSample, int endSample = -1)
{
    TruePeakDetector detector;
    detector.prepare (2);
    double peak = 0.0;
    const int end = endSample > 0 ? std::min (endSample, buffer.getNumSamples())
                                  : buffer.getNumSamples();
    // Prime the oversampling filter so the measurement window does not include
    // its own start-up transient.
    const int primeFrom = std::max (0, startSample - 128);

    for (int i = primeFrom; i < end; ++i)
    {
        detector.push (0, buffer.getReadPointer (0)[i]);
        detector.push (1, buffer.getReadPointer (1)[i]);

        if (i >= startSample)
            peak = std::max (peak, std::max (detector.peak (0), detector.peak (1)));

        detector.advanceFrame();
    }
    return toDb (peak);
}
} // namespace

//==============================================================================
SP_TEST (Dsp, LimiterCeilingSampleAndTruePeak)
{
    const double fs = 48000.0;
    auto p = makeNeutralParams();
    p.preamp.limiterEnabled = true;
    p.preamp.outputCeilingDb = -0.3f;
    p.preamp.limiterReleaseMs = 150.0f;

    DspChain chain;
    chain.prepare (fs, 512, 2);
    chain.setParamsImmediate (p);

    const int latency = chain.getLatencySamples();
    auto input = makeSine (fs, 3.0, 1000.0, 1.0, 2);
    auto output = input;
    processThroughChain (chain, output, 512);

    const int warmup = latency + (int) (0.5 * fs);
    const double samplePeakDb = toDb (peakAbs (output, 0, warmup, output.getNumSamples() - warmup));
    const double truePeakDb = measureTruePeakDb (output, warmup, output.getNumSamples() - 64);
    const auto status = chain.getStatus();

    printValue ("1 kHz 0 dBFS input, ceiling -0.3 dBTP", 0.0);
    printValue ("limiter latency (samples)", (double) latency);
    printValue ("output sample peak (dBFS)", samplePeakDb);
    printValue ("output true peak (dBTP, 4x oversampled)", truePeakDb);
    printValue ("limiter gain reduction (dB)", (double) status.limiterReductionDb);

    SP_REQUIRE_LE (samplePeakDb, -0.3 + 0.15);
    SP_REQUIRE_LE (truePeakDb, -0.3 + 0.35);
    SP_REQUIRE_GT (status.limiterReductionDb, 0.1f);

    // Intersample-over signal: fs/4 with 45 degree phase has 0 dBTP continuous
    // peak but only -3 dBFS sample peaks.
    juce::AudioBuffer<double> isp (2, (int) fs * 2);
    for (int ch = 0; ch < 2; ++ch)
        for (int n = 0; n < isp.getNumSamples(); ++n)
            isp.getWritePointer (ch)[n] = std::cos (2.0 * pi() * (fs / 4.0) * (double) n / fs + pi() / 4.0);

    chain.reset();
    chain.setParamsImmediate (p);
    auto ispOut = isp;
    processThroughChain (chain, ispOut, 512);
    const double ispSamplePeakDb = toDb (peakAbs (ispOut, 0, warmup, ispOut.getNumSamples() - warmup));
    const double ispTruePeakDb = measureTruePeakDb (ispOut, warmup, ispOut.getNumSamples() - 64);
    const auto ispStatus = chain.getStatus();

    printValue ("intersample-overs input sample peak (dBFS)", -3.01);
    printValue ("intersample-overs input true peak (dBTP)", 0.0);
    printValue ("intersample-overs output sample peak (dBFS)", ispSamplePeakDb);
    printValue ("intersample-overs output true peak (dBTP)", ispTruePeakDb);
    printValue ("intersample-overs limiter reduction (dB)", (double) ispStatus.limiterReductionDb);

    SP_REQUIRE_LE (ispTruePeakDb, -0.3 + 0.35);
    // The 4x detector's grid lands between the cosine's true peaks for this
    // fs/4 signal, so the actually required reduction is about 0.13 dB.
    SP_REQUIRE_GT (ispStatus.limiterReductionDb, 0.05f);
}

//==============================================================================
SP_TEST (Dsp, LimiterQuietMaterialTransparent)
{
    const double fs = 48000.0;
    auto p = makeNeutralParams();
    p.preamp.limiterEnabled = true;
    p.preamp.outputCeilingDb = -0.3f;

    DspChain chain;
    chain.prepare (fs, 512, 2);
    chain.setParamsImmediate (p);

    const int latency = chain.getLatencySamples();
    auto input = makeSine (fs, 2.0, 440.0, 0.1, 2);   // -20 dBFS peak, well below ceiling
    auto output = input;
    processThroughChain (chain, output, 512);

    const auto status = chain.getStatus();
    const int compare = input.getNumSamples() - latency;
    const double nullDb = nullTestDb (output, input, latency, 0, compare);
    const double peak = peakAbs (output, 0, latency, compare);

    printValue ("quiet material null test vs delayed input (dB)", nullDb);
    printValue ("quiet material output peak (dBFS)", toDb (peak));
    printValue ("limiter gain reduction on quiet material (dB)", (double) status.limiterReductionDb);

    SP_REQUIRE_LT (nullDb, -100.0);
    SP_REQUIRE_LT (status.limiterReductionDb, 0.01f);
    SP_REQUIRE (! containsNonFinite (output));
}

//==============================================================================
SP_TEST (Dsp, LimiterReleaseBehaviour)
{
    const double fs = 48000.0;
    auto p = makeNeutralParams();
    p.preamp.limiterEnabled = true;
    p.preamp.outputCeilingDb = -0.3f;
    p.preamp.limiterReleaseMs = 150.0f;

    DspChain chain;
    chain.prepare (fs, 512, 2);
    chain.setParamsImmediate (p);

    const int latency = chain.getLatencySamples();
    const int loudFrames = (int) (0.25 * fs);
    const int quietFrames = (int) (1.0 * fs);

    juce::AudioBuffer<double> input (2, loudFrames + quietFrames);
    for (int ch = 0; ch < 2; ++ch)
    {
        for (int n = 0; n < loudFrames; ++n)
            input.getWritePointer (ch)[n] = 1.6 * std::sin (2.0 * pi() * 1000.0 * (double) n / fs);
        for (int n = loudFrames; n < input.getNumSamples(); ++n)
            input.getWritePointer (ch)[n] = 0.05 * std::sin (2.0 * pi() * 1000.0 * (double) (n - loudFrames) / fs);
    }

    auto output = input;
    processThroughChain (chain, output, 512);

    // Peak within the loud section (skip the limiter latency and initial ramp).
    const double loudPeak = peakAbs (output, 0, latency + 2400, loudFrames - 2400);
    // Reduction reported at the very end (after a full second of quiet).
    const auto status = chain.getStatus();
    const double quietPeak = peakAbs (output, 0, latency + loudFrames + 2400, quietFrames - 2400);

    printValue ("burst output sample peak (dBFS)", toDb (loudPeak));
    printValue ("residual gain reduction after 1 s quiet (dB)", (double) status.limiterReductionDb);
    printValue ("quiet-section output peak (dBFS)", toDb (quietPeak));

    SP_REQUIRE_LE (toDb (loudPeak), -0.3 + 0.2);
    SP_REQUIRE_LT (status.limiterReductionDb, 0.3f);
    SP_REQUIRE_LT (toDb (quietPeak), -20.0);
    SP_REQUIRE (! containsNonFinite (output));
}

//==============================================================================
SP_TEST (Dsp, LimiterClippingCounterAndBelowCeiling)
{
    const double fs = 48000.0;
    auto p = makeNeutralParams();
    p.preamp.limiterEnabled = true;
    p.preamp.outputCeilingDb = -0.3f;

    DspChain chain;
    chain.prepare (fs, 512, 2);
    chain.setParamsImmediate (p);

    chain.resetClippingCount();
    auto input = makeSine (fs, 1.0, 500.0, 1.5, 2);
    auto output = input;
    processThroughChain (chain, output, 512);
    const auto clipsAfterLoud = chain.getClippedSampleCount();
    printValue ("clipped frame count for 1.5 FS input", (double) clipsAfterLoud);
    SP_REQUIRE_GT (clipsAfterLoud, (std::int64_t) 1000);

    chain.resetClippingCount();
    auto quiet = makeSine (fs, 1.0, 500.0, 0.2, 2);
    auto quietOut = quiet;
    processThroughChain (chain, quietOut, 512);
    const auto clipsAfterQuiet = chain.getClippedSampleCount();
    printValue ("clipped frame count for -14 dBFS input", (double) clipsAfterQuiet);
    SP_REQUIRE_LT (clipsAfterQuiet, (std::int64_t) 1);
}
