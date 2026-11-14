/*
    DSP tests: graphic (15/31 band) and parametric EQ, measured through the real
    DspChain with sine stimuli. Every assertion is backed by a printed measured
    number.
*/
#include "Test_DspUtils.h"

#include "DSP/DspChain.h"

#include <cmath>
#include <vector>

using namespace sptest;
using namespace sptest::dsp;
using namespace sp;

namespace
{
sp::DspParams graphicParams (int band, double gainDb, bool use31 = false)
{
    auto p = makeNeutralParams();
    p.graphic.bandSet = use31 ? GraphicEqParams::BandSet::bands31 : GraphicEqParams::BandSet::bands15;
    p.graphic.gains15.fill (0.0f);
    p.graphic.gains31.fill (0.0f);
    p.graphic.gainForSet (band) = (float) gainDb;
    return p;
}

sp::DspParams parametricParams (int index, BandType type, double freq, double gain, double q)
{
    auto p = makeNeutralParams();
    auto& band = p.parametric.bands[(size_t) index];
    band.enabled = true;
    band.bypassed = false;
    band.type = type;
    band.freqHz = freq;
    band.gainDb = gain;
    band.q = q;
    return p;
}
} // namespace

//==============================================================================
SP_TEST (Dsp, Graphic15BoostAndCut)
{
    const double fs = 48000.0;
    DspChain chain;

    // +6 dB on the 1 kHz band (index 8 of the 15-band ISO set).
    const int band1k = 8;
    SP_REQUIRE_NEAR (GraphicEqParams::centres15()[(size_t) band1k], 1000.0, 1.0e-9);

    const double centreGain = measureChainGainDb (chain, graphicParams (band1k, 6.0), fs, 1000.0);
    const double lowAdjacent = measureChainGainDb (chain, graphicParams (band1k, 6.0), fs, 630.0);
    const double highAdjacent = measureChainGainDb (chain, graphicParams (band1k, 6.0), fs, 1600.0);
    const double twoBandsAway = measureChainGainDb (chain, graphicParams (band1k, 6.0), fs, 4000.0);

    printValue ("15-band +6 @1 kHz: measured centre gain (dB)", centreGain);
    printValue ("15-band +6 @1 kHz: gain at 630 Hz (dB)", lowAdjacent);
    printValue ("15-band +6 @1 kHz: gain at 1.6 kHz (dB)", highAdjacent);
    printValue ("15-band +6 @1 kHz: gain at 4 kHz (dB)", twoBandsAway);

    SP_REQUIRE_NEAR (centreGain, 6.0, 0.6);
    SP_REQUIRE_LT (std::abs (lowAdjacent), 4.0);
    SP_REQUIRE_LT (std::abs (highAdjacent), 4.0);
    SP_REQUIRE_LT (std::abs (lowAdjacent), std::abs (centreGain) - 0.5);
    SP_REQUIRE_LT (std::abs (twoBandsAway), 1.5);

    // -6 dB on the same band.
    const double cutGain = measureChainGainDb (chain, graphicParams (band1k, -6.0), fs, 1000.0);
    printValue ("15-band -6 @1 kHz: measured centre gain (dB)", cutGain);
    SP_REQUIRE_NEAR (cutGain, -6.0, 0.6);

    // 25 Hz band (index 0) boost.
    const double lowBandGain = measureChainGainDb (chain, graphicParams (0, 6.0), fs, 25.0);
    printValue ("15-band +6 @25 Hz: measured centre gain (dB)", lowBandGain);
    SP_REQUIRE_NEAR (lowBandGain, 6.0, 0.6);
}

//==============================================================================
SP_TEST (Dsp, Graphic31Band)
{
    const double fs = 48000.0;
    DspChain chain;

    int band1k = -1;
    for (int i = 0; i < GraphicEqParams::kNumBands31; ++i)
        if (std::abs (GraphicEqParams::centres31()[(size_t) i] - 1000.0) < 0.01)
            band1k = i;
    SP_REQUIRE (band1k >= 0);

    const double centre = measureChainGainDb (chain, graphicParams (band1k, 6.0, true), fs, 1000.0);
    const double adjacentLow = measureChainGainDb (chain, graphicParams (band1k, 6.0, true), fs, 800.0);
    const double adjacentHigh = measureChainGainDb (chain, graphicParams (band1k, 6.0, true), fs, 1250.0);

    printValue ("31-band +6 @1 kHz: measured centre gain (dB)", centre);
    printValue ("31-band +6 @1 kHz: gain at 800 Hz (dB)", adjacentLow);
    printValue ("31-band +6 @1 kHz: gain at 1.25 kHz (dB)", adjacentHigh);

    SP_REQUIRE_NEAR (centre, 6.0, 0.6);
    SP_REQUIRE_LT (std::abs (adjacentLow), 4.0);
    SP_REQUIRE_LT (std::abs (adjacentHigh), 4.0);

    // Multi-band curve: the displayed response must match the measured response.
    auto p = makeNeutralParams();
    p.graphic.bandSet = GraphicEqParams::BandSet::bands31;
    p.graphic.gains31.fill (0.0f);
    p.graphic.gains31[3] = 6.0f;    // 40 Hz
    p.graphic.gains31[10] = -6.0f;  // 200 Hz
    p.graphic.gains31[17] = 4.0f;   // 1 kHz
    p.graphic.gains31[24] = -5.0f;  // 5 kHz
    p.graphic.gains31[28] = 6.0f;   // 12.5 kHz

    const std::vector<double> probeFreqs { 40.0, 80.0, 200.0, 500.0, 1000.0, 2500.0, 5000.0, 12500.0 };

    DspChain responseChain;
    responseChain.prepare (fs, 512, 2);
    responseChain.setParamsImmediate (p);
    const auto displayed = responseChain.getMagnitudeResponseDb (probeFreqs);

    double worstError = 0.0;
    for (size_t i = 0; i < probeFreqs.size(); ++i)
    {
        const double measured = measureChainGainDb (chain, p, fs, probeFreqs[i]);
        const double error = measured - (double) displayed[i];
        worstError = std::max (worstError, std::abs (error));
        std::cout << "        " << probeFreqs[i] << " Hz: measured " << measured
                  << " dB, displayed " << displayed[i] << " dB, error " << error << " dB\n";
    }

    printValue ("31-band multi-curve worst displayed-vs-measured error (dB)", worstError);
    SP_REQUIRE_LT (worstError, 0.35);
}

//==============================================================================
SP_TEST (Dsp, ParametricBellShelf)
{
    const double fs = 48000.0;
    DspChain chain;

    const double bellCentre = measureChainGainDb (chain,
        parametricParams (0, BandType::bell, 1000.0, 6.0, 1.0), fs, 1000.0);
    const double bellHalfOctave = measureChainGainDb (chain,
        parametricParams (0, BandType::bell, 1000.0, 6.0, 1.0), fs, 707.0);
    printValue ("parametric bell 1 kHz +6 Q1: centre (dB)", bellCentre);
    printValue ("parametric bell 1 kHz +6 Q1: 707 Hz (dB)", bellHalfOctave);
    SP_REQUIRE_NEAR (bellCentre, 6.0, 0.5);
    SP_REQUIRE_LT (bellHalfOctave, 6.0);
    SP_REQUIRE_GT (bellHalfOctave, 1.0);

    const double lowShelf40 = measureChainGainDb (chain,
        parametricParams (0, BandType::lowShelf, 100.0, 6.0, 0.70710678), fs, 40.0);
    printValue ("parametric low shelf 100 Hz +6: 40 Hz (dB)", lowShelf40);
    SP_REQUIRE_GT (lowShelf40, 4.8);
    SP_REQUIRE_LT (lowShelf40, 6.5);

    const double highShelf16k = measureChainGainDb (chain,
        parametricParams (0, BandType::highShelf, 8000.0, 6.0, 0.70710678), fs, 16000.0);
    printValue ("parametric high shelf 8 kHz +6: 16 kHz (dB)", highShelf16k);
    SP_REQUIRE_GT (highShelf16k, 4.8);
    SP_REQUIRE_LT (highShelf16k, 6.5);
}

//==============================================================================
SP_TEST (Dsp, ParametricPassFiltersAndBypass)
{
    const double fs = 48000.0;
    DspChain chain;

    const double hpfAt50 = measureChainGainDb (chain,
        parametricParams (0, BandType::highPass, 200.0, 0.0, 0.70710678), fs, 50.0);
    printValue ("parametric HPF 200 Hz 12 dB/oct: 50 Hz (dB)", hpfAt50);
    SP_REQUIRE_LT (hpfAt50, -18.0);
    SP_REQUIRE_GT (hpfAt50, -30.0);

    // One octave above the corner, with the bilinear warp:
    // omega = tan(pi f/fs)/tan(pi fc/fs) = 2.155, |H|^2 = 1/(1+omega^4) -> -13.5 dB.
    const double lpfAt8k = measureChainGainDb (chain,
        parametricParams (0, BandType::lowPass, 4000.0, 0.0, 0.70710678), fs, 8000.0);
    printValue ("parametric LPF 4 kHz 12 dB/oct: 8 kHz (dB)", lpfAt8k);
    SP_REQUIRE_NEAR (lpfAt8k, -13.53, 1.5);

    const double notch = measureChainGainDb (chain,
        parametricParams (0, BandType::notch, 1000.0, 0.0, 4.0), fs, 1000.0);
    printValue ("parametric notch 1 kHz Q4: centre (dB)", notch);
    SP_REQUIRE_LT (notch, -30.0);

    // Per-band bypass must make the band a true no-op.
    auto bypassed = parametricParams (0, BandType::bell, 1000.0, 12.0, 1.0);
    bypassed.parametric.bands[0].bypassed = true;
    const double bypassedGain = measureChainGainDb (chain, bypassed, fs, 1000.0);
    printValue ("parametric band bypassed: gain @1 kHz (dB)", bypassedGain);
    SP_REQUIRE_NEAR (bypassedGain, 0.0, 0.01);

    // Disabled parametric section must be a no-op as well.
    auto disabled = parametricParams (0, BandType::bell, 1000.0, 12.0, 1.0);
    disabled.parametric.enabled = false;
    const double disabledGain = measureChainGainDb (chain, disabled, fs, 1000.0);
    printValue ("parametric section disabled: gain @1 kHz (dB)", disabledGain);
    SP_REQUIRE_NEAR (disabledGain, 0.0, 0.01);

    // eqBypass must disable graphic, parametric and bass together.
    auto p = parametricParams (0, BandType::bell, 1000.0, 12.0, 1.0);
    p.eqBypass = true;
    const double eqBypassGain = measureChainGainDb (chain, p, fs, 1000.0);
    printValue ("eqBypass: gain @1 kHz (dB)", eqBypassGain);
    SP_REQUIRE_NEAR (eqBypassGain, 0.0, 0.01);
}

//==============================================================================
SP_TEST (Dsp, FilterSection)
{
    const double fs = 48000.0;
    DspChain chain;

    auto p = makeNeutralParams();
    p.filters.highPassEnabled = true;
    p.filters.highPassHz = 100.0;
    p.filters.highPassSlopeDbPerOct = 24;
    const double hpAt25 = measureChainGainDb (chain, p, fs, 25.0);
    printValue ("HP 100 Hz 24 dB/oct: measured gain at 25 Hz (dB)", hpAt25);
    SP_REQUIRE_NEAR (hpAt25, -48.16, 2.0);

    p = makeNeutralParams();
    p.filters.lowPassEnabled = true;
    p.filters.lowPassHz = 4000.0;
    p.filters.lowPassSlopeDbPerOct = 48;
    const double lpAt16k = measureChainGainDb (chain, p, fs, 16000.0);
    printValue ("LP 4 kHz 48 dB/oct: measured gain at 16 kHz (dB)", lpAt16k);
    SP_REQUIRE_NEAR (lpAt16k, -129.7, 3.0);
}

//==============================================================================
SP_TEST (Dsp, CombinedMagnitudeResponse)
{
    const double fs = 48000.0;

    auto p = makeNeutralParams();
    p.graphic.bandSet = GraphicEqParams::BandSet::bands31;
    p.graphic.gains31.fill (0.0f);
    p.graphic.gains31[10] = -4.0f;    // 200 Hz
    p.graphic.gains31[17] = 3.0f;     // 1 kHz
    p.graphic.gains31[26] = 4.0f;     // 8 kHz

    p.parametric.bands[0].enabled = true;
    p.parametric.bands[0].type = BandType::bell;
    p.parametric.bands[0].freqHz = 500.0;
    p.parametric.bands[0].gainDb = 3.0;
    p.parametric.bands[0].q = 1.2;

    p.parametric.bands[1].enabled = true;
    p.parametric.bands[1].type = BandType::lowShelf;
    p.parametric.bands[1].freqHz = 120.0;
    p.parametric.bands[1].gainDb = 2.0;
    p.parametric.bands[1].q = 0.70710678;

    p.bass.gainDb = 3.0f;
    p.bass.shelfFreqHz = 80.0;
    p.bass.subEnabled = true;
    p.bass.subGainDb = 2.0f;
    p.bass.subFreqHz = 35.0;

    p.filters.highPassEnabled = true;
    p.filters.highPassHz = 30.0;
    p.filters.highPassSlopeDbPerOct = 24;
    p.filters.lowPassEnabled = true;
    p.filters.lowPassHz = 18000.0;
    p.filters.lowPassSlopeDbPerOct = 12;

    DspChain responseChain;
    responseChain.prepare (fs, 512, 2);
    responseChain.setParamsImmediate (p);

    const std::vector<double> probeFreqs { 30.0, 60.0, 120.0, 200.0, 500.0, 1000.0, 3000.0, 8000.0, 16000.0 };
    const auto displayed = responseChain.getMagnitudeResponseDb (probeFreqs);

    DspChain measurementChain;
    double worstError = 0.0;
    for (size_t i = 0; i < probeFreqs.size(); ++i)
    {
        const double measured = measureChainGainDb (measurementChain, p, fs, probeFreqs[i]);
        const double error = measured - (double) displayed[i];
        worstError = std::max (worstError, std::abs (error));
        std::cout << "        " << probeFreqs[i] << " Hz: measured " << measured
                  << " dB, displayed " << displayed[i] << " dB, error " << error << " dB\n";
    }

    printValue ("combined graphic+parametric+bass+HP/LP response worst error (dB)", worstError);
    SP_REQUIRE_LT (worstError, 0.5);
}

//==============================================================================
SP_TEST (Dsp, EqBypassDisablesBassToo)
{
    const double fs = 48000.0;
    DspChain chain;

    auto p = makeNeutralParams();
    p.bass.gainDb = 6.0f;
    p.bass.subEnabled = true;
    p.bass.subGainDb = 6.0f;
    p.eqBypass = true;

    const double g25 = measureChainGainDb (chain, p, fs, 25.0);
    const double g40 = measureChainGainDb (chain, p, fs, 40.0);
    printValue ("eqBypass with +6 main bass +6 sub: gain @25 Hz (dB)", g25);
    printValue ("eqBypass with +6 main bass +6 sub: gain @40 Hz (dB)", g40);
    SP_REQUIRE_NEAR (g25, 0.0, 0.01);
    SP_REQUIRE_NEAR (g40, 0.0, 0.01);
}
