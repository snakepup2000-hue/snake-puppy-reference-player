/*
    Snake Puppy Reference Player
    Direct PiBridge output: renders the current track through the local 64-bit
    DSP chain and streams it as raw S32_LE PCM over SSH to the user's PiBridge
    receiver (`hqdac`) on a Raspberry Pi, which writes it to the USB DAC.

    Why a separate path: the DLNA path sends the original file to a renderer,
    so local DSP cannot apply. This path sends processed PCM, so EQ, bass,
    limiter and volume are applied before the DAC. It is intentionally a
    one-track stream (the receiver has no transport protocol): STOP ends it.
*/
#pragma once

#include "Core/DspParams.h"
#include "Core/Types.h"
#include <juce_events/juce_events.h>

namespace sp
{

struct PiBridgeConfig
{
    /** ssh target, resolved through ~/.ssh/config (default matches the
        user's PiBridge setup: an SSH host alias plus the receiver user). */
    juce::String sshTarget = "pi";
    juce::String receiver = "~/hqdac";
    juce::String alsaDevice = "hw:CARD=R2R,DEV=0";
    int bufferUs = 20000;
    int periodUs = 2500;
    int targetUs = 10000;
    /** Test/diagnostic escape hatch: when non-empty this shell command replaces
        the ssh command and receives PCM on stdin. */
    juce::String commandOverride;
};

class PiBridgeOutput
{
public:
    PiBridgeOutput();
    ~PiBridgeOutput();

    enum class State { idle, preparing, streaming, finished, failed };

    /** Renders `file` through the DSP chain and streams it to the receiver.
        Blocks only to start the decoder; the transfer runs on a worker thread. */
    bool start (const juce::File& file, const DspParams& params, float volumeDb,
                bool muted, const PiBridgeConfig& config, juce::String& errorOut,
                std::int64_t startMs = 0);

    /** Live master volume/mute for the running stream (safe from any thread). */
    void setVolume (float volumeDb, bool muted);

    /** Live DSP parameter changes for the running stream. */
    void setDspParams (const DspParams& params, int rampMs = 60);

    /** Post-DSP output taps for the app's meters/spectrum (worker thread). */
    using MonitorCallback = std::function<void (const float* left, const float* right, int frames)>;
    void setMonitorCallback (MonitorCallback callback);

    /** Stops the worker, closes the pipe and terminates the ssh child. */
    void stop();

    bool isRunning() const;
    State getState() const;
    juce::String getStatusText() const;
    juce::String getLastError() const;
    int getOutputSampleRate() const;
    std::int64_t getFramesSent() const;
    /** Current playback position including the seek offset. */
    std::int64_t getPositionMs() const;
    std::int64_t getFramesTotal() const;

    /** ssh command line (exposed for tests). */
    static juce::String buildCommand (const PiBridgeConfig&, int sampleRate);

    /** DAC rate selection, matching PiBridge: use the K11 maximum that is an
        exact integer multiple of the source rate (384000 for the 48 kHz family,
        352800 for the 44.1 kHz family), otherwise the clock family's maximum. */
    static int chooseReceiverRate (int sourceSampleRate);

    struct Impl;
    std::unique_ptr<Impl> impl;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PiBridgeOutput)
};

} // namespace sp
