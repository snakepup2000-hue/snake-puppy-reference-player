/*
    Snake Puppy Reference Player
    Native DSD (DSF / DFF) decoder tests: probe facts, PCM rate, tone accuracy,
    seek and honest errors for unsupported/corrupt DSD headers.
*/
#include "TestFramework.h"
#include "Test_AudioHelpers.h"
#include "Audio/AudioEngine.h"
#include "Audio/DecodeSource.h"
#include <memory>

using namespace sp;

namespace
{
using namespace spatest;

constexpr int kDsdFixturePcmRate = 176400;   // DSD64 decimated by 16
constexpr double kExpectedDsdToneHz = 1000.0;
constexpr double kExpectedDsdAmplitude = 0.25;   // 0.5 modulator amplitude * -6 dB

void checkDsdFixture (const char* relativePath, const char* expectedFormat)
{
    const auto file = fixture (relativePath);
    SP_REQUIRE_MSG (file.existsAsFile(), file.getFullPathName());

    AudioFileInfo info;
    juce::String error;
    const bool probed = DecodeSource::probeInfo (file, info, error);
    SP_REQUIRE_MSG (probed, error);

    std::cout << "        " << file.getFileName() << ": " << info.formatName << ", DSD"
              << info.dsdMultiple << ", PCM " << info.sampleRate << " Hz, "
              << juce::String (info.totalFrames) << " frames, " << juce::String (info.durationMs)
              << " ms, gain=" << juce::String (info.dsdGainDb) << " dB\n";

    SP_REQUIRE_MSG (info.isDsd, "must be flagged as DSD");
    SP_REQUIRE_MSG (info.family == Track::Family::dsd, "must be DSD family");
    SP_REQUIRE_MSG (info.bitDepth == 1, "DSD bit depth must be 1");
    SP_REQUIRE_MSG (info.dsdMultiple == 64, juce::String ("dsd multiple ") + juce::String (info.dsdMultiple));
    SP_REQUIRE_MSG (info.sampleRate == kDsdFixturePcmRate,
                    juce::String ("PCM rate ") + juce::String (info.sampleRate));
    SP_REQUIRE_MSG (info.totalFrames == kDsdFixturePcmRate,
                    juce::String ("frames ") + juce::String (info.totalFrames));
    SP_REQUIRE_MSG (std::abs ((double) info.durationMs - 1000.0) <= 5.0,
                    juce::String ("duration ") + juce::String (info.durationMs) + " ms");
    SP_REQUIRE_MSG (std::abs (info.dsdGainDb + 6.0f) < 0.01f, "default DSD gain must be -6 dB");
    SP_REQUIRE_MSG (info.formatName.equalsIgnoreCase (expectedFormat), info.formatName);
    SP_REQUIRE_MSG (info.decoderName.containsIgnoreCase ("DSD"), info.decoderName);

    juce::AudioBuffer<double> decoded;
    const bool decodedOk = AudioEngine::decodeFileToBuffer (file, decoded, 0, error);
    SP_REQUIRE_MSG (decodedOk, error);
    SP_REQUIRE (decoded.getNumChannels() == 2);
    SP_REQUIRE_MSG (decoded.getNumSamples() == info.totalFrames,
                    juce::String (decoded.getNumSamples()) + " vs " + juce::String (info.totalFrames));

    const auto tone = measureTone (decoded, (double) info.sampleRate, 0, 0 + info.sampleRate / 5, info.sampleRate * 3 / 5);
    logMeasurement ("decoded", tone);

    SP_REQUIRE_MSG (std::abs (tone.frequencyHz - kExpectedDsdToneHz) <= kExpectedDsdToneHz * 0.005,
                    "DSD tone frequency " + juce::String (tone.frequencyHz, 3));
    SP_REQUIRE_MSG (std::abs (tone.amplitude - kExpectedDsdAmplitude) <= kExpectedDsdAmplitude * 0.20,
                    "DSD tone amplitude " + juce::String (tone.amplitude, 6));
    SP_REQUIRE_MSG (std::abs (tone.dcOffset) < 0.01, "DSD DC offset " + juce::String (tone.dcOffset));

    // Seek to 500 ms and verify the decoder reports and produces the tone.
    auto source = DecodeSource::create (file, error);
    SP_REQUIRE_MSG (source != nullptr, error);
    SP_REQUIRE (source->seekToMs (500));
    SP_REQUIRE_MSG (std::abs (source->positionMs() - 500) <= 2,
                    "DSD seek position " + juce::String (source->positionMs()));

    std::vector<double> chunk ((size_t) 16384 * 2, 0.0);
    const int got = source->readFrames (chunk.data(), 16384);
    SP_REQUIRE_MSG (got == 16384, "DSD seek read " + juce::String (got));

    juce::AudioBuffer<double> seekBuffer (2, got);
    for (int i = 0; i < got; ++i)
    {
        seekBuffer.setSample (0, i, chunk[(size_t) i * 2]);
        seekBuffer.setSample (1, i, chunk[(size_t) i * 2 + 1]);
    }
    const auto seekTone = measureTone (seekBuffer, (double) info.sampleRate, 0, 1024, 16384);
    SP_REQUIRE_MSG (std::abs (seekTone.frequencyHz - kExpectedDsdToneHz) <= kExpectedDsdToneHz * 0.02,
                    "DSD seek tone " + juce::String (seekTone.frequencyHz, 3));
}

void writeLe32 (juce::FileOutputStream& out, std::uint32_t v)
{
    unsigned char b[4] { (unsigned char) (v & 0xff), (unsigned char) ((v >> 8) & 0xff),
                         (unsigned char) ((v >> 16) & 0xff), (unsigned char) ((v >> 24) & 0xff) };
    out.write (b, 4);
}

void writeLe64 (juce::FileOutputStream& out, std::uint64_t v)
{
    writeLe32 (out, (std::uint32_t) (v & 0xffffffffu));
    writeLe32 (out, (std::uint32_t) (v >> 32));
}

/** Builds a minimal DSF file with an arbitrary DSD rate so the decoder's
    "unsupported rate" error path can be measured. */
juce::File writeSyntheticDsf (const juce::String& name, std::uint32_t dsdRate, std::uint64_t bitsPerChannel)
{
    const auto file = testTempDirectory().getChildFile (name);
    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> out (file.createOutputStream());
    if (out == nullptr)
        return {};

    const std::uint32_t channels = 2;
    const std::uint32_t blockSize = 4096;
    const std::uint64_t dataBytes = ((bitsPerChannel + 7) / 8) * channels;
    const std::uint64_t fileSize = 28 + 52 + 12 + dataBytes;

    out->write ("DSD ", 4);
    writeLe64 (*out, 28);
    writeLe64 (*out, fileSize);
    writeLe64 (*out, 0);

    out->write ("fmt ", 4);
    writeLe64 (*out, 52);
    writeLe32 (*out, 1);            // format version
    writeLe32 (*out, 0);            // format id
    writeLe32 (*out, 2);            // channel type
    writeLe32 (*out, channels);
    writeLe32 (*out, dsdRate);
    writeLe32 (*out, 1);            // bits per sample
    writeLe64 (*out, bitsPerChannel);
    writeLe32 (*out, blockSize);
    writeLe32 (*out, 0);            // reserved

    out->write ("data", 4);
    writeLe64 (*out, 12 + dataBytes);
    std::vector<unsigned char> silence ((size_t) dataBytes, 0x69);
    out->write (silence.data(), (int) silence.size());
    out->flush();
    return file;
}

} // namespace

SP_TEST (AudioDsd, DecodesDsfAndDffTo176kWithTone)
{
    checkDsdFixture ("Library/DSD/01 - DSD Test Tone.dsf", "DSF");
    checkDsdFixture ("Library/DSD/02 - DSD Test Tone.dff", "DFF");
}

SP_TEST (AudioDsd, UnsupportedAndCorruptDsdHeadersReportClearErrors)
{
    // DSD512 is deliberately out of the supported set.
    const auto dsd512 = writeSyntheticDsf ("dsd512.dsf", 22579200u, 2822400ull);
    SP_REQUIRE_MSG (dsd512.existsAsFile(), "could not write synthetic DSF");

    AudioFileInfo info;
    juce::String error;
    const bool ok = DecodeSource::probeInfo (dsd512, info, error);
    std::cout << "        DSD512 probe=" << (ok ? "ok" : "failed") << " error=" << error << "\n";
    SP_REQUIRE_MSG (! ok, "DSD512 must be rejected");
    SP_REQUIRE_MSG (error.containsIgnoreCase ("unsupported"), "clear unsupported-rate error, got: " + error);

    // Truncated DSF header.
    const auto corrupt = testTempDirectory().getChildFile ("corrupt.dsf");
    corrupt.replaceWithText ("DSD only-a-few-bytes");
    error.clear();
    const bool corruptOk = DecodeSource::probeInfo (corrupt, info, error);
    std::cout << "        corrupt DSF probe=" << (corruptOk ? "ok" : "failed") << " error=" << error << "\n";
    SP_REQUIRE_MSG (! corruptOk, "corrupt DSF must be rejected");
    SP_REQUIRE_MSG (error.isNotEmpty(), "corrupt DSF must report a reason");
}
