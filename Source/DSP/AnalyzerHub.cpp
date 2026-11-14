/*
    Snake Puppy Reference Player
    Metering / analysis hub.

    Threading:
      * pushInput()/pushOutput()/notifyUnderrun()/notifyDeviceReset() are called
        from the audio callback. They only write into preallocated lock-free
        FIFOs (juce::AbstractFifo) and bump atomics.
      * run() is the analysis thread. It drains the FIFOs, performs all
        measurement maths (peak/RMS, 4x true peak, BS.1770-4 loudness with
        gating, correlation, crest factor, FFT spectrum, spectrogram, band
        energies) and publishes an immutable snapshot under a short lock.
      * The getters are called from the UI/message thread, copy the snapshot
        under the same short lock and return immediately.

    Every published number is measured from real samples; nothing is animated
    or synthesised.
*/
#include "DSP/AnalyzerHub.h"

#include "DSP/Biquad.h"
#include "DSP/DspSmoothing.h"
#include "DSP/Dynamics.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace sp
{

namespace
{
constexpr int kFifoCapacity = 16384;
constexpr int kMonoHistorySize = 16384;
constexpr int kMaxFftSize = 16384;
constexpr double kMinusInfinityDb = -144.0;

inline double linearToDb (double linear)
{
    return linear > 1.0e-12 ? 20.0 * std::log10 (linear) : kMinusInfinityDb;
}

inline double energyToLufs (double energy)
{
    return energy > 1.0e-20 ? -0.691 + 10.0 * std::log10 (energy) : kMinusInfinityDb;
}

inline double lufsToEnergy (double lufs)
{
    return std::pow (10.0, (lufs + 0.691) / 10.0);
}

/** ITU-R BS.1770-4 K-weighting: high shelf followed by the RLB high pass.
    Coefficients are derived from the analog prototype (libebur128 formulation)
    so the meter is accurate at 44.1 kHz as well as 48 kHz where the standard
    tabulates its coefficients. */
void designKWeighting (double sampleRate, BiquadCoeffs& shelf, BiquadCoeffs& highPass)
{
    {
        const double f0 = 1681.974450955533;
        const double gainDb = 3.999843853973347;
        const double q = 0.7071752369554196;
        const double k = std::tan (kPi * f0 / sampleRate);
        const double vh = std::pow (10.0, gainDb / 20.0);
        const double vb = std::pow (vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;

        shelf.b0 = (vh + vb * k / q + k * k) / a0;
        shelf.b1 = 2.0 * (k * k - vh) / a0;
        shelf.b2 = (vh - vb * k / q + k * k) / a0;
        shelf.a1 = 2.0 * (k * k - 1.0) / a0;
        shelf.a2 = (1.0 - k / q + k * k) / a0;
    }

    {
        const double f0 = 38.13547087602444;
        const double q = 0.5003270373238773;
        const double k = std::tan (kPi * f0 / sampleRate);
        const double a0 = 1.0 + k / q + k * k;

        highPass.b0 = 1.0;
        highPass.b1 = -2.0;
        highPass.b2 = 1.0;
        highPass.a1 = 2.0 * (k * k - 1.0) / a0;
        highPass.a2 = (1.0 - k / q + k * k) / a0;
    }
}

} // namespace

//==============================================================================
struct AnalyzerHub::Impl
{
    struct Snapshot
    {
        MeterValues values;
        std::vector<float> displayDb;
        std::vector<float> displayFreqHz;
        std::vector<float> rawDb;
        std::vector<float> spectrogram;
        int spectrogramRows = 0;
        int fftSize = 4096;
        int updateRate = 60;
    };

    static constexpr int kDisplayBins = 128;
    static constexpr int kSubblockCount = 30;          // 3 s at 100 ms
    static constexpr int kSpectrogramBands = AnalyzerHub::kSpectrogramBands;
    static constexpr int kSpectrogramRows = AnalyzerHub::kSpectrogramHistory;
    static constexpr int kMaxChannels = 2;

    //== Configuration =======================================================
    double sampleRate = 48000.0;
    int maxBlockSize = 512;

    //== Audio-thread FIFOs ==================================================
    mutable juce::AbstractFifo inputFifo { kFifoCapacity };
    mutable juce::AbstractFifo outputFifo { kFifoCapacity };
    std::vector<float> inputBuffer;    // interleaved L/R
    std::vector<float> outputBuffer;

    //== Settings ============================================================
    std::atomic<int> requestedFftSize { 4096 };
    std::atomic<int> actualFftSize { 4096 };
    std::atomic<int> requestedUpdateRateHz { 60 };
    std::atomic<int> actualUpdateRateHz { 60 };
    std::atomic<bool> resetRequested { false };
    std::atomic<bool> deviceResetRequested { false };
    std::atomic<std::int64_t> underrunCounter { 0 };

    //== Snapshot ============================================================
    mutable juce::CriticalSection snapshotLock;
    Snapshot snapshot;

    //== Analysis-thread state ==============================================
    TruePeakDetector truePeakDetector;
    double peakLinear[kMaxChannels] {};
    double truePeakLinear[kMaxChannels] {};
    double rmsEnergy[kMaxChannels] {};
    double inputPeakLinear[kMaxChannels] {};
    double inputRmsEnergy[kMaxChannels] {};
    double correlationLL = 0.0, correlationRR = 0.0, correlationLR = 0.0;

    double peakDecay = 0.9999;
    double rmsCoeff = 0.0001;
    double correlationCoeff = 0.0001;

    std::int64_t clipCount = 0;
    int clipHoldFrames = 0;
    double samplePeakMax = 0.0;
    double truePeakMax = 0.0;
    std::int64_t analysedFrames = 0;

    //== K-weighting and loudness ===========================================
    BiquadCoeffs kShelf, kHighPass;
    BiquadState kShelfState[kMaxChannels] {};
    BiquadState kHighPassState[kMaxChannels] {};

    static constexpr int kLraBins = 1000;
    static constexpr double kLraMinLufs = -70.0;
    static constexpr double kLraStepLu = 0.1;

    std::array<double, kSubblockCount> subblockEnergy {};
    int subblockWrite = 0;
    int subblockCount = 0;
    int subblockFrames = 0;
    double subblockSumSq[kMaxChannels] {};

    double momentaryEnergy = 0.0;
    double shortTermEnergy = 0.0;
    double momentaryLufs = kMinusInfinityDb;
    double shortTermLufs = kMinusInfinityDb;

    std::vector<double> integratedEnergies;
    std::array<std::int64_t, kLraBins> lraHistogram {};
    double lraEnergySum = 0.0;
    std::int64_t lraValueCount = 0;

    //== FFT =================================================================
    std::unique_ptr<juce::dsp::FFT> fft;
    int fftSize = 4096;
    std::vector<float> fftWork;       // 2 * fftSize
    std::vector<float> window;        // fftSize, Hann
    double windowSumSquares = 0.0;
    std::vector<float> monoHistory;   // kMonoHistorySize
    int monoWrite = 0;
    std::int64_t monoCount = 0;

    std::vector<float> rawDb;
    std::vector<float> displayDb;
    std::vector<float> displayFreqHz;
    std::vector<float> spectrogram;   // rows * bands
    int spectrogramRows = 0;
    int spectrogramWrite = 0;

    //==========================================================================
    void configureForSampleRate (double sr)
    {
        sampleRate = sr > 0.0 ? sr : 48000.0;
        designKWeighting (sampleRate, kShelf, kHighPass);

        // 20 dB/s peak decay, 300 ms RMS / correlation windows.
        peakDecay = std::pow (10.0, -20.0 / (20.0 * sampleRate));
        rmsCoeff = 1.0 - std::exp (-1.0 / (0.3 * sampleRate));
        correlationCoeff = rmsCoeff;

        displayFreqHz.resize ((size_t) kDisplayBins);
        for (int i = 0; i < kDisplayBins; ++i)
        {
            const double t = kDisplayBins > 1 ? (double) i / (double) (kDisplayBins - 1) : 0.0;
            displayFreqHz[(size_t) i] = (float) (20.0 * std::pow (1000.0, t));
        }

        displayDb.assign ((size_t) kDisplayBins, (float) kMinusInfinityDb);
        spectrogram.assign ((size_t) (kSpectrogramBands * kSpectrogramRows), (float) kMinusInfinityDb);
        spectrogramRows = 0;
        spectrogramWrite = 0;

        snapshot.displayFreqHz = displayFreqHz;
        snapshot.displayDb.assign ((size_t) kDisplayBins, (float) kMinusInfinityDb);
        snapshot.spectrogram.assign ((size_t) (kSpectrogramBands * kSpectrogramRows), (float) kMinusInfinityDb);
        snapshot.spectrogramRows = 0;
    }

    //==========================================================================
    void resetAnalysisState (bool keepCounters)
    {
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            peakLinear[ch] = 0.0;
            truePeakLinear[ch] = 0.0;
            rmsEnergy[ch] = 0.0;
            inputPeakLinear[ch] = 0.0;
            inputRmsEnergy[ch] = 0.0;
            kShelfState[ch].reset();
            kHighPassState[ch].reset();
        }

        truePeakDetector.reset();
        correlationLL = correlationRR = correlationLR = 0.0;
        samplePeakMax = 0.0;
        truePeakMax = 0.0;
        clipHoldFrames = 0;
        analysedFrames = 0;
        if (! keepCounters)
            clipCount = 0;

        subblockEnergy.fill (0.0);
        subblockWrite = 0;
        subblockCount = 0;
        subblockFrames = 0;
        subblockSumSq[0] = subblockSumSq[1] = 0.0;
        momentaryEnergy = shortTermEnergy = 0.0;
        momentaryLufs = shortTermLufs = kMinusInfinityDb;
        integratedEnergies.clear();
        lraHistogram.fill (0);
        lraEnergySum = 0.0;
        lraValueCount = 0;

        monoWrite = 0;
        monoCount = 0;
        std::fill (monoHistory.begin(), monoHistory.end(), 0.0f);
        for (auto& v : displayDb) v = (float) kMinusInfinityDb;
        for (auto& v : spectrogram) v = (float) kMinusInfinityDb;
        spectrogramRows = 0;
        spectrogramWrite = 0;

        const juce::ScopedLock lock (snapshotLock);
        snapshot.values = MeterValues {};
        snapshot.values.sampleRate = sampleRate;
        snapshot.values.blockSize = maxBlockSize;
        snapshot.spectrogramRows = 0;
    }

    void drainFifo (juce::AbstractFifo& fifo, std::vector<float>& buffer, bool asInput)
    {
        int remaining = fifo.getNumReady();
        while (remaining > 0)
        {
            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            fifo.prepareToRead (std::min (remaining, 512), start1, size1, start2, size2);

            if (size1 > 0)
                processRegion (buffer.data() + (size_t) (2 * start1), size1, asInput);
            if (size2 > 0)
                processRegion (buffer.data() + (size_t) (2 * start2), size2, asInput);

            fifo.finishedRead (size1 + size2);
            remaining -= size1 + size2;
            if (size1 + size2 == 0)
                break;
        }
    }

    void updatePeakAndRms (double l, double r, bool asInput)
    {
        double* peak = asInput ? inputPeakLinear : peakLinear;
        double* rms = asInput ? inputRmsEnergy : rmsEnergy;

        const double samples[2] = { l, r };
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            const double absValue = std::abs (samples[ch]);
            peak[ch] = std::max (absValue, peak[ch] * peakDecay);
            rms[ch] += (samples[ch] * samples[ch] - rms[ch]) * rmsCoeff;
        }
    }

    void processRegion (const float* interleaved, int frames, bool asInput)
    {
        if (asInput)
        {
            for (int i = 0; i < frames; ++i)
                updatePeakAndRms ((double) interleaved[2 * i], (double) interleaved[2 * i + 1], true);
            return;
        }

        const int subblockLength = std::max (1, (int) std::lround (0.1 * sampleRate));

        for (int i = 0; i < frames; ++i)
        {
            const double l = (double) interleaved[2 * i];
            const double r = (double) interleaved[2 * i + 1];

            updatePeakAndRms (l, r, false);

            // True peak (4x oversampled), per channel.
            truePeakDetector.push (0, l);
            truePeakDetector.push (1, r);
            for (int ch = 0; ch < kMaxChannels; ++ch)
            {
                const double tp = std::max (truePeakDetector.peak (ch), std::abs (ch == 0 ? l : r));
                truePeakLinear[ch] = std::max (tp, truePeakLinear[ch] * peakDecay);
            }
            truePeakDetector.advanceFrame();

            // Sample peak / clipping.
            const double absL = std::abs (l);
            const double absR = std::abs (r);
            samplePeakMax = std::max (samplePeakMax, std::max (absL, absR));
            truePeakMax = std::max (truePeakMax, std::max (truePeakLinear[0], truePeakLinear[1]));
            if (absL >= 1.0 || absR >= 1.0)
            {
                ++clipCount;
                clipHoldFrames = (int) sampleRate;
            }
            if (clipHoldFrames > 0)
                --clipHoldFrames;

            // Correlation (300 ms window).
            correlationLL += (l * l - correlationLL) * correlationCoeff;
            correlationRR += (r * r - correlationRR) * correlationCoeff;
            correlationLR += (l * r - correlationLR) * correlationCoeff;

            // K-weighted loudness accumulation (BS.1770-4 pre-filter + RLB).
            const double kwL = kHighPassState[0].process (kHighPass, kShelfState[0].process (kShelf, l));
            const double kwR = kHighPassState[1].process (kHighPass, kShelfState[1].process (kShelf, r));

            subblockSumSq[0] += kwL * kwL;
            subblockSumSq[1] += kwR * kwR;
            ++subblockFrames;

            // Mono history for the spectrum.
            monoHistory[(size_t) monoWrite] = (float) (0.5 * (l + r));
            monoWrite = (monoWrite + 1) % kMonoHistorySize;
            ++monoCount;

            ++analysedFrames;

            if (subblockFrames >= subblockLength)
            {
                finishSubblock();
                subblockFrames = 0;
                subblockSumSq[0] = subblockSumSq[1] = 0.0;
            }
        }
    }

    void finishSubblock()
    {
        const double frames = (double) std::max (1, subblockFrames);
        const double energy = (subblockSumSq[0] + subblockSumSq[1]) / frames;

        subblockEnergy[(size_t) subblockWrite] = energy;
        subblockWrite = (subblockWrite + 1) % kSubblockCount;
        subblockCount = std::min (subblockCount + 1, kSubblockCount);

        auto energyAt = [this] (int back) -> double
        {
            if (back >= subblockCount)
                return 0.0;
            const int index = (subblockWrite - 1 - back + kSubblockCount) % kSubblockCount;
            return subblockEnergy[(size_t) index];
        };

        if (subblockCount >= 4)
        {
            double sum = 0.0;
            for (int i = 0; i < 4; ++i)
                sum += energyAt (i);
            momentaryEnergy = sum / 4.0;
            momentaryLufs = energyToLufs (momentaryEnergy);

            if (momentaryLufs > -70.0)
                integratedEnergies.push_back (momentaryEnergy);
        }

        if (subblockCount >= 10)
        {
            const int n = std::min (subblockCount, kSubblockCount);
            double sum = 0.0;
            for (int i = 0; i < n; ++i)
                sum += energyAt (i);
            shortTermEnergy = sum / (double) n;
            shortTermLufs = energyToLufs (shortTermEnergy);

            if (shortTermLufs > -70.0)
            {
                const double clamped = std::clamp (shortTermLufs, kLraMinLufs, kLraMinLufs + (kLraBins - 1) * kLraStepLu);
                const int bin = (int) std::lround ((clamped - kLraMinLufs) / kLraStepLu);
                lraHistogram[(size_t) std::clamp (bin, 0, kLraBins - 1)] += 1;
                lraEnergySum += shortTermEnergy;
                ++lraValueCount;
            }
        }
    }

    double computeIntegratedLufs() const
    {
        if (integratedEnergies.empty())
            return kMinusInfinityDb;

        const double absoluteThreshold = lufsToEnergy (-70.0);

        double sum = 0.0;
        std::int64_t count = 0;
        for (double e : integratedEnergies)
            if (e > absoluteThreshold)
            {
                sum += e;
                ++count;
            }

        if (count == 0)
            return kMinusInfinityDb;

        const double relativeThreshold = lufsToEnergy (energyToLufs (sum / (double) count) - 10.0);

        sum = 0.0;
        count = 0;
        for (double e : integratedEnergies)
            if (e > absoluteThreshold && e > relativeThreshold)
            {
                sum += e;
                ++count;
            }

        return count > 0 ? energyToLufs (sum / (double) count) : kMinusInfinityDb;
    }

    double computeLoudnessRange() const
    {
        if (lraValueCount < 2)
            return 0.0;

        const double meanLufs = energyToLufs (lraEnergySum / (double) lraValueCount);
        const double relativeThreshold = meanLufs - 20.0;

        std::int64_t total = 0;
        int startBin = kLraBins;
        for (int b = 0; b < kLraBins; ++b)
        {
            const double lufs = kLraMinLufs + b * kLraStepLu;
            if (lraHistogram[(size_t) b] > 0 && lufs >= relativeThreshold)
            {
                startBin = std::min (startBin, b);
                total += lraHistogram[(size_t) b];
            }
        }

        if (total < 2 || startBin >= kLraBins)
            return 0.0;

        auto percentile = [this, startBin, total] (double fraction) -> double
        {
            const std::int64_t target = (std::int64_t) std::llround (fraction * (double) (total - 1));
            std::int64_t cumulative = 0;
            for (int b = startBin; b < kLraBins; ++b)
            {
                cumulative += lraHistogram[(size_t) b];
                if (cumulative > target)
                    return kLraMinLufs + b * kLraStepLu;
            }
            return kLraMinLufs + (kLraBins - 1) * kLraStepLu;
        };

        return std::max (0.0, percentile (0.95) - percentile (0.10));
    }

    //==========================================================================
    void applyFftSize (int newSize)
    {
        newSize = std::clamp (newSize, 1024, kMaxFftSize);

        fft = std::make_unique<juce::dsp::FFT> ((int) std::lround (std::log2 ((double) newSize)));
        fftSize = newSize;
        fftWork.assign ((size_t) (2 * fftSize), 0.0f);

        window.assign ((size_t) fftSize, 0.0f);
        double sumSquares = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) (fftSize - 1));
            window[(size_t) i] = (float) w;
            sumSquares += w * w;
        }
        windowSumSquares = sumSquares;

        rawDb.assign ((size_t) (fftSize / 2), (float) kMinusInfinityDb);

        const juce::ScopedLock lock (snapshotLock);
        snapshot.rawDb.assign ((size_t) (fftSize / 2), (float) kMinusInfinityDb);
        snapshot.fftSize = fftSize;
        actualFftSize.store (fftSize);
    }

    //== Spectrum ============================================================
    void computeSpectrum()
    {
        if (fft == nullptr)
            return;

        const int n = fftSize;
        if (monoCount < (std::int64_t) n)
        {
            std::fill (displayDb.begin(), displayDb.end(), (float) kMinusInfinityDb);
            std::fill (rawDb.begin(), rawDb.end(), (float) kMinusInfinityDb);
            return;
        }

        for (int i = 0; i < n; ++i)
        {
            const int index = (monoWrite - n + i + kMonoHistorySize * 2) % kMonoHistorySize;
            fftWork[(size_t) i] = monoHistory[(size_t) index] * window[(size_t) i];
        }
        std::fill (fftWork.begin() + n, fftWork.end(), 0.0f);

        fft->performFrequencyOnlyForwardTransform (fftWork.data(), true);

        const double binScale = 4.0 / (double) n;   // Hann coherent gain 0.5

        for (int k = 1; k < n / 2; ++k)
            rawDb[(size_t) k] = (float) linearToDb ((double) fftWork[(size_t) k] * binScale);

        const double binHz = sampleRate / (double) n;

        // Log-spaced display spectrum (128 bands, 20 Hz .. 20 kHz).
        const double top = std::min (20000.0, sampleRate * 0.49);
        for (int band = 0; band < kDisplayBins; ++band)
        {
            const double t0 = (double) band / (double) kDisplayBins;
            const double t1 = (double) (band + 1) / (double) kDisplayBins;
            const double f0 = 20.0 * std::pow (top / 20.0, t0);
            const double f1 = 20.0 * std::pow (top / 20.0, t1);

            const int k0 = std::max (1, (int) std::floor (f0 / binHz));
            const int k1 = std::max (k0, std::min (n / 2 - 1, (int) std::ceil (f1 / binHz)));

            double energy = 0.0;
            for (int k = k0; k <= k1; ++k)
                energy += meanSquareForBin (k, n);

            const double db = energy > 1.0e-20 ? 10.0 * std::log10 (energy) : kMinusInfinityDb;

            // Fast attack, slow release smoothing for a usable display.
            const float previous = displayDb[(size_t) band];
            displayDb[(size_t) band] = (float) (db > (double) previous
                                                    ? 0.5 * previous + 0.5 * db
                                                    : 0.9 * previous + 0.1 * db);
        }
    }

    inline double meanSquareForBin (int k, int n) const
    {
        const double magnitude = (double) fftWork[(size_t) k];
        return 2.0 * magnitude * magnitude / ((double) n * windowSumSquares);
    }

    double bandEnergyDb (double lowHz, double highHz) const
    {
        if (fft == nullptr)
            return kMinusInfinityDb;

        const int n = fftSize;
        const double binHz = sampleRate / (double) n;
        const int k0 = std::max (1, (int) std::ceil (lowHz / binHz));
        const int k1 = std::min (n / 2 - 1, (int) std::floor (highHz / binHz));

        double energy = 0.0;
        for (int k = k0; k <= k1; ++k)
            energy += meanSquareForBin (k, n);

        return energy > 1.0e-20 ? 10.0 * std::log10 (energy) : kMinusInfinityDb;
    }

    void computeSpectrogramRow()
    {
        if (fft == nullptr)
            return;

        const double top = std::min (20000.0, sampleRate * 0.49);
        const double binHz = sampleRate / (double) fftSize;
        const int n = fftSize;

        float* row = spectrogram.data() + (size_t) (spectrogramWrite * kSpectrogramBands);

        for (int band = 0; band < kSpectrogramBands; ++band)
        {
            const double t0 = (double) band / (double) kSpectrogramBands;
            const double t1 = (double) (band + 1) / (double) kSpectrogramBands;
            const double f0 = 20.0 * std::pow (top / 20.0, t0);
            const double f1 = 20.0 * std::pow (top / 20.0, t1);
            const int k0 = std::max (1, (int) std::floor (f0 / binHz));
            const int k1 = std::max (k0, std::min (n / 2 - 1, (int) std::ceil (f1 / binHz)));

            double energy = 0.0;
            for (int k = k0; k <= k1; ++k)
                energy += meanSquareForBin (k, n);

            row[band] = (float) (energy > 1.0e-20 ? 10.0 * std::log10 (energy) : kMinusInfinityDb);
        }

        spectrogramWrite = (spectrogramWrite + 1) % kSpectrogramRows;
        spectrogramRows = std::min (spectrogramRows + 1, kSpectrogramRows);
    }

    //== Publishing ==========================================================
    void publishSnapshot()
    {
        computeSpectrum();
        computeSpectrogramRow();

        MeterValues values;
        values.sampleRate = sampleRate;
        values.blockSize = maxBlockSize;

        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            values.peakDb[ch] = (float) linearToDb (peakLinear[ch]);
            values.truePeakDb[ch] = (float) linearToDb (truePeakLinear[ch]);
            values.rmsDb[ch] = (float) linearToDb (std::sqrt (rmsEnergy[ch]));
            values.inputPeakDb[ch] = (float) linearToDb (inputPeakLinear[ch]);
            values.inputRmsDb[ch] = (float) linearToDb (std::sqrt (inputRmsEnergy[ch]));
        }

        values.momentaryLufs = (float) momentaryLufs;
        values.shortTermLufs = (float) shortTermLufs;
        values.integratedLufs = (float) computeIntegratedLufs();
        values.loudnessRangeLu = (float) computeLoudnessRange();

        const double denominator = std::sqrt (std::max (1.0e-20, correlationLL * correlationRR));
        values.correlation = denominator > 0.0
                                 ? (float) std::clamp (correlationLR / denominator, -1.0, 1.0) : 1.0f;

        const double peakDb = std::max ((double) values.peakDb[0], (double) values.peakDb[1]);
        const double rmsDb = std::max ((double) values.rmsDb[0], (double) values.rmsDb[1]);
        values.crestFactorDb = (float) std::max (0.0, peakDb - rmsDb);
        values.samplePeakDb = linearToDb (samplePeakMax);
        values.truePeakMaxDb = linearToDb (truePeakMax);

        values.bassEnergyDb[0] = (float) bandEnergyDb (20.0, 60.0);
        values.bassEnergyDb[1] = (float) bandEnergyDb (60.0, 120.0);
        values.bassEnergyDb[2] = (float) bandEnergyDb (120.0, 250.0);
        values.lowEnergyDb = (float) bandEnergyDb (20.0, 250.0);
        values.midEnergyDb = (float) bandEnergyDb (250.0, 2000.0);
        values.highEnergyDb = (float) bandEnergyDb (2000.0, 16000.0);

        values.clipping = clipHoldFrames > 0;
        values.clipCount = clipCount;
        values.underrunCount = underrunCounter.load (std::memory_order_relaxed);
        values.analysedFrames = analysedFrames;

        const juce::ScopedLock lock (snapshotLock);
        snapshot.values = values;
        snapshot.updateRate = actualUpdateRateHz.load();
        snapshot.displayDb = displayDb;
        snapshot.rawDb = rawDb;
        snapshot.fftSize = fftSize;

        const int rows = spectrogramRows;
        const int firstRow = (spectrogramWrite - rows + kSpectrogramRows) % kSpectrogramRows;
        for (int r = 0; r < rows; ++r)
        {
            const int srcRow = (firstRow + r) % kSpectrogramRows;
            std::memcpy (snapshot.spectrogram.data() + (size_t) (r * kSpectrogramBands),
                         spectrogram.data() + (size_t) (srcRow * kSpectrogramBands),
                         sizeof (float) * (size_t) kSpectrogramBands);
        }
        snapshot.spectrogramRows = rows;
    }
};

//==============================================================================
AnalyzerHub::AnalyzerHub()
    : juce::Thread ("Snake Puppy Analyzer"),
      impl (std::make_unique<Impl>())
{
    impl->inputBuffer.assign ((size_t) (2 * kFifoCapacity), 0.0f);
    impl->outputBuffer.assign ((size_t) (2 * kFifoCapacity), 0.0f);
    impl->monoHistory.assign ((size_t) kMonoHistorySize, 0.0f);
    impl->truePeakDetector.prepare (2);
    impl->configureForSampleRate (48000.0);
    impl->applyFftSize (4096);
    impl->resetAnalysisState (false);
}

AnalyzerHub::~AnalyzerHub()
{
    stop();
}

void AnalyzerHub::prepare (double sampleRate, int maxBlockSize)
{
    // A device change calls this while the analysis thread may be running; pause
    // it briefly so the FFT/state reallocation cannot race the analysis loop.
    const bool wasRunning = isThreadRunning();
    if (wasRunning)
        stop();

    impl->maxBlockSize = std::max (1, maxBlockSize);
    impl->configureForSampleRate (sampleRate);
    impl->applyFftSize (impl->requestedFftSize.load (std::memory_order_relaxed));
    impl->inputFifo.reset();
    impl->outputFifo.reset();
    impl->resetAnalysisState (false);

    if (wasRunning)
        start();
}

void AnalyzerHub::reset()
{
    if (isThreadRunning())
    {
        impl->resetRequested.store (true);
    }
    else
    {
        impl->inputFifo.reset();
        impl->outputFifo.reset();
        impl->resetAnalysisState (false);
    }
}

void AnalyzerHub::start()
{
    if (! isThreadRunning())
        startThread (juce::Thread::Priority::normal);
}

void AnalyzerHub::stop()
{
    stopThread (1000);
}

void AnalyzerHub::run()
{
    while (! threadShouldExit())
    {
        if (impl->resetRequested.exchange (false, std::memory_order_acq_rel))
        {
            // Note: the FIFOs are not reset here because the audio thread may be
            // writing to them; stale frames are simply discarded by the meters.
            impl->resetAnalysisState (false);
        }

        if (impl->deviceResetRequested.exchange (false, std::memory_order_acq_rel))
        {
            impl->truePeakDetector.reset();
            for (int ch = 0; ch < Impl::kMaxChannels; ++ch)
            {
                impl->peakLinear[ch] = 0.0;
                impl->truePeakLinear[ch] = 0.0;
                impl->rmsEnergy[ch] = 0.0;
                impl->kShelfState[ch].reset();
                impl->kHighPassState[ch].reset();
            }
        }

        const int requested = impl->requestedFftSize.load (std::memory_order_relaxed);
        if (requested != impl->actualFftSize.load (std::memory_order_relaxed))
            impl->applyFftSize (requested);

        impl->actualUpdateRateHz.store (impl->requestedUpdateRateHz.load (std::memory_order_relaxed));

        impl->drainFifo (impl->outputFifo, impl->outputBuffer, false);
        impl->drainFifo (impl->inputFifo, impl->inputBuffer, true);

        impl->publishSnapshot();

        const int rate = std::clamp (impl->requestedUpdateRateHz.load (std::memory_order_relaxed), 10, 120);
        wait (std::max (5, 1000 / rate));
    }
}

//==============================================================================
void AnalyzerHub::pushInput (const float* left, const float* right, int numFrames) noexcept
{
    if (numFrames <= 0 || left == nullptr || right == nullptr)
        return;

    auto& d = *impl;
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    d.inputFifo.prepareToWrite (numFrames, start1, size1, start2, size2);

    if (size1 + size2 < numFrames)
        return;   // dropped: analysis thread is behind (never blocks the audio thread)

    float* dst = d.inputBuffer.data();
    for (int i = 0; i < size1; ++i)
    {
        dst[2 * (start1 + i)] = left[i];
        dst[2 * (start1 + i) + 1] = right[i];
    }
    for (int i = 0; i < size2; ++i)
    {
        dst[2 * (start2 + i)] = left[size1 + i];
        dst[2 * (start2 + i) + 1] = right[size1 + i];
    }

    d.inputFifo.finishedWrite (size1 + size2);
}

void AnalyzerHub::pushOutput (const float* left, const float* right, int numFrames) noexcept
{
    if (numFrames <= 0 || left == nullptr || right == nullptr)
        return;

    auto& d = *impl;
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    d.outputFifo.prepareToWrite (numFrames, start1, size1, start2, size2);

    if (size1 + size2 < numFrames)
        return;

    float* dst = d.outputBuffer.data();
    for (int i = 0; i < size1; ++i)
    {
        dst[2 * (start1 + i)] = left[i];
        dst[2 * (start1 + i) + 1] = right[i];
    }
    for (int i = 0; i < size2; ++i)
    {
        dst[2 * (start2 + i)] = left[size1 + i];
        dst[2 * (start2 + i) + 1] = right[size1 + i];
    }

    d.outputFifo.finishedWrite (size1 + size2);
}

void AnalyzerHub::notifyUnderrun() noexcept
{
    impl->underrunCounter.fetch_add (1, std::memory_order_relaxed);
}

void AnalyzerHub::notifyDeviceReset() noexcept
{
    impl->deviceResetRequested.store (true, std::memory_order_release);
}

//==============================================================================
MeterValues AnalyzerHub::getMeterValues() const
{
    const juce::ScopedLock lock (impl->snapshotLock);
    return impl->snapshot.values;
}

void AnalyzerHub::getSpectrumDb (std::vector<float>& outDb, std::vector<float>& outFreqsHz) const
{
    const juce::ScopedLock lock (impl->snapshotLock);
    outDb = impl->snapshot.displayDb;
    outFreqsHz = impl->snapshot.displayFreqHz;
}

void AnalyzerHub::getRawSpectrumDb (std::vector<float>& outDb) const
{
    const juce::ScopedLock lock (impl->snapshotLock);
    outDb = impl->snapshot.rawDb;
}

void AnalyzerHub::getSpectrogram (std::vector<float>& outBands, int& numRows, int& numCols) const
{
    const juce::ScopedLock lock (impl->snapshotLock);
    numRows = impl->snapshot.spectrogramRows;
    numCols = kSpectrogramBands;
    outBands.assign ((size_t) (numRows * numCols), (float) kMinusInfinityDb);

    for (int r = 0; r < numRows; ++r)
        for (int c = 0; c < numCols; ++c)
            outBands[(size_t) (r * numCols + c)] = impl->snapshot.spectrogram[(size_t) (r * numCols + c)];
}

void AnalyzerHub::setFftSize (int size)
{
    static const int allowed[] = { 1024, 2048, 4096, 8192, 16384 };
    int best = 4096;
    for (int candidate : allowed)
        if (std::abs (candidate - size) < std::abs (best - size))
            best = candidate;

    impl->requestedFftSize.store (best, std::memory_order_relaxed);
}

int AnalyzerHub::getFftSize() const
{
    return impl->actualFftSize.load (std::memory_order_relaxed);
}

void AnalyzerHub::setDisplayUpdateRateHz (int hz)
{
    impl->requestedUpdateRateHz.store (std::clamp (hz, 10, 120), std::memory_order_relaxed);
}

int AnalyzerHub::getDisplayUpdateRateHz() const
{
    return impl->actualUpdateRateHz.load (std::memory_order_relaxed);
}

} // namespace sp
