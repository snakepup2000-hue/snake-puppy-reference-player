/*
    Snake Puppy Reference Player
    Metering / analysis hub. The audio callback pushes lock-free sample blocks
    (pre-DSP input and post-DSP output). A dedicated analysis thread computes
    true peak, RMS, LUFS (momentary / short-term / integrated), stereo
    correlation, FFT spectrum, spectrogram history, bass energy and clipping.
    The UI polls snapshots; no analysis work happens on the audio thread.
*/
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace sp
{

struct MeterValues
{
    // Peak / RMS in dBFS, per channel.
    float peakDb[2] { -144.0f, -144.0f };
    float truePeakDb[2] { -144.0f, -144.0f };
    float rmsDb[2] { -144.0f, -144.0f };

    // Pre-DSP input levels (for input-vs-output comparison).
    float inputPeakDb[2] { -144.0f, -144.0f };
    float inputRmsDb[2] { -144.0f, -144.0f };

    float momentaryLufs = -144.0f;
    float shortTermLufs = -144.0f;
    float integratedLufs = -144.0f;
    float loudnessRangeLu = 0.0f;

    float correlation = 1.0f;          // -1 .. +1
    float crestFactorDb = 0.0f;        // peak - rms

    bool clipping = false;
    std::int64_t clipCount = 0;
    std::int64_t underrunCount = 0;

    // Bass energy in three bands, dBFS RMS: 20-60, 60-120, 120-250 Hz.
    float bassEnergyDb[3] { -144.0f, -144.0f, -144.0f };
    float lowEnergyDb = -144.0f;       // 20-250
    float midEnergyDb = -144.0f;       // 250-2000
    float highEnergyDb = -144.0f;      // 2000-16000

    double sampleRate = 0.0;
    int blockSize = 0;
    double samplePeakDb = -144.0;
    double truePeakMaxDb = -144.0;

    // Processing detail exposed mainly in diagnostics.
    std::int64_t analysedFrames = 0;
};

class AnalyzerHub : private juce::Thread
{
public:
    AnalyzerHub();
    ~AnalyzerHub() override;

    void prepare (double sampleRate, int maxBlockSize);
    void reset();
    void start();
    void stop();

    //== Called from the audio callback: lock-free pushes ======================
    void pushInput (const float* left, const float* right, int numFrames) noexcept;
    void pushOutput (const float* left, const float* right, int numFrames) noexcept;
    void notifyUnderrun() noexcept;
    void notifyDeviceReset() noexcept;

    //== UI reads =============================================================
    MeterValues getMeterValues() const;

    /** Log-spaced spectrum in dB, `numBins` values from 20 Hz to 20 kHz, plus
        the matching centre frequencies. Suitable for direct display. */
    void getSpectrumDb (std::vector<float>& outDb, std::vector<float>& outFreqsHz) const;

    /** Raw FFT magnitude in dB for the current FFT size (bins 0..N/2). */
    void getRawSpectrumDb (std::vector<float>& outDb) const;

    /** Spectrogram history: rows = time (oldest first), columns = log bands. */
    static constexpr int kSpectrogramBands = 96;
    static constexpr int kSpectrogramHistory = 256;
    void getSpectrogram (std::vector<float>& outBands, int& numRows, int& numCols) const;

    void setFftSize (int size);           // 1024, 2048, 4096, 8192, 16384
    int getFftSize() const;
    void setDisplayUpdateRateHz (int hz); // 20, 30, 60
    int getDisplayUpdateRateHz() const;

    /** True-peak oversampling factor (4x). */
    static constexpr int kTruePeakOversampling = 4;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    void run() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalyzerHub)
};

} // namespace sp
