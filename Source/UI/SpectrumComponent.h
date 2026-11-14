/*
    Snake Puppy Reference Player
    Real-time FFT spectrum display fed by AnalyzerHub::getSpectrumDb.

    Log frequency axis (20 Hz - 20 kHz), dBFS scale, peak-hold with time decay,
    selectable FFT size and display update rate, and a stereo L/R overlay when
    the analyser exposes per-channel spectra. No synthetic motion: if the
    analyser has no data the panel says so.

    Message thread only.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/DspUiStyle.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace sp
{

class SpectrumComponent : public juce::Component,
                          private juce::Timer
{
public:
    explicit SpectrumComponent (AppContext&);
    ~SpectrumComponent() override;

    void setPeakHoldEnabled (bool);
    void setDisplayRange (float floorDb, float topDb);
    void setStereoOverlay (bool);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void applyTimerRate();
    void paintGraph (juce::Graphics&, juce::Rectangle<int>);
    void paintEmpty (juce::Graphics&, juce::Rectangle<int>);

    AppContext& ctx;
    juce::ComboBox fftBox;
    juce::ComboBox rateBox;
    juce::ToggleButton peakHoldToggle { "PEAK HOLD" };
    juce::ToggleButton stereoToggle { "L / R" };

    std::vector<float> db, freqs, dbLeft, dbRight, peakDb;
    bool perChannelSupported = false;
    bool stereoOn = false;
    bool peakHoldOn = true;
    bool hasData = false;
    bool frozen = false;
    float floorDb = -96.0f;
    float topDb = 0.0f;
    juce::Rectangle<int> graphArea, controlsArea, headerArea;
    juce::String unavailableDetail;
    double lastUpdateMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumComponent)
};

} // namespace sp
