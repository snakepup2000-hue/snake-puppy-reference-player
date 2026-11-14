/*
    Snake Puppy Reference Player - persistent transport bar.

    Playback transport, seek with real times, volume in dB with mute, output
    device selection, honest signal-path badge, Reference Mode, DSP Studio /
    Settings entry points and a compact level meter.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/Widgets.h"

namespace sp
{

class TransportBar : public juce::Component,
                     private juce::Timer
{
public:
    explicit TransportBar (AppContext&);
    ~TransportBar() override;

    void setRightPanelVisible (bool visible);
    void refreshDeviceList();

    /** Called when the user toggles the DSP rack rail button. */
    std::function<void()> onToggleRightPanel;
    /** Return true when the request was handled (e.g. start the selected track);
        otherwise the transport falls back to engine play/pause. */
    std::function<bool()> onPlayRequested;
    /** Called when the user wants the full signal-path diagnostics. */
    std::function<void()> onShowSignalPath;
    /** Called when the user opens the DSP Studio window. */
    std::function<void()> onOpenDspStudio;
    /** Called when the user opens the Settings window. */
    std::function<void()> onOpenSettings;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    class SignalBadge;
    class FallbackMeters;

    void timerCallback() override;
    void updateControls();
    void rebuildDeviceCombo();

    AppContext& ctx;

    SpIconButton previousButton { Icon::previous, "Previous track" };
    SpIconButton playPauseButton { Icon::play, "Play / pause (Space)" };
    SpIconButton stopButton { Icon::stop, "Stop" };
    SpIconButton nextButton { Icon::next, "Next track" };
    SpIconButton muteButton { Icon::volume, "Mute" };
    SpIconButton rackButton { Icon::rack, "Show / hide the DSP rack" };
    SpIconButton settingsButton { Icon::settings, "Settings" };

    SpFader seekSlider;
    SpFader volumeSlider;
    SpLabel positionLabel, totalLabel, volumeLabel;
    SpToggleButton referenceToggle { "Reference" };
    SpButton dspStudioButton { "DSP Studio", SpButton::Style::ghost };
    SpComboBox deviceCombo;

    std::unique_ptr<SignalBadge> badge;
    std::unique_ptr<FallbackMeters> fallbackMeters;
    std::unique_ptr<juce::Component> metersComponent;

    juce::StringArray cachedDeviceNames, cachedDeviceLabels;
    juce::String currentDeviceName;
    int cachedOutputRate = 0;
    bool rightPanelVisible = true;

    double durationMs = 0.0;
    bool scrubbing = false;
    bool draggingVolume = false;
    bool updatingControls = false;
    int deviceRefreshTicks = 0;

    juce::Rectangle<int> badgeArea;
    juce::Rectangle<int> metersArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBar)
};

} // namespace sp
