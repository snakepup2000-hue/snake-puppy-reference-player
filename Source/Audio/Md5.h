/*
    Snake Puppy Reference Player
    Small streaming MD5 implementation (RFC 1321), used to verify the FLAC
    STREAMINFO checksum while decoding. Header-only, no allocation.
*/
#pragma once

#include <cstdint>
#include <cstring>

namespace sp
{

class Md5
{
public:
    Md5() { reset(); }

    void reset() noexcept
    {
        a = 0x67452301u;
        b = 0xefcdab89u;
        c = 0x98badcfeu;
        d = 0x10325476u;
        totalBytes = 0;
        bufferLength = 0;
    }

    void update (const void* data, size_t numBytes) noexcept
    {
        const auto* bytes = static_cast<const std::uint8_t*> (data);
        totalBytes += numBytes;

        while (numBytes > 0)
        {
            const size_t space = 64 - bufferLength;
            const size_t take = numBytes < space ? numBytes : space;
            std::memcpy (buffer + bufferLength, bytes, take);
            bufferLength += take;
            bytes += take;
            numBytes -= take;

            if (bufferLength == 64)
            {
                processBlock (buffer);
                bufferLength = 0;
            }
        }
    }

    /** Finalises and writes the 16-byte big-endian digest. */
    void finalize (std::uint8_t digest[16]) noexcept
    {
        const std::uint64_t bitCount = totalBytes * 8ull;
        std::uint8_t padding[128] {};
        padding[0] = 0x80;
        const size_t padLength = (bufferLength < 56) ? (56 - bufferLength) : (120 - bufferLength);

        // Manual update to avoid changing totalBytes semantics.
        const size_t savedTotal = totalBytes;
        update (padding, padLength);
        totalBytes = savedTotal;

        std::uint8_t lengthBytes[8];
        for (int i = 0; i < 8; ++i)
            lengthBytes[i] = (std::uint8_t) ((bitCount >> (8 * i)) & 0xffu);
        update (lengthBytes, 8);
        totalBytes = savedTotal;

        writeLittleEndian (a, digest + 0);
        writeLittleEndian (b, digest + 4);
        writeLittleEndian (c, digest + 8);
        writeLittleEndian (d, digest + 12);
    }

    static void digest (const void* data, size_t numBytes, std::uint8_t out[16]) noexcept
    {
        Md5 md5;
        md5.update (data, numBytes);
        md5.finalize (out);
    }

private:
    static std::uint32_t rotateLeft (std::uint32_t x, int c) noexcept { return (x << c) | (x >> (32 - c)); }

    static void writeLittleEndian (std::uint32_t v, std::uint8_t* out) noexcept
    {
        out[0] = (std::uint8_t) (v & 0xffu);
        out[1] = (std::uint8_t) ((v >> 8) & 0xffu);
        out[2] = (std::uint8_t) ((v >> 16) & 0xffu);
        out[3] = (std::uint8_t) ((v >> 24) & 0xffu);
    }

    void processBlock (const std::uint8_t* block) noexcept
    {
        static constexpr std::uint32_t K[64] = {
            0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
            0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
            0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
            0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
            0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
            0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
            0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
            0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
        };
        static constexpr int S[64] = {
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
            5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
        };

        std::uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = (std::uint32_t) block[i * 4] | ((std::uint32_t) block[i * 4 + 1] << 8)
                 | ((std::uint32_t) block[i * 4 + 2] << 16) | ((std::uint32_t) block[i * 4 + 3] << 24);

        std::uint32_t aa = a, bb = b, cc = c, dd = d;

        for (int i = 0; i < 64; ++i)
        {
            std::uint32_t f = 0;
            int g = 0;

            if (i < 16)      { f = (bb & cc) | (~bb & dd); g = i; }
            else if (i < 32) { f = (dd & bb) | (~dd & cc); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = bb ^ cc ^ dd;           g = (3 * i + 5) % 16; }
            else             { f = cc ^ (bb | ~dd);        g = (7 * i) % 16; }

            const std::uint32_t temp = dd;
            dd = cc;
            cc = bb;
            bb = bb + rotateLeft (aa + f + K[i] + M[g], S[i]);
            aa = temp;
        }

        a += aa;
        b += bb;
        c += cc;
        d += dd;
    }

    std::uint32_t a = 0, b = 0, c = 0, d = 0;
    std::uint64_t totalBytes = 0;
    std::uint8_t buffer[64] {};
    size_t bufferLength = 0;
};

} // namespace sp
