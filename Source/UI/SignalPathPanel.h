/*
    Snake Puppy Reference Player
    Honest audio signal-path panel.

    Renders EngineStatus::signalPath (source format/rate/bits, decoder, DSD,
    resampling, active DSP steps, volume / ReplayGain / dither, output device and
    buffer) plus the chain status. The quality label is colour-coded exactly as
    computed by the engine:
      gold   = bit-perfect
      silver = bit-transparent
      dim    = processed
      red    = error
    When the engine is offline the panel says so instead of inventing values.

    Message thread only.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/DspUiStyle.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace sp
{

class SignalPathPanel : public juce::Component,
                        private juce::Timer
{
public:
    explicit SignalPathPanel (AppContext&);
    ~SignalPathPanel() override;

    void refreshNow();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintSection (juce::Graphics&, juce::Rectangle<int>, const juce::String& title,
                       const std::vector<std::pair<juce::String, juce::String>>& rows);
    void paintHeader (juce::Graphics&, juce::Rectangle<int>);

    AppContext& ctx;
    EngineStatus status;
    juce::Rectangle<int> headerArea, qualityArea, engineArea, sourceArea, dspArea, outputArea, qualityDetailArea;
    juce::String lastFingerprint;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SignalPathPanel)
};

} // namespace sp
