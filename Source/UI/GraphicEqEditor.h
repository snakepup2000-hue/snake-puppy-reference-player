/*
    Snake Puppy Reference Player
    15 / 31 band graphic equaliser editor.

    Per-band machined faders (bipolar, gold for boost, blue for cut), live
    numeric readout, +/-6 / +/-12 dB range, flat and EQ-bypass controls. Every band
    change goes through AppContext::setGraphicEqBandGain, so the DSP chain, the
    curve and the persisted manual parameters stay in sync.

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

class GraphicEqEditor : public juce::Component,
                        private juce::Timer
{
public:
    explicit GraphicEqEditor (AppContext&);
    ~GraphicEqEditor() override;

    /** Re-read parameters and repaint immediately (message thread). */
    void refreshNow();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class BandFader;

    void timerCallback() override;
    void rebuildFaders();
    void layoutFaders (juce::Rectangle<int> area);
    void syncFromContext (bool force);
    void updateHeaderState();
    void applyGraphicParams (const GraphicEqParams&, bool rebuildIfCountChanged);
    DspParams currentParams() const { return ctx.getCurrentDspParams(); }

    AppContext& ctx;
    EqCurveComponent curve;
    SectionHeader header { "GRAPHIC EQUALISER" };
    SpComboBox bandSetBox { "Bands" };
    SpComboBox rangeBox { "Range" };
    SpButton flatButton { "FLAT" };
    SpToggleButton bypassButton { "EQ BYPASS" };
    SpLabel statusLabel;

    std::vector<std::unique_ptr<BandFader>> faders;
    juce::Rectangle<int> faderArea;
    int builtBandSet = -1;
    float builtRange = -1.0f;
    bool updating = false;
    juce::String lastStatus;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GraphicEqEditor)
};

} // namespace sp
