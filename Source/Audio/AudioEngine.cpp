/*
    Snake Puppy Reference Player
    Playback engine. See AudioEngine.h for the architecture contract.

    Threads:
      - message thread: transport/queue/options/DSP calls, the 15 Hz status
        timer and all listener notifications.
      - decode thread: owns PlaybackPipeline (file I/O, decoding, resampling),
        fills the lock-free ring buffer, publishes position/track atomics and
        posts auto-advance/error notifications through atomics.
      - Core Audio callback: reads the ring, runs the DspChain (double), applies
        smoothed volume/mute, optional TPDF dither, converts to float and writes
        the device. No allocation/mutex/disk/String in this path.

    The engine keeps a single lock (configLock/statusLock) between the message
    and decode threads; the audio callback never touches it.
*/
#include "Audio/AudioEngine.h"
#include "Audio/Dither.h"
#include "Audio/LockFreeRingBuffer.h"
#include "Audio/PlaybackPipeline.h"
#include "Audio/SignalPath.h"

#include <atomic>
#include <deque>
#include <list>
#include <vector>

namespace sp
{

namespace
{
constexpr int kRingCapacityFrames = 1 << 20;    // ~23.8 s @44.1k, ~2.7 s @384k
constexpr int kDecodeBlockFrames = 4096;
constexpr double kTargetBufferedSeconds = 3.0;
constexpr double kPausedPrefillSeconds = 0.6;
constexpr int kStatusTimerHz = 15;
constexpr double kFadeSeconds = 0.008;
constexpr int kMaxCallbackBlock = 16384;

juce::String formatRate (double rate)
{
    return juce::String (rate, juce::String (rate) == juce::String ((int) rate) ? 0 : 1);
}

//==============================================================================
/** Debug-only final-output recorder. The audio callback pushes float frames
    into a lock-free FIFO; this thread writes a 24-bit stereo WAV. */
class DebugWavRecorder : public juce::Thread
{
public:
    DebugWavRecorder() : juce::Thread ("SP DebugRecorder")
    {
        fifo.assign ((size_t) kFifoFrames * 2, 0.0f);
        readBuffer.setSize (2, kFifoFrames, false, true, false);
    }

    ~DebugWavRecorder() override { stopRecording(); }

    bool startRecording (const juce::File& file, double sampleRate, juce::String& errorOut)
    {
        errorOut.clear();
        if (isThreadRunning())
        {
            errorOut = "Debug recording is already running";
            return false;
        }
        if (sampleRate <= 0.0)
        {
            errorOut = "No output device is open";
            return false;
        }

        file.deleteFile();
        std::unique_ptr<juce::FileOutputStream> out (file.createOutputStream());
        if (out == nullptr || ! out->openedOk())
        {
            errorOut = "Could not create the debug recording file: " + file.getFullPathName();
            return false;
        }

        sameRate = sampleRate;
        std::unique_ptr<juce::AudioFormatWriter> newWriter (
            juce::WavAudioFormat().createWriterFor (out.release(), sameRate, 2, 24, {}, 0));
        if (newWriter == nullptr)
        {
            errorOut = "Could not create a WAV writer for the debug recording";
            return false;
        }

        writer = std::move (newWriter);
        writePos.store (0, std::memory_order_relaxed);
        readPos.store (0, std::memory_order_relaxed);
        droppedFrames.store (0, std::memory_order_relaxed);
        stopRequested.store (false, std::memory_order_relaxed);
        startThread (juce::Thread::Priority::low);
        return true;
    }

    void stopRecording()
    {
        stopRequested.store (true, std::memory_order_release);
        if (isThreadRunning())
            stopThread (3000);

        writer.reset();
        writePos.store (0, std::memory_order_relaxed);
        readPos.store (0, std::memory_order_relaxed);
    }

    bool isRecording() const { return isThreadRunning(); }

    /** Audio callback: non-blocking push of final stereo float frames. */
    void push (const float* left, const float* right, int frames) noexcept
    {
        if (left == nullptr || right == nullptr || frames <= 0)
            return;

        const auto wr = writePos.load (std::memory_order_relaxed);
        const auto rd = readPos.load (std::memory_order_acquire);
        const int room = kFifoFrames - (int) (wr - rd);
        if (room <= 0)
        {
            droppedFrames.fetch_add (frames, std::memory_order_relaxed);
            return;
        }

        const int n = juce::jmin (frames, room);
        int done = 0;
        auto frame = wr;
        while (done < n)
        {
            const int idx = (int) (frame & (kFifoFrames - 1));
            const int chunk = juce::jmin (n - done, kFifoFrames - idx);
            for (int i = 0; i < chunk; ++i)
            {
                fifo[(size_t) (idx + i) * 2] = left[done + i];
                fifo[(size_t) (idx + i) * 2 + 1] = right[done + i];
            }
            done += chunk;
            frame += chunk;
        }
        writePos.store (wr + n, std::memory_order_release);

        if (n < frames)
            droppedFrames.fetch_add (frames - n, std::memory_order_relaxed);
    }

    std::int64_t getDroppedFrames() const { return droppedFrames.load(); }
    double getSampleRate() const { return sameRate; }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            const auto wr = writePos.load (std::memory_order_acquire);
            const auto rd = readPos.load (std::memory_order_relaxed);
            const int avail = (int) (wr - rd);

            if (avail <= 0)
            {
                if (stopRequested.load (std::memory_order_acquire))
                    break;
                wait (2);
                continue;
            }

            const int n = juce::jmin (avail, kFifoFrames);
            int done = 0;
            auto frame = rd;
            while (done < n)
            {
                const int idx = (int) (frame & (kFifoFrames - 1));
                const int chunk = juce::jmin (n - done, kFifoFrames - idx);
                for (int i = 0; i < chunk; ++i)
                {
                    readBuffer.getWritePointer (0)[done + i] = fifo[(size_t) (idx + i) * 2];
                    readBuffer.getWritePointer (1)[done + i] = fifo[(size_t) (idx + i) * 2 + 1];
                }
                done += chunk;
                frame += chunk;
            }

            if (writer != nullptr)
                writer->writeFromAudioSampleBuffer (readBuffer, 0, n);

            readPos.store (rd + n, std::memory_order_release);
        }
    }

    static constexpr int kFifoFrames = 1 << 15;
    std::vector<float> fifo;
    juce::AudioBuffer<float> readBuffer;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    std::atomic<std::int64_t> writePos { 0 }, readPos { 0 };
    std::atomic<std::int64_t> droppedFrames { 0 };
    std::atomic<bool> stopRequested { false };
    double sameRate = 0.0;
};
} // namespace

//==============================================================================
struct AudioEngine::Impl
{
    enum class CommandType
    {
        none, load, resume, pause, stop, seek, setNext, clearNext, setQuality, setMemory, quit
    };

    struct Command
    {
        CommandType type = CommandType::none;
        Track track;
        int index = -1;
        std::int64_t ms = 0;
        bool autoPlay = true;
        int quality = 1;
        bool flag = false;
    };

    struct SourceSnapshot
    {
        bool valid = false;
        Track track;
        int index = -1;
        juce::String file;
        juce::String format;
        juce::String decoder;
        int rate = 0;
        int bits = 0;
        int channels = 0;
        bool isDsd = false;
        int dsdMultiple = 0;
        bool integerPcm = false;
        bool downmixed = false;
        bool resampling = false;
        double resampleFrom = 0.0;
        double resampleTo = 0.0;
        juce::String resamplerName;
    };

    struct Segment
    {
        std::int64_t startEpoch = 0;
        int index = -1;
        std::int64_t startMs = 0;
        std::int64_t durationMs = 0;
    };

    //== Core objects =========================================================
    juce::AudioDeviceManager manager;
    DeviceManager devices { manager };
    DspChain chain;
    AnalyzerHub hub;
    DebugWavRecorder debugRecorder;

    mutable juce::CriticalSection configLock;      // options, queue, params, dsp state
    mutable juce::CriticalSection statusLock;      // snapshot, published status, messages
    mutable juce::CriticalSection commandLock;     // decode command queue
    mutable juce::CriticalSection listenerLock;    // listener list

    EngineOptions options;
    std::vector<Track> queue;
    int queueIndex = -1;
    DspParams baseParams, effectiveParams;
    bool initialised = false;

    std::list<Listener*> listeners;

    //== Volume / reference (atomics readable by the audio thread) ============
    std::atomic<float> volumeDb { 0.0f };
    std::atomic<bool> muted { false };
    std::atomic<bool> referenceActive { false };
    float savedVolumeDb = 0.0f;
    bool savedMuted = false;
    double savedDeviceRate = 0.0;

    //== Device state =========================================================
    std::atomic<bool> deviceRunning { false };
    std::atomic<double> deviceRateAtomic { 44100.0 };
    std::atomic<int> deviceBufferFramesAtomic { 512 };
    std::atomic<int> nativeBitsAtomic { 0 };
    std::atomic<int> outputLatencyAtomic { 0 };
    std::atomic<bool> hogActiveAtomic { false };
    std::atomic<int> deviceOutputChannelsAtomic { 2 };
    bool deviceWasOpen = false;

    //== Transport status (published by the decode thread / callback) =========
    std::atomic<PlaybackState> stateAtomic { PlaybackState::stopped };
    std::atomic<std::int64_t> positionMsAtomic { 0 };
    std::atomic<std::int64_t> durationMsAtomic { 0 };
    std::atomic<std::int64_t> bufferedMsAtomic { 0 };
    std::atomic<std::int64_t> consumedFramesAtomic { 0 };
    std::atomic<std::int64_t> underrunCountAtomic { 0 };
    std::atomic<std::int64_t> clipCountAtomic { 0 };
    std::atomic<double> cpuPercentAtomic { 0.0 };
    std::atomic<int> publishedIndexAtomic { -1 };
    std::atomic<std::uint64_t> trackSerialAtomic { 0 };
    std::atomic<bool> endOfQueueAtomic { false };
    std::atomic<bool> memoryActiveAtomic { false };
    std::atomic<bool> decodeActiveAtomic { false };
    std::atomic<std::int64_t> naturalEndIndexAtomic { -1 };
    std::atomic<bool> drainingAtomic { false };
    std::atomic<bool> suspendDecodeAtomic { false };

    //== Signal path inputs ===================================================
    std::atomic<bool> resamplingActiveAtomic { false };
    std::atomic<double> resampleFromAtomic { 0.0 }, resampleToAtomic { 0.0 };
    std::atomic<bool> replayGainActiveAtomic { false };
    std::atomic<float> replayGainDbAtomic { 0.0f };
    std::atomic<bool> ditherActiveAtomic { false };
    std::atomic<int> ditherTargetBitsAtomic { 0 };
    std::atomic<int> ditherSourceBitsAtomic { 0 };
    std::atomic<bool> ditherEnabledOptionAtomic { true };
    std::atomic<int> resamplerQualityAtomic { 1 };
    std::atomic<bool> memoryPlaybackOptionAtomic { false };
    std::atomic<bool> loadPendingAtomic { false };

    SourceSnapshot sourceSnapshot;   // statusLock
    EngineStatus publishedStatus;    // statusLock
    juce::String pendingError, pendingInfo;
    std::uint64_t errorSerial = 0, reportedErrorSerial = 0;
    std::uint64_t infoSerial = 0, reportedInfoSerial = 0;
    bool overrideReplayGainValid = false;
    float overrideReplayGainDb = 0.0f;
    float overridePeak = 0.0f;
    bool overrideHasGain = false;

    int lastPublishedIndex = -1;
    std::uint64_t lastTrackSerial = 0;
    PlaybackState lastPublishedState = PlaybackState::stopped;
    juce::int64 lastDeviceCheckMs = 0;
    juce::int64 lastRecoveryAttemptMs = 0;
    juce::int64 ignorePublishedIndexUntilMs = 0;
    bool deviceChangePending = false;
    juce::String hogAttemptDevice;
    juce::int64 hogAttemptMs = 0;
    bool resumeWhenDeviceReturns = false;

    //== Decode commands ======================================================
    std::deque<Command> commands;

    //== Audio callback scratch (allocated in audioDeviceAboutToStart) ========
    std::unique_ptr<LockFreeStereoRingBuffer> ring;
    std::vector<double> ringScratch;
    juce::AudioBuffer<double> dspBuffer;
    juce::AudioBuffer<float> preDspFloat;
    juce::AudioBuffer<float> postDspFloat;
    double* dspChannelPointers[2] { nullptr, nullptr };
    juce::SmoothedValue<double> volumeSmoother;
    int fadeInRemaining = 0, fadeInTotal = 0;
    int fadeOutRemaining = 0, fadeOutTotal = 0;
    int allocatedBlock = 0;
    bool lastCallbackWasPlaying = false;
    TpdfDither ditherGenerator;
    std::atomic<DebugWavRecorder*> recorderForCallback { nullptr };

    //== Decode thread ========================================================
    struct DecodeThread : juce::Thread
    {
        explicit DecodeThread (Impl& o) : juce::Thread ("SP Decode"), owner (o) {}
        void run() override { owner.decodeLoop(); }
        Impl& owner;
    };

    std::unique_ptr<DecodeThread> decodeThread;

    // Decode-thread-owned state
    PlaybackPipeline pipeline;
    std::vector<double> decodeScratch;
    std::vector<Segment> segments;
    std::int64_t framesAppended = 0;
    std::int64_t consumedAtEpoch = 0;
    double preparedRate = 0.0;
    bool playingDesired = false;
    bool loaded = false;
    Track loadedTrack;
    int loadedIndex = -1;
    std::int64_t loadedStartMs = 0;
    bool drainingLocal = false;
    std::atomic<bool> decodeQuit { false };

    //==========================================================================
    Impl() { pipeline.setMemoryPlayback (false); }

    ~Impl() = default;

    //== Message helpers ======================================================
    void postCommand (const Command& c)
    {
        const juce::ScopedLock sl (commandLock);
        if (commands.size() > 128)
            commands.pop_front();
        commands.push_back (c);
    }

    void postError (const juce::String& message)
    {
        const juce::ScopedLock sl (statusLock);
        pendingError = message;
        ++errorSerial;
    }

    /** Message thread only: records an error and notifies listeners immediately
        (the 15 Hz timer would otherwise be the only path, which leaves headless
        callers without a listener notification). */
    void postErrorImmediate (const juce::String& message)
    {
        {
            const juce::ScopedLock sl (statusLock);
            pendingError = message;
            ++errorSerial;
            reportedErrorSerial = errorSerial;
        }
        notifyError (message);
    }

    /** True when Core Audio currently has an output device open. Message thread. */
    bool isDeviceOpen() const { return manager.getCurrentAudioDevice() != nullptr; }

    /** Message thread: clear a stale status error without re-notifying listeners. */
    void clearError()
    {
        const juce::ScopedLock sl (statusLock);
        pendingError.clear();
    }

    void postInfo (const juce::String& message)
    {
        const juce::ScopedLock sl (statusLock);
        pendingInfo = message;
        ++infoSerial;
    }

    void notifyTrackChanged (const Track& t)
    {
        std::vector<Listener*> copy;
        {
            const juce::ScopedLock sl (listenerLock);
            copy.assign (listeners.begin(), listeners.end());
        }
        for (auto* l : copy)
            l->engineTrackChanged (t);
    }

    void notifyQueueChanged()
    {
        std::vector<Listener*> copy;
        {
            const juce::ScopedLock sl (listenerLock);
            copy.assign (listeners.begin(), listeners.end());
        }
        for (auto* l : copy)
            l->engineQueueChanged();
    }

    void notifyDevicesChanged()
    {
        std::vector<Listener*> copy;
        {
            const juce::ScopedLock sl (listenerLock);
            copy.assign (listeners.begin(), listeners.end());
        }
        for (auto* l : copy)
            l->engineDevicesChanged();
    }

    void notifyError (const juce::String& message)
    {
        std::vector<Listener*> copy;
        {
            const juce::ScopedLock sl (listenerLock);
            copy.assign (listeners.begin(), listeners.end());
        }
        for (auto* l : copy)
            l->engineError (message);
    }

    void notifyReferenceModeChanged (bool active)
    {
        std::vector<Listener*> copy;
        {
            const juce::ScopedLock sl (listenerLock);
            copy.assign (listeners.begin(), listeners.end());
        }
        for (auto* l : copy)
            l->engineReferenceModeChanged (active);
    }

    //== Device / rate helpers ================================================
    bool isRateSupported (double rate) const
    {
        if (rate <= 0.0)
            return false;
        const auto name = devices.getCurrentDeviceName();
        if (name.isEmpty())
            return false;
        for (auto r : devices.getDeviceSampleRates (name))
            if (std::abs (r - rate) < 0.5)
                return true;
        return false;
    }

    double currentDeviceRate() const
    {
        return devices.getCurrentSampleRate();
    }

    /** Rate the engine would like to run at for this track (0 = unknown). */
    double desiredRateForTrack (const Track& track) const
    {
        const double current = currentDeviceRate();
        AudioFileInfo info;
        juce::String error;
        if (! DecodeSource::probeInfo (juce::File (track.path), info, error) || info.sampleRate <= 0)
            return current;

        const double fileRate = (double) info.sampleRate;

        if (referenceActive.load())
            return isRateSupported (fileRate) ? fileRate : current;

        if (options.nativeSampleRateSwitching && isRateSupported (fileRate))
            return fileRate;

        if (options.preferredSampleRate > 0.0 && isRateSupported (options.preferredSampleRate))
            return options.preferredSampleRate;

        return current;
    }

    /** Applies a device rate change; optionally reloads the current track at
        its current position (when the rate change was not part of starting a
        new track). */
    bool applyDeviceRate (double rate, bool reloadCurrentTrack)
    {
        const double current = currentDeviceRate();
        if (rate <= 0.0 || std::abs (rate - current) < 0.5)
            return true;

        suspendDecodeAtomic.store (true);

        const PlaybackState oldState = stateAtomic.load();
        const bool hadTrack = hasLoadedTrack();

        if (reloadCurrentTrack && hadTrack)
            loadPendingAtomic.store (true);

        juce::String error;
        if (! devices.setSampleRate (rate, error))
        {
            loadPendingAtomic.store (false);
            suspendDecodeAtomic.store (false);
            postInfo (error);
            return false;
        }

        postInfo ("Output sample rate: " + formatRate (rate) + " Hz");

        if (reloadCurrentTrack && hadTrack)
        {
            Command c;
            Track track;
            int index = -1;
            if (getLoadedSnapshot (track, index))
            {
                c.type = CommandType::load;
                c.track = track;
                c.index = index;
                c.ms = positionMsAtomic.load();
                c.autoPlay = (oldState == PlaybackState::playing);
                postCommand (c);
                stateAtomic.store (c.autoPlay ? PlaybackState::loading : PlaybackState::paused);
            }
        }

        suspendDecodeAtomic.store (false);
        return true;
    }

    /** Decides what follows the current track for gapless purposes. Returns
        true when the next track can be spliced at the current device rate. */
    bool prepareNextForGapless (int currentIdx)
    {
        juce::String ignored;
        juce::ignoreUnused (ignored);
        if (! options.gaplessEnabled)
        {
            Command c;
            c.type = CommandType::clearNext;
            postCommand (c);
            return false;
        }

        Track nextTrack;
        int nextIndex = -1;
        {
            const juce::ScopedLock sl (configLock);
            nextIndex = currentIdx + 1;
            if (currentIdx < 0 || nextIndex >= (int) queue.size())
            {
                Command c;
                c.type = CommandType::clearNext;
                postCommand (c);
                return false;
            }
            nextTrack = queue[(size_t) nextIndex];
        }

        const double currentRate = currentDeviceRate();
        const double nextRate = desiredRateForTrack (nextTrack);
        if (std::abs (nextRate - currentRate) > 0.5)
        {
            // Cannot splice across a device rate change: let the track end and
            // auto-advance with a rate switch at the boundary.
            Command c;
            c.type = CommandType::clearNext;
            postCommand (c);
            return false;
        }

        Command c;
        c.type = CommandType::setNext;
        c.track = nextTrack;
        c.index = nextIndex;
        postCommand (c);
        return true;
    }

    //== Transport helpers (message thread) ===================================
    void startTrackAt (const Track& track, int index, std::int64_t startMs, bool autoPlay)
    {
        // Never leave the UI in "Loading" when there is no output device: report
        // an explicit error and remember to resume when Core Audio returns one.
        if (! initialised || ! isDeviceOpen())
        {
            {
                const juce::ScopedLock sl (configLock);
                queueIndex = index;
            }
            stateAtomic.store (PlaybackState::error);
            endOfQueueAtomic.store (false);
            naturalEndIndexAtomic.store (-1);
            publishedIndexAtomic.store (index);
            positionMsAtomic.store (startMs);
            durationMsAtomic.store (track.durationMs);
            resumeWhenDeviceReturns = true;
            postErrorImmediate ("No audio output device is available. Connect an output device and press Play again.");
            return;
        }

        if (resumeWhenDeviceReturns)
            clearError();
        resumeWhenDeviceReturns = false;

        // Switch the device rate first so the decode thread loads the track at
        // the right rate from its first frame.
        loadPendingAtomic.store (true);
        applyDeviceRate (desiredRateForTrack (track), false);

        {
            const juce::ScopedLock sl (configLock);
            queueIndex = index;
        }

        stateAtomic.store (autoPlay ? PlaybackState::loading : PlaybackState::paused);
        endOfQueueAtomic.store (false);
        naturalEndIndexAtomic.store (-1);
        ignorePublishedIndexUntilMs = juce::Time::currentTimeMillis() + 400;

        Command c;
        c.type = CommandType::load;
        c.track = track;
        c.index = index;
        c.ms = startMs;
        c.autoPlay = autoPlay;
        postCommand (c);

        prepareNextForGapless (index);
    }

    void syncQueueIndexFromDecode()
    {
        const int idx = publishedIndexAtomic.load();
        if (idx < 0)
            return;
        if (juce::Time::currentTimeMillis() < ignorePublishedIndexUntilMs)
            return;

        const juce::ScopedLock sl (configLock);
        if (idx < (int) queue.size())
            queueIndex = idx;
    }

    bool hasLoadedTrack() const
    {
        const juce::ScopedLock sl (statusLock);
        return sourceSnapshot.valid;
    }

    bool getLoadedSnapshot (Track& trackOut, int& indexOut) const
    {
        const juce::ScopedLock sl (statusLock);
        if (! sourceSnapshot.valid)
            return false;
        trackOut = sourceSnapshot.track;
        indexOut = sourceSnapshot.index;
        return true;
    }

    void reloadCurrentTrackAtPosition (bool autoPlay)
    {
        Track track;
        int index = -1;
        if (! getLoadedSnapshot (track, index))
            return;

        loadPendingAtomic.store (true);
        Command c;
        c.type = CommandType::load;
        c.track = track;
        c.index = index;
        c.ms = positionMsAtomic.load();
        c.autoPlay = autoPlay;
        postCommand (c);
        stateAtomic.store (autoPlay ? PlaybackState::loading : PlaybackState::paused);
    }

    Track trackForIndex (int index) const
    {
        const juce::ScopedLock sl (configLock);
        if (index >= 0 && index < (int) queue.size())
            return queue[(size_t) index];
        return {};
    }

    int queueSize() const
    {
        const juce::ScopedLock sl (configLock);
        return (int) queue.size();
    }

    //== ReplayGain (message thread) ==========================================
    void applyReplayGain()
    {
        DspParams params;
        Track track;
        {
            const juce::ScopedLock sl (configLock);
            params = baseParams;
        }
        {
            const juce::ScopedLock sl (statusLock);
            if (sourceSnapshot.valid)
                track = sourceSnapshot.track;
        }

        const bool enabled = params.replayGain.enabled && ! referenceActive.load();
        float gain = 0.0f;
        bool hasGain = false;
        float peak = 0.0f;

        if (overrideReplayGainValid)
        {
            gain = overrideReplayGainDb;
            hasGain = overrideHasGain;
            peak = overridePeak;
        }
        else if (track.hasReplayGainTrack)
        {
            gain = track.replayGainTrackDb;
            hasGain = true;
            peak = track.peakLinear;
        }
        else if (track.hasReplayGainAlbum && params.replayGain.albumMode)
        {
            gain = track.replayGainAlbumDb;
            hasGain = true;
            peak = track.peakLinear;
        }
        else
        {
            gain = params.replayGain.fallbackGainDb;
        }

        if (! params.replayGain.preventClipping || peak <= 0.0f)
        {
            // nothing to clamp against
        }
        else
        {
            const float predictedPeakDb = 20.0f * std::log10 (peak) + gain;
            if (predictedPeakDb > 0.0f)
                gain -= predictedPeakDb;
        }

        if (! enabled)
        {
            chain.setTrackReplayGain (0.0f, 0.0f, false);
            replayGainActiveAtomic.store (false);
            replayGainDbAtomic.store (0.0f);
            return;
        }

        const bool active = std::abs (gain) > 1.0e-4f;
        chain.setTrackReplayGain (gain, peak, hasGain);
        replayGainActiveAtomic.store (active);
        replayGainDbAtomic.store (active ? gain : 0.0f);
    }

    //== Status / signal path =================================================
    SignalPathInfo buildSignalPath() const
    {
        SignalPathInputs in;
        EngineStatus st;
        {
            const juce::ScopedLock sl (statusLock);
            st = publishedStatus;
        }

        SourceSnapshot snapshot;
        {
            const juce::ScopedLock sl (statusLock);
            snapshot = sourceSnapshot;
        }

        auto* device = const_cast<juce::AudioDeviceManager&> (manager).getCurrentAudioDevice();
        const bool deviceOpen = device != nullptr;

        in.hasTrack = snapshot.valid;
        in.sourceFile = snapshot.file;
        in.sourceFormat = snapshot.format;
        in.decoderName = snapshot.decoder;
        in.sourceSampleRate = snapshot.rate;
        in.sourceBitDepth = snapshot.bits;
        in.sourceChannels = snapshot.channels;
        in.sourceIsDsd = snapshot.isDsd;
        in.dsdMultiple = snapshot.dsdMultiple;
        in.sourceIsIntegerPcm = snapshot.integerPcm && ! snapshot.isDsd;
        in.sourceDownmixed = snapshot.downmixed;

        in.resampling = resamplingActiveAtomic.load();
        in.resampleFrom = resampleFromAtomic.load();
        in.resampleTo = resampleToAtomic.load();
        const int quality = resamplerQualityAtomic.load();
        in.resamplerName = "sp-resampler ("
                         + juce::String (quality == 0 ? "fast" : (quality == 1 ? "high" : "ultra")) + ")";

        const DspChainStatus chainStatus = chain.getStatus();
        in.activeDspSteps = chainStatus.activeSteps;
        in.dspProcessing = chainStatus.anyProcessing || ! in.activeDspSteps.empty();
        in.dspBypassed = effectiveParams.bypassed || ! in.dspProcessing;
        in.volumeDb = volumeDb.load();
        in.muted = muted.load();
        in.replayGainApplied = replayGainActiveAtomic.load();
        in.replayGainDb = replayGainDbAtomic.load();

        in.ditherApplied = ditherActiveAtomic.load();
        in.ditherName = ditherActiveAtomic.load()
                          ? TpdfDither::nameForBits (ditherTargetBitsAtomic.load())
                          : juce::String();

        in.outputDeviceName = deviceOpen ? device->getName() : juce::String();
        in.outputDeviceType = deviceOpen ? device->getTypeName() : juce::String();
        in.outputSampleRate = deviceOpen ? juce::roundToInt (device->getCurrentSampleRate()) : 0;
        in.outputBitDepth = nativeBitsAtomic.load();
        in.outputChannels = deviceOutputChannelsAtomic.load();
        in.hogModeActive = hogActiveAtomic.load();
        in.integerOutputFormat = nativeBitsAtomic.load() > 0;
        in.bufferMs = (deviceOpen && device->getCurrentSampleRate() > 0.0)
                        ? 1000.0 * (double) device->getCurrentBufferSizeSamples() / device->getCurrentSampleRate()
                        : 0.0;

        juce::ignoreUnused (st);
        return evaluateSignalPath (in);
    }

    void publishStatus();
    void handleAutoAdvance();

    //== Decode thread methods ================================================
    void decodeLoop();
    void takeCommands();
    void processLoad (const Command& c);
    void processSeek (std::int64_t ms);
    bool flushRing();
    void pump();
    void publishDecodeState();
    void resetEpoch (int index, std::int64_t startMs, std::int64_t durationMs);
    void handleDeviceRateChange();
    void finishNaturalEnd();
    int highWatermarkFrames() const;
};

//==============================================================================
void AudioEngine::Impl::publishStatus()
{
    EngineStatus status;
    {
        const juce::ScopedLock sl (statusLock);
        status = publishedStatus;
    }

    status.state = stateAtomic.load();
    status.positionMs = positionMsAtomic.load();
    status.durationMs = durationMsAtomic.load();
    status.bufferedMs = bufferedMsAtomic.load();
    status.volumeDb = volumeDb.load();
    status.muted = muted.load();
    status.referenceMode = referenceActive.load();
    status.gaplessEnabled = options.gaplessEnabled;
    status.memoryPlaybackActive = memoryActiveAtomic.load();
    status.cpuPercent = cpuPercentAtomic.load();
    status.outputLatencySamples = outputLatencyAtomic.load();
    status.underrunCount = underrunCountAtomic.load();
    status.decodeThreadActive = decodeActiveAtomic.load();
    status.currentQueueIndex = publishedIndexAtomic.load();
    status.queueSize = queueSize();
    status.endOfQueue = endOfQueueAtomic.load();

    SourceSnapshot snapshot;
    {
        const juce::ScopedLock sl (statusLock);
        snapshot = sourceSnapshot;
    }

    if (snapshot.valid)
        status.currentTrack = snapshot.track;

    if (status.currentTrack.path.isEmpty() && status.currentQueueIndex >= 0)
        status.currentTrack = trackForIndex (status.currentQueueIndex);

    auto* device = manager.getCurrentAudioDevice();
    status.deviceOpen = device != nullptr;

    status.dspStatus = chain.getStatus();
    status.meters = hub.getMeterValues();
    status.signalPath = buildSignalPath();

    {
        const juce::ScopedLock sl (statusLock);
        status.errorMessage = pendingError;
        status.infoMessage = pendingInfo;
        publishedStatus = status;
    }

    //---- track change detection -------------------------------------------
    const int idx = publishedIndexAtomic.load();
    const std::uint64_t serial = trackSerialAtomic.load (std::memory_order_acquire);
    if (serial != lastTrackSerial && idx >= 0)
    {
        lastTrackSerial = serial;
        lastPublishedIndex = idx;
        overrideReplayGainValid = false;
        applyReplayGain();

        Track track = snapshot.valid ? snapshot.track : trackForIndex (idx);
        notifyTrackChanged (track);

        // After the decode thread splices the next track in, queue the one after
        // it so gapless playback can continue.
        if (options.gaplessEnabled && stateAtomic.load() == PlaybackState::playing)
            prepareNextForGapless (idx);
        else if (! options.gaplessEnabled)
        {
            Command c;
            c.type = CommandType::clearNext;
            postCommand (c);
        }

        if (referenceActive.load())
        {
            const double want = desiredRateForTrack (track);
            if (std::abs (want - currentDeviceRate()) > 0.5 && isRateSupported (want))
                applyDeviceRate (want, true);
        }
    }

    lastPublishedState = status.state;

    //---- notifications -----------------------------------------------------
    std::uint64_t errSerial = 0, infSerial = 0;
    juce::String errorText, infoText;
    {
        const juce::ScopedLock sl (statusLock);
        errSerial = errorSerial;
        infSerial = infoSerial;
        errorText = pendingError;
        infoText = pendingInfo;
    }

    if (errSerial != reportedErrorSerial)
    {
        reportedErrorSerial = errSerial;
        notifyError (errorText);
    }
    if (infSerial != reportedInfoSerial)
    {
        reportedInfoSerial = infSerial;
        juce::Logger::writeToLog ("Audio: " + infoText);
    }
}

//==============================================================================
void AudioEngine::Impl::handleAutoAdvance()
{
    const std::int64_t endIndex64 = naturalEndIndexAtomic.exchange (-1);
    if (endIndex64 < 0)
        return;

    const int endIndex = (int) endIndex64;

    if (stateAtomic.load() != PlaybackState::stopped)
        return;

    Track nextTrack;
    int nextIndex = -1;
    {
        const juce::ScopedLock sl (configLock);
        nextIndex = endIndex + 1;
        if (nextIndex >= 0 && nextIndex < (int) queue.size())
            nextTrack = queue[(size_t) nextIndex];
    }

    if (nextIndex < 0 || nextTrack.path.isEmpty())
    {
        endOfQueueAtomic.store (true);
        return;
    }

    startTrackAt (nextTrack, nextIndex, 0, true);
}

//==============================================================================
// Decode thread
//==============================================================================
int AudioEngine::Impl::highWatermarkFrames() const
{
    if (ring == nullptr)
        return 0;

    const double rate = preparedRate > 0.0 ? preparedRate : deviceRateAtomic.load();
    const double seconds = playingDesired ? kTargetBufferedSeconds : kPausedPrefillSeconds;
    const int capacity = ring->getCapacityFrames();
    return juce::jlimit (1024, capacity / 2, (int) (seconds * rate));
}

void AudioEngine::Impl::resetEpoch (int index, std::int64_t startMs, std::int64_t durationMs)
{
    segments.clear();
    Segment s;
    s.startEpoch = 0;
    s.index = index;
    s.startMs = startMs;
    s.durationMs = durationMs;
    segments.push_back (s);
    framesAppended = 0;
    consumedAtEpoch = consumedFramesAtomic.load();
    positionMsAtomic.store (startMs);
    durationMsAtomic.store (durationMs);
    bufferedMsAtomic.store (0);
}

bool AudioEngine::Impl::flushRing()
{
    if (ring == nullptr)
        return true;

    bool ok = true;

    if (deviceRunning.load())
    {
        ring->requestFlush();
        if (! ring->waitForFlushAck (250))
        {
            if (! deviceRunning.load())
            {
                const juce::ScopedLock sl (configLock);
                ring->cancelFlush();
                ring->consumerHardReset();
            }
            else
            {
                // Device is running but a callback did not service the flush in
                // time; retry once with a longer window.
                if (! ring->waitForFlushAck (500))
                {
                    ring->cancelFlush();
                    postError ("Audio output stalled while resetting the playback buffer");
                    ok = false;
                }
            }
        }
    }
    else
    {
        const juce::ScopedLock sl (configLock);
        ring->cancelFlush();
        ring->consumerHardReset();
    }

    consumedAtEpoch = consumedFramesAtomic.load();
    framesAppended = 0;
    bufferedMsAtomic.store (0);
    return ok;
}

void AudioEngine::Impl::takeCommands()
{
    std::deque<Command> pending;
    {
        const juce::ScopedLock sl (commandLock);
        pending.swap (commands);
    }

    for (const auto& c : pending)
    {
        if (decodeQuit.load())
            break;

        switch (c.type)
        {
            case CommandType::load:
                processLoad (c);
                break;

            case CommandType::resume:
                if (loaded)
                {
                    playingDesired = true;
                    drainingLocal = false;
                    stateAtomic.store (PlaybackState::playing);
                }
                break;

            case CommandType::pause:
                playingDesired = false;
                if (loaded)
                    stateAtomic.store (PlaybackState::paused);
                break;

            case CommandType::stop:
                playingDesired = false;
                drainingLocal = false;
                drainingAtomic.store (false);
                flushRing();
                loaded = false;
                loadedTrack = {};
                loadedIndex = -1;
                loadedStartMs = 0;
                pipeline.prepare (preparedRate, resamplerQualityAtomic.load());
                segments.clear();
                framesAppended = 0;
                consumedAtEpoch = consumedFramesAtomic.load();
                positionMsAtomic.store (0);
                bufferedMsAtomic.store (0);
                memoryActiveAtomic.store (false);
                resamplingActiveAtomic.store (false);
                naturalEndIndexAtomic.store (-1);
                stateAtomic.store (PlaybackState::stopped);
                break;

            case CommandType::seek:
                processSeek (c.ms);
                break;

            case CommandType::setNext:
                pipeline.setNextTrack (c.track, c.index);
                break;

            case CommandType::clearNext:
                pipeline.clearNextTrack();
                break;

            case CommandType::setQuality:
                resamplerQualityAtomic.store (juce::jlimit (0, 2, c.quality));
                pipeline.setResamplerQuality (resamplerQualityAtomic.load());
                break;

            case CommandType::setMemory:
                memoryPlaybackOptionAtomic.store (c.flag);
                pipeline.setMemoryPlayback (c.flag);
                break;

            case CommandType::quit:
                decodeQuit.store (true);
                break;

            case CommandType::none:
            default:
                break;
        }
    }
}

void AudioEngine::Impl::processLoad (const Command& c)
{
    if (decodeQuit.load())
        return;

    loadPendingAtomic.store (false);
    playingDesired = false;
    drainingLocal = false;
    drainingAtomic.store (false);
    endOfQueueAtomic.store (false);
    naturalEndIndexAtomic.store (-1);

    // The rate may have changed on the message thread before this command was
    // posted; make sure the pipeline is configured for it.
    handleDeviceRateChange();

    if (! flushRing())
    {
        loaded = false;
        playingDesired = false;
        stateAtomic.store (PlaybackState::error);
        return;
    }

    juce::String error;
    if (! pipeline.loadTrack (c.track, c.index, c.ms, error))
    {
        loaded = false;
        playingDesired = false;
        stateAtomic.store (PlaybackState::error);
        postError (error.isNotEmpty() ? error : ("Could not play " + c.track.path));
        return;
    }

    loaded = true;
    loadedTrack = c.track;
    loadedIndex = c.index;
    loadedStartMs = c.ms;
    drainingLocal = false;

    const auto& info = pipeline.currentInfo();
    resetEpoch (c.index, c.ms, info.durationMs);

    // Publish the source facts used for the signal path display.
    {
        const juce::ScopedLock sl (statusLock);
        sourceSnapshot.valid = true;
        sourceSnapshot.track = c.track;
        sourceSnapshot.index = c.index;
        sourceSnapshot.file = info.path;
        sourceSnapshot.format = info.formatName;
        sourceSnapshot.decoder = info.decoderName;
        sourceSnapshot.rate = info.sampleRate;
        sourceSnapshot.bits = info.bitDepth;
        sourceSnapshot.channels = info.numChannels;
        sourceSnapshot.isDsd = info.isDsd;
        sourceSnapshot.dsdMultiple = info.dsdMultiple;
        sourceSnapshot.integerPcm = (info.family == Track::Family::pcmInteger);
        sourceSnapshot.downmixed = info.downmixedToStereo;
        sourceSnapshot.resampling = pipeline.isResampling();
        sourceSnapshot.resampleFrom = pipeline.resampleFrom();
        sourceSnapshot.resampleTo = pipeline.resampleTo();
        sourceSnapshot.resamplerName = "sp-resampler";
    }

    resamplingActiveAtomic.store (pipeline.isResampling());
    resampleFromAtomic.store (pipeline.resampleFrom());
    resampleToAtomic.store (pipeline.resampleTo());
    memoryActiveAtomic.store (pipeline.currentTrackUsesMemory());
    publishedIndexAtomic.store (c.index);
    trackSerialAtomic.fetch_add (1, std::memory_order_release);
    durationMsAtomic.store (info.durationMs);
    positionMsAtomic.store (c.ms);
    bufferedMsAtomic.store (0);

    if (c.autoPlay)
    {
        playingDesired = true;
        stateAtomic.store (PlaybackState::playing);
    }
    else
    {
        playingDesired = false;
        stateAtomic.store (PlaybackState::paused);
    }
}

void AudioEngine::Impl::processSeek (std::int64_t ms)
{
    if (! loaded)
        return;

    const bool wasPlaying = playingDesired;
    if (! flushRing())
    {
        playingDesired = false;
        stateAtomic.store (PlaybackState::error);
        return;
    }

    juce::String error;
    if (! pipeline.loadTrack (loadedTrack, loadedIndex, ms, error))
    {
        postError (error.isNotEmpty() ? error : "Could not seek");
        playingDesired = false;
        stateAtomic.store (PlaybackState::error);
        return;
    }

    loadedStartMs = ms;
    drainingLocal = false;
    drainingAtomic.store (false);
    naturalEndIndexAtomic.store (-1);
    endOfQueueAtomic.store (false);
    resetEpoch (loadedIndex, ms, pipeline.currentInfo().durationMs);
    positionMsAtomic.store (ms);
    bufferedMsAtomic.store (0);
    memoryActiveAtomic.store (pipeline.currentTrackUsesMemory());
    resamplingActiveAtomic.store (pipeline.isResampling());

    playingDesired = wasPlaying;
    stateAtomic.store (wasPlaying ? PlaybackState::playing : PlaybackState::paused);
}

void AudioEngine::Impl::handleDeviceRateChange()
{
    const double newRate = deviceRateAtomic.load();
    if (newRate <= 0.0 || std::abs (newRate - preparedRate) < 0.5)
        return;

    const bool suspended = suspendDecodeAtomic.load() || loadPendingAtomic.load();
    const std::int64_t position = positionMsAtomic.load();
    const PlaybackState oldState = stateAtomic.load();
    const bool wasLoaded = loaded;

    preparedRate = newRate;
    pipeline.prepare (preparedRate, resamplerQualityAtomic.load());

    if (suspended)
    {
        // A new Load command is on its way from the message thread.
        loaded = false;
        playingDesired = false;
        stateAtomic.store (PlaybackState::loading);
        resamplingActiveAtomic.store (false);
        return;
    }

    if (wasLoaded)
    {
        flushRing();
        juce::String error;
        if (pipeline.loadTrack (loadedTrack, loadedIndex, position, error))
        {
            resetEpoch (loadedIndex, position, pipeline.currentInfo().durationMs);
            positionMsAtomic.store (position);
            resamplingActiveAtomic.store (pipeline.isResampling());
            resampleFromAtomic.store (pipeline.resampleFrom());
            resampleToAtomic.store (pipeline.resampleTo());
            playingDesired = (oldState == PlaybackState::playing);
            stateAtomic.store (playingDesired ? PlaybackState::playing : PlaybackState::paused);
        }
        else
        {
            loaded = false;
            playingDesired = false;
            stateAtomic.store (PlaybackState::error);
            postError (error);
        }
    }
}

void AudioEngine::Impl::pump()
{
    if (! loaded || ring == nullptr)
        return;

    handleDeviceRateChange();

    if (! deviceRunning.load() || suspendDecodeAtomic.load())
        return;

    if (ring->isFlushPending())
    {
        if (! ring->waitForFlushAck (250))
            return;
        consumedAtEpoch = consumedFramesAtomic.load();
    }

    const int high = highWatermarkFrames();

    while (! decodeQuit && deviceRunning.load())
    {
        const int buffered = ring->getBufferedFrames();
        if (buffered >= high)
        {
            // Idle decode-thread work: prefetch the next track into RAM.
            pipeline.prefetchNext (8192);
            break;
        }

        const int freeFrames = ring->getFreeFrames();
        if (freeFrames < 256)
            break;

        const int block = juce::jmin (kDecodeBlockFrames, freeFrames);
        PipelineTransition transition;
        const std::int64_t blockStart = framesAppended;

        const int n = pipeline.read (decodeScratch.data(), block, transition);

        if (transition.occurred)
        {
            Segment s;
            s.startEpoch = blockStart + transition.frameOffset;
            s.index = transition.newQueueIndex;
            s.startMs = 0;
            s.durationMs = pipeline.currentInfo().durationMs;
            segments.push_back (s);
            if (segments.size() > 8)
                segments.erase (segments.begin());

            loadedTrack = transition.newTrack;
            loadedIndex = transition.newQueueIndex;
            loadedStartMs = 0;
            drainingLocal = false;
            drainingAtomic.store (false);
            naturalEndIndexAtomic.store (-1);

            const auto& info = pipeline.currentInfo();
            {
                const juce::ScopedLock sl (statusLock);
                sourceSnapshot.valid = true;
                sourceSnapshot.track = transition.newTrack;
                sourceSnapshot.index = transition.newQueueIndex;
                sourceSnapshot.file = info.path;
                sourceSnapshot.format = info.formatName;
                sourceSnapshot.decoder = info.decoderName;
                sourceSnapshot.rate = info.sampleRate;
                sourceSnapshot.bits = info.bitDepth;
                sourceSnapshot.channels = info.numChannels;
                sourceSnapshot.isDsd = info.isDsd;
                sourceSnapshot.dsdMultiple = info.dsdMultiple;
                sourceSnapshot.integerPcm = (info.family == Track::Family::pcmInteger);
                sourceSnapshot.downmixed = info.downmixedToStereo;
                sourceSnapshot.resampling = pipeline.isResampling();
                sourceSnapshot.resampleFrom = pipeline.resampleFrom();
                sourceSnapshot.resampleTo = pipeline.resampleTo();
            }
            publishedIndexAtomic.store (transition.newQueueIndex);
            trackSerialAtomic.fetch_add (1, std::memory_order_release);
            durationMsAtomic.store (info.durationMs);
            resamplingActiveAtomic.store (pipeline.isResampling());
            resampleFromAtomic.store (pipeline.resampleFrom());
            resampleToAtomic.store (pipeline.resampleTo());
            memoryActiveAtomic.store (pipeline.currentTrackUsesMemory());
        }

        if (n > 0)
        {
            if (suspendDecodeAtomic.load())
                break;
            const juce::ScopedLock sl (configLock);
            const int written = ring->write (decodeScratch.data(), n);
            framesAppended += written;
        }

        if (pipeline.lastError().isNotEmpty())
        {
            postError (pipeline.lastError());
            playingDesired = false;
            stateAtomic.store (PlaybackState::error);
            break;
        }

        if (n == 0)
        {
            if (pipeline.finished())
            {
                finishNaturalEnd();
                break;
            }

            // No progress this iteration: avoid a busy spin.
            juce::Thread::sleep (2);
            if (pipeline.lastError().isNotEmpty())
                break;
        }
    }

    // Drain bookkeeping: when the pipeline is finished and the consumer has
    // emptied the ring, the track is really over.
    if (drainingLocal)
    {
        if (ring->getBufferedFrames() == 0)
        {
            drainingLocal = false;
            drainingAtomic.store (false);
            playingDesired = false;
            stateAtomic.store (PlaybackState::stopped);
            const auto dur = durationMsAtomic.load();
            positionMsAtomic.store (dur);
            naturalEndIndexAtomic.store (loadedIndex);
            endOfQueueAtomic.store (false);
        }
    }
}

void AudioEngine::Impl::finishNaturalEnd()
{
    drainingLocal = true;
    drainingAtomic.store (true);
    memoryActiveAtomic.store (false);

    if (ring != nullptr && ring->getBufferedFrames() == 0)
    {
        drainingLocal = false;
        drainingAtomic.store (false);
        playingDesired = false;
        stateAtomic.store (PlaybackState::stopped);
        positionMsAtomic.store (durationMsAtomic.load());
        naturalEndIndexAtomic.store (loadedIndex);
    }
}

void AudioEngine::Impl::publishDecodeState()
{
    if (ring == nullptr)
        return;

    const std::int64_t epochConsumed = juce::jmax<std::int64_t> (0, consumedFramesAtomic.load() - consumedAtEpoch);

    const Segment* active = nullptr;
    for (const auto& s : segments)
        if (s.startEpoch <= epochConsumed)
            active = &s;

    if (active != nullptr)
    {
        const std::int64_t into = epochConsumed - active->startEpoch;
        std::int64_t pos = active->startMs;
        if (preparedRate > 0.0)
            pos += (into * 1000) / (std::int64_t) preparedRate;
        if (active->durationMs > 0 && pos > active->durationMs)
            pos = active->durationMs;
        positionMsAtomic.store (pos);
        durationMsAtomic.store (active->durationMs);
        publishedIndexAtomic.store (active->index);
    }

    const int buffered = ring->getBufferedFrames();
    bufferedMsAtomic.store (preparedRate > 0.0 ? (std::int64_t) (buffered * 1000.0 / preparedRate) : 0);

    // Prune old segments once playback is well past them.
    while (segments.size() > 3 && segments[1].startEpoch + (std::int64_t) (0.5 * preparedRate) < epochConsumed)
        segments.erase (segments.begin());
}

void AudioEngine::Impl::decodeLoop()
{
    decodeActiveAtomic.store (true);
    preparedRate = deviceRateAtomic.load();
    pipeline.prepare (preparedRate, resamplerQualityAtomic.load());
    decodeScratch.assign ((size_t) kDecodeBlockFrames * 2, 0.0);

    while (! juce::Thread::currentThreadShouldExit() && ! decodeQuit.load())
    {
        takeCommands();
        handleDeviceRateChange();

        if (loaded)
        {
            if (playingDesired || stateAtomic.load() == PlaybackState::paused)
                pump();
            else
                publishDecodeState();
        }
        else
        {
            // Keep the buffered/position readouts stable while stopped.
            bufferedMsAtomic.store (ring != nullptr ? (ring->getBufferedFrames() * 1000) : 0);
        }

        publishDecodeState();
        juce::Thread::sleep (playingDesired ? 2 : 5);
    }

    decodeActiveAtomic.store (false);
    decodeQuit.store (true);
}

//==============================================================================
// AudioEngine public API
//==============================================================================
AudioEngine::AudioEngine() : impl (std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() { shutdown(); }

juce::String AudioEngine::initialise()
{
    if (impl->initialised)
        return {};

    impl->ring = std::make_unique<LockFreeStereoRingBuffer>();
    impl->ring->prepare (kRingCapacityFrames);
    impl->decodeScratch.assign ((size_t) kDecodeBlockFrames * 2, 0.0);

    const auto deviceError = impl->manager.initialiseWithDefaultDevices (0, 2);
    if (deviceError.isNotEmpty())
        impl->postError ("Audio output: " + deviceError);

    impl->manager.addAudioCallback (this);
    impl->manager.addChangeListener (this);
    impl->hub.start();

    // Bring the device buffer/preferred rate in line with the stored options.
    {
        auto setup = impl->manager.getAudioDeviceSetup();
        bool changed = false;
        if (impl->options.bufferFrames > 0 && setup.bufferSize != impl->options.bufferFrames)
        {
            setup.bufferSize = impl->options.bufferFrames;
            changed = true;
        }
        if (impl->options.preferredSampleRate > 0.0
            && std::abs (setup.sampleRate - impl->options.preferredSampleRate) > 0.5
            && impl->isRateSupported (impl->options.preferredSampleRate))
        {
            setup.sampleRate = impl->options.preferredSampleRate;
            changed = true;
        }
        if (changed)
        {
            const auto error = impl->manager.setAudioDeviceSetup (setup, false);
            if (error.isNotEmpty())
                impl->postInfo ("Device setup: " + error);
        }
    }

    if (impl->options.hogModeEnabled)
    {
        juce::String hogError;
        if (! impl->devices.setHogMode (true, hogError))
            impl->postError (hogError);
    }
    impl->hogActiveAtomic.store (impl->devices.isHogModeActive());

    impl->preparedRate = impl->deviceRateAtomic.load();
    impl->pipeline.prepare (impl->preparedRate, impl->options.resamplerQuality);

    impl->decodeThread = std::make_unique<Impl::DecodeThread> (*impl);
    impl->decodeThread->startThread (juce::Thread::Priority::normal);

    impl->initialised = true;
    startTimerHz (kStatusTimerHz);

    if (impl->manager.getCurrentAudioDevice() == nullptr)
    {
        const auto message = deviceError.isNotEmpty()
                               ? ("Audio output: " + deviceError)
                               : juce::String ("No audio output device is available. "
                                               "Playback will start when one is connected.");
        impl->postErrorImmediate (message);
        impl->deviceWasOpen = false;
        return message;
    }

    impl->postInfo (deviceError.isEmpty()
                      ? ("Audio output: " + impl->devices.getCurrentDeviceName()
                         + " @ " + formatRate (impl->deviceRateAtomic.load()) + " Hz")
                      : juce::String());

    juce::String error = deviceError.isNotEmpty() ? ("Audio output: " + deviceError) : juce::String();
    return error;
}

void AudioEngine::shutdown()
{
    if (! impl->initialised && impl->decodeThread == nullptr)
        return;

    stopTimer();
    stopDebugRecording();

    impl->decodeQuit = true;
    {
        Impl::Command c;
        c.type = Impl::CommandType::quit;
        impl->postCommand (c);
    }

    if (impl->decodeThread != nullptr)
    {
        if (! impl->decodeThread->stopThread (5000))
            juce::Logger::writeToLog ("Audio: decode thread did not stop within 5 s during shutdown");
        impl->decodeThread.reset();
    }

    impl->manager.removeAudioCallback (this);
    impl->manager.removeChangeListener (this);

    juce::String hogError;
    impl->devices.setHogMode (false, hogError);
    impl->hogActiveAtomic.store (false);

    impl->hub.stop();
    impl->manager.closeAudioDevice();
    impl->deviceRunning.store (false);
    impl->initialised = false;

    {
        const juce::ScopedLock sl (impl->configLock);
        impl->ring.reset();
    }

    // Release the DSP / analyzer threads' audio-rate state.
    impl->chain.reset();
}

//==============================================================================
EngineOptions AudioEngine::getOptions() const
{
    const juce::ScopedLock sl (impl->configLock);
    return impl->options;
}

void AudioEngine::setOptions (const EngineOptions& newOptions, bool reopenDevice)
{
    EngineOptions old;
    {
        const juce::ScopedLock sl (impl->configLock);
        old = impl->options;
        impl->options = newOptions;
        impl->options.bufferFrames = juce::jlimit (32, 4096, impl->options.bufferFrames);
        impl->options.resamplerQuality = juce::jlimit (0, 2, impl->options.resamplerQuality);
    }

    impl->ditherEnabledOptionAtomic.store (impl->options.ditherEnabled);
    impl->resamplerQualityAtomic.store (impl->options.resamplerQuality);
    impl->memoryPlaybackOptionAtomic.store (impl->options.memoryPlayback);

    if (old.resamplerQuality != impl->options.resamplerQuality)
    {
        Impl::Command c;
        c.type = Impl::CommandType::setQuality;
        c.quality = impl->options.resamplerQuality;
        impl->postCommand (c);
    }

    if (old.memoryPlayback != impl->options.memoryPlayback)
    {
        Impl::Command c;
        c.type = Impl::CommandType::setMemory;
        c.flag = impl->options.memoryPlayback;
        impl->postCommand (c);
    }

    if (old.gaplessEnabled != impl->options.gaplessEnabled)
    {
        if (! impl->options.gaplessEnabled)
        {
            Impl::Command c;
            c.type = Impl::CommandType::clearNext;
            impl->postCommand (c);
        }
    }

    if (old.hogModeEnabled != impl->options.hogModeEnabled && reopenDevice)
    {
        juce::String error;
        if (impl->options.hogModeEnabled)
        {
            if (! impl->devices.setHogMode (true, error))
                impl->postError (error);
        }
        else
        {
            if (! impl->devices.setHogMode (false, error))
                impl->postError (error);
        }
        impl->hogActiveAtomic.store (impl->devices.isHogModeActive());
    }

    const bool deviceNeedsReopen = reopenDevice
        && (old.bufferFrames != impl->options.bufferFrames
            || std::abs (old.preferredSampleRate - impl->options.preferredSampleRate) > 0.5
            || old.hogModeEnabled != impl->options.hogModeEnabled);

    if (deviceNeedsReopen && impl->manager.getCurrentAudioDevice() != nullptr)
    {
        impl->suspendDecodeAtomic.store (true);
        auto setup = impl->manager.getAudioDeviceSetup();
        setup.bufferSize = impl->options.bufferFrames;
        if (impl->options.preferredSampleRate > 0.0 && impl->isRateSupported (impl->options.preferredSampleRate))
            setup.sampleRate = impl->options.preferredSampleRate;
        const auto error = impl->manager.setAudioDeviceSetup (setup, false);
        if (error.isNotEmpty())
            impl->postError ("Could not apply audio options: " + error);
        impl->suspendDecodeAtomic.store (false);

        // Re-apply the current track at its position with the new device config.
        impl->reloadCurrentTrackAtPosition (impl->stateAtomic.load() == PlaybackState::playing);
        impl->hogActiveAtomic.store (impl->devices.isHogModeActive());
    }
}

void AudioEngine::setGaplessEnabled (bool enabled)
{
    auto o = getOptions();
    o.gaplessEnabled = enabled;
    setOptions (o, false);
}

void AudioEngine::setNativeSampleRateSwitching (bool enabled)
{
    auto o = getOptions();
    o.nativeSampleRateSwitching = enabled;
    setOptions (o, false);
}

void AudioEngine::setMemoryPlayback (bool enabled)
{
    auto o = getOptions();
    o.memoryPlayback = enabled;
    setOptions (o, false);
}

void AudioEngine::setDitherEnabled (bool enabled)
{
    auto o = getOptions();
    o.ditherEnabled = enabled;
    setOptions (o, false);
}

void AudioEngine::setHogModeEnabled (bool enabled)
{
    auto o = getOptions();
    o.hogModeEnabled = enabled;
    setOptions (o, true);
}

void AudioEngine::setResamplerQuality (int quality0to2)
{
    auto o = getOptions();
    o.resamplerQuality = juce::jlimit (0, 2, quality0to2);
    setOptions (o, false);
}

//==============================================================================
void AudioEngine::setQueue (std::vector<Track> tracks, int startIndex, bool autoPlay)
{
    {
        const juce::ScopedLock sl (impl->configLock);
        impl->queue = std::move (tracks);
        impl->queueIndex = impl->queue.empty()
                             ? -1
                             : juce::jlimit (0, (int) impl->queue.size() - 1, startIndex);
    }

    impl->notifyQueueChanged();

    if (impl->queueSize() == 0)
    {
        stop();
        return;
    }

    const int index = juce::jlimit (0, impl->queueSize() - 1, startIndex);
    const auto track = impl->trackForIndex (index);
    if (track.path.isNotEmpty())
    {
        if (autoPlay)
            impl->startTrackAt (track, index, 0, true);
        else
        {
            impl->startTrackAt (track, index, 0, false);
            impl->stateAtomic.store (PlaybackState::paused);
        }
    }
}

void AudioEngine::enqueueNext (const Track& track)
{
    {
        const juce::ScopedLock sl (impl->configLock);
        impl->queue.push_back (track);
    }
    impl->notifyQueueChanged();

    // If the appended track is the one after the current, queue it for gapless.
    const int current = impl->publishedIndexAtomic.load();
    if (current >= 0 && impl->queueSize() == current + 2 && impl->options.gaplessEnabled)
        impl->prepareNextForGapless (current);
}

void AudioEngine::enqueueAfterCurrent (const std::vector<Track>& tracks)
{
    if (tracks.empty())
        return;

    int current = impl->publishedIndexAtomic.load();
    {
        const juce::ScopedLock sl (impl->configLock);
        if (impl->queue.empty())
        {
            impl->queue = tracks;
            impl->queueIndex = 0;
            current = -1;
        }
        else
        {
            if (current < 0 || current >= (int) impl->queue.size())
                current = impl->queueIndex;

            const int insertAt = juce::jlimit (0, (int) impl->queue.size(), current + 1);
            impl->queue.insert (impl->queue.begin() + insertAt, tracks.begin(), tracks.end());
            impl->queueIndex = current;
        }
    }
    impl->notifyQueueChanged();

    if (current >= 0 && impl->options.gaplessEnabled)
        impl->prepareNextForGapless (current);
}

void AudioEngine::clearQueue()
{
    stop();
    {
        const juce::ScopedLock sl (impl->configLock);
        impl->queue.clear();
        impl->queueIndex = -1;
    }
    impl->publishedIndexAtomic.store (-1);
    Impl::Command clearNextCommand;
    clearNextCommand.type = Impl::CommandType::clearNext;
    impl->postCommand (clearNextCommand);
    impl->notifyQueueChanged();
}

std::vector<Track> AudioEngine::getQueue() const
{
    const juce::ScopedLock sl (impl->configLock);
    return impl->queue;
}

int AudioEngine::getCurrentQueueIndex() const
{
    const int published = impl->publishedIndexAtomic.load();
    if (published >= 0)
        return published;
    const juce::ScopedLock sl (impl->configLock);
    return impl->queueIndex;
}

void AudioEngine::playTrack (const Track& track, bool autoPlay)
{
    {
        const juce::ScopedLock sl (impl->configLock);
        impl->queue.clear();
        impl->queue.push_back (track);
        impl->queueIndex = 0;
    }
    impl->notifyQueueChanged();
    impl->startTrackAt (track, 0, 0, autoPlay);
}

void AudioEngine::playQueueIndex (int index, bool autoPlay)
{
    const int size = impl->queueSize();
    if (size == 0 || index < 0 || index >= size)
        return;

    const auto track = impl->trackForIndex (index);
    if (track.path.isEmpty())
        return;

    impl->startTrackAt (track, index, 0, autoPlay);
}

void AudioEngine::play()
{
    const auto state = impl->stateAtomic.load();

    if (state == PlaybackState::playing)
        return;

    if (state == PlaybackState::paused && impl->hasLoadedTrack())
    {
        Impl::Command resumeCommand;
        resumeCommand.type = Impl::CommandType::resume;
        impl->postCommand (resumeCommand);
        impl->stateAtomic.store (PlaybackState::playing);
        return;
    }

    const int size = impl->queueSize();
    if (size == 0)
        return;

    int index = impl->queueIndex;
    if (index < 0 || index >= size)
        index = 0;

    playQueueIndex (index, true);
}

void AudioEngine::pause()
{
    if (impl->stateAtomic.load() == PlaybackState::playing)
    {
        Impl::Command pauseCommand;
        pauseCommand.type = Impl::CommandType::pause;
        impl->postCommand (pauseCommand);
        impl->stateAtomic.store (PlaybackState::paused);
    }
}

void AudioEngine::togglePlayPause()
{
    if (impl->stateAtomic.load() == PlaybackState::playing)
        pause();
    else
        play();
}

void AudioEngine::stop()
{
    Impl::Command stopCommand;
    stopCommand.type = Impl::CommandType::stop;
    impl->postCommand (stopCommand);
    impl->stateAtomic.store (PlaybackState::stopped);
    impl->positionMsAtomic.store (0);
    impl->bufferedMsAtomic.store (0);
    impl->endOfQueueAtomic.store (false);
    impl->naturalEndIndexAtomic.store (-1);
    impl->drainingAtomic.store (false);
}

void AudioEngine::next()
{
    const int size = impl->queueSize();
    if (size == 0)
        return;

    int current = getCurrentQueueIndex();
    if (current < 0)
        current = 0;

    const auto state = impl->stateAtomic.load();

    if (current + 1 >= size)
    {
        stop();
        impl->endOfQueueAtomic.store (true);
        return;
    }

    playQueueIndex (current + 1, state == PlaybackState::playing || state == PlaybackState::stopped);
}

void AudioEngine::previous()
{
    const int size = impl->queueSize();
    if (size == 0)
        return;

    int current = getCurrentQueueIndex();
    if (current < 0)
        current = 0;

    if (impl->positionMsAtomic.load() > 3000)
    {
        seekMs (0);
        return;
    }

    const auto state = impl->stateAtomic.load();

    if (current <= 0)
    {
        seekMs (0);
        return;
    }

    playQueueIndex (current - 1, state == PlaybackState::playing || state == PlaybackState::stopped);
}

void AudioEngine::seekMs (std::int64_t positionMs)
{
    const auto state = impl->stateAtomic.load();
    if (state == PlaybackState::stopped)
        return;

    std::int64_t clamped = juce::jmax<std::int64_t> (0, positionMs);
    const auto duration = impl->durationMsAtomic.load();
    if (duration > 0)
        clamped = juce::jmin (clamped, duration);

    impl->positionMsAtomic.store (clamped);
    Impl::Command seekCommand;
    seekCommand.type = Impl::CommandType::seek;
    seekCommand.ms = clamped;
    impl->postCommand (seekCommand);
}

void AudioEngine::setVolumeDb (float db)
{
    if (impl->referenceActive.load())
        return;   // Reference Mode holds unity volume; exit restores the user value.

    impl->volumeDb.store (juce::jlimit (-144.0f, 12.0f, db));
}

void AudioEngine::setMuted (bool shouldMute)
{
    if (impl->referenceActive.load())
        return;

    impl->muted.store (shouldMute);
}

float AudioEngine::getVolumeDb() const { return impl->volumeDb.load(); }
bool AudioEngine::isMuted() const { return impl->muted.load(); }

void AudioEngine::setRateReplayGainForCurrentTrack (float gainDb, float peakLinear, bool hasGain)
{
    impl->overrideReplayGainValid = true;
    impl->overrideReplayGainDb = gainDb;
    impl->overridePeak = peakLinear;
    impl->overrideHasGain = hasGain;
    impl->applyReplayGain();
}

//==============================================================================
DspChain& AudioEngine::dspChain() { return impl->chain; }
AnalyzerHub& AudioEngine::analyzers() { return impl->hub; }

void AudioEngine::setDspParams (const DspParams& params, int rampMs)
{
    {
        const juce::ScopedLock sl (impl->configLock);
        impl->baseParams = params;
    }

    DspParams effective = params;
    if (impl->referenceActive.load())
    {
        effective.bypassed = true;
        effective.replayGain.enabled = false;
    }
    {
        const juce::ScopedLock sl (impl->configLock);
        impl->effectiveParams = effective;
    }

    impl->chain.setParams (effective, juce::jmax (0, rampMs));
    impl->applyReplayGain();
}

DspParams AudioEngine::getDspParams() const
{
    const juce::ScopedLock sl (impl->configLock);
    return impl->referenceActive.load() ? impl->effectiveParams : impl->baseParams;
}

void AudioEngine::setReferenceMode (bool active)
{
    if (impl->referenceActive.load() == active)
        return;

    const bool wasPlaying = impl->stateAtomic.load() == PlaybackState::playing;
    const std::int64_t position = impl->positionMsAtomic.load();
    Track referenceTrack;
    int referenceIndex = -1;
    const bool hadTrack = impl->getLoadedSnapshot (referenceTrack, referenceIndex);
    juce::ignoreUnused (referenceIndex);

    if (active)
    {
        impl->savedVolumeDb = impl->volumeDb.load();
        impl->savedMuted = impl->muted.load();
        impl->savedDeviceRate = impl->currentDeviceRate();

        impl->volumeDb.store (0.0f);
        impl->muted.store (false);
        impl->referenceActive.store (true);

        DspParams effective;
        {
            const juce::ScopedLock sl (impl->configLock);
            effective = impl->baseParams;
        }
        effective.bypassed = true;
        effective.replayGain.enabled = false;
        {
            const juce::ScopedLock sl (impl->configLock);
            impl->effectiveParams = effective;
        }
        impl->chain.setParams (effective, 60);
        impl->chain.setTrackReplayGain (0.0f, 0.0f, false);
        impl->replayGainActiveAtomic.store (false);
        impl->replayGainDbAtomic.store (0.0f);
        impl->ditherActiveAtomic.store (false);

        // Ask the device for the current file's native rate when supported.
        if (hadTrack)
        {
            const double want = impl->desiredRateForTrack (referenceTrack);
            if (std::abs (want - impl->currentDeviceRate()) > 0.5)
                impl->applyDeviceRate (want, true);
        }
    }
    else
    {
        impl->referenceActive.store (false);
        impl->volumeDb.store (impl->savedVolumeDb);
        impl->muted.store (impl->savedMuted);

        DspParams effective;
        {
            const juce::ScopedLock sl (impl->configLock);
            effective = impl->baseParams;
        }
        {
            const juce::ScopedLock sl (impl->configLock);
            impl->effectiveParams = effective;
        }
        impl->chain.setParams (effective, 60);
        impl->applyReplayGain();

        if (impl->savedDeviceRate > 0.0 && std::abs (impl->savedDeviceRate - impl->currentDeviceRate()) > 0.5)
        {
            if (! impl->applyDeviceRate (impl->savedDeviceRate, true))
                impl->postInfo ("Stayed at " + formatRate (impl->currentDeviceRate()) + " Hz after leaving Reference Mode");
        }
    }

    juce::ignoreUnused (position, wasPlaying);
    impl->notifyReferenceModeChanged (active);
}

bool AudioEngine::isReferenceMode() const { return impl->referenceActive.load(); }

//==============================================================================
DeviceManager& AudioEngine::devices() { return impl->devices; }
juce::AudioDeviceManager& AudioEngine::getDeviceManager() { return impl->manager; }

void AudioEngine::rescanDevices()
{
    impl->notifyDevicesChanged();
}

bool AudioEngine::selectOutputDevice (const juce::String& name, double preferredRate, juce::String& errorOut)
{
    errorOut.clear();

    const bool wasPlaying = impl->stateAtomic.load() == PlaybackState::playing;
    const bool wasLoaded = impl->hasLoadedTrack();

    impl->suspendDecodeAtomic.store (true);

    const int bufferSize = impl->options.bufferFrames;
    if (! impl->devices.openOutputDevice (name, preferredRate, bufferSize, errorOut))
    {
        impl->suspendDecodeAtomic.store (false);
        return false;
    }

    if (impl->options.hogModeEnabled)
    {
        juce::String hogError;
        if (! impl->devices.setHogMode (true, hogError))
            impl->postError (hogError);
    }
    impl->hogActiveAtomic.store (impl->devices.isHogModeActive());

    if (wasLoaded)
        impl->reloadCurrentTrackAtPosition (wasPlaying);

    impl->suspendDecodeAtomic.store (false);
    impl->notifyDevicesChanged();
    impl->postInfo ("Output device: " + impl->devices.getCurrentDeviceName()
                    + " @ " + formatRate (impl->devices.getCurrentSampleRate()) + " Hz");
    return true;
}

//==============================================================================
EngineStatus AudioEngine::getStatus() const
{
    EngineStatus status;
    {
        const juce::ScopedLock sl (impl->statusLock);
        status = impl->publishedStatus;
    }

    // Overlay the live atomics so callers never see a stale snapshot (the
    // timer may not be running in headless tests).
    status.state = impl->stateAtomic.load();
    status.positionMs = impl->positionMsAtomic.load();
    status.durationMs = impl->durationMsAtomic.load();
    status.bufferedMs = impl->bufferedMsAtomic.load();
    status.volumeDb = impl->volumeDb.load();
    status.muted = impl->muted.load();
    status.referenceMode = impl->referenceActive.load();
    status.memoryPlaybackActive = impl->memoryActiveAtomic.load();
    status.cpuPercent = impl->cpuPercentAtomic.load();
    status.outputLatencySamples = impl->outputLatencyAtomic.load();
    status.underrunCount = impl->underrunCountAtomic.load();
    status.decodeThreadActive = impl->decodeActiveAtomic.load();
    status.currentQueueIndex = impl->publishedIndexAtomic.load();
    status.endOfQueue = impl->endOfQueueAtomic.load();
    status.queueSize = impl->queueSize();
    status.deviceOpen = impl->manager.getCurrentAudioDevice() != nullptr;

    {
        const juce::ScopedLock sl (impl->statusLock);
        status.currentTrack = impl->sourceSnapshot.valid
                                ? impl->sourceSnapshot.track
                                : status.currentTrack;
        status.errorMessage = impl->pendingError;
        status.infoMessage = impl->pendingInfo;
    }

    if (status.currentTrack.path.isEmpty() && status.currentQueueIndex >= 0)
        status.currentTrack = impl->trackForIndex (status.currentQueueIndex);

    status.signalPath = impl->buildSignalPath();
    status.dspStatus = impl->chain.getStatus();
    status.meters = impl->hub.getMeterValues();
    return status;
}

PersistedPlaybackState AudioEngine::capturePersistedState() const
{
    PersistedPlaybackState state;
    {
        const juce::ScopedLock sl (impl->configLock);
        state.queue = impl->queue;
        state.currentQueueIndex = impl->queueIndex;
    }
    state.positionMs = impl->positionMsAtomic.load();
    state.volumeDb = impl->volumeDb.load();
    state.muted = impl->muted.load();
    state.referenceMode = impl->referenceActive.load();
    return state;
}

void AudioEngine::restorePersistedState (const PersistedPlaybackState& state)
{
    {
        const juce::ScopedLock sl (impl->configLock);
        impl->queue = state.queue;
        impl->queueIndex = state.currentQueueIndex;
    }
    impl->volumeDb.store (juce::jlimit (-144.0f, 12.0f, state.volumeDb));
    impl->muted.store (state.muted);
    impl->positionMsAtomic.store (state.positionMs);
    impl->publishedIndexAtomic.store (state.currentQueueIndex);
    impl->notifyQueueChanged();
}

void AudioEngine::addListener (Listener* listener)
{
    const juce::ScopedLock sl (impl->listenerLock);
    impl->listeners.push_back (listener);
}

void AudioEngine::removeListener (Listener* listener)
{
    const juce::ScopedLock sl (impl->listenerLock);
    impl->listeners.remove (listener);
}

//==============================================================================
bool AudioEngine::decodeFileToBuffer (const juce::File& file, juce::AudioBuffer<double>& out,
                                      int maxSeconds, juce::String& errorOut)
{
    errorOut.clear();

    auto source = DecodeSource::create (file, errorOut);
    if (source == nullptr)
        return false;

    const auto& info = source->getInfo();
    if (info.sampleRate <= 0 || info.totalFrames <= 0)
    {
        errorOut = "No audio frames in " + file.getFileName();
        return false;
    }

    std::int64_t framesWanted = info.totalFrames;
    if (maxSeconds > 0)
        framesWanted = juce::jmin<std::int64_t> (framesWanted, (std::int64_t) maxSeconds * info.sampleRate);

    if (framesWanted <= 0)
    {
        errorOut = "No audio frames requested";
        return false;
    }

    out.setSize (2, (int) framesWanted, false, true, false);
    out.clear();

    std::vector<double> interleaved ((size_t) kDecodeBlockFrames * 2, 0.0);
    std::int64_t frame = 0;

    while (frame < framesWanted)
    {
        const int want = (int) juce::jmin<std::int64_t> ((std::int64_t) kDecodeBlockFrames, framesWanted - frame);
        const int got = source->readFrames (interleaved.data(), want);

        if (got <= 0)
        {
            const auto sourceError = source->getLastError();
            errorOut = sourceError.isNotEmpty()
                         ? sourceError
                         : ("Decode ended early at frame " + juce::String (frame) + " of "
                            + juce::String (framesWanted) + " (file truncated or corrupt)");
            return false;
        }

        auto* left = out.getWritePointer (0);
        auto* right = out.getWritePointer (1);
        for (int i = 0; i < got; ++i)
        {
            left[frame + i] = interleaved[(size_t) i * 2];
            right[frame + i] = interleaved[(size_t) i * 2 + 1];
        }
        frame += got;
    }

    const auto integrityError = source->getLastError();
    if (integrityError.isNotEmpty())
    {
        errorOut = integrityError;
        return false;
    }

    return true;
}

//==============================================================================
bool AudioEngine::startDebugRecording (const juce::File& wavFile, juce::String& errorOut)
{
    errorOut.clear();

    if (impl->debugRecorder.isRecording())
    {
        errorOut = "Debug recording is already running";
        return false;
    }

    const double rate = impl->currentDeviceRate();
    if (rate <= 0.0)
    {
        errorOut = "No output device is open";
        return false;
    }

    if (! impl->debugRecorder.startRecording (wavFile, rate, errorOut))
        return false;

    impl->recorderForCallback.store (&impl->debugRecorder, std::memory_order_release);
    impl->postInfo ("Debug recording: " + wavFile.getFullPathName());
    return true;
}

void AudioEngine::stopDebugRecording()
{
    impl->recorderForCallback.store (nullptr, std::memory_order_release);
    impl->debugRecorder.stopRecording();
}

bool AudioEngine::isDebugRecording() const { return impl->debugRecorder.isRecording(); }

double AudioEngine::getOutputSampleRate() const
{
    return impl->manager.getCurrentAudioDevice() != nullptr ? impl->deviceRateAtomic.load() : 0.0;
}

//==============================================================================
// Core Audio callback
//==============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device == nullptr)
        return;

    impl->deviceRunning.store (false);

    const double rate = device->getCurrentSampleRate();
    const int block = device->getCurrentBufferSizeSamples();
    const int numOut = device->getActiveOutputChannels().countNumberOfSetBits();

    impl->deviceRateAtomic.store (rate > 0.0 ? rate : 44100.0);
    impl->deviceBufferFramesAtomic.store (block);
    impl->deviceOutputChannelsAtomic.store (juce::jmax (1, numOut));
    impl->outputLatencyAtomic.store (device->getOutputLatencyInSamples());

    const int maxBlock = juce::jlimit (block, kMaxCallbackBlock, juce::jmax (block, 4096));

    impl->ringScratch.assign ((size_t) maxBlock * 2, 0.0);
    impl->dspBuffer.setSize (2, maxBlock, false, true, false);
    impl->dspBuffer.clear();
    impl->preDspFloat.setSize (2, maxBlock, false, true, false);
    impl->preDspFloat.clear();
    impl->postDspFloat.setSize (2, maxBlock, false, true, false);
    impl->postDspFloat.clear();
    impl->dspChannelPointers[0] = impl->dspBuffer.getWritePointer (0);
    impl->dspChannelPointers[1] = impl->dspBuffer.getWritePointer (1);
    impl->allocatedBlock = maxBlock;

    impl->fadeInTotal = juce::jmax (1, (int) (kFadeSeconds * rate));
    impl->fadeOutTotal = impl->fadeInTotal;
    impl->fadeInRemaining = 0;
    impl->fadeOutRemaining = 0;
    impl->lastCallbackWasPlaying = false;

    impl->volumeSmoother.reset (rate, 0.05);
    impl->volumeSmoother.setCurrentAndTargetValue (impl->muted.load() ? 0.0
                                                  : juce::Decibels::decibelsToGain (impl->volumeDb.load()));

    impl->chain.prepare (rate > 0.0 ? rate : 44100.0, maxBlock, 2);
    impl->hub.prepare (rate > 0.0 ? rate : 44100.0, maxBlock);
    impl->ditherGenerator.prepare();

    // The device may have restarted at a different rate/configuration: discard
    // any audio left over from the previous device instance. No callback is
    // running at this point.
    {
        const juce::ScopedLock sl (impl->configLock);
        if (impl->ring != nullptr)
        {
            impl->ring->cancelFlush();
            impl->ring->consumerHardReset();
        }
    }

    impl->deviceRunning.store (true);
}

void AudioEngine::audioDeviceStopped()
{
    impl->deviceRunning.store (false);
    impl->lastCallbackWasPlaying = false;
}

void AudioEngine::audioDeviceError (const juce::String& errorMessage)
{
    impl->postError ("Audio device error: " + errorMessage);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused (inputChannelData, numInputChannels, context);

    if (outputChannelData == nullptr || numOutputChannels <= 0 || numSamples <= 0)
        return;

    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    if (! impl->deviceRunning.load() || impl->allocatedBlock <= 0 || numSamples > impl->allocatedBlock)
        return;

    const auto startTicks = juce::Time::getHighResolutionTicks();

    const double rate = impl->deviceRateAtomic.load();
    const auto state = impl->stateAtomic.load();
    const bool playing = (state == PlaybackState::playing);

    // Transport fades avoid clicks on play/pause/stop/seek.
    if (playing && ! impl->lastCallbackWasPlaying)
    {
        impl->fadeInRemaining = impl->fadeInTotal;
        impl->fadeOutRemaining = 0;
    }
    else if (! playing && impl->lastCallbackWasPlaying)
    {
        impl->fadeOutRemaining = impl->fadeOutTotal;
        impl->fadeInRemaining = 0;
    }

    // A flush request from the decode thread (seek/load/rate change) must be
    // acknowledged even while paused or stopped; otherwise the decode thread
    // waits for a consumer that is not reading.
    if (impl->ring != nullptr)
        impl->ring->serviceFlushRequest();

    const bool readingAudio = playing || impl->fadeOutRemaining > 0 || impl->fadeInRemaining > 0;

    int framesRead = 0;
    if (readingAudio && impl->ring != nullptr)
        framesRead = impl->ring->read (impl->ringScratch.data(), numSamples);

    impl->consumedFramesAtomic.fetch_add (framesRead, std::memory_order_relaxed);

    const bool starving = playing && framesRead < numSamples && ! impl->drainingAtomic.load();
    if (starving)
    {
        impl->underrunCountAtomic.fetch_add (1, std::memory_order_relaxed);
        impl->hub.notifyUnderrun();
    }

    // Deinterleave the ring into the double DSP buffer; zero-fill any shortage.
    auto* dspL = impl->dspBuffer.getWritePointer (0);
    auto* dspR = impl->dspBuffer.getWritePointer (1);
    for (int i = 0; i < framesRead; ++i)
    {
        dspL[i] = impl->ringScratch[(size_t) i * 2];
        dspR[i] = impl->ringScratch[(size_t) i * 2 + 1];
    }
    for (int i = framesRead; i < numSamples; ++i)
    {
        dspL[i] = 0.0;
        dspR[i] = 0.0;
    }

    // Pre-DSP metering tap (float).
    auto* preL = impl->preDspFloat.getWritePointer (0);
    auto* preR = impl->preDspFloat.getWritePointer (1);
    for (int i = 0; i < numSamples; ++i)
    {
        preL[i] = (float) dspL[i];
        preR[i] = (float) dspR[i];
    }
    impl->hub.pushInput (preL, preR, numSamples);

    // 64-bit DSP chain (bypassed by default / in Reference Mode).
    {
        juce::AudioBuffer<double> view (impl->dspBuffer.getArrayOfWritePointers(), 2, numSamples);
        impl->chain.process (view);
    }

    // Smoothed master volume + mute + transport fade.
    const double targetGain = impl->muted.load() ? 0.0 : juce::Decibels::decibelsToGain (impl->volumeDb.load());
    impl->volumeSmoother.setTargetValue (targetGain);

    const bool ditherOn = impl->ditherActiveAtomic.load();
    const int ditherBits = impl->ditherTargetBitsAtomic.load();
    const int sourceBits = impl->ditherSourceBitsAtomic.load();
    if (ditherOn && ditherBits > 0)
        impl->ditherGenerator.process (dspL, dspR, numSamples, ditherBits, juce::jmax (sourceBits, ditherBits + 1));

    auto* outL = impl->postDspFloat.getWritePointer (0);
    auto* outR = impl->postDspFloat.getWritePointer (1);
    std::int64_t clipped = 0;

    for (int i = 0; i < numSamples; ++i)
    {
        double transportGain = 1.0;

        if (impl->fadeInRemaining > 0)
        {
            transportGain = 1.0 - (double) impl->fadeInRemaining / (double) impl->fadeInTotal;
            --impl->fadeInRemaining;
        }
        else if (! playing && impl->fadeOutRemaining > 0)
        {
            transportGain = (double) impl->fadeOutRemaining / (double) impl->fadeOutTotal;
            --impl->fadeOutRemaining;
        }
        else if (! playing)
        {
            transportGain = 0.0;
        }

        const double gain = impl->volumeSmoother.getNextValue() * transportGain;

        double l = dspL[i] * gain;
        double r = dspR[i] * gain;

        if (l > 1.0) { l = 1.0; ++clipped; }
        else if (l < -1.0) { l = -1.0; ++clipped; }
        if (r > 1.0) { r = 1.0; ++clipped; }
        else if (r < -1.0) { r = -1.0; ++clipped; }

        outL[i] = (float) l;
        outR[i] = (float) r;
    }

    if (clipped > 0)
        impl->clipCountAtomic.fetch_add (clipped, std::memory_order_relaxed);

    impl->hub.pushOutput (outL, outR, numSamples);

    // Write to the device.
    if (numOutputChannels == 1)
    {
        auto* mono = outputChannelData[0];
        if (mono != nullptr)
            for (int i = 0; i < numSamples; ++i)
                mono[i] = 0.5f * (outL[i] + outR[i]);
    }
    else
    {
        auto* left = outputChannelData[0];
        auto* right = outputChannelData[1];
        if (left != nullptr)
            juce::FloatVectorOperations::copy (left, outL, numSamples);
        if (right != nullptr)
            juce::FloatVectorOperations::copy (right, outR, numSamples);
        for (int ch = 2; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);
    }

    // Debug verification tap (lock-free, no disk I/O here).
    if (auto* recorder = impl->recorderForCallback.load (std::memory_order_acquire))
        recorder->push (outL, outR, numSamples);

    impl->lastCallbackWasPlaying = playing;

    // CPU load: callback time vs the block's wall-clock duration.
    const auto elapsedSec = juce::Time::highResolutionTicksToSeconds (
        juce::Time::getHighResolutionTicks() - startTicks);
    const double blockSec = rate > 0.0 ? (double) numSamples / rate : 0.0;
    if (blockSec > 0.0)
    {
        const double percent = juce::jlimit (0.0, 400.0, 100.0 * elapsedSec / blockSec);
        const double previous = impl->cpuPercentAtomic.load (std::memory_order_relaxed);
        impl->cpuPercentAtomic.store (previous * 0.9 + percent * 0.1, std::memory_order_relaxed);
    }
}

//==============================================================================
void AudioEngine::timerCallback()
{
    if (! impl->initialised)
        return;

    const auto now = juce::Time::currentTimeMillis();

    //---- hotplug / device loss --------------------------------------------
    auto* current = impl->manager.getCurrentAudioDevice();
    if (current == nullptr)
    {
        if (impl->deviceWasOpen)
        {
            impl->deviceWasOpen = false;
            const bool wasPlaying = impl->stateAtomic.load() == PlaybackState::playing
                                 || impl->stateAtomic.load() == PlaybackState::loading;
            if (wasPlaying)
            {
                impl->resumeWhenDeviceReturns = true;
                impl->stateAtomic.store (PlaybackState::paused);
            }
            impl->postErrorImmediate ("Output device lost. Waiting for Core Audio to return a device...");
            impl->notifyDevicesChanged();
        }

        if (now - impl->lastRecoveryAttemptMs > 2000)
        {
            impl->lastRecoveryAttemptMs = now;
            juce::String error;
            if (impl->devices.openOutputDevice ({}, impl->options.preferredSampleRate,
                                                impl->options.bufferFrames, error))
            {
                impl->deviceWasOpen = true;
                impl->devices.noteDeviceChange();
                impl->postInfo ("Output device restored: " + impl->devices.getCurrentDeviceName());
                impl->notifyDevicesChanged();

                const bool shouldResume = impl->resumeWhenDeviceReturns;
                impl->resumeWhenDeviceReturns = false;
                impl->clearError();

                if (shouldResume && ! impl->hasLoadedTrack() && impl->queueSize() > 0)
                {
                    // Playback was requested but never started (no device): start
                    // the queued track now that Core Audio is back.
                    int startIndex = 0;
                    {
                        const juce::ScopedLock sl (impl->configLock);
                        startIndex = juce::jlimit (0, (int) impl->queue.size() - 1, impl->queueIndex);
                    }
                    const auto track = impl->trackForIndex (startIndex);
                    if (track.path.isNotEmpty())
                        impl->startTrackAt (track, startIndex, 0, true);
                }
                else
                {
                    impl->reloadCurrentTrackAtPosition (shouldResume);
                }
            }
        }
    }
    else if (! impl->deviceWasOpen)
    {
        impl->deviceWasOpen = true;
        impl->postInfo ("Audio output: " + current->getName() + " @ "
                        + formatRate (current->getCurrentSampleRate()) + " Hz");
    }

    if (current != nullptr)
    {
        if (impl->options.hogModeEnabled && ! impl->devices.isHogModeActive())
        {
            const auto deviceName = impl->devices.getCurrentDeviceName();
            if (deviceName != impl->hogAttemptDevice || now - impl->hogAttemptMs > 5000)
            {
                impl->hogAttemptDevice = deviceName;
                impl->hogAttemptMs = now;
                juce::String error;
                if (! impl->devices.setHogMode (true, error))
                    impl->postError (error);
            }
        }
        impl->hogActiveAtomic.store (impl->devices.isHogModeActive());
        impl->nativeBitsAtomic.store (impl->devices.getCurrentNativeBitDepth());

        // Work out whether dithering is needed for the current source/device.
        DspChainStatus chainStatus = impl->chain.getStatus();
        juce::ignoreUnused (chainStatus);

        const int targetBits = impl->nativeBitsAtomic.load();
        int sourceBits = 0;
        bool sourceIsIntegerPcm = false;
        bool sourceIsDsd = false;
        {
            const juce::ScopedLock sl (impl->statusLock);
            if (impl->sourceSnapshot.valid)
            {
                sourceBits = impl->sourceSnapshot.bits;
                sourceIsIntegerPcm = impl->sourceSnapshot.integerPcm;
                sourceIsDsd = impl->sourceSnapshot.isDsd;
            }
        }

        const bool reference = impl->referenceActive.load();
        const bool ditherOption = impl->options.ditherEnabled && ! reference;
        // Dither only makes sense when truncating an integer PCM source to a
        // lower integer device depth. DSD (1-bit) and lossy sources have no
        // meaningful integer LSB to dither.
        const bool dither = ditherOption && sourceIsIntegerPcm && ! sourceIsDsd
                         && targetBits > 0 && sourceBits > targetBits;
        impl->ditherActiveAtomic.store (dither);
        impl->ditherTargetBitsAtomic.store (dither ? targetBits : 0);
        impl->ditherSourceBitsAtomic.store (sourceBits);
    }
    else
    {
        impl->hogActiveAtomic.store (false);
        impl->nativeBitsAtomic.store (0);
        impl->ditherActiveAtomic.store (false);
    }

    impl->handleAutoAdvance();
    impl->syncQueueIndexFromDecode();
    impl->publishStatus();
}

void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    juce::ignoreUnused (source);
    impl->devices.noteDeviceChange();
    impl->deviceChangePending = true;
}

void AudioEngine::handleAsyncUpdate()
{
    // Called on the message thread when the decode thread reached a natural end
    // (so gapless-off auto-advance does not have to wait for the 15 Hz timer).
    impl->handleAutoAdvance();
    impl->syncQueueIndexFromDecode();
    impl->publishStatus();
}

} // namespace sp
