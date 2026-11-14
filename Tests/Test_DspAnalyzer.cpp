/*
    DSP tests: AnalyzerHub. Real measurements from pushed audio blocks: LUFS
    (BS.1770-4), true peak, FFT spectrum, bass band energies, spectrogram,
    clipping and underrun counters.
*/
#include "Test_DspUtils.h"

#include "DSP/AnalyzerHub.h"

#include <cmath>
#include <vector>

using namespace sptest;
using namespace sptest::dsp;
using namespace sp;

namespace
{
void pushStereoBlocks (AnalyzerHub& hub, const juce::AudioBuffer<double>& buffer, int blockSize, int maxWaitMs = 3000)
{
    const int total = buffer.getNumSamples();
    for (int start = 0; start < total; start += blockSize)
    {
        const int n = std::min (blockSize, total - start);
        const float* left = nullptr;
        const float* right = nullptr;
        std::vector<float> leftF ((size_t) n), rightF ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            leftF[(size_t) i] = (float) buffer.getReadPointer (0)[start + i];
            rightF[(size_t) i] = (float) buffer.getReadPointer (1)[start + i];
        }
        left = leftF.data();
        right = rightF.data();
        hub.pushOutput (left, right, n);

        // Pace the pushes at roughly 2x real time so the lock-free FIFOs do not
        // overflow (the analysis thread drains them every display frame).
        juce::Thread::sleep (5);
    }

    // Wait for the analysis thread to catch up with everything we pushed.
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + maxWaitMs;
    while (juce::Time::getMillisecondCounterHiRes() < deadline)
    {
        const auto values = hub.getMeterValues();
        if (values.analysedFrames >= total)
            break;
        juce::Thread::sleep (10);
    }
}

MeterValues waitForMeters (AnalyzerHub& hub, std::int64_t minFrames)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 4000.0;
    while (juce::Time::getMillisecondCounterHiRes() < deadline)
    {
        const auto values = hub.getMeterValues();
        if (values.analysedFrames >= minFrames)
            return values;
        juce::Thread::sleep (10);
    }
    return hub.getMeterValues();
}
} // namespace

//==============================================================================
SP_TEST (Dsp, AnalyzerLufsReference)
{
    const double fs = 48000.0;
    AnalyzerHub hub;
    hub.prepare (fs, 512);
    hub.start();

    // Stereo 1 kHz sine, amplitude 0.1 per channel => about -20.0 LUFS integrated
    // (K-weighting at 1 kHz is +0.70 dB; two channels add 3.01 dB).
    auto buffer = makeSine (fs, 6.0, 1000.0, 0.1, 2);
    pushStereoBlocks (hub, buffer, 512);

    const auto values = waitForMeters (hub, (std::int64_t) (5.0 * fs));
    hub.stop();

    printValue ("analysed frames", (double) values.analysedFrames);
    printValue ("momentary LUFS (400 ms)", values.momentaryLufs);
    printValue ("short-term LUFS (3 s)", values.shortTermLufs);
    printValue ("integrated LUFS (gated)", values.integratedLufs);
    printValue ("loudness range (LU)", values.loudnessRangeLu);
    printValue ("peak L (dBFS)", values.peakDb[0]);
    printValue ("RMS L (dBFS)", values.rmsDb[0]);

    SP_REQUIRE_NEAR (values.integratedLufs, -20.0, 0.5);
    SP_REQUIRE_NEAR (values.momentaryLufs, -20.0, 0.5);
    SP_REQUIRE_NEAR (values.shortTermLufs, -20.0, 0.5);
    SP_REQUIRE_LT (values.loudnessRangeLu, 1.0f);

    // Peak / RMS of a -20 dBFS peak sine: peak -20 dBFS, RMS -23.01 dBFS.
    SP_REQUIRE_NEAR (values.peakDb[0], -20.0, 0.3);
    SP_REQUIRE_NEAR (values.rmsDb[0], -23.01, 0.4);
    SP_REQUIRE_NEAR (values.crestFactorDb, 3.01, 0.4);
}

//==============================================================================
SP_TEST (Dsp, AnalyzerTruePeakAboveSamplePeak)
{
    const double fs = 48000.0;
    AnalyzerHub hub;
    hub.prepare (fs, 512);
    hub.start();

    // fs/4 at 45 degrees: sample peak is 0.7071 (-3.01 dBFS) but the continuous
    // waveform peaks at 1.0 (0 dBTP).
    juce::AudioBuffer<double> buffer (2, (int) (4.0 * fs));
    for (int ch = 0; ch < 2; ++ch)
        for (int n = 0; n < buffer.getNumSamples(); ++n)
            buffer.getWritePointer (ch)[n] = 0.9 * std::cos (2.0 * pi() * (fs / 4.0) * (double) n / fs + pi() / 4.0);

    pushStereoBlocks (hub, buffer, 512);
    const auto values = waitForMeters (hub, (std::int64_t) (3.0 * fs));
    hub.stop();

    printValue ("sample peak (dBFS)", values.peakDb[0]);
    printValue ("true peak (dBTP)", values.truePeakDb[0]);
    printValue ("true peak max (dBTP)", values.truePeakMaxDb);
    printValue ("true peak minus sample peak (dB)", (double) (values.truePeakDb[0] - values.peakDb[0]));

    SP_REQUIRE_NEAR (values.peakDb[0], toDb (0.9 * std::cos (pi() / 4.0)), 0.3);
    SP_REQUIRE_GT (values.truePeakDb[0], values.peakDb[0] + 1.5f);
    SP_REQUIRE_NEAR (values.truePeakDb[0], toDb (0.9), 0.8);
}

//==============================================================================
SP_TEST (Dsp, AnalyzerSpectrumAndBands)
{
    const double fs = 48000.0;
    AnalyzerHub hub;
    hub.prepare (fs, 512);
    hub.setFftSize (4096);
    hub.setDisplayUpdateRateHz (30);
    hub.start();

    auto buffer = makeSine (fs, 3.0, 1000.0, 0.5, 2);
    pushStereoBlocks (hub, buffer, 512);
    waitForMeters (hub, (std::int64_t) (2.5 * fs));

    std::vector<float> rawDb, displayDb, displayFreqs;
    hub.getRawSpectrumDb (rawDb);
    hub.getSpectrumDb (displayDb, displayFreqs);
    hub.stop();

    SP_REQUIRE (rawDb.size() == 2048);
    SP_REQUIRE (! displayDb.empty());
    SP_REQUIRE (displayDb.size() == displayFreqs.size());

    int peakBin = 1;
    for (int i = 2; i < (int) rawDb.size(); ++i)
        if (rawDb[(size_t) i] > rawDb[(size_t) peakBin])
            peakBin = i;

    const double binHz = fs / 4096.0;
    const double peakHz = peakBin * binHz;
    printValue ("FFT peak bin", (double) peakBin);
    printValue ("FFT peak frequency (Hz)", peakHz);
    printValue ("FFT peak level (dBFS peak)", (double) rawDb[(size_t) peakBin]);

    SP_REQUIRE_NEAR (peakHz, 1000.0, 2.0 * binHz);
    SP_REQUIRE_GT (rawDb[(size_t) peakBin], -14.0f);
    SP_REQUIRE_LT (rawDb[(size_t) peakBin], -3.0f);

    // Display spectrum maximum should also land near 1 kHz.
    int displayPeak = 0;
    for (int i = 1; i < (int) displayDb.size(); ++i)
        if (displayDb[(size_t) i] > displayDb[(size_t) displayPeak])
            displayPeak = i;
    printValue ("display spectrum peak frequency (Hz)", (double) displayFreqs[(size_t) displayPeak]);
    SP_REQUIRE_NEAR (displayFreqs[(size_t) displayPeak], 1000.0, 120.0);

    // Bass energy on a 40 Hz tone.
    AnalyzerHub bassHub;
    bassHub.prepare (fs, 512);
    bassHub.setFftSize (4096);
    bassHub.start();
    auto bassBuffer = makeSine (fs, 3.0, 40.0, 0.5, 2);
    pushStereoBlocks (bassHub, bassBuffer, 512);
    const auto bassValues = waitForMeters (bassHub, (std::int64_t) (2.5 * fs));
    bassHub.stop();

    printValue ("40 Hz tone: bass 20-60 dBFS", bassValues.bassEnergyDb[0]);
    printValue ("40 Hz tone: bass 60-120 dBFS", bassValues.bassEnergyDb[1]);
    printValue ("40 Hz tone: bass 120-250 dBFS", bassValues.bassEnergyDb[2]);
    printValue ("40 Hz tone: low 20-250 dBFS", bassValues.lowEnergyDb);
    printValue ("40 Hz tone: mid 250-2k dBFS", bassValues.midEnergyDb);
    printValue ("40 Hz tone: high 2k-16k dBFS", bassValues.highEnergyDb);

    SP_REQUIRE_NEAR (bassValues.bassEnergyDb[0], -9.0, 3.0);
    SP_REQUIRE_LT (bassValues.bassEnergyDb[1], -20.0f);
    SP_REQUIRE_LT (bassValues.bassEnergyDb[2], -20.0f);
    SP_REQUIRE_LT (bassValues.highEnergyDb, -30.0f);
}

//==============================================================================
SP_TEST (Dsp, AnalyzerSpectrogramHistory)
{
    const double fs = 48000.0;
    AnalyzerHub hub;
    hub.prepare (fs, 512);
    hub.start();

    auto buffer = makeSine (fs, 2.5, 1000.0, 0.5, 2);
    pushStereoBlocks (hub, buffer, 512);
    waitForMeters (hub, (std::int64_t) (2.0 * fs));

    std::vector<float> bands;
    int rows = 0;
    int cols = 0;
    hub.getSpectrogram (bands, rows, cols);
    hub.stop();

    printValue ("spectrogram rows", (double) rows);
    printValue ("spectrogram columns", (double) cols);
    SP_REQUIRE (cols == AnalyzerHub::kSpectrogramBands);
    SP_REQUIRE_GT (rows, 0);
    SP_REQUIRE (rows <= AnalyzerHub::kSpectrogramHistory);
    SP_REQUIRE (bands.size() == (size_t) (rows * cols));

    float maximum = -1000.0f;
    for (auto v : bands)
        maximum = std::max (maximum, v);
    printValue ("spectrogram maximum level (dB)", (double) maximum);
    SP_REQUIRE_GT (maximum, -40.0f);
}

//==============================================================================
SP_TEST (Dsp, AnalyzerCountersAndInputOutput)
{
    const double fs = 48000.0;
    AnalyzerHub hub;
    hub.prepare (fs, 512);
    hub.start();

    auto loud = makeSine (fs, 1.0, 500.0, 1.2, 2);
    auto quiet = makeSine (fs, 1.0, 500.0, 0.25, 2);

    // Push quieter input than output to check the input-vs-output comparison.
    const int blockSize = 512;
    for (int start = 0; start < quiet.getNumSamples(); start += blockSize)
    {
        const int n = std::min (blockSize, quiet.getNumSamples() - start);
        std::vector<float> inL ((size_t) n), inR ((size_t) n), outL ((size_t) n), outR ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            inL[(size_t) i] = (float) quiet.getReadPointer (0)[start + i];
            inR[(size_t) i] = (float) quiet.getReadPointer (1)[start + i];
            outL[(size_t) i] = (float) loud.getReadPointer (0)[start + i];
            outR[(size_t) i] = (float) loud.getReadPointer (1)[start + i];
        }
        hub.pushInput (inL.data(), inR.data(), n);
        hub.pushOutput (outL.data(), outR.data(), n);
    }

    hub.notifyUnderrun();
    hub.notifyUnderrun();

    const auto values = waitForMeters (hub, (std::int64_t) (fs * 0.9));
    hub.stop();

    printValue ("input peak (dBFS)", (double) values.inputPeakDb[0]);
    printValue ("output peak (dBFS)", (double) values.peakDb[0]);
    printValue ("clip count", (double) values.clipCount);
    printValue ("underrun count", (double) values.underrunCount);
    printValue ("clipping flag", values.clipping ? 1.0 : 0.0);

    SP_REQUIRE_NEAR (values.inputPeakDb[0], -12.04, 1.0);
    SP_REQUIRE_GT (values.peakDb[0], values.inputPeakDb[0] + 6.0f);
    SP_REQUIRE_GT (values.clipCount, (std::int64_t) 0);
    SP_REQUIRE_GE (values.underrunCount, (std::int64_t) 2);
    SP_REQUIRE (values.clipping);
}
