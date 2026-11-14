/*
    Snake Puppy Reference Player
    Decode-thread playback pipeline. See PlaybackPipeline.h for the contract.
*/
#include "Audio/PlaybackPipeline.h"
#include <algorithm>
#include <cstring>

namespace sp
{

PlaybackPipeline::PlaybackPipeline() = default;
PlaybackPipeline::~PlaybackPipeline() = default;

void PlaybackPipeline::prepare (double outputSampleRate, int resamplerQuality0to2)
{
    outputRate = outputSampleRate > 0.0 ? outputSampleRate : 48000.0;
    resamplerQuality = juce::jlimit (0, 2, resamplerQuality0to2);
    resetState();
}

void PlaybackPipeline::setResamplerQuality (int quality0to2)
{
    resamplerQuality = juce::jlimit (0, 2, quality0to2);
    if (hasCurrent && currentResampling)
    {
        resampler.prepare (currentInfoValue.sampleRate, outputRate, 2,
                           (Resampler::Quality) resamplerQuality);
        resampledCapacityFrames = (std::size_t) Resampler::maxOutputFrames (
            currentInfoValue.sampleRate, outputRate, kInputBlockFrames);
        resampledLeft.assign (resampledCapacityFrames, 0.0);
        resampledRight.assign (resampledCapacityFrames, 0.0);
        outputChannels[0] = resampledLeft.data();
        outputChannels[1] = resampledRight.data();
        currentFlushDone = false;
    }
}

int PlaybackPipeline::getResamplerQuality() const { return resamplerQuality; }

void PlaybackPipeline::setMemoryPlayback (bool enabled) { memoryPlaybackEnabled = enabled; }
bool PlaybackPipeline::isMemoryPlayback() const { return memoryPlaybackEnabled; }

void PlaybackPipeline::resetState()
{
    source.reset();
    hasCurrent = false;
    currentInfoValue = {};
    currentTrackValue = {};
    currentQueueIndexValue = -1;
    currentResampling = false;
    currentMemory = false;
    currentMemoryData.clear();
    currentMemoryPos = 0;
    currentMemoryFrames = 0;
    sourceFramesConsumed = 0;
    startFrameOffset = 0;
    startFrameOffsetMs = 0;
    currentSourceExhausted = false;
    currentFlushDone = false;
    finishedFlag = false;
    errorValue.clear();
    clearNextTrack();
}

//==============================================================================
bool PlaybackPipeline::loadTrack (const Track& track, int queueIndex, std::int64_t startMs, juce::String& errorOut)
{
    errorOut.clear();
    resetState();

    const juce::File file (track.path);
    juce::String openError;
    auto newSource = DecodeSource::create (file, openError);
    if (newSource == nullptr)
    {
        errorOut = openError.isNotEmpty() ? openError : ("Could not open " + track.path);
        finishedFlag = true;
        return false;
    }

    const auto& info = newSource->getInfo();
    if (info.sampleRate <= 0 || info.totalFrames <= 0)
    {
        errorOut = "Decoder reported no usable audio for " + track.path;
        finishedFlag = true;
        return false;
    }

    if (! newSource->seekToMs (startMs))
    {
        errorOut = "Could not seek " + track.path + " to " + juce::String (startMs) + " ms";
        finishedFlag = true;
        return false;
    }

    const auto frameOffset = (startMs * (std::int64_t) info.sampleRate) / 1000;
    startFrameOffset = juce::jlimit<std::int64_t> (0, info.totalFrames, frameOffset);
    startFrameOffsetMs = startMs;

    // Whole-track memory playback, when it fits the cap.
    bool loadedFromMemory = false;

    if (memoryPlaybackEnabled)
    {
        const auto bytes = info.totalFrames * 2 * (std::int64_t) sizeof (double);
        if (bytes > 0 && bytes <= kMemoryPlaybackCapBytes)
        {
            newSource->seekToMs (0);
            std::vector<double> decoded;
            decoded.resize ((size_t) (info.totalFrames * 2));

            std::string memError;
            if (decodeWholeTrackToMemory (*newSource, decoded, info.totalFrames, openError))
            {
                currentMemoryData = std::move (decoded);
                currentMemoryFrames = info.totalFrames;
                currentMemoryPos = startFrameOffset;
                loadedFromMemory = true;
            }
            else
            {
                // Fall back to streaming rather than failing the track.
                if (! newSource->seekToMs (startMs))
                {
                    errorOut = "Could not seek " + track.path + " after memory load fallback";
                    finishedFlag = true;
                    return false;
                }
                loadedFromMemory = false;
            }
        }
    }

    currentTrackValue = track;
    currentQueueIndexValue = queueIndex;
    setupCurrent (std::move (newSource), track, queueIndex, startMs, loadedFromMemory);
    currentMemory = loadedFromMemory;
    currentInfoValue = source->getInfo();

    configureResampler();
    updateSourceInfo (currentInfoValue);
    finishedFlag = false;
    return true;
}

void PlaybackPipeline::setupCurrent (std::unique_ptr<DecodeSource> newSource, const Track& track, int queueIndex,
                                     std::int64_t startMs, bool fromMemory)
{
    source = std::move (newSource);
    hasCurrent = source != nullptr;
    currentTrackValue = track;
    currentQueueIndexValue = queueIndex;
    startFrameOffsetMs = startMs;
    sourceFramesConsumed = 0;
    currentSourceExhausted = false;
    currentFlushDone = false;

    if (source != nullptr)
    {
        currentInfoValue = source->getInfo();
        if (! fromMemory)
        {
            const auto frameOffset = (startMs * (std::int64_t) currentInfoValue.sampleRate) / 1000;
            startFrameOffset = juce::jlimit<std::int64_t> (0, currentInfoValue.totalFrames, frameOffset);
        }
        else
        {
            startFrameOffset = currentMemoryPos;
        }
    }
}

bool PlaybackPipeline::decodeWholeTrackToMemory (DecodeSource& src, std::vector<double>& memory,
                                                 std::int64_t totalFrames, juce::String& errorOut)
{
    std::int64_t frame = 0;

    while (frame < totalFrames)
    {
        const int want = (int) juce::jmin<std::int64_t> ((std::int64_t) kInputBlockFrames, totalFrames - frame);
        const int got = src.readFrames (memory.data() + (size_t) frame * 2, want);
        if (got <= 0)
        {
            errorOut = src.getLastError().isNotEmpty()
                         ? src.getLastError()
                         : ("Decoder ended early at frame " + juce::String (frame) + " of " + juce::String (totalFrames));
            return false;
        }
        frame += got;
    }

    return true;
}

//==============================================================================
void PlaybackPipeline::setNextTrack (const Track& track, int queueIndex)
{
    clearNextTrack();

    next.valid = true;
    next.track = track;
    next.queueIndex = queueIndex;

    if (memoryPlaybackEnabled)
    {
        // Open now (decode thread) so memory prefetch can start immediately.
        juce::String error;
        next.source = DecodeSource::create (juce::File (track.path), error);
        next.openAttempted = true;
        if (next.source == nullptr)
        {
            next.openFailed = true;
            next.error = error;
            return;
        }
        next.info = next.source->getInfo();
        next.source->seekToMs (0);
        next.memoryEnabled = true;
        next.memoryTargetFrames = next.info.totalFrames;
        const auto bytes = next.memoryTargetFrames * 2 * (std::int64_t) sizeof (double);
        if (bytes > kMemoryPlaybackCapBytes)
        {
            next.memoryEnabled = false;
        }
        else
        {
            next.memory.reserve ((size_t) (next.memoryTargetFrames * 2));
        }
    }
}

void PlaybackPipeline::clearNextTrack()
{
    next = NextTrack {};
}

bool PlaybackPipeline::hasNextTrack() const { return next.valid; }
int PlaybackPipeline::getNextQueueIndex() const { return next.valid ? next.queueIndex : -1; }

void PlaybackPipeline::prefetchNext (int maxFrames)
{
    if (! next.valid || ! next.memoryEnabled || next.memoryComplete || next.source == nullptr || maxFrames <= 0)
        return;

    const auto remaining = next.memoryTargetFrames - next.memoryFrames;
    if (remaining <= 0)
    {
        next.memoryComplete = true;
        return;
    }

    const int want = (int) juce::jmin<std::int64_t> ((std::int64_t) maxFrames, remaining);

    next.memory.resize ((size_t) (next.memoryTargetFrames * 2));
    const int got = next.source->readFrames (next.memory.data() + (size_t) next.memoryFrames * 2, want);

    if (got <= 0)
    {
        // Could not fully prefetch: at the boundary we will stream the rest
        // from the still-open source rather than fail the transition.
        next.memoryComplete = false;
        next.memoryEnabled = false;
        return;
    }

    next.memoryFrames += got;
    if (next.memoryFrames >= next.memoryTargetFrames)
    {
        next.memoryComplete = true;
        next.memory.shrink_to_fit();
    }
}

//==============================================================================
void PlaybackPipeline::updateSourceInfo (const AudioFileInfo& info)
{
    resampleFromValue = info.sampleRate;
    resampleToValue = outputRate;
}

void PlaybackPipeline::configureResampler()
{
    currentResampling = hasCurrent
                     && currentInfoValue.sampleRate > 0
                     && std::abs ((double) currentInfoValue.sampleRate - outputRate) > 0.5;

    if (currentResampling)
    {
        resampler.prepare ((double) currentInfoValue.sampleRate, outputRate, 2,
                           (Resampler::Quality) resamplerQuality);
        resampledCapacityFrames = (std::size_t) Resampler::maxOutputFrames (
            currentInfoValue.sampleRate, outputRate, kInputBlockFrames);
        resampledCapacityFrames = juce::jlimit<std::size_t> (64, 1u << 20, resampledCapacityFrames);
        resampledLeft.assign (resampledCapacityFrames, 0.0);
        resampledRight.assign (resampledCapacityFrames, 0.0);
        outputChannels[0] = resampledLeft.data();
        outputChannels[1] = resampledRight.data();

        sourceInterleaved.assign ((size_t) kInputBlockFrames * 2, 0.0);
        inputLeft.assign ((size_t) kInputBlockFrames, 0.0);
        inputRight.assign ((size_t) kInputBlockFrames, 0.0);
        inputChannels[0] = inputLeft.data();
        inputChannels[1] = inputRight.data();
    }
    else
    {
        resampler.prepare (outputRate, outputRate, 2, (Resampler::Quality) resamplerQuality);
    }
}

//==============================================================================
std::int64_t PlaybackPipeline::positionMs() const
{
    if (! hasCurrent || currentInfoValue.sampleRate <= 0)
        return 0;
    return ((startFrameOffset + sourceFramesConsumed) * 1000) / currentInfoValue.sampleRate;
}

//==============================================================================
int PlaybackPipeline::readSourceInterleaved (double* out, int maxFrames)
{
    if (! hasCurrent)
        return 0;

    if (currentMemory)
    {
        const auto remaining = currentMemoryFrames - currentMemoryPos;
        if (remaining <= 0)
            return 0;
        const int n = (int) juce::jmin<std::int64_t> ((std::int64_t) maxFrames, remaining);
        std::memcpy (out, currentMemoryData.data() + (size_t) (currentMemoryPos * 2),
                     sizeof (double) * (size_t) n * 2);
        currentMemoryPos += n;
        sourceFramesConsumed += n;
        return n;
    }

    const int n = source->readFrames (out, maxFrames);
    if (n > 0)
    {
        sourceFramesConsumed += n;
    }
    else
    {
        const auto err = source->getLastError();
        if (err.isNotEmpty())
            errorValue = err;
    }
    return n;
}

int PlaybackPipeline::readDirect (double* out, int maxFrames)
{
    const int n = readSourceInterleaved (out, maxFrames);
    if (n <= 0)
        currentSourceExhausted = true;
    return n;
}

int PlaybackPipeline::readResampled (double* out, int maxFrames)
{
    int produced = 0;
    int idleSpins = 0;

    if (currentSourceExhausted && currentFlushDone)
        return 0;

    while (produced < maxFrames)
    {
        if (! currentSourceExhausted)
        {
            const int got = readSourceInterleaved (sourceInterleaved.data(), kInputBlockFrames);
            if (got <= 0)
            {
                currentSourceExhausted = true;
                inputLeft.assign ((size_t) 1, 0.0);
                inputRight.assign ((size_t) 1, 0.0);
                inputChannels[0] = inputLeft.data();
                inputChannels[1] = inputRight.data();
            }
            else
            {
                for (int i = 0; i < got; ++i)
                {
                    inputLeft[(size_t) i] = sourceInterleaved[(size_t) i * 2];
                    inputRight[(size_t) i] = sourceInterleaved[(size_t) i * 2 + 1];
                }
                inputChannels[0] = inputLeft.data();
                inputChannels[1] = inputRight.data();
            }

            const int inFrames = got > 0 ? got : 0;
            const int maxOut = (int) juce::jmin<std::size_t> ((std::size_t) (maxFrames - produced),
                                                              resampledCapacityFrames);
            if (maxOut <= 0)
                break;

            const int n = resampler.process (inputChannels, inFrames, outputChannels, maxOut, false);

            if (n > 0)
            {
                for (int i = 0; i < n; ++i)
                {
                    out[(size_t) (produced + i) * 2] = resampledLeft[(size_t) i];
                    out[(size_t) (produced + i) * 2 + 1] = resampledRight[(size_t) i];
                }
                produced += n;
                idleSpins = 0;
            }
            else if (inFrames == 0)
            {
                // Nothing to flush yet; will fall through to the flush below.
            }
            else if (++idleSpins > 24)
            {
                errorValue = "Sample-rate converter produced no output at "
                             + juce::String (currentInfoValue.sampleRate) + " -> "
                             + juce::String (outputRate) + " Hz (resampler unavailable or failed)";
                break;
            }

            continue;
        }

        // Source exhausted: flush the resampler tail.
        const int maxOut = (int) juce::jmin<std::size_t> ((std::size_t) (maxFrames - produced),
                                                          resampledCapacityFrames);
        if (maxOut <= 0)
            break;

        const int flushed = resampler.process (inputChannels, 0, outputChannels, maxOut, true);
        if (flushed > 0)
        {
            for (int i = 0; i < flushed; ++i)
            {
                out[(size_t) (produced + i) * 2] = resampledLeft[(size_t) i];
                out[(size_t) (produced + i) * 2 + 1] = resampledRight[(size_t) i];
            }
            produced += flushed;
            continue;
        }

        currentFlushDone = true;
        break;
    }

    if (errorValue.isNotEmpty())
        return produced;

    return produced;
}

int PlaybackPipeline::readFromCurrent (double* out, int maxFrames)
{
    if (! hasCurrent)
        return 0;

    if (currentResampling)
        return readResampled (out, maxFrames);

    return readDirect (out, maxFrames);
}

//==============================================================================
bool PlaybackPipeline::switchToNext (PipelineTransition& transition, int frameOffset)
{
    if (! next.valid)
        return false;

    NextTrack pending = std::move (next);
    next = NextTrack {};

    const bool useMemory = pending.memoryEnabled && pending.memoryComplete
                        && ! pending.memory.empty();

    std::unique_ptr<DecodeSource> newSource;

    if (useMemory)
    {
        newSource = std::move (pending.source);
        if (newSource == nullptr)
        {
            errorValue = "Next track memory buffer is unavailable";
            return false;
        }
        newSource->seekToMs (0);
    }
    else
    {
        newSource = std::move (pending.source);
        if (newSource == nullptr)
        {
            juce::String openError;
            newSource = DecodeSource::create (juce::File (pending.track.path), openError);
            if (newSource == nullptr)
            {
                errorValue = openError.isNotEmpty() ? openError
                                                    : ("Could not open next track " + pending.track.path);
                return false;
            }
        }
        if (! newSource->seekToMs (0))
        {
            errorValue = "Could not seek next track to the start: " + pending.track.path;
            return false;
        }
    }

    // A prefetched-but-incomplete memory buffer means we already consumed part
    // of the stream: seek it back to zero and stream the whole track instead.
    currentMemoryData.clear();
    currentMemoryFrames = 0;
    currentMemoryPos = 0;
    currentMemory = false;

    const auto startMs = (std::int64_t) 0;
    setupCurrent (std::move (newSource), pending.track, pending.queueIndex, startMs, false);

    if (useMemory)
    {
        currentMemoryData = std::move (pending.memory);
        currentMemoryFrames = pending.memoryTargetFrames;
        currentMemoryPos = 0;
        currentMemory = true;
        startFrameOffset = 0;
    }

    configureResampler();

    transition.occurred = true;
    transition.newQueueIndex = pending.queueIndex;
    transition.newTrack = pending.track;
    transition.frameOffset = frameOffset;
    finishedFlag = false;
    return true;
}

//==============================================================================
int PlaybackPipeline::read (double* interleavedStereo, int maxFrames, PipelineTransition& transition)
{
    transition = {};

    if (interleavedStereo == nullptr || maxFrames <= 0)
        return 0;

    if (errorValue.isNotEmpty())
        return 0;

    if (! hasCurrent)
    {
        if (next.valid)
        {
            if (! switchToNext (transition, 0))
                return 0;
        }
        else
        {
            finishedFlag = true;
            return 0;
        }
    }

    int produced = 0;
    int guard = 0;

    while (produced < maxFrames)
    {
        const int remaining = maxFrames - produced;
        const int n = readFromCurrent (interleavedStereo + (size_t) produced * 2, remaining);
        produced += n;

        if (errorValue.isNotEmpty())
            break;

        if (produced >= maxFrames)
            break;

        const bool exhausted = currentResampling ? (currentSourceExhausted && currentFlushDone)
                                                 : currentSourceExhausted;
        if (! exhausted)
        {
            if (n <= 0 && ++guard > 8)
            {
                errorValue = "Decoder made no progress";
                break;
            }
            continue;
        }

        // Current track finished. Splice the next one, if any.
        if (! next.valid)
        {
            finishedFlag = true;
            break;
        }

        if (! switchToNext (transition, produced))
            break;

        transition.occurred = true;
        transition.frameOffset = produced;
    }

    return produced;
}

} // namespace sp
