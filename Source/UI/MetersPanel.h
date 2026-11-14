/*
    Snake Puppy Reference Player
    Metering panel - compact (transport) and expanded (DSP studio).

    Every value comes from AnalyzerHub::getMeterValues(), i.e. from real audio
    pushed by the audio callback. There is no simulated movement: with no data
    the panel says so. Peak hold and clip latching are UI-side presentations of
    real measurements (hold decays after 1.5 s, clip latches for 1.5 s).

    Message thread only; polls at 30 Hz.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/DspUiStyle.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace sp
{

class MetersPanel : public juce::Component,
                    private juce::Timer
{
public:
    enum class Mode { compact, expanded };

    explicit MetersPanel (AppContext&, Mode mode = Mode::expanded);
    ~MetersPanel() override;

    void setMode (Mode);
    Mode getMode() const noexcept { return mode; }
    void setCompact (bool shouldBeCompact) { setMode (shouldBeCompact ? Mode::compact : Mode::expanded); }
    void setShowInputComparison (bool);
    void resetPeakHolds();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    struct ChannelHold
    {
        float peakDb = -144.0f;
        float truePeakDb = -144.0f;
        juce::int64 holdUntilMs = 0;
    };

    void timerCallback() override;
    void paintHeader (juce::Graphics&, juce::Rectangle<int>);
    void paintCompact (juce::Graphics&, juce::Rectangle<int>);
    void paintExpanded (juce::Graphics&, juce::Rectangle<int>);
    void paintChannelRow (juce::Graphics&, juce::Rectangle<int>, const juce::String& label,
                          int channel, bool showTruePeak);
    void paintLufs (juce::Graphics&, juce::Rectangle<int>);
    void paintStats (juce::Graphics&, juce::Rectangle<int>);
    void paintInputOutput (juce::Graphics&, juce::Rectangle<int>);
    void paintBassEnergy (juce::Graphics&, juce::Rectangle<int>);
    void drawMeterBar (juce::Graphics&, juce::Rectangle<int>, float peakDb, float holdDb,
                       float rmsDb, bool clipped);
    bool isDataAvailable() const;
    juce::int64 timeSinceLastRepaintMs();
    static float dbToX (float db, juce::Rectangle<int> bar);
    static juce::String lufsText (float v);

    AppContext& ctx;
    Mode mode = Mode::expanded;
    bool showInput = true;
    MeterValues values;
    ChannelHold holds[2];
    bool clipLatched = false;
    juce::int64 clipLatchUntilMs = 0;
    std::int64_t lastClipCount = 0;
    std::int64_t lastUnderrunCount = 0;
    juce::String engineError;
    bool engineOnline = false;
    juce::int64 lastRepaintRequestMs = 0;

    // Layout rectangles (recomputed in resized()).
    juce::Rectangle<int> headerArea, peakArea, lufsArea, statsArea, ioArea, bassArea, flagsArea;
    juce::Rectangle<int> channelRows[2];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MetersPanel)
};

} // namespace sp
