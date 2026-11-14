/*
    Snake Puppy Reference Player
    High quality streaming sample-rate converter.

    Implementation: band-limited interpolation with a Kaiser-windowed sinc
    kernel, evaluated at arbitrary fractional input positions through a
    precomputed polyphase table (P+1 phases, linearly interpolated between
    neighbouring phases to remove the phase-quantisation error). The kernel
    cutoff is the smaller of the two Nyquist frequencies, so both upsampling and
    downsampling are correctly band limited (downsampling gets a longer kernel
    automatically, proportional to the rate ratio).

    The converter runs on the decode thread. All buffers and the phase table are
    allocated in prepare(); process() only reshapes an already-sized buffer in
    the rare case that the caller passes more input than any previous call.
*/
#include "DSP/Resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace sp
{

namespace
{
//==============================================================================
double besselI0 (double x) noexcept
{
    double sum = 1.0;
    double term = 1.0;
    const double halfX = x * 0.5;

    for (int k = 1; k < 40; ++k)
    {
        term *= (halfX / (double) k);
        const double add = term * term;
        sum += add;
        if (add < 1.0e-20 * sum)
            break;
    }
    return sum;
}

double sinc (double x) noexcept
{
    if (std::abs (x) < 1.0e-14)
        return 1.0;
    const double pix = juce::MathConstants<double>::pi * x;
    return std::sin (pix) / pix;
}

int halfTapsForQuality (int quality)
{
    switch (quality)
    {
        case 0:  return 12;   // fast
        case 2:  return 64;   // ultra
        case 1:
        default: return 32;   // high
    }
}

double kaiserBetaForQuality (int quality)
{
    return quality == 0 ? 7.0 : 10.0;
}
} // namespace

//==============================================================================
struct Resampler::Impl
{
    static constexpr int kNumPhases = 512;

    double sourceRateValue = 48000.0;
    double targetRateValue = 48000.0;
    double ratioValue = 1.0;
    int channels = 2;
    Quality quality = Quality::high;

    int halfTaps = 24;
    int taps = 48;
    double cutoffNorm = 1.0;   // relative to source Nyquist
    std::vector<double> phaseTable;   // (kNumPhases + 1) * taps

    std::vector<double> history;      // channels * capacity
    int capacity = 0;
    int storedFrames = 0;
    std::int64_t baseIndex = 0;       // absolute input index of history[0]
    double position = 0.0;            // absolute input position of the next output sample
    bool flushed = false;
    int latency = 0;
    bool passthrough = false;

    void ensureCapacity (int neededFrames)
    {
        if (neededFrames <= capacity)
            return;

        int newCapacity = std::max (capacity, 4096);
        while (newCapacity < neededFrames)
            newCapacity *= 2;

        std::vector<double> grown ((size_t) (channels * newCapacity), 0.0);
        for (int ch = 0; ch < channels; ++ch)
            std::memcpy (grown.data() + (size_t) (ch * newCapacity),
                         history.data() + (size_t) (ch * capacity),
                         sizeof (double) * (size_t) storedFrames);

        history.swap (grown);
        capacity = newCapacity;
    }

    void buildTable()
    {
        const int numPhases = kNumPhases;
        const double beta = kaiserBetaForQuality ((int) quality);
        const double i0beta = besselI0 (beta);

        phaseTable.assign ((size_t) ((numPhases + 1) * taps), 0.0);

        for (int q = 0; q <= numPhases; ++q)
        {
            const double frac = (double) q / (double) numPhases;
            double rowSum = 0.0;

            for (int m = 0; m < taps; ++m)
            {
                // Offset of this input tap from the interpolation position, in
                // input samples: tap m sits at (i0 - halfTaps + 1 + m).
                const double t = (double) (m - halfTaps + 1) - frac;

                // The window must be centred on the sinc (t = 0), not on a fixed
                // tap index, otherwise the kernel changes shape with the
                // fractional phase and modulates the signal.
                const double u = t / (double) halfTaps;
                double window = 0.0;
                if (std::abs (u) < 1.0)
                    window = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - u * u))) / i0beta;

                const double value = cutoffNorm * sinc (cutoffNorm * t) * window;
                phaseTable[(size_t) (q * taps + m)] = value;
                rowSum += value;
            }

            // Normalise each phase to unity DC gain.
            if (std::abs (rowSum) > 1.0e-12)
            {
                const double inv = 1.0 / rowSum;
                for (int m = 0; m < taps; ++m)
                    phaseTable[(size_t) (q * taps + m)] *= inv;
            }
        }
    }

    inline double readInput (int channel, std::int64_t absoluteIndex) const noexcept
    {
        const std::int64_t offset = absoluteIndex - baseIndex;
        if (offset < 0 || offset >= (std::int64_t) storedFrames)
            return 0.0;
        return history[(size_t) (channel * capacity + (int) offset)];
    }

    void generateSample (double* const* outChannels, int outIndex) noexcept
    {
        const std::int64_t i0 = (std::int64_t) std::floor (position);
        const double frac = position - (double) i0;
        const double phase = frac * (double) kNumPhases;
        const int p = std::min (kNumPhases - 1, std::max (0, (int) std::floor (phase)));
        const double f = phase - (double) p;

        const double* rowA = phaseTable.data() + (size_t) (p * taps);
        const double* rowB = rowA + taps;

        const std::int64_t first = i0 - halfTaps + 1;

        for (int ch = 0; ch < channels; ++ch)
        {
            double acc = 0.0;
            for (int m = 0; m < taps; ++m)
            {
                const double w = rowA[m] + f * (rowB[m] - rowA[m]);
                acc += w * readInput (ch, first + m);
            }
            outChannels[ch][outIndex] = acc;
        }

        position += 1.0 / ratioValue;
    }
};

//==============================================================================
Resampler::Resampler() : impl (std::make_unique<Impl>()) {}
Resampler::~Resampler() = default;

void Resampler::prepare (double sourceRate, double targetRate, int numChannels, Quality quality)
{
    auto& d = *impl;

    d.sourceRateValue = sourceRate > 0.0 ? sourceRate : 48000.0;
    d.targetRateValue = targetRate > 0.0 ? targetRate : d.sourceRateValue;
    d.ratioValue = d.targetRateValue / d.sourceRateValue;
    d.channels = std::max (1, numChannels);
    d.quality = quality;

    d.passthrough = std::abs (d.ratioValue - 1.0) < 1.0e-12;

    const int baseHalf = halfTapsForQuality ((int) quality);
    d.cutoffNorm = std::min (1.0, d.ratioValue);
    d.halfTaps = d.passthrough ? baseHalf
                               : (int) std::ceil ((double) baseHalf / std::max (0.05, d.cutoffNorm));
    d.taps = 2 * d.halfTaps;

    if (! d.passthrough)
        d.buildTable();

    d.capacity = std::max (4096, d.taps * 4);
    d.history.assign ((size_t) (d.channels * d.capacity), 0.0);
    d.storedFrames = 0;
    d.baseIndex = 0;
    d.position = 0.0;
    d.flushed = false;
    d.latency = d.passthrough ? 0 : (int) std::llround ((double) d.halfTaps * d.ratioValue);
}

void Resampler::reset()
{
    auto& d = *impl;
    std::fill (d.history.begin(), d.history.end(), 0.0);
    d.storedFrames = 0;
    d.baseIndex = 0;
    d.position = 0.0;
    d.flushed = false;
}

int Resampler::process (const double* const* inChannels, int inFrames,
                        double* const* outChannels, int maxOutFrames, bool flush)
{
    auto& d = *impl;

    if (maxOutFrames <= 0)
        return 0;

    if (d.channels <= 0 || outChannels == nullptr)
        return 0;

    if (d.passthrough)
    {
        const int n = std::min (inFrames, maxOutFrames);
        for (int ch = 0; ch < d.channels; ++ch)
            if (inChannels != nullptr && inChannels[ch] != nullptr)
                std::memcpy (outChannels[ch], inChannels[ch], sizeof (double) * (size_t) n);
        return n;
    }

    if (d.flushed && inFrames <= 0)
        return 0;

    if (inFrames > 0 && inChannels != nullptr)
    {
        d.ensureCapacity (d.storedFrames + inFrames);

        for (int ch = 0; ch < d.channels; ++ch)
        {
            const double* src = inChannels[ch];
            double* dst = d.history.data() + (size_t) (ch * d.capacity + d.storedFrames);
            if (src == nullptr)
                std::fill (dst, dst + inFrames, 0.0);
            else
                std::memcpy (dst, src, sizeof (double) * (size_t) inFrames);
        }
        d.storedFrames += inFrames;
    }

    const std::int64_t endIndex = d.baseIndex + (std::int64_t) d.storedFrames;
    int produced = 0;

    if (! flush)
    {
        while (produced < maxOutFrames && (d.position + (double) d.halfTaps) < (double) endIndex)
            d.generateSample (outChannels, produced++);
    }
    else
    {
        // Emit the filter tail: continue while the kernel still overlaps real
        // input (out-of-range taps are read as zero, which is exactly the
        // end-of-stream padding).
        while (produced < maxOutFrames && (d.position - (double) d.halfTaps) < (double) endIndex)
            d.generateSample (outChannels, produced++);

        if (produced < maxOutFrames)
            d.flushed = true;
    }

    // Discard input that can no longer contribute.
    const std::int64_t keepFrom = (std::int64_t) std::floor (d.position) - (std::int64_t) d.halfTaps - 1;
    const std::int64_t drop = std::min ((std::int64_t) d.storedFrames, std::max ((std::int64_t) 0, keepFrom - d.baseIndex));
    if (drop > 0)
    {
        const int remaining = d.storedFrames - (int) drop;
        for (int ch = 0; ch < d.channels; ++ch)
            std::memmove (d.history.data() + (size_t) (ch * d.capacity),
                          d.history.data() + (size_t) (ch * d.capacity + (int) drop),
                          sizeof (double) * (size_t) remaining);
        d.storedFrames = remaining;
        d.baseIndex += drop;
    }

    return produced;
}

double Resampler::sourceRate() const { return impl->sourceRateValue; }
double Resampler::targetRate() const { return impl->targetRateValue; }
int Resampler::numChannels() const { return impl->channels; }
int Resampler::latencySamples() const { return impl->latency; }
double Resampler::ratio() const { return impl->ratioValue; }

int Resampler::maxOutputFrames (double src, double dst, int inFrames)
{
    const double r = (src > 0.0) ? dst / src : 1.0;
    return (int) std::ceil ((double) inFrames * r) + 128;
}

} // namespace sp
