/*
    Snake Puppy Reference Player
    Dedicated bass management panel.

    The prominent gold BASS knob (low shelf gain, +/-6 dB with optional +/-12 dB
    extended range) plus shelf frequency / Q / slope and three independent
    peaking sections - Sub, Mid-Bass and Punch - each with enable, gain,
    frequency and Q, and a live band indicator drawn from the actual parameters.
    Every change is applied instantly through AppContext::applyUserDspParams and
    only the BassParams fields are modified.

    Message thread only.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/DspUiStyle.h"
#include "UI/EqCurveComponent.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace sp
{

class BassControlPanel : public juce::Component,
                         private juce::Timer
{
public:
    explicit BassControlPanel (AppContext&);
    ~BassControlPanel() override;

    void refreshNow();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Section
    {
        Section (const char* titleText, const char* subtitleText, juce::Colour sectionColour)
            : title (titleText), subtitle (subtitleText), colour (sectionColour) {}

        const char* title = "";
        const char* subtitle = "";
        juce::Colour colour { theme::gold };
        juce::ToggleButton enable;
        SpKnob gain;
        SpKnob freq;
        SpKnob q;
        juce::Rectangle<int> card;
        juce::Rectangle<int> indicator;
    };

    void timerCallback() override;
    void syncFromContext (bool force);
    void updateEnabledStates();
    void updateReadout();
    void applyBass (const std::function<void (BassParams&)>& mutate);
    void paintCard (juce::Graphics&, juce::Rectangle<int> card, const juce::String& title,
                    const juce::String& right = {});
    void paintBandIndicator (juce::Graphics&, const Section&);
    void paintHeadroom (juce::Graphics&, juce::Rectangle<int> area);

    void configureKnob (SpKnob& knob, double minValue, double maxValue, double midpoint,
                        double doubleClickValue, std::function<juce::String (double)> formatter,
                        std::function<void (double)> onChange);

    AppContext& ctx;
    EqCurveComponent curve;

    SpKnob bassKnob { "BASS" };
    juce::Label bassValueLabel;
    juce::Label bassHintLabel;
    juce::ToggleButton enableToggle { "BASS MANAGEMENT" };
    juce::ToggleButton extendedToggle { juce::String::fromUTF8 ("EXTENDED RANGE \xc2\xb1") + "12 dB" };
    juce::ToggleButton autoHeadroomToggle { "AUTO HEADROOM" };

    SpKnob shelfFreq { "FREQ" };
    SpKnob shelfQ { "Q" };
    SpKnob shelfSlope { "SLOPE" };

    Section sub { "SUB", "20 - 120 Hz", theme::info };
    Section midBass { "MID-BASS", "40 - 250 Hz", theme::gold };
    Section punch { "PUNCH", "60 - 400 Hz", theme::goldBright };

    juce::Label headroomLabel;
    juce::Rectangle<int> bassCard, shelfCard, curveCard, sectionRow, headroomArea;
    juce::String lastHeadroom;
    std::uint64_t lastFingerprint = 0;
    bool updating = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BassControlPanel)
};

} // namespace sp
