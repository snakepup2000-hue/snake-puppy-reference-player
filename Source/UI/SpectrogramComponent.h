/*
    Snake Puppy Reference Player
    Scrolling spectrogram (waterfall) fed by AnalyzerHub::getSpectrogram.

    Rows are time (oldest at the top, newest at the bottom), columns are log
    frequency bands. Colour map: charcoal -> deep blue -> violet -> amber -> gold
    -> warm white. Display floor is adjustable; freeze holds the current image.
    No synthetic content: with no analysis rows the panel says so.

    Message thread only.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/DspUiStyle.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <vector>

namespace sp
{

class SpectrogramComponent : public juce::Component,
                             private juce::Timer
{
public:
    explicit SpectrogramComponent (AppContext&);
    ~SpectrogramComponent() override;

    void setFloorDb (float);
    void setFrozen (bool);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void rebuildColourMap();
    juce::Colour colourForDb (float db) const;
    void paintGraph (juce::Graphics&, juce::Rectangle<int>);

    AppContext& ctx;
    juce::Slider floorSlider;
    juce::ToggleButton freezeToggle { "FREEZE" };

    juce::Image image;
    std::vector<float> bands;
    int rows = 0;
    int cols = 0;
    bool hasData = false;
    bool frozen = false;
    float floorDb = -96.0f;
    float topDb = 0.0f;
    std::array<juce::Colour, 256> colourMap {};
    juce::String unavailableDetail;

    juce::Rectangle<int> headerArea, controlArea, graphArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrogramComponent)
};

} // namespace sp
