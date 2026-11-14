/*
    Snake Puppy Reference Player
    Profile / listening-intelligence panel.

    Mode selector (Manual / Auto genre / Intelligent), profile selector, the
    current decision and its human-readable explanation (with a visible
    distinction between metadata matching and measured audio analysis), Learn
    (genre / album / song), song/album override set & clear, A/B comparison with
    level matching, Reference Mode, and per-device DSP profile save/load.

    Message thread only.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/DspUiStyle.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace sp
{

class ProfilePanel : public juce::Component,
                     private juce::Timer
{
public:
    explicit ProfilePanel (AppContext&);
    ~ProfilePanel() override;

    void refreshNow();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void syncFromContext (bool force);
    void rebuildProfileList();
    void refreshModeButtons();
    void setLearnStatus (const juce::String&);
    void setOverrideStatus (const juce::String&);
    void paintCard (juce::Graphics&, juce::Rectangle<int>, const juce::String& title,
                    const juce::String& right = {});
    void paintDecision (juce::Graphics&, juce::Rectangle<int>);
    void drawBadges (juce::Graphics&, juce::Rectangle<int>, const ProfileDecision&, float& xOut);

    AppContext& ctx;

    SpButton manualButton { "MANUAL", SpButton::Style::toggle };
    SpButton autoButton { "AUTO GENRE", SpButton::Style::toggle };
    SpButton intelligentButton { "INTELLIGENT", SpButton::Style::toggle };
    SpToggleButton referenceButton { "REFERENCE MODE" };

    SpComboBox profileBox { "Profiles" };
    juce::Label activeProfileLabel;

    SpButton learnGenreButton { "LEARN GENRE" };
    SpButton learnAlbumButton { "LEARN ALBUM" };
    SpButton learnSongButton { "LEARN SONG" };
    juce::Label learnStatusLabel;

    SpButton setSongOverrideButton { "SET SONG" };
    SpButton clearSongOverrideButton { "CLEAR SONG" };
    SpButton setAlbumOverrideButton { "SET ALBUM" };
    SpButton clearAlbumOverrideButton { "CLEAR ALBUM" };
    juce::Label overrideStatusLabel;

    SpToggleButton abToggle { "A/B COMPARE" };
    SpToggleButton levelMatchToggle { "LEVEL MATCH" };
    juce::Label abStatusLabel;

    juce::Label deviceLabel;
    SpButton saveDeviceButton { "SAVE DEVICE" };
    SpButton loadDeviceButton { "LOAD DEVICE" };

    std::vector<juce::String> profileIds;
    ProfileDecision decision;
    juce::String lastExplanation;
    juce::String currentTrackTitle;
    int lastProfileCount = -1;
    bool updating = false;

    juce::Rectangle<int> modeRow, profileRow, decisionCard, learnCard, overrideCard, abCard, deviceCard;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProfilePanel)
};

} // namespace sp
