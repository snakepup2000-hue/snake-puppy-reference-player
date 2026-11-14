/*
    Snake Puppy Reference Player
    Real-time equaliser processors: 15/31-band graphic EQ, 12-band parametric
    EQ, dedicated bass management and the HP/LP filter section.

    All processors:
      * are prepared with the sample rate, maximum block size and channel count;
      * preallocate everything in prepare() (no allocation/locks in process());
      * accept target parameters through setParams() together with a ramp length
        in samples. Every gain/frequency/Q change is smoothed, and sections are
        faded in/out against the dry signal so enabling, disabling, band-set
        changes and profile changes never click;
      * process in place on non-interleaved double buffers.

    Threading: prepare/reset/setParams must be called from the message or decode
    thread while audio may be running, or from the audio thread when the DSP
    chain applies an atomic parameter snapshot. process() is audio-thread only.
*/
#pragma once

#include "Core/DspParams.h"
#include "DSP/Biquad.h"
#include "DSP/DspSmoothing.h"
#include <array>

namespace sp
{

//==============================================================================
/** 15 or 31 band graphic equaliser built from a cascade of RBJ peaking filters.

    The 15-band set uses 2/3-octave band centres (Q = 2.145) and the 31-band set
    1/3-octave centres (Q = 4.32), matching the ISO preferred frequencies from
    Core/DspParams. Gains are clamped to +-12 dB (the Core `rangeDb` is a UI
    range, not a hard signal limit). Switching band set crossfades through the
    dry signal so the topology change cannot click.
*/
class GraphicEqProcessor
{
public:
    static constexpr int kMaxChannels = 8;

    GraphicEqProcessor();

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    void setParams (const GraphicEqParams& params, bool eqBypass, int rampSamples);

    void process (double* const* channels, int numChannels, int numFrames) noexcept;

    /** Theoretical gain of the current target curve at one frequency (dB). */
    double magnitudeDbAt (double freqHz) const;

private:
    struct BandBank
    {
        int count = 0;
        std::array<double, GraphicEqParams::kNumBands31> centreHz {};
        std::array<double, GraphicEqParams::kNumBands31> q {};
        std::array<BiquadCoeffs, GraphicEqParams::kNumBands31> coeffs {};
        std::array<std::array<BiquadState, GraphicEqParams::kNumBands31>, kMaxChannels> states {};
        std::array<LinearSmooth, GraphicEqParams::kNumBands31> gainSmooth {};
        std::array<double, GraphicEqParams::kNumBands31> designedGain {};
        std::array<bool, GraphicEqParams::kNumBands31> coeffValid {};
    };

    void updateBankCoefficients (BandBank& bank, int numFrames) noexcept;
    void resetBankStates (BandBank& bank) noexcept;
    void processBank (BandBank& bank, double* const* channels, int numChannels,
                      int numFrames, bool blend, SampleRamp& sectionMix) noexcept;

    double sampleRate = 48000.0;
    int maxBlock = 512;
    int numChannelsPrepared = 2;
    int rampSamples = 0;

    BandBank bank15;
    BandBank bank31;

    int activeBank = 0;             // 0 = 15 bands, 1 = 31 bands
    int targetBank = 0;
    bool pendingBankSwitch = false;

    SampleRamp sectionMix { 0.0 };
    bool wantActive = false;
};

//==============================================================================
/** 12-band parametric EQ.

    Every band runs a fully independent biquad and has its own fade in/out. Band
    type changes, enable/disable and per-band bypass all crossfade, so editing a
    profile while audio plays is click-free.
*/
class ParametricEqProcessor
{
public:
    static constexpr int kMaxChannels = 8;
    static constexpr int kNumBands = ParametricEqParams::kNumBands;

    ParametricEqProcessor();

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    void setParams (const ParametricEqParams& params, bool eqBypass, int rampSamples);

    void process (double* const* channels, int numChannels, int numFrames) noexcept;

    /** Theoretical gain of the target bands at one frequency (dB). */
    double magnitudeDbAt (double freqHz) const;

private:
    struct Band
    {
        int type = 0;               // BandType
        int targetType = 0;
        bool wantActive = false;

        LinearSmooth gainDb;
        LinearSmooth log2Freq;
        LinearSmooth log2Q;
        SampleRamp mix { 0.0 };

        BiquadCoeffs coeffs;
        bool coeffValid = false;
        double designedGain = 0.0;
        double designedFreq = 0.0;
        double designedQ = 0.0;
        int designedType = -1;

        std::array<BiquadState, kMaxChannels> states {};
    };

    void updateBandCoefficients (Band& band, int numFrames) noexcept;
    void activateBand (Band& band);

    double sampleRate = 48000.0;
    int maxBlock = 512;
    int numChannelsPrepared = 2;
    int rampSamples = 0;

    std::array<Band, kNumBands> bands;
};

//==============================================================================
/** Dedicated bass management: one main low shelf plus sub / mid-bass / punch
    peaking bands. All four bands are plain gain stages (0 dB is bit-transparent),
    so enable/disable and gain changes are smoothed in the coefficient domain.

    The main shelf slope comes from `shelfS`; if `shelfS` is left at its default
    (1.0) but `shelfQ` has been moved off 0.707 the Q is converted to the
    equivalent RBJ slope so both Core controls have an effect.
*/
class BassProcessor
{
public:
    static constexpr int kMaxChannels = 8;
    static constexpr int kNumBands = 4;   // main shelf, sub, mid-bass, punch

    BassProcessor();

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    void setParams (const BassParams& params, int rampSamples);

    void process (double* const* channels, int numChannels, int numFrames) noexcept;

    /** Theoretical gain of the target bass curve at one frequency (dB). */
    double magnitudeDbAt (double freqHz) const;

private:
    enum BandIndex { main = 0, sub = 1, midBass = 2, punch = 3 };

    void updateCoefficients (int numFrames) noexcept;

    double sampleRate = 48000.0;
    int maxBlock = 512;
    int numChannelsPrepared = 2;
    int rampSamples = 0;

    double targetShelfS = 1.0;
    std::array<LinearSmooth, kNumBands> gainDb;
    std::array<LinearSmooth, kNumBands> log2Freq;
    std::array<LinearSmooth, kNumBands> log2Q;
    std::array<BiquadCoeffs, kNumBands> coeffs;
    std::array<std::array<BiquadState, kNumBands>, kMaxChannels> states {};

    std::array<bool, kNumBands> coeffValid {};
    std::array<double, kNumBands> designedGain {};
    std::array<double, kNumBands> designedFreq {};
    std::array<double, kNumBands> designedQ {};
    bool wasSkipping = true;
};

//==============================================================================
/** High-pass and low-pass section, 12 / 24 / 48 dB per octave (Butterworth
    cascades). Stage Q and cutoff are smoothed; stages fade in and out so slope
    changes while playing do not click.
*/
class FilterProcessor
{
public:
    static constexpr int kMaxChannels = 8;
    static constexpr int kMaxStages = 4;

    FilterProcessor();

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    void setParams (const FilterParams& params, int rampSamples);

    void process (double* const* channels, int numChannels, int numFrames) noexcept;

    /** Theoretical gain of the target HP/LP section at one frequency (dB). */
    double magnitudeDbAt (double freqHz) const;

private:
    struct Stage
    {
        LinearSmooth log2Q;
        SampleRamp mix { 0.0 };
        BiquadCoeffs coeffs;
        bool coeffValid = false;
        double designedQ = 0.0;
        std::array<BiquadState, kMaxChannels> states {};
    };

    struct Bank
    {
        bool highPass = true;
        bool wantActive = false;
        LinearSmooth log2Cutoff;
        std::array<Stage, kMaxStages> stages;
        int numStagesActive = 0;
    };

    void updateBankCoefficients (Bank& bank, int numFrames) noexcept;
    void resetBankStates (Bank& bank) noexcept;
    void processBank (Bank& bank, double* const* channels, int numChannels, int numFrames) noexcept;
    bool bankIsAudible (const Bank& bank) const noexcept;

    double sampleRate = 48000.0;
    int maxBlock = 512;
    int numChannelsPrepared = 2;
    int rampSamples = 0;

    Bank hp;
    Bank lp;
};

} // namespace sp
