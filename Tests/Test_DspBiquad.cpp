/*
    DSP tests: the biquad library itself.
    Measures actual magnitudes from the transfer function and processes real
    sample streams to verify stability at every supported sample rate.
*/
#include "Test_DspUtils.h"

#include "DSP/Biquad.h"

#include <array>
#include <cmath>
#include <vector>

using namespace sptest;
using namespace sptest::dsp;
using namespace sp;

//==============================================================================
SP_TEST (Dsp, BiquadBellResponse)
{
    const double fs = 48000.0;
    const auto boost = BiquadCoeffs::bell (fs, 1000.0, 6.0, 1.4);
    SP_REQUIRE (boost.isStable());

    const double atCentre = boost.magnitudeDbAt (1000.0, fs);
    const double oneOctaveUp = boost.magnitudeDbAt (2000.0, fs);
    const double twoOctavesDown = boost.magnitudeDbAt (250.0, fs);

    printValue ("bell +6 dB @1k centre gain (dB)", atCentre);
    printValue ("bell +6 dB @1k gain at 2 kHz (dB)", oneOctaveUp);
    printValue ("bell +6 dB @1k gain at 250 Hz (dB)", twoOctavesDown);

    SP_REQUIRE_NEAR (atCentre, 6.0, 0.01);
    SP_REQUIRE_LT (std::abs (oneOctaveUp), 1.5);
    SP_REQUIRE_LT (std::abs (twoOctavesDown), 0.5);

    const auto cut = BiquadCoeffs::bell (fs, 1000.0, -6.0, 1.4);
    const double cutCentre = cut.magnitudeDbAt (1000.0, fs);
    printValue ("bell -6 dB @1k centre gain (dB)", cutCentre);
    SP_REQUIRE_NEAR (cutCentre, -6.0, 0.01);
}

//==============================================================================
SP_TEST (Dsp, BiquadShelves)
{
    const double fs = 48000.0;
    const auto low = BiquadCoeffs::lowShelf (fs, 80.0, 6.0, 1.0);
    SP_REQUIRE (low.isStable());

    const double at20 = low.magnitudeDbAt (20.0, fs);
    const double at40 = low.magnitudeDbAt (40.0, fs);
    const double at60 = low.magnitudeDbAt (60.0, fs);
    const double at80 = low.magnitudeDbAt (80.0, fs);
    const double at1k = low.magnitudeDbAt (1000.0, fs);

    printValue ("low shelf 80 Hz +6 dB S=1 @20 Hz (dB)", at20);
    printValue ("low shelf 80 Hz +6 dB S=1 @40 Hz (dB)", at40);
    printValue ("low shelf 80 Hz +6 dB S=1 @60 Hz (dB)", at60);
    printValue ("low shelf 80 Hz +6 dB S=1 @80 Hz (dB)", at80);
    printValue ("low shelf 80 Hz +6 dB S=1 @1 kHz (dB)", at1k);

    SP_REQUIRE_NEAR (at20, 5.975, 0.1);
    SP_REQUIRE_NEAR (at40, 5.624, 0.1);
    SP_REQUIRE_NEAR (at60, 4.514, 0.1);
    SP_REQUIRE_NEAR (at80, 3.0, 0.1);
    SP_REQUIRE_LT (std::abs (at1k), 0.01);

    // A high shelf measured one octave above its corner is slightly steeper than
    // the mirror-image low shelf because of the bilinear frequency warp:
    // measured +5.92 dB here versus +5.62 dB for the 80 Hz low shelf at 40 Hz.
    const auto high = BiquadCoeffs::highShelf (fs, 8000.0, 6.0, 1.0);
    printValue ("high shelf 8 kHz +6 dB S=1 @16 kHz (dB)", high.magnitudeDbAt (16000.0, fs));
    printValue ("high shelf 8 kHz +6 dB S=1 @1 kHz (dB)", high.magnitudeDbAt (1000.0, fs));
    SP_REQUIRE_NEAR (high.magnitudeDbAt (16000.0, fs), 5.92, 0.1);
    SP_REQUIRE_LT (std::abs (high.magnitudeDbAt (1000.0, fs)), 0.1);

    // The Q variant must agree with the equivalent RBJ slope.
    const auto lowQ = BiquadCoeffs::lowShelfQ (fs, 80.0, 6.0, 0.7071067811865476);
    SP_REQUIRE_NEAR (lowQ.magnitudeDbAt (40.0, fs), at40, 0.001);
}

//==============================================================================
SP_TEST (Dsp, BiquadPassFilters)
{
    const double fs = 48000.0;

    // 12 dB/oct high pass at 100 Hz: -24 dB two octaves below (plus the
    // Butterworth -3 dB corner) => around -24 dB at 25 Hz.
    const auto hp1 = BiquadCoeffs::highPass (fs, 100.0, 0.7071067811865476);
    const double hp1At25 = hp1.magnitudeDbAt (25.0, fs);
    printValue ("HPF 12 dB/oct 100 Hz @25 Hz (dB)", hp1At25);
    SP_REQUIRE_LT (hp1At25, -20.0);
    SP_REQUIRE_GT (hp1At25, -30.0);
    SP_REQUIRE_NEAR (hp1.magnitudeDbAt (100.0, fs), -3.01, 0.1);

    // Expected response of the digital Butterworth cascade, accounting for the
    // bilinear transform frequency warp:
    //   omega = tan(pi f / fs) / tan(pi fc / fs)
    //   |H|^2 = omega^(2N) / (1 + omega^(2N))   (high pass, N = 2 * stages)
    //   |H|^2 = 1 / (1 + omega^(2N))            (low pass)
    auto expectedCascadeDb = [] (bool highPass, double fs, double cutoff, int stages, double freq)
    {
        const int order = 2 * stages;
        const double omega = std::tan (pi() * freq / fs) / std::tan (pi() * cutoff / fs);
        const double powered = std::pow (omega, (double) (2 * order));
        return highPass ? 10.0 * std::log10 (powered / (1.0 + powered))
                        : -10.0 * std::log10 (1.0 + powered);
    };

    const double hp2At25 = expectedCascadeDb (true, fs, 100.0, 2, 25.0);
    const double hp4At25 = expectedCascadeDb (true, fs, 100.0, 4, 25.0);
    printValue ("HPF 24 dB/oct 100 Hz @25 Hz expected (dB)", hp2At25);
    printValue ("HPF 48 dB/oct 100 Hz @25 Hz expected (dB)", hp4At25);
    SP_REQUIRE_NEAR (hp2At25, -48.16, 1.0);
    SP_REQUIRE_NEAR (hp4At25, -96.4, 1.5);
    SP_REQUIRE_NEAR (expectedCascadeDb (true, fs, 100.0, 4, 100.0), -3.01, 0.05);

    const double lp2At16k = expectedCascadeDb (false, fs, 4000.0, 2, 16000.0);
    printValue ("LPF 24 dB/oct 4 kHz @16 kHz expected, warped (dB)", lp2At16k);
    SP_REQUIRE_NEAR (lp2At16k, -64.84, 1.0);

    const auto notch = BiquadCoeffs::notch (fs, 1000.0, 4.0);
    const double notchCentre = notch.magnitudeDbAt (1000.0, fs);
    printValue ("notch 1 kHz Q=4 centre (dB)", notchCentre);
    SP_REQUIRE_LT (notchCentre, -60.0);

    const auto bandPass = BiquadCoeffs::bandPass (fs, 1000.0, 2.0);
    printValue ("band pass 1 kHz centre (dB)", bandPass.magnitudeDbAt (1000.0, fs));
    printValue ("band pass 1 kHz @10 kHz (dB)", bandPass.magnitudeDbAt (10000.0, fs));
    SP_REQUIRE_NEAR (bandPass.magnitudeDbAt (1000.0, fs), 0.0, 0.01);
    SP_REQUIRE_LT (bandPass.magnitudeDbAt (10000.0, fs), -25.0);

    const auto allPass = BiquadCoeffs::allPass (fs, 1000.0, 1.0);
    double worstAllPass = 0.0;
    for (double f : { 20.0, 100.0, 1000.0, 5000.0, 20000.0 })
        worstAllPass = std::max (worstAllPass, std::abs (allPass.magnitudeDbAt (f, fs)));
    printValue ("all pass worst magnitude deviation (dB)", worstAllPass);
    SP_REQUIRE_LT (worstAllPass, 0.001);
}

//==============================================================================
SP_TEST (Dsp, BiquadStabilityAcrossRates)
{
    const std::array<double, 5> rates { 44100.0, 48000.0, 96000.0, 192000.0, 384000.0 };
    double worstPeak = 0.0;

    for (double fs : rates)
    {
        std::vector<BiquadCoeffs> designs;
        designs.push_back (BiquadCoeffs::bell (fs, 20.0, 24.0, 24.0));
        designs.push_back (BiquadCoeffs::bell (fs, 18000.0, -24.0, 24.0));
        designs.push_back (BiquadCoeffs::lowShelf (fs, 30.0, 24.0, 0.5));
        designs.push_back (BiquadCoeffs::highShelf (fs, 16000.0, 24.0, 0.5));
        designs.push_back (BiquadCoeffs::highPass (fs, 5.0, 0.05));
        designs.push_back (BiquadCoeffs::lowPass (fs, 0.49 * fs, 60.0));
        designs.push_back (BiquadCoeffs::notch (fs, 1000.0, 60.0));

        for (const auto& c : designs)
        {
            SP_REQUIRE_MSG (c.isStable(), fs);
            BiquadState state;
            double peak = 0.0;
            for (int n = 0; n < 20000; ++n)
            {
                const double x = (n < 4) ? 1.0 : 0.0;
                const double y = state.process (c, x);
                SP_REQUIRE (std::isfinite (y));
                peak = std::max (peak, std::abs (y));
            }
            worstPeak = std::max (worstPeak, peak);
        }
    }

    printValue ("worst impulse response peak, all rates/designs", worstPeak);
    SP_REQUIRE_LT (worstPeak, 1.0e4);
}

//==============================================================================
SP_TEST (Dsp, ButterworthCascadeQ)
{
    std::array<double, 4> qs {};

    SP_REQUIRE (Butterworth::stagesForSlope (12) == 1);
    SP_REQUIRE (Butterworth::stagesForSlope (24) == 2);
    SP_REQUIRE (Butterworth::stagesForSlope (48) == 4);

    Butterworth::designQs (24, qs);
    printValue ("Butterworth 4th order stage 0 Q", qs[0]);
    printValue ("Butterworth 4th order stage 1 Q", qs[1]);
    SP_REQUIRE_NEAR (qs[0], 1.30656296, 1.0e-6);
    SP_REQUIRE_NEAR (qs[1], 0.54119610, 1.0e-6);

    Butterworth::designQs (48, qs);
    printValue ("Butterworth 8th order Qs", qs[0]);
    SP_REQUIRE_NEAR (qs[0], 2.56291545, 1.0e-6);
    SP_REQUIRE_NEAR (qs[1], 0.89997622, 1.0e-6);
    SP_REQUIRE_NEAR (qs[2], 0.60134489, 1.0e-6);
    SP_REQUIRE_NEAR (qs[3], 0.50979558, 1.0e-6);
}
