/*
    Snake Puppy Reference Player
    macOS AudioToolbox ExtAudioFile decoder backend.

    Used for containers JUCE's stock readers do not cover: M4A (AAC and ALAC),
    raw AAC, CAF and other CoreAudio formats. ExtAudioFile is configured to
    deliver interleaved 32-bit float stereo-or-source-channel audio at the
    file's native sample rate (no hidden sample-rate conversion), which we then
    convert to the stereo interleaved double format the engine expects.

    Threading: single-threaded use (decode thread / test thread); performs disk
    I/O and must never be called from the audio callback.
*/
#include "Audio/DecodeBackends.h"
#include "Audio/DecodeUtils.h"

#if JUCE_MAC || JUCE_IOS

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstring>
#include <vector>

namespace sp
{

namespace
{
juce::String describeOSStatus (OSStatus status)
{
    const auto code = (std::uint32_t) status;
    juce::String four;
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        const auto c = (char) ((code >> shift) & 0xffu);
        four << (c >= 32 && c < 127 ? c : '?');
    }
    return juce::String ((int) status) + " ('" + four + "')";
}

class AudioToolboxDecodeSource final : public DecodeSource
{
public:
    AudioToolboxDecodeSource (const juce::File& file, juce::String& errorOut)
    {
        CFURLRef url = CFURLCreateFromFileSystemRepresentation (nullptr,
                                                               (const UInt8*) file.getFullPathName().toRawUTF8(),
                                                               (CFIndex) file.getFullPathName().getNumBytesAsUTF8(),
                                                               false);
        if (url == nullptr)
        {
            errorOut = "Could not create a file URL for " + file.getFileName();
            return;
        }

        OSStatus status = ExtAudioFileOpenURL (url, &fileRef);
        CFRelease (url);

        if (status != noErr || fileRef == nullptr)
        {
            errorOut = "AudioToolbox could not open " + file.getFileName() + ": " + describeOSStatus (status);
            return;
        }

        UInt32 size = sizeof (fileFormat);
        status = ExtAudioFileGetProperty (fileRef, kExtAudioFileProperty_FileDataFormat, &size, &fileFormat);
        if (status != noErr)
        {
            errorOut = "AudioToolbox could not read the format of " + file.getFileName() + ": " + describeOSStatus (status);
            close();
            return;
        }

        SInt64 lengthFrames = 0;
        size = sizeof (lengthFrames);
        status = ExtAudioFileGetProperty (fileRef, kExtAudioFileProperty_FileLengthFrames, &size, &lengthFrames);
        if (status != noErr || lengthFrames <= 0)
        {
            errorOut = "AudioToolbox reported no audio frames in " + file.getFileName()
                     + (status != noErr ? " (" + describeOSStatus (status) + ")" : "");
            close();
            return;
        }

        const int channels = (int) fileFormat.mChannelsPerFrame;
        if (channels <= 0 || channels > 16)
        {
            errorOut = "Unsupported channel count (" + juce::String (channels) + ") in " + file.getFileName();
            close();
            return;
        }

        if (fileFormat.mSampleRate <= 0.0)
        {
            errorOut = "Invalid sample rate in " + file.getFileName();
            close();
            return;
        }

        sourceChannels = channels;
        totalFrames = (std::int64_t) lengthFrames;

        AudioStreamBasicDescription client {};
        client.mSampleRate = fileFormat.mSampleRate;
        client.mFormatID = kAudioFormatLinearPCM;
        client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        client.mFramesPerPacket = 1;
        client.mChannelsPerFrame = (UInt32) channels;
        client.mBitsPerChannel = 32;
        client.mBytesPerFrame = (UInt32) (4 * channels);
        client.mBytesPerPacket = (UInt32) (4 * channels);

        status = ExtAudioFileSetProperty (fileRef, kExtAudioFileProperty_ClientDataFormat,
                                          sizeof (client), &client);
        if (status != noErr)
        {
            errorOut = "AudioToolbox rejected the float client format for " + file.getFileName()
                     + ": " + describeOSStatus (status);
            close();
            return;
        }

        const auto formatId = (std::uint32_t) fileFormat.mFormatID;
        formatName = detail::formatNameForFourCC (formatId);
        lossy = detail::isLossyFormat (formatId);

        info.path = file.getFullPathName();
        info.sampleRate = juce::roundToInt (fileFormat.mSampleRate);
        info.numChannels = channels;
        info.totalFrames = totalFrames;
        info.durationMs = info.sampleRate > 0 ? (totalFrames * 1000) / info.sampleRate : 0;
        info.formatName = formatName;
        info.downmixedToStereo = channels > 2;

        int bits = (int) fileFormat.mBitsPerChannel;

        if (detail::isALACFormat (formatId))
        {
            info.family = Track::Family::pcmInteger;
            if (bits <= 0 || bits > 32 || bits == 32)
                bits = readALACBitDepth (file);
            if (bits <= 0)
                bits = queryALACBitDepth();
            info.bitDepth = bits > 0 ? bits : 0;
        }
        else if (lossy)
        {
            info.family = Track::Family::lossy;
            info.bitDepth = 0;
        }
        else if (fileFormat.mFormatFlags & kAudioFormatFlagIsFloat)
        {
            info.family = Track::Family::pcmFloat;
            info.bitDepth = bits > 0 ? bits : 32;
        }
        else if (detail::isLinearPCMFormat (formatId))
        {
            info.family = Track::Family::pcmInteger;
            info.bitDepth = bits > 0 ? bits : 0;
        }
        else
        {
            info.family = Track::Family::unknown;
            info.bitDepth = bits > 0 ? bits : 0;
        }

        info.decoderName = "AudioToolbox ExtAudioFile (" + formatName + ", "
                         + juce::String (info.sampleRate) + " Hz)";

        detail::parseReplayGainTags (file, info);

        readStorage.assign ((size_t) channels * (size_t) kReadBlock, 0.0f);
        errorOut.clear();
    }

    ~AudioToolboxDecodeSource() override { close(); }

    const AudioFileInfo& getInfo() const override { return info; }

    bool seekToMs (std::int64_t ms) override
    {
        if (fileRef == nullptr)
            return false;

        auto frame = (ms * (std::int64_t) juce::jmax (1, info.sampleRate)) / 1000;
        frame = juce::jlimit<std::int64_t> (0, totalFrames, frame);

        const OSStatus status = ExtAudioFileSeek (fileRef, (SInt64) frame);
        if (status != noErr)
        {
            lastError = "AudioToolbox could not seek " + info.path + ": " + describeOSStatus (status);
            return false;
        }

        filePos = frame;
        finished = filePos >= totalFrames;
        return true;
    }

    int readFrames (double* interleavedStereo, int numFramesRequested) override
    {
        if (fileRef == nullptr || interleavedStereo == nullptr || numFramesRequested <= 0)
            return 0;

        if (finished || filePos >= totalFrames)
        {
            finished = true;
            return 0;
        }

        int done = 0;

        while (done < numFramesRequested)
        {
            const int remaining = (int) juce::jmin<std::int64_t> ((std::int64_t) (numFramesRequested - done),
                                                                 totalFrames - filePos);
            if (remaining <= 0)
            {
                finished = true;
                break;
            }

            const int chunk = juce::jmin (remaining, kReadBlock);

            AudioBufferList list {};
            list.mNumberBuffers = 1;
            list.mBuffers[0].mNumberChannels = (UInt32) sourceChannels;
            list.mBuffers[0].mDataByteSize = (UInt32) (sizeof (float) * (size_t) chunk * (size_t) sourceChannels);
            list.mBuffers[0].mData = readStorage.data();

            UInt32 framesToRead = (UInt32) chunk;
            const OSStatus status = ExtAudioFileRead (fileRef, &framesToRead, &list);

            if (status != noErr)
            {
                lastError = "AudioToolbox decode failed at frame " + juce::String (filePos) + " of "
                          + juce::String (totalFrames) + ": " + describeOSStatus (status);
                break;
            }

            if (framesToRead == 0)
            {
                finished = true;
                break;
            }

            float* channelPointers[16] {};
            for (int ch = 0; ch < sourceChannels; ++ch)
                channelPointers[ch] = readStorage.data() + ch;   // stride = sourceChannels => interleaved view

            detail::downmixFloatToStereo (channelPointers, sourceChannels, (int) framesToRead,
                                          interleavedStereo + (size_t) done * 2);

            done += (int) framesToRead;
            filePos += (std::int64_t) framesToRead;

            if (filePos >= totalFrames)
            {
                finished = true;
                break;
            }
        }

        return done;
    }

    std::int64_t positionMs() const override
    {
        if (info.sampleRate <= 0)
            return 0;
        return (filePos * 1000) / info.sampleRate;
    }

    bool isFinished() const override { return finished; }
    juce::String getLastError() const override { return lastError; }

    void close() override
    {
        if (fileRef != nullptr)
        {
            ExtAudioFileDispose (fileRef);
            fileRef = nullptr;
        }
    }

private:
    static constexpr int kReadBlock = 4096;

    /** Reads the ALAC magic-cookie bit depth from the MP4 'alac' box: it is the
        byte after frameLength + compatibleVersion, i.e. +13 from the fourcc. */
    static int readALACBitDepth (const juce::File& file)
    {
        std::unique_ptr<juce::FileInputStream> stream (file.createInputStream());
        if (stream == nullptr)
            return 0;
        const auto total = stream->getTotalLength();
        std::vector<char> bytes;
        bytes.reserve ((size_t) (512 * 1024));

        auto append = [&] (juce::int64 start, juce::int64 length)
        {
            if (length <= 0 || ! stream->setPosition (start))
                return;
            const size_t old = bytes.size();
            bytes.resize (old + (size_t) length);
            const int got = stream->read (bytes.data() + old, (int) length);
            bytes.resize (old + (size_t) juce::jmax (0, got));
        };
        append (0, juce::jmin<juce::int64> (total, 512 * 1024));
        if (total > 512 * 1024)
            append (juce::jmax<juce::int64> (0, total - 1024 * 1024), juce::jmin<juce::int64> (total - 512 * 1024, 1024 * 1024));

        for (size_t i = 0; i + 14 < bytes.size(); ++i)
        {
            if (std::memcmp (bytes.data() + i, "alac", 4) != 0)
                continue;
            const auto bitDepth = (unsigned char) bytes[i + 13];
            if (bitDepth == 16 || bitDepth == 20 || bitDepth == 24 || bitDepth == 32)
                return (int) bitDepth;
        }
        return 0;
    }

    int queryALACBitDepth() const
    {
        // ALAC bit depth lives in the codec magic cookie; the ExtAudioFile
        // FileDataFormat sometimes leaves mBitsPerChannel at zero. Ask the
        // audio converter behind ExtAudioFile for the output format instead.
        AudioConverterRef converter = nullptr;
        UInt32 size = sizeof (converter);
        if (ExtAudioFileGetProperty (fileRef, kExtAudioFileProperty_AudioConverter, &size, &converter) == noErr
            && converter != nullptr)
        {
            AudioStreamBasicDescription output {};
            size = sizeof (output);
            if (AudioConverterGetProperty (converter, kAudioConverterCurrentOutputStreamDescription,
                                           &size, &output) == noErr)
                return (int) output.mBitsPerChannel;
        }
        return 0;
    }

    ExtAudioFileRef fileRef = nullptr;
    AudioStreamBasicDescription fileFormat {};
    AudioFileInfo info;
    juce::String formatName;
    juce::String lastError;
    std::vector<float> readStorage;
    int sourceChannels = 0;
    std::int64_t totalFrames = 0;
    std::int64_t filePos = 0;
    bool lossy = false;
    bool finished = false;
};

} // namespace

std::unique_ptr<DecodeSource> detail::createAudioToolboxDecodeSource (const juce::File& file, juce::String& errorOut)
{
    auto source = std::make_unique<AudioToolboxDecodeSource> (file, errorOut);
    if (errorOut.isNotEmpty())
        return nullptr;
    return source;
}

} // namespace sp

#else

namespace sp
{
std::unique_ptr<DecodeSource> detail::createAudioToolboxDecodeSource (const juce::File&, juce::String& errorOut)
{
    errorOut = "AudioToolbox decoding is only available on macOS/iOS";
    return nullptr;
}
} // namespace sp

#endif
