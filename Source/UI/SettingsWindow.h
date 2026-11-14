/*
    Snake Puppy Reference Player
    Settings window.

    Tabs: Audio (device, rate, buffer, hog mode, native-rate switching, dither,
    resampler quality, gapless, memory playback), Library (locations, scan),
    Shortcuts (edit/clear/reset), Genre Profiles, Network (NetworkPanel),
    Diagnostics (copyable text) and About (build info + honest limitations).

    Open with SettingsWindow::show(ctx); the shell calls destroyInstance() on
    shutdown before the AppContext is destroyed.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/DspUiStyle.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace sp
{

class SettingsWindow : public juce::DocumentWindow,
                       private juce::Timer
{
public:
    explicit SettingsWindow (AppContext&);
    ~SettingsWindow() override;

    static SettingsWindow* show (AppContext&);
    static void destroyInstance();

    /** Selects a settings tab (used by the visual verification rigs). */
    void setTab (int index);

    void closeButtonPressed() override;

private:
    class Content;

    void timerCallback() override;
    void saveBounds();

    AppContext& ctx;
    Content* content = nullptr;
    juce::TooltipWindow tooltips { this, 700 };
    juce::Rectangle<int> lastSavedBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsWindow)
};

} // namespace sp
