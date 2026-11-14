#include "DSP/StereoProcessor.h"

#include <algorithm>
#include <cmath>

namespace sp
{

namespace
{
constexpr double kMonoBassQ[2] = { 0.54119610, 1.30656296 };   // 24 dB/oct Butterworth
constexpr double kCrossfeedQ = 0.7071067811865476;

inline double readFractionalDelay (const std::array<double, StereoProcessor::kCrossfeedDelaySize>& buf,
                                   int writePos, double delaySamples) noexcept
{
    constexpr double size = (double) StereoProcessor::kCrossfeedDelaySize;
    double readPos = (double) writePos - delaySamples;
    while (readPos < 0.0)
        readPos += size;
    while (readPos >= size)
        readPos -= size;

    const int i0 = (int) readPos;
    const double frac = readPos - (double) i0;
    const double a = buf[(size_t) i0];
    const double b = buf[(size_t) ((i0 + 1) % StereoProcessor::kCrossfeedDelaySize)];
    return a + frac * (b - a);
}
} // namespace

//==============================================================================
StereoProcessor::StereoProcessor()
{
    monoBassLog2Cutoff.reset (std::log2 (120.0));
    crossfeedLog2Cutoff.reset (std::log2 (700.0));
    crossfeedDelaySamples.reset (0.0);
}

void StereoProcessor::prepare (double sr, int maxBlockSize, int numChannels)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;
    maxBlock = std::max (1, maxBlockSize);
    numChannelsPrepared = std::clamp (numChannels, 1, kMaxChannels);

    // ~200 ms energy window for the mono-compatibility safeguard.
    energyCoeff = std::exp (-1.0 / (0.2 * sampleRate));

    reset();
}

void StereoProcessor::reset()
{
    balanceLeft.reset (balanceLeft.target);
    balanceRight.reset (balanceRight.target);
    width.reset (width.target);
    effectiveWidth = width.target;
    midEnergy = sideEnergy = 0.0;

    monoBassMix.reset (monoBassMix.target);
    monoBassCoeffValid = false;
    for (auto& st : monoBassState)
        st.reset();

    crossfeedGain.reset (crossfeedGain.target);
    crossfeedCoeffValid = false;
    crossfeedWasActive = crossfeedGain.target > 0.0;
    for (auto& st : crossfeedState)
        st.reset();
    for (auto& buf : crossfeedDelay)
        buf.fill (0.0);
    crossfeedWritePos = 0;
    crossfeedDelaySamples.reset (crossfeedDelaySamples.target);
}

void StereoProcessor::setParams (const StereoParams& p, int ramp)
{
    rampSamples = std::max (0, ramp);

    const double balance = std::clamp ((double) p.balance, -1.0, 1.0);
    const double leftGain = balance <= 0.0 ? 1.0 : 1.0 - balance;
    const double rightGain = balance >= 0.0 ? 1.0 : 1.0 + balance;
    balanceLeft.setTarget (leftGain, rampSamples);
    balanceRight.setTarget (rightGain, rampSamples);

    const double widthTarget = std::clamp ((double) p.widthPercent / 100.0, 0.0, 2.0);
    width.setTarget (widthTarget, rampSamples);
    protectMono = p.protectMono;
    if (widthTarget >= 1.0)
        effectiveWidth = widthTarget;

    monoBassEnabled = p.monoBassEnabled;
    const double monoCutoff = BiquadCoeffs::clampFrequency (p.monoBassHz, sampleRate);
    monoBassLog2Cutoff.setTarget (std::log2 (std::max (1.0, monoCutoff)), rampSamples);
    if (monoBassEnabled && monoBassMix.isAt (0.0))
    {
        for (auto& st : monoBassState)
            st.reset();
        monoBassCoeffValid = false;
    }
    monoBassMix.setTarget (monoBassEnabled ? 1.0 : 0.0, rampSamples);

    crossfeedEnabled = p.crossfeedEnabled;
    const double amountDb = std::clamp (std::abs ((double) p.crossfeedAmountDb), 0.0, 24.0);
    const double gain = dbToGain (-amountDb);
    const double crossCutoff = BiquadCoeffs::clampFrequency (p.crossfeedCutoffHz, sampleRate);
    crossfeedLog2Cutoff.setTarget (std::log2 (std::max (1.0, crossCutoff)), rampSamples);

    const double delaySamples = std::clamp ((double) p.crossfeedDelayUs, 0.0, 2000.0) * 1.0e-6 * sampleRate;
    crossfeedDelaySamples.setTarget (delaySamples, rampSamples);

    if (crossfeedEnabled && ! crossfeedWasActive && crossfeedGain.isAt (0.0))
    {
        for (auto& st : crossfeedState)
            st.reset();
        for (auto& buf : crossfeedDelay)
            buf.fill (0.0);
        crossfeedWritePos = 0;
        crossfeedCoeffValid = false;
    }
    crossfeedWasActive = crossfeedEnabled;
    crossfeedGain.setTarget (crossfeedEnabled ? gain : 0.0, rampSamples);
}

//==============================================================================
void StereoProcessor::processBalance (double* const* channels, int numChannels, int numFrames) noexcept
{
    if (numFrames <= 0 || channels == nullptr || numChannels < 1)
        return;

    if (balanceLeft.isAt (1.0) && balanceRight.isAt (1.0))
        return;

    const int nch = std::min (numChannels, kMaxChannels);

    if (nch == 1)
    {
        for (int n = 0; n < numFrames; ++n)
        {
            const double gl = balanceLeft.next();
            const double gr = balanceRight.next();
            channels[0][n] *= 0.5 * (gl + gr);
        }
        return;
    }

    for (int n = 0; n < numFrames; ++n)
    {
        const double gl = balanceLeft.next();
        const double gr = balanceRight.next();
        channels[0][n] *= gl;
        channels[1][n] *= gr;
    }
}

void StereoProcessor::updateMonoBassCoefficients (int numFrames) noexcept
{
    const double cutoff = std::pow (2.0, monoBassLog2Cutoff.advance (numFrames));
    if (monoBassCoeffValid && cutoff == monoBassDesignedCutoff)
        return;

    for (int s = 0; s < 2; ++s)
        monoBassCoeffs[(size_t) s] = BiquadCoeffs::highPass (sampleRate, cutoff, kMonoBassQ[s]);
    monoBassDesignedCutoff = cutoff;
    monoBassCoeffValid = true;
}

void StereoProcessor::processWidth (double* const* channels, int numChannels, int numFrames) noexcept
{
    if (numFrames <= 0 || channels == nullptr || numChannels < 2)
        return;

    const bool monoBassAudible = monoBassMix.current > 0.0 || monoBassMix.isRamping();
    if (width.isAt (1.0) && ! width.isRamping() && ! monoBassAudible)
        return;

    updateMonoBassCoefficients (numFrames);

    // Effective width with mono safeguard: do not widen so far that the side
    // energy exceeds the mid energy (which would collapse in a mono fold-down).
    double safeWidth = 2.0;
    if (protectMono && width.current > 1.0 && sideEnergy > 1.0e-20)
        safeWidth = std::clamp (0.7071067811865476 * std::sqrt (midEnergy / sideEnergy), 1.0, 2.0);

    double* left = channels[0];
    double* right = channels[1];

    if (! monoBassAudible)
    {
        for (int n = 0; n < numFrames; ++n)
        {
            const double l = left[n];
            const double r = right[n];
            const double mid = 0.5 * (l + r);
            const double side = 0.5 * (l - r);

            midEnergy += (mid * mid - midEnergy) * energyCoeff;
            sideEnergy += (side * side - sideEnergy) * energyCoeff;

            double w = width.next();
            if (protectMono && w > 1.0)
                w = std::min (w, safeWidth);

            effectiveWidth = w;
            left[n] = mid + w * side;
            right[n] = mid - w * side;
        }
        return;
    }

    for (int n = 0; n < numFrames; ++n)
    {
        const double l = left[n];
        const double r = right[n];
        const double mid = 0.5 * (l + r);
        const double side = 0.5 * (l - r);

        midEnergy += (mid * mid - midEnergy) * energyCoeff;
        sideEnergy += (side * side - sideEnergy) * energyCoeff;

        double sideOut = side;
        const double wet = monoBassState[0].process (monoBassCoeffs[0],
                                                     monoBassState[1].process (monoBassCoeffs[1], side));
        const double m = monoBassMix.next();
        sideOut = side + m * (wet - side);

        double w = width.next();
        if (protectMono && w > 1.0)
            w = std::min (w, safeWidth);

        effectiveWidth = w;
        left[n] = mid + w * sideOut;
        right[n] = mid - w * sideOut;
    }
}

void StereoProcessor::processCrossfeed (double* const* channels, int numChannels, int numFrames) noexcept
{
    if (numFrames <= 0 || channels == nullptr || numChannels < 2)
        return;

    const bool audible = crossfeedGain.current > 0.0 || crossfeedGain.isRamping();
    if (! audible)
        return;

    if (! crossfeedWasActive)
    {
        for (auto& st : crossfeedState)
            st.reset();
        for (auto& buf : crossfeedDelay)
            buf.fill (0.0);
        crossfeedWritePos = 0;
        crossfeedCoeffValid = false;
        crossfeedWasActive = true;
    }

    const double cutoff = std::pow (2.0, crossfeedLog2Cutoff.advance (numFrames));
    if (! crossfeedCoeffValid || cutoff != crossfeedDesignedCutoff)
    {
        crossfeedCoeffs = BiquadCoeffs::lowPass (sampleRate, cutoff, kCrossfeedQ);
        crossfeedDesignedCutoff = cutoff;
        crossfeedCoeffValid = true;
    }

    const double delay = crossfeedDelaySamples.advance (numFrames);
    double* left = channels[0];
    double* right = channels[1];

    for (int n = 0; n < numFrames; ++n)
    {
        const double l = left[n];
        const double r = right[n];

        crossfeedDelay[0][(size_t) crossfeedWritePos] = l;
        crossfeedDelay[1][(size_t) crossfeedWritePos] = r;

        const double delayedR = readFractionalDelay (crossfeedDelay[1], crossfeedWritePos, delay);
        const double delayedL = readFractionalDelay (crossfeedDelay[0], crossfeedWritePos, delay);

        const double lpR = crossfeedState[0].process (crossfeedCoeffs, delayedR);
        const double lpL = crossfeedState[1].process (crossfeedCoeffs, delayedL);

        const double g = crossfeedGain.next();

        left[n] = l + g * lpR;
        right[n] = r + g * lpL;

        crossfeedWritePos = (crossfeedWritePos + 1) % kCrossfeedDelaySize;
    }
}

} // namespace sp
