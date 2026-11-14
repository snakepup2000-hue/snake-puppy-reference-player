/*
    Snake Puppy Reference Player
    Decode / probe tests for every fixture container: rate, channels, duration,
    bit depth, family, decoder name and the decoded tone (frequency/amplitude).
*/
#include "TestFramework.h"
#include "Test_AudioHelpers.h"
#include "Audio/AudioEngine.h"
#include "Audio/DecodeSource.h"

using namespace sp;

namespace
{
using namespace spatest;

struct FormatCase
{
    const char* path;
    int expectedRate;
    int expectedSourceChannels;
    double expectedDurationMs;
    double expectedFrequencyHz;
    double durationToleranceMs;
    double frequencyTolerancePercent;
    bool lossy;
    int expectedBitDepth;      // -1 = don't care
    const char* expectedFormat;
    double expectedAmplitude;  // ffmpeg sine is 1/8 full scale; Ogg was downmixed with -3 dB
};

const FormatCase formatCases[] =
{
    { "Library/Album A/01 - Track One.flac",        96000, 1, 2000.0, 440.0,  25.0, 0.6, false, 24, "FLAC", 0.125 },
    { "Library/Album A/02 - Track Two.flac",        44100, 1, 2000.0, 523.0,  25.0, 0.6, false, 16, "FLAC", 0.125 },
    { "Library/Album A/03 - Track Three.mp3",       44100, 1, 2000.0, 659.0, 150.0, 2.0, true,  -1, "MP3", 0.125 },
    { "Library/Album B/01 - Rhythm Section.wav",    48000, 1, 2000.0, 880.0,  25.0, 0.6, false, 24, "WAV", 0.125 },
    { "Library/Album B/02 - Big Room.aiff",         44100, 1, 2000.0, 330.0,  25.0, 0.6, false, 16, "AIFF", 0.125 },
    { "Library/Album B/03 - Slow Burn.m4a",         44100, 1, 2000.0, 196.0,  40.0, 1.0, false, -1, "ALAC", 0.125 },
    { "Library/Album B/Disc 2/01 - Late Night.m4a", 44100, 1, 2000.0, 294.0, 150.0, 2.0, true,  -1, "AAC", 0.125 },
    { "Library/Album B/04 - Pocket Groove.ogg",     44100, 2, 2000.0, 392.0, 150.0, 2.0, true,  -1, "Ogg Vorbis", 0.0884 },
};

void checkCase (const FormatCase& testCase)
{
    const auto file = fixture (testCase.path);
    SP_REQUIRE_MSG (file.existsAsFile(), file.getFullPathName());

    AudioFileInfo info;
    juce::String error;
    const bool probed = DecodeSource::probeInfo (file, info, error);
    SP_REQUIRE_MSG (probed, error);
    SP_REQUIRE_MSG (info.sampleRate == testCase.expectedRate,
                    juce::String (info.sampleRate) + " != " + juce::String (testCase.expectedRate));
    SP_REQUIRE_MSG (info.numChannels == testCase.expectedSourceChannels,
                    juce::String (info.numChannels) + " ch != " + juce::String (testCase.expectedSourceChannels));
    SP_REQUIRE_MSG (info.isDsd == false, "not DSD");
    SP_REQUIRE_MSG (info.decoderName.isNotEmpty(), "decoder name must be reported");

    std::cout << "        " << file.getFileName() << ": " << info.formatName << " "
              << info.sampleRate << " Hz, " << info.numChannels << " ch, "
              << info.bitDepth << "-bit, " << juce::String (info.durationMs) << " ms, decoder="
              << info.decoderName << "\n";

    SP_REQUIRE_MSG (std::abs ((double) info.durationMs - testCase.expectedDurationMs) <= testCase.durationToleranceMs,
                    juce::String (info.durationMs) + " ms vs " + juce::String (testCase.expectedDurationMs));

    if (testCase.expectedBitDepth >= 0)
        SP_REQUIRE_MSG (info.bitDepth == testCase.expectedBitDepth,
                        juce::String (info.bitDepth) + " bit != " + juce::String (testCase.expectedBitDepth));

    if (testCase.lossy)
        SP_REQUIRE_MSG (info.family == Track::Family::lossy, "expected lossy family");
    else
        SP_REQUIRE_MSG (info.family == Track::Family::pcmInteger, "expected integer PCM family");

    SP_REQUIRE_MSG (info.formatName.containsIgnoreCase (testCase.expectedFormat),
                    info.formatName + " does not contain " + testCase.expectedFormat);

    //---- decode the whole file -------------------------------------------------
    juce::AudioBuffer<double> decoded;
    const bool decodedOk = AudioEngine::decodeFileToBuffer (file, decoded, 0, error);
    SP_REQUIRE_MSG (decodedOk, error);
    SP_REQUIRE (decoded.getNumChannels() == 2);
    SP_REQUIRE_MSG (decoded.getNumSamples() == (int) info.totalFrames,
                    juce::String (decoded.getNumSamples()) + " vs " + juce::String (info.totalFrames));

    // Mono sources are duplicated to both channels.
    if (info.numChannels == 1)
    {
        const auto* l = decoded.getReadPointer (0);
        const auto* r = decoded.getReadPointer (1);
        double maxDiff = 0.0;
        for (int i = 0; i < decoded.getNumSamples(); ++i)
            maxDiff = juce::jmax (maxDiff, std::abs (l[i] - r[i]));
        SP_REQUIRE_MSG (maxDiff < 1.0e-9, "mono was not duplicated to stereo: " + juce::String (maxDiff));
    }

    const auto tone = measureTone (decoded, (double) info.sampleRate);
    logMeasurement ("decoded", tone);

    SP_REQUIRE_MSG (tone.framesMeasured > 0, "no samples measured");
    const double freqErrorPercent = 100.0 * std::abs (tone.frequencyHz - testCase.expectedFrequencyHz)
                                  / testCase.expectedFrequencyHz;
    SP_REQUIRE_MSG (freqErrorPercent <= testCase.frequencyTolerancePercent,
                    "frequency " + juce::String (tone.frequencyHz, 2) + " Hz, error "
                    + juce::String (freqErrorPercent, 3) + " %");

    // The fixtures are generated from ffmpeg's sine source at 1/8 full scale.
    const double expectedAmplitude = testCase.expectedAmplitude;
    const double amplitudeTolerance = testCase.lossy ? 0.10 : 0.02;
    SP_REQUIRE_MSG (std::abs (tone.amplitude - expectedAmplitude) <= expectedAmplitude * amplitudeTolerance,
                    "amplitude " + juce::String (tone.amplitude, 6) + " vs " + juce::String (expectedAmplitude));
    SP_REQUIRE_MSG (std::abs (tone.dcOffset) < 0.01, "DC offset " + juce::String (tone.dcOffset));

    // Seeking to the middle must land inside the file and decode clean audio.
    juce::String seekError;
    auto source = DecodeSource::create (file, seekError);
    SP_REQUIRE_MSG (source != nullptr, seekError);
    SP_REQUIRE (source->seekToMs (1000));
    SP_REQUIRE_MSG (std::abs (source->positionMs() - 1000) <= 3,
                    "seek position " + juce::String (source->positionMs()));

    std::vector<double> chunk (2 * 4096, 0.0);
    const int got = source->readFrames (chunk.data(), 4096);
    SP_REQUIRE_MSG (got == 4096, "seek read returned " + juce::String (got));
    juce::AudioBuffer<double> seekBuffer (2, got);
    for (int i = 0; i < got; ++i)
    {
        seekBuffer.setSample (0, i, chunk[(size_t) i * 2]);
        seekBuffer.setSample (1, i, chunk[(size_t) i * 2 + 1]);
    }
    const auto seekTone = measureTone (seekBuffer, (double) info.sampleRate, 0, 512, 2048);
    SP_REQUIRE_MSG (std::abs (seekTone.frequencyHz - testCase.expectedFrequencyHz) <= testCase.expectedFrequencyHz * 0.05,
                    "seek tone " + juce::String (seekTone.frequencyHz, 2));
}

} // namespace

SP_TEST (AudioDecode, ProbeAndDecodeAllFixtureFormats)
{
    for (const auto& testCase : formatCases)
        checkCase (testCase);
}

SP_TEST (AudioDecode, BrokenAndTruncatedFixturesReportErrors)
{
    const auto broken = fixture ("Broken/broken.flac");
    AudioFileInfo info;
    juce::String error;
    const bool probeOk = DecodeSource::probeInfo (broken, info, error);
    std::cout << "        broken.flac probe=" << (probeOk ? "ok" : "failed")
              << " error=" << error << "\n";
    SP_REQUIRE_MSG (! probeOk, "a text file must not probe as FLAC");

    juce::AudioBuffer<double> decoded;
    error.clear();
    const bool decodeOk = AudioEngine::decodeFileToBuffer (broken, decoded, 0, error);
    SP_REQUIRE_MSG (! decodeOk, "a text file must not decode");
    SP_REQUIRE_MSG (error.containsIgnoreCase ("corrupt") || error.containsIgnoreCase ("unsupported")
                        || error.containsIgnoreCase ("could not"),
                    "error message should be clear, got: " + error);

    // Truncated: probing may succeed (the header is intact), but decoding must
    // fail with a clear message rather than silently returning short audio.
    // The committed fixture may not actually be truncated if the source file is
    // smaller than the generator's byte count, so build a guaranteed-truncated
    // copy from a good FLAC at test time.
    const auto original = fixture ("Library/Album A/01 - Track One.flac");
    const auto truncated = testTempDirectory().getChildFile ("truncated.flac");
    {
        std::unique_ptr<juce::FileInputStream> in (original.createInputStream());
        SP_REQUIRE (in != nullptr);
        const auto total = in->getTotalLength();
        std::unique_ptr<juce::FileOutputStream> out (truncated.createOutputStream());
        SP_REQUIRE (out != nullptr);
        const auto keep = juce::jmax<juce::int64> (16, total / 2);
        std::vector<char> bytes ((size_t) keep);
        const int read = in->read (bytes.data(), (int) keep);
        SP_REQUIRE (read > 0);
        SP_REQUIRE (out->write (bytes.data(), read));
        out->flush();
    }

    info = {};
    error.clear();
    const bool truncatedProbe = DecodeSource::probeInfo (truncated, info, error);
    std::cout << "        synthetic-truncated FLAC probe=" << (truncatedProbe ? "ok" : "failed")
              << " (" << info.sampleRate << " Hz, " << juce::String (info.totalFrames)
              << " frames) error=" << error << "\n";

    juce::AudioBuffer<double> truncatedBuffer;
    error.clear();
    const bool truncatedDecode = AudioEngine::decodeFileToBuffer (truncated, truncatedBuffer, 0, error);
    std::cout << "        synthetic-truncated FLAC decode=" << (truncatedDecode ? "ok" : "failed")
              << " error=" << error << "\n";
    SP_REQUIRE_MSG (! truncatedDecode, "truncated FLAC must not decode silently");
    SP_REQUIRE_MSG (error.isNotEmpty(), "truncated FLAC must report a reason");
}

SP_TEST (AudioDecode, UnsupportedAndMissingFilesReportErrors)
{
    const auto unsupported = testTempDirectory().getChildFile ("not-audio.xyz");
    unsupported.replaceWithText ("this is not audio");
    SP_REQUIRE_MSG (! DecodeSource::isSupportedFile (unsupported), "unknown extension must be unsupported");

    juce::String error;
    auto source = DecodeSource::create (unsupported, error);
    SP_REQUIRE_MSG (source == nullptr, "unknown extension must not decode");
    SP_REQUIRE_MSG (error.containsIgnoreCase ("unsupported") || error.containsIgnoreCase ("format"),
                    "clear unsupported error, got: " + error);

    const auto missing = fixture ("Library/does-not-exist.flac");
    error.clear();
    source = DecodeSource::create (missing, error);
    SP_REQUIRE_MSG (source == nullptr, "missing file must not decode");
    SP_REQUIRE_MSG (error.containsIgnoreCase ("not found") || error.containsIgnoreCase ("no such"),
                    "clear missing-file error, got: " + error);
}

SP_TEST (AudioDecode, DecodeFileToBufferLimitsSeconds)
{
    const auto file = fixture ("test-1k-44100.wav");
    juce::AudioBuffer<double> buffer;
    juce::String error;
    SP_REQUIRE_MSG (AudioEngine::decodeFileToBuffer (file, buffer, 1, error), error);
    SP_REQUIRE_MSG (buffer.getNumSamples() == 44100,
                    "expected 44100 frames, got " + juce::String (buffer.getNumSamples()));

    const auto tone = measureTone (buffer, 44100.0);
    logMeasurement ("limited", tone);
    SP_REQUIRE_NEAR (tone.frequencyHz, 1000.0, 2.0);
}
