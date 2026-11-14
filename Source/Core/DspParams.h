/*
    Snake Puppy Reference Player
    DSP parameter model. This is the single source of truth shared by the DSP
    chain, the profile engine, persistence and the UI. All values are plain data
    (no JUCE DSP objects) so they can be snapshot, compared, serialised and
    smooth-ramped.
*/
#pragma once

#include "Core/Types.h"
#include <juce_core/juce_core.h>
#include <array>
#include <vector>

namespace sp
{
//==============================================================================
// Graphic EQ
//==============================================================================
struct GraphicEqParams
{
    enum class BandSet { bands15 = 0, bands31 = 1 };
    static constexpr int kNumBands15 = 15;
    static constexpr int kNumBands31 = 31;

    bool enabled = true;
    BandSet bandSet = BandSet::bands15;
    float rangeDb = 6.0f;                      // 6 or 12 dB (UI range only)
    std::array<float, kNumBands15> gains15 {}; // dB, clamped to +-rangeDb
    std::array<float, kNumBands31> gains31 {}; // dB, clamped to +-rangeDb

    static const std::array<double, kNumBands15>& centres15();
    static const std::array<double, kNumBands31>& centres31();

    float& gainForSet (int bandIndex);
    void reset() { gains15.fill (0.0f); gains31.fill (0.0f); }
    bool isFlat() const;
};

//==============================================================================
// Parametric EQ
//==============================================================================
enum class BandType
{
    bell = 0,
    lowShelf,
    highShelf,
    highPass,
    lowPass,
    notch,
    bandPass,
    allPass
};

const char* bandTypeName (BandType t);
const char* bandTypeShortName (BandType t);

struct EqBand
{
    bool enabled = false;
    bool bypassed = false;          // per-band bypass kept separate from enable
    BandType type = BandType::bell;
    double freqHz = 1000.0;
    double gainDb = 0.0;
    double q = 0.707;
    bool isActive() const { return enabled && ! bypassed; }
};

struct ParametricEqParams
{
    static constexpr int kNumBands = 12;

    bool enabled = true;
    std::array<EqBand, kNumBands> bands;

    ParametricEqParams();
    void reset();
    bool hasActiveBands() const;
};

//==============================================================================
// Dedicated bass management
//==============================================================================
struct BassParams
{
    // The big BASS control on the main window: a low shelf (default 80 Hz).
    bool enabled = true;
    float gainDb = 0.0f;            // -6 .. +6 (extended +-12 when allowed)
    float rangeDb = 6.0f;
    double shelfFreqHz = 80.0;
    double shelfQ = 0.707;          // slope / bandwidth of the shelf
    double shelfS = 1.0;            // shelf slope parameter (0.5..1.5)

    // Dedicated sub controls, each an independent peaking band.
    bool subEnabled = false;
    float subGainDb = 0.0f;
    double subFreqHz = 35.0;
    double subQ = 0.9;

    bool midBassEnabled = false;
    float midBassGainDb = 0.0f;
    double midBassFreqHz = 100.0;
    double midBassQ = 1.0;

    bool punchEnabled = false;
    float punchGainDb = 0.0f;
    double punchFreqHz = 140.0;
    double punchQ = 0.9;

    bool isActive() const;
    /** Sum of all positive bass gains, used for automatic headroom. */
    double maxPositiveGainDb() const;
    void reset();
};

//==============================================================================
struct FilterParams
{
    bool highPassEnabled = false;
    double highPassHz = 20.0;
    int highPassSlopeDbPerOct = 12;     // 12, 24, 48

    bool lowPassEnabled = false;
    double lowPassHz = 20000.0;
    int lowPassSlopeDbPerOct = 12;

    bool isActive() const { return highPassEnabled || lowPassEnabled; }
};

//==============================================================================
struct StereoParams
{
    float balance = 0.0f;               // -1 left .. +1 right
    float widthPercent = 100.0f;        // 0 mono .. 200 wide
    bool monoBassEnabled = false;
    double monoBassHz = 120.0;
    bool protectMono = true;            // limit width if it would hurt mono sum

    bool crossfeedEnabled = false;
    float crossfeedAmountDb = -6.0f;    // crossfeed attenuation, -3 .. -12 dB
    double crossfeedCutoffHz = 700.0;
    float crossfeedDelayUs = 250.0f;    // interaural time difference

    bool isActive() const;
    void reset();
};

//==============================================================================
struct ReplayGainParams
{
    bool enabled = true;
    bool albumMode = false;
    bool preventClipping = true;
    float preampDb = 0.0f;
    float fallbackGainDb = 0.0f;        // used when a file has no RG tags
};

//==============================================================================
struct ConvolutionParams
{
    bool enabled = false;
    juce::String irPath;
    juce::String irName;
    float mixPercent = 100.0f;
    bool trueStereo = false;            // false: stereo file used on both channels
};

//==============================================================================
struct PreampParams
{
    bool autoHeadroom = false;          // optional: reduce preamp to cover positive EQ gain
    float manualPreampDb = 0.0f;        // user trim, -24 .. +12
    bool limiterEnabled = true;
    float outputCeilingDb = -0.3f;      // true-peak ceiling, -6 .. 0
    float limiterReleaseMs = 150.0f;

    bool isActive() const { return limiterEnabled; }
};

//==============================================================================
/** Processor slots, in processing order. Order is user-configurable. */
enum class DspSlot
{
    replayGain = 0,
    graphicEq,
    parametricEq,
    bass,
    filters,
    convolution,
    crossfeed,
    stereoWidth,
    balance,
    preamp,
    limiter,
    numSlots
};

const char* dspSlotName (DspSlot s);
const char* dspSlotDescription (DspSlot s);

//==============================================================================
/** Complete DSP state for one listening context. */
struct DspParams
{
    bool bypassed = false;              // Reference Mode: everything off
    bool eqBypass = false;              // EQ-only bypass switch (does not stop other DSP)

    GraphicEqParams graphic;
    ParametricEqParams parametric;
    BassParams bass;
    FilterParams filters;
    StereoParams stereo;
    ConvolutionParams convolution;
    ReplayGainParams replayGain;
    PreampParams preamp;

    std::array<DspSlot, static_cast<size_t> (DspSlot::numSlots)> chainOrder {};

    DspParams();
    void reset();

    /** ReplayGain gain to apply for this track, given enabled state. */
    float replayGainFor (bool hasTrackGain, float trackGainDb,
                         bool hasAlbumGain, float albumGainDb) const;

    /** Max positive boost produced by EQ + bass + preamp, in dB. */
    double maxPositiveBoostDb() const;

    /** Is any signal-altering processor active? Reference/bit-perfect checks use this. */
    bool hasActiveProcessing (bool volumeIsUnity) const;

    std::vector<juce::String> activeProcessorNames() const;
    bool operator== (const DspParams&) const = delete;   // compare via equals()
    bool equals (const DspParams& other) const;
};

//==============================================================================
/** A named, persistable sound profile (EQ preset or genre profile). */
struct EqProfile
{
    juce::String id;                    // stable UUID string
    juce::String name;
    juce::String genre;                 // for genre profiles
    juce::String description;
    bool builtIn = false;
    juce::String deviceKey;             // optional binding to a device profile
    DspParams params;

    EqProfile() = default;
};

//==============================================================================
/** A/B preset comparison entry. */
struct AbSnapshot
{
    juce::String name;
    DspParams params;
};

//==============================================================================
/** Headphone / speaker / DAC-specific DSP configuration. */
struct DeviceDspProfile
{
    juce::String id;
    DeviceProfileKey key;
    juce::String name;
    DspParams params;
    juce::String eqProfileId;
};

//==============================================================================
/** Preference learning scopes. */
enum class LearnScope { off = 0, genre, album, song };

const char* learnScopeName (LearnScope s);

} // namespace sp
