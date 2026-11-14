/*
    Snake Puppy Reference Player
    Shared helpers for the audio engine tests: fixture paths, tone measurement,
    temporary WAV generation. Header-only, included by Tests/Test_Audio*.cpp.
*/
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <iostream>
#include <vector>

namespace spatest
{

inline juce::File fixture (const juce::String& relativePath)
{
    return juce::File (juce::String (SP_SOURCE_DIR) + "/Tests/Fixtures/" + relativePath);
}

inline juce::File testTempDirectory()
{
    // Use a workspace-local directory: the test runner may be sandboxed away
    // from ~/Library/Caches. Tests/Fixtures is git-ignored.
    auto dir = juce::File (juce::String (SP_SOURCE_DIR)).getChildFile ("Tests/Fixtures/.tmp");
    dir.createDirectory();
    return dir;
}

struct ToneMeasurement
{
    double frequencyHz = 0.0;
    double amplitude = 0.0;
    double rms = 0.0;
    double dcOffset = 0.0;
    int framesMeasured = 0;
};

/** Measures a (near) sine tone with positive-going zero crossings plus RMS. */
inline ToneMeasurement measureTone (const juce::AudioBuffer<double>& buffer, double sampleRate,
                                    int channel = 0, int startFrame = -1, int numFrames = -1)
{
    ToneMeasurement result;

    const int total = buffer.getNumSamples();
    if (total < 64 || channel < 0 || channel >= buffer.getNumChannels() || sampleRate <= 0.0)
        return result;

    if (startFrame < 0)
    {
        startFrame = total / 4;
        numFrames = total / 2;
    }
    if (numFrames <= 0)
        numFrames = total - startFrame;

    startFrame = juce::jlimit (0, total - 2, startFrame);
    numFrames = juce::jmin (numFrames, total - startFrame);
    if (numFrames < 16)
        return result;

    const double* data = buffer.getReadPointer (channel);

    int crossings = 0;
    double sum = 0.0;
    double sumSquares = 0.0;

    for (int i = startFrame + 1; i < startFrame + numFrames; ++i)
    {
        if (data[i - 1] < 0.0 && data[i] >= 0.0)
            ++crossings;
        sum += data[i];
        sumSquares += data[i] * data[i];
    }

    const double n = (double) numFrames;
    result.rms = std::sqrt (sumSquares / n);
    result.amplitude = result.rms * juce::MathConstants<double>::sqrt2;
    result.dcOffset = sum / n;
    result.frequencyHz = (double) crossings * sampleRate / n;
    result.framesMeasured = numFrames;
    return result;
}

/** Largest absolute difference between neighbouring samples in a range. */
inline double maxSampleStep (const juce::AudioBuffer<double>& buffer, int channel, int startFrame, int numFrames)
{
    const int total = buffer.getNumSamples();
    startFrame = juce::jmax (0, startFrame);
    numFrames = juce::jmin (numFrames, total - startFrame - 1);
    if (numFrames <= 0 || channel >= buffer.getNumChannels())
        return 0.0;

    const double* data = buffer.getReadPointer (channel);
    double maxStep = 0.0;
    for (int i = startFrame + 1; i <= startFrame + numFrames; ++i)
        maxStep = juce::jmax (maxStep, std::abs (data[i] - data[i - 1]));
    return maxStep;
}

/** Writes a stereo (or mono) WAV file from an interleaved double buffer. */
inline bool writeWav (const juce::File& file, const juce::AudioBuffer<double>& buffer, double sampleRate)
{
    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    if (stream == nullptr || ! stream->openedOk())
        return false;

    std::unique_ptr<juce::AudioFormatWriter> writer (
        juce::WavAudioFormat().createWriterFor (stream.release(), sampleRate,
                                                (unsigned int) buffer.getNumChannels(), 24, {}, 0));
    if (writer == nullptr)
        return false;

    const int numChannels = buffer.getNumChannels();
    const int numFrames = buffer.getNumSamples();
    std::vector<std::vector<float>> channelData ((size_t) numChannels);
    std::vector<const float*> pointers ((size_t) numChannels, nullptr);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        channelData[(size_t) ch].resize ((size_t) numFrames);
        const auto* src = buffer.getReadPointer (ch);
        for (int i = 0; i < numFrames; ++i)
            channelData[(size_t) ch][(size_t) i] = (float) src[i];
        pointers[(size_t) ch] = channelData[(size_t) ch].data();
    }

    const bool ok = writer->writeFromFloatArrays (pointers.data(), numChannels, numFrames);
    writer->flush();
    return ok;
}

/** Generates a stereo sine buffer. */
inline juce::AudioBuffer<double> makeSine (double sampleRate, double seconds, double frequency,
                                           double amplitude, int channels = 2, double startPhase = 0.0)
{
    const int frames = juce::jmax (1, (int) std::lround (sampleRate * seconds));
    juce::AudioBuffer<double> buffer (channels, frames);
    const double phasePerSample = juce::MathConstants<double>::twoPi * frequency / sampleRate;

    for (int ch = 0; ch < channels; ++ch)
    {
        auto* data = buffer.getWritePointer (ch);
        for (int i = 0; i < frames; ++i)
            data[i] = amplitude * std::sin (startPhase + phasePerSample * (double) i);
    }
    return buffer;
}

inline void logMeasurement (const juce::String& label, const ToneMeasurement& m)
{
    std::cout << "        " << label << ": freq=" << juce::String (m.frequencyHz, 2)
              << " Hz, amplitude=" << juce::String (m.amplitude, 6)
              << ", rms=" << juce::String (m.rms, 6)
              << ", dc=" << juce::String (m.dcOffset, 6) << "\n";
}

} // namespace spatest
