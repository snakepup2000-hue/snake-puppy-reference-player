#include "DSP/Dynamics.h"

#include <algorithm>
#include <cmath>

namespace sp
{

namespace
{
double besselI0 (double x) noexcept
{
    // Series expansion; x is small (<10) for our Kaiser beta, converges quickly.
    double sum = 1.0;
    double term = 1.0;
    const double halfX = x * 0.5;

    for (int k = 1; k < 32; ++k)
    {
        term *= (halfX / (double) k);
        const double add = term * term;
        sum += add;
        if (add < 1.0e-18 * sum)
            break;
    }
    return sum;
}

double sinc (double x) noexcept
{
    if (std::abs (x) < 1.0e-12)
        return 1.0;
    const double pix = kPi * x;
    return std::sin (pix) / pix;
}
} // namespace

//==============================================================================
TruePeakDetector::TruePeakDetector()
{
    constexpr int total = kTotalTaps;
    const double centre = (double) (total - 1) * 0.5;   // 4x sample units
    const double beta = 9.0;
    const double i0beta = besselI0 (beta);

    std::array<double, total> kernel {};

    for (int j = 0; j < total; ++j)
    {
        // Windowed sinc low-pass: cutoff at half of the base-rate Nyquist, i.e.
        // 0.25 cycles/sample at the 4x rate -> sinc((j - centre) / 4).
        const double normalized = (double) j / (double) (total - 1);
        const double windowArg = 2.0 * normalized - 1.0;
        const double window = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - windowArg * windowArg))) / i0beta;

        kernel[(size_t) j] = sinc (((double) j - centre) * 0.25) * window;
    }

    for (int p = 0; p < kPhases; ++p)
    {
        double sum = 0.0;
        for (int d = 0; d < kTapsPerPhase; ++d)
            sum += kernel[(size_t) (kPhases * d + p)];

        const double norm = std::abs (sum) > 1.0e-12 ? 1.0 / sum : 1.0;
        for (int d = 0; d < kTapsPerPhase; ++d)
            coeffs[(size_t) p][(size_t) d] = kernel[(size_t) (kPhases * d + p)] * norm;
    }

    prepare (kMaxChannels);
}

void TruePeakDetector::prepare (int numChannels)
{
    (void) numChannels;
    reset();
}

void TruePeakDetector::reset()
{
    for (auto& perChannel : history)
        perChannel.fill (0.0);
    pos = 0;
}

double TruePeakDetector::peak (int channel) const noexcept
{
    const auto& hist = history[(size_t) std::clamp (channel, 0, kMaxChannels - 1)];
    int idx = pos;
    double peakValue = 0.0;

    for (int p = 0; p < kPhases; ++p)
    {
        const auto& c = coeffs[(size_t) p];
        double y = 0.0;
        idx = pos;

        for (int d = 0; d < kTapsPerPhase; ++d)
        {
            y += hist[(size_t) idx] * c[(size_t) d];
            idx = (idx == 0) ? kTapsPerPhase - 1 : idx - 1;
        }

        peakValue = std::max (peakValue, std::abs (y));
    }

    return peakValue;
}

//==============================================================================
Limiter::Limiter()
{
    prepare (48000.0, 512, 2);
}

void Limiter::prepare (double sr, int maxBlockSize, int numChannels)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;
    maxBlock = std::max (1, maxBlockSize);
    numChannelsPrepared = std::clamp (numChannels, 1, kMaxChannels);

    lookaheadSamples = std::max (1, (int) std::lround (0.005 * sampleRate));   // 5 ms
    detectorDelay = TruePeakDetector::latencySamples();
    totalDelay = lookaheadSamples + detectorDelay;

    const int needed = totalDelay + maxBlock + 4;
    delayCapacity = 1;
    while (delayCapacity < needed)
        delayCapacity <<= 1;

    delayLine.assign ((size_t) (kMaxChannels * delayCapacity), 0.0);
    minDequeGain.assign ((size_t) (lookaheadSamples + 2), 1.0);
    minDequeLabel.assign ((size_t) (lookaheadSamples + 2), 0);

    // Smooth the attack over a fraction of the look-ahead so gain changes do
    // not splatter broadband energy (which would itself create intersample
    // peaks). The sliding minimum looks ahead, so the gain is already moving
    // before a peak reaches the output.
    const int attackSamples = std::max (2, lookaheadSamples / 6);
    attackCoeff = 1.0 - std::exp (-1.0 / (double) attackSamples);

    detector.prepare (numChannelsPrepared);
    prepared = true;
    reset();
}

void Limiter::reset()
{
    std::fill (delayLine.begin(), delayLine.end(), 0.0);
    writePos = 0;
    dequeHead = 0;
    dequeCount = 0;
    labelCounter = 0;
    currentGain = 1.0;
    clippedFrames = 0;
    ceilingGain.reset (ceilingGain.target);
    detector.reset();
}

void Limiter::setCeilingDb (double db, int rampSamples)
{
    ceilingGain.setTarget (dbToGain (std::clamp (db, -24.0, 0.0)), std::max (0, rampSamples));
}

void Limiter::setReleaseMs (double ms)
{
    const double tau = std::max (0.001, ms * 0.001);
    releaseCoeff = std::exp (-1.0 / (tau * sampleRate));
}

double Limiter::currentGainReductionDb() const noexcept
{
    if (currentGain >= 0.999999)
        return 0.0;
    return std::max (0.0, -20.0 * std::log10 (std::max (currentGain, 1.0e-9)));
}

void Limiter::process (double* const* channels, int numChannels, int numFrames) noexcept
{
    if (! prepared || numFrames <= 0 || channels == nullptr)
        return;

    const int nch = std::min (numChannels, kMaxChannels);
    const int dequeCapacity = lookaheadSamples + 2;

    for (int n = 0; n < numFrames; ++n)
    {
        const double ceiling = std::max (1.0e-6, ceilingGain.next());

        for (int ch = 0; ch < nch; ++ch)
            detector.push (ch, channels[ch][n]);

        double required = 1.0;
        for (int ch = 0; ch < nch; ++ch)
        {
            const double tp = detector.peak (ch);
            const double samplePeak = std::abs (channels[ch][n]);
            const double peakValue = std::max (tp, samplePeak);
            if (peakValue > ceiling)
                required = std::min (required, ceiling / peakValue);
        }

        detector.advanceFrame();

        if (required < 1.0)
            ++clippedFrames;

        // Sliding minimum of the required gain over the look-ahead window
        // (monotonic deque: O(1) amortised, allocation free).
        while (dequeCount > 0)
        {
            const int back = (dequeHead + dequeCount - 1) % dequeCapacity;
            if (minDequeGain[(size_t) back] >= required)
                --dequeCount;
            else
                break;
        }

        const int store = (dequeHead + dequeCount) % dequeCapacity;
        minDequeGain[(size_t) store] = required;
        minDequeLabel[(size_t) store] = labelCounter;
        ++dequeCount;

        while (dequeCount > 0 && minDequeLabel[(size_t) dequeHead] < labelCounter - lookaheadSamples)
        {
            dequeHead = (dequeHead + 1) % dequeCapacity;
            --dequeCount;
        }

        const double windowMin = dequeCount > 0 ? minDequeGain[(size_t) dequeHead] : 1.0;
        ++labelCounter;

        if (windowMin < currentGain)
            currentGain += (windowMin - currentGain) * attackCoeff;   // look-ahead attack
        else
            currentGain = windowMin + (currentGain - windowMin) * releaseCoeff;   // release

        currentGain = std::clamp (currentGain, 0.0, 1.0);

        for (int ch = 0; ch < nch; ++ch)
            delayLine[(size_t) (ch * delayCapacity + writePos)] = channels[ch][n];

        int readPos = writePos - totalDelay;
        if (readPos < 0)
            readPos += delayCapacity;

        for (int ch = 0; ch < nch; ++ch)
            channels[ch][n] = delayLine[(size_t) (ch * delayCapacity + readPos)] * currentGain;

        writePos = (writePos + 1) % delayCapacity;
    }
}

} // namespace sp
