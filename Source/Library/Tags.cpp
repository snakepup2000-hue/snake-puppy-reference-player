/*
    Snake Puppy Reference Player
    Tag reading/writing and embedded artwork extraction.

    Implementation notes
    --------------------
    Pure byte-level parsers and writers: no JUCE audio format readers and no
    external tools are used. Every parser is bounds-checked and returns
    `false` with a human-readable error instead of asserting or crashing on
    malformed input.

    Writing semantics
    -----------------
    - Only the tag/metadata region of a file is rebuilt. Audio sample payload
      bytes are copied verbatim, so the decoded audio is bit-identical after a
      tag write (Tests/Test_Tags* proves this with an MD5 of the payload).
    - Empty / zero values in the supplied TagData are ignored ("skip empty"):
      the existing value (if any) is preserved. There is no way to erase a
      single field; the TagData::readOk / formatName fields are ignored on
      write. ReplayGain is only rewritten when the caller supplies has* flags
      (writeReplayGain always does).
    - Writes go to a temporary file in the same directory and are installed
      with an atomic rename; a failure never corrupts the original file.

    Threading
    ---------
    All functions are re-entrant (no shared mutable state) but perform
    blocking file I/O. Call them from a worker/scanner thread, never from the
    audio thread.
*/

#include "Library/Tags.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#if JUCE_MAC || JUCE_LINUX || JUCE_BSD
 #include <sys/stat.h>
 #define SP_HAVE_POSIX_PERMISSIONS 1
#endif

namespace sp::tags
{
namespace
{
//==============================================================================
// Basic types and bounds-checked byte access
//==============================================================================

using Bytes = std::vector<std::uint8_t>;

/** Hard cap for reading a file into memory for tag parsing/writing. Anything
    larger is reported as an error rather than risking the address space. */
constexpr std::size_t kMaxReadableFileBytes = (std::size_t) 1 << 30;   // 1 GiB

inline bool inBounds (std::size_t total, std::size_t offset, std::size_t length) noexcept
{
    return offset <= total && length <= total - offset;
}

inline std::uint16_t readU16LE (const std::uint8_t* p) noexcept
{
    return (std::uint16_t) ((std::uint16_t) p[0] | ((std::uint16_t) p[1] << 8));
}

inline std::uint16_t readU16BE (const std::uint8_t* p) noexcept
{
    return (std::uint16_t) (((std::uint16_t) p[0] << 8) | (std::uint16_t) p[1]);
}

inline std::uint32_t readU24BE (const std::uint8_t* p) noexcept
{
    return ((std::uint32_t) p[0] << 16) | ((std::uint32_t) p[1] << 8) | (std::uint32_t) p[2];
}

inline std::uint32_t readU32LE (const std::uint8_t* p) noexcept
{
    return (std::uint32_t) p[0] | ((std::uint32_t) p[1] << 8) | ((std::uint32_t) p[2] << 16)
           | ((std::uint32_t) p[3] << 24);
}

inline std::uint32_t readU32BE (const std::uint8_t* p) noexcept
{
    return ((std::uint32_t) p[0] << 24) | ((std::uint32_t) p[1] << 16) | ((std::uint32_t) p[2] << 8)
           | (std::uint32_t) p[3];
}

inline std::uint64_t readU64LE (const std::uint8_t* p) noexcept
{
    return (std::uint64_t) readU32LE (p) | ((std::uint64_t) readU32LE (p + 4) << 32);
}

inline std::uint64_t readU64BE (const std::uint8_t* p) noexcept
{
    return ((std::uint64_t) readU32BE (p) << 32) | (std::uint64_t) readU32BE (p + 4);
}

inline void appendU16LE (Bytes& b, std::uint16_t v)
{
    b.push_back ((std::uint8_t) (v & 0xff));
    b.push_back ((std::uint8_t) ((v >> 8) & 0xff));
}

inline void appendU32LE (Bytes& b, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xff));
}

inline void appendU64LE (Bytes& b, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        b.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xff));
}

inline void appendU32BE (Bytes& b, std::uint32_t v)
{
    for (int i = 3; i >= 0; --i)
        b.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xff));
}

inline void appendU64BE (Bytes& b, std::uint64_t v)
{
    for (int i = 7; i >= 0; --i)
        b.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xff));
}

inline void appendBytes (Bytes& dst, const std::uint8_t* data, std::size_t size)
{
    if (data != nullptr && size > 0)
        dst.insert (dst.end(), data, data + size);
}

inline void appendBytes (Bytes& dst, const Bytes& src)
{
    dst.insert (dst.end(), src.begin(), src.end());
}

inline void appendAscii (Bytes& b, const char* s)
{
    while (*s != 0)
        b.push_back ((std::uint8_t) *s++);
}

/** Sequential reader with a sticky failure flag. */
struct Cursor
{
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
    bool ok = true;

    Cursor() = default;
    Cursor (const std::uint8_t* d, std::size_t n) : data (d), size (n) {}
    explicit Cursor (const Bytes& b) : data (b.data()), size (b.size()) {}

    std::size_t remaining() const noexcept { return ok ? size - pos : 0; }
    bool canRead (std::size_t n) const noexcept { return ok && pos <= size && n <= size - pos; }

    std::uint8_t u8()
    {
        if (! canRead (1)) { ok = false; return 0; }
        return data[pos++];
    }

    std::uint16_t u16le()
    {
        if (! canRead (2)) { ok = false; return 0; }
        const auto v = readU16LE (data + pos);
        pos += 2;
        return v;
    }

    std::uint32_t u24be()
    {
        if (! canRead (3)) { ok = false; return 0; }
        const auto v = readU24BE (data + pos);
        pos += 3;
        return v;
    }

    std::uint32_t u32le()
    {
        if (! canRead (4)) { ok = false; return 0; }
        const auto v = readU32LE (data + pos);
        pos += 4;
        return v;
    }

    std::uint32_t u32be()
    {
        if (! canRead (4)) { ok = false; return 0; }
        const auto v = readU32BE (data + pos);
        pos += 4;
        return v;
    }

    std::uint64_t u64le()
    {
        if (! canRead (8)) { ok = false; return 0; }
        const auto v = readU64LE (data + pos);
        pos += 8;
        return v;
    }

    std::uint64_t u64be()
    {
        if (! canRead (8)) { ok = false; return 0; }
        const auto v = readU64BE (data + pos);
        pos += 8;
        return v;
    }

    Bytes bytes (std::size_t n)
    {
        if (! canRead (n)) { ok = false; return {}; }
        Bytes r (data + pos, data + pos + n);
        pos += n;
        return r;
    }

    Cursor sub (std::size_t n)
    {
        if (! canRead (n)) { ok = false; return {}; }
        Cursor c (data + pos, n);
        pos += n;
        return c;
    }

    void skip (std::size_t n)
    {
        if (! canRead (n)) { ok = false; return; }
        pos += n;
    }

    const std::uint8_t* ptr() const noexcept { return data + pos; }
};

inline bool isAsciiAlnumUpper (std::uint8_t c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

inline bool fourCCEquals (const std::uint8_t* p, const char* fourCC) noexcept
{
    return p[0] == (std::uint8_t) fourCC[0] && p[1] == (std::uint8_t) fourCC[1]
           && p[2] == (std::uint8_t) fourCC[2] && p[3] == (std::uint8_t) fourCC[3];
}

//==============================================================================
// File I/O
//==============================================================================

bool readFileBytes (const juce::File& file, Bytes& out, juce::String& errorOut)
{
    if (! file.existsAsFile())
    {
        errorOut = "File not found: " + file.getFullPathName();
        return false;
    }

    const juce::int64 size64 = file.getSize();

    if (size64 <= 0)
    {
        errorOut = "File is empty: " + file.getFullPathName();
        return false;
    }

    if ((std::uint64_t) size64 > (std::uint64_t) kMaxReadableFileBytes)
    {
        errorOut = "File is too large to read into memory for tagging (" + juce::String (size64) + " bytes)";
        return false;
    }

    juce::FileInputStream in (file);

    if (! in.openedOk())
    {
        errorOut = "Could not open file: " + file.getFullPathName();
        return false;
    }

    out.resize ((std::size_t) size64);
    std::size_t done = 0;

    while (done < out.size())
    {
        const int want = (int) std::min<std::size_t> (out.size() - done, (std::size_t) 1 << 20);

        if (want <= 0)
            break;

        const int got = in.read (out.data() + done, want);

        if (got <= 0)
        {
            errorOut = "Short read while reading " + file.getFullPathName();
            out.clear();
            return false;
        }

        done += (std::size_t) got;
    }

    return true;
}

/** Writes `data` to a sibling temporary file, then atomically renames it over
    `target`. On any failure the original file is untouched. */
bool writeFileAtomically (const juce::File& target, const Bytes& data, juce::String& errorOut)
{
    auto parent = target.getParentDirectory();

    if (! parent.isDirectory() && ! parent.createDirectory().wasOk())
    {
        errorOut = "Could not create directory for " + target.getFullPathName();
        return false;
    }

    const bool existed = target.existsAsFile();

    // Preserve the original POSIX mode bits across the atomic rename.
    unsigned long oldMode = 0;
    bool haveOldMode = false;

   #ifdef SP_HAVE_POSIX_PERMISSIONS
    if (existed)
    {
        struct stat info {};

        if (::stat (target.getFullPathName().toRawUTF8(), &info) == 0)
        {
            oldMode = (unsigned long) (info.st_mode & 07777);
            haveOldMode = true;
        }
    }
   #endif

    juce::File temp;

    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const auto candidate = parent.getChildFile (target.getFileName()
                                                   + ".sptmp" + juce::String (attempt));

        if (! candidate.existsAsFile())
        {
            temp = candidate;
            break;
        }
    }

    if (temp == juce::File())
    {
        errorOut = "Could not find a free temporary file name next to " + target.getFullPathName();
        return false;
    }

    {
        juce::FileOutputStream out (temp);

        if (! out.openedOk())
        {
            errorOut = "Could not create temporary file " + temp.getFullPathName();
            return false;
        }

        std::size_t done = 0;

        while (done < data.size())
        {
            const int want = (int) std::min<std::size_t> (data.size() - done, (std::size_t) 1 << 20);

            if (want <= 0 || ! out.write (data.data() + done, (std::size_t) want))
            {
                errorOut = "Failed writing temporary file " + temp.getFullPathName();
                out.flush();
                temp.deleteFile();
                return false;
            }

            done += (std::size_t) want;
        }

        out.flush();
    }

    const juce::String tempPath = temp.getFullPathName();
    const juce::String targetPath = target.getFullPathName();

   #ifdef SP_HAVE_POSIX_PERMISSIONS
    if (haveOldMode)
        ::chmod (tempPath.toRawUTF8(), (mode_t) oldMode);
   #else
    (void) haveOldMode;
    (void) oldMode;
   #endif

    if (std::rename (tempPath.toRawUTF8(), targetPath.toRawUTF8()) != 0)
    {
        errorOut = "Could not atomically replace " + targetPath + " (rename failed)";
        temp.deleteFile();
        return false;
    }

    return true;
}

//==============================================================================
// MD5 (self-contained; juce_cryptography is not linked into this target)
//==============================================================================

class Md5
{
public:
    Md5() { reset(); }

    void reset()
    {
        state[0] = 0x67452301u;
        state[1] = 0xefcdab89u;
        state[2] = 0x98badcfeu;
        state[3] = 0x10325476u;
        bitCount = 0;
        bufferLen = 0;
    }

    void update (const void* data, std::size_t len)
    {
        auto* p = static_cast<const std::uint8_t*> (data);
        bitCount += (std::uint64_t) len * 8u;

        while (len > 0)
        {
            const std::size_t take = std::min<std::size_t> (len, 64 - bufferLen);
            std::memcpy (buffer + bufferLen, p, take);
            bufferLen += take;
            p += take;
            len -= take;

            if (bufferLen == 64)
            {
                transform (buffer);
                bufferLen = 0;
            }
        }
    }

    std::array<std::uint8_t, 16> finish()
    {
        const std::uint64_t bits = bitCount;
        std::uint8_t pad[64] = {};

        pad[0] = 0x80;
        const std::size_t zeros = (std::size_t) ((56 - (int) ((bufferLen + 1) % 64) + 64) % 64);
        update (pad, 1 + zeros);

        std::uint8_t lengthBytes[8];

        for (int i = 0; i < 8; ++i)
            lengthBytes[i] = (std::uint8_t) ((bits >> (8 * i)) & 0xff);

        update (lengthBytes, 8);

        std::array<std::uint8_t, 16> digest {};

        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                digest[(std::size_t) (i * 4 + j)] = (std::uint8_t) ((state[i] >> (8 * j)) & 0xff);

        return digest;
    }

private:
    static std::uint32_t rotl (std::uint32_t v, int n) noexcept
    {
        return (v << n) | (v >> (32 - n));
    }

    void transform (const std::uint8_t* block)
    {
        static const std::uint32_t k[64] = {
            0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
            0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
            0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
            0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
            0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
            0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
            0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
            0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
        };

        static const int s[64] = {
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
            5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
        };

        std::uint32_t m[16];

        for (int i = 0; i < 16; ++i)
            m[i] = readU32LE (block + i * 4);

        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];

        for (int i = 0; i < 64; ++i)
        {
            std::uint32_t f = 0;
            int g = 0;

            if (i < 16)
            {
                f = (b & c) | ((~ b) & d);
                g = i;
            }
            else if (i < 32)
            {
                f = (d & b) | ((~ d) & c);
                g = (5 * i + 1) & 15;
            }
            else if (i < 48)
            {
                f = b ^ c ^ d;
                g = (3 * i + 5) & 15;
            }
            else
            {
                f = c ^ (b | (~ d));
                g = (7 * i) & 15;
            }

            const std::uint32_t tmp = d;
            d = c;
            c = b;
            b = b + rotl (a + f + k[(std::size_t) i] + m[(std::size_t) g], s[(std::size_t) i]);
            a = tmp;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
    }

    std::uint32_t state[4] {};
    std::uint64_t bitCount = 0;
    std::uint8_t buffer[64] {};
    std::size_t bufferLen = 0;
};

juce::String md5Hex (const void* data, std::size_t size)
{
    Md5 md5;
    md5.update (data, size);
    const auto digest = md5.finish();
    return juce::String::toHexString (digest.data(), (int) digest.size()).toLowerCase().removeCharacters (" ");
}

//==============================================================================
// UTF-8 / UTF-16 / Latin-1 helpers (ID3 text encodings)
//==============================================================================

void appendUtf8CodePoint (std::string& out, std::uint32_t cp)
{
    if (cp <= 0x7f)
    {
        out.push_back ((char) cp);
    }
    else if (cp <= 0x7ff)
    {
        out.push_back ((char) (0xc0 | (cp >> 6)));
        out.push_back ((char) (0x80 | (cp & 0x3f)));
    }
    else if (cp <= 0xffff)
    {
        out.push_back ((char) (0xe0 | (cp >> 12)));
        out.push_back ((char) (0x80 | ((cp >> 6) & 0x3f)));
        out.push_back ((char) (0x80 | (cp & 0x3f)));
    }
    else
    {
        out.push_back ((char) (0xf0 | (cp >> 18)));
        out.push_back ((char) (0x80 | ((cp >> 12) & 0x3f)));
        out.push_back ((char) (0x80 | ((cp >> 6) & 0x3f)));
        out.push_back ((char) (0x80 | (cp & 0x3f)));
    }
}

std::vector<std::uint32_t> decodeUtf8CodePoints (const std::uint8_t* p, std::size_t n)
{
    std::vector<std::uint32_t> out;
    out.reserve (n);
    std::size_t i = 0;

    while (i < n)
    {
        const std::uint8_t c = p[i];

        if (c < 0x80)
        {
            out.push_back (c);
            ++i;
        }
        else if ((c & 0xe0) == 0xc0 && i + 1 < n && (p[i + 1] & 0xc0) == 0x80)
        {
            out.push_back (((std::uint32_t) (c & 0x1f) << 6) | (std::uint32_t) (p[i + 1] & 0x3f));
            i += 2;
        }
        else if ((c & 0xf0) == 0xe0 && i + 2 < n && (p[i + 1] & 0xc0) == 0x80 && (p[i + 2] & 0xc0) == 0x80)
        {
            out.push_back (((std::uint32_t) (c & 0x0f) << 12) | ((std::uint32_t) (p[i + 1] & 0x3f) << 6)
                           | (std::uint32_t) (p[i + 2] & 0x3f));
            i += 3;
        }
        else if ((c & 0xf8) == 0xf0 && i + 3 < n && (p[i + 1] & 0xc0) == 0x80 && (p[i + 2] & 0xc0) == 0x80
                 && (p[i + 3] & 0xc0) == 0x80)
        {
            out.push_back (((std::uint32_t) (c & 0x07) << 18) | ((std::uint32_t) (p[i + 1] & 0x3f) << 12)
                           | ((std::uint32_t) (p[i + 2] & 0x3f) << 6) | (std::uint32_t) (p[i + 3] & 0x3f));
            i += 4;
        }
        else
        {
            out.push_back (0xfffd);   // replacement character
            ++i;
        }
    }

    return out;
}

std::string encodeUtf8 (const std::vector<std::uint32_t>& codePoints)
{
    std::string out;
    out.reserve (codePoints.size());

    for (const auto cp : codePoints)
    {
        if (cp == 0)
            break;                    // C string semantics for JUCE strings
        appendUtf8CodePoint (out, cp);
    }

    return out;
}

juce::String stringFromUtf8 (const std::uint8_t* p, std::size_t n)
{
    const auto codePoints = decodeUtf8CodePoints (p, n);
    const auto utf8 = encodeUtf8 (codePoints);
    return juce::String::fromUTF8 (utf8.data(), (int) utf8.size());
}

juce::String stringFromLatin1 (const std::uint8_t* p, std::size_t n)
{
    std::vector<std::uint32_t> codePoints;
    codePoints.reserve (n);

    for (std::size_t i = 0; i < n; ++i)
        codePoints.push_back (p[i]);

    const auto utf8 = encodeUtf8 (codePoints);
    return juce::String::fromUTF8 (utf8.data(), (int) utf8.size());
}

enum class Utf16Endianness { little, big };

std::vector<std::uint32_t> decodeUtf16 (const std::uint8_t* p, std::size_t n, Utf16Endianness endian)
{
    std::vector<std::uint32_t> out;
    out.reserve (n / 2);

    auto unitAt = [&] (std::size_t i) -> std::uint32_t
    {
        return endian == Utf16Endianness::little ? (std::uint32_t) readU16LE (p + i)
                                                 : (std::uint32_t) ((std::uint32_t) p[i] << 8 | (std::uint32_t) p[i + 1]);
    };

    std::size_t i = 0;

    while (i + 1 < n)
    {
        const std::uint32_t unit = unitAt (i);
        i += 2;

        if (unit >= 0xd800 && unit <= 0xdbff && i + 1 < n)
        {
            const std::uint32_t low = unitAt (i);

            if (low >= 0xdc00 && low <= 0xdfff)
            {
                i += 2;
                out.push_back (0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00));
                continue;
            }
        }

        out.push_back (unit);
    }

    return out;
}

std::vector<std::uint32_t> decodeId3UnitsImpl (const std::uint8_t* p, std::size_t n, int hint)
{
    if (n == 0)
        return {};

    int encoding = hint;

    if (hint < 0)
    {
        encoding = (int) p[0];
        ++p;
        --n;
    }

    if (encoding < 0 || encoding > 3)
        encoding = 0;

    switch (encoding)
    {
        case 0:
            return [&]
            {
                std::vector<std::uint32_t> v;
                v.reserve (n);
                for (std::size_t i = 0; i < n; ++i)
                    v.push_back (p[i]);
                return v;
            }();

        case 1:
        {
            Utf16Endianness endian = Utf16Endianness::little;

            if (n >= 2 && p[0] == 0xff && p[1] == 0xfe)
            {
                p += 2;
                n -= 2;
            }
            else if (n >= 2 && p[0] == 0xfe && p[1] == 0xff)
            {
                p += 2;
                n -= 2;
                endian = Utf16Endianness::big;
            }

            return decodeUtf16 (p, n, endian);
        }

        case 2:
            return decodeUtf16 (p, n, Utf16Endianness::big);

        case 3:
        default:
            return decodeUtf8CodePoints (p, n);
    }
}

/** Decodes an ID3 text payload. `hintEncoding` < 0 means the first byte of the
    buffer is the encoding selector (the normal case for ID3 frames). */
std::vector<std::uint32_t> decodeId3Text (const std::uint8_t* p, std::size_t n, int hintEncoding = -1)
{
    return decodeId3UnitsImpl (p, n, hintEncoding);
}

/** Converts decoded code units into a JUCE string, replacing embedded NULs
    (which separate multiple values in v2.4 frames) with "; ". */
juce::String codeUnitsToString (const std::vector<std::uint32_t>& units)
{
    std::string utf8;
    utf8.reserve (units.size());
    bool pendingSeparator = false;

    for (const auto cp : units)
    {
        if (cp == 0)
        {
            pendingSeparator = ! utf8.empty();
            continue;
        }

        if (pendingSeparator)
        {
            utf8 += "; ";
            pendingSeparator = false;
        }

        appendUtf8CodePoint (utf8, cp);
    }

    return juce::String::fromUTF8 (utf8.data(), (int) utf8.size());
}

juce::String trimTrailingNuls (juce::String s)
{
    while (s.isNotEmpty() && s.getLastCharacter() == 0)
        s = s.dropLastCharacters (1);

    return s;
}

void appendUtf16LE (std::vector<std::uint8_t>& out, const std::vector<std::uint32_t>& codePoints)
{
    for (const auto cp : codePoints)
    {
        if (cp <= 0xffff)
        {
            appendU16LE (out, (std::uint16_t) cp);
        }
        else
        {
            const std::uint32_t v = cp - 0x10000;
            appendU16LE (out, (std::uint16_t) (0xd800 + (v >> 10)));
            appendU16LE (out, (std::uint16_t) (0xdc00 + (v & 0x3ff)));
        }
    }
}

juce::String sanitiseText (const juce::String& s)
{
    return trimTrailingNuls (s).trim();
}

int parseLeadingInt (const juce::String& s)
{
    const auto trimmed = s.trimStart();
    juce::String digits;

    for (int i = 0; i < trimmed.length(); ++i)
    {
        const auto c = trimmed[i];

        if (c >= '0' && c <= '9')
            digits += c;
        else
            break;
    }

    return digits.isEmpty() ? 0 : digits.getIntValue();
}

float parseLeadingFloat (const juce::String& s)
{
    juce::String cleaned = s.trim();

    if (cleaned.isEmpty())
        return 0.0f;

    return cleaned.getFloatValue();
}

//==============================================================================
// Image sniffing (MIME + dimensions, no decoding)
//==============================================================================

juce::String sniffImageMime (const std::uint8_t* p, std::size_t n)
{
    if (n >= 3 && p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff)
        return "image/jpeg";

    if (n >= 8 && p[0] == 0x89 && p[1] == 0x50 && p[2] == 0x4e && p[3] == 0x47)
        return "image/png";

    if (n >= 12 && fourCCEquals (p, "RIFF") && p[8] == 'W' && p[9] == 'E' && p[10] == 'B' && p[11] == 'P')
        return "image/webp";

    if (n >= 2 && p[0] == 'B' && p[1] == 'M')
        return "image/bmp";

    if (n >= 6 && p[0] == 'G' && p[1] == 'I' && p[2] == 'F')
        return "image/gif";

    return {};
}

bool imageDimensions (const std::uint8_t* p, std::size_t n, int& width, int& height)
{
    width = height = 0;

    if (n >= 24 && p[0] == 0x89 && p[1] == 0x50 && p[2] == 0x4e && p[3] == 0x47
        && p[12] == 'I' && p[13] == 'H' && p[14] == 'D' && p[15] == 'R')
    {
        width = (int) std::min<std::uint32_t> (readU32BE (p + 16), 0x7fffffff);
        height = (int) std::min<std::uint32_t> (readU32BE (p + 20), 0x7fffffff);
        return width > 0 && height > 0;
    }

    if (n >= 4 && p[0] == 0xff && p[1] == 0xd8)
    {
        std::size_t pos = 2;

        while (pos + 4 <= n)
        {
            if (p[pos] != 0xff)
            {
                ++pos;
                continue;
            }

            const std::uint8_t marker = p[pos + 1];

            if (marker == 0xff)
            {
                ++pos;
                continue;
            }

            if (marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
            {
                pos += 2;
                continue;
            }

            if (marker == 0xd9 || marker == 0xda)
                break;

            if (pos + 4 > n)
                break;

            const std::uint16_t segmentLength = (std::uint16_t) ((p[pos + 2] << 8) | p[pos + 3]);

            if (segmentLength < 2 || pos + 2 + segmentLength > n)
                break;

            const bool isFrameHeader = marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8
                                       && marker != 0xcc;

            if (isFrameHeader && pos + 9 <= n)
            {
                height = (int) ((p[pos + 5] << 8) | p[pos + 6]);
                width = (int) ((p[pos + 7] << 8) | p[pos + 8]);
                return width > 0 && height > 0;
            }

            pos += 2 + (std::size_t) segmentLength;
        }
    }

    return false;
}

//==============================================================================
// Base64 helpers (Vorbis METADATA_BLOCK_PICTURE)
//==============================================================================

bool base64DecodeBytes (const juce::String& text, Bytes& out)
{
    juce::String cleaned;

    for (auto c : text)
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t')
            cleaned += juce::String::charToString (c);

    if (cleaned.isEmpty())
    {
        out.clear();
        return true;
    }

    juce::MemoryOutputStream mos;

    if (! juce::Base64::convertFromBase64 (mos, cleaned))
        return false;

    out.resize (mos.getDataSize());

    if (! out.empty())
        std::memcpy (out.data(), mos.getData(), out.size());

    return true;
}

juce::String base64EncodeBytes (const std::uint8_t* data, std::size_t size)
{
    juce::MemoryOutputStream mos;
    juce::Base64::convertToBase64 (mos, data, size);
    return juce::String::fromUTF8 (static_cast<const char*> (mos.getData()), (int) mos.getDataSize());
}

//==============================================================================
// Ogg CRC32 (poly 0x04c11db7, MSB-first, no final xor - per the Ogg spec)
//==============================================================================

std::uint32_t oggCrc32 (const std::uint8_t* data, std::size_t size)
{
    static std::uint32_t table[256];
    static bool tableReady = false;

    if (! tableReady)
    {
        for (std::uint32_t i = 0; i < 256; ++i)
        {
            std::uint32_t r = i << 24;

            for (int j = 0; j < 8; ++j)
                r = (r & 0x80000000u) != 0 ? (r << 1) ^ 0x04c11db7u : (r << 1);

            table[i] = r;
        }

        tableReady = true;
    }

    std::uint32_t crc = 0;

    for (std::size_t i = 0; i < size; ++i)
        crc = (crc << 8) ^ table[((crc >> 24) ^ data[i]) & 0xff];

    return crc;
}

//==============================================================================
// Generic fourCC + container helpers
//==============================================================================

//==============================================================================
// Format detection
//==============================================================================

const char* formatForExtension (const juce::String& ext)
{
    if (ext == ".flac") return "FLAC";
    if (ext == ".mp3" || ext == ".mp2") return "MP3";
    if (ext == ".m4a" || ext == ".m4b" || ext == ".mp4") return "M4A";
    if (ext == ".aac" || ext == ".adts") return "AAC";
    if (ext == ".ogg" || ext == ".oga" || ext == ".opus") return "OGG";
    if (ext == ".wav" || ext == ".wave" || ext == ".bwf") return "WAV";
    if (ext == ".aif" || ext == ".aiff" || ext == ".aifc") return "AIFF";
    if (ext == ".dsf") return "DSF";
    if (ext == ".dff") return "DFF";
    if (ext == ".caf") return "CAF";
    return nullptr;
}

/** Reads up to 12 bytes from the head of the file without loading the file. */
std::size_t readFileHead (const juce::File& file, std::uint8_t* buffer, std::size_t maxBytes)
{
    juce::FileInputStream in (file);

    if (! in.openedOk())
        return 0;

    const int got = in.read (buffer, (int) maxBytes);
    return got > 0 ? (std::size_t) got : 0;
}

//==============================================================================
// Small value formatting helpers
//==============================================================================

juce::String formatGainDb (float db)
{
    return juce::String (db, 2) + " dB";
}

juce::String formatPeak (float peak)
{
    return juce::String (peak, 6);
}

juce::String formatTrackNumber (int number)
{
    return number > 0 ? juce::String (number) : juce::String();
}

void setIfNotEmpty (juce::String& target, const juce::String& value)
{
    if (value.isNotEmpty())
        target = value;
}

//==============================================================================
// TagData merge: non-empty / non-zero values from `src` override `dst`.
//==============================================================================

void mergeNonEmpty (TagData& dst, const TagData& src)
{
    setIfNotEmpty (dst.title, src.title);
    setIfNotEmpty (dst.artist, src.artist);
    setIfNotEmpty (dst.albumArtist, src.albumArtist);
    setIfNotEmpty (dst.album, src.album);
    setIfNotEmpty (dst.genre, src.genre);
    setIfNotEmpty (dst.composer, src.composer);
    setIfNotEmpty (dst.comment, src.comment);

    if (src.year > 0) dst.year = src.year;
    if (src.trackNumber > 0) dst.trackNumber = src.trackNumber;
    if (src.discNumber > 0) dst.discNumber = src.discNumber;
    if (src.rating > 0) dst.rating = src.rating;

    if (src.hasReplayGainTrack)
    {
        dst.replayGainTrackDb = src.replayGainTrackDb;
        dst.hasReplayGainTrack = true;
    }

    if (src.hasReplayGainAlbum)
    {
        dst.replayGainAlbumDb = src.replayGainAlbumDb;
        dst.hasReplayGainAlbum = true;
    }

    if (src.hasPeak)
    {
        dst.peakLinear = src.peakLinear;
        dst.hasPeak = true;
    }
}

bool hasAnyReplayGain (const TagData& tags)
{
    return tags.hasReplayGainTrack || tags.hasReplayGainAlbum || tags.hasPeak;
}

/** A container-agnostic description of a metadata update. */
struct TagUpdate
{
    bool updateFields = false;
    const TagData* fields = nullptr;
    bool updateReplayGain = false;
    bool updateArtwork = false;
    const EmbeddedArtwork* artwork = nullptr;
};

//==============================================================================
// ID3v2 (2.2 / 2.3 / 2.4)
//==============================================================================

const char* const kId3Genres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal",
    "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial",
    "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop",
    "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House", "Game",
    "Sound Clip", "Gospel", "Noise", "AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative",
    "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave", "Techno-Industrial",
    "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta",
    "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
    "Psychadelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka",
    "Retro", "Musical", "Rock & Roll", "Hard Rock", "Folk", "Folk-Rock", "National Folk", "Swing",
    "Fast Fusion", "Bebob", "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde", "Gothic Rock",
    "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band", "Chorus",
    "Easy Listening", "Acoustic", "Humour", "Speech", "Chanson", "Opera", "Chamber Music", "Sonata",
    "Symphony", "Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba",
    "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo",
    "A capella", "Euro-House", "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore", "Terror",
    "Indie", "BritPop", "Negerpunk", "Polsk Punk", "Beat", "Christian Gangsta Rap", "Heavy Metal",
    "Black Metal", "Crossover", "Contemporary Christian", "Christian Rock", "Merengue", "Salsa",
    "Thrash Metal", "Anime", "JPop", "Synthpop", "Abstract", "Art Rock", "Baroque", "Bhangra",
    "Big Beat", "Breakbeat", "Chillout", "Downtempo", "Dub", "EBM", "Eclectic", "Electro", "Electroclash",
    "Emo", "Experimental", "Garage", "Global", "IDM", "Illbient", "Industro-Goth", "Jam Band", "Krautrock",
    "Leftfield", "Lounge", "Math Rock", "New Romantic", "Nu-Breakz", "Post-Punk", "Post-Rock", "Psytrance",
    "Shoegaze", "Space Rock", "Trop Rock", "World Music", "Neoclassical", "Audiobook", "Audio Theatre",
    "Neue Deutsche Welle", "Podcast", "Indie Rock", "G-Funk", "Dubstep", "Garage Rock", "Psybient",
};

juce::String id3GenreName (int index)
{
    const auto count = (int) (sizeof (kId3Genres) / sizeof (kId3Genres[0]));

    if (index >= 0 && index < count)
        return kId3Genres[(std::size_t) index];

    return {};
}

/** Maps a 3-character ID3v2.2 frame id to its 4-character v2.3/2.4 id. */
juce::String frameIdV22ToV24 (const juce::String& id)
{
    struct Map { const char* from; const char* to; };
    static const Map maps[] = {
        { "TT1", "TIT1" }, { "TT2", "TIT2" }, { "TT3", "TIT3" }, { "TP1", "TPE1" }, { "TP2", "TPE2" },
        { "TP3", "TPE3" }, { "TP4", "TPE4" }, { "TAL", "TALB" }, { "TCO", "TCON" }, { "TCM", "TCOM" },
        { "TYE", "TYER" }, { "TRK", "TRCK" }, { "TPA", "TPOS" }, { "COM", "COMM" }, { "TXX", "TXXX" },
        { "PIC", "APIC" }, { "WXX", "WXXX" }, { "TBP", "TBPM" }, { "TEN", "TENC" }, { "TSS", "TSSE" },
        { "TKE", "TKEY" }, { "TLA", "TLAN" }, { "TLE", "TLEN" }, { "TMT", "TMED" }, { "TOA", "TOPE" },
        { "TOF", "TOFN" }, { "TOL", "TOLY" }, { "TOR", "TORY" }, { "TOT", "TOAL" }, { "TXT", "TEXT" },
        { "TCR", "TCOP" }, { "TDA", "TDAT" }, { "TIM", "TIME" }, { "TRD", "TRDA" }, { "TSI", "TSIZ" },
        { "TCT", "TCON" }, { "ULT", "USLT" }, { "SLT", "SYLT" }, { "WAF", "WOAF" }, { "WAR", "WOAR" },
        { "WAS", "WOAS" }, { "WCM", "WCOM" }, { "WCP", "WCOP" }, { "WPB", "WPUB" }, { "MCI", "MCDI" },
        { "ETC", "ETCO" }, { "MLL", "MLLT" }, { "STC", "SYTC" }, { "RVA", "RVAD" }, { "EQU", "EQUA" },
        { "IPL", "IPLS" }, { "LNK", "LINK" }, { "GEO", "GEOB" }, { "CNT", "PCNT" }, { "POP", "POPM" },
        { "BUF", "RBUF" }, { "CRA", "AENC" }, { "UFI", "UFID" }, { "TS2", "TSO2" }, { "TSA", "TSOA" },
        { "TSP", "TSOP" }, { "TST", "TSOT" }, { "TCP", "TCMP" }, { "TDY", "TDLY" }, { "TFL", "TFLT" },
        { "TMT", "TMED" }, { "TSZ", "TSIZ" }, { "TST", "TSOT" },
    };

    const auto upper = id.toUpperCase();

    for (const auto& m : maps)
        if (upper == m.from)
            return m.to;

    return {};
}

struct Id3Frame
{
    juce::String id;
    std::uint16_t formatFlags = 0;
    Bytes payload;
};

struct Id3TagInfo
{
    bool present = false;
    int major = 0;
    int minor = 0;
    std::uint8_t headerFlags = 0;
    std::size_t offset = 0;
    std::size_t totalBytes = 0;
    std::vector<Id3Frame> frames;
};

bool readSyncSafe32 (const std::uint8_t* p, std::uint32_t& valueOut) noexcept
{
    if ((p[0] | p[1] | p[2] | p[3]) & 0x80)
        return false;

    valueOut = ((std::uint32_t) p[0] << 21) | ((std::uint32_t) p[1] << 14) | ((std::uint32_t) p[2] << 7)
               | (std::uint32_t) p[3];
    return true;
}

void appendSyncSafe32 (Bytes& b, std::uint32_t v)
{
    b.push_back ((std::uint8_t) ((v >> 21) & 0x7f));
    b.push_back ((std::uint8_t) ((v >> 14) & 0x7f));
    b.push_back ((std::uint8_t) ((v >> 7) & 0x7f));
    b.push_back ((std::uint8_t) (v & 0x7f));
}

Bytes deUnsynchronise (const std::uint8_t* p, std::size_t n)
{
    Bytes out;
    out.reserve (n);

    for (std::size_t i = 0; i < n; ++i)
    {
        out.push_back (p[i]);

        if (p[i] == 0xff && i + 1 < n && p[i + 1] == 0x00)
            ++i;
    }

    return out;
}

bool zlibInflate (const Bytes& compressed, Bytes& out, juce::String& errorOut)
{
    if (compressed.empty())
    {
        errorOut = "empty compressed frame";
        return false;
    }

    juce::MemoryInputStream source (compressed.data(), compressed.size(), false);
    juce::GZIPDecompressorInputStream gz (&source, false, juce::GZIPDecompressorInputStream::zlibFormat);
    std::uint8_t buffer[8192];
    out.clear();

    for (;;)
    {
        const int got = gz.read (buffer, (int) sizeof (buffer));

        if (got <= 0)
            break;

        if (out.size() + (std::size_t) got > (std::size_t) 64 * 1024 * 1024)
        {
            errorOut = "decompressed ID3 frame is unreasonably large";
            return false;
        }

        out.insert (out.end(), buffer, buffer + got);
    }

    if (out.empty())
    {
        errorOut = "zlib inflate produced no data";
        return false;
    }

    return true;
}

/** Converts a v2.2 PIC payload into the v2.3/2.4 APIC layout. */
Bytes convertV22PicturePayload (const Bytes& pic)
{
    if (pic.empty())
        return {};

    Cursor c (pic);
    const auto encoding = c.u8();

    if (! c.canRead (3))
        return {};

    const auto* fmt = c.ptr();
    juce::String mime = "image/";

    if (fmt[0] == 'J' || fmt[0] == 'j')
        mime += "jpeg";
    else if (fmt[0] == 'P' || fmt[0] == 'p')
        mime += "png";
    else
        mime = "application/octet-stream";

    c.skip (3);
    const auto pictureType = c.u8();
    const auto rest = c.bytes (c.remaining());

    Bytes out;
    out.push_back (encoding);
    appendAscii (out, mime.toRawUTF8());
    out.push_back (0);
    out.push_back (pictureType);
    appendBytes (out, rest);
    return out;
}

bool parseId3Tag (const std::uint8_t* data, std::size_t fileSize, std::size_t offset, Id3TagInfo& out,
                  juce::String& errorOut, bool& foundOut)
{
    out = Id3TagInfo {};
    foundOut = false;

    if (! inBounds (fileSize, offset, 10) || data[offset] != 'I' || data[offset + 1] != 'D'
        || data[offset + 2] != '3')
        return true;

    foundOut = true;
    out.present = true;
    out.offset = offset;

    const int major = (int) data[offset + 3];
    const int minor = (int) data[offset + 4];

    if (major < 2 || major > 4)
    {
        errorOut = "Unsupported ID3v2 version 2." + juce::String (major) + "." + juce::String (minor);
        return false;
    }

    out.major = major;
    out.minor = minor;
    out.headerFlags = data[offset + 5];

    if ((out.headerFlags & 0x1f) != 0 && major < 4)
    {
        errorOut = "Malformed ID3v2 header flags";
        return false;
    }

    std::uint32_t tagSize = 0;

    if (! readSyncSafe32 (data + offset + 6, tagSize))
    {
        errorOut = "Malformed ID3v2 tag size";
        return false;
    }

    const bool hasFooter = major == 4 && (out.headerFlags & 0x10) != 0;
    const std::size_t totalBytes = 10 + (std::size_t) tagSize + (hasFooter ? 10 : 0);

    if (! inBounds (fileSize, offset, totalBytes))
    {
        errorOut = "Truncated ID3v2 tag (declared " + juce::String ((int) totalBytes) + " bytes, file has "
                   + juce::String ((int) (fileSize - offset)) + ")";
        return false;
    }

    if (hasFooter)
    {
        const auto* footer = data + offset + 10 + tagSize;

        if (! (footer[0] == '3' && footer[1] == 'D' && footer[2] == 'I'
               && footer[3] == (std::uint8_t) major && footer[4] == (std::uint8_t) minor))
        {
            errorOut = "Malformed ID3v2.4 footer";
            return false;
        }
    }

    out.totalBytes = totalBytes;

    const std::uint8_t* body = data + offset + 10;
    std::size_t bodySize = (std::size_t) tagSize;
    Bytes deUnsyncedBody;

    if (major <= 3 && (out.headerFlags & 0x80) != 0)
    {
        deUnsyncedBody = deUnsynchronise (body, bodySize);
        body = deUnsyncedBody.data();
        bodySize = deUnsyncedBody.size();
    }

    Cursor cursor (body, bodySize);

    if (major == 2 && (out.headerFlags & 0x40) != 0)
    {
        errorOut = "Compressed ID3v2.2 tags are not supported";
        return false;
    }

    if ((out.headerFlags & 0x40) != 0)
    {
        if (major == 3)
        {
            if (! cursor.canRead (4))
            {
                errorOut = "Truncated ID3v2.3 extended header";
                return false;
            }

            const auto extSize = cursor.u32be();
            cursor.skip ((std::size_t) extSize);
        }
        else if (major == 4)
        {
            if (! cursor.canRead (4))
            {
                errorOut = "Truncated ID3v2.4 extended header";
                return false;
            }

            std::uint32_t extSize = 0;

            if (! readSyncSafe32 (cursor.ptr(), extSize))
            {
                errorOut = "Malformed ID3v2.4 extended header size";
                return false;
            }

            cursor.skip ((std::size_t) extSize);
        }

        if (! cursor.ok)
        {
            errorOut = "Truncated ID3v2 extended header";
            return false;
        }
    }

    while (cursor.remaining() > 0)
    {
        if (cursor.data[cursor.pos] == 0)
            break;   // start of padding

        if (major == 2)
        {
            if (! cursor.canRead (6))
            {
                errorOut = "Truncated ID3v2.2 frame header";
                return false;
            }

            const auto* h = cursor.ptr();
            juce::String rawId;

            for (int i = 0; i < 3; ++i)
            {
                if (! isAsciiAlnumUpper (h[i]))
                {
                    errorOut = "Malformed ID3v2.2 frame id";
                    return false;
                }

                rawId += juce::String::charToString ((juce::juce_wchar) h[i]);
            }

            cursor.skip (3);
            const auto frameSize = cursor.u24be();

            if (! cursor.canRead ((std::size_t) frameSize))
            {
                errorOut = "Truncated ID3v2.2 frame " + rawId;
                return false;
            }

            const Bytes payload = cursor.bytes ((std::size_t) frameSize);
            const auto mappedId = frameIdV22ToV24 (rawId);

            if (mappedId.isEmpty())
                continue;

            Id3Frame frame;
            frame.id = mappedId;
            frame.payload = rawId == "PIC" ? convertV22PicturePayload (payload) : payload;
            out.frames.push_back (std::move (frame));
        }
        else
        {
            if (! cursor.canRead (10))
            {
                errorOut = "Truncated ID3v2 frame header";
                return false;
            }

            const auto* h = cursor.ptr();

            for (int i = 0; i < 4; ++i)
            {
                if (! isAsciiAlnumUpper (h[i]))
                {
                    errorOut = "Malformed ID3v2 frame id";
                    return false;
                }
            }

            juce::String frameId;

            for (int i = 0; i < 4; ++i)
                frameId += juce::String::charToString ((juce::juce_wchar) h[i]);

            std::uint32_t frameSize = 0;

            if (major == 4)
            {
                if (! readSyncSafe32 (h + 4, frameSize))
                {
                    errorOut = "Malformed ID3v2.4 frame size in " + frameId;
                    return false;
                }
            }
            else
            {
                frameSize = readU32BE (h + 4);
            }

            const auto formatFlags = (std::uint16_t) ((h[8] << 8) | h[9]);
            cursor.skip (10);

            if (! cursor.canRead ((std::size_t) frameSize))
            {
                errorOut = "Truncated ID3v2 frame " + frameId;
                return false;
            }

            Bytes raw = cursor.bytes ((std::size_t) frameSize);

            std::size_t prefix = 0;
            bool encrypted = false;

            if (major == 3)
            {
                if ((formatFlags & 0x0080) != 0)
                {
                    if (raw.size() < 4)
                    {
                        errorOut = "Malformed compressed ID3v2.3 frame " + frameId;
                        return false;
                    }

                    prefix += 4;
                }

                if ((formatFlags & 0x0040) != 0)
                    encrypted = true;

                if ((formatFlags & 0x0020) != 0)
                    ++prefix;
            }
            else
            {
                if ((formatFlags & 0x0040) != 0)
                    ++prefix;

                if ((formatFlags & 0x0004) != 0)
                    encrypted = true;

                if ((formatFlags & 0x0001) != 0)
                {
                    if (raw.size() < prefix + 4)
                    {
                        errorOut = "Malformed data length indicator in ID3 frame " + frameId;
                        return false;
                    }

                    prefix += 4;
                }
            }

            if (encrypted)
                continue;   // cannot decode encrypted frames; drop them on rewrite

            if (prefix > raw.size())
            {
                errorOut = "Malformed ID3 frame prefix in " + frameId;
                return false;
            }

            Bytes payload (raw.begin() + (std::ptrdiff_t) prefix, raw.end());

            if (major == 4)
            {
                const bool unsync = (formatFlags & 0x0002) != 0
                                    || ((out.headerFlags & 0x80) != 0 && (formatFlags & 0x0002) == 0);

                if (unsync)
                    payload = deUnsynchronise (payload.data(), payload.size());
            }

            const bool compressed = major == 3 ? (formatFlags & 0x0080) != 0 : (formatFlags & 0x0008) != 0;

            if (compressed)
            {
                Bytes inflated;

                if (! zlibInflate (payload, inflated, errorOut))
                {
                    errorOut = "Could not decompress ID3 " + frameId + " frame: " + errorOut;
                    return false;
                }

                payload = std::move (inflated);
            }

            Id3Frame frame;
            frame.id = frameId;
            frame.formatFlags = formatFlags;
            frame.payload = std::move (payload);
            out.frames.push_back (std::move (frame));
        }
    }

    if (! cursor.ok)
    {
        errorOut = "Malformed ID3v2 tag body";
        return false;
    }

    return true;
}

//==============================================================================
// ID3 text / frame decoding
//==============================================================================

juce::String id3DecodeSpan (const std::uint8_t* p, std::size_t n, int encoding,
                            Utf16Endianness utf16Fallback = Utf16Endianness::little)
{
    if (n == 0)
        return {};

    switch (encoding)
    {
        case 0:  return stringFromLatin1 (p, n);
        case 3:  return codeUnitsToString (decodeUtf8CodePoints (p, n));
        case 2:  return codeUnitsToString (decodeUtf16 (p, n, Utf16Endianness::big));
        case 1:
        default:
        {
            Utf16Endianness endian = utf16Fallback;

            if (n >= 2 && p[0] == 0xff && p[1] == 0xfe)
            {
                p += 2;
                n -= 2;
                endian = Utf16Endianness::little;
            }
            else if (n >= 2 && p[0] == 0xfe && p[1] == 0xff)
            {
                p += 2;
                n -= 2;
                endian = Utf16Endianness::big;
            }

            return codeUnitsToString (decodeUtf16 (p, n, endian));
        }
    }
}

std::size_t findId3Terminator (const std::uint8_t* p, std::size_t n, std::size_t start, int encoding)
{
    const bool wide = encoding == 1 || encoding == 2;
    const std::size_t step = wide ? 2 : 1;

    for (std::size_t i = start; i + step <= n; i += step)
        if (wide ? (p[i] == 0 && p[i + 1] == 0) : (p[i] == 0))
            return i;

    return n;
}

juce::String id3ReadTextFrame (const Bytes& payload)
{
    if (payload.empty())
        return {};

    return sanitiseText (codeUnitsToString (decodeId3Text (payload.data(), payload.size(), -1)));
}

juce::String id3ParseGenre (const juce::String& raw)
{
    juce::String s = raw.trim();

    if (s.isEmpty())
        return s;

    if (s.startsWith ("(("))
        return s.substring (1);

    if (s[0] == '(')
    {
        const int close = s.indexOfChar (')');

        if (close > 1)
        {
            const auto inner = s.substring (1, close);

            if (inner.isNotEmpty() && inner.containsOnly ("0123456789"))
            {
                const auto name = id3GenreName (inner.getIntValue());
                const auto rest = s.substring (close + 1).trim();

                if (name.isNotEmpty())
                    return rest.isEmpty() ? name : name + " " + rest;
            }
        }
    }

    if (s.containsOnly ("0123456789"))
    {
        const auto name = id3GenreName (s.getIntValue());

        if (name.isNotEmpty())
            return name;
    }

    return s;
}

void appendMultiValue (juce::String& target, const juce::String& value)
{
    if (value.isEmpty())
        return;

    if (target.isEmpty())
        target = value;
    else if (! target.containsIgnoreCase (value))
        target += "; " + value;
}

bool id3ParseCommentFrame (const Bytes& payload, juce::String& commentOut)
{
    if (payload.size() < 4)
        return false;

    const int encoding = payload[0] <= 3 ? (int) payload[0] : 0;

    const std::size_t descriptionStart = 4;
    const std::size_t terminator = findId3Terminator (payload.data(), payload.size(), descriptionStart, encoding);
    const std::size_t step = (encoding == 1 || encoding == 2) ? 2 : 1;
    const std::size_t textStart = std::min (payload.size(), terminator + step);

    commentOut = id3DecodeSpan (payload.data() + textStart, payload.size() - textStart, encoding);
    return true;
}

bool id3ParseTxxxFrame (const Bytes& payload, juce::String& descriptionOut, juce::String& valueOut)
{
    if (payload.size() < 2)
        return false;

    const int encoding = payload[0] <= 3 ? (int) payload[0] : 0;
    const std::size_t terminator = findId3Terminator (payload.data(), payload.size(), 1, encoding);
    const std::size_t step = (encoding == 1 || encoding == 2) ? 2 : 1;

    descriptionOut = id3DecodeSpan (payload.data() + 1, terminator - 1, encoding);

    const std::size_t valueStart = std::min (payload.size(), terminator + step);
    valueOut = id3DecodeSpan (payload.data() + valueStart, payload.size() - valueStart, encoding);
    return true;
}

bool id3ParsePopmFrame (const Bytes& payload, int& ratingOut)
{
    std::size_t pos = 0;

    while (pos < payload.size() && payload[pos] != 0)
        ++pos;

    if (pos + 1 >= payload.size())
        return false;

    const auto rawRating = payload[pos + 1];

    if (rawRating == 0)
        ratingOut = 0;
    else if (rawRating <= 31)
        ratingOut = 1;
    else if (rawRating <= 95)
        ratingOut = 2;
    else if (rawRating <= 159)
        ratingOut = 3;
    else if (rawRating <= 223)
        ratingOut = 4;
    else
        ratingOut = 5;

    return true;
}

std::uint8_t ratingToPopmByte (int rating)
{
    switch (rating)
    {
        case 1:  return 13;
        case 2:  return 64;
        case 3:  return 128;
        case 4:  return 192;
        case 5:  return 255;
        default: return 0;
    }
}

bool id3ParseApicFrame (const Bytes& payload, EmbeddedArtwork& artworkOut)
{
    if (payload.size() < 4)
        return false;

    const int encoding = payload[0] <= 3 ? (int) payload[0] : 0;
    std::size_t pos = 1;
    const std::size_t mimeStart = pos;

    while (pos < payload.size() && payload[pos] != 0)
        ++pos;

    if (pos >= payload.size())
        return false;

    artworkOut.mimeType = stringFromLatin1 (payload.data() + mimeStart, pos - mimeStart);
    ++pos;

    if (pos >= payload.size())
        return false;

    artworkOut.pictureType = (int) payload[pos++];

    const std::size_t descriptionStart = pos;
    const std::size_t terminator =
        findId3Terminator (payload.data(), payload.size(), descriptionStart, encoding);
    const std::size_t step = (encoding == 1 || encoding == 2) ? 2 : 1;
    const std::size_t dataStart = std::min (payload.size(), terminator + step);

    if (dataStart >= payload.size())
        return false;

    const auto* imageData = payload.data() + dataStart;
    const std::size_t imageSize = payload.size() - dataStart;

    artworkOut.data.setSize (imageSize, false);

    if (imageSize > 0)
        std::memcpy (artworkOut.data.getData(), imageData, imageSize);

    if (artworkOut.mimeType.isEmpty())
        artworkOut.mimeType = sniffImageMime (imageData, imageSize);

    imageDimensions (imageData, imageSize, artworkOut.width, artworkOut.height);
    return true;
}

bool id3ApicPictureType (const Bytes& payload, int& pictureTypeOut)
{
    EmbeddedArtwork artwork;

    if (! id3ParseApicFrame (payload, artwork))
        return false;

    pictureTypeOut = artwork.pictureType;
    return true;
}

void applyTxxxReplayGain (const juce::String& description, const juce::String& value, TagData& out)
{
    const auto key = description.trim().toUpperCase();

    if (key == "REPLAYGAIN_TRACK_GAIN")
    {
        out.replayGainTrackDb = parseLeadingFloat (value);
        out.hasReplayGainTrack = true;
    }
    else if (key == "REPLAYGAIN_ALBUM_GAIN")
    {
        out.replayGainAlbumDb = parseLeadingFloat (value);
        out.hasReplayGainAlbum = true;
    }
    else if (key == "REPLAYGAIN_TRACK_PEAK")
    {
        out.peakLinear = parseLeadingFloat (value);
        out.hasPeak = true;
    }
    else if (key == "R128_TRACK_GAIN" && ! out.hasReplayGainTrack)
    {
        out.replayGainTrackDb = parseLeadingFloat (value) / 256.0f;
        out.hasReplayGainTrack = true;
    }
    else if (key == "R128_ALBUM_GAIN" && ! out.hasReplayGainAlbum)
    {
        out.replayGainAlbumDb = parseLeadingFloat (value) / 256.0f;
        out.hasReplayGainAlbum = true;
    }
}

void applyId3Frames (const Id3TagInfo& tag, TagData& out)
{
    for (const auto& frame : tag.frames)
    {
        const auto& id = frame.id;

        if (id == "TIT2")             appendMultiValue (out.title, id3ReadTextFrame (frame.payload));
        else if (id == "TPE1")        appendMultiValue (out.artist, id3ReadTextFrame (frame.payload));
        else if (id == "TPE2")        appendMultiValue (out.albumArtist, id3ReadTextFrame (frame.payload));
        else if (id == "TALB")        appendMultiValue (out.album, id3ReadTextFrame (frame.payload));
        else if (id == "TCON")        appendMultiValue (out.genre, id3ParseGenre (id3ReadTextFrame (frame.payload)));
        else if (id == "TCOM")        appendMultiValue (out.composer, id3ReadTextFrame (frame.payload));
        else if (id == "TYER" || id == "TDRC" || id == "TDAT")
        {
            auto value = id3ReadTextFrame (frame.payload);

            if (id == "TDAT" && value.length() == 4)
                value = "19" + value;

            const int year = parseLeadingInt (value);

            if (year > 0)
                out.year = year;
        }
        else if (id == "TRCK")
        {
            const int track = parseLeadingInt (id3ReadTextFrame (frame.payload));

            if (track > 0)
                out.trackNumber = track;
        }
        else if (id == "TPOS")
        {
            const int disc = parseLeadingInt (id3ReadTextFrame (frame.payload));

            if (disc > 0)
                out.discNumber = disc;
        }
        else if (id == "COMM")
        {
            juce::String comment;

            if (id3ParseCommentFrame (frame.payload, comment) && comment.isNotEmpty() && out.comment.isEmpty())
                out.comment = comment;
        }
        else if (id == "TXXX")
        {
            juce::String description, value;

            if (id3ParseTxxxFrame (frame.payload, description, value))
                applyTxxxReplayGain (description, value, out);
        }
        else if (id == "POPM")
        {
            int rating = 0;

            if (id3ParsePopmFrame (frame.payload, rating) && rating > 0)
                out.rating = rating;
        }
    }
}

//==============================================================================
// ID3 writing
//==============================================================================

struct OutFrame
{
    std::string id;
    Bytes payload;
};

std::vector<std::uint32_t> codePointsFromString (const juce::String& s)
{
    const auto utf8 = s.toRawUTF8();
    return decodeUtf8CodePoints (reinterpret_cast<const std::uint8_t*> (utf8),
                                 std::strlen (utf8));
}

OutFrame makeTextFrame (int major, const std::string& id, const juce::String& value)
{
    OutFrame frame;
    frame.id = id;

    if (value.isEmpty())
        return frame;

    if (major == 4)
    {
        frame.payload.push_back (3);   // UTF-8
        const auto utf8 = value.toRawUTF8();
        appendBytes (frame.payload, reinterpret_cast<const std::uint8_t*> (utf8), std::strlen (utf8));
        frame.payload.push_back (0);
    }
    else
    {
        frame.payload.push_back (1);   // UTF-16 with BOM
        frame.payload.push_back (0xff);
        frame.payload.push_back (0xfe);
        appendUtf16LE (frame.payload, codePointsFromString (value));
        frame.payload.push_back (0);
        frame.payload.push_back (0);
    }

    return frame;
}

OutFrame makeCommentFrame (int major, const juce::String& text)
{
    OutFrame frame;
    frame.id = "COMM";
    frame.payload.push_back (major == 4 ? 3 : 1);

    if (major == 4)
    {
        appendAscii (frame.payload, "eng");
        frame.payload.push_back (0);   // empty description
        const auto utf8 = text.toRawUTF8();
        appendBytes (frame.payload, reinterpret_cast<const std::uint8_t*> (utf8), std::strlen (utf8));
    }
    else
    {
        appendAscii (frame.payload, "eng");
        frame.payload.push_back (0x00);   // description terminator (UTF-16)
        frame.payload.push_back (0x00);
        frame.payload.push_back (0xff);
        frame.payload.push_back (0xfe);
        appendUtf16LE (frame.payload, codePointsFromString (text));
    }

    return frame;
}

OutFrame makeTxxxFrame (int major, const char* description, const juce::String& value)
{
    OutFrame frame;
    frame.id = "TXXX";
    frame.payload.push_back (major == 4 ? 3 : 1);

    if (major == 4)
    {
        appendAscii (frame.payload, description);
        frame.payload.push_back (0);
        const auto utf8 = value.toRawUTF8();
        appendBytes (frame.payload, reinterpret_cast<const std::uint8_t*> (utf8), std::strlen (utf8));
    }
    else
    {
        frame.payload.push_back (0xff);
        frame.payload.push_back (0xfe);
        appendUtf16LE (frame.payload, codePointsFromString (juce::String (description)));
        frame.payload.push_back (0x00);
        frame.payload.push_back (0x00);
        frame.payload.push_back (0xff);
        frame.payload.push_back (0xfe);
        appendUtf16LE (frame.payload, codePointsFromString (value));
    }

    return frame;
}

OutFrame makePopmFrame (int rating)
{
    OutFrame frame;
    frame.id = "POPM";
    appendAscii (frame.payload, "SnakePuppy Reference Player");
    frame.payload.push_back (0);
    frame.payload.push_back (ratingToPopmByte (rating));
    appendU32BE (frame.payload, 0);   // play counter
    return frame;
}

OutFrame makeApicFrame (int major, const EmbeddedArtwork& artwork)
{
    OutFrame frame;
    frame.id = "APIC";
    frame.payload.push_back (0);   // ISO-8859-1 for the mime/description

    const auto mime = artwork.mimeType.isNotEmpty() ? artwork.mimeType
                                                    : sniffImageMime ((const std::uint8_t*) artwork.data.getData(),
                                                                      artwork.data.getSize());
    appendAscii (frame.payload, mime.toRawUTF8());
    frame.payload.push_back (0);
    frame.payload.push_back ((std::uint8_t) (artwork.pictureType >= 0 ? artwork.pictureType : 3));
    frame.payload.push_back (0);   // empty description

    appendBytes (frame.payload, static_cast<const std::uint8_t*> (artwork.data.getData()),
                 artwork.data.getSize());
    (void) major;
    return frame;
}

Bytes serializeId3Tag (int major, const std::vector<OutFrame>& frames)
{
    Bytes body;

    for (const auto& frame : frames)
    {
        const std::size_t expectedIdLength = major == 2 ? 3 : 4;

        if (frame.id.size() != expectedIdLength || frame.payload.empty())
            continue;

        appendAscii (body, frame.id.c_str());

        if (major == 4)
            appendSyncSafe32 (body, (std::uint32_t) frame.payload.size());
        else
            appendU32BE (body, (std::uint32_t) frame.payload.size());

        body.push_back (0);   // status flags
        body.push_back (0);   // format flags
        appendBytes (body, frame.payload);
    }

    constexpr std::size_t padding = 1024;
    Bytes tag;
    tag.reserve (10 + body.size() + padding);
    appendAscii (tag, "ID3");
    tag.push_back ((std::uint8_t) major);
    tag.push_back (0);
    tag.push_back (0);
    appendSyncSafe32 (tag, (std::uint32_t) (body.size() + padding));
    appendBytes (tag, body);
    tag.insert (tag.end(), padding, 0);
    return tag;
}

bool isFrameIdInSet (const juce::String& id, const char* const* set, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        if (id == set[i])
            return true;

    return false;
}

/** Rebuilds an ID3 tag from an existing tag (if any) and an update request.
    Unrelated/unknown frames (including non-front-cover artwork) are preserved. */
Bytes buildUpdatedId3Tag (const Id3TagInfo* existing, const TagUpdate& update, juce::String& errorOut)
{
    errorOut.clear();

    const int major = existing != nullptr && existing->major == 3 ? 3 : 4;

    std::vector<OutFrame> frames;

    if (existing != nullptr)
    {
        for (const auto& frame : existing->frames)
        {
            if (frame.id.length() != 4)
                continue;

            OutFrame out;
            out.id = frame.id.toStdString();
            out.payload = frame.payload;
            frames.push_back (std::move (out));
        }
    }

    static const char* const kFieldsToReplace[] = {
        "TIT2", "TPE1", "TPE2", "TALB", "TCON", "TCOM", "TYER", "TDRC", "TRCK", "TPOS", "COMM", "POPM",
    };

    auto eraseIf = [&frames] (const std::function<bool (const OutFrame&)>& predicate)
    {
        frames.erase (std::remove_if (frames.begin(), frames.end(), predicate), frames.end());
    };

    const bool writeReplayGain = update.updateReplayGain
                                || (update.updateFields && update.fields != nullptr
                                    && hasAnyReplayGain (*update.fields));

    if (update.updateFields)
    {
        eraseIf ([&] (const OutFrame& f)
                 {
                     return isFrameIdInSet (juce::String (f.id.c_str()), kFieldsToReplace,
                                            sizeof (kFieldsToReplace) / sizeof (kFieldsToReplace[0]));
                 });
    }

    if (writeReplayGain)
    {
        eraseIf ([] (const OutFrame& f)
                 {
                     if (f.id != "TXXX")
                         return false;

                     juce::String description, value;
                     return id3ParseTxxxFrame (f.payload, description, value)
                            && description.trim().toUpperCase().startsWith ("REPLAYGAIN_");
                 });
        eraseIf ([] (const OutFrame& f)
                 {
                     if (f.id != "TXXX")
                         return false;

                     juce::String description, value;
                     return id3ParseTxxxFrame (f.payload, description, value)
                            && description.trim().toUpperCase().startsWith ("R128_");
                 });
    }

    if (update.updateArtwork)
    {
        eraseIf ([] (const OutFrame& f)
                 {
                     if (f.id != "APIC")
                         return false;

                     int pictureType = 3;
                     return id3ApicPictureType (f.payload, pictureType)
                            && (pictureType == 3 || pictureType == 0);
                 });
    }

    if (update.updateFields && update.fields != nullptr)
    {
        const auto& tags = *update.fields;

        auto addText = [&] (const char* id, const juce::String& value)
        {
            if (value.isNotEmpty())
                frames.push_back (makeTextFrame (major, id, value));
        };

        addText ("TIT2", tags.title);
        addText ("TPE1", tags.artist);
        addText ("TPE2", tags.albumArtist);
        addText ("TALB", tags.album);
        addText ("TCON", tags.genre);
        addText ("TCOM", tags.composer);

        if (tags.year > 0)
            frames.push_back (makeTextFrame (major, major == 3 ? "TYER" : "TDRC",
                                             juce::String (tags.year)));

        addText ("TRCK", formatTrackNumber (tags.trackNumber));
        addText ("TPOS", formatTrackNumber (tags.discNumber));

        if (tags.comment.isNotEmpty())
            frames.push_back (makeCommentFrame (major, tags.comment));

        if (tags.rating > 0)
            frames.push_back (makePopmFrame (tags.rating));
    }

    if (writeReplayGain && update.fields != nullptr)
    {
        const auto& tags = *update.fields;

        if (tags.hasReplayGainTrack)
            frames.push_back (makeTxxxFrame (major, "REPLAYGAIN_TRACK_GAIN", formatGainDb (tags.replayGainTrackDb)));

        if (tags.hasReplayGainAlbum)
            frames.push_back (makeTxxxFrame (major, "REPLAYGAIN_ALBUM_GAIN", formatGainDb (tags.replayGainAlbumDb)));

        if (tags.hasPeak)
            frames.push_back (makeTxxxFrame (major, "REPLAYGAIN_TRACK_PEAK", formatPeak (tags.peakLinear)));
    }

    if (update.updateArtwork && update.artwork != nullptr && update.artwork->data.getSize() > 0)
        frames.push_back (makeApicFrame (major, *update.artwork));

    return serializeId3Tag (major, frames);
}

/** Reads and applies an ID3v2 tag at an arbitrary offset (used by WAV, AIFF,
    DSF and DFF containers). */
bool readEmbeddedId3Tags (const std::uint8_t* data, std::size_t size, std::size_t offset, bool& foundOut,
                          Id3TagInfo& tagOut, TagData& tagsOut, juce::String& errorOut)
{
    if (! parseId3Tag (data, size, offset, tagOut, errorOut, foundOut))
        return false;

    if (foundOut)
        applyId3Frames (tagOut, tagsOut);

    return true;
}

//==============================================================================
// Vorbis comments (FLAC metadata block 4 and the Ogg Vorbis comment header)
//==============================================================================

bool asciiKeyEquals (const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;

    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const auto ca = (char) std::tolower ((unsigned char) a[i]);
        const auto cb = (char) std::tolower ((unsigned char) b[i]);

        if (ca != cb)
            return false;
    }

    return true;
}

bool asciiKeyStartsWith (const std::string& a, const char* prefix)
{
    const std::size_t plen = std::strlen (prefix);

    if (a.size() < plen)
        return false;

    for (std::size_t i = 0; i < plen; ++i)
        if ((char) std::tolower ((unsigned char) a[i]) != (char) std::tolower ((unsigned char) prefix[i]))
            return false;

    return true;
}

struct CommentBag
{
    std::string vendor;
    std::vector<std::pair<std::string, std::string>> entries;

    const std::string* find (const char* key) const
    {
        for (const auto& entry : entries)
            if (asciiKeyEquals (entry.first, key))
                return &entry.second;

        return nullptr;
    }

    juce::String findString (const char* key) const
    {
        const auto* value = find (key);

        if (value == nullptr)
            return {};

        return stringFromUtf8 (reinterpret_cast<const std::uint8_t*> (value->data()), value->size());
    }

    void remove (const char* key)
    {
        entries.erase (std::remove_if (entries.begin(), entries.end(),
                                       [key] (const std::pair<std::string, std::string>& entry)
                                       {
                                           return asciiKeyEquals (entry.first, key);
                                       }),
                       entries.end());
    }

    void set (const char* key, const std::string& value)
    {
        remove (key);
        entries.emplace_back (key, value);
    }

    void set (const char* key, const juce::String& value)
    {
        if (value.isNotEmpty())
        {
            const auto utf8 = value.toRawUTF8();
            set (key, std::string (utf8, std::strlen (utf8)));
        }
    }

    bool parse (const std::uint8_t* data, std::size_t size, bool hasFramingBit, std::string& errorOut)
    {
        entries.clear();
        vendor.clear();

        Cursor cursor (data, size);
        const auto vendorLength = cursor.u32le();

        if (! cursor.ok)
        {
            errorOut = "Truncated Vorbis comment header";
            return false;
        }

        if (! cursor.canRead ((std::size_t) vendorLength))
        {
            errorOut = "Truncated Vorbis comment vendor string";
            return false;
        }

        vendor.assign (reinterpret_cast<const char*> (cursor.ptr()), (std::size_t) vendorLength);
        cursor.skip ((std::size_t) vendorLength);

        const auto commentCount = cursor.u32le();

        if (! cursor.ok)
        {
            errorOut = "Truncated Vorbis comment count";
            return false;
        }

        if ((std::uint64_t) commentCount > (std::uint64_t) size / 4 + 1)
        {
            errorOut = "Malformed Vorbis comment count";
            return false;
        }

        for (std::uint32_t i = 0; i < commentCount; ++i)
        {
            const auto length = cursor.u32le();

            if (! cursor.ok || ! cursor.canRead ((std::size_t) length))
            {
                errorOut = "Truncated Vorbis comment entry";
                return false;
            }

            const auto* entryData = cursor.ptr();
            cursor.skip ((std::size_t) length);

            const auto* equals = static_cast<const std::uint8_t*> (
                std::memchr (entryData, '=', (std::size_t) length));

            if (equals == nullptr)
                continue;   // malformed entry: ignore rather than fail the whole tag

            const std::size_t keyLength = (std::size_t) (equals - entryData);
            const std::size_t valueLength = (std::size_t) length - keyLength - 1;

            entries.emplace_back (std::string (reinterpret_cast<const char*> (entryData), keyLength),
                                  std::string (reinterpret_cast<const char*> (equals + 1), valueLength));
        }

        if (hasFramingBit && cursor.remaining() >= 1)
        {
            const auto framing = cursor.u8();

            if ((framing & 0x01) == 0)
            {
                errorOut = "Vorbis comment header is missing its framing bit";
                return false;
            }
        }

        return cursor.ok;
    }

    Bytes build (bool hasFramingBit) const
    {
        Bytes out;
        out.reserve (64 + entries.size() * 16);
        appendU32LE (out, (std::uint32_t) vendor.size());
        appendBytes (out, reinterpret_cast<const std::uint8_t*> (vendor.data()), vendor.size());
        appendU32LE (out, (std::uint32_t) entries.size());

        for (const auto& entry : entries)
        {
            const std::size_t entryLength = entry.first.size() + 1 + entry.second.size();
            appendU32LE (out, (std::uint32_t) entryLength);
            appendBytes (out, reinterpret_cast<const std::uint8_t*> (entry.first.data()), entry.first.size());
            out.push_back ('=');
            appendBytes (out, reinterpret_cast<const std::uint8_t*> (entry.second.data()), entry.second.size());
        }

        if (hasFramingBit)
            out.push_back (1);

        return out;
    }
};

void applyVorbisReplayGain (const CommentBag& bag, TagData& out)
{
    auto get = [&bag] (const char* key) { return bag.findString (key); };

    const auto trackGain = get ("REPLAYGAIN_TRACK_GAIN");
    const auto albumGain = get ("REPLAYGAIN_ALBUM_GAIN");
    const auto trackPeak = get ("REPLAYGAIN_TRACK_PEAK");
    const auto r128Track = get ("R128_TRACK_GAIN");
    const auto r128Album = get ("R128_ALBUM_GAIN");

    if (trackGain.isNotEmpty())
    {
        out.replayGainTrackDb = parseLeadingFloat (trackGain);
        out.hasReplayGainTrack = true;
    }
    else if (r128Track.isNotEmpty())
    {
        out.replayGainTrackDb = parseLeadingFloat (r128Track) / 256.0f;
        out.hasReplayGainTrack = true;
    }

    if (albumGain.isNotEmpty())
    {
        out.replayGainAlbumDb = parseLeadingFloat (albumGain);
        out.hasReplayGainAlbum = true;
    }
    else if (r128Album.isNotEmpty())
    {
        out.replayGainAlbumDb = parseLeadingFloat (r128Album) / 256.0f;
        out.hasReplayGainAlbum = true;
    }

    if (trackPeak.isNotEmpty())
    {
        out.peakLinear = parseLeadingFloat (trackPeak);
        out.hasPeak = true;
    }
}

void applyVorbisComments (const CommentBag& bag, TagData& out)
{
    auto get = [&bag] (const char* key) { return sanitiseText (bag.findString (key)); };

    setIfNotEmpty (out.title, get ("TITLE"));
    setIfNotEmpty (out.artist, get ("ARTIST"));
    setIfNotEmpty (out.album, get ("ALBUM"));
    setIfNotEmpty (out.genre, get ("GENRE"));
    setIfNotEmpty (out.composer, get ("COMPOSER"));

    auto albumArtist = get ("ALBUMARTIST");

    if (albumArtist.isEmpty())
        albumArtist = get ("ALBUM ARTIST");

    setIfNotEmpty (out.albumArtist, albumArtist);

    auto comment = get ("COMMENT");

    if (comment.isEmpty())
        comment = get ("DESCRIPTION");

    setIfNotEmpty (out.comment, comment);

    auto year = get ("DATE");

    if (year.isEmpty())
        year = get ("YEAR");

    if (year.isNotEmpty())
    {
        const int parsed = parseLeadingInt (year);

        if (parsed > 0)
            out.year = parsed;
    }

    auto track = get ("TRACKNUMBER");

    if (track.isEmpty())
        track = get ("TRACK");

    if (track.isNotEmpty())
    {
        const int parsed = parseLeadingInt (track);

        if (parsed > 0)
            out.trackNumber = parsed;
    }

    auto disc = get ("DISCNUMBER");

    if (disc.isEmpty())
        disc = get ("DISC");

    if (disc.isNotEmpty())
    {
        const int parsed = parseLeadingInt (disc);

        if (parsed > 0)
            out.discNumber = parsed;
    }

    applyVorbisReplayGain (bag, out);
}

void rewriteVorbisCommentFields (CommentBag& bag, const TagData& fields, bool writeReplayGain)
{
    static const char* const kReplaceKeys[] = {
        "TITLE", "ARTIST", "ALBUMARTIST", "ALBUM ARTIST", "ALBUM", "GENRE", "COMPOSER",
        "COMMENT", "DESCRIPTION", "DATE", "YEAR", "TRACKNUMBER", "TRACK", "DISCNUMBER", "DISC",
    };

    for (const auto* key : kReplaceKeys)
        bag.remove (key);

    bag.set ("TITLE", fields.title);
    bag.set ("ARTIST", fields.artist);
    bag.set ("ALBUMARTIST", fields.albumArtist);
    bag.set ("ALBUM", fields.album);
    bag.set ("GENRE", fields.genre);
    bag.set ("COMPOSER", fields.composer);
    bag.set ("COMMENT", fields.comment);

    if (fields.year > 0)
        bag.set ("DATE", juce::String (fields.year));

    if (fields.trackNumber > 0)
        bag.set ("TRACKNUMBER", formatTrackNumber (fields.trackNumber));

    if (fields.discNumber > 0)
        bag.set ("DISCNUMBER", formatTrackNumber (fields.discNumber));

    if (writeReplayGain)
    {
        bag.entries.erase (std::remove_if (bag.entries.begin(), bag.entries.end(),
                                           [] (const std::pair<std::string, std::string>& entry)
                                           {
                                               return asciiKeyStartsWith (entry.first, "REPLAYGAIN_")
                                                      || asciiKeyStartsWith (entry.first, "R128_");
                                           }),
                           bag.entries.end());

        if (fields.hasReplayGainTrack)
            bag.set ("REPLAYGAIN_TRACK_GAIN", formatGainDb (fields.replayGainTrackDb));

        if (fields.hasReplayGainAlbum)
            bag.set ("REPLAYGAIN_ALBUM_GAIN", formatGainDb (fields.replayGainAlbumDb));

        if (fields.hasPeak)
            bag.set ("REPLAYGAIN_TRACK_PEAK", formatPeak (fields.peakLinear));
    }
}

//==============================================================================
// Vorbis/FLAC picture blocks (also used by Ogg METADATA_BLOCK_PICTURE)
//==============================================================================

struct PictureData
{
    std::uint32_t type = 3;
    std::string mime;
    std::string description;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::uint32_t colors = 0;
    Bytes data;
};

bool parsePictureBlock (const std::uint8_t* data, std::size_t size, PictureData& out, std::string& errorOut)
{
    Cursor cursor (data, size);
    out.type = cursor.u32be();
    const auto mimeLength = cursor.u32be();

    if (! cursor.canRead ((std::size_t) mimeLength))
    {
        errorOut = "Truncated picture MIME string";
        return false;
    }

    out.mime.assign (reinterpret_cast<const char*> (cursor.ptr()), (std::size_t) mimeLength);
    cursor.skip ((std::size_t) mimeLength);

    const auto descriptionLength = cursor.u32be();

    if (! cursor.canRead ((std::size_t) descriptionLength))
    {
        errorOut = "Truncated picture description";
        return false;
    }

    out.description.assign (reinterpret_cast<const char*> (cursor.ptr()), (std::size_t) descriptionLength);
    cursor.skip ((std::size_t) descriptionLength);

    out.width = cursor.u32be();
    out.height = cursor.u32be();
    out.depth = cursor.u32be();
    out.colors = cursor.u32be();
    const auto dataLength = cursor.u32be();

    if (! cursor.ok || ! cursor.canRead ((std::size_t) dataLength))
    {
        errorOut = "Truncated embedded picture data";
        return false;
    }

    out.data = cursor.bytes ((std::size_t) dataLength);
    return true;
}

Bytes buildPictureBlock (const PictureData& picture)
{
    Bytes out;
    appendU32BE (out, picture.type);
    appendU32BE (out, (std::uint32_t) picture.mime.size());
    appendBytes (out, reinterpret_cast<const std::uint8_t*> (picture.mime.data()), picture.mime.size());
    appendU32BE (out, (std::uint32_t) picture.description.size());
    appendBytes (out, reinterpret_cast<const std::uint8_t*> (picture.description.data()),
                 picture.description.size());
    appendU32BE (out, picture.width);
    appendU32BE (out, picture.height);
    appendU32BE (out, picture.depth);
    appendU32BE (out, picture.colors);
    appendU32BE (out, (std::uint32_t) picture.data.size());
    appendBytes (out, picture.data);
    return out;
}

PictureData pictureFromArtwork (const EmbeddedArtwork& artwork)
{
    PictureData picture;
    picture.type = (std::uint32_t) (artwork.pictureType >= 0 ? artwork.pictureType : 3);
    picture.mime = artwork.mimeType.isNotEmpty()
                       ? artwork.mimeType.toStdString()
                       : sniffImageMime (static_cast<const std::uint8_t*> (artwork.data.getData()),
                                         artwork.data.getSize())
                             .toStdString();

    const auto* imageData = static_cast<const std::uint8_t*> (artwork.data.getData());
    picture.width = (std::uint32_t) std::max (0, artwork.width);
    picture.height = (std::uint32_t) std::max (0, artwork.height);

    if (picture.width == 0 || picture.height == 0)
    {
        int w = 0, h = 0;

        if (imageDimensions (imageData, artwork.data.getSize(), w, h))
        {
            picture.width = (std::uint32_t) w;
            picture.height = (std::uint32_t) h;
        }
    }

    if (picture.mime == "image/jpeg")
        picture.depth = 24;
    else if (picture.mime == "image/png")
        picture.depth = 32;

    picture.data.assign (imageData, imageData + artwork.data.getSize());
    return picture;
}

EmbeddedArtwork artworkFromPicture (const PictureData& picture)
{
    EmbeddedArtwork artwork;
    artwork.pictureType = (int) picture.type;
    artwork.mimeType = juce::String (picture.mime.c_str());
    artwork.width = (int) std::min<std::uint32_t> (picture.width, 0x7fffffff);
    artwork.height = (int) std::min<std::uint32_t> (picture.height, 0x7fffffff);
    artwork.data.setSize (picture.data.size(), false);

    if (! picture.data.empty())
        std::memcpy (artwork.data.getData(), picture.data.data(), picture.data.size());

    if (artwork.mimeType.isEmpty())
        artwork.mimeType = sniffImageMime (picture.data.data(), picture.data.size());

    if (artwork.width == 0 || artwork.height == 0)
        imageDimensions (picture.data.data(), picture.data.size(), artwork.width, artwork.height);

    return artwork;
}

void sortArtworkFrontCoverFirst (std::vector<EmbeddedArtwork>& artwork)
{
    std::stable_sort (artwork.begin(), artwork.end(),
                      [] (const EmbeddedArtwork& a, const EmbeddedArtwork& b)
                      {
                          if (a.isFrontCover() != b.isFrontCover())
                              return a.isFrontCover();

                          return a.pictureType < b.pictureType;
                      });
}

//==============================================================================
// FLAC
//==============================================================================

struct FlacBlock
{
    int type = 0;
    Bytes data;
};

struct FlacFile
{
    std::vector<FlacBlock> blocks;
    std::size_t audioStart = 0;
};

bool parseFlacFile (const std::uint8_t* data, std::size_t size, FlacFile& out, juce::String& errorOut)
{
    out = FlacFile {};

    if (size < 8 || ! fourCCEquals (data, "fLaC"))
    {
        errorOut = "Not a FLAC stream (missing fLaC marker)";
        return false;
    }

    Cursor cursor (data + 4, size - 4);
    bool last = false;

    while (! last)
    {
        if (! cursor.canRead (4))
        {
            errorOut = "Truncated FLAC metadata block header";
            return false;
        }

        const auto header = cursor.u8();
        last = (header & 0x80) != 0;
        const int type = header & 0x7f;

        if (type == 127)
        {
            errorOut = "Invalid FLAC metadata block type 127";
            return false;
        }

        const auto length = cursor.u24be();

        if (! cursor.ok || ! cursor.canRead ((std::size_t) length))
        {
            errorOut = "Truncated FLAC metadata block (declared " + juce::String ((int) length) + " bytes)";
            return false;
        }

        FlacBlock block;
        block.type = type;
        block.data = cursor.bytes ((std::size_t) length);
        out.blocks.push_back (std::move (block));
    }

    out.audioStart = 4 + cursor.pos;
    return true;
}

bool readFlacTags (const std::uint8_t* data, std::size_t size, TagData& tagsOut, juce::String& errorOut)
{
    FlacFile flac;

    if (! parseFlacFile (data, size, flac, errorOut))
        return false;

    for (const auto& block : flac.blocks)
    {
        if (block.type == 4)
        {
            CommentBag bag;
            std::string commentError;

            if (! bag.parse (block.data.data(), block.data.size(), false, commentError))
            {
                errorOut = "Malformed FLAC Vorbis comment block: " + juce::String (commentError.c_str());
                return false;
            }

            applyVorbisComments (bag, tagsOut);
            break;
        }
    }

    return true;
}

std::vector<EmbeddedArtwork> readFlacArtwork (const FlacFile& flac, juce::String& errorOut)
{
    std::vector<EmbeddedArtwork> artwork;

    for (const auto& block : flac.blocks)
    {
        if (block.type != 6)
            continue;

        PictureData picture;
        std::string pictureError;

        if (! parsePictureBlock (block.data.data(), block.data.size(), picture, pictureError))
        {
            errorOut = "Malformed FLAC picture block: " + juce::String (pictureError.c_str());
            return {};
        }

        artwork.push_back (artworkFromPicture (picture));
    }

    sortArtworkFrontCoverFirst (artwork);
    return artwork;
}

bool writeFlac (const juce::File& file, const Bytes& fileData, const TagUpdate& update, juce::String& errorOut)
{
    FlacFile flac;

    if (! parseFlacFile (fileData.data(), fileData.size(), flac, errorOut))
        return false;

    if (flac.blocks.empty() || flac.blocks.front().type != 0)
    {
        errorOut = "FLAC file has no STREAMINFO block; refusing to write tags";
        return false;
    }

    if (flac.blocks.front().data.size() != 34)
    {
        errorOut = "FLAC STREAMINFO block has an unexpected size; refusing to write tags";
        return false;
    }

    CommentBag bag;

    for (const auto& block : flac.blocks)
    {
        if (block.type == 4)
        {
            std::string commentError;

            if (! bag.parse (block.data.data(), block.data.size(), false, commentError))
            {
                errorOut = "Existing FLAC comment block is malformed: " + juce::String (commentError.c_str());
                return false;
            }

            break;
        }
    }

    if (bag.vendor.empty())
        bag.vendor = "Snake Puppy Reference Player";

    if (update.updateFields && update.fields != nullptr)
    {
        const bool writeReplayGain = update.updateReplayGain || hasAnyReplayGain (*update.fields);
        rewriteVorbisCommentFields (bag, *update.fields, writeReplayGain);
    }
    else if (update.updateReplayGain && update.fields != nullptr)
    {
        rewriteVorbisCommentFields (bag, *update.fields, true);
    }

    std::vector<FlacBlock> newBlocks;
    bool commentWritten = false;

    for (const auto& block : flac.blocks)
    {
        if (block.type == 4)
        {
            if (! commentWritten)
            {
                FlacBlock commentBlock;
                commentBlock.type = 4;
                commentBlock.data = bag.build (false);
                newBlocks.push_back (std::move (commentBlock));
                commentWritten = true;
            }

            continue;
        }

        if (block.type == 1)
            continue;   // old padding is regenerated below

        if (block.type == 6 && update.updateArtwork)
        {
            PictureData picture;
            std::string pictureError;

            if (parsePictureBlock (block.data.data(), block.data.size(), picture, pictureError))
            {
                if (picture.type == 3 || picture.type == 0)
                    continue;   // drop the old front cover
            }
        }

        newBlocks.push_back (block);
    }

    if (! commentWritten)
    {
        FlacBlock commentBlock;
        commentBlock.type = 4;
        commentBlock.data = bag.build (false);
        newBlocks.insert (newBlocks.begin() + 1, std::move (commentBlock));
    }

    if (update.updateArtwork && update.artwork != nullptr && update.artwork->data.getSize() > 0)
    {
        FlacBlock pictureBlock;
        pictureBlock.type = 6;
        pictureBlock.data = buildPictureBlock (pictureFromArtwork (*update.artwork));
        newBlocks.push_back (std::move (pictureBlock));
    }

    // Recompute padding so the audio payload normally stays at the same byte
    // offset, and never exceeds the 24-bit block-size field.
    std::size_t contentSize = 4;   // "fLaC"

    for (const auto& block : newBlocks)
        contentSize += 4 + block.data.size();

    std::size_t paddingSize = 0;

    if (flac.audioStart > contentSize + 4)
        paddingSize = flac.audioStart - contentSize - 4;

    if (paddingSize > 0xffffff)
        paddingSize = 0xffffff;

    if (paddingSize > 0)
    {
        FlacBlock padding;
        padding.type = 1;
        padding.data.assign (paddingSize, 0);
        newBlocks.push_back (std::move (padding));
    }

    Bytes out;
    out.reserve (flac.audioStart + (fileData.size() - flac.audioStart) + 64);
    appendAscii (out, "fLaC");

    for (std::size_t i = 0; i < newBlocks.size(); ++i)
    {
        const bool last = i + 1 == newBlocks.size();
        const auto& block = newBlocks[i];
        out.push_back ((std::uint8_t) (block.type | (last ? 0x80 : 0)));
        out.push_back ((std::uint8_t) ((block.data.size() >> 16) & 0xff));
        out.push_back ((std::uint8_t) ((block.data.size() >> 8) & 0xff));
        out.push_back ((std::uint8_t) (block.data.size() & 0xff));
        appendBytes (out, block.data);
    }

    appendBytes (out, fileData.data() + flac.audioStart, fileData.size() - flac.audioStart);

    return writeFileAtomically (file, out, errorOut);
}

//==============================================================================
// Ogg (Vorbis + METADATA_BLOCK_PICTURE)
//==============================================================================

struct OggPage
{
    std::size_t start = 0;
    std::uint8_t headerType = 0;
    std::uint64_t granule = 0;
    std::uint32_t serial = 0;
    std::uint32_t sequence = 0;
    Bytes lacing;
    Bytes body;
};

struct OggPacket
{
    Bytes bytes;
    std::size_t endPageIndex = 0;
    bool endsAtPageEnd = false;
};

bool parseOggPages (const std::uint8_t* data, std::size_t size, std::vector<OggPage>& pages,
                    juce::String& errorOut)
{
    pages.clear();
    std::size_t pos = 0;

    while (pos < size)
    {
        if (! inBounds (size, pos, 27))
        {
            errorOut = "Truncated Ogg page header";
            return false;
        }

        if (! fourCCEquals (data + pos, "OggS"))
        {
            errorOut = "Malformed Ogg stream (lost page sync at offset " + juce::String ((int) pos) + ")";
            return false;
        }

        if (data[pos + 4] != 0)
        {
            errorOut = "Unsupported Ogg stream version " + juce::String ((int) data[pos + 4]);
            return false;
        }

        OggPage page;
        page.start = pos;
        page.headerType = data[pos + 5];
        page.granule = readU64LE (data + pos + 6);
        page.serial = readU32LE (data + pos + 14);
        page.sequence = readU32LE (data + pos + 18);

        const std::size_t segmentCount = (std::size_t) data[pos + 26];

        if (! inBounds (size, pos + 27, segmentCount))
        {
            errorOut = "Truncated Ogg segment table";
            return false;
        }

        page.lacing.assign (data + pos + 27, data + pos + 27 + segmentCount);

        std::size_t bodySize = 0;

        for (const auto segment : page.lacing)
            bodySize += segment;

        if (! inBounds (size, pos + 27 + segmentCount, bodySize))
        {
            errorOut = "Truncated Ogg page body";
            return false;
        }

        page.body.assign (data + pos + 27 + segmentCount, data + pos + 27 + segmentCount + bodySize);
        pos += 27 + segmentCount + bodySize;
        pages.push_back (std::move (page));
    }

    if (pages.empty())
    {
        errorOut = "No Ogg pages found";
        return false;
    }

    return true;
}

bool assembleOggPackets (const std::vector<OggPage>& pages, std::vector<OggPacket>& packets,
                         juce::String& errorOut)
{
    packets.clear();
    Bytes current;

    for (std::size_t pageIndex = 0; pageIndex < pages.size(); ++pageIndex)
    {
        const auto& page = pages[pageIndex];
        std::size_t bodyOffset = 0;

        for (std::size_t segmentIndex = 0; segmentIndex < page.lacing.size(); ++segmentIndex)
        {
            const std::size_t segmentLength = (std::size_t) page.lacing[segmentIndex];

            if (! inBounds (page.body.size(), bodyOffset, segmentLength))
            {
                errorOut = "Malformed Ogg lacing table";
                return false;
            }

            current.insert (current.end(), page.body.begin() + (std::ptrdiff_t) bodyOffset,
                            page.body.begin() + (std::ptrdiff_t) (bodyOffset + segmentLength));
            bodyOffset += segmentLength;

            if (segmentLength < 255)
            {
                OggPacket packet;
                packet.bytes = std::move (current);
                packet.endPageIndex = pageIndex;
                packet.endsAtPageEnd = segmentIndex + 1 == page.lacing.size();
                packets.push_back (std::move (packet));
                current.clear();
            }
        }
    }

    if (! current.empty())
    {
        errorOut = "Truncated Ogg packet at end of stream";
        return false;
    }

    return true;
}

std::vector<std::uint8_t> oggLacingForPacket (std::size_t packetSize)
{
    std::vector<std::uint8_t> lacing;
    const std::size_t fullSegments = packetSize / 255;
    lacing.reserve (fullSegments + 1);

    for (std::size_t i = 0; i < fullSegments; ++i)
        lacing.push_back (255);

    lacing.push_back ((std::uint8_t) (packetSize % 255));
    return lacing;
}

Bytes buildOggPage (std::uint8_t headerType, std::uint64_t granule, std::uint32_t serial,
                    std::uint32_t sequence, const std::vector<std::uint8_t>& lacing, const Bytes& body)
{
    Bytes page;
    page.reserve (27 + lacing.size() + body.size());
    appendAscii (page, "OggS");
    page.push_back (0);              // stream structure version
    page.push_back (headerType);
    appendU64LE (page, granule);
    appendU32LE (page, serial);
    appendU32LE (page, sequence);
    appendU32LE (page, 0);           // checksum placeholder
    page.push_back ((std::uint8_t) lacing.size());
    appendBytes (page, lacing.data(), lacing.size());
    appendBytes (page, body);

    const auto crc = oggCrc32 (page.data(), page.size());
    page[22] = (std::uint8_t) (crc & 0xff);
    page[23] = (std::uint8_t) ((crc >> 8) & 0xff);
    page[24] = (std::uint8_t) ((crc >> 16) & 0xff);
    page[25] = (std::uint8_t) ((crc >> 24) & 0xff);
    return page;
}

bool readOggTags (const std::uint8_t* data, std::size_t size, TagData& tagsOut,
                  std::vector<EmbeddedArtwork>* artworkOut, juce::String& errorOut)
{
    std::vector<OggPage> pages;
    std::vector<OggPacket> packets;

    if (! parseOggPages (data, size, pages, errorOut))
        return false;

    if (! assembleOggPackets (pages, packets, errorOut))
        return false;

    if (packets.size() < 3 || packets[0].bytes.size() < 7 || packets[1].bytes.size() < 7)
    {
        errorOut = "Ogg stream does not contain the three Vorbis headers";
        return false;
    }

    if (packets[0].bytes[0] != 0x01 || packets[0].bytes[1] != 'v')
    {
        errorOut = "Ogg stream is not Vorbis (unsupported codec)";
        return false;
    }

    if (packets[1].bytes[0] != 0x03)
    {
        errorOut = "Ogg Vorbis comment header is missing";
        return false;
    }

    CommentBag bag;
    std::string commentError;

    if (! bag.parse (packets[1].bytes.data() + 7, packets[1].bytes.size() - 7, true, commentError))
    {
        errorOut = "Malformed Ogg Vorbis comment header: " + juce::String (commentError.c_str());
        return false;
    }

    applyVorbisComments (bag, tagsOut);

    if (artworkOut != nullptr)
    {
        for (const auto& entry : bag.entries)
        {
            if (! asciiKeyEquals (entry.first, "METADATA_BLOCK_PICTURE"))
                continue;

            Bytes pictureBytes;

            if (! base64DecodeBytes (stringFromUtf8 (reinterpret_cast<const std::uint8_t*> (entry.second.data()),
                                                     entry.second.size()),
                                     pictureBytes))
            {
                errorOut = "Malformed Ogg METADATA_BLOCK_PICTURE base64 data";
                return false;
            }

            PictureData picture;
            std::string pictureError;

            if (! parsePictureBlock (pictureBytes.data(), pictureBytes.size(), picture, pictureError))
            {
                errorOut = "Malformed Ogg artwork block: " + juce::String (pictureError.c_str());
                return false;
            }

            artworkOut->push_back (artworkFromPicture (picture));
        }

        sortArtworkFrontCoverFirst (*artworkOut);
    }

    return true;
}

bool writeOgg (const juce::File& file, const Bytes& fileData, const TagUpdate& update, juce::String& errorOut)
{
    std::vector<OggPage> pages;
    std::vector<OggPacket> packets;

    if (! parseOggPages (fileData.data(), fileData.size(), pages, errorOut))
        return false;

    if (! assembleOggPackets (pages, packets, errorOut))
        return false;

    if (packets.size() < 3)
    {
        errorOut = "Ogg stream does not contain the three Vorbis headers";
        return false;
    }

    if (packets[0].bytes.size() < 7 || packets[0].bytes[0] != 0x01 || packets[1].bytes.size() < 7
        || packets[1].bytes[0] != 0x03 || packets[2].bytes.size() < 7 || packets[2].bytes[0] != 0x05)
    {
        errorOut = "Ogg stream is not a supported Vorbis stream";
        return false;
    }

    if (! packets[2].endsAtPageEnd)
    {
        errorOut = "Unexpected Ogg Vorbis page layout (setup header shares a page with audio)";
        return false;
    }

    const auto serial = pages[0].serial;

    for (const auto& page : pages)
    {
        if (page.serial != serial)
        {
            errorOut = "Chained or multiplexed Ogg streams are not supported for tag writing";
            return false;
        }
    }

    CommentBag bag;
    std::string commentError;

    if (! bag.parse (packets[1].bytes.data() + 7, packets[1].bytes.size() - 7, true, commentError))
    {
        errorOut = "Existing Ogg comment header is malformed: " + juce::String (commentError.c_str());
        return false;
    }

    if (bag.vendor.empty())
        bag.vendor = "Snake Puppy Reference Player";

    if (update.updateFields && update.fields != nullptr)
    {
        const bool writeReplayGain = update.updateReplayGain || hasAnyReplayGain (*update.fields);
        rewriteVorbisCommentFields (bag, *update.fields, writeReplayGain);
    }
    else if (update.updateReplayGain && update.fields != nullptr)
    {
        rewriteVorbisCommentFields (bag, *update.fields, true);
    }

    if (update.updateArtwork)
    {
        bag.entries.erase (std::remove_if (bag.entries.begin(), bag.entries.end(),
                                           [] (const std::pair<std::string, std::string>& entry)
                                           {
                                               return asciiKeyEquals (entry.first, "METADATA_BLOCK_PICTURE");
                                           }),
                           bag.entries.end());

        if (update.artwork != nullptr && update.artwork->data.getSize() > 0)
        {
            const auto pictureBytes = buildPictureBlock (pictureFromArtwork (*update.artwork));
            bag.set ("METADATA_BLOCK_PICTURE",
                     base64EncodeBytes (pictureBytes.data(), pictureBytes.size()).toStdString());
        }
    }

    Bytes newCommentPacket;
    newCommentPacket.push_back (0x03);
    appendAscii (newCommentPacket, "vorbis");
    appendBytes (newCommentPacket, bag.build (true));

    const std::size_t firstAudioPageIndex = packets[2].endPageIndex + 1;

    // Vorbis requires the comment header and the setup header to share the
    // second page of the stream. Keep that layout so other decoders can read
    // the tags too.
    const auto commentLacing = oggLacingForPacket (newCommentPacket.size());
    const auto setupLacing = oggLacingForPacket (packets[2].bytes.size());

    if (commentLacing.size() + setupLacing.size() > 255)
    {
        errorOut = "Ogg comment + setup headers do not fit in one page (embedded artwork too large?)";
        return false;
    }

    std::vector<std::uint8_t> headerLacing = commentLacing;
    headerLacing.insert (headerLacing.end(), setupLacing.begin(), setupLacing.end());

    Bytes headerBody = newCommentPacket;
    appendBytes (headerBody, packets[2].bytes);

    Bytes out;
    out.reserve (fileData.size() + newCommentPacket.size() + 128);

    appendBytes (out, buildOggPage (0, 0, serial, 0, oggLacingForPacket (packets[0].bytes.size()),
                                    packets[0].bytes));
    appendBytes (out, buildOggPage (0, 0, serial, 1, headerLacing, headerBody));

    // The header page count may change when a multi-page header is normalised
    // to the canonical two-page layout; keep the audio page sequence valid.
    const std::int64_t sequenceDelta = (std::int64_t) 2 - (std::int64_t) firstAudioPageIndex;

    for (std::size_t pageIndex = firstAudioPageIndex; pageIndex < pages.size(); ++pageIndex)
    {
        const auto& page = pages[pageIndex];
        const auto adjustedSequence = (std::uint32_t) ((std::int64_t) page.sequence + sequenceDelta);
        appendBytes (out, buildOggPage (page.headerType, page.granule, page.serial, adjustedSequence,
                                        page.lacing, page.body));
    }

    return writeFileAtomically (file, out, errorOut);
}

//==============================================================================
// MP4 / M4A (IsoMedia ilst atoms)
//==============================================================================

struct Mp4Atom
{
    std::string type;
    std::size_t start = 0;
    std::size_t headerSize = 8;
    std::size_t size = 0;

    std::size_t contentStart() const noexcept { return start + headerSize; }
    std::size_t end() const noexcept { return start + size; }
};

bool parseMp4Atoms (const std::uint8_t* data, std::size_t dataSize, std::size_t start, std::size_t end,
                    std::vector<Mp4Atom>& out, juce::String& errorOut)
{
    out.clear();
    std::size_t pos = start;
    int guard = 0;

    while (pos + 8 <= end)
    {
        if (++guard > 100000)
        {
            errorOut = "Too many MP4 atoms (malformed file?)";
            return false;
        }

        const std::uint32_t atomSize = readU32BE (data + pos);
        const std::string type (reinterpret_cast<const char*> (data + pos + 4), 4);

        std::size_t headerSize = 8;
        std::uint64_t totalSize = atomSize;

        if (atomSize == 1)
        {
            if (pos + 16 > end)
            {
                errorOut = "Truncated 64-bit MP4 atom header";
                return false;
            }

            headerSize = 16;
            totalSize = readU64BE (data + pos + 8);
        }
        else if (atomSize == 0)
        {
            totalSize = end - pos;
        }

        if (totalSize < headerSize || totalSize > (std::uint64_t) (end - pos))
        {
            errorOut = "Malformed MP4 atom '" + juce::String (type.c_str()) + "' (size "
                       + juce::String ((juce::int64) totalSize) + " at offset " + juce::String ((int) pos) + ")";
            return false;
        }

        Mp4Atom atom;
        atom.type = type;
        atom.start = pos;
        atom.headerSize = headerSize;
        atom.size = (std::size_t) totalSize;
        out.push_back (atom);
        pos += atom.size;
    }

    if (pos != end)
    {
        // A few files end with zero padding smaller than an atom header.
        for (std::size_t i = pos; i < end; ++i)
        {
            if (data[i] != 0)
            {
                errorOut = "Trailing garbage after MP4 atoms";
                return false;
            }
        }
    }

    (void) dataSize;
    return true;
}

/** Builds the fourCC for an iTunes "copyright-sign" atom such as (c)nam.
    Written this way to avoid ambiguous C++ hex escapes like "\xa9alb". */
std::string mp4CopyTag (const char* suffix)
{
    return std::string (1, (char) 0xa9) + suffix;
}

const Mp4Atom* findMp4Atom (const std::vector<Mp4Atom>& atoms, const char* type)
{
    for (const auto& atom : atoms)
        if (atom.type == type)
            return &atom;

    return nullptr;
}

Bytes makeMp4Atom (const char* type, const Bytes& content)
{
    Bytes atom;
    atom.reserve (8 + content.size());
    appendU32BE (atom, (std::uint32_t) (8 + content.size()));
    appendAscii (atom, type);
    appendBytes (atom, content);
    return atom;
}

Bytes makeMp4MetaHandler()
{
    Bytes content;
    appendU32BE (content, 0);   // version/flags
    appendU32BE (content, 0);   // predefined
    appendAscii (content, "mdir");
    content.insert (content.end(), 12, 0);   // reserved
    content.push_back (0);                   // empty name
    return makeMp4Atom ("hdlr", content);
}

struct Mp4MetadataLocation
{
    Mp4Atom moov;
    bool hasMoov = false;
    Mp4Atom udta;
    Mp4Atom meta;
    Mp4Atom ilst;
    bool hasUdta = false;
    bool hasMeta = false;
    bool hasIlst = false;
    bool metaHasVersionFlags = true;
};

bool parseMp4MetaChildren (const std::uint8_t* data, std::size_t dataSize, const Mp4Atom& meta,
                           std::vector<Mp4Atom>& children, bool& hasVersionFlags, juce::String& errorOut)
{
    std::vector<Mp4Atom> withVersionFlags;
    juce::String firstError;

    if (parseMp4Atoms (data, dataSize, meta.contentStart() + 4, meta.end(), withVersionFlags, firstError)
        && ! withVersionFlags.empty())
    {
        children = std::move (withVersionFlags);
        hasVersionFlags = true;
        return true;
    }

    std::vector<Mp4Atom> withoutVersionFlags;
    juce::String secondError;

    if (parseMp4Atoms (data, dataSize, meta.contentStart(), meta.end(), withoutVersionFlags, secondError))
    {
        children = std::move (withoutVersionFlags);
        hasVersionFlags = false;
        return true;
    }

    errorOut = "Malformed MP4 meta atom: " + firstError;
    return false;
}

bool locateMp4Metadata (const std::uint8_t* data, std::size_t dataSize, Mp4MetadataLocation& loc,
                        juce::String& errorOut)
{
    loc = Mp4MetadataLocation {};

    std::vector<Mp4Atom> topLevel;

    if (! parseMp4Atoms (data, dataSize, 0, dataSize, topLevel, errorOut))
        return false;

    if (findMp4Atom (topLevel, "moof") != nullptr)
    {
        errorOut = "Fragmented MP4 files are not supported for tag access";
        return false;
    }

    const auto* moov = findMp4Atom (topLevel, "moov");

    if (moov == nullptr)
    {
        errorOut = "MP4 file has no moov atom";
        return false;
    }

    loc.moov = *moov;
    loc.hasMoov = true;

    std::vector<Mp4Atom> moovChildren;

    if (! parseMp4Atoms (data, dataSize, moov->contentStart(), moov->end(), moovChildren, errorOut))
    {
        errorOut = "Malformed MP4 moov atom: " + errorOut;
        return false;
    }

    if (const auto* udta = findMp4Atom (moovChildren, "udta"))
    {
        loc.udta = *udta;
        loc.hasUdta = true;

        std::vector<Mp4Atom> udtaChildren;

        if (! parseMp4Atoms (data, dataSize, udta->contentStart(), udta->end(), udtaChildren, errorOut))
        {
            errorOut = "Malformed MP4 udta atom: " + errorOut;
            return false;
        }

        if (const auto* meta = findMp4Atom (udtaChildren, "meta"))
        {
            loc.meta = *meta;
            loc.hasMeta = true;
        }
    }

    if (! loc.hasMeta)
    {
        if (const auto* meta = findMp4Atom (moovChildren, "meta"))
        {
            loc.meta = *meta;
            loc.hasMeta = true;
        }
    }

    if (! loc.hasMeta)
        return true;   // no metadata: valid, caller can create it

    std::vector<Mp4Atom> metaChildren;

    if (! parseMp4MetaChildren (data, dataSize, loc.meta, metaChildren, loc.metaHasVersionFlags, errorOut))
        return false;

    if (const auto* ilst = findMp4Atom (metaChildren, "ilst"))
    {
        loc.ilst = *ilst;
        loc.hasIlst = true;
    }

    return true;
}

struct Mp4Item
{
    std::string type;
    Bytes raw;   // full atom bytes
};

bool parseMp4IlstItems (const std::uint8_t* data, std::size_t dataSize, const Mp4Atom& ilst,
                        std::vector<Mp4Item>& items, juce::String& errorOut)
{
    items.clear();
    std::vector<Mp4Atom> atoms;

    if (! parseMp4Atoms (data, dataSize, ilst.contentStart(), ilst.end(), atoms, errorOut))
    {
        errorOut = "Malformed MP4 ilst atom: " + errorOut;
        return false;
    }

    for (const auto& atom : atoms)
    {
        Mp4Item item;
        item.type = atom.type;
        item.raw.assign (data + atom.start, data + atom.end());
        items.push_back (std::move (item));
    }

    return true;
}

struct Mp4DataAtom
{
    bool found = false;
    std::uint32_t dataType = 0;
    Bytes payload;
};

bool mp4FindDataAtom (const std::uint8_t* data, std::size_t dataSize, const Mp4Item& item, Mp4DataAtom& out,
                      juce::String& errorOut)
{
    out = Mp4DataAtom {};

    // Parse the item atom's content as child atoms.
    const auto* raw = item.raw.data();
    const std::size_t rawSize = item.raw.size();

    if (rawSize < 8)
        return true;

    std::vector<Mp4Atom> children;

    if (! parseMp4Atoms (raw, rawSize, 8, rawSize, children, errorOut))
        return false;

    for (const auto& child : children)
    {
        if (child.type != "data")
            continue;

        if (! inBounds (rawSize, child.contentStart(), 8))
        {
            errorOut = "Truncated MP4 data atom";
            return false;
        }

        out.found = true;
        out.dataType = readU32BE (raw + child.contentStart());
        out.payload.assign (raw + child.contentStart() + 8, raw + child.end());
        return true;
    }

    (void) data;
    (void) dataSize;
    return true;
}

bool mp4ParseFreeformItem (const Mp4Item& item, juce::String& meanOut, juce::String& nameOut,
                            Bytes& valueOut)
{
    meanOut.clear();
    nameOut.clear();
    valueOut.clear();

    if (item.raw.size() < 8)
        return false;

    std::vector<Mp4Atom> children;
    juce::String ignoredError;

    if (! parseMp4Atoms (item.raw.data(), item.raw.size(), 8, item.raw.size(), children, ignoredError))
        return false;

    for (const auto& child : children)
    {
        if (! inBounds (item.raw.size(), child.contentStart(), 4))
            continue;

        const auto contentSize = child.size - child.headerSize;

        if (child.type == "mean")
            meanOut = sanitiseText (stringFromUtf8 (item.raw.data() + child.contentStart() + 4,
                                                   contentSize - 4));
        else if (child.type == "name")
            nameOut = sanitiseText (stringFromUtf8 (item.raw.data() + child.contentStart() + 4,
                                                   contentSize - 4));
        else if (child.type == "data" && contentSize >= 8)
            valueOut.assign (item.raw.begin() + (std::ptrdiff_t) (child.contentStart() + 8),
                             item.raw.begin() + (std::ptrdiff_t) child.end());
    }

    return meanOut.isNotEmpty();
}

bool mp4IsReplayGainFreeformName (const juce::String& name)
{
    const auto upper = name.trim().toUpperCase();
    return upper.startsWith ("REPLAYGAIN_") || upper.startsWith ("R128_");
}

Bytes makeMp4FreeformItem (const char* mean, const char* name, const juce::String& value)
{
    // Keep the juce::String temporaries alive while their UTF-8 pointers are used.
    const juce::String meanString (mean);
    const juce::String nameString (name);

    Bytes meanContent;
    appendU32BE (meanContent, 0);
    appendBytes (meanContent, reinterpret_cast<const std::uint8_t*> (meanString.toRawUTF8()),
                 (std::size_t) meanString.getNumBytesAsUTF8());

    Bytes nameContent;
    appendU32BE (nameContent, 0);
    appendBytes (nameContent, reinterpret_cast<const std::uint8_t*> (nameString.toRawUTF8()),
                 (std::size_t) nameString.getNumBytesAsUTF8());

    Bytes dataContent;
    appendU32BE (dataContent, 0);   // implicit binary / UTF-8 text
    appendU32BE (dataContent, 0);   // locale
    appendBytes (dataContent, reinterpret_cast<const std::uint8_t*> (value.toRawUTF8()),
                 (std::size_t) value.getNumBytesAsUTF8());

    Bytes itemContent;
    appendBytes (itemContent, makeMp4Atom ("mean", meanContent));
    appendBytes (itemContent, makeMp4Atom ("name", nameContent));
    appendBytes (itemContent, makeMp4Atom ("data", dataContent));
    return makeMp4Atom ("----", itemContent);
}

void applyMp4FreeformReplayGain (const Mp4Item& item, TagData& tagsOut)
{
    juce::String mean, name;
    Bytes value;

    if (! mp4ParseFreeformItem (item, mean, name, value))
        return;

    if (! mean.equalsIgnoreCase ("com.apple.iTunes") && ! mean.equalsIgnoreCase ("com.apple.iTunes:replaygain"))
        return;

    applyTxxxReplayGain (name, stringFromUtf8 (value.data(), value.size()), tagsOut);
}

bool readMp4Tags (const std::uint8_t* data, std::size_t size, TagData& tagsOut,
                  std::vector<EmbeddedArtwork>* artworkOut, juce::String& errorOut)
{
    Mp4MetadataLocation loc;

    if (! locateMp4Metadata (data, size, loc, errorOut))
        return false;

    if (! loc.hasIlst)
        return true;

    std::vector<Mp4Item> items;

    if (! parseMp4IlstItems (data, size, loc.ilst, items, errorOut))
        return false;

    for (const auto& item : items)
    {
        Mp4DataAtom dataAtom;

        if (! mp4FindDataAtom (data, size, item, dataAtom, errorOut))
            return false;

        if (! dataAtom.found)
            continue;

        const auto text = [&dataAtom]
        {
            return sanitiseText (stringFromUtf8 (dataAtom.payload.data(), dataAtom.payload.size()));
        };

        if (item.type == mp4CopyTag ("nam"))
            setIfNotEmpty (tagsOut.title, text());
        else if (item.type == mp4CopyTag ("ART"))
            setIfNotEmpty (tagsOut.artist, text());
        else if (item.type == "aART")
            setIfNotEmpty (tagsOut.albumArtist, text());
        else if (item.type == mp4CopyTag ("alb"))
            setIfNotEmpty (tagsOut.album, text());
        else if (item.type == mp4CopyTag ("gen"))
            setIfNotEmpty (tagsOut.genre, text());
        else if (item.type == "gnre")
        {
            if (dataAtom.payload.size() >= 2)
            {
                const int index = (int) readU16BE (dataAtom.payload.data()) - 1;
                const auto name = id3GenreName (index);

                if (name.isNotEmpty())
                    setIfNotEmpty (tagsOut.genre, name);
            }
        }
        else if (item.type == mp4CopyTag ("day"))
        {
            const int year = parseLeadingInt (text());

            if (year > 0)
                tagsOut.year = year;
        }
        else if (item.type == "trkn")
        {
            if (dataAtom.payload.size() >= 4)
            {
                const int track = (int) readU16BE (dataAtom.payload.data() + 2);

                if (track > 0)
                    tagsOut.trackNumber = track;
            }
        }
        else if (item.type == "disk")
        {
            if (dataAtom.payload.size() >= 4)
            {
                const int disc = (int) readU16BE (dataAtom.payload.data() + 2);

                if (disc > 0)
                    tagsOut.discNumber = disc;
            }
        }
        else if (item.type == mp4CopyTag ("cmt"))
            setIfNotEmpty (tagsOut.comment, text());
        else if (item.type == mp4CopyTag ("wrt"))
            setIfNotEmpty (tagsOut.composer, text());
        else if (item.type == "----")
        {
            applyMp4FreeformReplayGain (item, tagsOut);
        }
        else if (item.type == "covr" && artworkOut != nullptr)
        {
            // covr may contain several data atoms (JPEG + PNG); re-parse the item.
            if (item.raw.size() >= 8)
            {
                std::vector<Mp4Atom> children;

                if (parseMp4Atoms (item.raw.data(), item.raw.size(), 8, item.raw.size(), children, errorOut))
                {
                    for (const auto& child : children)
                    {
                        if (child.type != "data" || ! inBounds (item.raw.size(), child.contentStart(), 8))
                            continue;

                        const auto kind = readU32BE (item.raw.data() + child.contentStart()) & 0xffffff;
                        EmbeddedArtwork artwork;
                        artwork.pictureType = 3;

                        const auto* imageData = item.raw.data() + child.contentStart() + 8;
                        const std::size_t imageSize = child.end() - child.contentStart() - 8;

                        if (kind == 13)
                            artwork.mimeType = "image/jpeg";
                        else if (kind == 14)
                            artwork.mimeType = "image/png";
                        else
                            artwork.mimeType = sniffImageMime (imageData, imageSize);

                        artwork.data.setSize (imageSize, false);

                        if (imageSize > 0)
                            std::memcpy (artwork.data.getData(), imageData, imageSize);

                        imageDimensions (imageData, imageSize, artwork.width, artwork.height);
                        artworkOut->push_back (std::move (artwork));
                    }
                }
            }
        }
    }

    if (artworkOut != nullptr)
        sortArtworkFrontCoverFirst (*artworkOut);

    return true;
}

/** Rebuilds a container atom with one child atom replaced (or appended). */
bool rebuildMp4Container (const std::uint8_t* data, std::size_t dataSize, const Mp4Atom& container,
                          const char* childType, const Bytes& newChild, bool hasVersionFlags, Bytes& out,
                          juce::String& errorOut)
{
    std::vector<Mp4Atom> children;
    const std::size_t childrenStart = container.contentStart() + (hasVersionFlags ? 4 : 0);

    if (! parseMp4Atoms (data, dataSize, childrenStart, container.end(), children, errorOut))
    {
        errorOut = "Malformed MP4 '" + juce::String (container.type.c_str()) + "' atom: " + errorOut;
        return false;
    }

    Bytes content;

    if (hasVersionFlags)
    {
        if (! inBounds (dataSize, container.contentStart(), 4))
        {
            errorOut = "Truncated MP4 meta version/flags field";
            return false;
        }

        appendBytes (content, data + container.contentStart(), 4);
    }

    bool replaced = false;

    for (const auto& child : children)
    {
        if (child.type == childType)
        {
            appendBytes (content, newChild);
            replaced = true;
        }
        else
        {
            appendBytes (content, data + child.start, child.size);
        }
    }

    if (! replaced)
        appendBytes (content, newChild);

    out = makeMp4Atom (container.type.c_str(), content);
    return true;
}

Bytes makeMp4TextItem (const std::string& itemType, const juce::String& value)
{
    Bytes payload;
    const auto utf8 = value.toRawUTF8();
    appendBytes (payload, reinterpret_cast<const std::uint8_t*> (utf8), std::strlen (utf8));

    Bytes dataContent;
    appendU32BE (dataContent, 1);   // data type: UTF-8 text
    appendU32BE (dataContent, 0);   // locale
    appendBytes (dataContent, payload);

    Bytes itemContent;
    appendBytes (itemContent, makeMp4Atom ("data", dataContent));
    return makeMp4Atom (itemType.c_str(), itemContent);
}

Bytes makeMp4BinaryItem (const char* itemType, const Bytes& payload)
{
    Bytes dataContent;
    appendU32BE (dataContent, 0);   // implicit binary
    appendU32BE (dataContent, 0);   // locale
    appendBytes (dataContent, payload);

    Bytes itemContent;
    appendBytes (itemContent, makeMp4Atom ("data", dataContent));
    return makeMp4Atom (itemType, itemContent);
}

Bytes makeMp4NumberItem (const char* itemType, int number, bool isDisc)
{
    Bytes payload (8, 0);
    payload[2] = (std::uint8_t) ((number >> 8) & 0xff);
    payload[3] = (std::uint8_t) (number & 0xff);
    (void) isDisc;
    return makeMp4BinaryItem (itemType, payload);
}

Bytes makeMp4ArtworkItem (const EmbeddedArtwork& artwork)
{
    juce::String mime = artwork.mimeType;

    if (mime.isEmpty())
        mime = sniffImageMime (static_cast<const std::uint8_t*> (artwork.data.getData()),
                               artwork.data.getSize());

    std::uint32_t kind = 0;

    if (mime.containsIgnoreCase ("jpeg") || mime.containsIgnoreCase ("jpg"))
        kind = 13;
    else if (mime.containsIgnoreCase ("png"))
        kind = 14;

    Bytes dataContent;
    appendU32BE (dataContent, kind);
    appendU32BE (dataContent, 0);
    appendBytes (dataContent, static_cast<const std::uint8_t*> (artwork.data.getData()),
                 artwork.data.getSize());

    Bytes itemContent;
    appendBytes (itemContent, makeMp4Atom ("data", dataContent));
    return makeMp4Atom ("covr", itemContent);
}

bool isMp4ReplaceableField (const std::string& type)
{
    return type == mp4CopyTag ("nam") || type == mp4CopyTag ("ART") || type == "aART"
           || type == mp4CopyTag ("alb") || type == mp4CopyTag ("gen") || type == "gnre"
           || type == mp4CopyTag ("day") || type == "trkn" || type == "disk"
           || type == mp4CopyTag ("cmt") || type == mp4CopyTag ("wrt");
}

Bytes buildMp4IlstBytes (const std::uint8_t* data, std::size_t dataSize, const Mp4MetadataLocation& loc,
                         const TagUpdate& update)
{
    Bytes ilstContent;

    const bool writeReplayGain = update.updateReplayGain
                                || (update.updateFields && update.fields != nullptr
                                    && hasAnyReplayGain (*update.fields));

    if (loc.hasIlst)
    {
        std::vector<Mp4Item> items;
        juce::String unusedError;

        if (parseMp4IlstItems (data, dataSize, loc.ilst, items, unusedError))
        {
            for (const auto& item : items)
            {
                if (update.updateFields && isMp4ReplaceableField (item.type))
                    continue;

                if (writeReplayGain && item.type == "----")
                {
                    juce::String mean, name;
                    Bytes value;

                    if (mp4ParseFreeformItem (item, mean, name, value)
                        && mp4IsReplayGainFreeformName (name))
                        continue;
                }

                if (update.updateArtwork && item.type == "covr")
                    continue;

                appendBytes (ilstContent, item.raw);
            }
        }
    }

    if (update.updateFields && update.fields != nullptr)
    {
        const auto& fields = *update.fields;

        if (fields.title.isNotEmpty())
            appendBytes (ilstContent, makeMp4TextItem (mp4CopyTag ("nam"), fields.title));

        if (fields.artist.isNotEmpty())
            appendBytes (ilstContent, makeMp4TextItem (mp4CopyTag ("ART"), fields.artist));

        if (fields.albumArtist.isNotEmpty())
            appendBytes (ilstContent, makeMp4TextItem ("aART", fields.albumArtist));

        if (fields.album.isNotEmpty())
            appendBytes (ilstContent, makeMp4TextItem (mp4CopyTag ("alb"), fields.album));

        if (fields.genre.isNotEmpty())
            appendBytes (ilstContent, makeMp4TextItem (mp4CopyTag ("gen"), fields.genre));

        if (fields.composer.isNotEmpty())
            appendBytes (ilstContent, makeMp4TextItem (mp4CopyTag ("wrt"), fields.composer));

        if (fields.year > 0)
            appendBytes (ilstContent, makeMp4TextItem (mp4CopyTag ("day"), juce::String (fields.year)));

        if (fields.trackNumber > 0)
            appendBytes (ilstContent, makeMp4NumberItem ("trkn", fields.trackNumber, false));

        if (fields.discNumber > 0)
            appendBytes (ilstContent, makeMp4NumberItem ("disk", fields.discNumber, true));

        if (fields.comment.isNotEmpty())
            appendBytes (ilstContent, makeMp4TextItem (mp4CopyTag ("cmt"), fields.comment));
    }

    if (writeReplayGain && update.fields != nullptr)
    {
        const auto& fields = *update.fields;

        if (fields.hasReplayGainTrack)
            appendBytes (ilstContent, makeMp4FreeformItem ("com.apple.iTunes", "REPLAYGAIN_TRACK_GAIN",
                                                          formatGainDb (fields.replayGainTrackDb)));

        if (fields.hasReplayGainAlbum)
            appendBytes (ilstContent, makeMp4FreeformItem ("com.apple.iTunes", "REPLAYGAIN_ALBUM_GAIN",
                                                          formatGainDb (fields.replayGainAlbumDb)));

        if (fields.hasPeak)
            appendBytes (ilstContent, makeMp4FreeformItem ("com.apple.iTunes", "REPLAYGAIN_TRACK_PEAK",
                                                          formatPeak (fields.peakLinear)));
    }

    if (update.updateArtwork && update.artwork != nullptr && update.artwork->data.getSize() > 0)
        appendBytes (ilstContent, makeMp4ArtworkItem (*update.artwork));

    return makeMp4Atom ("ilst", ilstContent);
}

/** Builds a new moov atom containing the updated ilst. */
bool buildUpdatedMoov (const std::uint8_t* data, std::size_t dataSize, const Mp4MetadataLocation& loc,
                       const TagUpdate& update, Bytes& newMoovOut, juce::String& errorOut)
{
    const Bytes newIlst = buildMp4IlstBytes (data, dataSize, loc, update);

    Bytes newMeta;

    if (loc.hasMeta)
    {
        // Preserve the meta version/flags plus every child except ilst.
        std::vector<Mp4Atom> metaChildren;
        bool metaHasVersionFlags = loc.metaHasVersionFlags;

        if (! parseMp4MetaChildren (data, dataSize, loc.meta, metaChildren, metaHasVersionFlags, errorOut))
            return false;

        Bytes metaContent;

        if (metaHasVersionFlags)
            appendBytes (metaContent, data + loc.meta.contentStart(), 4);

        for (const auto& child : metaChildren)
            if (child.type != "ilst")
                appendBytes (metaContent, data + child.start, child.size);

        appendBytes (metaContent, newIlst);
        newMeta = makeMp4Atom ("meta", metaContent);
    }
    else
    {
        Bytes metaContent;
        appendU32BE (metaContent, 0);   // version/flags
        appendBytes (metaContent, makeMp4MetaHandler());
        appendBytes (metaContent, newIlst);
        newMeta = makeMp4Atom ("meta", metaContent);
    }

    Bytes newUdta;

    if (loc.hasUdta)
    {
        if (! rebuildMp4Container (data, dataSize, loc.udta, "meta", newMeta, false, newUdta, errorOut))
            return false;
    }
    else
    {
        newUdta = makeMp4Atom ("udta", newMeta);
    }

    return rebuildMp4Container (data, dataSize, loc.moov, "udta", newUdta, false, newMoovOut, errorOut);
}

struct Mp4TopLevelLayout
{
    std::vector<Mp4Atom> atoms;
    std::size_t oldContentStart = 0;
    std::size_t newContentStart = 0;
};

bool patchMp4ChunkOffsets (Bytes& file, std::size_t start, std::size_t end,
                           const std::vector<Mp4TopLevelLayout>& layout, juce::String& errorOut);

bool patchMp4ChunkOffsetAtom (Bytes& file, const Mp4Atom& atom,
                              const std::vector<Mp4TopLevelLayout>& layout, bool is64Bit,
                              juce::String& errorOut)
{
    const std::size_t entrySize = is64Bit ? 8 : 4;
    const std::size_t contentOffset = atom.contentStart();

    if (! inBounds (file.size(), contentOffset, 8))
    {
        errorOut = "Truncated MP4 chunk offset table";
        return false;
    }

    const std::uint32_t entryCount = readU32BE (file.data() + contentOffset + 4);

    if ((std::uint64_t) entryCount * entrySize > file.size() - contentOffset - 8)
    {
        errorOut = "Malformed MP4 chunk offset table (entry count exceeds atom)";
        return false;
    }

    auto mapOffset = [&layout] (std::uint64_t oldOffset, std::uint64_t& newOffset) -> bool
    {
        for (const auto& entry : layout)
        {
            const auto& top = entry.atoms[0];

            if (oldOffset >= (std::uint64_t) entry.oldContentStart
                && oldOffset < (std::uint64_t) top.size + (std::uint64_t) top.start)
            {
                newOffset = (std::uint64_t) entry.newContentStart
                            + (oldOffset - (std::uint64_t) entry.oldContentStart);
                return true;
            }
        }

        return false;
    };

    std::size_t entryOffset = contentOffset + 8;

    for (std::uint32_t i = 0; i < entryCount; ++i)
    {
        const std::uint64_t oldOffset = is64Bit ? readU64BE (file.data() + entryOffset)
                                                : (std::uint64_t) readU32BE (file.data() + entryOffset);

        std::uint64_t newOffset = oldOffset;

        if (mapOffset (oldOffset, newOffset))
        {
            if (! is64Bit && newOffset > 0xffffffffULL)
            {
                errorOut = "MP4 media offset no longer fits in a 32-bit stco entry";
                return false;
            }

            if (is64Bit)
            {
                for (int b = 0; b < 8; ++b)
                    file[entryOffset + (std::size_t) b] = (std::uint8_t) ((newOffset >> (8 * (7 - b))) & 0xff);
            }
            else
            {
                const auto v = (std::uint32_t) newOffset;

                for (int b = 0; b < 4; ++b)
                    file[entryOffset + (std::size_t) b] = (std::uint8_t) ((v >> (8 * (3 - b))) & 0xff);
            }
        }

        entryOffset += entrySize;
    }

    return true;
}

bool patchMp4ChunkOffsets (Bytes& file, std::size_t start, std::size_t end,
                           const std::vector<Mp4TopLevelLayout>& layout, juce::String& errorOut)
{
    std::vector<Mp4Atom> children;

    if (! parseMp4Atoms (file.data(), file.size(), start, end, children, errorOut))
        return false;

    static const char* const containers[] = { "trak", "mdia", "minf", "stbl" };

    for (const auto& child : children)
    {
        if (child.type == "stco")
        {
            if (! patchMp4ChunkOffsetAtom (file, child, layout, false, errorOut))
                return false;
        }
        else if (child.type == "co64")
        {
            if (! patchMp4ChunkOffsetAtom (file, child, layout, true, errorOut))
                return false;
        }
        else
        {
            for (const auto* container : containers)
                if (child.type == container)
                    if (! patchMp4ChunkOffsets (file, child.contentStart(), child.end(), layout, errorOut))
                        return false;
        }
    }

    return true;
}

bool rewriteMp4File (const Bytes& fileData, const Bytes& newMoov, Bytes& out, juce::String& errorOut)
{
    std::vector<Mp4Atom> topLevel;

    if (! parseMp4Atoms (fileData.data(), fileData.size(), 0, fileData.size(), topLevel, errorOut))
        return false;

    std::size_t moovCount = 0;

    for (const auto& atom : topLevel)
        if (atom.type == "moov")
            ++moovCount;

    if (moovCount != 1)
    {
        errorOut = "Expected exactly one MP4 moov atom, found " + juce::String ((int) moovCount);
        return false;
    }

    std::vector<Mp4TopLevelLayout> layout;
    std::size_t newPosition = 0;

    for (const auto& atom : topLevel)
    {
        Mp4TopLevelLayout entry;
        entry.atoms.push_back (atom);
        entry.oldContentStart = atom.contentStart();
        entry.newContentStart = newPosition + atom.headerSize;
        layout.push_back (entry);
        newPosition += atom.type == "moov" ? newMoov.size() : atom.size;
    }

    out.clear();
    out.reserve (newPosition);

    for (const auto& atom : topLevel)
    {
        if (atom.type == "moov")
            appendBytes (out, newMoov);
        else
            appendBytes (out, fileData.data() + atom.start, atom.size);
    }

    // Patch stco/co64 tables inside the new moov. Because the moov contains
    // absolute file offsets, adding/removing bytes from it can shift mdat.
    std::size_t newMoovStart = 0;

    for (const auto& entry : layout)
    {
        if (entry.atoms[0].type == "moov")
        {
            newMoovStart = entry.newContentStart - entry.atoms[0].headerSize;
            break;
        }
    }

    if (! patchMp4ChunkOffsets (out, newMoovStart + 8, newMoovStart + newMoov.size(), layout, errorOut))
        return false;

    return true;
}

/** Returns the concatenated payload of every top-level mdat atom. */
bool extractMp4Payload (const std::uint8_t* data, std::size_t size, Bytes& out, juce::String& errorOut)
{
    std::vector<Mp4Atom> topLevel;

    if (! parseMp4Atoms (data, size, 0, size, topLevel, errorOut))
        return false;

    out.clear();

    for (const auto& atom : topLevel)
        if (atom.type == "mdat")
            appendBytes (out, data + atom.contentStart(), atom.size - atom.headerSize);

    if (out.empty())
    {
        errorOut = "MP4 file contains no mdat (media data) payload";
        return false;
    }

    return true;
}

bool writeMp4 (const juce::File& file, const Bytes& fileData, const TagUpdate& update, juce::String& errorOut)
{
    Mp4MetadataLocation loc;

    if (! locateMp4Metadata (fileData.data(), fileData.size(), loc, errorOut))
        return false;

    Bytes newMoov;

    if (! buildUpdatedMoov (fileData.data(), fileData.size(), loc, update, newMoov, errorOut))
        return false;

    Bytes output;

    if (! rewriteMp4File (fileData, newMoov, output, errorOut))
        return false;

    // Verify the result is structurally sound before replacing the original.
    Mp4MetadataLocation verify;
    juce::String verifyError;

    if (! locateMp4Metadata (output.data(), output.size(), verify, verifyError) || ! verify.hasMoov)
    {
        errorOut = "Internal error: rebuilt MP4 failed validation: " + verifyError;
        return false;
    }

    return writeFileAtomically (file, output, errorOut);
}

//==============================================================================
// RIFF chunk helpers (shared by WAV and, with big-endian sizes, AIFF)
//==============================================================================

struct RiffChunk
{
    std::string id;
    std::size_t start = 0;
    std::size_t size = 0;
    bool bigEndian = false;

    std::size_t contentStart() const noexcept { return start + 8; }
    std::size_t end() const noexcept { return start + 8 + size; }
    std::size_t paddedEnd() const noexcept { return end() + (size & 1); }
};

bool parseRiffChunks (const std::uint8_t* data, std::size_t dataSize, std::size_t start, std::size_t end,
                      bool bigEndian, std::vector<RiffChunk>& out, juce::String& errorOut,
                      const juce::String& what)
{
    out.clear();
    std::size_t pos = start;

    while (pos + 8 <= end)
    {
        RiffChunk chunk;
        chunk.id.assign (reinterpret_cast<const char*> (data + pos), 4);
        chunk.start = pos;
        chunk.size = bigEndian ? (std::size_t) readU32BE (data + pos + 4)
                               : (std::size_t) readU32LE (data + pos + 4);
        chunk.bigEndian = bigEndian;

        if (! inBounds (dataSize, pos + 8, chunk.size))
        {
            errorOut = "Truncated " + what + " chunk '" + juce::String (chunk.id.c_str()) + "'";
            return false;
        }

        out.push_back (chunk);
        pos = chunk.paddedEnd();

        if (pos > dataSize)
            pos = dataSize;
    }

    if (pos < end)
    {
        for (std::size_t i = pos; i < end; ++i)
        {
            if (data[i] != 0)
            {
                errorOut = "Trailing garbage in " + what + " chunk stream";
                return false;
            }
        }
    }

    return true;
}

juce::String riffReadString (const std::uint8_t* data, std::size_t size)
{
    std::size_t length = size;

    while (length > 0 && data[length - 1] == 0)
        --length;

    return sanitiseText (stringFromUtf8 (data, length));
}

Bytes makeRiffChunk (const char* id, const std::uint8_t* payload, std::size_t payloadSize, bool bigEndian,
                     bool addStringTerminator = false)
{
    Bytes chunk;
    const std::size_t contentSize = payloadSize + (addStringTerminator ? 1 : 0);
    appendAscii (chunk, id);

    if (bigEndian)
        appendU32BE (chunk, (std::uint32_t) contentSize);
    else
        appendU32LE (chunk, (std::uint32_t) contentSize);

    appendBytes (chunk, payload, payloadSize);

    if (addStringTerminator)
        chunk.push_back (0);

    if ((contentSize & 1) != 0)
        chunk.push_back (0);

    return chunk;
}

//==============================================================================
// ID3v2-at-start containers (MP3 and ADTS AAC)
//==============================================================================

bool findLeadingId3Region (const std::uint8_t* data, std::size_t size, Id3TagInfo& firstTag, bool& found,
                           std::size_t& regionEnd, juce::String& errorOut)
{
    found = false;
    firstTag = Id3TagInfo {};
    regionEnd = 0;

    while (true)
    {
        Id3TagInfo tag;
        bool tagFound = false;

        if (! parseId3Tag (data, size, regionEnd, tag, errorOut, tagFound))
            return false;

        if (! tagFound)
            break;

        if (! found)
        {
            firstTag = tag;
            found = true;
        }

        regionEnd += tag.totalBytes;
    }

    return true;
}

bool readId3AtStartTags (const std::uint8_t* data, std::size_t size, TagData& tagsOut,
                         std::vector<EmbeddedArtwork>* artworkOut, juce::String& errorOut)
{
    Id3TagInfo tag;
    bool found = false;
    std::size_t regionEnd = 0;

    if (! findLeadingId3Region (data, size, tag, found, regionEnd, errorOut))
        return false;

    if (found)
    {
        applyId3Frames (tag, tagsOut);

        if (artworkOut != nullptr)
        {
            for (const auto& frame : tag.frames)
            {
                if (frame.id != "APIC")
                    continue;

                EmbeddedArtwork artwork;

                if (id3ParseApicFrame (frame.payload, artwork))
                    artworkOut->push_back (std::move (artwork));
            }

            sortArtworkFrontCoverFirst (*artworkOut);
            return true;
        }

        return true;
    }

    // No ID3 tag: make sure this at least looks like MPEG audio / ADTS so a
    // mislabelled text file is reported instead of silently accepted.
    const std::size_t scanLimit = std::min<std::size_t> (size, 16384);
    bool foundSync = false;

    for (std::size_t i = 0; i + 1 < scanLimit; ++i)
    {
        if (data[i] == 0xff && (data[i + 1] & 0xe0) == 0xe0)
        {
            foundSync = true;
            break;
        }
    }

    if (! foundSync)
    {
        errorOut = "Not an MPEG audio stream (no frame sync found)";
        return false;
    }

    return true;
}

bool writeId3AtStart (const juce::File& file, const Bytes& fileData, const TagUpdate& update,
                      juce::String& errorOut)
{
    Id3TagInfo existing;
    bool found = false;
    std::size_t regionEnd = 0;

    if (! findLeadingId3Region (fileData.data(), fileData.size(), existing, found, regionEnd, errorOut))
        return false;

    if (! found)
    {
        const std::size_t scanLimit = std::min<std::size_t> (fileData.size(), 16384);
        bool foundSync = false;

        for (std::size_t i = 0; i + 1 < scanLimit; ++i)
        {
            if (fileData[i] == 0xff && (fileData[i + 1] & 0xe0) == 0xe0)
            {
                foundSync = true;
                break;
            }
        }

        if (! foundSync)
        {
            errorOut = "Not an MPEG audio stream (no frame sync found); refusing to write an ID3 tag";
            return false;
        }
    }

    const Bytes newTag = buildUpdatedId3Tag (found ? &existing : nullptr, update, errorOut);

    if (! errorOut.isEmpty())
        return false;

    Bytes out;
    out.reserve (newTag.size() + fileData.size() - regionEnd);
    appendBytes (out, newTag);
    appendBytes (out, fileData.data() + regionEnd, fileData.size() - regionEnd);
    return writeFileAtomically (file, out, errorOut);
}

//==============================================================================
// WAV (RIFF/WAVE + LIST INFO + id3 chunk)
//==============================================================================

struct WavInfoEntry
{
    std::string id;
    Bytes value;
};

void applyWavInfoField (const std::string& id, const juce::String& value, TagData& tags)
{
    if (id == "INAM")            setIfNotEmpty (tags.title, value);
    else if (id == "IART")       setIfNotEmpty (tags.artist, value);
    else if (id == "IPRD")       setIfNotEmpty (tags.album, value);
    else if (id == "IGNR")       setIfNotEmpty (tags.genre, value);
    else if (id == "ICMT")       setIfNotEmpty (tags.comment, value);
    else if (id == "IAAR")       setIfNotEmpty (tags.albumArtist, value);
    else if (id == "ICRD")
    {
        const int year = parseLeadingInt (value);

        if (year > 0)
            tags.year = year;
    }
    else if (id == "ITRK" || id == "IPRT")
    {
        const int track = parseLeadingInt (value);

        if (track > 0)
            tags.trackNumber = track;
    }
}

bool readWavTags (const std::uint8_t* data, std::size_t size, TagData& tagsOut,
                  std::vector<EmbeddedArtwork>* artworkOut, juce::String& errorOut)
{
    if (size < 12 || ! fourCCEquals (data, "RIFF") || ! fourCCEquals (data + 8, "WAVE"))
    {
        errorOut = "Not a RIFF/WAVE file";
        return false;
    }

    std::vector<RiffChunk> chunks;

    if (! parseRiffChunks (data, size, 12, size, false, chunks, errorOut, "WAV"))
        return false;

    Id3TagInfo id3Tag;
    bool haveId3 = false;

    for (const auto& chunk : chunks)
    {
        if (chunk.id == "LIST" && chunk.size >= 4 && fourCCEquals (data + chunk.contentStart(), "INFO"))
        {
            std::vector<RiffChunk> infoChunks;

            if (! parseRiffChunks (data, size, chunk.contentStart() + 4, chunk.end(), false, infoChunks,
                                   errorOut, "WAV INFO"))
                return false;

            for (const auto& entry : infoChunks)
                applyWavInfoField (entry.id, riffReadString (data + entry.contentStart(), entry.size), tagsOut);
        }
        else if ((chunk.id == "id3 " || chunk.id == "ID3 ") && ! haveId3)
        {
            juce::String id3Error;

            if (! readEmbeddedId3Tags (data, size, chunk.contentStart(), haveId3, id3Tag, tagsOut, id3Error))
                return false;
        }
    }

    if (haveId3 && artworkOut != nullptr)
    {
        for (const auto& frame : id3Tag.frames)
        {
            if (frame.id != "APIC")
                continue;

            EmbeddedArtwork artwork;

            if (id3ParseApicFrame (frame.payload, artwork))
                artworkOut->push_back (std::move (artwork));
        }

        sortArtworkFrontCoverFirst (*artworkOut);
    }

    return true;
}

Bytes buildUpdatedWavInfoChunk (const std::uint8_t* data, std::size_t dataSize, const RiffChunk& listChunk,
                                const WavInfoEntry& unusedEntry, const TagData* fields, bool updateFields,
                                juce::String& errorOut)
{
    (void) unusedEntry;

    std::vector<WavInfoEntry> entries;
    std::vector<RiffChunk> infoChunks;

    if (! parseRiffChunks (data, dataSize, listChunk.contentStart() + 4, listChunk.end(), false, infoChunks,
                           errorOut, "WAV INFO"))
        return {};

    for (const auto& chunk : infoChunks)
    {
        WavInfoEntry entry;
        entry.id = chunk.id;
        entry.value.assign (data + chunk.contentStart(), data + chunk.contentStart() + chunk.size);
        entries.push_back (std::move (entry));
    }

    if (updateFields && fields != nullptr)
    {
        auto findEntry = [&entries] (const char* id) -> WavInfoEntry*
        {
            for (auto& entry : entries)
                if (entry.id == id)
                    return &entry;

            return nullptr;
        };

        auto makeValue = [] (const juce::String& value) -> Bytes
        {
            const auto utf8 = value.toRawUTF8();
            Bytes bytes (reinterpret_cast<const std::uint8_t*> (utf8),
                         reinterpret_cast<const std::uint8_t*> (utf8) + std::strlen (utf8));
            bytes.push_back (0);
            return bytes;
        };

        auto setEntry = [&] (const char* id, const juce::String& value)
        {
            if (value.isEmpty())
                return;

            if (auto* entry = findEntry (id))
                entry->value = makeValue (value);
            else
                entries.push_back ({ id, makeValue (value) });
        };

        // ffmpeg writes the track number as IPRT; Windows tools use ITRK.
        // Update whichever exists, create ITRK otherwise.
        auto setTrackEntry = [&] (const juce::String& value)
        {
            if (value.isEmpty())
                return;

            if (auto* entry = findEntry ("ITRK"))
                entry->value = makeValue (value);
            else if (auto* iprt = findEntry ("IPRT"))
                iprt->value = makeValue (value);
            else
                entries.push_back ({ "ITRK", makeValue (value) });
        };

        const auto& tags = *fields;
        setEntry ("INAM", tags.title);
        setEntry ("IART", tags.artist);
        setEntry ("IPRD", tags.album);
        setEntry ("IGNR", tags.genre);
        setEntry ("ICMT", tags.comment);

        if (tags.albumArtist.isNotEmpty())
            setEntry ("IAAR", tags.albumArtist);

        if (tags.year > 0)
            setEntry ("ICRD", juce::String (tags.year));

        if (tags.trackNumber > 0)
            setTrackEntry (formatTrackNumber (tags.trackNumber));
    }

    Bytes content;
    appendAscii (content, "INFO");

    for (const auto& entry : entries)
    {
        Bytes chunk = makeRiffChunk (entry.id.c_str(), entry.value.data(), entry.value.size(), false, false);

        // makeRiffChunk adds a terminator only on request; INFO values already
        // carry their own trailing NUL byte.
        appendBytes (content, chunk);
    }

    Bytes list;
    appendAscii (list, "LIST");
    appendU32LE (list, (std::uint32_t) content.size());
    appendBytes (list, content);
    return list;
}

Bytes buildId3Chunk (const Bytes& tag, bool bigEndian)
{
    const std::size_t contentSize = tag.size();
    Bytes chunk;
    appendAscii (chunk, bigEndian ? "ID3 " : "id3 ");

    if (bigEndian)
        appendU32BE (chunk, (std::uint32_t) contentSize);
    else
        appendU32LE (chunk, (std::uint32_t) contentSize);

    appendBytes (chunk, tag);

    if ((contentSize & 1) != 0)
        chunk.push_back (0);

    return chunk;
}

bool writeWav (const juce::File& file, const Bytes& fileData, const TagUpdate& update, juce::String& errorOut)
{
    if (fileData.size() < 12 || ! fourCCEquals (fileData.data(), "RIFF")
        || ! fourCCEquals (fileData.data() + 8, "WAVE"))
    {
        errorOut = "Not a RIFF/WAVE file";
        return false;
    }

    std::vector<RiffChunk> chunks;

    if (! parseRiffChunks (fileData.data(), fileData.size(), 12, fileData.size(), false, chunks, errorOut, "WAV"))
        return false;

    Id3TagInfo existingId3;
    bool haveId3 = false;
    bool fileHasId3 = false;
    const TagData* fields = update.fields;

    for (const auto& chunk : chunks)
    {
        if (chunk.id == "id3 " || chunk.id == "ID3 ")
        {
            fileHasId3 = true;

            if (! haveId3)
            {
                juce::String id3Error;

                if (! parseId3Tag (fileData.data(), fileData.size(), chunk.contentStart(), existingId3, id3Error,
                                   haveId3))
                {
                    errorOut = "Existing WAV id3 chunk is malformed: " + id3Error;
                    return false;
                }
            }
        }
    }

    Bytes newId3Tag = buildUpdatedId3Tag (haveId3 ? &existingId3 : nullptr, update, errorOut);

    if (! errorOut.isEmpty())
        return false;

    const Bytes newId3Chunk = buildId3Chunk (newId3Tag, false);

    Bytes newInfoChunk;
    bool fileHasInfo = false;
    WavInfoEntry placeholder;

    for (const auto& chunk : chunks)
    {
        if (chunk.id == "LIST" && chunk.size >= 4 && fourCCEquals (fileData.data() + chunk.contentStart(), "INFO"))
        {
            fileHasInfo = true;
            newInfoChunk = buildUpdatedWavInfoChunk (fileData.data(), fileData.size(), chunk, placeholder,
                                                     fields, update.updateFields, errorOut);

            if (! errorOut.isEmpty())
                return false;

            break;
        }
    }

    (void) fileHasInfo;

    std::vector<Bytes> pieces;
    bool id3Written = false;

    for (const auto& chunk : chunks)
    {
        if (chunk.id == "id3 " || chunk.id == "ID3 ")
        {
            if (! id3Written)
            {
                pieces.push_back (newId3Chunk);
                id3Written = true;
            }

            continue;
        }

        if (chunk.id == "LIST" && chunk.size >= 4
            && fourCCEquals (fileData.data() + chunk.contentStart(), "INFO") && ! newInfoChunk.empty())
        {
            pieces.push_back (newInfoChunk);
            continue;
        }

        Bytes rawChunk (fileData.data() + chunk.start,
                        fileData.data() + std::min (chunk.paddedEnd(), fileData.size()));
        pieces.push_back (std::move (rawChunk));

        if (! fileHasId3 && chunk.id == "fmt " && ! id3Written)
        {
            pieces.push_back (newId3Chunk);
            id3Written = true;
        }
    }

    if (! id3Written)
        pieces.push_back (newId3Chunk);

    std::size_t total = 12;

    for (const auto& piece : pieces)
        total += piece.size();

    Bytes out;
    out.reserve (total);
    appendAscii (out, "RIFF");
    appendU32LE (out, (std::uint32_t) (total - 8));
    appendAscii (out, "WAVE");

    for (const auto& piece : pieces)
        appendBytes (out, piece);

    return writeFileAtomically (file, out, errorOut);
}

//==============================================================================
// AIFF (FORM/AIFF + ID3 chunk)
//==============================================================================

bool validateAiff (const std::uint8_t* data, std::size_t size, juce::String& errorOut)
{
    if (size < 12 || ! fourCCEquals (data, "FORM")
        || (! fourCCEquals (data + 8, "AIFF") && ! fourCCEquals (data + 8, "AIFC")))
    {
        errorOut = "Not an AIFF file";
        return false;
    }

    return true;
}

bool readAiffTags (const std::uint8_t* data, std::size_t size, TagData& tagsOut,
                   std::vector<EmbeddedArtwork>* artworkOut, juce::String& errorOut)
{
    if (! validateAiff (data, size, errorOut))
        return false;

    std::vector<RiffChunk> chunks;

    if (! parseRiffChunks (data, size, 12, size, true, chunks, errorOut, "AIFF"))
        return false;

    Id3TagInfo id3Tag;
    bool haveId3 = false;
    TagData id3Fields;

    for (const auto& chunk : chunks)
    {
        if (chunk.id == "NAME")
            setIfNotEmpty (tagsOut.title, riffReadString (data + chunk.contentStart(), chunk.size));
        else if (chunk.id == "AUTH")
            setIfNotEmpty (tagsOut.artist, riffReadString (data + chunk.contentStart(), chunk.size));
        else if (chunk.id == "ANNO")
            setIfNotEmpty (tagsOut.comment, riffReadString (data + chunk.contentStart(), chunk.size));
        else if (chunk.id == "ID3 " && ! haveId3)
        {
            juce::String id3Error;

            if (! readEmbeddedId3Tags (data, size, chunk.contentStart(), haveId3, id3Tag, id3Fields, id3Error))
                return false;
        }
    }

    mergeNonEmpty (tagsOut, id3Fields);

    if (haveId3 && artworkOut != nullptr)
    {
        for (const auto& frame : id3Tag.frames)
        {
            if (frame.id != "APIC")
                continue;

            EmbeddedArtwork artwork;

            if (id3ParseApicFrame (frame.payload, artwork))
                artworkOut->push_back (std::move (artwork));
        }

        sortArtworkFrontCoverFirst (*artworkOut);
    }

    return true;
}

bool writeAiff (const juce::File& file, const Bytes& fileData, const TagUpdate& update, juce::String& errorOut)
{
    if (! validateAiff (fileData.data(), fileData.size(), errorOut))
        return false;

    std::vector<RiffChunk> chunks;

    if (! parseRiffChunks (fileData.data(), fileData.size(), 12, fileData.size(), true, chunks, errorOut, "AIFF"))
        return false;

    Id3TagInfo existingId3;
    bool haveId3 = false;

    for (const auto& chunk : chunks)
    {
        if (chunk.id == "ID3 " && ! haveId3)
        {
            juce::String id3Error;

            if (! parseId3Tag (fileData.data(), fileData.size(), chunk.contentStart(), existingId3, id3Error,
                               haveId3))
            {
                errorOut = "Existing AIFF ID3 chunk is malformed: " + id3Error;
                return false;
            }
        }
    }

    const Bytes newId3Tag = buildUpdatedId3Tag (haveId3 ? &existingId3 : nullptr, update, errorOut);

    if (! errorOut.isEmpty())
        return false;

    const Bytes newId3Chunk = buildId3Chunk (newId3Tag, true);
    const TagData* fields = update.fields;

    std::vector<Bytes> pieces;
    bool id3Written = false;
    bool nameWritten = false;
    bool authWritten = false;
    bool annoWritten = false;

    auto makeTextField = [&] (const char* id, const juce::String& value) -> Bytes
    {
        Bytes payload;

        if (value.isNotEmpty())
        {
            const auto utf8 = value.toRawUTF8();
            appendBytes (payload, reinterpret_cast<const std::uint8_t*> (utf8), std::strlen (utf8));
        }

        return makeRiffChunk (id, payload.data(), payload.size(), true, false);
    };

    for (const auto& chunk : chunks)
    {
        if (chunk.id == "ID3 ")
        {
            if (! id3Written)
            {
                pieces.push_back (newId3Chunk);
                id3Written = true;
            }

            continue;
        }

        if (update.updateFields && fields != nullptr)
        {
            if (chunk.id == "NAME" && fields->title.isNotEmpty())
            {
                pieces.push_back (makeTextField ("NAME", fields->title));
                nameWritten = true;
                continue;
            }

            if (chunk.id == "AUTH" && fields->artist.isNotEmpty())
            {
                pieces.push_back (makeTextField ("AUTH", fields->artist));
                authWritten = true;
                continue;
            }

            if (chunk.id == "ANNO" && fields->comment.isNotEmpty())
            {
                pieces.push_back (makeTextField ("ANNO", fields->comment));
                annoWritten = true;
                continue;
            }
        }

        Bytes rawChunk (fileData.data() + chunk.start,
                        fileData.data() + std::min (chunk.paddedEnd(), fileData.size()));
        pieces.push_back (std::move (rawChunk));
    }

    if (! id3Written)
        pieces.push_back (newId3Chunk);

    if (update.updateFields && fields != nullptr)
    {
        if (! nameWritten && fields->title.isNotEmpty())
            pieces.insert (pieces.begin(), makeTextField ("NAME", fields->title));

        if (! authWritten && fields->artist.isNotEmpty())
            pieces.insert (pieces.begin() + 1, makeTextField ("AUTH", fields->artist));

        if (! annoWritten && fields->comment.isNotEmpty())
            pieces.push_back (makeTextField ("ANNO", fields->comment));
    }

    std::size_t total = 12;

    for (const auto& piece : pieces)
        total += piece.size();

    Bytes out;
    out.reserve (total);
    appendAscii (out, "FORM");
    appendU32BE (out, (std::uint32_t) (total - 8));
    appendBytes (out, fileData.data() + 8, 4);   // "AIFF" / "AIFC"

    for (const auto& piece : pieces)
        appendBytes (out, piece);

    return writeFileAtomically (file, out, errorOut);
}

//==============================================================================
// DSF / DFF (ID3v2 metadata blocks inside DSD containers)
//==============================================================================

struct DsfLayout
{
    std::uint64_t metadataOffset = 0;
    std::size_t audioEnd = 0;
    Bytes dataPayload;
};

bool parseDsfFile (const std::uint8_t* data, std::size_t size, DsfLayout& out, juce::String& errorOut)
{
    out = DsfLayout {};

    if (size < 28 || ! fourCCEquals (data, "DSD "))
    {
        errorOut = "Not a DSF file (missing DSD marker)";
        return false;
    }

    const auto headerSize = readU64LE (data + 4);
    const auto totalSize = readU64LE (data + 12);
    out.metadataOffset = readU64LE (data + 20);

    if (headerSize < 28 || headerSize > size)
    {
        errorOut = "Malformed DSF header size";
        return false;
    }

    if (totalSize > size)
    {
        errorOut = "Truncated DSF file (header declares " + juce::String ((juce::int64) totalSize) + " bytes, file has "
                   + juce::String ((int) size) + ")";
        return false;
    }

    std::size_t pos = (std::size_t) headerSize;

    while (pos + 12 <= size)
    {
        const std::string id (reinterpret_cast<const char*> (data + pos), 4);
        const auto chunkSize = readU64LE (data + pos + 4);

        if (chunkSize < 12 || chunkSize > size - pos)
        {
            errorOut = "Malformed DSF chunk '" + juce::String (id.c_str()) + "'";
            return false;
        }

        if (id == "data")
        {
            out.dataPayload.assign (data + pos + 12, data + pos + (std::size_t) chunkSize);
        }

        out.audioEnd = pos + (std::size_t) chunkSize;
        pos = out.audioEnd;

        if (id == "data")
            break;
    }

    if (out.audioEnd == 0)
    {
        errorOut = "DSF file has no data chunk";
        return false;
    }

    if (out.metadataOffset != 0 && (out.metadataOffset > size || out.metadataOffset < out.audioEnd))
    {
        errorOut = "Malformed DSF metadata pointer";
        return false;
    }

    return true;
}

bool readDsfTags (const std::uint8_t* data, std::size_t size, TagData& tagsOut,
                  std::vector<EmbeddedArtwork>* artworkOut, juce::String& errorOut)
{
    DsfLayout layout;

    if (! parseDsfFile (data, size, layout, errorOut))
        return false;

    if (layout.metadataOffset == 0)
        return true;

    Id3TagInfo tag;
    bool found = false;

    if (! readEmbeddedId3Tags (data, size, (std::size_t) layout.metadataOffset, found, tag, tagsOut, errorOut))
        return false;

    if (found && artworkOut != nullptr)
    {
        for (const auto& frame : tag.frames)
        {
            if (frame.id != "APIC")
                continue;

            EmbeddedArtwork artwork;

            if (id3ParseApicFrame (frame.payload, artwork))
                artworkOut->push_back (std::move (artwork));
        }

        sortArtworkFrontCoverFirst (*artworkOut);
    }

    return true;
}

bool writeDsf (const juce::File& file, const Bytes& fileData, const TagUpdate& update, juce::String& errorOut)
{
    DsfLayout layout;

    if (! parseDsfFile (fileData.data(), fileData.size(), layout, errorOut))
        return false;

    Id3TagInfo existingId3;
    bool haveId3 = false;

    if (layout.metadataOffset != 0)
    {
        juce::String id3Error;

        if (! parseId3Tag (fileData.data(), fileData.size(), (std::size_t) layout.metadataOffset, existingId3,
                           id3Error, haveId3))
        {
            errorOut = "Existing DSF metadata is malformed: " + id3Error;
            return false;
        }
    }

    const Bytes newTag = buildUpdatedId3Tag (haveId3 ? &existingId3 : nullptr, update, errorOut);

    if (! errorOut.isEmpty())
        return false;

    const std::size_t newTotal = layout.audioEnd + newTag.size();

    Bytes out;
    out.reserve (newTotal);
    appendBytes (out, fileData.data(), layout.audioEnd);
    appendBytes (out, newTag);

    // Patch the DSF header: total file size and metadata pointer.
    for (int i = 0; i < 8; ++i)
    {
        out[12 + (std::size_t) i] = (std::uint8_t) (((std::uint64_t) newTotal >> (8 * i)) & 0xff);
        out[20 + (std::size_t) i] = (std::uint8_t) (((std::uint64_t) layout.audioEnd >> (8 * i)) & 0xff);
    }

    return writeFileAtomically (file, out, errorOut);
}

bool readDffTags (const std::uint8_t* data, std::size_t size, TagData& tagsOut,
                  std::vector<EmbeddedArtwork>* artworkOut, juce::String& errorOut)
{
    if (size < 16 || ! fourCCEquals (data, "FRM8"))
    {
        errorOut = "Not a DFF (DSDIFF) file";
        return false;
    }

    std::size_t pos = 16;
    bool foundId3 = false;

    while (pos + 12 <= size)
    {
        const std::string id (reinterpret_cast<const char*> (data + pos), 4);
        const auto chunkSize = readU64BE (data + pos + 4);

        if (chunkSize > size - pos - 12)
        {
            errorOut = "Malformed DFF chunk '" + juce::String (id.c_str()) + "'";
            return false;
        }

        if (id == "ID3 " && ! foundId3)
        {
            Id3TagInfo tag;

            if (! readEmbeddedId3Tags (data, size, pos + 12, foundId3, tag, tagsOut, errorOut))
                return false;

            if (foundId3 && artworkOut != nullptr)
            {
                for (const auto& frame : tag.frames)
                {
                    if (frame.id != "APIC")
                        continue;

                    EmbeddedArtwork artwork;

                    if (id3ParseApicFrame (frame.payload, artwork))
                        artworkOut->push_back (std::move (artwork));
                }

                sortArtworkFrontCoverFirst (*artworkOut);
            }
        }

        pos += 12 + (std::size_t) chunkSize;

        if ((chunkSize & 1) != 0 && pos < size)
            ++pos;
    }

    return true;
}

bool writeDff (const juce::File& file, const Bytes& fileData, const TagUpdate& update, juce::String& errorOut)
{
    if (fileData.size() < 16 || ! fourCCEquals (fileData.data(), "FRM8"))
    {
        errorOut = "Not a DFF (DSDIFF) file";
        return false;
    }

    std::size_t pos = 16;
    bool haveId3 = false;
    Id3TagInfo existingId3;
    std::vector<Bytes> pieces;

    while (pos + 12 <= fileData.size())
    {
        const std::string id (reinterpret_cast<const char*> (fileData.data() + pos), 4);
        const auto chunkSize = readU64BE (fileData.data() + pos + 4);

        if (chunkSize > fileData.size() - pos - 12)
        {
            errorOut = "Malformed DFF chunk '" + juce::String (id.c_str()) + "'";
            return false;
        }

        std::size_t chunkEnd = pos + 12 + (std::size_t) chunkSize;

        if ((chunkSize & 1) != 0 && chunkEnd < fileData.size())
            ++chunkEnd;

        if (id == "ID3 ")
        {
            if (! haveId3)
            {
                juce::String id3Error;

                if (! parseId3Tag (fileData.data(), fileData.size(), pos + 12, existingId3, id3Error, haveId3))
                {
                    errorOut = "Existing DFF ID3 chunk is malformed: " + id3Error;
                    return false;
                }
            }

            pos = chunkEnd;
            continue;
        }

        pieces.emplace_back (fileData.data() + pos, fileData.data() + std::min (chunkEnd, fileData.size()));
        pos = chunkEnd;
    }

    const Bytes newTag = buildUpdatedId3Tag (haveId3 ? &existingId3 : nullptr, update, errorOut);

    if (! errorOut.isEmpty())
        return false;

    Bytes id3Chunk;
    appendAscii (id3Chunk, "ID3 ");
    appendU64BE (id3Chunk, (std::uint64_t) newTag.size());
    appendBytes (id3Chunk, newTag);

    if ((newTag.size() & 1) != 0)
        id3Chunk.push_back (0);

    pieces.push_back (id3Chunk);

    std::size_t bodySize = 0;

    for (const auto& piece : pieces)
        bodySize += piece.size();

    Bytes out;
    out.reserve (16 + bodySize);
    appendAscii (out, "FRM8");

    // Preserve the original FRM8 size convention while accounting for the change.
    const std::int64_t oldFormSize = (std::int64_t) readU64BE (fileData.data() + 4);
    const std::int64_t sizeDelta = (std::int64_t) bodySize
                                   - (std::int64_t) (fileData.size() >= 16 ? fileData.size() - 16 : 0);
    appendU64BE (out, (std::uint64_t) std::max<std::int64_t> (0, oldFormSize + sizeDelta));
    appendBytes (out, fileData.data() + 12, 4);   // "DSD "

    for (const auto& piece : pieces)
        appendBytes (out, piece);

    return writeFileAtomically (file, out, errorOut);
}

//==============================================================================
// Audio payload extraction
//==============================================================================

bool extractId3AtStartPayload (const std::uint8_t* data, std::size_t size, Bytes& out,
                               juce::String& errorOut)
{
    std::size_t start = 0;

    while (true)
    {
        Id3TagInfo tag;
        bool found = false;

        if (! parseId3Tag (data, size, start, tag, errorOut, found))
        {
            errorOut = "Malformed ID3v2 tag: " + errorOut;
            return false;
        }

        if (! found)
            break;

        start += tag.totalBytes;
    }

    std::size_t end = size;

    // Trailing ID3v1 tag.
    if (end >= 128 && data[end - 128] == 'T' && data[end - 127] == 'A' && data[end - 126] == 'G')
        end -= 128;

    // Trailing APEv2 tag (footer: "APETAGEX", version, size, item count, flags...).
    if (end >= 32 && std::memcmp (data + end - 32, "APETAGEX", 8) == 0)
    {
        const auto apeSize = (std::size_t) readU32LE (data + end - 32 + 12);
        const auto apeFlags = readU32LE (data + end - 32 + 20);
        std::size_t total = apeSize + ((apeFlags & 0x80000000u) != 0 ? 32 : 0);

        if (total <= end)
            end -= total;
    }

    if (end <= start)
    {
        errorOut = "No MPEG audio payload found after the ID3 tag";
        return false;
    }

    out.assign (data + start, data + end);
    return true;
}

bool extractWavPayload (const std::uint8_t* data, std::size_t size, Bytes& out, juce::String& errorOut)
{
    if (size < 12 || ! fourCCEquals (data, "RIFF") || ! fourCCEquals (data + 8, "WAVE"))
    {
        errorOut = "Not a RIFF/WAVE file";
        return false;
    }

    std::vector<RiffChunk> chunks;

    if (! parseRiffChunks (data, size, 12, size, false, chunks, errorOut, "WAV"))
        return false;

    out.clear();

    for (const auto& chunk : chunks)
        if (chunk.id == "data")
            appendBytes (out, data + chunk.contentStart(), chunk.size);

    if (out.empty())
    {
        errorOut = "WAV file has no data chunk";
        return false;
    }

    return true;
}

bool extractAiffPayload (const std::uint8_t* data, std::size_t size, Bytes& out, juce::String& errorOut)
{
    if (! validateAiff (data, size, errorOut))
        return false;

    std::vector<RiffChunk> chunks;

    if (! parseRiffChunks (data, size, 12, size, true, chunks, errorOut, "AIFF"))
        return false;

    for (const auto& chunk : chunks)
    {
        if (chunk.id != "SSND")
            continue;

        if (chunk.size < 8)
        {
            errorOut = "AIFF SSND chunk is too small";
            return false;
        }

        const auto offset = readU32BE (data + chunk.contentStart());

        if ((std::size_t) offset > chunk.size - 8)
        {
            errorOut = "AIFF SSND chunk has an invalid sample offset";
            return false;
        }

        const std::size_t dataStart = chunk.contentStart() + 8 + (std::size_t) offset;

        if (! inBounds (size, dataStart, chunk.size - 8 - (std::size_t) offset))
        {
            errorOut = "Truncated AIFF SSND chunk";
            return false;
        }

        out.assign (data + dataStart, data + dataStart + (chunk.size - 8 - (std::size_t) offset));
        return true;
    }

    errorOut = "AIFF file has no SSND chunk";
    return false;
}

bool extractOggPayload (const std::uint8_t* data, std::size_t size, Bytes& out, juce::String& errorOut)
{
    std::vector<OggPage> pages;
    std::vector<OggPacket> packets;

    if (! parseOggPages (data, size, pages, errorOut))
        return false;

    if (! assembleOggPackets (pages, packets, errorOut))
        return false;

    if (packets.size() < 4)
    {
        errorOut = "Ogg stream contains no audio packets";
        return false;
    }

    out.clear();

    for (std::size_t i = 3; i < packets.size(); ++i)
        appendBytes (out, packets[i].bytes);

    if (out.empty())
    {
        errorOut = "Ogg stream contains no audio payload";
        return false;
    }

    return true;
}

bool extractDffPayload (const std::uint8_t* data, std::size_t size, Bytes& out, juce::String& errorOut)
{
    if (size < 16 || ! fourCCEquals (data, "FRM8"))
    {
        errorOut = "Not a DFF (DSDIFF) file";
        return false;
    }

    std::size_t pos = 16;

    while (pos + 12 <= size)
    {
        const std::string id (reinterpret_cast<const char*> (data + pos), 4);
        const auto chunkSize = readU64BE (data + pos + 4);

        if (chunkSize > size - pos - 12)
        {
            errorOut = "Malformed DFF chunk '" + juce::String (id.c_str()) + "'";
            return false;
        }

        if (id == "DSD ")
        {
            out.assign (data + pos + 12, data + pos + 12 + (std::size_t) chunkSize);
            return true;
        }

        pos += 12 + (std::size_t) chunkSize;

        if ((chunkSize & 1) != 0 && pos < size)
            ++pos;
    }

    errorOut = "DFF file has no DSD audio chunk";
    return false;
}

//==============================================================================
// Write dispatch
//==============================================================================

bool writeTagUpdate (const juce::File& file, const TagUpdate& update, juce::String& errorOut)
{
    const auto format = detectFormatName (file);

    if (format.isEmpty())
    {
        errorOut = "Unrecognised audio file format: " + file.getFullPathName();
        return false;
    }

    Bytes fileData;

    if (! readFileBytes (file, fileData, errorOut))
        return false;

    if (format == "FLAC")
        return writeFlac (file, fileData, update, errorOut);

    if (format == "MP3")
        return writeId3AtStart (file, fileData, update, errorOut);

    if (format == "M4A")
        return writeMp4 (file, fileData, update, errorOut);

    if (format == "AAC")
    {
        const bool isMp4Container = fileData.size() >= 8 && fourCCEquals (fileData.data() + 4, "ftyp");
        return isMp4Container ? writeMp4 (file, fileData, update, errorOut)
                              : writeId3AtStart (file, fileData, update, errorOut);
    }

    if (format == "OGG")
        return writeOgg (file, fileData, update, errorOut);

    if (format == "WAV")
        return writeWav (file, fileData, update, errorOut);

    if (format == "AIFF")
        return writeAiff (file, fileData, update, errorOut);

    if (format == "DSF")
        return writeDsf (file, fileData, update, errorOut);

    if (format == "DFF")
        return writeDff (file, fileData, update, errorOut);

    if (format == "CAF")
    {
        errorOut = "CAF metadata writing is not supported";
        return false;
    }

    errorOut = "Tag writing is not supported for " + format + " files";
    return false;
}

} // namespace

//==============================================================================
// Public API
//==============================================================================

juce::String detectFormatName (const juce::File& file)
{
    const auto extension = file.getFileExtension().toLowerCase();
    const char* extensionFormat = formatForExtension (extension);

    std::uint8_t head[12] = {};
    const std::size_t got = readFileHead (file, head, sizeof (head));

    if (got >= 4)
    {
        if (fourCCEquals (head, "fLaC")) return "FLAC";
        if (fourCCEquals (head, "OggS")) return "OGG";
        if (fourCCEquals (head, "DSD ")) return "DSF";
        if (fourCCEquals (head, "FRM8")) return "DFF";
        if (fourCCEquals (head, "caff")) return "CAF";

        if (head[0] == 'I' && head[1] == 'D' && head[2] == '3') return "MP3";

        if (got >= 12 && fourCCEquals (head, "RIFF") && fourCCEquals (head + 8, "WAVE")) return "WAV";

        if (got >= 12 && fourCCEquals (head, "FORM")
            && (fourCCEquals (head + 8, "AIFF") || fourCCEquals (head + 8, "AIFC")))
            return "AIFF";

        if (got >= 8 && fourCCEquals (head + 4, "ftyp")) return "M4A";

        if (head[0] == 0xff && (head[1] & 0xf6) == 0xf0) return "AAC";   // ADTS sync
    }

    return extensionFormat != nullptr ? juce::String (extensionFormat) : juce::String();
}

bool readTags (const juce::File& file, TagData& out, juce::String& errorOut)
{
    out = TagData {};
    errorOut.clear();

    out.formatName = detectFormatName (file);

    if (out.formatName.isEmpty())
    {
        errorOut = "Unrecognised audio file format: " + file.getFullPathName();
        return false;
    }

    Bytes data;

    if (! readFileBytes (file, data, errorOut))
        return false;

    bool ok = false;
    const auto& format = out.formatName;

    if (format == "FLAC")
        ok = readFlacTags (data.data(), data.size(), out, errorOut);
    else if (format == "MP3")
        ok = readId3AtStartTags (data.data(), data.size(), out, nullptr, errorOut);
    else if (format == "M4A")
        ok = readMp4Tags (data.data(), data.size(), out, nullptr, errorOut);
    else if (format == "AAC")
    {
        const bool isMp4Container = data.size() >= 8 && fourCCEquals (data.data() + 4, "ftyp");

        ok = isMp4Container ? readMp4Tags (data.data(), data.size(), out, nullptr, errorOut)
                            : readId3AtStartTags (data.data(), data.size(), out, nullptr, errorOut);
    }
    else if (format == "OGG")
        ok = readOggTags (data.data(), data.size(), out, nullptr, errorOut);
    else if (format == "WAV")
        ok = readWavTags (data.data(), data.size(), out, nullptr, errorOut);
    else if (format == "AIFF")
        ok = readAiffTags (data.data(), data.size(), out, nullptr, errorOut);
    else if (format == "DSF")
        ok = readDsfTags (data.data(), data.size(), out, nullptr, errorOut);
    else if (format == "DFF")
        ok = readDffTags (data.data(), data.size(), out, nullptr, errorOut);
    else if (format == "CAF")
    {
        errorOut = "CAF metadata reading is not supported";
        ok = false;
    }
    else
    {
        errorOut = "Tag reading is not supported for " + format + " files";
        ok = false;
    }

    if (! ok && errorOut.isEmpty())
        errorOut = "Could not read " + format + " tags from " + file.getFullPathName();

    out.readOk = ok;
    return ok;
}

bool writeTags (const juce::File& file, const TagData& tags, juce::String& errorOut)
{
    errorOut.clear();

    TagData existing;
    juce::String readError;

    if (! readTags (file, existing, readError))
    {
        errorOut = "Cannot update tags, the existing file could not be read: " + readError;
        return false;
    }

    TagData merged = existing;
    mergeNonEmpty (merged, tags);

    TagUpdate update;
    update.updateFields = true;
    update.fields = &merged;
    return writeTagUpdate (file, update, errorOut);
}

std::vector<EmbeddedArtwork> readArtwork (const juce::File& file, juce::String& errorOut)
{
    errorOut.clear();

    std::vector<EmbeddedArtwork> artwork;
    const auto format = detectFormatName (file);

    if (format.isEmpty())
    {
        errorOut = "Unrecognised audio file format: " + file.getFullPathName();
        return {};
    }

    Bytes data;

    if (! readFileBytes (file, data, errorOut))
        return {};

    bool ok = false;

    if (format == "FLAC")
    {
        FlacFile flac;

        if (parseFlacFile (data.data(), data.size(), flac, errorOut))
        {
            artwork = readFlacArtwork (flac, errorOut);
            ok = errorOut.isEmpty();
        }
    }
    else if (format == "MP3" || format == "AAC")
    {
        TagData unused;
        ok = readId3AtStartTags (data.data(), data.size(), unused, &artwork, errorOut);
    }
    else if (format == "M4A")
    {
        TagData unused;
        ok = readMp4Tags (data.data(), data.size(), unused, &artwork, errorOut);
    }
    else if (format == "OGG")
    {
        TagData unused;
        ok = readOggTags (data.data(), data.size(), unused, &artwork, errorOut);
    }
    else if (format == "WAV")
    {
        TagData unused;
        ok = readWavTags (data.data(), data.size(), unused, &artwork, errorOut);
    }
    else if (format == "AIFF")
    {
        TagData unused;
        ok = readAiffTags (data.data(), data.size(), unused, &artwork, errorOut);
    }
    else if (format == "DSF")
    {
        TagData unused;
        ok = readDsfTags (data.data(), data.size(), unused, &artwork, errorOut);
    }
    else if (format == "DFF")
    {
        TagData unused;
        ok = readDffTags (data.data(), data.size(), unused, &artwork, errorOut);
    }
    else
    {
        errorOut = "Artwork extraction is not supported for " + format + " files";
    }

    if (! ok)
        return {};

    return artwork;
}

bool writeArtwork (const juce::File& file, const juce::MemoryBlock& imageData,
                   const juce::String& mimeType, juce::String& errorOut)
{
    errorOut.clear();

    if (imageData.getSize() == 0)
    {
        errorOut = "Cannot write empty artwork data";
        return false;
    }

    const auto* bytes = static_cast<const std::uint8_t*> (imageData.getData());

    // Trust the bytes over the caller-supplied MIME type; never declare one
    // image format while embedding another.
    const juce::String sniffed = sniffImageMime (bytes, imageData.getSize());

    if (sniffed != "image/jpeg" && sniffed != "image/png")
    {
        errorOut = "Unsupported artwork image format (expected JPEG or PNG)";
        return false;
    }

    const juce::String mime = sniffed;
    (void) mimeType;

    EmbeddedArtwork artwork;
    artwork.data = imageData;
    artwork.mimeType = mime;
    artwork.pictureType = 3;   // front cover
    imageDimensions (bytes, imageData.getSize(), artwork.width, artwork.height);

    TagUpdate update;
    update.updateArtwork = true;
    update.artwork = &artwork;
    return writeTagUpdate (file, update, errorOut);
}

bool readReplayGain (const juce::File& file,
                     float& trackGainDb, float& albumGainDb, float& peakLinear,
                     bool& hasTrack, bool& hasAlbum, bool& hasPeak,
                     juce::String& errorOut)
{
    trackGainDb = albumGainDb = peakLinear = 0.0f;
    hasTrack = hasAlbum = hasPeak = false;
    errorOut.clear();

    TagData tags;

    if (! readTags (file, tags, errorOut))
        return false;

    trackGainDb = tags.replayGainTrackDb;
    albumGainDb = tags.replayGainAlbumDb;
    peakLinear = tags.peakLinear;
    hasTrack = tags.hasReplayGainTrack;
    hasAlbum = tags.hasReplayGainAlbum;
    hasPeak = tags.hasPeak;
    return true;
}

bool writeReplayGain (const juce::File& file, float trackGainDb, float albumGainDb,
                      float peakLinear, juce::String& errorOut)
{
    errorOut.clear();

    TagData tags;
    tags.replayGainTrackDb = trackGainDb;
    tags.hasReplayGainTrack = true;
    tags.replayGainAlbumDb = albumGainDb;
    tags.hasReplayGainAlbum = true;
    tags.peakLinear = peakLinear;
    tags.hasPeak = true;

    TagUpdate update;
    update.updateReplayGain = true;
    update.fields = &tags;
    return writeTagUpdate (file, update, errorOut);
}

bool extractAudioPayload (const juce::File& file, juce::MemoryBlock& out, juce::String& errorOut)
{
    out.reset();
    errorOut.clear();

    const auto format = detectFormatName (file);

    if (format.isEmpty())
    {
        errorOut = "Unrecognised audio file format: " + file.getFullPathName();
        return false;
    }

    Bytes data;

    if (! readFileBytes (file, data, errorOut))
        return false;

    Bytes payload;
    bool ok = false;

    if (format == "FLAC")
    {
        FlacFile flac;

        if (parseFlacFile (data.data(), data.size(), flac, errorOut))
        {
            payload.assign (data.begin() + (std::ptrdiff_t) flac.audioStart, data.end());
            ok = true;
        }
    }
    else if (format == "MP3" || format == "AAC")
    {
        ok = extractId3AtStartPayload (data.data(), data.size(), payload, errorOut);
    }
    else if (format == "M4A")
    {
        ok = extractMp4Payload (data.data(), data.size(), payload, errorOut);
    }
    else if (format == "OGG")
    {
        ok = extractOggPayload (data.data(), data.size(), payload, errorOut);
    }
    else if (format == "WAV")
    {
        ok = extractWavPayload (data.data(), data.size(), payload, errorOut);
    }
    else if (format == "AIFF")
    {
        ok = extractAiffPayload (data.data(), data.size(), payload, errorOut);
    }
    else if (format == "DSF")
    {
        DsfLayout layout;

        if (parseDsfFile (data.data(), data.size(), layout, errorOut))
        {
            payload = layout.dataPayload;
            ok = ! payload.empty();
        }
    }
    else if (format == "DFF")
    {
        ok = extractDffPayload (data.data(), data.size(), payload, errorOut);
    }
    else
    {
        errorOut = "Audio payload extraction is not supported for " + format + " files";
    }

    if (! ok)
    {
        if (errorOut.isEmpty())
            errorOut = "Could not extract audio payload from " + file.getFullPathName();

        return false;
    }

    out.setSize (payload.size(), false);

    if (! payload.empty())
        std::memcpy (out.getData(), payload.data(), payload.size());

    return true;
}

juce::String audioPayloadHash (const juce::File& file)
{
    juce::MemoryBlock payload;
    juce::String error;

    if (! extractAudioPayload (file, payload, error))
        return {};

    return md5Hex (payload.getData(), payload.getSize());
}

} // namespace sp::tags
