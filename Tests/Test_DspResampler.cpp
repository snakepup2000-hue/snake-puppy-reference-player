/*
    DSP tests: streaming polyphase resampler. Measures amplitude, THD+N and
    passband/stopband behaviour for the 44.1 / 48 / 96 / 192 families.
*/
#include "Test_DspUtils.h"

#include "DSP/Resampler.h"

#include <cmath>
#include <vector>

using namespace sptest;
using namespace sptest::dsp;

namespace
{
struct Resampled
{
    std::vector<double> data;
    int latencySamples = 0;
    double targetRate = 0.0;
};

Resampled resampleSine (double sourceRate, double targetRate, double freq, double amplitude,
                        double seconds, sp::Resampler::Quality quality)
{
    sp::Resampler resampler;
    resampler.prepare (sourceRate, targetRate, 1, quality);

    const int inFrames = (int) std::lround (seconds * sourceRate);
    std::vector<double> input ((size_t) inFrames);
    for (int i = 0; i < inFrames; ++i)
        input[(size_t) i] = amplitude * std::sin (2.0 * pi() * freq * (double) i / sourceRate);

    Resampled result;
    result.latencySamples = resampler.latencySamples();
    result.targetRate = targetRate;

    const int chunk = 512;
    std::vector<double> output ((size_t) sp::Resampler::maxOutputFrames (sourceRate, targetRate, chunk) + 16);

    for (int start = 0; start < inFrames; start += chunk)
    {
        const int n = std::min (chunk, inFrames - start);
        const double* inPtr = input.data() + start;
        double* outPtr = output.data();
        const int produced = resampler.process (&inPtr, n, &outPtr, (int) output.size(), false);
        for (int i = 0; i < produced; ++i)
            result.data.push_back (output[(size_t) i]);
    }

    // Flush the filter tail.
    for (int flushIteration = 0; flushIteration < 1000; ++flushIteration)
    {
        double* outPtr = output.data();
        const int produced = resampler.process (nullptr, 0, &outPtr, (int) output.size(), true);
        if (produced <= 0)
            break;
        for (int i = 0; i < produced; ++i)
            result.data.push_back (output[(size_t) i]);
    }

    return result;
}

double measureToneLevel (const std::vector<double>& data, int start, int count,
                         double freq, double fs, sptest::dsp::ToneMeasurement& out)
{
    out = measureTone (data.data(), start, count, freq, fs);
    return out.amplitudeDb;
}
} // namespace

//==============================================================================
SP_TEST (Dsp, Resampler441To96Thd)
{
    const double sourceRate = 44100.0;
    const double targetRate = 96000.0;
    const auto result = resampleSine (sourceRate, targetRate, 1000.0, 0.5, 1.0,
                                      sp::Resampler::Quality::high);

    printValue ("44.1 -> 96 kHz: reported latency (output samples)", (double) result.latencySamples);
    printValue ("44.1 -> 96 kHz: total output samples", (double) result.data.size());
    SP_REQUIRE_GT (result.data.size(), (size_t) 96000);
    SP_REQUIRE (result.latencySamples > 0);

    const int warmup = (int) std::lround (0.2 * targetRate);
    const int measureCount = (int) std::lround (0.5 * targetRate);   // 500 cycles at 1 kHz
    sptest::dsp::ToneMeasurement m;
    measureToneLevel (result.data, warmup, measureCount, 1000.0, targetRate, m);

    printValue ("44.1 -> 96 kHz: measured 1 kHz amplitude", m.amplitude);
    printValue ("44.1 -> 96 kHz: amplitude error (dB)", toDb (m.amplitude / 0.5));
    printValue ("44.1 -> 96 kHz: THD+N (dB)", m.thdPlusNDb);

    SP_REQUIRE_NEAR (m.amplitude, 0.5, 0.002);
    SP_REQUIRE_LT (m.thdPlusNDb, -90.0);
}

//==============================================================================
SP_TEST (Dsp, Resampler96To441Thd)
{
    const double sourceRate = 96000.0;
    const double targetRate = 44100.0;
    const auto result = resampleSine (sourceRate, targetRate, 1000.0, 0.5, 1.0,
                                      sp::Resampler::Quality::high);

    printValue ("96 -> 44.1 kHz: reported latency (output samples)", (double) result.latencySamples);
    printValue ("96 -> 44.1 kHz: total output samples", (double) result.data.size());
    SP_REQUIRE_GT (result.data.size(), (size_t) 44100);

    const int warmup = (int) std::lround (0.2 * targetRate);
    const int measureCount = (int) std::lround (0.7 * targetRate);   // 700 cycles at 1 kHz
    sptest::dsp::ToneMeasurement m;
    measureToneLevel (result.data, warmup, measureCount, 1000.0, targetRate, m);

    printValue ("96 -> 44.1 kHz: measured 1 kHz amplitude", m.amplitude);
    printValue ("96 -> 44.1 kHz: amplitude error (dB)", toDb (m.amplitude / 0.5));
    printValue ("96 -> 44.1 kHz: THD+N (dB)", m.thdPlusNDb);

    SP_REQUIRE_NEAR (m.amplitude, 0.5, 0.002);
    SP_REQUIRE_LT (m.thdPlusNDb, -90.0);
}

//==============================================================================
SP_TEST (Dsp, ResamplerPassbandStopband)
{
    // Passband: 19 kHz survives 44.1 -> 96 kHz (below the 22.05 kHz source Nyquist).
    {
        const auto result = resampleSine (44100.0, 96000.0, 19000.0, 0.5, 0.5,
                                          sp::Resampler::Quality::high);
        const int warmup = (int) std::lround (0.1 * 96000.0);
        const int count = (int) std::lround (0.3 * 96000.0);
        sptest::dsp::ToneMeasurement m;
        measureToneLevel (result.data, warmup, count, 19000.0, 96000.0, m);
        printValue ("44.1 -> 96 kHz: 19 kHz passband amplitude", m.amplitude);
        printValue ("44.1 -> 96 kHz: 19 kHz passband error (dB)", toDb (m.amplitude / 0.5));
        SP_REQUIRE_LT (std::abs (toDb (m.amplitude / 0.5)), 1.0);

        // Stopband: 30 kHz must be removed when downsampling 96 -> 44.1 kHz.
        const auto down = resampleSine (96000.0, 44100.0, 30000.0, 0.5, 0.5,
                                        sp::Resampler::Quality::high);
        const int downWarmup = (int) std::lround (0.1 * 44100.0);
        const int downCount = (int) std::lround (0.2 * 44100.0);
        const double stopbandRms = rms (down.data.data(), downWarmup, downCount);
        printValue ("96 -> 44.1 kHz: 30 kHz alias residual (dBFS)", toDb (stopbandRms));
        SP_REQUIRE_LT (toDb (stopbandRms), -80.0);
    }

    // 192 kHz -> 44.1 kHz: 1 kHz amplitude and THD+N in the ultra mode.
    {
        const auto result = resampleSine (192000.0, 44100.0, 1000.0, 0.5, 0.6,
                                          sp::Resampler::Quality::ultra);
        const int warmup = (int) std::lround (0.1 * 44100.0);
        const int count = (int) std::lround (0.4 * 44100.0);
        sptest::dsp::ToneMeasurement m;
        measureToneLevel (result.data, warmup, count, 1000.0, 44100.0, m);
        printValue ("192 -> 44.1 kHz ultra: 1 kHz amplitude", m.amplitude);
        printValue ("192 -> 44.1 kHz ultra: THD+N (dB)", m.thdPlusNDb);
        SP_REQUIRE_NEAR (m.amplitude, 0.5, 0.002);
        SP_REQUIRE_LT (m.thdPlusNDb, -90.0);
    }
}

//==============================================================================
SP_TEST (Dsp, ResamplerFlushAndPassThrough)
{
    // Equal rates = lossless pass-through, zero latency.
    {
        sp::Resampler resampler;
        resampler.prepare (48000.0, 48000.0, 2);
        SP_REQUIRE (resampler.latencySamples() == 0);

        std::vector<double> left (256), right (256);
        for (size_t i = 0; i < left.size(); ++i)
        {
            left[i] = std::sin (0.01 * (double) i);
            right[i] = -left[i];
        }
        const double* in[2] = { left.data(), right.data() };
        std::vector<double> outL (256), outR (256);
        double* out[2] = { outL.data(), outR.data() };
        const int produced = resampler.process (in, 256, out, 256, false);
        SP_REQUIRE (produced == 256);
        for (int i = 0; i < 256; ++i)
        {
            SP_REQUIRE (outL[(size_t) i] == left[(size_t) i]);
            SP_REQUIRE (outR[(size_t) i] == right[(size_t) i]);
        }
    }

    // Flush must emit the filter tail and terminate cleanly.
    {
        sp::Resampler resampler;
        resampler.prepare (44100.0, 48000.0, 1, sp::Resampler::Quality::fast);
        std::vector<double> input (44100, 0.0);
        for (int i = 0; i < 44100; ++i)
            input[(size_t) i] = std::sin (2.0 * pi() * 1000.0 * (double) i / 44100.0);

        std::vector<double> output (4096);
        int total = 0;
        for (int start = 0; start < 44100; start += 512)
        {
            const int n = std::min (512, 44100 - start);
            const double* in = input.data() + start;
            double* outPtr = output.data();
            total += resampler.process (&in, n, &outPtr, (int) output.size(), false);
        }
        int tail = 0;
        for (int i = 0; i < 100; ++i)
        {
            double* outPtr = output.data();
        const int produced = resampler.process (nullptr, 0, &outPtr, (int) output.size(), true);
            if (produced <= 0)
                break;
            tail += produced;
        }
        printValue ("44.1 -> 48 kHz flush tail frames", (double) tail);
        printValue ("44.1 -> 48 kHz total frames (expected ~48000)", (double) (total + tail));
        SP_REQUIRE_GT (tail, 0);
        SP_REQUIRE_GT (total + tail, 47500);
        SP_REQUIRE_LT (total + tail, 49000);
    }
}

//==============================================================================
SP_TEST (Dsp, ResamplerRateFamilies)
{
    const std::vector<std::pair<double, double>> pairs {
        { 44100.0, 48000.0 }, { 48000.0, 44100.0 },
        { 44100.0, 88200.0 }, { 88200.0, 44100.0 },
        { 44100.0, 192000.0 }, { 192000.0, 44100.0 },
        { 96000.0, 192000.0 }, { 192000.0, 384000.0 },
        { 352800.0, 44100.0 }, { 44100.0, 352800.0 },
        { 384000.0, 48000.0 }
    };

    for (const auto& pair : pairs)
    {
        const auto result = resampleSine (pair.first, pair.second, 1000.0, 0.5, 0.3,
                                          sp::Resampler::Quality::high);
        const int warmup = (int) std::lround (0.1 * pair.second);
        const int count = (int) std::lround (0.15 * pair.second);
        sptest::dsp::ToneMeasurement m;
        measureToneLevel (result.data, warmup, count, 1000.0, pair.second, m);

        std::cout << "        " << pair.first << " -> " << pair.second << " Hz: amplitude "
                  << m.amplitude << ", error " << toDb (m.amplitude / 0.5) << " dB\n";

        SP_REQUIRE_NEAR (m.amplitude, 0.5, 0.005);
        SP_REQUIRE (result.latencySamples > 0);
    }
}
