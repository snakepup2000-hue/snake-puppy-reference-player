/*
    Snake Puppy Reference Player
    Playback engine.

    Architecture:
      - The message thread drives transport operations.
      - A dedicated decode thread opens DecodeSource objects, decodes at the
        file's native rate, resamples to the device rate when needed, applies
        gapless queue transitions, and writes stereo blocks into a large
        lock-free ring buffer.
      - The audio callback reads the ring buffer, applies the 64-bit DSP chain
        (DspChain), applies smoothed master volume, dither, converts to float
        and writes to Core Audio. It allocates nothing, locks nothing, does no
        disk or network I/O.
      - Metering samples are pushed into AnalyzerHub from the callback.
      - A 15 Hz timer publishes EngineStatus snapshots and notifies listeners.

    Bit-perfect logic: a track is reported bit-perfect only when the DSP chain is
    fully bypassed, master volume is exactly unity, ReplayGain is off, no
    resampling is happening, the source is integer PCM and the output device is
    in exclusive integer mode at the same rate. Otherwise the signal path is
    reported as bit-transparent (float pipeline, no processing) or processed.
*/
#pragma once

#include "Audio/DecodeSource.h"
#include "Audio/DeviceManager.h"
#include "Core/Types.h"
#include "DSP/AnalyzerHub.h"
#include "DSP/DspChain.h"
#include <juce_audio_devices/juce_audio_devices.h>

namespace sp
{

struct EngineOptions
{
    bool gaplessEnabled = true;
    bool nativeSampleRateSwitching = true;   // follow each file's rate when the device allows
    bool memoryPlayback = false;             // decode whole track to RAM if it fits
    bool ditherEnabled = true;               // TPDF when reducing effective bit depth
    bool hogModeEnabled = false;             // exclusive device access
    int bufferFrames = 512;
    double preferredSampleRate = 0.0;        // 0 = device rate / auto
    int resamplerQuality = 1;                // 0 fast, 1 high, 2 ultra
};

struct EngineStatus
{
    PlaybackState state = PlaybackState::stopped;
    Track currentTrack;
    int currentQueueIndex = -1;
    int queueSize = 0;

    std::int64_t positionMs = 0;
    std::int64_t durationMs = 0;
    std::int64_t bufferedMs = 0;

    float volumeDb = 0.0f;
    bool muted = false;

    bool referenceMode = false;
    bool gaplessEnabled = true;
    bool memoryPlaybackActive = false;
    double cpuPercent = 0.0;
    int outputLatencySamples = 0;
    int64_t underrunCount = 0;

    bool deviceOpen = false;
    juce::String errorMessage;
    juce::String infoMessage;

    SignalPathInfo signalPath;
    DspChainStatus dspStatus;
    MeterValues meters;

    bool endOfQueue = false;
    bool decodeThreadActive = false;
};

struct PersistedPlaybackState
{
    std::vector<Track> queue;
    int currentQueueIndex = -1;
    std::int64_t positionMs = 0;
    float volumeDb = -6.0f;
    bool muted = false;
    bool referenceMode = false;
};

class AudioEngine : private juce::AudioIODeviceCallback,
                    private juce::Timer,
                    private juce::ChangeListener,
                    private juce::AsyncUpdater
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void engineTrackChanged (const Track&) {}
        virtual void engineQueueChanged() {}
        virtual void engineDevicesChanged() {}
        virtual void engineError (const juce::String&) {}
        virtual void engineReferenceModeChanged (bool) {}
    };

    AudioEngine();
    ~AudioEngine() override;

    /** Opens the default output device and starts the engine. Returns empty on
        success or a human-readable error. */
    juce::String initialise();
    void shutdown();

    //== Options ==============================================================
    EngineOptions getOptions() const;
    /** Applies options live; may reopen the device and reset the decode thread. */
    void setOptions (const EngineOptions&, bool reopenDevice = true);
    void setGaplessEnabled (bool);
    void setNativeSampleRateSwitching (bool);
    void setMemoryPlayback (bool);
    void setDitherEnabled (bool);
    void setHogModeEnabled (bool);
    void setResamplerQuality (int quality0to2);

    //== Queue / transport ====================================================
    void setQueue (std::vector<Track> tracks, int startIndex, bool autoPlay);
    void enqueueNext (const Track& track);
    void enqueueAfterCurrent (const std::vector<Track>& tracks);
    void clearQueue();
    std::vector<Track> getQueue() const;
    int getCurrentQueueIndex() const;

    void playTrack (const Track& track, bool autoPlay = true);   // replaces queue with one track
    void playQueueIndex (int index, bool autoPlay);
    void play();
    void pause();
    void togglePlayPause();
    void stop();
    void next();
    void previous();
    void seekMs (std::int64_t positionMs);
    void setVolumeDb (float db);            // -144 .. +12, unity at 0
    void setMuted (bool);
    float getVolumeDb() const;
    bool isMuted() const;
    void setRateReplayGainForCurrentTrack (float gainDb, float peakLinear, bool hasGain);

    //== DSP ==================================================================
    DspChain& dspChain();
    AnalyzerHub& analyzers();
    /** Applies params to the live chain with smoothing (used by UI/profile engine). */
    void setDspParams (const DspParams&, int rampMs = 80);
    DspParams getDspParams() const;
    /** Reference Mode: bypasses the whole chain, disables ReplayGain, sets
        volume to unity (restoring the previous value on exit) and asks the
        device for the file's native rate. */
    void setReferenceMode (bool active);
    bool isReferenceMode() const;

    //== Output device ========================================================
    DeviceManager& devices();
    juce::AudioDeviceManager& getDeviceManager();
    void rescanDevices();
    bool selectOutputDevice (const juce::String& name, double preferredRate, juce::String& errorOut);

    //== Status / listeners ===================================================
    EngineStatus getStatus() const;
    PersistedPlaybackState capturePersistedState() const;
    /** Restores a queue without playing; the UI decides when to resume. */
    void restorePersistedState (const PersistedPlaybackState&);

    void addListener (Listener*);
    void removeListener (Listener*);

    //== Offline helper for tests / analysis ==================================
    /** Decodes a whole file (limit seconds, 0 = all) to non-interleaved double
        channels at the file's native rate. Applies the decoder's DSD gain if
        any. Never touches the output device. */
    static bool decodeFileToBuffer (const juce::File& file,
                                    juce::AudioBuffer<double>& out,
                                    int maxSeconds,
                                    juce::String& errorOut);

    //== Debug verification ===================================================
    /** Records the final post-volume stereo output (device rate, 24-bit WAV) to
        a file. The audio callback only pushes into a lock-free FIFO; a separate
        writer thread performs the disk I/O, so this never violates real-time
        safety. Message thread only. */
    bool startDebugRecording (const juce::File& wavFile, juce::String& errorOut);
    void stopDebugRecording();
    bool isDebugRecording() const;

    /** The effective device sample rate the engine is currently rendering at
        (0 when no device is open). */
    double getOutputSampleRate() const;

private:
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& errorMessage) override;
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void handleAsyncUpdate() override;

    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace sp
