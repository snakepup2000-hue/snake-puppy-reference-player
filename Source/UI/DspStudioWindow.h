/*
    Snake Puppy Reference Player
    DSP Studio - the large processing workspace.

    Tabs: Graphic EQ, Parametric EQ, Bass, Chain, Stereo / Crossfeed, Filters /
    Convolution, ReplayGain / Preamp / Limiter, A/B presets, Meters (meters,
    spectrum, spectrogram), Signal Path, Device Profiles and Profiles.

    The toolbar saves and loads user profiles through ProfileEngine and JSON
    presets through ApplicationPaths::presetsDir (Core/Serialization.h), and
    exposes Reference Mode. Window bounds persist through AppSettings.

    Open with DspStudioWindow::show(ctx); the application shell calls
    destroyInstance() on shutdown before the AppContext is destroyed.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/DspUiStyle.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace sp
{

class DspStudioWindow : public juce::DocumentWindow,
                        private juce::Timer
{
public:
    explicit DspStudioWindow (AppContext&);
    ~DspStudioWindow() override;

    /** Creates the studio if needed, then shows and raises it. Message thread. */
    static DspStudioWindow* show (AppContext&);

    /** Deletes the singleton. Call before the AppContext is destroyed. */
    static void destroyInstance();

    /** Selects a workspace tab (used by the visual verification rigs). */
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DspStudioWindow)
};

} // namespace sp
