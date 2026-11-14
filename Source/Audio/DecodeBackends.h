/*
    Snake Puppy Reference Player
    Internal decoder backend declarations. The public factory in DecodeSource.h
    picks one of these based on content sniffing and the file extension.
*/
#pragma once

#include "Audio/DecodeSource.h"

namespace sp::detail
{

/** JUCE AudioFormatReader based decoder: WAV/AIFF/FLAC/MP3/OGG (and any other
    format the JUCE format manager knows). Always delivers stereo double at the
    file rate. */
std::unique_ptr<DecodeSource> createJuceDecodeSource (const juce::File& file, const juce::String& formatHint,
                                                      juce::String& errorOut);

/** macOS AudioToolbox ExtAudioFile decoder: M4A/AAC/ALAC/CAF and other
    CoreAudio formats JUCE does not cover. Stereo double at the file rate. */
std::unique_ptr<DecodeSource> createAudioToolboxDecodeSource (const juce::File& file, juce::String& errorOut);

/** Native DSF/DFF decoder with multistage FIR decimation to 176.4/352.8/705.6
    kHz PCM (DSD64/128/256) and -6 dB default gain. */
std::unique_ptr<DecodeSource> createDsdDecodeSource (const juce::File& file, juce::String& errorOut);

/** Human-readable name of the decoder that would be selected for a file, used
    by probeInfo even when opening fails. */
juce::String describeDecoderFor (const juce::File& file);

} // namespace sp::detail
