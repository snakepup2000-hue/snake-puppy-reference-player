/*
    Snake Puppy Reference Player
    Core Audio device enumeration tests: real devices, sane sample rates,
    integer native bit depth reporting, buffer/rate configuration and hog mode.
*/
#include "TestFramework.h"
#include "Test_AudioHelpers.h"
#include "Audio/DeviceManager.h"
#include <juce_audio_devices/juce_audio_devices.h>

using namespace sp;

SP_TEST (AudioDevices, EnumeratesRealCoreAudioOutputDevices)
{
    juce::AudioDeviceManager manager;
    sp::DeviceManager devices (manager);

    const auto list = devices.getOutputDevices();
    std::cout << "        " << list.size() << " Core Audio output device(s):\n";

    int withIntegerDepth = 0;
    for (const auto& d : list)
    {
        std::cout << "          - " << d.name << " [" << d.typeName << "] "
                  << d.numOutputChannels << " ch, native " << d.nativeBitDepth << "-bit, "
                  << "current " << juce::String (d.currentSampleRate) << " Hz / "
                  << d.currentBufferFrames << " frames, UID=" << d.coreAudioUid
                  << (d.isDefault ? " (default)" : "") << "\n";

        SP_REQUIRE_MSG (d.name.isNotEmpty(), "device name must not be empty");
        SP_REQUIRE_MSG (d.numOutputChannels > 0, "output device must have output channels");
        SP_REQUIRE (! d.availableSampleRates.empty() || d.currentSampleRate > 0.0);
        SP_REQUIRE_MSG (d.nativeBitDepth >= 0 && d.nativeBitDepth <= 64,
                        "implausible native bit depth " + juce::String (d.nativeBitDepth));

        for (auto rate : d.availableSampleRates)
        {
            SP_REQUIRE_MSG (rate >= 8000.0 && rate <= 768000.0,
                            "implausible sample rate " + juce::String (rate));
        }

        if (d.nativeBitDepth > 0)
            ++withIntegerDepth;
    }

    SP_REQUIRE_MSG (! list.empty(), "at least one real Core Audio output device must be found");
    std::cout << "        " << withIntegerDepth << " device(s) reported an integer native bit depth\n";
}

SP_TEST (AudioDevices, OpensDefaultDeviceAndReportsConfiguration)
{
    juce::AudioDeviceManager manager;
    const auto error = manager.initialiseWithDefaultDevices (0, 2);
    SP_REQUIRE_MSG (error.isEmpty(), error);

    sp::DeviceManager devices (manager);
    auto* device = manager.getCurrentAudioDevice();
    SP_REQUIRE_MSG (device != nullptr, "no default output device");

    std::cout << "        opened: " << devices.getCurrentDeviceName() << " ["
              << devices.getCurrentDeviceType() << "] "
              << juce::String (devices.getCurrentSampleRate()) << " Hz, "
              << devices.getCurrentBufferSize() << " frames, native "
              << devices.getCurrentNativeBitDepth() << "-bit\n";

    SP_REQUIRE (devices.getCurrentSampleRate() > 0.0);
    SP_REQUIRE (devices.getCurrentBufferSize() > 0);
    SP_REQUIRE_MSG (devices.getDeviceSampleRates (devices.getCurrentDeviceName()).size() > 0,
                    "current device must report sample rates");
    SP_REQUIRE (devices.getCurrentNativeBitDepth() >= 0);

    // Re-open by name through the wrapper (this is the path the settings UI uses).
    juce::String openError;
    const auto name = devices.getCurrentDeviceName();
    SP_REQUIRE_MSG (devices.openOutputDevice (name, 0.0, 512, openError), openError);
    SP_REQUIRE_MSG (devices.getCurrentDeviceName() == name, "device name changed unexpectedly");
    SP_REQUIRE (devices.getCurrentSampleRate() > 0.0);

    // Buffer size change.
    if (devices.getCurrentBufferSize() != 256)
    {
        juce::String bufferError;
        if (devices.setBufferSize (256, bufferError))
            SP_REQUIRE_MSG (devices.getCurrentBufferSize() == 256,
                            "buffer size " + juce::String (devices.getCurrentBufferSize()));
        else
            std::cout << "        buffer size change not supported: " << bufferError << "\n";
    }

    std::cout << "        hog mode supported=" << (devices.isHogModeSupported() ? "yes" : "no")
              << ", active=" << (devices.isHogModeActive() ? "yes" : "no") << "\n";

    if (devices.isHogModeSupported())
    {
        juce::String hogError;
        if (devices.setHogMode (true, hogError))
        {
            SP_REQUIRE_MSG (devices.isHogModeActive(), "hog mode was not actually taken");
            juce::String releaseError;
            SP_REQUIRE_MSG (devices.setHogMode (false, releaseError), releaseError);
            SP_REQUIRE_MSG (! devices.isHogModeActive(), "hog mode was not released");
            std::cout << "        hog mode take/release verified\n";
        }
        else
        {
            std::cout << "        hog mode unavailable (expected if another process owns the device): "
                      << hogError << "\n";
        }
    }

    devices.noteDeviceChange();
    SP_REQUIRE (devices.lastChangeMs() > 0);

    manager.closeAudioDevice();
}
