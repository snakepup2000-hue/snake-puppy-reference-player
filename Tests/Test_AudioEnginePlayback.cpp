/*
    Snake Puppy Reference Player
    End-to-end engine tests: real Core Audio output, real decode thread, the
    debug loopback recorder, transport (play/pause/resume/seek/stop) and honest
    signal-path reporting.

    The test generates a tone WAV at the current device rate so no resampler is
    involved (the shared Resampler is exercised by the DSP tests), plays it for
    a couple of seconds, and verifies the actual audio that reached the output
    device via the engine's lock-free debug recorder.
*/
#include "TestFramework.h"
#include "Test_AudioHelpers.h"
#include "Audio/AudioEngine.h"
#include <memory>

using namespace sp;

namespace
{
using namespace spatest;

sp::Track makeToneTrack (const juce::File& file)
{
    sp::Track track;
    track.id = 4242;
    track.path = file.getFullPathName();
    track.folder = file.getParentDirectory().getFullPathName();
    track.title = "Engine Loopback Tone";
    track.artist = "Snake Puppy Tests";
    track.album = "Verification";
    track.sampleRate = 0;
    track.bitDepth = 24;
    track.numChannels = 2;
    track.formatName = "WAV";
    track.family = sp::Track::Family::pcmInteger;
    return track;
}

/** Finds the first sample above a threshold and returns a span safe to analyse. */
int findOnset (const juce::AudioBuffer<double>& buffer, double threshold)
{
    const auto* data = buffer.getReadPointer (0);
    for (int i = 0; i < buffer.getNumSamples(); ++i)
        if (std::abs (data[i]) > threshold)
            return i;
    return -1;
}

} // namespace

SP_TEST (AudioEnginePlayback, RealDevicePlaybackProducesAudibleSignal)
{
    const auto rateHolder = spatest::testTempDirectory();
    juce::ignoreUnused (rateHolder);

    sp::AudioEngine engine;

    sp::EngineOptions options;
    options.gaplessEnabled = true;
    options.nativeSampleRateSwitching = false;   // keep the default device rate
    options.memoryPlayback = false;
    options.ditherEnabled = true;
    options.hogModeEnabled = false;
    options.bufferFrames = 256;
    options.preferredSampleRate = 0.0;
    options.resamplerQuality = 1;
    engine.setOptions (options, false);

    const auto initError = engine.initialise();
    SP_REQUIRE_MSG (initError.isEmpty(), "engine.initialise() failed: " + initError);

    const double rate = engine.getOutputSampleRate();
    SP_REQUIRE_MSG (rate > 0.0, "engine reported no output sample rate");
    std::cout << "        output rate: " << juce::String (rate) << " Hz\n";

    // Generate a 2.5 s 1 kHz tone at the actual device rate.
    const auto toneFile = spatest::testTempDirectory().getChildFile ("engine-loopback-tone.wav");
    const auto toneBuffer = spatest::makeSine (rate, 2.5, 1000.0, 0.25, 2, 0.0);
    SP_REQUIRE_MSG (spatest::writeWav (toneFile, toneBuffer, rate),
                    "could not write " + toneFile.getFullPathName());

    const auto track = makeToneTrack (toneFile);

    // Reference Mode must hold unity volume and restore the user's value.
    engine.setVolumeDb (-12.0f);
    engine.setReferenceMode (true);
    SP_REQUIRE_MSG (engine.isReferenceMode(), "reference mode not active");
    SP_REQUIRE_MSG (std::abs (engine.getVolumeDb()) < 1.0e-6f, "reference mode must set unity volume");
    SP_REQUIRE_MSG (! engine.isMuted(), "reference mode must unmute");
    engine.setReferenceMode (false);
    SP_REQUIRE_MSG (! engine.isReferenceMode(), "reference mode not cleared");
    SP_REQUIRE_MSG (std::abs (engine.getVolumeDb() + 12.0f) < 0.01f,
                    "volume not restored after reference mode: " + juce::String (engine.getVolumeDb()));
    engine.setVolumeDb (0.0f);

    // Start the loopback recorder, then play the tone.
    const auto recordedFile = spatest::testTempDirectory().getChildFile ("engine-loopback-recorded.wav");
    juce::String recorderError;
    SP_REQUIRE_MSG (engine.startDebugRecording (recordedFile, recorderError), recorderError);

    engine.playTrack (track, true);

    // Give the engine ~1.5 s of real playback (no message loop in the harness,
    // so the decode thread and Core Audio callback do all the work).
    juce::Thread::sleep (1500);

    auto status = engine.getStatus();
    std::cout << "        after 1.5 s: state=" << sp::playbackStateName (status.state)
              << ", position=" << juce::String (status.positionMs) << " ms, buffered="
              << juce::String (status.bufferedMs) << " ms, underruns="
              << juce::String (status.underrunCount) << ", cpu="
              << juce::String (status.cpuPercent, 2) << " %, track='"
              << status.currentTrack.title << "', path quality="
              << status.signalPath.qualityLabel() << "\n";

    SP_REQUIRE_MSG (status.deviceOpen, "device should be open");
    SP_REQUIRE_MSG (status.state == sp::PlaybackState::playing,
                    "expected playing, got " + juce::String (sp::playbackStateName (status.state)));
    SP_REQUIRE_MSG (status.positionMs > 500,
                    "position did not advance: " + juce::String (status.positionMs));
    SP_REQUIRE_MSG (status.currentTrack.path == track.path, "current track metadata missing");
    SP_REQUIRE_MSG (status.signalPath.quality != sp::SignalPathInfo::Quality::bitPerfect,
                    "must not claim bit-perfect without hog mode");
    SP_REQUIRE_MSG (status.signalPath.quality == sp::SignalPathInfo::Quality::bitTransparent
                        || status.signalPath.quality == sp::SignalPathInfo::Quality::processed,
                    "quality must be transparent or processed, got " + status.signalPath.qualityLabel());
    SP_REQUIRE_MSG (status.underrunCount < 20,
                    "too many underruns: " + juce::String (status.underrunCount));
    SP_REQUIRE_MSG (status.dspStatus.channels == 2, "DSP chain channel count");

    // Pause, then resume; the state machine must be honest.
    engine.pause();
    juce::Thread::sleep (300);
    status = engine.getStatus();
    std::cout << "        paused: state=" << sp::playbackStateName (status.state)
              << ", position=" << juce::String (status.positionMs) << " ms\n";
    SP_REQUIRE_MSG (status.state == sp::PlaybackState::paused, "pause did not take effect");

    // Seek while paused: the position must move and resume must play from there.
    engine.seekMs (800);
    juce::Thread::sleep (250);
    status = engine.getStatus();
    std::cout << "        paused seek: state=" << sp::playbackStateName (status.state)
              << ", position=" << juce::String (status.positionMs) << " ms\n";
    SP_REQUIRE_MSG (status.state == sp::PlaybackState::paused, "seek changed the paused state");
    SP_REQUIRE_MSG (status.positionMs >= 700 && status.positionMs <= 900,
                    "paused seek position " + juce::String (status.positionMs));

    engine.play();
    juce::Thread::sleep (400);
    status = engine.getStatus();
    SP_REQUIRE_MSG (status.state == sp::PlaybackState::playing, "resume did not take effect");
    SP_REQUIRE_MSG (status.positionMs >= 850, "resume did not continue from the paused seek position");

    engine.seekMs (1200);
    juce::Thread::sleep (300);
    status = engine.getStatus();
    std::cout << "        seek: position=" << juce::String (status.positionMs) << " ms\n";
    SP_REQUIRE_MSG (status.positionMs >= 1100,
                    "seek did not move playback: " + juce::String (status.positionMs));

    engine.stop();
    juce::Thread::sleep (200);
    engine.stopDebugRecording();
    engine.shutdown();

    //---- analyse what actually reached the output device -------------------
    juce::AudioBuffer<double> recorded;
    juce::String decodeError;
    SP_REQUIRE_MSG (sp::AudioEngine::decodeFileToBuffer (recordedFile, recorded, 0, decodeError), decodeError);

    std::cout << "        recorded " << recorded.getNumSamples() << " frames at "
              << engine.getOutputSampleRate() << " Hz\n";
    SP_REQUIRE_MSG (recorded.getNumSamples() > (int) (rate * 0.5), "recording is too short");

    const int onset = findOnset (recorded, 0.02);
    SP_REQUIRE_MSG (onset >= 0, "no audio reached the output device");

    const int measureStart = onset + (int) (rate * 0.1);
    const int measureFrames = (int) (rate * 0.4);
    SP_REQUIRE_MSG (measureStart + measureFrames < recorded.getNumSamples(), "recording too short to measure");

    const auto tone = measureTone (recorded, rate, 0, measureStart, measureFrames);
    logMeasurement ("recorded", tone);

    SP_REQUIRE_MSG (std::abs (tone.frequencyHz - 1000.0) <= 5.0,
                    "recorded frequency " + juce::String (tone.frequencyHz, 2) + " Hz");
    SP_REQUIRE_MSG (std::abs (tone.amplitude - 0.25) <= 0.05,
                    "recorded amplitude " + juce::String (tone.amplitude, 5));

    // No dropouts in the recorded stream (a decode/underrun glitch would leave
    // a run of exact-zero samples while the tone should still be sounding).
    int longestZeroRun = 0;
    int zeroRun = 0;
    for (int i = measureStart; i < measureStart + measureFrames; ++i)
    {
        if (std::abs (recorded.getReadPointer (0)[i]) < 1.0e-9)
            ++zeroRun;
        else
            zeroRun = 0;
        longestZeroRun = juce::jmax (longestZeroRun, zeroRun);
    }
    std::cout << "        longest zero run while playing: " << longestZeroRun << " frames\n";
    SP_REQUIRE_MSG (longestZeroRun < 48, "dropout in the recorded output stream");

    recordedFile.deleteFile();
    toneFile.deleteFile();
}

SP_TEST (AudioEnginePlayback, QueueSpliceIsGaplessEndToEnd)
{
    sp::AudioEngine engine;

    sp::EngineOptions options;
    options.gaplessEnabled = true;
    options.nativeSampleRateSwitching = false;
    options.bufferFrames = 256;
    engine.setOptions (options, false);

    const auto initError = engine.initialise();
    SP_REQUIRE_MSG (initError.isEmpty(), initError);

    const double rate = engine.getOutputSampleRate();
    SP_REQUIRE_MSG (rate > 0.0, "no output sample rate");

    // One continuous 1 kHz tone split into two 1 s files at exactly one second,
    // queued through the real engine and recorded from the output device.
    const auto expected = spatest::makeSine (rate, 2.0, 1000.0, 0.25, 2, 0.0);
    const int split = (int) std::lround (rate);
    const auto dir = spatest::testTempDirectory();
    const auto fileA = dir.getChildFile ("engine-gapless-a.wav");
    const auto fileB = dir.getChildFile ("engine-gapless-b.wav");

    juce::AudioBuffer<double> a (2, split);
    juce::AudioBuffer<double> b (2, expected.getNumSamples() - split);
    for (int ch = 0; ch < 2; ++ch)
    {
        a.copyFrom (ch, 0, expected, ch, 0, split);
        b.copyFrom (ch, 0, expected, ch, split, b.getNumSamples());
    }
    SP_REQUIRE_MSG (spatest::writeWav (fileA, a, rate), "write A");
    SP_REQUIRE_MSG (spatest::writeWav (fileB, b, rate), "write B");

    auto trackA = makeToneTrack (fileA);
    auto trackB = makeToneTrack (fileB);
    trackA.title = "Gapless A";
    trackB.title = "Gapless B";

    const auto recordedFile = dir.getChildFile ("engine-gapless-recorded.wav");
    juce::String recorderError;
    SP_REQUIRE_MSG (engine.startDebugRecording (recordedFile, recorderError), recorderError);

    engine.setQueue ({ trackA, trackB }, 0, true);
    juce::Thread::sleep (1750);

    auto status = engine.getStatus();
    std::cout << "        gapless engine: state=" << sp::playbackStateName (status.state)
              << ", position=" << juce::String (status.positionMs) << " ms, index="
              << status.currentQueueIndex << ", track='" << status.currentTrack.title << "'\n";
    SP_REQUIRE_MSG (status.state == sp::PlaybackState::playing, "engine stopped too early");
    SP_REQUIRE_MSG (status.currentQueueIndex == 1, "engine did not splice to the next queue track");
    SP_REQUIRE_MSG (status.currentTrack.path == trackB.path, "track metadata did not advance");

    juce::Thread::sleep (700);
    engine.stop();
    juce::Thread::sleep (150);
    engine.stopDebugRecording();
    engine.shutdown();

    juce::AudioBuffer<double> recorded;
    juce::String decodeError;
    SP_REQUIRE_MSG (sp::AudioEngine::decodeFileToBuffer (recordedFile, recorded, 0, decodeError), decodeError);

    const int onset = findOnset (recorded, 0.02);
    SP_REQUIRE_MSG (onset >= 0, "no audio reached the output device");

    // The recording is the continuous 2 s tone starting at `onset` (a phase
    // estimate removes the sub-sample start offset).
    const int measureStart = onset + (int) (rate * 0.2);
    const int measureEnd = juce::jmin (recorded.getNumSamples() - 8, onset + (int) (rate * 1.8));
    SP_REQUIRE_MSG (measureEnd - measureStart > (int) (rate * 1.2), "recording too short");

    const double step = juce::MathConstants<double>::twoPi * 1000.0 / rate;
    const auto* data = recorded.getReadPointer (0);

    // Least-squares phase estimate from the first 100 ms removes the sub-sample
    // start offset without a fragile zero-crossing guess.
    const int fitStart = onset + 8;
    const int fitFrames = juce::jmin ((int) (rate * 0.1), recorded.getNumSamples() - fitStart - 1);
    double sinSum = 0.0, cosSum = 0.0;
    for (int i = 0; i < fitFrames; ++i)
    {
        sinSum += data[fitStart + i] * std::sin (step * (double) (fitStart + i - onset));
        cosSum += data[fitStart + i] * std::cos (step * (double) (fitStart + i - onset));
    }
    const double phase = std::atan2 (cosSum, sinSum);

    double maxError = 0.0;
    int maxErrorFrame = -1;
    for (int i = measureStart; i < measureEnd; ++i)
    {
        const double expectedSample = 0.25 * std::sin (phase + step * (double) (i - onset));
        const double error = std::abs (data[i] - expectedSample);
        if (error > maxError)
        {
            maxError = error;
            maxErrorFrame = i;
        }
    }
    std::cout << "        gapless recording max deviation from continuous tone: "
              << juce::String (maxError, 8) << " at frame " << maxErrorFrame
              << " (span " << measureStart << ".." << measureEnd << ")\n";

    // Sample-step continuity across the splice: a click or gap would show a
    // step far larger than the tone's normal per-sample slew.
    const int splice = onset + (int) rate;
    const double inTrackMaxStep = juce::jmax (
        spatest::maxSampleStep (recorded, 0, onset + 64, (int) (rate * 0.8)),
        spatest::maxSampleStep (recorded, 0, splice + 64, (int) (rate * 0.5)));
    const double spliceStep = spatest::maxSampleStep (recorded, 0, splice - 64, 128);
    std::cout << "        engine splice max step=" << juce::String (spliceStep, 7)
              << ", in-track max step=" << juce::String (inTrackMaxStep, 7) << "\n";
    SP_REQUIRE_MSG (spliceStep <= inTrackMaxStep * 1.5 + 1.0e-4,
                    "discontinuity at the engine gapless splice: " + juce::String (spliceStep, 7)
                    + " vs " + juce::String (inTrackMaxStep, 7));

    // No dropout in a window straddling the splice (onset + 1 s).
    int zeroRun = 0, longest = 0;
    for (int i = juce::jmax (0, splice - 2048); i < juce::jmin (recorded.getNumSamples(), splice + 2048); ++i)
    {
        zeroRun = std::abs (data[i]) < 1.0e-8 ? zeroRun + 1 : 0;
        longest = juce::jmax (longest, zeroRun);
    }
    std::cout << "        engine splice longest zero run: " << longest << " frames\n";
    SP_REQUIRE_MSG (longest < 8, "dropout at the engine gapless splice");
    SP_REQUIRE_MSG (maxError < 0.03,
                    "engine gapless waveform deviation too large: " + juce::String (maxError, 8));

    fileA.deleteFile();
    fileB.deleteFile();
    recordedFile.deleteFile();
}

SP_TEST (AudioEnginePlayback, NoOutputDeviceReportsErrorInsteadOfHanging)
{
    // Deliberately do NOT initialise the engine: there is no output device for
    // this instance, which is the state the sandboxed GUI launches ended up in.
    sp::AudioEngine engine;

    struct CaptureListener : sp::AudioEngine::Listener
    {
        juce::String lastError;
        void engineError (const juce::String& error) override { lastError = error; }
    } listener;

    engine.addListener (&listener);

    const auto track = makeToneTrack (fixture ("test-1k-44100.wav"));
    engine.playTrack (track, true);

    const auto status = engine.getStatus();
    std::cout << "        no-device status: state=" << sp::playbackStateName (status.state)
              << ", error='" << status.errorMessage << "', listener='" << listener.lastError << "'\n";

    SP_REQUIRE_MSG (status.state == sp::PlaybackState::error,
                    "playTrack with no output device must enter the error state, got "
                    + juce::String (sp::playbackStateName (status.state)));
    SP_REQUIRE_MSG (status.errorMessage.containsIgnoreCase ("output device"),
                    "EngineStatus.errorMessage must explain the missing device: " + status.errorMessage);
    SP_REQUIRE_MSG (listener.lastError.containsIgnoreCase ("output device"),
                    "engineError listener notification is missing: " + listener.lastError);
    SP_REQUIRE_MSG (status.queueSize == 1, "the queue should still hold the requested track");

    engine.removeListener (&listener);
}
