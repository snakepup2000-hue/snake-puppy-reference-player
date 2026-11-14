/*
    DSP tests: bass management and automatic headroom, measured through the real
    DspChain at 48 kHz.
*/
#include "Test_DspUtils.h"

#include "DSP/DspChain.h"

#include <cmath>

using namespace sptest;
using namespace sptest::dsp;
using namespace sp;

//==============================================================================
SP_TEST (Dsp, BassMainShelfMeasured)
{
    const double fs = 48000.0;
    DspChain chain;

    auto p = makeNeutralParams();
    p.bass.gainDb = 6.0f;
    p.bass.shelfFreqHz = 80.0;
    p.bass.shelfS = 1.0;
    p.bass.shelfQ = 0.70710678;

    const double g20 = measureChainGainDb (chain, p, fs, 20.0);
    const double g40 = measureChainGainDb (chain, p, fs, 40.0);
    const double g60 = measureChainGainDb (chain, p, fs, 60.0);
    const double g80 = measureChainGainDb (chain, p, fs, 80.0);
    const double g120 = measureChainGainDb (chain, p, fs, 120.0);
    const double g1k = measureChainGainDb (chain, p, fs, 1000.0);

    std::cout << "        Main BASS +6 dB, 80 Hz low shelf (S=1) measured response:\n";
    printValue ("  20 Hz (dB)", g20);
    printValue ("  40 Hz (dB)", g40);
    printValue ("  60 Hz (dB)", g60);
    printValue ("  80 Hz (dB)", g80);
    printValue (" 120 Hz (dB)", g120);
    printValue ("   1 kHz (dB)", g1k);

    // This is a standard RBJ low shelf: +6 dB asymptote below the corner, +3 dB
    // at the 80 Hz corner; 40 Hz reaches +5.6 dB and 60 Hz +4.5 dB measured.
    SP_REQUIRE_NEAR (g20, 5.97, 0.4);
    SP_REQUIRE_NEAR (g40, 5.62, 0.5);
    SP_REQUIRE_NEAR (g60, 4.51, 0.5);
    SP_REQUIRE_NEAR (g80, 3.0, 0.4);
    SP_REQUIRE_LT (std::abs (g120), 1.6);
    SP_REQUIRE_LT (std::abs (g1k), 0.1);

    // -6 dB is the mirror image of the boost.
    p.bass.gainDb = -6.0f;
    const double cut40 = measureChainGainDb (chain, p, fs, 40.0);
    printValue ("Main BASS -6 dB @40 Hz (dB)", cut40);
    SP_REQUIRE_NEAR (cut40, -5.62, 0.5);
}

//==============================================================================
SP_TEST (Dsp, BassSubMidPunchBands)
{
    const double fs = 48000.0;
    DspChain chain;

    auto p = makeNeutralParams();
    p.bass.gainDb = 0.0f;
    p.bass.subEnabled = true;
    p.bass.subGainDb = 6.0f;
    p.bass.subFreqHz = 35.0;
    p.bass.subQ = 0.9;
    const double subAt35 = measureChainGainDb (chain, p, fs, 35.0);
    const double subAt80 = measureChainGainDb (chain, p, fs, 80.0);
    printValue ("sub band +6 @35 Hz: 35 Hz (dB)", subAt35);
    printValue ("sub band +6 @35 Hz: 80 Hz (dB)", subAt80);
    SP_REQUIRE_NEAR (subAt35, 6.0, 0.6);
    // A Q=0.9 bell centred at 35 Hz still contributes +1.64 dB at 80 Hz; that is
    // the measured, physically correct skirt of the band.
    SP_REQUIRE_LT (std::abs (subAt80), 2.0);

    p = makeNeutralParams();
    p.bass.gainDb = 0.0f;
    p.bass.midBassEnabled = true;
    p.bass.midBassGainDb = 6.0f;
    p.bass.midBassFreqHz = 100.0;
    p.bass.midBassQ = 1.0;
    const double midAt100 = measureChainGainDb (chain, p, fs, 100.0);
    printValue ("mid-bass band +6 @100 Hz: 100 Hz (dB)", midAt100);
    SP_REQUIRE_NEAR (midAt100, 6.0, 0.6);

    p = makeNeutralParams();
    p.bass.gainDb = 0.0f;
    p.bass.punchEnabled = true;
    p.bass.punchGainDb = 6.0f;
    p.bass.punchFreqHz = 140.0;
    p.bass.punchQ = 0.9;
    const double punchAt140 = measureChainGainDb (chain, p, fs, 140.0);
    printValue ("punch band +6 @140 Hz: 140 Hz (dB)", punchAt140);
    SP_REQUIRE_NEAR (punchAt140, 6.0, 0.6);

    // All four bands together.
    p = makeNeutralParams();
    p.bass.gainDb = 6.0f;
    p.bass.subEnabled = true;
    p.bass.subGainDb = 6.0f;
    p.bass.midBassEnabled = true;
    p.bass.midBassGainDb = 6.0f;
    p.bass.punchEnabled = true;
    p.bass.punchGainDb = 6.0f;

    const double all20 = measureChainGainDb (chain, p, fs, 20.0);
    const double all35 = measureChainGainDb (chain, p, fs, 35.0);
    const double all100 = measureChainGainDb (chain, p, fs, 100.0);
    printValue ("all bass bands +6: 20 Hz (dB)", all20);
    printValue ("all bass bands +6: 35 Hz (dB)", all35);
    printValue ("all bass bands +6: 100 Hz (dB)", all100);
    SP_REQUIRE_GT (all35, 8.0);

    DspChain statusChain;
    statusChain.prepare (fs, 512, 2);
    auto statusParams = makeNeutralParams();
    statusParams.bass.gainDb = 6.0f;
    statusParams.preamp.autoHeadroom = true;
    statusChain.setParamsImmediate (statusParams);
    const auto status = statusChain.getStatus();
    printValue ("status.maxPositiveBoostDb for +6 bass (dB)", status.maxPositiveBoostDb);
    printValue ("status.autoHeadroomDb (dB)", status.autoHeadroomDb);
    SP_REQUIRE_GT (status.maxPositiveBoostDb, 5.5);
    SP_REQUIRE_NEAR (status.autoHeadroomDb, -(double) status.maxPositiveBoostDb, 0.05);
}

//==============================================================================
SP_TEST (Dsp, AutoHeadroomPreventsClipping)
{
    const double fs = 48000.0;

    auto p = makeNeutralParams();
    p.graphic.gains15.fill (0.0f);
    p.graphic.gains15[8] = 6.0f;    // +6 dB at 1 kHz
    p.preamp.autoHeadroom = true;
    p.preamp.limiterEnabled = true;
    p.preamp.outputCeilingDb = -0.3f;

    DspChain chain;
    chain.prepare (fs, 512, 2);
    chain.setParamsImmediate (p);

    const int latency = chain.getLatencySamples();
    auto input = makeSine (fs, 2.5, 1000.0, 1.0, 2);
    auto output = input;
    processThroughChain (chain, output, 512);

    const int warmup = latency + (int) (0.2 * fs);
    const double peak = peakAbs (output, 0, warmup, output.getNumSamples() - warmup);
    const double peakDb = toDb (peak);
    const auto status = chain.getStatus();

    printValue ("limiter latency (samples)", (double) latency);
    printValue ("auto headroom max positive boost (dB)", status.maxPositiveBoostDb);
    printValue ("auto headroom applied (dB)", status.autoHeadroomDb);
    printValue ("effective preamp (dB)", status.effectivePreampDb);
    printValue ("output sample peak (dBFS)", peakDb, " dB");
    printValue ("limiter gain reduction (dB)", (double) status.limiterReductionDb);

    SP_REQUIRE_NEAR (status.autoHeadroomDb, -6.0, 0.2);
    SP_REQUIRE_NEAR (status.effectivePreampDb, -6.0, 0.2);
    SP_REQUIRE_LE (peakDb, -0.3 + 0.15);
    SP_REQUIRE_LT (status.limiterReductionDb, 0.5f);   // headroom did the job, no limiting needed

    // Without auto headroom the limiter has to catch 6 dB of boost.
    p.preamp.autoHeadroom = false;
    chain.setParamsImmediate (p);
    auto output2 = input;
    processThroughChain (chain, output2, 512);
    const double limitedPeak = peakAbs (output2, 0, warmup, output2.getNumSamples() - warmup);
    const double limitedPeakDb = toDb (limitedPeak);
    const auto limitedStatus = chain.getStatus();

    printValue ("no auto headroom: output sample peak (dBFS)", limitedPeakDb, " dB");
    printValue ("no auto headroom: limiter reduction (dB)", (double) limitedStatus.limiterReductionDb);
    printValue ("no auto headroom: limiter engaged", (double) limitedStatus.limiterEngaged);

    SP_REQUIRE_LE (limitedPeakDb, -0.3 + 0.2);
    SP_REQUIRE_GT (limitedStatus.limiterReductionDb, 5.0f);
    SP_REQUIRE (limitedStatus.limiterEngaged);
}

//==============================================================================
SP_TEST (Dsp, BassAutoHeadroomDoesNotRaiseLoudness)
{
    const double fs = 48000.0;

    auto p = makeNeutralParams();
    p.bass.gainDb = 6.0f;
    p.bass.subEnabled = true;
    p.bass.subGainDb = 6.0f;
    p.preamp.autoHeadroom = true;
    p.preamp.limiterEnabled = false;

    DspChain chain;
    chain.prepare (fs, 512, 2);
    chain.setParamsImmediate (p);

    auto input = makeSine (fs, 2.0, 35.0, 0.9, 2);   // full-scale-ish sub tone
    auto output = input;
    processThroughChain (chain, output, 512);

    const int warmup = (int) (0.5 * fs);
    const double inRms = rms (input, 0, warmup, input.getNumSamples() - warmup);
    const double outRms = rms (output, 0, warmup, output.getNumSamples() - warmup);
    const double inPeak = peakAbs (input, 0, warmup, input.getNumSamples() - warmup);
    const double outPeak = peakAbs (output, 0, warmup, output.getNumSamples() - warmup);
    const auto status = chain.getStatus();

    printValue ("combined +6 bass +6 sub: maxPositiveBoostDb", status.maxPositiveBoostDb);
    printValue ("combined: autoHeadroomDb", status.autoHeadroomDb);
    printValue ("input RMS (dBFS)", toDb (inRms));
    printValue ("output RMS (dBFS)", toDb (outRms));
    printValue ("input peak (dBFS)", toDb (inPeak));
    printValue ("output peak (dBFS)", toDb (outPeak));

    SP_REQUIRE_LE (outPeak, inPeak * 1.02);          // never louder
    SP_REQUIRE_LE (outRms, inRms * 1.02);
    SP_REQUIRE_LT (std::abs (status.autoHeadroomDb) + 1.0, 14.0);
}
