#include "Core/DspParams.h"
#include "Core/Types.h"

namespace sp
{

const char* playbackStateName (PlaybackState s)
{
    switch (s)
    {
        case PlaybackState::stopped: return "Stopped";
        case PlaybackState::paused:  return "Paused";
        case PlaybackState::playing: return "Playing";
        case PlaybackState::loading: return "Loading";
        case PlaybackState::error:   return "Error";
    }
    return "Unknown";
}

const char* eqModeName (EqMode m)
{
    switch (m)
    {
        case EqMode::manual:      return "Manual EQ";
        case EqMode::autoGenre:   return "Automatic (genre)";
        case EqMode::intelligent: return "Intelligent (analysis-assisted)";
        case EqMode::reference:   return "Reference (bypass)";
    }
    return "Unknown";
}

juce::String ProfileDecision::explanation() const
{
    if (mode == EqMode::manual)
        return "Manual EQ - no automatic profile is being applied.";
    if (mode == EqMode::reference)
        return "Reference mode - all DSP is bypassed for an unaltered signal path.";

    juce::StringArray parts;
    if (profileName.isNotEmpty())
        parts.add ("Profile: " + profileName);
    if (matchedGenre.isNotEmpty())
        parts.add ("matched genre \"" + matchedGenre + "\"");
    if (usedAudioAnalysis)
        parts.add ("refined using measured audio analysis");
    if (learnedPreferenceApplied)
        parts.add ("your learned preference applied");
    if (overrideKind.isNotEmpty())
        parts.add (overrideKind + " override in use");

    for (auto& r : reasons)
        parts.add (r);

    return parts.joinIntoString (" | ");
}

juce::String SignalPathInfo::qualityLabel() const
{
    switch (quality)
    {
        case Quality::bitPerfect:     return "Bit-perfect";
        case Quality::bitTransparent: return "Bit-transparent (no processing)";
        case Quality::processed:      return "Processed by DSP / volume / resampling";
        case Quality::error:          return "Signal path error";
        case Quality::unknown:        break;
    }
    return "Unknown";
}

juce::String SignalPathInfo::summary() const
{
    juce::String s;
    s << sourceFormat;
    if (sourceSampleRate > 0)
        s << " " << (sourceBitDepth > 0 ? juce::String (sourceBitDepth) + "/" : juce::String())
          << juce::String (sourceSampleRate / 1000.0, 1) << " kHz";

    s << "  ->  ";
    if (resampling)
        s << juce::String (resampleFrom / 1000.0, 1) << " -> "
          << juce::String (resampleTo / 1000.0, 1) << " kHz  ->  ";
    else
        s << juce::String (outputSampleRate / 1000.0, 1) << " kHz  ->  ";

    s << (outputDeviceName.isNotEmpty() ? outputDeviceName : "no device");

    if (quality == Quality::bitPerfect)
        s << "  [BIT-PERFECT" << (hogModeActive ? ", exclusive" : "") << "]";
    else if (quality == Quality::bitTransparent)
        s << "  [bit-transparent; OS may mix]";
    else if (! activeDspSteps.empty())
        s << "  [" << juce::String (activeDspSteps.size()) << " DSP stage(s)]";

    return s;
}

juce::String DeviceProfileKey::asKey() const
{
    return deviceName + "|" + outputKind;
}

const char* learnScopeName (LearnScope s)
{
    switch (s)
    {
        case LearnScope::off:   return "Off";
        case LearnScope::genre: return "Genre";
        case LearnScope::album: return "Album";
        case LearnScope::song:  return "Song";
    }
    return "Off";
}

const char* appActionName (AppAction a)
{
    switch (a)
    {
        case AppAction::none:              return "";
        case AppAction::playPause:         return "Play / Pause";
        case AppAction::stop:              return "Stop";
        case AppAction::nextTrack:         return "Next Track";
        case AppAction::previousTrack:     return "Previous Track";
        case AppAction::seekForward5:      return "Seek Forward 5s";
        case AppAction::seekBack5:         return "Seek Back 5s";
        case AppAction::seekForward30:     return "Seek Forward 30s";
        case AppAction::seekBack30:        return "Seek Back 30s";
        case AppAction::volumeUp:          return "Volume Up";
        case AppAction::volumeDown:        return "Volume Down";
        case AppAction::mute:              return "Mute";
        case AppAction::toggleEqBypass:    return "Toggle EQ Bypass";
        case AppAction::toggleReferenceMode: return "Toggle Reference Mode";
        case AppAction::cycleEqMode:       return "Cycle EQ Mode";
        case AppAction::bassUp:            return "Bass +0.5 dB";
        case AppAction::bassDown:          return "Bass -0.5 dB";
        case AppAction::showLibrary:       return "Show Library";
        case AppAction::showPlaylists:     return "Show Playlists";
        case AppAction::showQueue:         return "Show Queue";
        case AppAction::showHealth:        return "Show Library Health";
        case AppAction::toggleDspStudio:   return "Open / Close DSP Studio";
        case AppAction::toggleFullPlayer:  return "Toggle Full Player";
        case AppAction::showSettings:      return "Settings";
        case AppAction::focusSearch:       return "Focus Search";
        case AppAction::addToQueue:        return "Add Selection to Queue";
        case AppAction::clearQueue:        return "Clear Queue";
        case AppAction::rateOneStar:       return "Rate 1 Star";
        case AppAction::rateTwoStars:      return "Rate 2 Stars";
        case AppAction::rateThreeStars:    return "Rate 3 Stars";
        case AppAction::rateFourStars:     return "Rate 4 Stars";
        case AppAction::rateFiveStars:     return "Rate 5 Stars";
        case AppAction::numActions:        break;
    }
    return "";
}

const char* appActionDescription (AppAction a)
{
    switch (a)
    {
        case AppAction::playPause:         return "Start or pause playback";
        case AppAction::stop:              return "Stop playback and return to the start";
        case AppAction::nextTrack:         return "Play the next track in the queue";
        case AppAction::previousTrack:     return "Play the previous track in the queue";
        case AppAction::seekForward5:      return "Move playback position forward 5 seconds";
        case AppAction::seekBack5:         return "Move playback position back 5 seconds";
        case AppAction::seekForward30:     return "Move playback position forward 30 seconds";
        case AppAction::seekBack30:        return "Move playback position back 30 seconds";
        case AppAction::volumeUp:          return "Raise master volume by 1 dB (0 dB = unity)";
        case AppAction::volumeDown:        return "Lower master volume by 1 dB";
        case AppAction::mute:              return "Toggle mute";
        case AppAction::toggleEqBypass:    return "Bypass the equaliser section only";
        case AppAction::toggleReferenceMode: return "Bypass all DSP and set unity volume";
        case AppAction::cycleEqMode:       return "Cycle Manual / Auto genre / Intelligent";
        case AppAction::bassUp:            return "Raise the bass control by 0.5 dB";
        case AppAction::bassDown:          return "Lower the bass control by 0.5 dB";
        default: return "";
    }
}

} // namespace sp
