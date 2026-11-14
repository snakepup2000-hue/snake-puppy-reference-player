/*
    Snake Puppy Reference Player - DSP test utilities.

    Small offline measurement helpers shared by the DSP test files. Everything
    here drives the real DSP code with generated signals and measures the real
    output; no expected values are hard-coded from the implementation.
*/
#pragma once

#include "TestFramework.h"

#include "DSP/DspChain.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace sptest
{
namespace dsp
{

constexpr double kDefaultFs = 48000.0;

inline double pi() { return 3.14159265358979323846; }

inline double toDb (double linear)
{
    return linear > 1.0e-14 ? 20.0 * std::log10 (linear) : -300.0;
}

inline double fromDb (double db)
{
    return std::pow (10.0, db / 20.0);
}

//==============================================================================
inline sp::DspParams makeNeutralParams()
{
    sp::DspParams p;
    p.reset();
    p.bypassed = false;
    p.eqBypass = false;
    p.graphic.enabled = true;
    p.parametric.enabled = true;
    p.bass.enabled = true;
    p.filters = sp::FilterParams {};
    p.stereo.reset();
    p.convolution.enabled = false;
    p.replayGain.enabled = false;
    p.preamp.autoHeadroom = false;
    p.preamp.manualPreampDb = 0.0f;
    p.preamp.limiterEnabled = false;
    p.preamp.outputCeilingDb = -0.3f;
    p.preamp.limiterReleaseMs = 150.0f;
    p.chainOrder = { sp::DspSlot::replayGain, sp::DspSlot::graphicEq, sp::DspSlot::parametricEq,
                     sp::DspSlot::bass, sp::DspSlot::filters, sp::DspSlot::convolution,
                     sp::DspSlot::crossfeed, sp::DspSlot::stereoWidth, sp::DspSlot::balance,
                     sp::DspSlot::preamp, sp::DspSlot::limiter };
    return p;
}

//==============================================================================
inline void fillSine (juce::AudioBuffer<double>& buffer, double freq, double amplitude,
                     double fs, double phase = 0.0)
{
    const int n = buffer.getNumSamples();
    const int channels = buffer.getNumChannels();
    for (int ch = 0; ch < channels; ++ch)
    {
        auto* data = buffer.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
            data[i] = amplitude * std::sin (2.0 * pi() * freq * (double) i / fs + phase);
    }
}

inline juce::AudioBuffer<double> makeSine (double fs, double seconds, double freq, double amplitude,
                                           int channels = 2, double phase = 0.0)
{
    juce::AudioBuffer<double> buffer (channels, (int) std::lround (fs * seconds));
    fillSine (buffer, freq, amplitude, fs, phase);
    return buffer;
}

inline juce::AudioBuffer<double> makeSilence (double fs, double seconds, int channels = 2)
{
    juce::AudioBuffer<double> buffer (channels, (int) std::lround (fs * seconds));
    buffer.clear();
    return buffer;
}

constexpr int kMaxDspTestChannels = 8;

//==============================================================================
/** Runs a whole buffer through a chain in blocks of blockSize (in place). */
inline void processThroughChain (sp::DspChain& chain, juce::AudioBuffer<double>& buffer, int blockSize)
{
    const int total = buffer.getNumSamples();
    const int channels = buffer.getNumChannels();

    for (int start = 0; start < total; start += blockSize)
    {
        const int n = std::min (blockSize, total - start);

        juce::AudioBuffer<double> view (buffer.getArrayOfWritePointers(), channels, start, n);
        chain.process (view);
    }
}

/** Configure a chain and render an input buffer through it, returning a copy. */
inline juce::AudioBuffer<double> render (sp::DspChain& chain, const sp::DspParams& params,
                                         const juce::AudioBuffer<double>& input,
                                         double fs, int blockSize = 512, int rampMs = 0)
{
    chain.prepare (fs, blockSize, input.getNumChannels());
    if (rampMs <= 0)
        chain.setParamsImmediate (params);
    else
        chain.setParams (params, rampMs);

    juce::AudioBuffer<double> output;
    output.makeCopyOf (input);
    processThroughChain (chain, output, blockSize);
    return output;
}

//==============================================================================
/** Least-squares single-tone measurement (amplitude, phase and residual). */
struct ToneMeasurement
{
    double amplitude = 0.0;
    double amplitudeDb = -300.0;
    double phaseRadians = 0.0;
    double residualRms = 0.0;
    double thdPlusNDb = -300.0;
    double signalRms = 0.0;
};

inline ToneMeasurement measureTone (const double* data, int startSample, int numSamples,
                                    double freq, double fs)
{
    ToneMeasurement result;
    if (numSamples <= 1 || data == nullptr)
        return result;

    double re = 0.0;
    double im = 0.0;
    double sumSq = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const double t = (double) (startSample + i) / fs;
        const double w = 2.0 * pi() * freq * t;
        const double x = data[startSample + i];
        re += x * std::cos (w);
        im += x * std::sin (w);
        sumSq += x * x;
    }

    re *= 2.0 / (double) numSamples;
    im *= 2.0 / (double) numSamples;

    result.amplitude = std::sqrt (re * re + im * im);
    result.amplitudeDb = toDb (result.amplitude);
    result.phaseRadians = std::atan2 (im, re);
    result.signalRms = std::sqrt (sumSq / (double) numSamples);

    // Residual after removing the fitted fundamental.
    double residualSq = 0.0;
    for (int i = 0; i < numSamples; ++i)
    {
        const double t = (double) (startSample + i) / fs;
        const double w = 2.0 * pi() * freq * t;
        const double fitted = re * std::cos (w) + im * std::sin (w);
        const double e = data[startSample + i] - fitted;
        residualSq += e * e;
    }

    result.residualRms = std::sqrt (residualSq / (double) numSamples);
    const double signalRmsFit = result.amplitude / std::sqrt (2.0);
    result.thdPlusNDb = toDb (result.residualRms / std::max (1.0e-12, signalRmsFit));
    return result;
}

/** Measures the gain of a chain for a sine at freq, discarding a warm-up. */
inline double measureChainGainDb (sp::DspChain& chain, sp::DspParams params, double fs,
                                  double freq, double amplitude = 0.25,
                                  double warmupSeconds = 0.7, double measureSeconds = 1.0,
                                  int blockSize = 512)
{
    params.replayGain.enabled = false;
    const int warmup = (int) std::lround (fs * warmupSeconds);
    const int measure = (int) std::lround (fs * measureSeconds);
    const int total = warmup + measure;

    auto input = makeSine (fs, (double) total / fs, freq, amplitude, 2);
    auto output = render (chain, params, input, fs, blockSize);

    const double* data = output.getReadPointer (0);
    const auto m = measureTone (data, warmup, measure, freq, fs);
    return toDb (m.amplitude / amplitude);
}

//==============================================================================
inline double peakAbs (const juce::AudioBuffer<double>& buffer, int channel, int start, int numSamples)
{
    const double* data = buffer.getReadPointer (channel, start);
    double peak = 0.0;
    for (int i = 0; i < numSamples; ++i)
        peak = std::max (peak, std::abs (data[i]));
    return peak;
}

inline double rms (const double* data, int start, int numSamples)
{
    double sum = 0.0;
    for (int i = 0; i < numSamples; ++i)
        sum += data[start + i] * data[start + i];
    return std::sqrt (sum / std::max (1, numSamples));
}

inline double rms (const juce::AudioBuffer<double>& buffer, int channel, int start, int numSamples)
{
    return rms (buffer.getReadPointer (channel, start), 0, numSamples);
}

inline bool containsNonFinite (const juce::AudioBuffer<double>& buffer)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const double* data = buffer.getReadPointer (ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (! std::isfinite (data[i]))
                return true;
    }
    return false;
}

/** Maximum absolute sample-to-sample step over a range. */
inline double maxSampleStep (const juce::AudioBuffer<double>& buffer, int channel, int start, int numSamples)
{
    const double* data = buffer.getReadPointer (channel, start);
    double maxStep = 0.0;
    for (int i = 1; i < numSamples; ++i)
        maxStep = std::max (maxStep, std::abs (data[i] - data[i - 1]));
    return maxStep;
}

/** Peak-to-peak null-test difference between two buffers (dB). */
inline double nullTestDb (const juce::AudioBuffer<double>& a, const juce::AudioBuffer<double>& b,
                          int startA, int startB, int numSamples)
{
    double error = 0.0;
    double signal = 0.0;
    for (int ch = 0; ch < std::min (a.getNumChannels(), b.getNumChannels()); ++ch)
    {
        const double* pa = a.getReadPointer (ch);
        const double* pb = b.getReadPointer (ch);
        for (int i = 0; i < numSamples; ++i)
        {
            const double d = pa[startA + i] - pb[startB + i];
            error += d * d;
            signal += pa[startA + i] * pa[startA + i];
        }
    }
    const double errorRms = std::sqrt (error / (double) std::max (1, numSamples));
    const double signalRms = std::sqrt (signal / (double) std::max (1, numSamples));
    return toDb (errorRms / std::max (1.0e-300, signalRms));
}

inline void printValue (const char* label, double value, const char* unit = "")
{
    std::cout << "        " << label << " = " << value << unit << "\n";
}

} // namespace dsp
} // namespace sptest
