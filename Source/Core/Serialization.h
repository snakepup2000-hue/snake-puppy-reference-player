/*
    Snake Puppy Reference Player
    JSON (juce::var <-> model struct) serialisation shared by settings,
    profiles, overrides and tests. Owned by the Lead: both the DSP chain UI and
    persistence depend on these formats being stable.
*/
#pragma once

#include "Core/DspParams.h"
#include "Core/Types.h"
#include <juce_core/juce_core.h>

namespace sp::serialize
{

juce::var toVar (const GraphicEqParams&);
GraphicEqParams graphicFromVar (const juce::var&, const GraphicEqParams& fallback = {});

juce::var toVar (const EqBand&);
EqBand bandFromVar (const juce::var&, const EqBand& fallback = {});

juce::var toVar (const ParametricEqParams&);
ParametricEqParams parametricFromVar (const juce::var&, const ParametricEqParams& fallback = {});

juce::var toVar (const BassParams&);
BassParams bassFromVar (const juce::var&, const BassParams& fallback = {});

juce::var toVar (const FilterParams&);
FilterParams filtersFromVar (const juce::var&, const FilterParams& fallback = {});

juce::var toVar (const StereoParams&);
StereoParams stereoFromVar (const juce::var&, const StereoParams& fallback = {});

juce::var toVar (const ReplayGainParams&);
ReplayGainParams replayGainFromVar (const juce::var&, const ReplayGainParams& fallback = {});

juce::var toVar (const ConvolutionParams&);
ConvolutionParams convolutionFromVar (const juce::var&, const ConvolutionParams& fallback = {});

juce::var toVar (const PreampParams&);
PreampParams preampFromVar (const juce::var&, const PreampParams& fallback = {});

juce::var toVar (const DspParams&);
DspParams dspFromVar (const juce::var&, const DspParams& fallback = {});

juce::var toVar (const EqProfile&);
EqProfile eqProfileFromVar (const juce::var&, const EqProfile& fallback = {});

juce::var toVar (const DeviceDspProfile&);
DeviceDspProfile deviceProfileFromVar (const juce::var&, const DeviceDspProfile& fallback = {});

juce::var toVar (const TrackAnalysis&);
TrackAnalysis analysisFromVar (const juce::var&, const TrackAnalysis& fallback = {});

juce::var toVar (const SmartPlaylistDefinition&);
SmartPlaylistDefinition smartDefFromVar (const juce::var&, const SmartPlaylistDefinition& fallback = {});

juce::var toVar (const ProfileDecision&);
ProfileDecision decisionFromVar (const juce::var&, const ProfileDecision& fallback = {});

} // namespace sp::serialize
