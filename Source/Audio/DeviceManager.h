/*
    Snake Puppy Reference Player
    Core Audio device discovery and configuration. Wraps juce::AudioDeviceManager
    and adds the macOS-specific facts an audiophile player needs: native integer
    bit depth, supported sample rates, Core Audio UID and exclusive (hog) mode.

    Runs on the message thread. Device changes are reported through the JUCE
    ChangeListener notification of the underlying AudioDeviceManager.
*/
#pragma once

#include "Core/Types.h"
#include <juce_audio_devices/juce_audio_devices.h>

namespace sp
{

class DeviceManager
{
public:
    explicit DeviceManager (juce::AudioDeviceManager& manager);
    ~DeviceManager();

    /** Snapshot of all output devices with their capabilities. */
    std::vector<OutputDeviceInfo> getOutputDevices() const;

    /** Opens an output device by name (empty = JUCE default). desiredRate 0
        means keep the current/native rate. */
    bool openOutputDevice (const juce::String& deviceName, double desiredRate,
                           int bufferFrames, juce::String& errorOut);

    bool setSampleRate (double rate, juce::String& errorOut);
    bool setBufferSize (int frames, juce::String& errorOut);

    double getCurrentSampleRate() const;
    int getCurrentBufferSize() const;
    juce::String getCurrentDeviceName() const;
    juce::String getCurrentDeviceType() const;
    int getCurrentNativeBitDepth() const;

    /** Enumerate the sample rates Core Audio reports for a device. */
    std::vector<double> getDeviceSampleRates (const juce::String& deviceName) const;

    /** Exclusive access (kAudioDevicePropertyHogMode). Prevents the OS from
        mixing other audio into the same device. Returns false with a reason if
        unsupported or already hogged by another process. */
    bool setHogMode (bool enabled, juce::String& errorOut);
    bool isHogModeActive() const;
    bool isHogModeSupported() const;

    juce::AudioDeviceManager& getManager();

    /** Time of the last observed device-list change (ms since epoch). */
    juce::int64 lastChangeMs() const { return lastChangeTime.load(); }
    void noteDeviceChange();

private:
    juce::AudioDeviceManager& deviceManager;
    std::atomic<juce::int64> lastChangeTime { 0 };
};

} // namespace sp
