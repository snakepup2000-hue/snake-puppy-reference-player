/*
    DSP tests: stereo width, mono-compatibility safeguard, mono bass, balance
    and Bauer crossfeed, measured on real processed buffers.
*/
#include "Test_DspUtils.h"

#include "DSP/StereoProcessor.h"

#include <cmath>
#include <vector>

using namespace sptest;
using namespace sptest::dsp;
using namespace sp;

namespace
{
void renderStereo (StereoProcessor& processor, juce::AudioBuffer<double>& buffer,
                   int blockSize, int which)
{
    const int total = buffer.getNumSamples();
    for (int start = 0; start < total; start += blockSize)
    {
        const int n = std::min (blockSize, total - start);
        double* pointers[2] = { buffer.getWritePointer (0) + start, buffer.getWritePointer (1) + start };
        if (which == 0)
            processor.processWidth (pointers, 2, n);
        else if (which == 1)
            processor.processBalance (pointers, 2, n);
        else
            processor.processCrossfeed (pointers, 2, n);
    }
}
} // namespace

//==============================================================================
SP_TEST (Dsp, StereoBalance)
{
    const double fs = 48000.0;
    juce::AudioBuffer<double> buffer (2, (int) fs);
    fillSine (buffer, 1000.0, 0.5, fs);

    StereoProcessor processor;
    processor.prepare (fs, 512, 2);

    StereoParams params;
    params.balance = -1.0f;
    processor.setParams (params, 0);

    renderStereo (processor, buffer, 512, 1);

    const auto left = measureTone (buffer.getReadPointer (0), 1000, buffer.getNumSamples() - 1000, 1000.0, fs);
    const auto right = measureTone (buffer.getReadPointer (1), 1000, buffer.getNumSamples() - 1000, 1000.0, fs);
    printValue ("balance -1: left amplitude", left.amplitude);
    printValue ("balance -1: right amplitude", right.amplitude);

    SP_REQUIRE_NEAR (left.amplitude, 0.5, 0.002);
    SP_REQUIRE_LT (right.amplitude, 1.0e-6);
}

//==============================================================================
SP_TEST (Dsp, StereoWidthMidSide)
{
    const double fs = 48000.0;
    const int frames = (int) fs;

    juce::AudioBuffer<double> buffer (2, frames);
    for (int n = 0; n < frames; ++n)
    {
        const double mid = 0.4 * std::sin (2.0 * pi() * 100.0 * (double) n / fs);
        const double side = 0.4 * std::sin (2.0 * pi() * 1000.0 * (double) n / fs);
        buffer.getWritePointer (0)[n] = mid + side;
        buffer.getWritePointer (1)[n] = mid - side;
    }

    StereoProcessor processor;
    processor.prepare (fs, 512, 2);

    StereoParams params;
    params.widthPercent = 200.0f;
    params.protectMono = false;
    processor.setParams (params, 0);

    renderStereo (processor, buffer, 512, 0);

    const int start = 2400;
    const int count = frames - start;
    std::vector<double> midOut ((size_t) count), sideOut ((size_t) count);
    for (int n = 0; n < count; ++n)
    {
        midOut[(size_t) n] = 0.5 * (buffer.getReadPointer (0)[start + n] + buffer.getReadPointer (1)[start + n]);
        sideOut[(size_t) n] = 0.5 * (buffer.getReadPointer (0)[start + n] - buffer.getReadPointer (1)[start + n]);
    }

    const auto midTone = measureTone (midOut.data(), 0, count, 100.0, fs);
    const auto sideTone = measureTone (sideOut.data(), 0, count, 1000.0, fs);

    printValue ("width 200%: mid component amplitude (expected 0.4)", midTone.amplitude);
    printValue ("width 200%: side component amplitude (expected 0.8)", sideTone.amplitude);
    printValue ("width 200%: effective width (%)", processor.effectiveWidthPercent());

    SP_REQUIRE_NEAR (midTone.amplitude, 0.4, 0.01);
    SP_REQUIRE_NEAR (sideTone.amplitude, 0.8, 0.02);
    SP_REQUIRE_NEAR (processor.effectiveWidthPercent(), 200.0, 1.0);

    // Mono sum is preserved exactly by M/S width (window 2 protection).
    StereoProcessor monoChecker;
    monoChecker.prepare (fs, 512, 2);
    params.protectMono = true;
    monoChecker.setParams (params, 0);

    juce::AudioBuffer<double> monoBuffer (2, frames);
    monoBuffer.clear();
    for (int ch = 0; ch < 2; ++ch)
        for (int n = 0; n < frames; ++n)
            monoBuffer.getWritePointer (ch)[n] = 0.5 * std::sin (2.0 * pi() * 200.0 * (double) n / fs);

    renderStereo (monoChecker, monoBuffer, 512, 0);
    const auto monoOut = measureTone (monoBuffer.getReadPointer (0), 2400, frames - 2400, 200.0, fs);
    printValue ("mono content through width 200%: amplitude (expected 0.5)", monoOut.amplitude);
    SP_REQUIRE_NEAR (monoOut.amplitude, 0.5, 0.01);
}

//==============================================================================
SP_TEST (Dsp, StereoMonoCompatibilitySafeguard)
{
    const double fs = 48000.0;
    const int frames = (int) (2.0 * fs);

    // Pure side content: folding to mono would cancel it completely. The
    // safeguard must refuse to widen such material beyond 100%.
    juce::AudioBuffer<double> buffer (2, frames);
    for (int n = 0; n < frames; ++n)
    {
        const double side = 0.4 * std::sin (2.0 * pi() * 1000.0 * (double) n / fs);
        buffer.getWritePointer (0)[n] = side;
        buffer.getWritePointer (1)[n] = -side;
    }

    StereoProcessor processor;
    processor.prepare (fs, 512, 2);

    StereoParams params;
    params.widthPercent = 200.0f;
    params.protectMono = true;
    processor.setParams (params, 0);

    renderStereo (processor, buffer, 512, 0);

    const double effective = processor.effectiveWidthPercent();
    const auto left = measureTone (buffer.getReadPointer (0), frames - (int) fs, (int) fs, 1000.0, fs);

    printValue ("pure-side content: effective width with safeguard (%)", effective);
    printValue ("pure-side content: left amplitude (expected 0.4)", left.amplitude);

    SP_REQUIRE_NEAR (effective, 100.0, 2.0);
    SP_REQUIRE_NEAR (left.amplitude, 0.4, 0.01);
}

//==============================================================================
SP_TEST (Dsp, StereoMonoBass)
{
    const double fs = 48000.0;

    auto runTest = [&] (double toneHz) -> double
    {
        const int frames = (int) (2.0 * fs);
        juce::AudioBuffer<double> buffer (2, frames);
        for (int n = 0; n < frames; ++n)
        {
            const double side = 0.4 * std::sin (2.0 * pi() * toneHz * (double) n / fs);
            buffer.getWritePointer (0)[n] = side;
            buffer.getWritePointer (1)[n] = -side;
        }

        StereoProcessor processor;
        processor.prepare (fs, 512, 2);
        StereoParams params;
        params.monoBassEnabled = true;
        params.monoBassHz = 120.0;
        processor.setParams (params, 0);

        renderStereo (processor, buffer, 512, 0);

        const auto m = measureTone (buffer.getReadPointer (0), frames - (int) fs, (int) fs, toneHz, fs);
        return toDb (m.amplitude / 0.4);
    };

    const double at50 = runTest (50.0);
    const double at1000 = runTest (1000.0);

    printValue ("mono bass 120 Hz: side gain at 50 Hz (dB)", at50);
    printValue ("mono bass 120 Hz: side gain at 1 kHz (dB)", at1000);

    SP_REQUIRE_LT (at50, -20.0);
    SP_REQUIRE_NEAR (at1000, 0.0, 0.5);
}

//==============================================================================
SP_TEST (Dsp, StereoCrossfeed)
{
    const double fs = 48000.0;

    auto runTest = [&] (double toneHz) -> double
    {
        const int frames = (int) (2.0 * fs);
        juce::AudioBuffer<double> buffer (2, frames);
        buffer.clear();
        for (int n = 0; n < frames; ++n)
            buffer.getWritePointer (0)[n] = 0.5 * std::sin (2.0 * pi() * toneHz * (double) n / fs);

        StereoProcessor processor;
        processor.prepare (fs, 512, 2);
        StereoParams params;
        params.crossfeedEnabled = true;
        params.crossfeedAmountDb = 6.0f;
        params.crossfeedCutoffHz = 700.0;
        params.crossfeedDelayUs = 250.0f;
        processor.setParams (params, 0);

        renderStereo (processor, buffer, 512, 2);

        const auto right = measureTone (buffer.getReadPointer (1), frames - (int) fs, (int) fs, toneHz, fs);
        return toDb (right.amplitude / 0.5);
    };

    const double at100 = runTest (100.0);
    const double at10k = runTest (10000.0);

    printValue ("crossfeed -6 dB / 700 Hz LP: right-channel gain at 100 Hz (dB)", at100);
    printValue ("crossfeed -6 dB / 700 Hz LP: right-channel gain at 10 kHz (dB)", at10k);

    SP_REQUIRE_GT (at100, -7.5);
    SP_REQUIRE_LT (at100, -5.0);
    SP_REQUIRE_LT (at10k, -30.0);
    SP_REQUIRE_GT (at10k, -80.0);
}
