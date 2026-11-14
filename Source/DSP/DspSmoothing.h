/*
    Snake Puppy Reference Player
    Small, allocation-free smoothing primitives shared by the DSP processors.

    Two kinds of smoothing are used:

      * LinearSmooth  - a parameter value (dB gain, log2 frequency, log2 Q)
                        ramped linearly over a fixed number of samples and
                        advanced once per control block. Cheap and deterministic.
      * SampleRamp    - a value (linear amplitude, wet/dry mix in 0..1) ramped
                        per sample so that gain changes are C0-continuous and
                        cannot click.

    All methods are noexcept and allocation free; they are safe on the audio
    thread.
*/
#pragma once

#include <algorithm>
#include <cmath>

namespace sp
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

//==============================================================================
/** Linear ramp used for slowly varying parameters that are re-evaluated once
    per control block (gain in dB, frequency in log2 Hz, Q in log2 units). */
struct LinearSmooth
{
    double current = 0.0;
    double target = 0.0;
    double step = 0.0;
    int remaining = 0;

    void reset (double value = 0.0) noexcept
    {
        current = target = value;
        step = 0.0;
        remaining = 0;
    }

    void setTarget (double newTarget, int rampSamples) noexcept
    {
        target = newTarget;
        if (rampSamples <= 0 || ! std::isfinite (newTarget))
        {
            current = target = newTarget;
            step = 0.0;
            remaining = 0;
            return;
        }
        remaining = rampSamples;
        step = (target - current) / (double) rampSamples;
    }

    /** Advances by `numSamples`; returns the value valid for this block. */
    double advance (int numSamples) noexcept
    {
        if (remaining > 0)
        {
            const int n = std::min (numSamples, remaining);
            current += step * (double) n;
            remaining -= n;
            if (remaining == 0)
                current = target;
        }
        else
        {
            current = target;
        }
        return current;
    }

    bool isRamping() const noexcept { return remaining > 0; }
};

//==============================================================================
/** Per-sample linear ramp (amplitudes and mixes). */
struct SampleRamp
{
    double current = 1.0;
    double target = 1.0;
    double step = 0.0;
    int remaining = 0;

    void reset (double value = 1.0) noexcept
    {
        current = target = value;
        step = 0.0;
        remaining = 0;
    }

    void setTarget (double newTarget, int rampSamples) noexcept
    {
        target = newTarget;
        if (rampSamples <= 0 || ! std::isfinite (newTarget))
        {
            current = target = newTarget;
            step = 0.0;
            remaining = 0;
            return;
        }
        remaining = rampSamples;
        step = (target - current) / (double) rampSamples;
    }

    inline double next() noexcept
    {
        if (remaining > 0)
        {
            current += step;
            if (--remaining == 0)
                current = target;
        }
        else
        {
            current = target;
        }
        return current;
    }

    /** Advances the whole ramp at once (used when a processor has finished a block). */
    void advance (int numSamples) noexcept
    {
        if (remaining > 0)
        {
            const int n = std::min (numSamples, remaining);
            current += step * (double) n;
            remaining -= n;
            if (remaining == 0)
                current = target;
        }
        else
        {
            current = target;
        }
    }

    bool isRamping() const noexcept { return remaining > 0; }
    bool isAt (double value) const noexcept { return remaining == 0 && current == value; }
    bool isSettled() const noexcept { return remaining == 0; }
};

//==============================================================================
inline double dbToGain (double db) noexcept
{
    return std::pow (10.0, db / 20.0);
}

inline double gainToDb (double gain) noexcept
{
    return gain > 1.0e-12 ? 20.0 * std::log10 (gain) : -240.0;
}

} // namespace sp
