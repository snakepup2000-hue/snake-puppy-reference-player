/*
    Snake Puppy Reference Player
    Format-agnostic decoding source. One DecodeSource decodes exactly one file
    and always delivers stereo interleaved 64-bit float frames at the file's
    native sample rate. Resampling to the output rate happens in the audio
    engine's decode thread, not here.

    Implementations:
      - JUCE AudioFormatReader based (WAV, AIFF, FLAC, MP3, OGG)
      - macOS AudioToolbox / ExtAudioFile based (M4A/AAC/ALAC, CAF, others)
      - Native DSD (DSF, DFF) with multistage decimation to PCM

    The factory picks the best decoder and reports which one was used so the
    Audio Signal Path display is honest.
*/
#pragma once

#include "Core/Types.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace sp
{

struct AudioFileInfo
{
    juce::String path;
    int sampleRate = 0;
    int bitDepth = 0;
    int numChannels = 0;
    std::int64_t durationMs = 0;
    std::int64_t totalFrames = 0;
    juce::String formatName;
    Track::Family family = Track::Family::unknown;
    bool isDsd = false;
    int dsdMultiple = 0;
    float dsdGainDb = -6.0f;
    juce::String decoderName;

    /** True when a source with more than two channels was folded down to the
        stereo delivery format (ITU-style coefficients, LFE dropped). */
    bool downmixedToStereo = false;

    float replayGainTrackDb = 0.0f;
    bool hasReplayGainTrack = false;
    float replayGainAlbumDb = 0.0f;
    bool hasReplayGainAlbum = false;
    float peakLinear = 0.0f;
};

class DecodeSource
{
public:
    virtual ~DecodeSource() = default;

    /** Opens a file and returns a decoder, or nullptr with a clear error. */
    static std::unique_ptr<DecodeSource> create (const juce::File& file, juce::String& errorOut);

    /** Cheap metadata-only probe (may read only headers). */
    static bool probeInfo (const juce::File& file, AudioFileInfo& infoOut, juce::String& errorOut);

    static bool isSupportedFile (const juce::File& file);
    static juce::StringArray supportedExtensions();

    virtual const AudioFileInfo& getInfo() const = 0;

    /** Seek to a position in milliseconds (frame accurate where possible). */
    virtual bool seekToMs (std::int64_t ms) = 0;

    /** Reads up to numFramesRequested stereo frames into `interleavedStereo`
        (2 * numFrames doubles). Returns the number of frames written; 0 means
        end of file. Must never block on the audio callback because it is only
        ever called from the decode thread. */
    virtual int readFrames (double* interleavedStereo, int numFramesRequested) = 0;

    virtual std::int64_t positionMs() const = 0;
    virtual bool isFinished() const = 0;
    virtual void close() = 0;

    /** Human-readable reason for the last decode/open failure, empty when the
        source is healthy. Never blocks; decode-thread use only. */
    virtual juce::String getLastError() const { return {}; }
};

} // namespace sp
