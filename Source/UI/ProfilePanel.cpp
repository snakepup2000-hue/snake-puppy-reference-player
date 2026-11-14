/*
    Snake Puppy Reference Player - profile / intelligence panel.
    Message thread only.
*/
#include "UI/ProfilePanel.h"

namespace sp
{

namespace
{
using ui::configureLabel;

juce::String modeDisplayName (EqMode mode)
{
    switch (mode)
    {
        case EqMode::manual:      return "MANUAL";
        case EqMode::autoGenre:   return "AUTO GENRE";
        case EqMode::intelligent: return "INTELLIGENT";
        case EqMode::reference:   return "REFERENCE";
    }
    return "MANUAL";
}
} // namespace

//==============================================================================
ProfilePanel::ProfilePanel (AppContext& context) : ctx (context)
{
    setOpaque (true);

    auto configureModeButton = [this] (SpButton& b, EqMode mode, int groupIndex)
    {
        b.setStyle (SpButton::Style::toggle);
        b.setClickingTogglesState (true);
        b.setRadioGroupId (0x5100 + groupIndex);
        b.onClick = [this, mode]
        {
            if (updating)
                return;
            ctx.setEqMode (mode);
            if (ctx.isReferenceMode() && mode != EqMode::reference)
                ctx.setReferenceMode (false);
            refreshModeButtons();
            syncFromContext (true);
        };
        addAndMakeVisible (b);
    };
    configureModeButton (manualButton, EqMode::manual, 1);
    configureModeButton (autoButton, EqMode::autoGenre, 1);
    configureModeButton (intelligentButton, EqMode::intelligent, 1);

    referenceButton.setClickingTogglesState (true);
    referenceButton.onClick = [this]
    {
        if (updating)
            return;
        ctx.setReferenceMode (referenceButton.getToggleState());
        refreshModeButtons();
        syncFromContext (true);
    };
    addAndMakeVisible (referenceButton);

    addAndMakeVisible (profileBox);
    profileBox.setTextWhenNothingSelected ("No profile");
    profileBox.onChange = [this]
    {
        if (updating)
            return;
        const int index = profileBox.getSelectedItemIndex();
        if (index >= 0 && index < (int) profileIds.size())
            ctx.selectProfile (profileIds[(size_t) index]);
        syncFromContext (true);
    };

    configureLabel (activeProfileLabel, 10.5f, theme::textDim);
    addAndMakeVisible (activeProfileLabel);

    learnGenreButton.onClick = [this] { juce::String m; ctx.learnCurrentAdjustment (LearnScope::genre, m); setLearnStatus (m); };
    learnAlbumButton.onClick = [this] { juce::String m; ctx.learnCurrentAdjustment (LearnScope::album, m); setLearnStatus (m); };
    learnSongButton.onClick = [this] { juce::String m; ctx.learnCurrentAdjustment (LearnScope::song, m); setLearnStatus (m); };
    addAndMakeVisible (learnGenreButton);
    addAndMakeVisible (learnAlbumButton);
    addAndMakeVisible (learnSongButton);
    configureLabel (learnStatusLabel, 10.0f, theme::textDim);
    addAndMakeVisible (learnStatusLabel);

    setSongOverrideButton.onClick = [this]
    {
        const auto track = ctx.engine().getStatus().currentTrack;
        if (track.path.isEmpty())
        {
            setOverrideStatus ("No track is loaded to override.");
            return;
        }
        const bool ok = ctx.profiles().setSongOverride (track, ctx.getCurrentDspParams());
        setOverrideStatus (ok ? "Song override saved for \"" + track.displayTitle() + "\"."
                              : "Could not save the song override.");
    };
    clearSongOverrideButton.onClick = [this]
    {
        const auto track = ctx.engine().getStatus().currentTrack;
        if (track.path.isEmpty())
        {
            setOverrideStatus ("No track is loaded.");
            return;
        }
        ctx.profiles().clearSongOverride (track.id);
        setOverrideStatus ("Song override cleared.");
        syncFromContext (true);
    };
    setAlbumOverrideButton.onClick = [this]
    {
        const auto track = ctx.engine().getStatus().currentTrack;
        if (track.path.isEmpty())
        {
            setOverrideStatus ("No track is loaded to override.");
            return;
        }
        const bool ok = ctx.profiles().setAlbumOverride (track, ctx.getCurrentDspParams());
        setOverrideStatus (ok ? "Album override saved for \"" + track.displayAlbum() + "\"."
                              : "Could not save the album override.");
    };
    clearAlbumOverrideButton.onClick = [this]
    {
        const auto track = ctx.engine().getStatus().currentTrack;
        if (track.path.isEmpty())
        {
            setOverrideStatus ("No track is loaded.");
            return;
        }
        ctx.profiles().clearAlbumOverride (track);
        setOverrideStatus ("Album override cleared.");
        syncFromContext (true);
    };
    addAndMakeVisible (setSongOverrideButton);
    addAndMakeVisible (clearSongOverrideButton);
    addAndMakeVisible (setAlbumOverrideButton);
    addAndMakeVisible (clearAlbumOverrideButton);
    configureLabel (overrideStatusLabel, 10.0f, theme::textDim);
    addAndMakeVisible (overrideStatusLabel);

    abToggle.setClickingTogglesState (true);
    abToggle.onClick = [this]
    {
        if (updating)
            return;
        ctx.setAbComparisonActive (abToggle.getToggleState());
        syncFromContext (true);
    };
    addAndMakeVisible (abToggle);

    levelMatchToggle.setClickingTogglesState (true);
    levelMatchToggle.onClick = [this]
    {
        if (updating)
            return;
        ctx.setAbLevelMatching (levelMatchToggle.getToggleState());
        syncFromContext (true);
    };
    addAndMakeVisible (levelMatchToggle);
    configureLabel (abStatusLabel, 10.0f, theme::textDim);
    addAndMakeVisible (abStatusLabel);

    configureLabel (deviceLabel, 10.0f, theme::textDim);
    addAndMakeVisible (deviceLabel);
    saveDeviceButton.onClick = [this]
    {
        ctx.saveDspForCurrentDevice();
        setOverrideStatus (ctx.getStatusMessage());
    };
    loadDeviceButton.onClick = [this]
    {
        ctx.applyDspForCurrentDevice();
        syncFromContext (true);
    };
    addAndMakeVisible (saveDeviceButton);
    addAndMakeVisible (loadDeviceButton);

    setSize (900, 660);
    rebuildProfileList();
    syncFromContext (true);
    startTimerHz (4);
}

ProfilePanel::~ProfilePanel()
{
    stopTimer();
}

//==============================================================================
void ProfilePanel::refreshNow()
{
    rebuildProfileList();
    syncFromContext (true);
}

void ProfilePanel::timerCallback()
{
    syncFromContext (false);
}

void ProfilePanel::rebuildProfileList()
{
    const auto profiles = ctx.profiles().getAllProfiles();
    if ((int) profiles.size() == lastProfileCount)
        return;

    updating = true;
    profileIds.clear();
    profileBox.clear (juce::dontSendNotification);

    int itemId = 1;
    for (auto& p : profiles)
    {
        juce::String label = p.name.isNotEmpty() ? p.name : p.id;
        if (p.genre.isNotEmpty())
            label << "  |  " << p.genre;
        if (p.builtIn)
            label << "  (built-in)";
        profileBox.addItem (label, itemId++);
        profileIds.push_back (p.id);
    }
    updating = false;
    lastProfileCount = (int) profiles.size();
}

void ProfilePanel::refreshModeButtons()
{
    updating = true;
    const bool reference = ctx.isReferenceMode();
    const auto mode = reference ? EqMode::reference : ctx.getEqMode();
    manualButton.setToggleState (mode == EqMode::manual, juce::dontSendNotification);
    autoButton.setToggleState (mode == EqMode::autoGenre, juce::dontSendNotification);
    intelligentButton.setToggleState (mode == EqMode::intelligent, juce::dontSendNotification);
    referenceButton.setToggleState (reference, juce::dontSendNotification);
    updating = false;
}

void ProfilePanel::syncFromContext (bool force)
{
    rebuildProfileList();
    refreshModeButtons();

    const auto& engineStatus = ctx.engine().getStatus();
    const auto track = engineStatus.currentTrack;
    currentTrackTitle = track.path.isNotEmpty() ? track.displayTitle() : juce::String();

    const auto activeId = ctx.getActiveProfileId();
    updating = true;
    int selectedIndex = -1;
    for (int i = 0; i < (int) profileIds.size(); ++i)
        if (profileIds[(size_t) i] == activeId)
            selectedIndex = i;
    profileBox.setSelectedItemIndex (selectedIndex, juce::dontSendNotification);
    updating = false;

    decision = ctx.getCurrentProfileDecision();
    const auto explanation = decision.explanation().isNotEmpty()
                                 ? decision.explanation()
                                 : (ctx.isReferenceMode()
                                        ? "Reference Mode: the whole DSP chain is bypassed and volume is unity."
                                        : "Manual mode: the current manual EQ and bass settings are applied exactly as set.");

    if (explanation != lastExplanation)
    {
        lastExplanation = explanation;
        repaint();
    }

    const bool haveTrack = track.path.isNotEmpty();
    learnGenreButton.setEnabled (haveTrack);
    learnAlbumButton.setEnabled (haveTrack);
    learnSongButton.setEnabled (haveTrack);
    setSongOverrideButton.setEnabled (haveTrack);
    setAlbumOverrideButton.setEnabled (haveTrack);
    clearSongOverrideButton.setEnabled (haveTrack && ctx.profiles().hasSongOverride (track.id));
    clearAlbumOverrideButton.setEnabled (haveTrack && ctx.profiles().hasAlbumOverride (track));
    saveDeviceButton.setEnabled (engineStatus.deviceOpen);

    const auto deviceName = engineStatus.signalPath.outputDeviceName.isNotEmpty()
                                ? engineStatus.signalPath.outputDeviceName
                                : engineStatus.signalPath.outputDeviceType;
    deviceLabel.setText (deviceName.isNotEmpty() ? "DEVICE: " + deviceName.toUpperCase()
                                                 : "DEVICE: NO OUTPUT DEVICE",
                         juce::dontSendNotification);

    juce::String profileInfo;
    if (decision.profileName.isNotEmpty())
        profileInfo = "PROFILE: " + decision.profileName;
    if (decision.matchedGenre.isNotEmpty())
        profileInfo << "  |  GENRE: " << decision.matchedGenre;
    if (! haveTrack)
        profileInfo << "  |  NO TRACK LOADED";
    activeProfileLabel.setText (profileInfo, juce::dontSendNotification);

    updating = true;
    abToggle.setToggleState (ctx.isAbComparisonActive(), juce::dontSendNotification);
    levelMatchToggle.setToggleState (ctx.isAbLevelMatching(), juce::dontSendNotification);
    updating = false;
    abStatusLabel.setText (ctx.getAbStatusText(), juce::dontSendNotification);

    if (force)
        repaint();
}

void ProfilePanel::setLearnStatus (const juce::String& message)
{
    learnStatusLabel.setText (message, juce::dontSendNotification);
    learnStatusLabel.setColour (juce::Label::textColourId,
                                message.containsIgnoreCase ("could not") || message.containsIgnoreCase ("no track")
                                    ? theme::warning : theme::success);
}

void ProfilePanel::setOverrideStatus (const juce::String& message)
{
    overrideStatusLabel.setText (message, juce::dontSendNotification);
    overrideStatusLabel.setColour (juce::Label::textColourId,
                                   message.containsIgnoreCase ("could not") || message.containsIgnoreCase ("no track")
                                       ? theme::warning : theme::silverDim);
}

//==============================================================================
void ProfilePanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    auto top = getLocalBounds().reduced (14, 8).removeFromTop (22);
    theme::drawSectionHeader (g, top, "LISTENING INTELLIGENCE",
                              getWidth() < 560 ? juce::String()
                                               : juce::String ("PROFILE / LEARNING / COMPARISON"));

    paintCard (g, learnCard, "LEARN THIS ADJUSTMENT", "SAVES THE CURRENT SOUND TO A PREFERENCE");
    paintCard (g, overrideCard, "OVERRIDE", "PIN THE SOUND FOR THIS SONG OR ALBUM");
    paintCard (g, abCard, "A/B COMPARISON", "PROCESSED VS ORIGINAL");
    paintCard (g, deviceCard, "DEVICE DSP PROFILE", "PER-OUTPUT EQ");

    paintDecision (g, decisionCard);
}

void ProfilePanel::paintCard (juce::Graphics& g, juce::Rectangle<int> card,
                              const juce::String& title, const juce::String& right)
{
    if (card.isEmpty())
        return;
    theme::fillPanel (g, card);
    auto titleArea = card.reduced (10, 5).removeFromTop (15);
    theme::drawSectionHeader (g, titleArea, title, right);
}

void ProfilePanel::paintDecision (juce::Graphics& g, juce::Rectangle<int> card)
{
    if (card.isEmpty())
        return;

    theme::fillPanel (g, card);
    const bool compactCard = card.getHeight() < 96;
    auto inner = card.reduced (10, compactCard ? 4 : 6);
    auto titleArea = inner.removeFromTop (compactCard ? 12 : 15);
    theme::drawSectionHeader (g, titleArea, compactCard ? "DECISION" : "CURRENT DECISION",
                              modeDisplayName (ctx.isReferenceMode() ? EqMode::reference : ctx.getEqMode()));
    inner.removeFromTop (compactCard ? 2 : 4);

    // Badges first: metadata vs analysis vs learned vs override is explicit.
    if (inner.getHeight() > (compactCard ? 64 : 44))
    {
        auto badgeRow = inner.removeFromTop (16);
        float x = (float) badgeRow.getX();
        drawBadges (g, badgeRow, decision, x);
    }

    // Profile / genre line.
    juce::String headline;
    if (decision.profileName.isNotEmpty())
        headline = decision.profileName;
    else
        headline = currentTrackTitle.isNotEmpty() ? currentTrackTitle : "No track loaded";

    g.setColour (ctx.isReferenceMode() ? theme::gold : theme::silver);
    g.setFont (theme::uiFont (compactCard ? 11.5f : (inner.getHeight() < 60 ? 12.0f : 14.0f), true, 0.03f));
    g.drawText (headline, inner.removeFromTop (compactCard ? 14 : (inner.getHeight() < 60 ? 16 : 20)),
                juce::Justification::centredLeft, true);

    inner.removeFromTop (2);

    // Explanation.
    g.setColour (theme::textDim);
    g.setFont (theme::uiFont (10.5f));
    auto explanationArea = inner.removeFromTop (juce::jmax (30, inner.getHeight()));
    g.drawFittedText (lastExplanation, explanationArea, juce::Justification::topLeft,
                      juce::jmax (2, explanationArea.getHeight() / 14));
}

void ProfilePanel::drawBadges (juce::Graphics& g, juce::Rectangle<int> area,
                               const ProfileDecision& d, float& xOut)
{
    struct Badge { juce::String text; juce::Colour colour; };
    std::vector<Badge> badges;

    const bool reference = ctx.isReferenceMode();
    if (reference)
        badges.push_back ({ "REFERENCE", theme::gold });
    else if (d.mode == EqMode::manual)
        badges.push_back ({ "MANUAL", theme::silverDim });
    else
        badges.push_back ({ "METADATA", theme::silverDim });

    if (d.usedAudioAnalysis)
        badges.push_back ({ "AUDIO ANALYSIS", theme::info });
    if (d.learnedPreferenceApplied)
        badges.push_back ({ "LEARNED", theme::success });
    if (d.overrideKind.equalsIgnoreCase ("song"))
        badges.push_back ({ "SONG OVERRIDE", theme::gold });
    else if (d.overrideKind.equalsIgnoreCase ("album"))
        badges.push_back ({ "ALBUM OVERRIDE", theme::gold });

    if (badges.empty())
        return;

    for (auto& badge : badges)
    {
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (theme::capsFont (9.0f, 0.10f), badge.text, 0.0f, 0.0f);
        const float w = glyphs.getBoundingBox (0, -1, true).getWidth() + 12.0f;

        if (xOut + w > (float) area.getRight())
            break;

        juce::Rectangle<float> pill (xOut, (float) area.getY(), w, (float) area.getHeight() - 2.0f);
        g.setColour (badge.colour.withAlpha (0.13f));
        g.fillRoundedRectangle (pill, 2.5f);
        g.setColour (badge.colour.withAlpha (0.65f));
        g.drawRoundedRectangle (pill, 2.5f, 1.0f);
        g.setColour (badge.colour);
        g.setFont (theme::capsFont (9.0f, 0.10f));
        g.drawText (badge.text, pill.toNearestInt().reduced (6, 0),
                    juce::Justification::centred, false);

        xOut += w + 6.0f;
    }
}

//==============================================================================
void ProfilePanel::resized()
{
    auto r = getLocalBounds().reduced (14, 8);
    r.removeFromTop (22);
    r.removeFromTop (6);

    const bool railMode = getHeight() < 480;
    if (railMode)
    {
        // The main-window rail gives this panel a fixed short height. Show the
        // mode, profile and the decision that is actually applied, and leave the
        // learning/override/device controls to the DSP Studio Profiles tab.
        decisionCard = {};
        learnCard = {};
        overrideCard = {};
        abCard = {};
        deviceCard = {};

        learnGenreButton.setVisible (false); learnAlbumButton.setVisible (false); learnSongButton.setVisible (false);
        learnStatusLabel.setVisible (false);
        setSongOverrideButton.setVisible (false); clearSongOverrideButton.setVisible (false);
        setAlbumOverrideButton.setVisible (false); clearAlbumOverrideButton.setVisible (false);
        overrideStatusLabel.setVisible (false);
        abToggle.setVisible (false); levelMatchToggle.setVisible (false); abStatusLabel.setVisible (false);
        deviceLabel.setVisible (false); saveDeviceButton.setVisible (false); loadDeviceButton.setVisible (false);
        activeProfileLabel.setVisible (false);
        // Reference Mode is available on the transport bar and in the DSP
        // Studio; in the short rail panel the space is better used by the
        // decision explanation.
        referenceButton.setVisible (false);

        const int rowHeight = juce::jmax (22, juce::jmin (28, (r.getHeight() - 70) / 2));
        modeRow = r.removeFromTop (rowHeight);
        r.removeFromTop (3);
        profileBox.setBounds (r.removeFromTop (26));
        r.removeFromTop (3);
        decisionCard = r.removeFromTop (juce::jmax (48, r.getHeight()));

        auto modeRowArea = modeRow;
        const int bw = juce::jmax (58, (modeRowArea.getWidth() - 16) / 3);
        manualButton.setBounds (modeRowArea.removeFromLeft (bw));
        modeRowArea.removeFromLeft (8);
        autoButton.setBounds (modeRowArea.removeFromLeft (bw));
        modeRowArea.removeFromLeft (8);
        intelligentButton.setBounds (modeRowArea.removeFromLeft (bw));
        return;
    }

    learnGenreButton.setVisible (true); learnAlbumButton.setVisible (true); learnSongButton.setVisible (true);
    learnStatusLabel.setVisible (true);
    setSongOverrideButton.setVisible (true); clearSongOverrideButton.setVisible (true);
    setAlbumOverrideButton.setVisible (true); clearAlbumOverrideButton.setVisible (true);
    overrideStatusLabel.setVisible (true);
    abToggle.setVisible (true); levelMatchToggle.setVisible (true); abStatusLabel.setVisible (true);
    deviceLabel.setVisible (true); saveDeviceButton.setVisible (true); loadDeviceButton.setVisible (true);
    activeProfileLabel.setVisible (true);
    referenceButton.setVisible (true);

    if (getWidth() < 620)
    {
        modeRow = r.removeFromTop (30);
        r.removeFromTop (6);
        {
            auto row = modeRow;
            const int bw = juce::jmax (60, (row.getWidth() - 16) / 3);
            manualButton.setBounds (row.removeFromLeft (bw));
            row.removeFromLeft (8);
            autoButton.setBounds (row.removeFromLeft (bw));
            row.removeFromLeft (8);
            intelligentButton.setBounds (row.removeFromLeft (bw));
        }

        referenceButton.setBounds (r.removeFromTop (30));
        r.removeFromTop (8);

        profileBox.setBounds (r.removeFromTop (28));
        r.removeFromTop (2);
        activeProfileLabel.setBounds (r.removeFromTop (16));
        r.removeFromTop (8);

        decisionCard = r.removeFromTop (juce::jmin (176, juce::jmax (130, r.getHeight() / 3)));
        r.removeFromTop (8);

        abCard = r.removeFromTop (84);
        r.removeFromTop (8);
        learnCard = r.removeFromTop (96);
        r.removeFromTop (8);
        overrideCard = r.removeFromTop (104);
        r.removeFromTop (8);
        deviceCard = r.removeFromTop (84);

        {
            auto inner = abCard.reduced (10, 5);
            inner.removeFromTop (16);
            auto row = inner.removeFromTop (24);
            abToggle.setBounds (row.removeFromLeft (row.getWidth() / 2 - 5));
            row.removeFromLeft (10);
            levelMatchToggle.setBounds (row);
            abStatusLabel.setBounds (inner.removeFromTop (18));
        }
        {
            auto inner = learnCard.reduced (10, 5);
            inner.removeFromTop (16);
            auto row = inner.removeFromTop (24);
            const int bw = juce::jmax (56, (row.getWidth() - 16) / 3);
            learnGenreButton.setBounds (row.removeFromLeft (bw));
            row.removeFromLeft (8);
            learnAlbumButton.setBounds (row.removeFromLeft (bw));
            row.removeFromLeft (8);
            learnSongButton.setBounds (row.removeFromLeft (bw));
            learnStatusLabel.setBounds (inner.removeFromTop (18));
        }
        {
            auto inner = overrideCard.reduced (10, 5);
            inner.removeFromTop (16);
            const int bw = juce::jmax (56, (inner.getWidth() - 10) / 2);
            auto row1 = inner.removeFromTop (24);
            setSongOverrideButton.setBounds (row1.removeFromLeft (bw));
            row1.removeFromLeft (10);
            clearSongOverrideButton.setBounds (row1.removeFromLeft (bw));
            inner.removeFromTop (4);
            auto row2 = inner.removeFromTop (24);
            setAlbumOverrideButton.setBounds (row2.removeFromLeft (bw));
            row2.removeFromLeft (10);
            clearAlbumOverrideButton.setBounds (row2.removeFromLeft (bw));
            inner.removeFromTop (2);
            overrideStatusLabel.setBounds (inner.removeFromTop (18));
        }
        {
            auto inner = deviceCard.reduced (10, 5);
            inner.removeFromTop (16);
            deviceLabel.setBounds (inner.removeFromTop (16));
            auto row = inner.removeFromTop (24);
            saveDeviceButton.setBounds (row.removeFromLeft (row.getWidth() / 2 - 5));
            row.removeFromLeft (10);
            loadDeviceButton.setBounds (row);
        }
        return;
    }

    modeRow = r.removeFromTop (32);
    r.removeFromTop (8);
    profileRow = r.removeFromTop (30);
    r.removeFromTop (8);

    decisionCard = r.removeFromTop (juce::jlimit (110, 150, r.getHeight() * 27 / 100));
    r.removeFromTop (8);

    learnCard = r.removeFromTop (74);
    r.removeFromTop (8);
    overrideCard = r.removeFromTop (74);
    r.removeFromTop (8);

    const int bottomHeight = juce::jmin (96, juce::jmax (64, r.getHeight()));
    auto bottom = r.removeFromTop (bottomHeight);
    abCard = bottom.removeFromLeft ((bottom.getWidth() - 10) / 2);
    bottom.removeFromLeft (10);
    deviceCard = bottom;

    // Mode row.
    {
        auto row = modeRow;
        const int bw = juce::jmax (92, (row.getWidth() - 190 - 24) / 3);
        manualButton.setBounds (row.removeFromLeft (bw));
        row.removeFromLeft (8);
        autoButton.setBounds (row.removeFromLeft (bw));
        row.removeFromLeft (8);
        intelligentButton.setBounds (row.removeFromLeft (bw));
        row.removeFromLeft (10);
        referenceButton.setBounds (row);
    }

    // Profile row.
    {
        auto row = profileRow;
        profileBox.setBounds (row.removeFromLeft (juce::jlimit (200, 300, row.getWidth() / 2)));
        row.removeFromLeft (10);
        activeProfileLabel.setBounds (row);
    }

    // Learn controls.
    {
        auto inner = learnCard.reduced (10, 6);
        inner.removeFromTop (17);
        auto row = inner.removeFromTop (24);
        const int bw = juce::jmin (128, (row.getWidth() - 20) / 3);
        learnGenreButton.setBounds (row.removeFromLeft (bw));
        row.removeFromLeft (10);
        learnAlbumButton.setBounds (row.removeFromLeft (bw));
        row.removeFromLeft (10);
        learnSongButton.setBounds (row.removeFromLeft (bw));
        row.removeFromLeft (10);
        learnStatusLabel.setBounds (row);
    }

    // Override controls.
    {
        auto inner = overrideCard.reduced (10, 6);
        inner.removeFromTop (17);
        auto row = inner.removeFromTop (24);
        const int bw = juce::jmin (104, (row.getWidth() - 30) / 4);
        setSongOverrideButton.setBounds (row.removeFromLeft (bw));
        row.removeFromLeft (8);
        clearSongOverrideButton.setBounds (row.removeFromLeft (bw));
        row.removeFromLeft (10);
        setAlbumOverrideButton.setBounds (row.removeFromLeft (bw));
        row.removeFromLeft (8);
        clearAlbumOverrideButton.setBounds (row.removeFromLeft (bw));
        row.removeFromLeft (10);
        overrideStatusLabel.setBounds (row);
    }

    // A/B card.
    {
        auto inner = abCard.reduced (10, 6);
        inner.removeFromTop (17);
        abToggle.setBounds (inner.removeFromTop (24).removeFromLeft (juce::jmin (150, inner.getWidth())));
        levelMatchToggle.setBounds (inner.removeFromTop (22).removeFromLeft (juce::jmin (150, inner.getWidth())));
        abStatusLabel.setBounds (inner.removeFromTop (16));
    }

    // Device card.
    {
        auto inner = deviceCard.reduced (10, 6);
        inner.removeFromTop (17);
        deviceLabel.setBounds (inner.removeFromTop (16));
        auto row = inner.removeFromTop (24);
        saveDeviceButton.setBounds (row.removeFromLeft (juce::jmin (120, row.getWidth() / 2)));
        row.removeFromLeft (8);
        loadDeviceButton.setBounds (row.removeFromLeft (juce::jmin (120, row.getWidth())));
    }
}

} // namespace sp
