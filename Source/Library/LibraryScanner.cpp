#include "Library/LibraryScanner.h"

#include "Audio/DecodeSource.h"

#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace sp::scanner
{

namespace
{
//==============================================================================
// Compact RFC 1321 MD5. JUCE's juce_cryptography module is not linked into this
// project, so the library keeps its own small implementation for content and
// artwork hashes. Verified against the standard test vectors in the test suite.
class Md5
{
public:
    void update (const void* data, size_t numBytes)
    {
        auto* bytes = static_cast<const unsigned char*> (data);
        totalBytes += numBytes;

        while (numBytes > 0)
        {
            const size_t space = 64 - bufferSize;
            const size_t take = numBytes < space ? numBytes : space;
            std::memcpy (buffer + bufferSize, bytes, take);
            bufferSize += take;
            bytes += take;
            numBytes -= take;

            if (bufferSize == 64)
            {
                transform (buffer);
                bufferSize = 0;
            }
        }
    }

    juce::String hexDigest()
    {
        const std::uint64_t bitCount = totalBytes * 8;

        unsigned char padding[72] = {};
        padding[0] = 0x80;
        const size_t padLen = (bufferSize < 56) ? (56 - bufferSize) : (120 - bufferSize);

        unsigned char lengthBytes[8];
        for (int i = 0; i < 8; ++i)
            lengthBytes[i] = (unsigned char) ((bitCount >> (8 * i)) & 0xff);

        update (padding, padLen);
        update (lengthBytes, 8);

        unsigned char digest[16];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                digest[i * 4 + j] = (unsigned char) ((state[i] >> (8 * j)) & 0xff);

        return juce::String::toHexString (digest, 16, 0);
    }

private:
    static std::uint32_t rotl (std::uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

    void transform (const unsigned char block[64])
    {
        static const std::uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
        };
        static const int S[64] = {
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
            5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
        };

        std::uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = (std::uint32_t) block[i * 4]
                 | ((std::uint32_t) block[i * 4 + 1] << 8)
                 | ((std::uint32_t) block[i * 4 + 2] << 16)
                 | ((std::uint32_t) block[i * 4 + 3] << 24);

        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];

        for (int i = 0; i < 64; ++i)
        {
            std::uint32_t f;
            int g;

            if (i < 16)      { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) % 16; }
            else             { f = c ^ (b | ~d);       g = (7 * i) % 16; }

            const std::uint32_t tmp = d;
            d = c;
            c = b;
            b = b + rotl (a + f + K[i] + M[g], S[i]);
            a = tmp;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
    }

    std::uint32_t state[4] { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
    std::uint64_t totalBytes = 0;
    unsigned char buffer[64] {};
    size_t bufferSize = 0;
};

//==============================================================================
std::uint32_t readLE32 (const unsigned char* p)
{
    return (std::uint32_t) p[0] | ((std::uint32_t) p[1] << 8) | ((std::uint32_t) p[2] << 16)
         | ((std::uint32_t) p[3] << 24);
}

std::uint32_t readBE32 (const unsigned char* p)
{
    return ((std::uint32_t) p[0] << 24) | ((std::uint32_t) p[1] << 16) | ((std::uint32_t) p[2] << 8)
         | (std::uint32_t) p[3];
}

std::uint64_t readLE64 (const unsigned char* p)
{
    return (std::uint64_t) readLE32 (p) | ((std::uint64_t) readLE32 (p + 4) << 32);
}

std::uint64_t readBE64 (const unsigned char* p)
{
    return ((std::uint64_t) readBE32 (p) << 32) | (std::uint64_t) readBE32 (p + 4);
}

juce::String fourCC (const unsigned char* p)
{
    return juce::String (juce::String::fromUTF8 ((const char*) p, 4));
}

int dsdMultipleFor (int sampleRate)
{
    if (sampleRate <= 0)
        return 0;
    const int multiple = (int) std::lround ((double) sampleRate / 44100.0);
    return (multiple == 64 || multiple == 128 || multiple == 256 || multiple == 512) ? multiple : 0;
}

//==============================================================================
bool readHeaderBytes (const juce::File& file, std::vector<unsigned char>& out, int maxBytes)
{
    juce::FileInputStream in (file);
    if (! in.openedOk())
        return false;

    out.resize ((size_t) maxBytes);
    const int got = in.read (out.data(), maxBytes);
    if (got <= 0)
        return false;
    out.resize ((size_t) got);
    return true;
}

bool fileContainsFourCC (const juce::File& file, const char* code, std::int64_t maxBytes)
{
    juce::FileInputStream in (file);
    if (! in.openedOk())
        return false;

    std::vector<char> buffer ((size_t) juce::jmin<std::int64_t> (maxBytes, 1 << 20));
    std::int64_t totalRead = 0;
    int got = 0;
    while (totalRead < maxBytes && (got = in.read (buffer.data(), (int) buffer.size())) > 0)
    {
        totalRead += got;
        if (got >= 4)
            for (int i = 0; i <= got - 4; ++i)
                if (std::memcmp (buffer.data() + i, code, 4) == 0)
                    return true;
    }
    return false;
}

Track::Family familyForFile (const juce::File& file, const AudioFileInfo& info)
{
    if (info.isDsd)
        return Track::Family::dsd;

    const auto ext = file.getFileExtension().toLowerCase();

    // Trust the container/codec identity for well-known extensions: a decoder's
    // "family" can be wrong for lossy codecs it decodes through float buffers.
    if (ext == ".mp3" || ext == ".ogg" || ext == ".oga" || ext == ".aac")
        return Track::Family::lossy;

    if (ext == ".m4a")
    {
        // An M4A container is lossless when it carries an ALAC stream, lossy
        // when it carries AAC. Read the first chunk of the file to find out.
        if (fileContainsFourCC (file, "alac", 4 * 1024 * 1024))
            return Track::Family::pcmInteger;
        return Track::Family::lossy;
    }

    if (ext == ".flac" || ext == ".wav" || ext == ".wave" || ext == ".aif" || ext == ".aiff"
        || ext == ".alac")
        return info.family == Track::Family::pcmFloat ? Track::Family::pcmFloat
                                                      : Track::Family::pcmInteger;

    // CAF and unknown containers: keep what the decoder reported, if anything.
    return info.family;
}

juce::AudioFormatManager& fallbackFormatManager()
{
    static juce::AudioFormatManager manager;
    static std::once_flag once;
    std::call_once (once, [] { manager.registerBasicFormats(); });
    return manager;
}

juce::String fallbackTitle (const juce::File& file)
{
    auto name = file.getFileNameWithoutExtension().trim();

    // "01 - Track Name" -> "Track Name"; otherwise keep the filename as-is.
    int i = 0;
    while (i < name.length() && juce::CharacterFunctions::isDigit (name[i]))
        ++i;

    if (i > 0 && i < name.length())
    {
        auto rest = name.substring (i).trimStart();
        if (rest.startsWithChar ('-') || rest.startsWithChar ('.') || rest.startsWithChar ('_'))
            rest = rest.substring (1).trimStart();
        if (rest.isNotEmpty())
            return rest;
    }

    return name;
}

} // namespace

//==============================================================================
const juce::StringArray& supportedExtensions()
{
    static const juce::StringArray exts = {
        "wav", "wave", "aif", "aiff", "flac", "mp3", "ogg", "oga", "m4a", "aac", "alac", "dsf", "dff", "caf"
    };
    return exts;
}

bool isSupportedAudioFile (const juce::File& file)
{
    const auto ext = file.getFileExtension().toLowerCase().substring (1);
    return ext.isNotEmpty() && supportedExtensions().contains (ext);
}

bool isDsdFile (const juce::File& file)
{
    const auto ext = file.getFileExtension().toLowerCase();
    return ext == ".dsf" || ext == ".dff";
}

//==============================================================================
bool parseDsdHeader (const juce::File& file, AudioFileInfo& infoOut, juce::String& errorOut)
{
    std::vector<unsigned char> bytes;
    if (! readHeaderBytes (file, bytes, 4096))
    {
        errorOut = "Could not read the DSD file header.";
        return false;
    }

    if (bytes.size() < 16)
    {
        errorOut = "DSD file is too short to contain a header.";
        return false;
    }

    auto finish = [&] (int channels, int sampleRate, std::uint64_t sampleCount, const juce::String& format)
    {
        if (channels < 1 || channels > 8 || sampleRate < 1000000 || sampleRate > 100000000
            || sampleCount == 0)
        {
            errorOut = "DSD header contains invalid stream parameters.";
            return false;
        }

        infoOut = {};
        infoOut.path = file.getFullPathName();
        infoOut.sampleRate = sampleRate;
        infoOut.bitDepth = 1;
        infoOut.numChannels = channels;
        infoOut.totalFrames = (std::int64_t) sampleCount;
        infoOut.durationMs = (std::int64_t) std::llround ((double) sampleCount * 1000.0 / (double) sampleRate);
        infoOut.formatName = format;
        infoOut.family = Track::Family::dsd;
        infoOut.isDsd = true;
        infoOut.dsdMultiple = dsdMultipleFor (sampleRate);
        infoOut.dsdGainDb = -6.0f;
        infoOut.decoderName = format == "DSF" ? "Native DSF" : "Native DFF";
        return true;
    };

    if (std::memcmp (bytes.data(), "DSD ", 4) == 0)
    {
        if (bytes.size() < 80)
        {
            errorOut = "DSF header is truncated.";
            return false;
        }

        const auto headerSize = readLE64 (bytes.data() + 4);
        if (headerSize < 28 || headerSize > 256)
        {
            errorOut = "DSF header size is invalid.";
            return false;
        }

        size_t pos = (size_t) headerSize;
        while (pos + 12 <= bytes.size())
        {
            const auto chunkId = fourCC (bytes.data() + pos);
            const auto chunkSize = readLE64 (bytes.data() + pos + 4);
            if (chunkSize < 12 || pos + chunkSize > bytes.size())
                break;

            if (chunkId == "fmt " && chunkSize >= 12 + 40)
            {
                const auto* body = bytes.data() + pos + 12;
                const int channels = (int) readLE32 (body + 12);
                const int sampleRate = (int) readLE32 (body + 16);
                const std::uint64_t sampleCount = readLE64 (body + 24);
                return finish (channels, sampleRate, sampleCount, "DSF");
            }

            pos += (size_t) chunkSize;
        }

        errorOut = "DSF file has no readable fmt chunk.";
        return false;
    }

    if (std::memcmp (bytes.data(), "FRM8", 4) == 0)
    {
        size_t pos = 16;
        while (pos + 12 <= bytes.size())
        {
            const auto chunkId = fourCC (bytes.data() + pos);
            const auto chunkSize = readBE64 (bytes.data() + pos + 4);
            if (chunkSize < 4 || pos + 12 + chunkSize > bytes.size())
                break;

            if (chunkId == "PROP")
            {
                size_t inner = pos + 12;
                const size_t end = inner + (size_t) chunkSize;
                if (inner + 4 <= bytes.size() && fourCC (bytes.data() + inner) == "SND ")
                    inner += 4;

                while (inner + 12 <= bytes.size() && inner + 12 <= end)
                {
                    const auto subId = fourCC (bytes.data() + inner);
                    const auto subSize = readBE64 (bytes.data() + inner + 4);
                    if (subSize < 4 || inner + 12 + subSize > bytes.size())
                        break;

                    if (subId == "fmt " && subSize >= 40)
                    {
                        const auto* body = bytes.data() + inner + 12;
                        const int channels = (int) readBE32 (body + 12);
                        const int sampleRate = (int) readBE32 (body + 16);
                        const std::uint64_t sampleCount = readBE64 (body + 24);
                        return finish (channels, sampleRate, sampleCount, "DFF");
                    }

                    inner += 12 + (size_t) subSize;
                }
            }

            pos += 12 + (size_t) chunkSize;
        }

        errorOut = "DFF file has no readable fmt chunk.";
        return false;
    }

    errorOut = "Not a DSF or DFF file.";
    return false;
}

//==============================================================================
bool probeAudioFile (const juce::File& file, AudioFileInfo& infoOut, juce::String& errorOut)
{
    errorOut.clear();

    if (! file.existsAsFile())
    {
        errorOut = "File not found: " + file.getFullPathName();
        return false;
    }

    // 1. Native DSF/DFF header parsing: cheap, exact, and independent of the
    //    decoder implementation (the DSD decoder uses the same header).
    if (isDsdFile (file))
    {
        if (parseDsdHeader (file, infoOut, errorOut))
            return true;
        // Fall through to the decoder in case the header parser is too strict.
    }

    // 2. The audio engine's own probe (understands DSD and platform codecs).
    {
        AudioFileInfo decoded;
        juce::String decodeError;
        if (DecodeSource::probeInfo (file, decoded, decodeError) && decoded.sampleRate > 0)
        {
            decoded.path = file.getFullPathName();

            // The container name comes from magic-byte/extension detection so
            // that e.g. MP3 never shows up as "AU"; the decoder supplies the
            // audio properties.
            const auto detected = tags::detectFormatName (file);
            if (detected.isNotEmpty())
                decoded.formatName = detected;
            else if (decoded.formatName.isEmpty())
                decoded.formatName = "Unknown";

            if (decoded.durationMs <= 0 && decoded.totalFrames > 0)
                decoded.durationMs = (std::int64_t) std::llround ((double) decoded.totalFrames * 1000.0
                                                                  / (double) decoded.sampleRate);

            decoded.family = familyForFile (file, decoded);
            if (decoded.family == Track::Family::lossy)
                decoded.bitDepth = 0;   // lossy codecs have no meaningful integer bit depth

            infoOut = decoded;
            return true;
        }
    }

    // 3. JUCE's format readers (WAV/AIFF/FLAC/MP3/OGG, plus CoreAudio M4A on macOS).
    std::unique_ptr<juce::AudioFormatReader> reader (fallbackFormatManager().createReaderFor (file));
    if (reader == nullptr)
    {
        errorOut = "Unsupported or unreadable audio format: " + file.getFileName();
        return false;
    }

    AudioFileInfo info;
    info.path = file.getFullPathName();
    info.sampleRate = (int) std::lround (reader->sampleRate);
    info.bitDepth = (int) reader->bitsPerSample;
    info.numChannels = (int) reader->numChannels;
    info.totalFrames = reader->lengthInSamples;
    info.formatName = tags::detectFormatName (file);
    if (info.formatName.isEmpty())
        info.formatName = reader->getFormatName();
    info.decoderName = "JUCE " + reader->getFormatName();

    if (info.sampleRate <= 0)
    {
        errorOut = "Audio format reports an invalid sample rate: " + file.getFileName();
        return false;
    }

    info.durationMs = (std::int64_t) std::llround ((double) info.totalFrames * 1000.0 / (double) info.sampleRate);
    info.isDsd = false;
    info.family = familyForFile (file, info);
    if (info.family == Track::Family::lossy)
        info.bitDepth = 0;

    infoOut = info;
    return true;
}

//==============================================================================
bool buildTrackInfo (const juce::File& file, const juce::File& artworkDir, bool extractArtwork,
                     Track& trackOut, TrackFileInfo& status)
{
    status = {};

    Track track;
    track.path = file.getFullPathName();
    track.folder = file.getParentDirectory().getFullPathName();
    track.fileSize = file.getSize();
    track.fileModifiedMs = file.getLastModificationTime().toMilliseconds();

    AudioFileInfo info;
    status.probed = probeAudioFile (file, info, status.probeError);
    if (! status.probed)
        return false;

    track.durationMs = info.durationMs;
    track.sampleRate = info.sampleRate;
    track.bitDepth = info.bitDepth;
    track.numChannels = info.numChannels;
    track.formatName = info.formatName.isNotEmpty() ? info.formatName : tags::detectFormatName (file);
    track.family = info.family;
    track.isDsd = info.isDsd;
    track.dsdRateMultiple = info.dsdMultiple;
    track.dsdGainDb = info.dsdGainDb;

    if (track.durationMs > 0 && track.fileSize > 0)
        track.bitrateKbps = (int) std::llround ((double) track.fileSize * 8.0 / (double) track.durationMs);

    // Tags. A failure is reported but is not fatal: the file is still playable
    // and the library keeps a filename-derived title.
    tags::TagData tagData;
    juce::String tagError;
    status.tagsRead = tags::readTags (file, tagData, tagError);
    if (status.tagsRead)
    {
        track.title = tagData.title;
        track.artist = tagData.artist;
        track.albumArtist = tagData.albumArtist;
        track.album = tagData.album;
        track.genre = tagData.genre;
        track.composer = tagData.composer;
        track.comment = tagData.comment;
        track.year = tagData.year;
        track.trackNumber = tagData.trackNumber;
        track.discNumber = tagData.discNumber;
        track.rating = juce::jlimit (0, 5, tagData.rating);

        track.replayGainTrackDb = tagData.replayGainTrackDb;
        track.hasReplayGainTrack = tagData.hasReplayGainTrack;
        track.replayGainAlbumDb = tagData.replayGainAlbumDb;
        track.hasReplayGainAlbum = tagData.hasReplayGainAlbum;
        track.peakLinear = tagData.peakLinear;
    }
    else
    {
        status.tagError = tagError;
    }

    if (track.title.isEmpty())
        track.title = fallbackTitle (file);

    if (extractArtwork)
    {
        juce::String artworkError;
        auto artwork = tags::readArtwork (file, artworkError);

        const tags::EmbeddedArtwork* chosen = nullptr;
        for (auto& a : artwork)
            if (a.isFrontCover() && a.data.getSize() > 0) { chosen = &a; break; }
        if (chosen == nullptr)
            for (auto& a : artwork)
                if (a.data.getSize() > 0) { chosen = &a; break; }

        if (chosen != nullptr)
        {
            juce::String id;
            juce::String cacheError;
            auto cached = cacheArtwork (artworkDir, chosen->data, chosen->mimeType, id, cacheError);
            if (cached.existsAsFile())
            {
                track.artworkId = id;
                status.artworkFound = true;
            }
        }
    }

    trackOut = std::move (track);
    return true;
}

//==============================================================================
juce::String md5Bytes (const void* data, size_t numBytes)
{
    Md5 md5;
    md5.update (data, numBytes);
    return md5.hexDigest();
}

juce::String md5File (const juce::File& file)
{
    juce::FileInputStream in (file);
    if (! in.openedOk())
        return {};

    Md5 md5;
    std::vector<unsigned char> buffer (64 * 1024);
    for (;;)
    {
        const int got = in.read (buffer.data(), (int) buffer.size());
        if (got < 0)
            return {};
        if (got == 0)
            break;
        md5.update (buffer.data(), (size_t) got);
    }

    return md5.hexDigest();
}

juce::String contentHashForFile (const juce::File& file, juce::String& methodOut)
{
    methodOut.clear();

    const auto payloadHash = tags::audioPayloadHash (file);
    if (payloadHash.isNotEmpty())
    {
        methodOut = "payload";
        return md5Bytes (payloadHash.toRawUTF8(), (size_t) payloadHash.getNumBytesAsUTF8());
    }

    const auto fileHash = md5File (file);
    if (fileHash.isNotEmpty())
        methodOut = "file";
    return fileHash;
}

//==============================================================================
juce::String artworkExtension (const juce::MemoryBlock& data, const juce::String& mimeType)
{
    const auto mime = mimeType.toLowerCase();
    if (mime.contains ("jpeg") || mime.contains ("jpg")) return "jpg";
    if (mime.contains ("png")) return "png";
    if (mime.contains ("gif")) return "gif";
    if (mime.contains ("webp")) return "webp";
    if (mime.contains ("bmp")) return "bmp";

    if (data.getSize() >= 4)
    {
        const auto* b = static_cast<const unsigned char*> (data.getData());
        if (b[0] == 0xff && b[1] == 0xd8) return "jpg";
        if (b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4e && b[3] == 0x47) return "png";
        if (b[0] == 'G' && b[1] == 'I' && b[2] == 'F') return "gif";
        if (b[0] == 'B' && b[1] == 'M') return "bmp";
    }

    return "img";
}

juce::String artworkIdForBytes (const juce::MemoryBlock& data)
{
    if (data.getSize() == 0)
        return {};
    return md5Bytes (data.getData(), data.getSize());
}

juce::File cacheArtwork (const juce::File& artworkDir, const juce::MemoryBlock& data,
                         const juce::String& mimeType, juce::String& artworkIdOut, juce::String& errorOut)
{
    artworkIdOut.clear();
    errorOut.clear();

    if (data.getSize() == 0)
    {
        errorOut = "Artwork data is empty.";
        return {};
    }

    const auto id = artworkIdForBytes (data);
    if (id.isEmpty())
    {
        errorOut = "Could not hash the artwork data.";
        return {};
    }

    if (artworkDir.createDirectory().failed() && ! artworkDir.isDirectory())
    {
        errorOut = "Could not create the artwork cache directory: " + artworkDir.getFullPathName();
        return {};
    }

    const auto extension = artworkExtension (data, mimeType);
    const auto target = artworkDir.getChildFile (id + "." + extension);

    if (target.existsAsFile() && target.getSize() == (juce::int64) data.getSize())
    {
        artworkIdOut = id;
        return target;
    }

    if (! target.replaceWithData (data.getData(), data.getSize()))
    {
        errorOut = "Could not write the artwork cache file: " + target.getFullPathName();
        return {};
    }

    artworkIdOut = id;
    return target;
}

juce::File findCachedArtwork (const juce::File& artworkDir, const juce::String& artworkId)
{
    if (artworkId.isEmpty() || artworkId.length() > 128)
        return {};

    // Ids are hex digests; reject anything that could escape the cache dir.
    for (int i = 0; i < artworkId.length(); ++i)
    {
        const auto c = artworkId[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (! hex)
            return {};
    }

    static const char* extensions[] = { "jpg", "png", "gif", "webp", "bmp", "img" };
    for (auto* ext : extensions)
    {
        auto file = artworkDir.getChildFile (artworkId + "." + ext);
        if (file.existsAsFile() && file.getSize() > 0)
            return file;
    }

    // Unknown extension written by a future version: fall back to a bounded scan.
    auto children = artworkDir.findChildFiles (juce::File::findFiles, false, artworkId + ".*");
    for (auto& f : children)
        if (f.existsAsFile() && f.getSize() > 0)
            return f;

    return {};
}

} // namespace sp::scanner
