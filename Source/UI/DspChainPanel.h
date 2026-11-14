/*
    Snake Puppy Reference Player
    DSP processing chain panel.

    Lists DspSlot in the current DspParams::chainOrder, shows whether each
    processor is enabled / currently active, allows enable/bypass per slot,
    reordering (move up / down) and a global bypass. Slots whose state is a
    continuous parameter (stereo width, balance) report an active LED and link
    to the relevant editor rather than pretending to have an on/off switch.

    Message thread only.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/DspUiStyle.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include <vector>

namespace sp
{

class DspChainPanel : public juce::Component,
                      private juce::Timer
{
public:
    explicit DspChainPanel (AppContext&);
    ~DspChainPanel() override;

    void refreshNow();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class SlotRow;

    void timerCallback() override;
    void rebuildRows();
    void refreshRows();
    void moveSlot (int rowIndex, int delta);
    void applyParams (const DspParams&);
    bool slotEnabled (DspSlot) const;
    bool slotCanToggle (DspSlot) const;
    bool slotActive (DspSlot) const;
    void setSlotEnabled (DspSlot, bool);

    AppContext& ctx;
    SpToggleButton masterBypass { "GLOBAL BYPASS" };
    SpToggleButton eqBypass { "EQ BYPASS" };
    SpButton defaultOrderButton { "DEFAULT ORDER" };
    juce::Label hintLabel;
    juce::Viewport viewport;
    juce::Component rowHolder;
    std::vector<std::unique_ptr<SlotRow>> rows;
    std::array<DspSlot, static_cast<size_t> (DspSlot::numSlots)> shownOrder {};
    bool updating = false;
    juce::Rectangle<int> headerArea, toolbarArea, listArea, footerArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DspChainPanel)
};

} // namespace sp
