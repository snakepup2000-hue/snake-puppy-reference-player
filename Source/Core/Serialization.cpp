#include "Core/Serialization.h"

namespace sp::serialize
{
namespace
{
juce::DynamicObject* newObject()
{
    return new juce::DynamicObject();
}

void setIfNotEmpty (juce::DynamicObject& o, const char* key, const juce::String& v)
{
    if (v.isNotEmpty())
        o.setProperty (key, v);
}

juce::var getVar (const juce::var& parent, const char* key, const juce::var& fallback)
{
    if (auto* o = parent.getDynamicObject())
        if (o->hasProperty (key))
            return o->getProperty (key);
    return fallback;
}

double getDouble (const juce::var& parent, const char* key, double fallback)
{
    auto v = getVar (parent, key, fallback);
    return v.isVoid() ? fallback : (double) v;
}

int getInt (const juce::var& parent, const char* key, int fallback)
{
    auto v = getVar (parent, key, fallback);
    return v.isVoid() ? fallback : (int) v;
}

bool getBool (const juce::var& parent, const char* key, bool fallback)
{
    auto v = getVar (parent, key, fallback);
    return v.isVoid() ? fallback : (bool) v;
}

juce::String getString (const juce::var& parent, const char* key, const juce::String& fallback = {})
{
    auto v = getVar (parent, key, fallback);
    return v.isVoid() ? fallback : v.toString();
}
} // namespace

//==============================================================================
juce::var toVar (const GraphicEqParams& p)
{
    auto* o = newObject();
    o->setProperty ("enabled", p.enabled);
    o->setProperty ("bandSet", (int) p.bandSet);
    o->setProperty ("rangeDb", (double) p.rangeDb);

    juce::Array<juce::var> g15;
    for (auto g : p.gains15) g15.add ((double) g);
    o->setProperty ("gains15", g15);

    juce::Array<juce::var> g31;
    for (auto g : p.gains31) g31.add ((double) g);
    o->setProperty ("gains31", g31);
    return juce::var (o);
}

GraphicEqParams graphicFromVar (const juce::var& v, const GraphicEqParams& fallback)
{
    GraphicEqParams p = fallback;
    if (! v.isObject()) return p;
    p.enabled = getBool (v, "enabled", p.enabled);
    p.bandSet = (GraphicEqParams::BandSet) juce::jlimit (0, 1, getInt (v, "bandSet", (int) p.bandSet));
    p.rangeDb = (float) juce::jlimit (6.0, 12.0, getDouble (v, "rangeDb", (double) p.rangeDb));

    if (auto* arr = getVar (v, "gains15", {}).getArray())
        for (int i = 0; i < juce::jmin (arr->size(), GraphicEqParams::kNumBands15); ++i)
            p.gains15[(size_t) i] = (float) juce::jlimit (-12.0, 12.0, (double) arr->getUnchecked (i));

    if (auto* arr = getVar (v, "gains31", {}).getArray())
        for (int i = 0; i < juce::jmin (arr->size(), GraphicEqParams::kNumBands31); ++i)
            p.gains31[(size_t) i] = (float) juce::jlimit (-12.0, 12.0, (double) arr->getUnchecked (i));
    return p;
}

//==============================================================================
juce::var toVar (const EqBand& b)
{
    auto* o = newObject();
    o->setProperty ("enabled", b.enabled);
    o->setProperty ("bypassed", b.bypassed);
    o->setProperty ("type", (int) b.type);
    o->setProperty ("freqHz", b.freqHz);
    o->setProperty ("gainDb", b.gainDb);
    o->setProperty ("q", b.q);
    return juce::var (o);
}

EqBand bandFromVar (const juce::var& v, const EqBand& fallback)
{
    EqBand b = fallback;
    if (! v.isObject()) return b;
    b.enabled = getBool (v, "enabled", b.enabled);
    b.bypassed = getBool (v, "bypassed", b.bypassed);
    b.type = (BandType) juce::jlimit (0, (int) BandType::allPass, getInt (v, "type", (int) b.type));
    b.freqHz = juce::jlimit (10.0, 22000.0, getDouble (v, "freqHz", b.freqHz));
    b.gainDb = juce::jlimit (-24.0, 24.0, getDouble (v, "gainDb", b.gainDb));
    b.q = juce::jlimit (0.05, 20.0, getDouble (v, "q", b.q));
    return b;
}

juce::var toVar (const ParametricEqParams& p)
{
    auto* o = newObject();
    o->setProperty ("enabled", p.enabled);
    juce::Array<juce::var> bands;
    for (auto& b : p.bands) bands.add (toVar (b));
    o->setProperty ("bands", bands);
    return juce::var (o);
}

ParametricEqParams parametricFromVar (const juce::var& v, const ParametricEqParams& fallback)
{
    ParametricEqParams p = fallback;
    if (! v.isObject()) return p;
    p.enabled = getBool (v, "enabled", p.enabled);
    if (auto* arr = getVar (v, "bands", {}).getArray())
        for (int i = 0; i < juce::jmin (arr->size(), ParametricEqParams::kNumBands); ++i)
            p.bands[(size_t) i] = bandFromVar (arr->getUnchecked (i), p.bands[(size_t) i]);
    return p;
}

//==============================================================================
juce::var toVar (const BassParams& p)
{
    auto* o = newObject();
    o->setProperty ("enabled", p.enabled);
    o->setProperty ("gainDb", (double) p.gainDb);
    o->setProperty ("rangeDb", (double) p.rangeDb);
    o->setProperty ("shelfFreqHz", p.shelfFreqHz);
    o->setProperty ("shelfQ", p.shelfQ);
    o->setProperty ("shelfS", p.shelfS);

    auto* sub = newObject();
    sub->setProperty ("enabled", p.subEnabled);
    sub->setProperty ("gainDb", (double) p.subGainDb);
    sub->setProperty ("freqHz", p.subFreqHz);
    sub->setProperty ("q", p.subQ);
    o->setProperty ("sub", juce::var (sub));

    auto* mid = newObject();
    mid->setProperty ("enabled", p.midBassEnabled);
    mid->setProperty ("gainDb", (double) p.midBassGainDb);
    mid->setProperty ("freqHz", p.midBassFreqHz);
    mid->setProperty ("q", p.midBassQ);
    o->setProperty ("midBass", juce::var (mid));

    auto* punch = newObject();
    punch->setProperty ("enabled", p.punchEnabled);
    punch->setProperty ("gainDb", (double) p.punchGainDb);
    punch->setProperty ("freqHz", p.punchFreqHz);
    punch->setProperty ("q", p.punchQ);
    o->setProperty ("punch", juce::var (punch));
    return juce::var (o);
}

BassParams bassFromVar (const juce::var& v, const BassParams& fallback)
{
    BassParams p = fallback;
    if (! v.isObject()) return p;
    p.enabled = getBool (v, "enabled", p.enabled);
    p.gainDb = (float) juce::jlimit (-12.0, 12.0, getDouble (v, "gainDb", p.gainDb));
    p.rangeDb = (float) juce::jlimit (6.0, 12.0, getDouble (v, "rangeDb", p.rangeDb));
    p.shelfFreqHz = juce::jlimit (20.0, 200.0, getDouble (v, "shelfFreqHz", p.shelfFreqHz));
    p.shelfQ = juce::jlimit (0.3, 2.0, getDouble (v, "shelfQ", p.shelfQ));
    p.shelfS = juce::jlimit (0.3, 2.0, getDouble (v, "shelfS", p.shelfS));

    auto sub = getVar (v, "sub", {});
    p.subEnabled = getBool (sub, "enabled", p.subEnabled);
    p.subGainDb = (float) juce::jlimit (-12.0, 12.0, getDouble (sub, "gainDb", p.subGainDb));
    p.subFreqHz = juce::jlimit (15.0, 80.0, getDouble (sub, "freqHz", p.subFreqHz));
    p.subQ = juce::jlimit (0.3, 4.0, getDouble (sub, "q", p.subQ));

    auto mid = getVar (v, "midBass", {});
    p.midBassEnabled = getBool (mid, "enabled", p.midBassEnabled);
    p.midBassGainDb = (float) juce::jlimit (-12.0, 12.0, getDouble (mid, "gainDb", p.midBassGainDb));
    p.midBassFreqHz = juce::jlimit (40.0, 300.0, getDouble (mid, "freqHz", p.midBassFreqHz));
    p.midBassQ = juce::jlimit (0.3, 4.0, getDouble (mid, "q", p.midBassQ));

    auto punch = getVar (v, "punch", {});
    p.punchEnabled = getBool (punch, "enabled", p.punchEnabled);
    p.punchGainDb = (float) juce::jlimit (-12.0, 12.0, getDouble (punch, "gainDb", p.punchGainDb));
    p.punchFreqHz = juce::jlimit (60.0, 320.0, getDouble (punch, "freqHz", p.punchFreqHz));
    p.punchQ = juce::jlimit (0.3, 4.0, getDouble (punch, "q", p.punchQ));
    return p;
}

//==============================================================================
juce::var toVar (const FilterParams& p)
{
    auto* o = newObject();
    o->setProperty ("highPassEnabled", p.highPassEnabled);
    o->setProperty ("highPassHz", p.highPassHz);
    o->setProperty ("highPassSlopeDbPerOct", p.highPassSlopeDbPerOct);
    o->setProperty ("lowPassEnabled", p.lowPassEnabled);
    o->setProperty ("lowPassHz", p.lowPassHz);
    o->setProperty ("lowPassSlopeDbPerOct", p.lowPassSlopeDbPerOct);
    return juce::var (o);
}

FilterParams filtersFromVar (const juce::var& v, const FilterParams& fallback)
{
    FilterParams p = fallback;
    if (! v.isObject()) return p;
    p.highPassEnabled = getBool (v, "highPassEnabled", p.highPassEnabled);
    p.highPassHz = juce::jlimit (10.0, 2000.0, getDouble (v, "highPassHz", p.highPassHz));
    p.highPassSlopeDbPerOct = juce::jlimit (6, 96, getInt (v, "highPassSlopeDbPerOct", p.highPassSlopeDbPerOct));
    p.lowPassEnabled = getBool (v, "lowPassEnabled", p.lowPassEnabled);
    p.lowPassHz = juce::jlimit (1000.0, 22000.0, getDouble (v, "lowPassHz", p.lowPassHz));
    p.lowPassSlopeDbPerOct = juce::jlimit (6, 96, getInt (v, "lowPassSlopeDbPerOct", p.lowPassSlopeDbPerOct));
    return p;
}

//==============================================================================
juce::var toVar (const StereoParams& p)
{
    auto* o = newObject();
    o->setProperty ("balance", (double) p.balance);
    o->setProperty ("widthPercent", (double) p.widthPercent);
    o->setProperty ("monoBassEnabled", p.monoBassEnabled);
    o->setProperty ("monoBassHz", p.monoBassHz);
    o->setProperty ("protectMono", p.protectMono);
    o->setProperty ("crossfeedEnabled", p.crossfeedEnabled);
    o->setProperty ("crossfeedAmountDb", (double) p.crossfeedAmountDb);
    o->setProperty ("crossfeedCutoffHz", p.crossfeedCutoffHz);
    o->setProperty ("crossfeedDelayUs", (double) p.crossfeedDelayUs);
    return juce::var (o);
}

StereoParams stereoFromVar (const juce::var& v, const StereoParams& fallback)
{
    StereoParams p = fallback;
    if (! v.isObject()) return p;
    p.balance = (float) juce::jlimit (-1.0, 1.0, getDouble (v, "balance", p.balance));
    p.widthPercent = (float) juce::jlimit (0.0, 200.0, getDouble (v, "widthPercent", p.widthPercent));
    p.monoBassEnabled = getBool (v, "monoBassEnabled", p.monoBassEnabled);
    p.monoBassHz = juce::jlimit (40.0, 400.0, getDouble (v, "monoBassHz", p.monoBassHz));
    p.protectMono = getBool (v, "protectMono", p.protectMono);
    p.crossfeedEnabled = getBool (v, "crossfeedEnabled", p.crossfeedEnabled);
    p.crossfeedAmountDb = (float) juce::jlimit (-18.0, -1.0, getDouble (v, "crossfeedAmountDb", p.crossfeedAmountDb));
    p.crossfeedCutoffHz = juce::jlimit (200.0, 2000.0, getDouble (v, "crossfeedCutoffHz", p.crossfeedCutoffHz));
    p.crossfeedDelayUs = (float) juce::jlimit (0.0, 1000.0, getDouble (v, "crossfeedDelayUs", p.crossfeedDelayUs));
    return p;
}

//==============================================================================
juce::var toVar (const ReplayGainParams& p)
{
    auto* o = newObject();
    o->setProperty ("enabled", p.enabled);
    o->setProperty ("albumMode", p.albumMode);
    o->setProperty ("preventClipping", p.preventClipping);
    o->setProperty ("preampDb", (double) p.preampDb);
    o->setProperty ("fallbackGainDb", (double) p.fallbackGainDb);
    return juce::var (o);
}

ReplayGainParams replayGainFromVar (const juce::var& v, const ReplayGainParams& fallback)
{
    ReplayGainParams p = fallback;
    if (! v.isObject()) return p;
    p.enabled = getBool (v, "enabled", p.enabled);
    p.albumMode = getBool (v, "albumMode", p.albumMode);
    p.preventClipping = getBool (v, "preventClipping", p.preventClipping);
    p.preampDb = (float) juce::jlimit (-24.0, 24.0, getDouble (v, "preampDb", p.preampDb));
    p.fallbackGainDb = (float) juce::jlimit (-24.0, 24.0, getDouble (v, "fallbackGainDb", p.fallbackGainDb));
    return p;
}

//==============================================================================
juce::var toVar (const ConvolutionParams& p)
{
    auto* o = newObject();
    o->setProperty ("enabled", p.enabled);
    setIfNotEmpty (*o, "irPath", p.irPath);
    setIfNotEmpty (*o, "irName", p.irName);
    o->setProperty ("mixPercent", (double) p.mixPercent);
    o->setProperty ("trueStereo", p.trueStereo);
    return juce::var (o);
}

ConvolutionParams convolutionFromVar (const juce::var& v, const ConvolutionParams& fallback)
{
    ConvolutionParams p = fallback;
    if (! v.isObject()) return p;
    p.enabled = getBool (v, "enabled", p.enabled);
    p.irPath = getString (v, "irPath", p.irPath);
    p.irName = getString (v, "irName", p.irName);
    p.mixPercent = (float) juce::jlimit (0.0, 100.0, getDouble (v, "mixPercent", p.mixPercent));
    p.trueStereo = getBool (v, "trueStereo", p.trueStereo);
    return p;
}

//==============================================================================
juce::var toVar (const PreampParams& p)
{
    auto* o = newObject();
    o->setProperty ("autoHeadroom", p.autoHeadroom);
    o->setProperty ("manualPreampDb", (double) p.manualPreampDb);
    o->setProperty ("limiterEnabled", p.limiterEnabled);
    o->setProperty ("outputCeilingDb", (double) p.outputCeilingDb);
    o->setProperty ("limiterReleaseMs", (double) p.limiterReleaseMs);
    return juce::var (o);
}

PreampParams preampFromVar (const juce::var& v, const PreampParams& fallback)
{
    PreampParams p = fallback;
    if (! v.isObject()) return p;
    p.autoHeadroom = getBool (v, "autoHeadroom", p.autoHeadroom);
    p.manualPreampDb = (float) juce::jlimit (-36.0, 24.0, getDouble (v, "manualPreampDb", p.manualPreampDb));
    p.limiterEnabled = getBool (v, "limiterEnabled", p.limiterEnabled);
    p.outputCeilingDb = (float) juce::jlimit (-12.0, 0.0, getDouble (v, "outputCeilingDb", p.outputCeilingDb));
    p.limiterReleaseMs = (float) juce::jlimit (20.0, 2000.0, getDouble (v, "limiterReleaseMs", p.limiterReleaseMs));
    return p;
}

//==============================================================================
juce::var toVar (const DspParams& p)
{
    auto* o = newObject();
    o->setProperty ("v", 1);
    o->setProperty ("bypassed", p.bypassed);
    o->setProperty ("eqBypass", p.eqBypass);
    o->setProperty ("graphic", toVar (p.graphic));
    o->setProperty ("parametric", toVar (p.parametric));
    o->setProperty ("bass", toVar (p.bass));
    o->setProperty ("filters", toVar (p.filters));
    o->setProperty ("stereo", toVar (p.stereo));
    o->setProperty ("convolution", toVar (p.convolution));
    o->setProperty ("replayGain", toVar (p.replayGain));
    o->setProperty ("preamp", toVar (p.preamp));

    juce::Array<juce::var> order;
    for (auto s : p.chainOrder) order.add ((int) s);
    o->setProperty ("chainOrder", order);
    return juce::var (o);
}

DspParams dspFromVar (const juce::var& v, const DspParams& fallback)
{
    DspParams p = fallback;
    if (! v.isObject()) return p;
    p.bypassed = getBool (v, "bypassed", p.bypassed);
    p.eqBypass = getBool (v, "eqBypass", p.eqBypass);
    p.graphic = graphicFromVar (getVar (v, "graphic", {}), p.graphic);
    p.parametric = parametricFromVar (getVar (v, "parametric", {}), p.parametric);
    p.bass = bassFromVar (getVar (v, "bass", {}), p.bass);
    p.filters = filtersFromVar (getVar (v, "filters", {}), p.filters);
    p.stereo = stereoFromVar (getVar (v, "stereo", {}), p.stereo);
    p.convolution = convolutionFromVar (getVar (v, "convolution", {}), p.convolution);
    p.replayGain = replayGainFromVar (getVar (v, "replayGain", {}), p.replayGain);
    p.preamp = preampFromVar (getVar (v, "preamp", {}), p.preamp);

    if (auto* arr = getVar (v, "chainOrder", {}).getArray())
    {
        for (int i = 0; i < juce::jmin (arr->size(), (int) p.chainOrder.size()); ++i)
        {
            int slot = (int) arr->getUnchecked (i);
            if (slot >= 0 && slot < (int) DspSlot::numSlots)
                p.chainOrder[(size_t) i] = (DspSlot) slot;
        }
    }
    return p;
}

//==============================================================================
juce::var toVar (const EqProfile& p)
{
    auto* o = newObject();
    o->setProperty ("id", p.id);
    o->setProperty ("name", p.name);
    setIfNotEmpty (*o, "genre", p.genre);
    setIfNotEmpty (*o, "description", p.description);
    o->setProperty ("builtIn", p.builtIn);
    setIfNotEmpty (*o, "deviceKey", p.deviceKey);
    o->setProperty ("params", toVar (p.params));
    return juce::var (o);
}

EqProfile eqProfileFromVar (const juce::var& v, const EqProfile& fallback)
{
    EqProfile p = fallback;
    if (! v.isObject()) return p;
    p.id = getString (v, "id", p.id);
    p.name = getString (v, "name", p.name);
    p.genre = getString (v, "genre", p.genre);
    p.description = getString (v, "description", p.description);
    p.builtIn = getBool (v, "builtIn", p.builtIn);
    p.deviceKey = getString (v, "deviceKey", p.deviceKey);
    p.params = dspFromVar (getVar (v, "params", {}), p.params);
    return p;
}

//==============================================================================
juce::var toVar (const DeviceDspProfile& p)
{
    auto* o = newObject();
    o->setProperty ("id", p.id);
    o->setProperty ("deviceName", p.key.deviceName);
    o->setProperty ("outputKind", p.key.outputKind);
    o->setProperty ("name", p.name);
    setIfNotEmpty (*o, "eqProfileId", p.eqProfileId);
    o->setProperty ("params", toVar (p.params));
    return juce::var (o);
}

DeviceDspProfile deviceProfileFromVar (const juce::var& v, const DeviceDspProfile& fallback)
{
    DeviceDspProfile p = fallback;
    if (! v.isObject()) return p;
    p.id = getString (v, "id", p.id);
    p.key.deviceName = getString (v, "deviceName", p.key.deviceName);
    p.key.outputKind = getString (v, "outputKind", p.key.outputKind);
    p.name = getString (v, "name", p.name);
    p.eqProfileId = getString (v, "eqProfileId", p.eqProfileId);
    p.params = dspFromVar (getVar (v, "params", {}), p.params);
    return p;
}

//==============================================================================
juce::var toVar (const TrackAnalysis& a)
{
    auto* o = newObject();
    o->setProperty ("valid", a.valid);
    o->setProperty ("version", a.version);
    o->setProperty ("analyzedSeconds", a.analyzedSeconds);
    o->setProperty ("integratedLufs", a.integratedLufs);
    o->setProperty ("loudnessRangeLu", a.loudnessRangeLu);
    o->setProperty ("samplePeakDb", a.samplePeakDb);
    o->setProperty ("truePeakDb", a.truePeakDb);
    o->setProperty ("crestFactorDb", a.crestFactorDb);
    o->setProperty ("spectralCentroidHz", a.spectralCentroidHz);
    o->setProperty ("lowEnergyRatio", a.lowEnergyRatio);
    o->setProperty ("subEnergyRatio", a.subEnergyRatio);
    o->setProperty ("midEnergyRatio", a.midEnergyRatio);
    o->setProperty ("highEnergyRatio", a.highEnergyRatio);
    o->setProperty ("lowBandDb", a.lowBandDb);
    o->setProperty ("midBandDb", a.midBandDb);
    o->setProperty ("highBandDb", a.highBandDb);
    o->setProperty ("analyzer", a.analyzer);
    return juce::var (o);
}

TrackAnalysis analysisFromVar (const juce::var& v, const TrackAnalysis& fallback)
{
    TrackAnalysis a = fallback;
    if (! v.isObject()) return a;
    a.valid = getBool (v, "valid", a.valid);
    a.version = getInt (v, "version", a.version);
    a.analyzedSeconds = getDouble (v, "analyzedSeconds", a.analyzedSeconds);
    a.integratedLufs = getDouble (v, "integratedLufs", a.integratedLufs);
    a.loudnessRangeLu = getDouble (v, "loudnessRangeLu", a.loudnessRangeLu);
    a.samplePeakDb = getDouble (v, "samplePeakDb", a.samplePeakDb);
    a.truePeakDb = getDouble (v, "truePeakDb", a.truePeakDb);
    a.crestFactorDb = getDouble (v, "crestFactorDb", a.crestFactorDb);
    a.spectralCentroidHz = getDouble (v, "spectralCentroidHz", a.spectralCentroidHz);
    a.lowEnergyRatio = getDouble (v, "lowEnergyRatio", a.lowEnergyRatio);
    a.subEnergyRatio = getDouble (v, "subEnergyRatio", a.subEnergyRatio);
    a.midEnergyRatio = getDouble (v, "midEnergyRatio", a.midEnergyRatio);
    a.highEnergyRatio = getDouble (v, "highEnergyRatio", a.highEnergyRatio);
    a.lowBandDb = getDouble (v, "lowBandDb", a.lowBandDb);
    a.midBandDb = getDouble (v, "midBandDb", a.midBandDb);
    a.highBandDb = getDouble (v, "highBandDb", a.highBandDb);
    a.analyzer = getString (v, "analyzer", a.analyzer);
    return a;
}

//==============================================================================
juce::var toVar (const SmartPlaylistDefinition& d)
{
    auto* o = newObject();
    o->setProperty ("matchAll", d.matchAll);
    o->setProperty ("sortColumn", d.sortColumn);
    o->setProperty ("sortDescending", d.sortDescending);
    o->setProperty ("limit", d.limit);

    juce::Array<juce::var> rules;
    for (auto& r : d.rules)
    {
        auto* ro = newObject();
        ro->setProperty ("field", (int) r.field);
        ro->setProperty ("op", (int) r.op);
        ro->setProperty ("text", r.text);
        ro->setProperty ("number", r.number);
        ro->setProperty ("number2", r.number2);
        juce::Array<juce::var> list;
        for (auto& s : r.list) list.add (s);
        ro->setProperty ("list", list);
        rules.add (juce::var (ro));
    }
    o->setProperty ("rules", rules);
    return juce::var (o);
}

SmartPlaylistDefinition smartDefFromVar (const juce::var& v, const SmartPlaylistDefinition& fallback)
{
    SmartPlaylistDefinition d = fallback;
    if (! v.isObject()) return d;
    d.matchAll = getBool (v, "matchAll", d.matchAll);
    d.sortColumn = getString (v, "sortColumn", d.sortColumn);
    d.sortDescending = getBool (v, "sortDescending", d.sortDescending);
    d.limit = getInt (v, "limit", d.limit);
    d.rules.clear();
    if (auto* arr = getVar (v, "rules", {}).getArray())
    {
        for (auto& rv : *arr)
        {
            if (! rv.isObject()) continue;
            SmartRule r;
            r.field = (SmartField) juce::jlimit (0, (int) SmartField::folder, getInt (rv, "field", 0));
            r.op = (SmartOp) juce::jlimit (0, (int) SmartOp::inList, getInt (rv, "op", 0));
            r.text = getString (rv, "text");
            r.number = getDouble (rv, "number", 0.0);
            r.number2 = getDouble (rv, "number2", 0.0);
            if (auto* la = getVar (rv, "list", {}).getArray())
                for (auto& s : *la) r.list.add (s.toString());
            d.rules.push_back (r);
        }
    }
    return d;
}

//==============================================================================
juce::var toVar (const ProfileDecision& d)
{
    auto* o = newObject();
    o->setProperty ("mode", (int) d.mode);
    setIfNotEmpty (*o, "profileId", d.profileId);
    setIfNotEmpty (*o, "profileName", d.profileName);
    setIfNotEmpty (*o, "matchedGenre", d.matchedGenre);
    o->setProperty ("usedAudioAnalysis", d.usedAudioAnalysis);
    o->setProperty ("learnedPreferenceApplied", d.learnedPreferenceApplied);
    setIfNotEmpty (*o, "overrideKind", d.overrideKind);
    juce::Array<juce::var> reasons;
    for (auto& r : d.reasons) reasons.add (r);
    o->setProperty ("reasons", reasons);
    return juce::var (o);
}

ProfileDecision decisionFromVar (const juce::var& v, const ProfileDecision& fallback)
{
    ProfileDecision d = fallback;
    if (! v.isObject()) return d;
    d.mode = (EqMode) juce::jlimit (0, 3, getInt (v, "mode", (int) d.mode));
    d.profileId = getString (v, "profileId", d.profileId);
    d.profileName = getString (v, "profileName", d.profileName);
    d.matchedGenre = getString (v, "matchedGenre", d.matchedGenre);
    d.usedAudioAnalysis = getBool (v, "usedAudioAnalysis", d.usedAudioAnalysis);
    d.learnedPreferenceApplied = getBool (v, "learnedPreferenceApplied", d.learnedPreferenceApplied);
    d.overrideKind = getString (v, "overrideKind", d.overrideKind);
    d.reasons.clear();
    if (auto* arr = getVar (v, "reasons", {}).getArray())
        for (auto& rv : *arr) d.reasons.push_back (rv.toString());
    return d;
}

} // namespace sp::serialize
