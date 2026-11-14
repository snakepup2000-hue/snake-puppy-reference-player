#include "Core/DspParams.h"
#include "Core/Serialization.h"
#include <algorithm>
#include <cmath>

namespace sp
{

//==============================================================================
// Graphic EQ
//==============================================================================
const std::array<double, GraphicEqParams::kNumBands15>& GraphicEqParams::centres15()
{
    static const std::array<double, kNumBands15> c {
        25.0, 40.0, 63.0, 100.0, 160.0, 250.0, 400.0, 630.0,
        1000.0, 1600.0, 2500.0, 4000.0, 6300.0, 10000.0, 16000.0
    };
    return c;
}

const std::array<double, GraphicEqParams::kNumBands31>& GraphicEqParams::centres31()
{
    static const std::array<double, kNumBands31> c {
        20.0, 25.0, 31.5, 40.0, 50.0, 63.0, 80.0, 100.0, 125.0, 160.0,
        200.0, 250.0, 315.0, 400.0, 500.0, 630.0, 800.0, 1000.0, 1250.0, 1600.0,
        2000.0, 2500.0, 3150.0, 4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0,
        20000.0
    };
    return c;
}

float& GraphicEqParams::gainForSet (int bandIndex)
{
    if (bandSet == BandSet::bands31)
        return gains31[(size_t) juce::jlimit (0, kNumBands31 - 1, bandIndex)];
    return gains15[(size_t) juce::jlimit (0, kNumBands15 - 1, bandIndex)];
}

bool GraphicEqParams::isFlat() const
{
    for (auto g : gains15) if (std::abs (g) > 0.0001f) return false;
    for (auto g : gains31) if (std::abs (g) > 0.0001f) return false;
    return true;
}

//==============================================================================
// Parametric EQ
//==============================================================================
const char* bandTypeName (BandType t)
{
    switch (t)
    {
        case BandType::bell:      return "Bell";
        case BandType::lowShelf:  return "Low Shelf";
        case BandType::highShelf: return "High Shelf";
        case BandType::highPass:  return "High Pass";
        case BandType::lowPass:   return "Low Pass";
        case BandType::notch:     return "Notch";
        case BandType::bandPass:  return "Band Pass";
        case BandType::allPass:   return "All Pass";
    }
    return "Bell";
}

const char* bandTypeShortName (BandType t)
{
    switch (t)
    {
        case BandType::bell:      return "BELL";
        case BandType::lowShelf:  return "LSH";
        case BandType::highShelf: return "HSH";
        case BandType::highPass:  return "HPF";
        case BandType::lowPass:   return "LPF";
        case BandType::notch:     return "NOTCH";
        case BandType::bandPass:  return "BPF";
        case BandType::allPass:   return "APF";
    }
    return "BELL";
}

ParametricEqParams::ParametricEqParams()
{
    reset();
}

void ParametricEqParams::reset()
{
    const BandType defaults[kNumBands] = {
        BandType::lowShelf, BandType::bell, BandType::bell, BandType::bell,
        BandType::bell, BandType::bell, BandType::bell, BandType::bell,
        BandType::highShelf, BandType::highPass, BandType::lowPass, BandType::bell
    };
    const double freqs[kNumBands] = { 80, 120, 250, 500, 1000, 2000, 4000, 8000, 10000, 20, 20000, 3000 };
    static const double defaultQ[kNumBands] = { 0.707, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.707, 0.707, 0.707, 1.0 };

    for (int i = 0; i < kNumBands; ++i)
    {
        bands[(size_t) i] = EqBand {};
        bands[(size_t) i].enabled = false;
        bands[(size_t) i].bypassed = false;
        bands[(size_t) i].type = defaults[i];
        bands[(size_t) i].freqHz = freqs[i];
        bands[(size_t) i].gainDb = 0.0;
        bands[(size_t) i].q = defaultQ[i];
    }
    enabled = true;
}

bool ParametricEqParams::hasActiveBands() const
{
    for (auto& b : bands)
        if (b.isActive() && (std::abs (b.gainDb) > 0.0001 || b.type == BandType::highPass
                             || b.type == BandType::lowPass || b.type == BandType::notch
                             || b.type == BandType::bandPass || b.type == BandType::allPass))
            return true;
    return false;
}

//==============================================================================
// Bass
//==============================================================================
bool BassParams::isActive() const
{
    if (! enabled) return false;
    return std::abs (gainDb) > 0.0001f
        || (subEnabled && std::abs (subGainDb) > 0.0001f)
        || (midBassEnabled && std::abs (midBassGainDb) > 0.0001f)
        || (punchEnabled && std::abs (punchGainDb) > 0.0001f);
}

double BassParams::maxPositiveGainDb() const
{
    if (! enabled) return 0.0;
    double total = juce::jmax (0.0f, gainDb);
    if (subEnabled)     total += juce::jmax (0.0f, subGainDb);
    if (midBassEnabled) total += juce::jmax (0.0f, midBassGainDb);
    if (punchEnabled)   total += juce::jmax (0.0f, punchGainDb);
    return total;
}

void BassParams::reset()
{
    enabled = true;
    gainDb = 0.0f;
    rangeDb = 6.0f;
    shelfFreqHz = 80.0;
    shelfQ = 0.707;
    shelfS = 1.0;
    subEnabled = false; subGainDb = 0.0f; subFreqHz = 35.0; subQ = 0.9;
    midBassEnabled = false; midBassGainDb = 0.0f; midBassFreqHz = 100.0; midBassQ = 1.0;
    punchEnabled = false; punchGainDb = 0.0f; punchFreqHz = 140.0; punchQ = 0.9;
}

//==============================================================================
bool StereoParams::isActive() const
{
    return std::abs (balance) > 0.0001f
        || std::abs (widthPercent - 100.0f) > 0.01f
        || monoBassEnabled
        || crossfeedEnabled;
}

void StereoParams::reset()
{
    balance = 0.0f;
    widthPercent = 100.0f;
    monoBassEnabled = false;
    monoBassHz = 120.0;
    protectMono = true;
    crossfeedEnabled = false;
    crossfeedAmountDb = -6.0f;
    crossfeedCutoffHz = 700.0;
    crossfeedDelayUs = 250.0f;
}

//==============================================================================
const char* dspSlotName (DspSlot s)
{
    switch (s)
    {
        case DspSlot::replayGain:   return "ReplayGain";
        case DspSlot::graphicEq:    return "Graphic EQ";
        case DspSlot::parametricEq: return "Parametric EQ";
        case DspSlot::bass:         return "Bass Management";
        case DspSlot::filters:      return "High / Low Pass";
        case DspSlot::convolution:  return "Convolution";
        case DspSlot::crossfeed:    return "Crossfeed";
        case DspSlot::stereoWidth:  return "Stereo Width";
        case DspSlot::balance:      return "Balance";
        case DspSlot::preamp:       return "Preamp / Headroom";
        case DspSlot::limiter:      return "Output Limiter";
        case DspSlot::numSlots:     break;
    }
    return "";
}

const char* dspSlotDescription (DspSlot s)
{
    switch (s)
    {
        case DspSlot::replayGain:   return "Applies track or album ReplayGain and optional clipping protection";
        case DspSlot::graphicEq:    return "15 or 31 band graphic equaliser";
        case DspSlot::parametricEq: return "12 fully parametric bands (bell, shelves, pass filters)";
        case DspSlot::bass:         return "Low shelf plus sub, mid-bass and punch bands";
        case DspSlot::filters:      return "Adjustable high-pass and low-pass filters";
        case DspSlot::convolution:  return "Convolution with a compatible impulse response file";
        case DspSlot::crossfeed:    return "Headphone crossfeed for a more natural stereo image";
        case DspSlot::stereoWidth:  return "Mid/side width with mono-compatibility protection";
        case DspSlot::balance:      return "Left / right channel balance";
        case DspSlot::preamp:       return "Manual preamp trim plus automatic headroom";
        case DspSlot::limiter:      return "Transparent output ceiling protection";
        case DspSlot::numSlots:     break;
    }
    return "";
}

//==============================================================================
DspParams::DspParams()
{
    reset();
}

void DspParams::reset()
{
    bypassed = false;
    eqBypass = false;

    graphic = GraphicEqParams {};
    parametric.reset();
    bass.reset();
    filters = FilterParams {};
    stereo.reset();
    convolution = ConvolutionParams {};
    replayGain = ReplayGainParams {};
    preamp = PreampParams {};

    chainOrder = { DspSlot::replayGain, DspSlot::graphicEq, DspSlot::parametricEq,
                   DspSlot::bass, DspSlot::filters, DspSlot::convolution,
                   DspSlot::crossfeed, DspSlot::stereoWidth, DspSlot::balance,
                   DspSlot::preamp, DspSlot::limiter };
}

float DspParams::replayGainFor (bool hasTrackGain, float trackGainDb,
                                bool hasAlbumGain, float albumGainDb) const
{
    if (! replayGain.enabled)
        return 0.0f;

    float gain;
    if (replayGain.albumMode && hasAlbumGain)
        gain = albumGainDb;
    else if (hasTrackGain)
        gain = trackGainDb;
    else
        gain = replayGain.fallbackGainDb;

    gain += replayGain.preampDb;
    return gain;
}

double DspParams::maxPositiveBoostDb() const
{
    if (bypassed)
        return 0.0;

    double total = 0.0;

    if (! eqBypass)
    {
        if (graphic.enabled)
        {
            if (graphic.bandSet == GraphicEqParams::BandSet::bands31)
            {
                for (auto g : graphic.gains31) total += juce::jmax (0.0f, g);
            }
            else
            {
                for (auto g : graphic.gains15) total += juce::jmax (0.0f, g);
            }
        }

        if (parametric.enabled)
        {
            for (auto& b : parametric.bands)
                if (b.isActive())
                    total += juce::jmax (0.0, b.gainDb);
        }
    }

    total += bass.maxPositiveGainDb();
    return juce::jlimit (0.0, 48.0, total);
}

bool DspParams::hasActiveProcessing (bool volumeIsUnity) const
{
    if (bypassed)
        return ! volumeIsUnity;

    if (! volumeIsUnity)
        return true;

    if (! eqBypass)
    {
        if (graphic.enabled && ! graphic.isFlat()) return true;
        if (parametric.enabled && parametric.hasActiveBands()) return true;
        if (bass.isActive()) return true;
    }

    if (filters.isActive()) return true;
    if (convolution.enabled && convolution.irPath.isNotEmpty()) return true;
    if (stereo.isActive()) return true;
    if (replayGain.enabled) return true;                 // RG may or may not be applied; engine decides precisely
    if (std::abs (preamp.manualPreampDb) > 0.0001f) return true;
    if (preamp.limiterEnabled) return true;
    if (preamp.autoHeadroom && maxPositiveBoostDb() > 0.0001) return true;
    return false;
}

std::vector<juce::String> DspParams::activeProcessorNames() const
{
    std::vector<juce::String> names;
    if (bypassed)
        return names;

    for (auto slot : chainOrder)
    {
        bool active = false;
        switch (slot)
        {
            case DspSlot::replayGain:   active = replayGain.enabled; break;
            case DspSlot::graphicEq:    active = graphic.enabled && ! graphic.isFlat() && ! eqBypass; break;
            case DspSlot::parametricEq: active = parametric.enabled && parametric.hasActiveBands() && ! eqBypass; break;
            case DspSlot::bass:         active = bass.isActive() && ! eqBypass; break;
            case DspSlot::filters:      active = filters.isActive(); break;
            case DspSlot::convolution:  active = convolution.enabled && convolution.irPath.isNotEmpty() && convolution.mixPercent > 0.0f; break;
            case DspSlot::crossfeed:    active = stereo.crossfeedEnabled; break;
            case DspSlot::stereoWidth:  active = std::abs (stereo.widthPercent - 100.0f) > 0.01f; break;
            case DspSlot::balance:      active = std::abs (stereo.balance) > 0.0001f; break;
            case DspSlot::preamp:       active = std::abs (preamp.manualPreampDb) > 0.0001f
                                               || (preamp.autoHeadroom && maxPositiveBoostDb() > 0.0001); break;
            case DspSlot::limiter:      active = preamp.limiterEnabled; break;
            case DspSlot::numSlots:     break;
        }
        if (active)
            names.push_back (dspSlotName (slot));
    }
    return names;
}

bool DspParams::equals (const DspParams& o) const
{
    // Deterministic structural comparison via the canonical serialiser (fixed key order).
    return juce::JSON::toString (serialize::toVar (*this)) == juce::JSON::toString (serialize::toVar (o));
}

} // namespace sp
