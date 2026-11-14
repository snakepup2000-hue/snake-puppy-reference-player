/*
    Snake Puppy Reference Player
    Decode-thread playback pipeline: one track at a time plus a pre-opened next
    track for sample-accurate gapless transitions.

    The pipeline owns decoding, optional memory pre-loading (whole track to RAM
    under a cap), streaming resampling to the output rate through the shared
    DSP Resampler, and the gapless splice: at the exact frame where the current
    file ends the next file's first frame follows, with the resampler tail
    flushed first. There is no silence, no seek and no gap at the boundary.

    Threading: ALL methods are called from the engine decode thread only (or a
    test thread). Never call from the audio callback or the message thread.
    DecodeSource and Resampler perform disk I/O / allocation here by design.
*/
#pragma once

#include "Audio/DecodeSource.h"
#include "DSP/Resampler.h"
#include <memory>
#include <vector>

namespace sp
{

/** Reported to the engine when a new track starts within a read block. */
struct PipelineTransition
{
    bool occurred = false;
    int newQueueIndex = -1;
    Track newTrack;
    int frameOffset = 0;    // offset in the block returned by read()
};

class PlaybackPipeline
{
public:
    PlaybackPipeline();
    ~PlaybackPipeline();

    /** Configure the output rate and resampler quality. Resets the pipeline. */
    void prepare (double outputSampleRate, int resamplerQuality0to2);

    void setResamplerQuality (int quality0to2);
    int getResamplerQuality() const;

    void setMemoryPlayback (bool enabled);
    bool isMemoryPlayback() const;

    /** Opens a track. startMs is clamped by the decoder. On success the
        previous track and any next track are discarded. */
    bool loadTrack (const Track& track, int queueIndex, std::int64_t startMs, juce::String& errorOut);

    /** Supplies the track to splice in when the current track ends. The source
        is opened here (decode thread) so the boundary costs no disk latency. */
    void setNextTrack (const Track& track, int queueIndex);
    void clearNextTrack();
    bool hasNextTrack() const;
    int getNextQueueIndex() const;

    /** Produces up to maxFrames stereo interleaved output-rate frames.
        Returns the number produced (0 only at the very end or on error). */
    int read (double* interleavedStereo, int maxFrames, PipelineTransition& transition);

    /** Decode-thread idle work: incrementally fills the next track's memory
        buffer when memory playback is enabled. */
    void prefetchNext (int maxFrames);

    const AudioFileInfo& currentInfo() const { return currentInfoValue; }
    const Track& currentTrack() const { return currentTrackValue; }
    int currentQueueIndex() const { return currentQueueIndexValue; }
    bool hasLoadedTrack() const { return hasCurrent; }
    bool finished() const { return finishedFlag; }
    const juce::String& lastError() const { return errorValue; }
    std::int64_t positionMs() const;
    bool isResampling() const { return currentResampling; }
    double resampleFrom() const { return resampleFromValue; }
    double resampleTo() const { return resampleToValue; }
    bool currentTrackUsesMemory() const { return currentMemory; }
    double outputSampleRate() const { return outputRate; }
    std::int64_t currentStartMs() const { return startFrameOffsetMs; }
    std::int64_t framesReadInTrack() const { return sourceFramesConsumed; }
    bool nextPrefetchComplete() const { return next.memoryComplete; }

    /** 512 MiB cap for whole-track memory playback. */
    static constexpr std::int64_t kMemoryPlaybackCapBytes = 512ll * 1024 * 1024;

private:
    void resetState();
    void setupCurrent (std::unique_ptr<DecodeSource> newSource, const Track& track, int queueIndex,
                       std::int64_t startMs, bool fromMemory);
    bool decodeWholeTrackToMemory (DecodeSource& source, std::vector<double>& memory,
                                   std::int64_t totalFrames, juce::String& errorOut);
    int readFromCurrent (double* out, int maxFrames);
    int readDirect (double* out, int maxFrames);
    int readResampled (double* out, int maxFrames);
    int readSourceInterleaved (double* out, int maxFrames);
    bool switchToNext (PipelineTransition& transition, int frameOffset);
    void updateSourceInfo (const AudioFileInfo& info);
    void configureResampler();

    struct NextTrack
    {
        bool valid = false;
        Track track;
        int queueIndex = -1;
        std::unique_ptr<DecodeSource> source;
        AudioFileInfo info;
        bool openAttempted = false;
        bool openFailed = false;
        juce::String error;

        std::vector<double> memory;
        std::int64_t memoryFrames = 0;
        std::int64_t memoryTargetFrames = 0;
        bool memoryComplete = false;
        bool memoryEnabled = false;
    };

    double outputRate = 48000.0;
    int resamplerQuality = 1;
    bool memoryPlaybackEnabled = false;

    // Current track
    bool hasCurrent = false;
    std::unique_ptr<DecodeSource> source;
    AudioFileInfo currentInfoValue;
    Track currentTrackValue;
    int currentQueueIndexValue = -1;
    bool currentResampling = false;
    double resampleFromValue = 0.0, resampleToValue = 0.0;
    bool currentMemory = false;
    std::vector<double> currentMemoryData;
    std::int64_t currentMemoryPos = 0;
    std::int64_t currentMemoryFrames = 0;
    std::int64_t sourceFramesConsumed = 0;
    std::int64_t startFrameOffset = 0;
    std::int64_t startFrameOffsetMs = 0;
    bool currentSourceExhausted = false;
    bool currentFlushDone = false;

    // Next track (pre-opened for gapless / prefetched for memory playback)
    NextTrack next;

    // Resampling scratch (allocated when the current track is set up)
    Resampler resampler;
    std::vector<double> sourceInterleaved;     // kInBlock stereo frames
    std::vector<double> inputLeft, inputRight;
    std::vector<double> resampledLeft, resampledRight;
    const double* inputChannels[2] { nullptr, nullptr };
    double* outputChannels[2] { nullptr, nullptr };
    std::size_t resampledCapacityFrames = 0;

    bool finishedFlag = false;
    juce::String errorValue;

    static constexpr int kInputBlockFrames = 4096;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackPipeline)
};

} // namespace sp
