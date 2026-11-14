/*
    Snake Puppy Reference Player
    Decoding factory + JUCE AudioFormatReader backend.

    The factory sniffs the file content first (so a FLAC named .wav still opens
    correctly and a text file named .flac produces a clear "corrupt" error) and
    falls back to the extension for container formats that cannot be identified
    from the first bytes.

    Threading: every DecodeSource is used by exactly one thread at a time (the
    engine's decode thread or a test thread). None of the methods are real-time
    safe; they perform disk I/O.
*/
#include "Audio/DecodeSource.h"
#include "Audio/DecodeBackends.h"
#include "Audio/DecodeUtils.h"
#include "Audio/Md5.h"
#include <cstring>
#include <cmath>
#include <cstring>
#include <memory>

namespace sp
{

namespace
{
enum class Backend { unknown, juce, audioToolbox, dsd };

struct SniffResult
{
    Backend backend = Backend::unknown;
    juce::String hint;      // short format hint for error messages, e.g. "FLAC"
};

SniffResult sniff (const juce::File& file, const juce::String& ext)
{
    SniffResult r;

    std::unique_ptr<juce::FileInputStream> in (file.createInputStream());
    unsigned char b[16] {};
    int got = 0;
    if (in != nullptr)
        got = in->read (b, 16);

    auto startsWith = [&] (const char* s)
    {
        const int n = (int) std::strlen (s);
        return got >= n && std::memcmp (b, s, (size_t) n) == 0;
    };

    if (startsWith ("fLaC"))            { r.backend = Backend::juce;          r.hint = "FLAC"; return r; }
    if (startsWith ("DSD "))            { r.backend = Backend::dsd;           r.hint = "DSF";  return r; }
    if (startsWith ("FRM8"))            { r.backend = Backend::dsd;           r.hint = "DFF";  return r; }
    if (startsWith ("RIFF") || startsWith ("RF64")) { r.backend = Backend::juce; r.hint = "WAV"; return r; }
    if (startsWith ("FORM"))            { r.backend = Backend::juce;          r.hint = "AIFF"; return r; }
    if (startsWith ("OggS"))            { r.backend = Backend::juce;          r.hint = "Ogg";  return r; }
    if (startsWith ("caff"))            { r.backend = Backend::audioToolbox;  r.hint = "CAF";  return r; }
    if (startsWith ("ID3"))             { r.backend = Backend::juce;          r.hint = "MP3";  return r; }
    if (got >= 2 && b[0] == 0xff && (b[1] & 0xe0) == 0xe0) { r.backend = Backend::juce; r.hint = "MP3"; return r; }
    if (got >= 8 && std::memcmp (b + 4, "ftyp", 4) == 0) { r.backend = Backend::audioToolbox; r.hint = "M4A"; return r; }

    if (ext == ".wav" || ext == ".wave" || ext == ".aif" || ext == ".aiff" || ext == ".flac"
        || ext == ".mp3" || ext == ".ogg" || ext == ".oga")
    {
        r.backend = Backend::juce;
        r.hint = ext.substring (1).toUpperCase();
        return r;
    }
    if (ext == ".m4a" || ext == ".aac" || ext == ".alac" || ext == ".caf" || ext == ".mp4")
    {
        r.backend = Backend::audioToolbox;
        r.hint = ext.substring (1).toUpperCase();
        return r;
    }
    if (ext == ".dsf" || ext == ".dff")
    {
        r.backend = Backend::dsd;
        r.hint = ext.substring (1).toUpperCase();
        return r;
    }

    return r;
}

juce::String normaliseJuceFormatName (const juce::String& raw)
{
    if (raw.containsIgnoreCase ("flac"))     return "FLAC";
    if (raw.containsIgnoreCase ("ogg") || raw.containsIgnoreCase ("vorbis")) return "Ogg Vorbis";
    if (raw.containsIgnoreCase ("mp3") || raw.containsIgnoreCase ("mpeg"))   return "MP3";
    if (raw.containsIgnoreCase ("wave") || raw.containsIgnoreCase ("wav"))   return "WAV";
    if (raw.containsIgnoreCase ("aiff") || raw.containsIgnoreCase ("aifc"))  return "AIFF";
    if (raw.containsIgnoreCase ("au"))       return "AU";
    return raw;
}

bool isLossyName (const juce::String& name)
{
    return name.containsIgnoreCase ("MP3") || name.containsIgnoreCase ("Ogg")
        || name.containsIgnoreCase ("Vorbis") || name.containsIgnoreCase ("Opus");
}

//==============================================================================
class JuceDecodeSource final : public DecodeSource
{
public:
    JuceDecodeSource (const juce::File& file, const juce::String& formatHint, juce::String& errorOut)
    {
        // Choose the exact reader for the sniffed container. Using the generic
        // AudioFormatManager here would let the macOS CoreAudio reader claim
        // FLAC/MP3/Ogg files and report misleading names/bit depths.
        std::unique_ptr<juce::AudioFormat> format;

        if (formatHint == "FLAC")
            format = std::make_unique<juce::FlacAudioFormat>();
        else if (formatHint == "WAV")
            format = std::make_unique<juce::WavAudioFormat>();
        else if (formatHint == "AIFF")
            format = std::make_unique<juce::AiffAudioFormat>();
        else if (formatHint == "Ogg")
            format = std::make_unique<juce::OggVorbisAudioFormat>();
        else if (formatHint == "MP3")
            format = std::make_unique<juce::MP3AudioFormat>();
        else
            format = std::make_unique<juce::WavAudioFormat>();

        auto stream = file.createInputStream();
        if (stream == nullptr)
        {
            errorOut = "Could not open " + file.getFileName() + " for reading";
            return;
        }

        reader.reset (format->createReaderFor (stream.release(), true));
        if (reader == nullptr)
        {
            errorOut = "Could not open " + file.getFileName()
                     + ": the " + formatHint + " container or codec is corrupt or unsupported.";
            return;
        }

        const int srcChannels = (int) reader->numChannels;
        if (srcChannels <= 0 || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        {
            errorOut = "Could not open " + file.getFileName()
                     + ": the file reported no audio samples (corrupt header?).";
            reader.reset();
            return;
        }

        rawFormatName = normaliseJuceFormatName (reader->getFormatName());
        lossy = isLossyName (rawFormatName);

        info.path = file.getFullPathName();
        info.sampleRate = juce::roundToInt (reader->sampleRate);
        info.numChannels = srcChannels;
        info.totalFrames = reader->lengthInSamples;
        info.durationMs = info.sampleRate > 0 ? (info.totalFrames * 1000) / info.sampleRate : 0;
        info.formatName = rawFormatName;
        info.downmixedToStereo = srcChannels > 2;

        if (lossy)
        {
            info.family = Track::Family::lossy;
            info.bitDepth = 0;   // lossy codecs have no meaningful integer depth
        }
        else if (reader->usesFloatingPointData)
        {
            info.family = Track::Family::pcmFloat;
            info.bitDepth = juce::jmax (32, (int) reader->bitsPerSample);
        }
        else
        {
            info.family = Track::Family::pcmInteger;
            info.bitDepth = (int) reader->bitsPerSample;
        }

        info.decoderName = "JUCE " + rawFormatName + " reader";

        detail::parseReplayGainTags (file, info);

        if (formatHint == "FLAC")
            parseFlacStreamInfo (file);

        readBuffer.setSize (juce::jmax (1, srcChannels), kReadBlock, false, true, false);
        sourcePos = 0;
        errorOut.clear();
    }

    const AudioFileInfo& getInfo() const override { return info; }

    bool seekToMs (std::int64_t ms) override
    {
        if (reader == nullptr)
            return false;

        const auto sr = juce::jmax (1, info.sampleRate);
        auto frame = (ms * (std::int64_t) sr) / 1000;
        frame = juce::jlimit<std::int64_t> (0, info.totalFrames, frame);
        sourcePos = frame;
        finished = sourcePos >= info.totalFrames;
        return true;
    }

    int readFrames (double* interleavedStereo, int numFramesRequested) override
    {
        if (reader == nullptr || interleavedStereo == nullptr || numFramesRequested <= 0)
            return 0;

        if (finished || sourcePos >= info.totalFrames)
        {
            finished = true;
            return 0;
        }

        const int srcChannels = (int) reader->numChannels;
        int done = 0;

        if (flacMd5Enabled && sourcePos != flacMd5NextFrame)
            flacMd5Sequential = false;

        while (done < numFramesRequested)
        {
            const int remaining = (int) juce::jmin<std::int64_t> ((std::int64_t) (numFramesRequested - done),
                                                                 info.totalFrames - sourcePos);
            if (remaining <= 0)
            {
                finished = true;
                break;
            }

            const int chunk = juce::jmin (remaining, readBuffer.getNumSamples());
            const bool ok = reader->read (readBuffer.getArrayOfWritePointers(), srcChannels, sourcePos, chunk);

            if (! ok)
            {
                lastError = "Decode failed at frame " + juce::String (sourcePos) + " of "
                          + juce::String (info.totalFrames) + " (" + info.formatName
                          + " data corrupt or truncated)";
                break;
            }

            if (flacMd5Enabled && flacMd5Sequential)
                accumulateFlacMd5 (chunk);

            detail::downmixFloatToStereo (readBuffer.getArrayOfReadPointers(), srcChannels, chunk,
                                          interleavedStereo + (size_t) done * 2);
            done += chunk;
            sourcePos += chunk;

            if (sourcePos >= info.totalFrames)
            {
                finished = true;
                break;
            }
        }

        if (finished)
            finishFlacVerification();

        return done;
    }

    std::int64_t positionMs() const override
    {
        if (info.sampleRate <= 0)
            return 0;
        return (sourcePos * 1000) / info.sampleRate;
    }

    bool isFinished() const override { return finished; }
    void close() override { reader.reset(); }
    juce::String getLastError() const override { return lastError; }

private:
    static constexpr int kReadBlock = 8192;

    void parseFlacStreamInfo (const juce::File& file)
    {
        std::unique_ptr<juce::FileInputStream> stream (file.createInputStream());
        if (stream == nullptr)
            return;

        unsigned char header[8] {};
        if (stream->read (header, 8) != 8 || std::memcmp (header, "fLaC", 4) != 0)
            return;

        const int blockType = header[4] & 0x7f;
        const int blockLength = ((int) header[5] << 16) | ((int) header[6] << 8) | (int) header[7];
        if (blockType != 0 || blockLength < 34)
            return;

        unsigned char body[34] {};
        if (stream->read (body, 34) != 34)
            return;

        std::uint64_t packed = 0;
        for (int i = 10; i < 18; ++i)
            packed = (packed << 8) | (std::uint64_t) body[i];

        const std::uint64_t totalSamples = packed & 0xFFFFFFFFFull;
        const int flacBitsPerSample = (int) ((packed >> 36) & 0x1fu) + 1;   // stored as bits-1
        const int flacChannels = (int) (((packed >> 41) & 0x7u) + 1u);

        if (totalSamples == 0 || flacChannels != (int) reader->numChannels
            || flacBitsPerSample != (int) reader->bitsPerSample || flacBitsPerSample <= 0
            || flacBitsPerSample > 32)
        {
            return;
        }

        bool anyNonZero = false;
        for (int i = 0; i < 16; ++i)
        {
            flacExpectedMd5[(size_t) i] = body[18 + i];
            anyNonZero = anyNonZero || body[18 + i] != 0;
        }

        if (! anyNonZero)
            return;

        flacTotalSamples = (std::int64_t) totalSamples;
        flacBits = flacBitsPerSample;
        flacBytesPerSample = (flacBits + 7) / 8;
        flacMd5Enabled = true;
        flacMd5Sequential = true;
        flacMd5NextFrame = 0;
        flacMd5.reset();
    }

    /** Reconstructs the original integer sample from JUCE's fixed-to-float
        conversion (int32 sample<<(32-bits) divided by 2^31-1). */
    int32_t reconstructFlacSample (float value) const
    {
        const double scale = 2147483647.0 / (double) ((std::uint32_t) 1u << (32 - flacBits));
        const auto base = (std::int64_t) std::llround ((double) value * scale);
        const float inv = 1.0f / 2147483647.0f;
        const std::int64_t minValue = -((std::int64_t) 1 << (flacBits - 1));
        const std::int64_t maxValue = ((std::int64_t) 1 << (flacBits - 1)) - 1;

        for (int delta = 0; delta <= 4; ++delta)
        {
            for (int sign = (delta == 0 ? 1 : -1); sign <= 1; sign += 2)
            {
                const auto candidate = base + (std::int64_t) delta * sign;
                if (candidate < minValue || candidate > maxValue)
                    continue;
                const auto shifted = (std::int32_t) (candidate << (32 - flacBits));
                const float converted = (float) shifted * inv;
                if (converted == value)
                    return (int32_t) candidate;
            }
            if (delta == 0)
                break;
        }

        return (int32_t) base;
    }

    void accumulateFlacMd5 (int frames)
    {
        const int srcChannels = (int) reader->numChannels;
        unsigned char bytes[4];

        for (int i = 0; i < frames; ++i)
        {
            for (int ch = 0; ch < srcChannels; ++ch)
            {
                const int32_t sample = reconstructFlacSample (readBuffer.getReadPointer (ch)[i]);
                std::uint32_t bitsValue = (std::uint32_t) sample;
                for (int b = 0; b < flacBytesPerSample; ++b)
                {
                    bytes[b] = (unsigned char) (bitsValue & 0xffu);
                    bitsValue >>= 8;
                }
                flacMd5.update (bytes, (size_t) flacBytesPerSample);
            }
        }

        flacMd5NextFrame += frames;
    }

    void finishFlacVerification()
    {
        if (! flacMd5Enabled || flacMd5Checked)
            return;
        flacMd5Checked = true;

        if (! flacMd5Sequential || flacMd5NextFrame != flacTotalSamples)
            return;   // partial decode (seek / limited read): checksum not checkable

        std::uint8_t actual[16] {};
        flacMd5.finalize (actual);
        if (std::memcmp (actual, flacExpectedMd5, 16) != 0)
            lastError = "FLAC integrity check failed: the file is corrupt or truncated";
    }

    std::unique_ptr<juce::AudioFormatReader> reader;
    juce::AudioBuffer<float> readBuffer;
    AudioFileInfo info;
    juce::String rawFormatName;
    juce::String lastError;
    std::int64_t sourcePos = 0;
    bool lossy = false;
    bool finished = false;

    bool flacMd5Enabled = false;
    bool flacMd5Sequential = true;
    bool flacMd5Checked = false;
    Md5 flacMd5;
    std::uint8_t flacExpectedMd5[16] {};
    std::int64_t flacTotalSamples = 0;
    std::int64_t flacMd5NextFrame = 0;
    int flacBits = 0;
    int flacBytesPerSample = 0;
};

} // namespace

//==============================================================================
std::unique_ptr<DecodeSource> detail::createJuceDecodeSource (const juce::File& file, const juce::String& formatHint,
                                                             juce::String& errorOut)
{
    auto source = std::make_unique<JuceDecodeSource> (file, formatHint, errorOut);
    if (errorOut.isNotEmpty())
        return nullptr;
    return source;
}

juce::String detail::describeDecoderFor (const juce::File& file)
{
    const auto r = sniff (file, file.getFileExtension().toLowerCase());
    switch (r.backend)
    {
        case Backend::juce:         return "JUCE " + r.hint + " reader";
        case Backend::audioToolbox: return "AudioToolbox ExtAudioFile (" + r.hint + ")";
        case Backend::dsd:          return "Native DSD (" + r.hint + ")";
        case Backend::unknown:      break;
    }
    return {};
}

//==============================================================================
std::unique_ptr<DecodeSource> DecodeSource::create (const juce::File& file, juce::String& errorOut)
{
    errorOut.clear();

    if (! file.existsAsFile())
    {
        errorOut = "File not found: " + file.getFullPathName();
        return nullptr;
    }

    const auto ext = file.getFileExtension().toLowerCase();
    const auto r = sniff (file, ext);

    switch (r.backend)
    {
        case Backend::juce:
            return detail::createJuceDecodeSource (file, r.hint, errorOut);

        case Backend::audioToolbox:
        {
            auto source = detail::createAudioToolboxDecodeSource (file, errorOut);
            if (source != nullptr)
                return source;

            // Last-resort fallback: some CAF containers are readable through
            // JUCE. Only try when the AudioToolbox error is not a decode error.
            juce::String juceError;
            auto fallback = detail::createJuceDecodeSource (file, r.hint, juceError);
            if (fallback != nullptr)
            {
                errorOut.clear();
                return fallback;
            }
            return nullptr;
        }

        case Backend::dsd:
            return detail::createDsdDecodeSource (file, errorOut);

        case Backend::unknown:
            break;
    }

    errorOut = "Unsupported audio format: " + file.getFileName()
             + (ext.isNotEmpty() ? " (" + ext + ")" : juce::String())
             + ". Supported: " + supportedExtensions().joinIntoString (", ");
    return nullptr;
}

//==============================================================================
bool DecodeSource::probeInfo (const juce::File& file, AudioFileInfo& infoOut, juce::String& errorOut)
{
    auto source = create (file, errorOut);
    if (source == nullptr)
        return false;

    infoOut = source->getInfo();
    delete source.release();   // destructor closes the decoder; avoids keeping it open
    return true;
}

bool DecodeSource::isSupportedFile (const juce::File& file)
{
    const auto ext = file.getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".wave" || ext == ".aif" || ext == ".aiff" || ext == ".flac"
        || ext == ".mp3" || ext == ".ogg" || ext == ".oga" || ext == ".m4a" || ext == ".aac"
        || ext == ".alac" || ext == ".caf" || ext == ".dsf" || ext == ".dff";
}

juce::StringArray DecodeSource::supportedExtensions()
{
    return { "wav", "wave", "aif", "aiff", "flac", "mp3", "ogg", "oga", "m4a", "aac", "alac", "caf", "dsf", "dff" };
}

} // namespace sp
