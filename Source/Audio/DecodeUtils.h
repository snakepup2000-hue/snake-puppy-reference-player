/*
    Snake Puppy Reference Player
    Internal helpers shared by the decoder backends (JUCE readers, macOS
    AudioToolbox ExtAudioFile, native DSD). Not a public interface.
*/
#pragma once

#include "Audio/DecodeSource.h"
#include <juce_core/juce_core.h>

namespace sp::detail
{

/** Folds an interleaved float source block with `numSrcChannels` channels into
    stereo interleaved double. Mono is duplicated, stereo copied, >2 uses
    ITU-style weights (LFE dropped). */
void downmixFloatToStereo (const float* const* src, int numSrcChannels, int frames, double* destInterleaved);

/** Stereo interleaved double -> non-interleaved double left/right. */
void deinterleaveStereo (const double* interleaved, double* left, double* right, int frames);

/** Reads ReplayGain tags from FLAC/Ogg/ID3/MP4 metadata with a bounded
    heuristic scan of the file head and tail. Fills the AudioFileInfo fields.
    Cheap enough for library probing; returns true when at least one gain tag
    was found. */
bool parseReplayGainTags (const juce::File& file, AudioFileInfo& infoOut);

/** Maps a Core Audio format ID (four character code) to a display name. */
juce::String formatNameForFourCC (std::uint32_t fourCC);

/** True for the Core Audio format IDs that carry integer PCM. */
bool isLinearPCMFormat (std::uint32_t fourCC);
bool isALACFormat (std::uint32_t fourCC);
bool isLossyFormat (std::uint32_t fourCC);

} // namespace sp::detail
