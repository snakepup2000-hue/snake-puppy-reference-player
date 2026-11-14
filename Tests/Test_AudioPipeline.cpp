/*
    Snake Puppy Reference Player
    Offline playback-pipeline tests: gapless splice of two tracks through the
    real decode path (no output device needed), plus memory playback.

    The two generated WAVs hold one continuous 1 kHz sine split at exactly one
    second, so any gap, overlap or resampling error at the splice is measurable
    as a deviation from the analytic waveform.
*/
#include "TestFramework.h"
#include "Test_AudioHelpers.h"
#include "Audio/PlaybackPipeline.h"
#include <cmath>

using namespace sp;

namespace
{
using namespace spatest;

constexpr double kRate = 48000.0;
constexpr double kFrequency = 1000.0;
constexpr double kAmplitude = 0.25;
constexpr double kSecondsPerTrack = 1.0;

struct GaplessFiles
{
    juce::File fileA, fileB;
    juce::AudioBuffer<double> expected;   // the unsplit continuous sine
};

GaplessFiles makeGaplessFiles()
{
    GaplessFiles files;
    auto dir = testTempDirectory();
    files.fileA = dir.getChildFile ("gapless-a.wav");
    files.fileB = dir.getChildFile ("gapless-b.wav");

    files.expected = makeSine (kRate, 2.0 * kSecondsPerTrack, kFrequency, kAmplitude, 2, 0.0);

    const int splitFrame = (int) std::lround (kRate * kSecondsPerTrack);

    juce::AudioBuffer<double> a (2, splitFrame);
    juce::AudioBuffer<double> b (2, files.expected.getNumSamples() - splitFrame);
    for (int ch = 0; ch < 2; ++ch)
    {
        a.copyFrom (ch, 0, files.expected, ch, 0, splitFrame);
        b.copyFrom (ch, 0, files.expected, ch, splitFrame, b.getNumSamples());
    }

    SP_REQUIRE_MSG (writeWav (files.fileA, a, kRate), "could not write " + files.fileA.getFullPathName());
    SP_REQUIRE_MSG (writeWav (files.fileB, b, kRate), "could not write " + files.fileB.getFullPathName());
    return files;
}

sp::Track makeTrack (const juce::File& file)
{
    sp::Track track;
    track.id = (sp::TrackId) file.hashCode64();
    track.path = file.getFullPathName();
    track.folder = file.getParentDirectory().getFullPathName();
    track.title = file.getFileNameWithoutExtension();
    track.sampleRate = (int) kRate;
    track.bitDepth = 24;
    track.numChannels = 2;
    track.formatName = "WAV";
    track.family = sp::Track::Family::pcmInteger;
    return track;
}

} // namespace

SP_TEST (AudioPipeline, GaplessSpliceIsSampleContinuous)
{
    const auto files = makeGaplessFiles();
    const auto trackA = makeTrack (files.fileA);
    const auto trackB = makeTrack (files.fileB);

    sp::PlaybackPipeline pipeline;
    pipeline.prepare (kRate, 1);
    pipeline.setMemoryPlayback (false);

    juce::String error;
    SP_REQUIRE_MSG (pipeline.loadTrack (trackA, 0, 0, error), error);
    pipeline.setNextTrack (trackB, 1);

    const int expectedFrames = files.expected.getNumSamples();
    juce::AudioBuffer<double> output (2, expectedFrames + 4096);
    output.clear();
    std::vector<double> interleaved ((size_t) (expectedFrames + 4096) * 2, 0.0);

    int produced = 0;
    int boundaryFrame = -1;
    int transitions = 0;
    double transitionFrameOffset = -1.0;
    sp::Track transitionTrack;
    int transitionIndex = -1;

    while (produced < output.getNumSamples())
    {
        sp::PipelineTransition transition;
        const int want = juce::jmin (1024, output.getNumSamples() - produced);
        const int got = pipeline.read (interleaved.data() + (size_t) produced * 2, want, transition);

        if (transition.occurred)
        {
            ++transitions;
            boundaryFrame = produced + transition.frameOffset;
            transitionFrameOffset = (double) transition.frameOffset;
            transitionTrack = transition.newTrack;
            transitionIndex = transition.newQueueIndex;
        }

        if (got <= 0)
            break;
        produced += got;

        if (pipeline.finished())
            break;

        SP_REQUIRE_MSG (pipeline.lastError().isEmpty(), pipeline.lastError());
    }

    for (int i = 0; i < produced; ++i)
    {
        output.setSample (0, i, interleaved[(size_t) i * 2]);
        output.setSample (1, i, interleaved[(size_t) i * 2 + 1]);
    }

    std::cout << "        produced=" << produced << " frames, expected=" << expectedFrames
              << ", transitions=" << transitions << ", boundary@" << boundaryFrame
              << " (offset " << transitionFrameOffset << " in block)\n";

    SP_REQUIRE_MSG (produced == expectedFrames,
                    "gapless output length " + juce::String (produced) + " vs " + juce::String (expectedFrames));
    SP_REQUIRE_MSG (transitions == 1, "expected exactly one gapless transition");
    SP_REQUIRE_MSG (boundaryFrame == (int) std::lround (kRate * kSecondsPerTrack),
                    "boundary at " + juce::String (boundaryFrame));
    SP_REQUIRE_MSG (transitionTrack.path == trackB.path, "transition reported the wrong track");
    SP_REQUIRE_MSG (transitionIndex == 1, "transition index " + juce::String (transitionIndex));

    // Compare against the analytic continuous sine: a gap, click, overlap or
    // filter transient at the splice all show up here.
    const double* left = output.getReadPointer (0);
    const double* right = output.getReadPointer (1);
    double maxError = 0.0;
    int maxErrorFrame = -1;

    for (int i = 0; i < produced; ++i)
    {
        const double expected = kAmplitude * std::sin (juce::MathConstants<double>::twoPi * kFrequency * (double) i / kRate);
        const double errorL = std::abs (left[i] - expected);
        const double errorR = std::abs (right[i] - expected);
        if (errorL > maxError)
        {
            maxError = errorL;
            maxErrorFrame = i;
        }
        if (errorR > maxError)
            maxError = errorR;
    }

    std::cout << "        max deviation from continuous sine: " << juce::String (maxError, 9)
              << " at frame " << maxErrorFrame << "\n";

    // 24-bit WAV quantisation is ~6e-8; anything larger means a real splice error.
    SP_REQUIRE_MSG (maxError < 1.0e-4, "sample-continuity error " + juce::String (maxError, 9));

    // The transition is exactly at the end of track A.
    SP_REQUIRE_MSG (std::abs (left[boundaryFrame] - kAmplitude * std::sin (juce::MathConstants<double>::twoPi * kFrequency
                                                                           * (double) boundaryFrame / kRate)) < 1.0e-4,
                    "first sample after the splice is discontinuous");

    const double stepAtBoundary = std::abs (left[boundaryFrame] - left[boundaryFrame - 1]);
    const double worstStepInTrack = juce::jmax (maxSampleStep (output, 0, 0, boundaryFrame - 1),
                                                maxSampleStep (output, 0, boundaryFrame + 1, produced - boundaryFrame - 2));
    std::cout << "        boundary step=" << juce::String (stepAtBoundary, 6)
              << ", worst in-track step=" << juce::String (worstStepInTrack, 6) << "\n";
    SP_REQUIRE_MSG (stepAtBoundary <= worstStepInTrack + 1.0e-6,
                    "step at boundary " + juce::String (stepAtBoundary) + " exceeds in-track maximum "
                    + juce::String (worstStepInTrack));
}

SP_TEST (AudioPipeline, ResamplesToDeviceRateAndSplicesGaplessly)
{
    // 44.1 kHz source, 48 kHz device: the shared DSP Resampler must preserve the
    // tone and the gapless splice must not leave a gap.
    constexpr double sourceRate = 44100.0;
    constexpr double targetRate = 48000.0;

    auto dir = testTempDirectory();
    const auto fileA = dir.getChildFile ("resample-a.wav");
    const auto fileB = dir.getChildFile ("resample-b.wav");

    const auto expected = makeSine (sourceRate, 2.0, kFrequency, kAmplitude, 2, 0.0);
    const int splitFrame = (int) std::lround (sourceRate);
    juce::AudioBuffer<double> a (2, splitFrame);
    juce::AudioBuffer<double> b (2, expected.getNumSamples() - splitFrame);
    for (int ch = 0; ch < 2; ++ch)
    {
        a.copyFrom (ch, 0, expected, ch, 0, splitFrame);
        b.copyFrom (ch, 0, expected, ch, splitFrame, b.getNumSamples());
    }
    SP_REQUIRE_MSG (writeWav (fileA, a, sourceRate), "write A");
    SP_REQUIRE_MSG (writeWav (fileB, b, sourceRate), "write B");

    sp::PlaybackPipeline pipeline;
    pipeline.prepare (targetRate, 1);
    pipeline.setMemoryPlayback (false);

    juce::String error;
    SP_REQUIRE_MSG (pipeline.loadTrack (makeTrack (fileA), 0, 0, error), error);
    SP_REQUIRE (pipeline.isResampling());
    SP_REQUIRE_NEAR (pipeline.resampleFrom(), sourceRate, 0.5);
    SP_REQUIRE_NEAR (pipeline.resampleTo(), targetRate, 0.5);
    pipeline.setNextTrack (makeTrack (fileB), 1);

    std::vector<double> out ((size_t) (targetRate * 2.2 + 8192) * 2, 0.0);
    int produced = 0;
    int boundary = -1;
    int transitions = 0;

    while (produced < (int) out.size() / 2)
    {
        sp::PipelineTransition transition;
        const int got = pipeline.read (out.data() + (size_t) produced * 2, 1024, transition);
        if (transition.occurred)
        {
            ++transitions;
            boundary = produced + transition.frameOffset;
        }
        if (got <= 0)
            break;
        produced += got;
        if (pipeline.finished())
            break;
        SP_REQUIRE_MSG (pipeline.lastError().isEmpty(), pipeline.lastError());
    }

    std::cout << "        resampled: produced=" << produced << " frames (expected ~"
              << (int) std::lround (targetRate * 2.0) << "), transitions=" << transitions
              << ", boundary@" << boundary << "\n";

    SP_REQUIRE_MSG (produced > (int) (targetRate * 1.9), "too few resampled frames");
    SP_REQUIRE_MSG (transitions == 1, "expected one gapless transition");
    SP_REQUIRE_MSG (std::abs (boundary - (int) targetRate) < 1024,
                    "resampled boundary at " + juce::String (boundary));

    juce::AudioBuffer<double> buffer (2, produced);
    for (int i = 0; i < produced; ++i)
    {
        buffer.setSample (0, i, out[(size_t) i * 2]);
        buffer.setSample (1, i, out[(size_t) i * 2 + 1]);
    }

    const auto before = measureTone (buffer, targetRate, 0, (int) (targetRate * 0.3), (int) (targetRate * 0.4));
    const auto after = measureTone (buffer, targetRate, 0, (int) (targetRate * 1.4), (int) (targetRate * 0.4));
    logMeasurement ("before splice", before);
    logMeasurement ("after splice", after);

    SP_REQUIRE_MSG (std::abs (before.frequencyHz - kFrequency) <= kFrequency * 0.01,
                    "resampled frequency before splice " + juce::String (before.frequencyHz, 2));
    SP_REQUIRE_MSG (std::abs (after.frequencyHz - kFrequency) <= kFrequency * 0.01,
                    "resampled frequency after splice " + juce::String (after.frequencyHz, 2));
    SP_REQUIRE_MSG (std::abs (before.amplitude - kAmplitude) <= kAmplitude * 0.10,
                    "resampled amplitude before " + juce::String (before.amplitude, 5));
    SP_REQUIRE_MSG (std::abs (after.amplitude - kAmplitude) <= kAmplitude * 0.10,
                    "resampled amplitude after " + juce::String (after.amplitude, 5));

    // No silence at the splice: measure RMS in a 20 ms window centred on it.
    const auto spliceWindow = measureTone (buffer, targetRate, 0, boundary - (int) (targetRate * 0.01),
                                            (int) (targetRate * 0.02));
    std::cout << "        splice window rms=" << juce::String (spliceWindow.rms, 6) << "\n";
    SP_REQUIRE_MSG (spliceWindow.rms > 0.1, "splice window is silent (gap)");
}

SP_TEST (AudioPipeline, MemoryPlaybackDecodesWholeTrackToRam)
{
    const auto files = makeGaplessFiles();
    const auto trackA = makeTrack (files.fileA);

    sp::PlaybackPipeline pipeline;
    pipeline.prepare (kRate, 1);
    pipeline.setMemoryPlayback (true);

    juce::String error;
    SP_REQUIRE_MSG (pipeline.loadTrack (trackA, 0, 0, error), error);
    SP_REQUIRE_MSG (pipeline.currentTrackUsesMemory(), "memory playback should be active for a small file");

    const int frames = (int) std::lround (kRate * kSecondsPerTrack);
    juce::AudioBuffer<double> output (2, frames);
    std::vector<double> interleaved ((size_t) frames * 2, 0.0);
    int produced = 0;

    while (produced < frames)
    {
        sp::PipelineTransition transition;
        const int got = pipeline.read (interleaved.data() + (size_t) produced * 2, frames - produced, transition);
        if (got <= 0)
            break;
        produced += got;
    }

    for (int i = 0; i < produced; ++i)
    {
        output.setSample (0, i, interleaved[(size_t) i * 2]);
        output.setSample (1, i, interleaved[(size_t) i * 2 + 1]);
    }

    SP_REQUIRE_MSG (produced == frames, "memory playback produced " + juce::String (produced) + " frames");

    double maxError = 0.0;
    for (int i = 0; i < produced; ++i)
    {
        const double expected = kAmplitude * std::sin (juce::MathConstants<double>::twoPi * kFrequency * (double) i / kRate);
        maxError = juce::jmax (maxError, std::abs (output.getReadPointer (0)[i] - expected));
    }
    std::cout << "        memory playback max deviation: " << juce::String (maxError, 9) << "\n";
    SP_REQUIRE_MSG (maxError < 1.0e-4, "memory playback data mismatch " + juce::String (maxError, 9));

    // Seeking inside a memory-loaded track is index based and must stay exact.
    SP_REQUIRE_MSG (pipeline.loadTrack (trackA, 0, 500, error), error);
    std::vector<double> scratch ((size_t) 1024 * 2, 0.0);
    sp::PipelineTransition transition;
    const int got = pipeline.read (scratch.data(), 1024, transition);
    SP_REQUIRE_MSG (got > 0, "seek read failed");
    const int seekFrame = (int) std::lround (0.5 * kRate);
    const double expected = kAmplitude * std::sin (juce::MathConstants<double>::twoPi * kFrequency
                                                   * (double) (seekFrame + 10) / kRate);
    SP_REQUIRE_MSG (std::abs (scratch[(size_t) 10 * 2] - expected) < 1.0e-4,
                    "memory seek sample mismatch");
}
