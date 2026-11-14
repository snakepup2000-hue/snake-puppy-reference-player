/*
    Snake Puppy Reference Player
    TPDF (triangular probability density) dither generator for the final
    quantisation stage.

    The audio path is 64-bit float, but Core Audio eventually hands integers to
    the DAC. When the effective device depth is lower than the source depth we
    add TPDF noise at the device's LSB scale before writing the float block, so
    the driver's truncation is decorrelated. Two independent uniform random
    values are subtracted, giving noise in (-1, +1) LSB with an RMS of
    0.408 * LSB and a flat spectrum.

    Threading: prepared/used only on the audio callback thread (or a test
    thread). No allocation, no locking, no JUCE String.
*/
#pragma once

#include <cstdint>

namespace sp
{

class TpdfDither
{
public:
    TpdfDither() = default;

    void prepare (std::uint32_t seed = 0x9e3779b9u) noexcept
    {
        state = seed != 0 ? seed : 1u;
    }

    void reset() noexcept { prepare(); }

    /** Should dither be applied for this source -> device depth pair? */
    static bool shouldApply (int targetBits, int sourceBits) noexcept
    {
        return targetBits > 0 && sourceBits > 0 && targetBits < sourceBits;
    }

    /** Adds TPDF noise at the target depth's LSB scale, in place, to a
        non-interleaved double stereo block. Only applied when shouldApply().
        Returns true when noise was added. */
    bool process (double* left, double* right, int frames, int targetBits, int sourceBits) noexcept
    {
        if (! shouldApply (targetBits, sourceBits) || frames <= 0)
            return false;

        const double lsb = lsbForBits (targetBits);

        for (int i = 0; i < frames; ++i)
        {
            if (left != nullptr)
                left[i] += nextTpdf() * lsb;
            if (right != nullptr)
                right[i] += nextTpdf() * lsb;
        }

        return true;
    }

    /** Full-scale LSB size for a signed integer depth (e.g. 16 bits -> 2^-15). */
    static double lsbForBits (int bits) noexcept
    {
        if (bits <= 0)
            return 0.0;
        return 1.0 / (double) ((std::uint64_t) 1 << (bits - 1));
    }

    static const char* nameForBits (int bits) noexcept
    {
        switch (bits)
        {
            case 16: return "TPDF 16-bit";
            case 20: return "TPDF 20-bit";
            case 24: return "TPDF 24-bit";
            case 32: return "TPDF 32-bit";
            default: return "TPDF";
        }
    }

    /** Deterministic xorshift32 -> uniform in [0, 1). */
    double nextUniform() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (double) (state >> 8) * (1.0 / 16777216.0);
    }

    /** One TPDF sample, nominal range (-1, +1) in LSB units. */
    double nextTpdf() noexcept
    {
        return nextUniform() - nextUniform();
    }

    std::uint32_t getState() const noexcept { return state; }

private:
    std::uint32_t state = 0x9e3779b9u;
};

} // namespace sp
