/*
    Snake Puppy Reference Player
    Core Audio device discovery and configuration.

    The implementation talks to the HAL directly (AudioObjectGetPropertyData)
    for the facts JUCE does not expose (UID, integer native bit depth, hog
    mode, the complete supported sample-rate list) and uses
    juce::AudioDeviceManager for opening/configuring the actual I/O device.

    Threading: all methods run on the message thread (device configuration,
    GUI). getOutputDevices() also performs HAL queries and must not be called
    from the audio callback. DeviceManager::lastChangeMs() is atomic and can be
    polled from any thread.
*/
#include "Audio/DeviceManager.h"
#include "Core/Types.h"

#if JUCE_MAC || JUCE_IOS
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#endif

namespace sp
{

namespace
{
#if JUCE_MAC || JUCE_IOS
juce::String cfStringToJuce (CFStringRef cf)
{
    if (cf == nullptr)
        return {};

    const CFIndex length = CFStringGetLength (cf);
    const CFIndex maxBytes = CFStringGetMaximumSizeForEncoding (length, kCFStringEncodingUTF8) + 1;
    std::vector<char> buffer ((size_t) juce::jmax<CFIndex> (maxBytes, 1), 0);
    if (CFStringGetCString (cf, buffer.data(), (CFIndex) buffer.size(), kCFStringEncodingUTF8))
        return juce::String::fromUTF8 (buffer.data());
    return {};
}

bool getDeviceString (AudioDeviceID device, AudioObjectPropertySelector selector, juce::String& out)
{
    AudioObjectPropertyAddress address { selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef value = nullptr;
    UInt32 size = sizeof (value);

    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &value) != noErr || value == nullptr)
        return false;

    out = cfStringToJuce (value);
    CFRelease (value);
    return out.isNotEmpty();
}

bool getDeviceUInt32 (AudioDeviceID device, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope, UInt32& out)
{
    AudioObjectPropertyAddress address { selector, scope, kAudioObjectPropertyElementMain };
    UInt32 size = sizeof (out);
    return AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &out) == noErr;
}

bool getDeviceFloat64 (AudioDeviceID device, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope, Float64& out)
{
    AudioObjectPropertyAddress address { selector, scope, kAudioObjectPropertyElementMain };
    UInt32 size = sizeof (out);
    return AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &out) == noErr;
}

int getOutputChannelCount (AudioDeviceID device)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyStreamConfiguration,
                                         kAudioObjectPropertyScopeOutput,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (device, &address, 0, nullptr, &size) != noErr || size == 0)
        return 0;

    std::vector<unsigned char> storage (size, 0);
    auto* list = reinterpret_cast<AudioBufferList*> (storage.data());
    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, list) != noErr)
        return 0;

    int channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        channels += (int) list->mBuffers[i].mNumberChannels;
    return channels;
}

std::vector<double> getAvailableRates (AudioDeviceID device)
{
    std::vector<double> rates;

    AudioObjectPropertyAddress address { kAudioDevicePropertyAvailableNominalSampleRates,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (device, &address, 0, nullptr, &size) != noErr || size == 0)
        return rates;

    std::vector<AudioValueRange> ranges (size / sizeof (AudioValueRange));
    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, ranges.data()) != noErr)
        return rates;

    for (const auto& r : ranges)
    {
        if (! std::isfinite (r.mMinimum) || ! std::isfinite (r.mMaximum) || r.mMinimum <= 0.0)
            continue;

        if (std::abs (r.mMaximum - r.mMinimum) < 0.5)
            rates.push_back (r.mMinimum);
        else
        {
            // A range means the device supports a continuum; list the standard
            // audiophile rates inside it plus the endpoints.
            static const double common[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0,
                                             352800.0, 384000.0, 705600.0, 768000.0 };
            for (auto c : common)
                if (c >= r.mMinimum - 0.5 && c <= r.mMaximum + 0.5)
                    rates.push_back (c);
        }
    }

    std::sort (rates.begin(), rates.end());
    rates.erase (std::unique (rates.begin(), rates.end()), rates.end());
    return rates;
}

/** Returns the integer bit depth of the device's physical output format, or 0
    when the format is float/unknown. kAudioStreamPropertyPhysicalFormat reports
    what the DAC actually receives; kAudioDevicePropertyStreamFormat usually
    reports JUCE's float virtual format, so it is only a fallback. */
int getIntegerBitDepth (AudioDeviceID device)
{
    AudioObjectPropertyAddress streamsAddress { kAudioDevicePropertyStreams,
                                                kAudioObjectPropertyScopeOutput,
                                                kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (device, &streamsAddress, 0, nullptr, &size) != noErr || size == 0)
        return 0;

    std::vector<AudioStreamID> streams (size / sizeof (AudioStreamID));
    if (AudioObjectGetPropertyData (device, &streamsAddress, 0, nullptr, &size, streams.data()) != noErr)
        return 0;

    for (auto stream : streams)
    {
        AudioObjectPropertyAddress fmtAddress { kAudioStreamPropertyPhysicalFormat,
                                                kAudioObjectPropertyScopeGlobal,
                                                kAudioObjectPropertyElementMain };
        AudioStreamBasicDescription format {};
        UInt32 fmtSize = sizeof (format);
        if (AudioObjectGetPropertyData (stream, &fmtAddress, 0, nullptr, &fmtSize, &format) != noErr)
            continue;

        if (format.mFormatFlags & kAudioFormatFlagIsFloat)
            return 0;
        if (format.mBitsPerChannel > 0)
            return (int) format.mBitsPerChannel;
    }

    return 0;
}

AudioDeviceID findDeviceByName (const juce::String& name, juce::String& uidOut)
{
    uidOut.clear();

    AudioObjectPropertyAddress listAddress { kAudioHardwarePropertyDevices,
                                              kAudioObjectPropertyScopeGlobal,
                                              kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &listAddress, 0, nullptr, &size) != noErr)
        return kAudioObjectUnknown;

    std::vector<AudioDeviceID> devices (size / sizeof (AudioDeviceID));
    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &listAddress, 0, nullptr, &size, devices.data()) != noErr)
        return kAudioObjectUnknown;

    for (auto device : devices)
    {
        juce::String deviceName;
        if (! getDeviceString (device, kAudioObjectPropertyName, deviceName))
            continue;

        if (deviceName == name)
        {
            juce::String uid;
            getDeviceString (device, kAudioDevicePropertyDeviceUID, uid);
            uidOut = uid;
            return device;
        }
    }

    return kAudioObjectUnknown;
}

AudioDeviceID getDefaultOutputDevice()
{
    AudioDeviceID device = kAudioObjectUnknown;
    AudioObjectPropertyAddress address { kAudioHardwarePropertyDefaultOutputDevice,
                                          kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain };
    UInt32 size = sizeof (device);
    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &address, 0, nullptr, &size, &device) != noErr)
        return kAudioObjectUnknown;
    return device;
}

bool isDeviceHoggedByUs (AudioDeviceID device)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyHogMode,
                                          kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain };
    pid_t owner = -1;
    UInt32 size = sizeof (owner);
    if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &owner) != noErr)
        return false;
    return owner == getpid();
}
#endif
} // namespace

//==============================================================================
DeviceManager::DeviceManager (juce::AudioDeviceManager& m) : deviceManager (m) {}
DeviceManager::~DeviceManager() = default;

juce::AudioDeviceManager& DeviceManager::getManager() { return deviceManager; }

std::vector<OutputDeviceInfo> DeviceManager::getOutputDevices() const
{
    std::vector<OutputDeviceInfo> result;

#if JUCE_MAC || JUCE_IOS
    AudioObjectPropertyAddress listAddress { kAudioHardwarePropertyDevices,
                                              kAudioObjectPropertyScopeGlobal,
                                              kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &listAddress, 0, nullptr, &size) != noErr)
        return result;

    std::vector<AudioDeviceID> devices (size / sizeof (AudioDeviceID));
    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &listAddress, 0, nullptr, &size, devices.data()) != noErr)
        return result;

    const auto defaultDevice = getDefaultOutputDevice();

    for (auto device : devices)
    {
        const int channels = getOutputChannelCount (device);
        if (channels <= 0)
            continue;   // input-only

        OutputDeviceInfo info;
        info.name = "Unknown device";
        getDeviceString (device, kAudioObjectPropertyName, info.name);
        info.typeName = "Core Audio";
        info.numOutputChannels = channels;
        info.isDefault = (device == defaultDevice);

        juce::String uid;
        if (getDeviceString (device, kAudioDevicePropertyDeviceUID, uid))
        {
            info.coreAudioUid = uid;
            info.deviceId = uid;
        }
        else
        {
            info.deviceId = info.name;
        }

        info.availableSampleRates = getAvailableRates (device);

        Float64 currentRate = 0.0;
        if (getDeviceFloat64 (device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, currentRate))
        {
            info.currentSampleRate = currentRate;
            if (info.availableSampleRates.empty())
                info.availableSampleRates.push_back (currentRate);
        }

        UInt32 bufferFrames = 0;
        if (getDeviceUInt32 (device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, bufferFrames))
            info.currentBufferFrames = (int) bufferFrames;

        info.nativeBitDepth = getIntegerBitDepth (device);

        result.push_back (std::move (info));
    }
#endif

    return result;
}

std::vector<double> DeviceManager::getDeviceSampleRates (const juce::String& deviceName) const
{
    const auto all = getOutputDevices();
    for (const auto& device : all)
        if (device.name == deviceName)
            return device.availableSampleRates;

    return {};
}

//==============================================================================
bool DeviceManager::openOutputDevice (const juce::String& deviceName, double desiredRate,
                                      int bufferFrames, juce::String& errorOut)
{
    errorOut.clear();

    juce::AudioDeviceManager::AudioDeviceSetup setup = deviceManager.getAudioDeviceSetup();

    juce::String targetName = deviceName;
    if (targetName.isEmpty())
    {
        // Empty means "the system default output device".
        for (const auto& device : getOutputDevices())
        {
            if (device.isDefault)
            {
                targetName = device.name;
                break;
            }
        }

        if (targetName.isEmpty())
        {
            const auto all = getOutputDevices();
            if (! all.empty())
                targetName = all.front().name;
        }

        if (targetName.isEmpty())
        {
            errorOut = "No Core Audio output device is available";
            return false;
        }
    }

    if (targetName.isNotEmpty())
        setup.outputDeviceName = targetName;

    if (desiredRate > 0.0)
        setup.sampleRate = desiredRate;
    else if (desiredRate <= 0.0)
        setup.sampleRate = 0.0;

    if (bufferFrames > 0)
        setup.bufferSize = bufferFrames;

    setup.useDefaultOutputChannels = true;

    const auto error = deviceManager.setAudioDeviceSetup (setup, true);
    if (error.isNotEmpty())
    {
        errorOut = "Could not open output device '" + (deviceName.isNotEmpty() ? deviceName : "default")
                 + "': " + error;
        return false;
    }

    if (deviceManager.getCurrentAudioDevice() == nullptr)
    {
        errorOut = "No output device became available after opening '" + deviceName + "'";
        return false;
    }

    if (desiredRate > 0.0)
    {
        const double actual = deviceManager.getCurrentAudioDevice()->getCurrentSampleRate();
        if (std::abs (actual - desiredRate) > 1.0)
            errorOut = "Device opened at " + juce::String (actual) + " Hz instead of the requested "
                     + juce::String (desiredRate) + " Hz";
    }

    return errorOut.isEmpty();
}

bool DeviceManager::setSampleRate (double rate, juce::String& errorOut)
{
    errorOut.clear();

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        errorOut = "No output device is open";
        return false;
    }

    if (std::abs (device->getCurrentSampleRate() - rate) < 0.5)
        return true;

    auto setup = deviceManager.getAudioDeviceSetup();
    setup.sampleRate = rate;
    const auto error = deviceManager.setAudioDeviceSetup (setup, false);
    if (error.isNotEmpty())
    {
        errorOut = "Could not switch " + device->getName() + " to " + juce::String (rate) + " Hz: " + error;
        return false;
    }

    auto* reopened = deviceManager.getCurrentAudioDevice();
    if (reopened == nullptr)
    {
        errorOut = "Device disappeared while switching to " + juce::String (rate) + " Hz";
        return false;
    }

    const double actual = reopened->getCurrentSampleRate();
    if (std::abs (actual - rate) > 1.0)
    {
        errorOut = "Device refused " + juce::String (rate) + " Hz (now at " + juce::String (actual) + " Hz)";
        return false;
    }

    return true;
}

bool DeviceManager::setBufferSize (int frames, juce::String& errorOut)
{
    errorOut.clear();

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        errorOut = "No output device is open";
        return false;
    }

    if (frames <= 0)
    {
        errorOut = "Invalid buffer size " + juce::String (frames);
        return false;
    }

    auto setup = deviceManager.getAudioDeviceSetup();
    setup.bufferSize = frames;
    const auto error = deviceManager.setAudioDeviceSetup (setup, false);
    if (error.isNotEmpty())
    {
        errorOut = "Could not set " + device->getName() + " buffer to " + juce::String (frames)
                 + " frames: " + error;
        return false;
    }

    return true;
}

//==============================================================================
double DeviceManager::getCurrentSampleRate() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentSampleRate();
    return 0.0;
}

int DeviceManager::getCurrentBufferSize() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentBufferSizeSamples();
    return 0;
}

juce::String DeviceManager::getCurrentDeviceName() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getName();
    return {};
}

juce::String DeviceManager::getCurrentDeviceType() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getTypeName();
    return {};
}

int DeviceManager::getCurrentNativeBitDepth() const
{
#if JUCE_MAC || JUCE_IOS
    const auto name = getCurrentDeviceName();
    if (name.isEmpty())
        return 0;

    juce::String uid;
    const auto device = findDeviceByName (name, uid);
    if (device == kAudioObjectUnknown)
        return 0;

    return getIntegerBitDepth (device);
#else
    return 0;
#endif
}

//==============================================================================
bool DeviceManager::setHogMode (bool enabled, juce::String& errorOut)
{
    errorOut.clear();

#if JUCE_MAC || JUCE_IOS
    juce::String uid;
    const auto device = findDeviceByName (getCurrentDeviceName(), uid);
    if (device == kAudioObjectUnknown)
    {
        errorOut = "No Core Audio output device is open for exclusive access";
        return false;
    }

    AudioObjectPropertyAddress address { kAudioDevicePropertyHogMode,
                                          kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain };

    if (! AudioObjectHasProperty (device, &address))
    {
        errorOut = "This output device does not support exclusive (hog) mode";
        return false;
    }

    Boolean settable = false;
    if (AudioObjectIsPropertySettable (device, &address, &settable) != noErr || ! settable)
    {
        errorOut = "Exclusive (hog) mode is not settable on this output device";
        return false;
    }

    pid_t owner = -1;
    {
        UInt32 size = sizeof (owner);
        if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &owner) != noErr)
            owner = -1;
    }

    if (! enabled)
    {
        if (owner == getpid())
        {
            pid_t none = -1;
            if (AudioObjectSetPropertyData (device, &address, 0, nullptr, sizeof (none), &none) != noErr)
            {
                errorOut = "Could not release exclusive access on " + getCurrentDeviceName();
                return false;
            }
        }
        return true;
    }

    if (owner == getpid())
        return true;

    if (owner > 0)
    {
        errorOut = "Exclusive access is already held by another process (pid " + juce::String ((int) owner) + ")";
        return false;
    }

    pid_t pid = getpid();
    const OSStatus status = AudioObjectSetPropertyData (device, &address, 0, nullptr, sizeof (pid), &pid);
    if (status != noErr)
    {
        errorOut = "Could not take exclusive access on " + getCurrentDeviceName()
                 + " (Core Audio error " + juce::String ((int) status) + ")";
        return false;
    }

    if (! isDeviceHoggedByUs (device))
    {
        errorOut = "Exclusive access request was accepted but the device reports another owner";
        return false;
    }

    return true;
#else
    if (enabled)
    {
        errorOut = "Exclusive (hog) mode is only implemented on macOS Core Audio";
        return false;
    }
    return true;
#endif
}

bool DeviceManager::isHogModeActive() const
{
#if JUCE_MAC || JUCE_IOS
    juce::String uid;
    const auto device = findDeviceByName (getCurrentDeviceName(), uid);
    if (device == kAudioObjectUnknown)
        return false;
    return isDeviceHoggedByUs (device);
#else
    return false;
#endif
}

bool DeviceManager::isHogModeSupported() const
{
#if JUCE_MAC || JUCE_IOS
    juce::String uid;
    const auto device = findDeviceByName (getCurrentDeviceName(), uid);
    if (device == kAudioObjectUnknown)
        return false;

    AudioObjectPropertyAddress address { kAudioDevicePropertyHogMode,
                                          kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain };
    if (! AudioObjectHasProperty (device, &address))
        return false;

    Boolean settable = false;
    if (AudioObjectIsPropertySettable (device, &address, &settable) != noErr)
        return false;
    return settable;
#else
    return false;
#endif
}

void DeviceManager::noteDeviceChange() { lastChangeTime = juce::Time::currentTimeMillis(); }

} // namespace sp
