/*
    Snake Puppy Reference Player
    Decoder backend helpers. See DecodeUtils.h.
*/
#include "Audio/DecodeUtils.h"
#include <cstdlib>
#include <cstring>
#include <vector>

namespace sp::detail
{

namespace
{
struct ChannelLayout
{
    int frontLeft = 0, frontRight = 1, frontCentre = -1, backLeft = -1, backRight = -1;

    static ChannelLayout forCount (int n)
    {
        ChannelLayout l;
        if (n >= 6)      { l.frontCentre = 2; l.backLeft = 4; l.backRight = 5; }
        else if (n == 5) { l.frontCentre = 2; l.backLeft = 3; l.backRight = 4; }
        else if (n == 4) { l.backLeft = 2; l.backRight = 3; }
        else if (n == 3) { l.frontCentre = 2; }
        return l;
    }
};

constexpr double kMinus3dB = 0.7071067811865476;
} // namespace

void downmixFloatToStereo (const float* const* src, int numSrcChannels, int frames, double* destInterleaved)
{
    if (src == nullptr || destInterleaved == nullptr || frames <= 0)
        return;

    if (numSrcChannels <= 0)
    {
        std::memset (destInterleaved, 0, sizeof (double) * (size_t) frames * 2);
        return;
    }

    if (numSrcChannels == 1)
    {
        const float* mono = src[0];
        for (int i = 0; i < frames; ++i)
        {
            const double v = mono[i];
            destInterleaved[i * 2] = v;
            destInterleaved[i * 2 + 1] = v;
        }
        return;
    }

    if (numSrcChannels == 2)
    {
        const float* l = src[0];
        const float* r = src[1];
        for (int i = 0; i < frames; ++i)
        {
            destInterleaved[i * 2] = l[i];
            destInterleaved[i * 2 + 1] = r[i];
        }
        return;
    }

    const auto layout = ChannelLayout::forCount (numSrcChannels);

    for (int i = 0; i < frames; ++i)
    {
        double left = src[layout.frontLeft][i];
        double right = src[layout.frontRight][i];

        if (layout.frontCentre >= 0 && layout.frontCentre < numSrcChannels)
        {
            left += kMinus3dB * src[layout.frontCentre][i];
            right += kMinus3dB * src[layout.frontCentre][i];
        }
        if (layout.backLeft >= 0 && layout.backLeft < numSrcChannels)
            left += kMinus3dB * src[layout.backLeft][i];
        if (layout.backRight >= 0 && layout.backRight < numSrcChannels)
            right += kMinus3dB * src[layout.backRight][i];

        destInterleaved[i * 2] = left;
        destInterleaved[i * 2 + 1] = right;
    }
}

void deinterleaveStereo (const double* interleaved, double* left, double* right, int frames)
{
    for (int i = 0; i < frames; ++i)
    {
        left[i] = interleaved[i * 2];
        right[i] = interleaved[i * 2 + 1];
    }
}

//==============================================================================
namespace
{
void scanTags (const char* data, size_t size, AudioFileInfo& info)
{
    static constexpr const char* kTrackGain = "REPLAYGAIN_TRACK_GAIN";
    static constexpr const char* kAlbumGain = "REPLAYGAIN_ALBUM_GAIN";
    static constexpr const char* kTrackPeak = "REPLAYGAIN_TRACK_PEAK";
    static constexpr const char* kAlbumPeak = "REPLAYGAIN_ALBUM_PEAK";

    auto tryParse = [&] (const char* key, float& target) -> bool
    {
        const size_t keyLen = std::strlen (key);
        const char* end = data + size;

        for (const char* p = data; p + keyLen <= end; ++p)
        {
            if (std::memcmp (p, key, keyLen) != 0)
                continue;

            const char* q = p + keyLen;
            // Vorbis comments use '=', ID3 TXXX frames put the value after a
            // NUL terminator. Skip both plus stray whitespace/quotes.
            int guard = 0;
            while (q < end && guard < 96)
            {
                const char c = *q;
                if (c == 0 || c == '=' || c == ' ' || c == '\t' || c == '"' || c == '\'')
                {
                    ++q;
                    ++guard;
                    continue;
                }
                break;
            }

            if (q >= end || guard >= 96)
                continue;

            char* parseEnd = nullptr;
            const double v = std::strtod (q, &parseEnd);
            if (parseEnd == q)
                continue;

            target = (float) v;
            return true;
        }

        return false;
    };

    if (! info.hasReplayGainTrack)
    {
        float v = 0.0f;
        if (tryParse (kTrackGain, v))
        {
            info.replayGainTrackDb = v;
            info.hasReplayGainTrack = true;
        }
    }

    if (! info.hasReplayGainAlbum)
    {
        float v = 0.0f;
        if (tryParse (kAlbumGain, v))
        {
            info.replayGainAlbumDb = v;
            info.hasReplayGainAlbum = true;
        }
    }

    if (info.peakLinear <= 0.0f)
    {
        float v = 0.0f;
        if (tryParse (kTrackPeak, v) || tryParse (kAlbumPeak, v))
            info.peakLinear = v;
    }
}
} // namespace

bool parseReplayGainTags (const juce::File& file, AudioFileInfo& infoOut)
{
    std::unique_ptr<juce::FileInputStream> stream (file.createInputStream());
    if (stream == nullptr)
        return false;

    const auto total = stream->getTotalLength();
    if (total <= 0)
        return false;

    constexpr juce::int64 kHeadBytes = 512 * 1024;
    constexpr juce::int64 kTailBytes = 1024 * 1024;

    std::vector<char> bytes;
    bytes.reserve ((size_t) (kHeadBytes + kTailBytes));

    auto appendRange = [&] (juce::int64 start, juce::int64 length)
    {
        if (length <= 0)
            return;
        if (! stream->setPosition (start))
            return;
        const size_t oldSize = bytes.size();
        bytes.resize (oldSize + (size_t) length);
        const int read = stream->read (bytes.data() + oldSize, (int) length);
        bytes.resize (oldSize + (size_t) juce::jmax (0, read));
    };

    appendRange (0, juce::jmin (total, kHeadBytes));
    if (total > kHeadBytes)
        appendRange (juce::jmax<juce::int64> (0, total - kTailBytes), juce::jmin (total - kHeadBytes, kTailBytes));

    if (bytes.empty())
        return false;

    scanTags (bytes.data(), bytes.size(), infoOut);
    return infoOut.hasReplayGainTrack || infoOut.hasReplayGainAlbum;
}

//==============================================================================
juce::String formatNameForFourCC (std::uint32_t fourCC)
{
    switch (fourCC)
    {
        case 0x6c70636d: return "PCM";          // 'lpcm'
        case 0x616c6163: return "ALAC";         // 'alac'
        case 0x61616320: return "AAC";          // 'aac '
        case 0x61616370: return "AAC";          // 'aacp'
        case 0x2e6d7033: return "MP3";          // '.mp3'
        case 0x6d7033U:  return "MP3";          // 'mp3 '
        case 0x666c6163: return "FLAC";         // 'flac'
        case 0x6f707573: return "Opus";         // 'opus'
        case 0x616c6177: return "A-law";        // 'alaw'
        case 0x756c6177: return "u-law";        // 'ulaw'
        case 0x696d6134: return "AAC";          // 'ima4'
        default: break;
    }

    // Print any printable fourCC for honest diagnostics.
    juce::String s;
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        const auto c = (char) ((fourCC >> shift) & 0xffu);
        s << (c >= 32 && c < 127 ? c : '?');
    }
    return s;
}

bool isLinearPCMFormat (std::uint32_t fourCC)
{
    return fourCC == 0x6c70636d; // 'lpcm'
}

bool isALACFormat (std::uint32_t fourCC)
{
    return fourCC == 0x616c6163; // 'alac'
}

bool isLossyFormat (std::uint32_t fourCC)
{
    switch (fourCC)
    {
        case 0x61616320: // 'aac '
        case 0x61616370: // 'aacp'
        case 0x2e6d7033: // '.mp3'
        case 0x6d7033U:  // 'mp3 '
        case 0x6f707573: // 'opus'
        case 0x696d6134: // 'ima4'
        case 0x616c6177: // 'alaw'
        case 0x756c6177: // 'ulaw'
            return true;
        default:
            return false;
    }
}

} // namespace sp::detail
