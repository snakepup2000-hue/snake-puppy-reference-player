/*
    Snake Puppy Reference Player
    12-band parametric equaliser editor.

    Scrollable band list: per-band enable and bypass, filter type
    (Bell / Low Shelf / High Shelf / High Pass / Low Pass / Notch / Band Pass /
    All Pass), frequency / gain / Q rotary controls with drag + double-click
    reset, per-band reset, add-band and section/global bypass. Every change is
    applied live through AppContext::applyUserDspParams.

    Message thread only.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/DspUiStyle.h"
#include "UI/EqCurveComponent.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <vector>

namespace sp
{

class ParametricEqEditor : public juce::Component,
                           private juce::Timer
{
public:
    explicit ParametricEqEditor (AppContext&);
    ~ParametricEqEditor() override;

    void refreshNow();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class BandRow;

    void timerCallback() override;
    void applyBand (int index, const EqBand&);
    void applySectionEnabled (bool enabled);
    void applyEqBypass (bool bypassed);
    void syncFromContext (bool force);
    void updateStatus();
    int firstDisabledBand() const;

    AppContext& ctx;
    EqCurveComponent curve;
    SectionHeader header { "PARAMETRIC EQUALISER" };
    SpLabel statusLabel;
    SpButton addBandButton { "ADD BAND" };
    SpButton resetAllButton { "RESET ALL" };
    SpToggleButton sectionToggle { "PARAMETRIC" };
    SpToggleButton eqBypassToggle { "EQ BYPASS" };

    juce::Viewport viewport;
    juce::Component rowHolder;
    std::vector<std::unique_ptr<BandRow>> rows;
    bool updating = false;
    juce::String lastStatus;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParametricEqEditor)
};

} // namespace sp
