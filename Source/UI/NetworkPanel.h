/*
    Snake Puppy Reference Player
    Network output panel.

    Honest network audio surface:
      - UPnP/DLNA renderer discovery and endpoint list
      - connect / disconnect, transport state, local self-test with pass/fail
      - start / stop network playback of the currently loaded track
      - supported vs unavailable transports (with reasons)
      - an explicit warning that local DSP does not apply to network playback
    Nothing is claimed as working until a self-test or a real device confirms it.

    Message thread only; self-test runs on a worker thread.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/DspUiStyle.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <thread>
#include <vector>

namespace sp
{

class NetworkPanel : public juce::Component,
                     private juce::Timer,
                     private juce::ListBoxModel
{
public:
    explicit NetworkPanel (AppContext&);
    ~NetworkPanel() override;

    void refreshNow();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void runSelfTest();
    void onSelfTestFinished (const NetworkOutput::SelfTestResult& result, const juce::String& notes);
    void connectSelected();
    void startHostProbe();
    void playOnSelected();
    bool startPiStream (const Track& track, juce::String& errorOut);
    void stopNetworkPlayback();
    void refreshEndpointList();
    void updateStatusTexts();
    void setResultStatus (const juce::String& message, bool warning);
    juce::String verificationLabel() const;

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem (int rowNumber, juce::Graphics&, int width, int height, bool rowIsSelected) override;
    void selectedRowsChanged (int lastRowSelected) override;

    AppContext& ctx;
    juce::ListBox endpointList { "renderers", this };
    std::vector<NetworkEndpoint> endpoints;
    int selectedRow = -1;
    juce::String lastEndpointSignature;

    juce::TextEditor hostField;
    SpButton probeHostButton { "PROBE HOST" };
    SpButton discoverButton { "DISCOVER" };
    SpButton selfTestButton { "RUN SELF-TEST" };
    SpButton connectButton { "CONNECT" };
    SpButton disconnectButton { "DISCONNECT" };
    SpButton playButton { "PLAY CURRENT TRACK" };
    SpButton stopButton { "STOP" };
    SpButton piPlayButton { "PLAY ON PI K11 (DSP)" };
    SpButton piStopButton { "STOP PI" };
    SpButton mediaServerButton { "START MEDIA SERVER" };
    SpToggleButton enableToggle { "NETWORK OUTPUT" };
    juce::Label statusLabel;
    juce::Label resultLabel;
    juce::Label endpointDetailLabel;
    juce::Label warningLabel;

    std::thread testThread;
    std::thread probeThread;
    std::atomic<bool> probeRunning { false };
    std::atomic<bool> testRunning { false };
    NetworkOutput::SelfTestResult lastSelfTest;
    juce::String lastSelfTestNotes;
    bool hasSelfTest = false;
    bool resultIsWarning = false;

    juce::Rectangle<int> headerArea, warningArea, listArea, buttonsArea, statusCard, resultCard,
                        capabilityCard, serverCard, detailArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkPanel)
};

} // namespace sp
