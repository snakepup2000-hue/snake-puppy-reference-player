/*
    Snake Puppy Reference Player - application shell implementation.
*/
#include "UI/MainComponent.h"

#include "UI/GlobalShortcutMonitor.h"
#include "UI/ObjCExceptionGuard.h"

#include <juce_events/juce_events.h>

#if __has_include("UI/BassControlPanel.h")
    #include "UI/BassControlPanel.h"
    #define SP_HAVE_BASS_PANEL 1
#else
    #define SP_HAVE_BASS_PANEL 0
#endif

#if __has_include("UI/EqCurveComponent.h")
    #include "UI/EqCurveComponent.h"
    #define SP_HAVE_EQ_CURVE 1
#else
    #define SP_HAVE_EQ_CURVE 0
#endif

#if __has_include("UI/ProfilePanel.h")
    #include "UI/ProfilePanel.h"
    #define SP_HAVE_PROFILE_PANEL 1
#else
    #define SP_HAVE_PROFILE_PANEL 0
#endif

#if __has_include("UI/SpectrumComponent.h")
    #include "UI/SpectrumComponent.h"
    #define SP_HAVE_SPECTRUM 1
#else
    #define SP_HAVE_SPECTRUM 0
#endif

#if __has_include("UI/DspStudioWindow.h")
    #include "UI/DspStudioWindow.h"
    #define SP_HAVE_DSP_STUDIO_WINDOW 1
#else
    #define SP_HAVE_DSP_STUDIO_WINDOW 0
#endif

#if __has_include("UI/SettingsWindow.h")
    #include "UI/SettingsWindow.h"
    #define SP_HAVE_SETTINGS_WINDOW 1
#else
    #define SP_HAVE_SETTINGS_WINDOW 0
#endif

namespace sp
{

namespace
{
enum MenuCommand
{
    cmdImportFiles = 1001,
    cmdImportFolder,
    cmdImportPlaylist,
    cmdExportPlaylist,
    cmdQuit,

    cmdToggleAb = 1101,
    cmdToggleAbLevelMatch,

    cmdViewSongs = 1200,
    cmdViewAlbums,
    cmdViewArtists,
    cmdViewGenres,
    cmdViewYears,
    cmdViewFormats,
    cmdViewFolders,
    cmdViewQueue,
    cmdViewHealth,
    cmdViewFullPlayer,
    cmdViewRack,

    cmdDspManual = 1300,
    cmdDspAutoGenre,
    cmdDspIntelligent,
    cmdDspBypassEq,
    cmdDspLearnOff,
    cmdDspLearnGenre,
    cmdDspLearnAlbum,
    cmdDspLearnSong,
    cmdDspSaveDeviceProfile,
    cmdDspLoadDeviceProfile,

    cmdSettingsPreferences = 1400,

    cmdHelpAbout = 1500,
    cmdHelpDiagnostics,
    cmdHelpShortcuts
};

juce::String audioFilePattern()
{
    return "*.wav;*.aif;*.aiff;*.flac;*.m4a;*.alac;*.mp3;*.ogg;*.opus;*.dsf;*.dff;*.ape;*.wv;*.aac;*.caf";
}

juce::String playlistFilePattern()
{
    return "*.m3u;*.m3u8;*.pls";
}

juce::String shortcutSignature (const std::map<AppAction, juce::String>& shortcuts)
{
    juce::String signature;
    for (auto& entry : shortcuts)
        signature << (int) entry.first << "=" << entry.second << ";";
    return signature;
}
} // namespace

//==============================================================================
class MainComponent::RackContent : public juce::Component
{
public:
    RackContent (AppContext& c, juce::Component* bass, juce::Component* eq,
                 juce::Component* profile, juce::Component* spectrum)
        : ctx (c)
    {
        // Heights tuned so all four rail panels fit the default 1440x900
        // window without clipping (ui-dsp compact/rail layouts).
        addSection ("BASS MANAGEMENT", bass, 190);
        addSection ("EQ CURVE", eq, 152);
        addSection ("PROFILE", profile, 176);
        addSection ("SPECTRUM", spectrum, 168);

        for (auto& section : sections)
            if (section.panel != nullptr)
                addAndMakeVisible (*section.panel);
    }

    int getPreferredHeight() const
    {
        int height = 3;
        for (auto& section : sections)
            height += section.height + 3;
        return height + 2;
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (7, 3);

        for (auto& section : sections)
        {
            if (section.panel == nullptr)
                section.labelArea = area.removeFromTop (20);

            section.panelArea = area.removeFromTop (section.height);
            area.removeFromTop (3);

            if (section.panel != nullptr)
                section.panel->setBounds (section.panelArea);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);

        for (auto& section : sections)
        {
            if (section.panel != nullptr)
                continue;      // ui-dsp panels draw their own engraved headers

            theme::drawSectionHeader (g, section.labelArea.withTrimmedLeft (4), section.title);

            g.setColour (theme::panelAlt);
            g.fillRoundedRectangle (section.panelArea.toFloat(), 3.0f);
            g.setColour (theme::line);
            g.drawRoundedRectangle (section.panelArea.toFloat().reduced (0.5f), 3.0f, 1.0f);
            g.setColour (theme::textFaint);
            g.setFont (theme::uiFont (11.0f));
            g.drawFittedText ("This DSP panel is not available in the current build.",
                              section.panelArea.reduced (12, 6), juce::Justification::centred, 2);
        }
    }

private:
    struct Section
    {
        juce::String title;
        juce::Component* panel = nullptr;
        int height = 120;
        juce::Rectangle<int> labelArea, panelArea;
    };

    void addSection (const juce::String& title, juce::Component* panel, int height)
    {
        Section section;
        section.title = title;
        section.panel = panel;
        section.height = height;
        sections.push_back (section);
    }

    AppContext& ctx;
    std::vector<Section> sections;
};

//==============================================================================
class MainComponent::RackRail : public juce::Component,
                               public juce::SettableTooltipClient
{
public:
    explicit RackRail (AppContext& c) : ctx (c)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip ("Show the DSP rack");
    }

    std::function<void()> onClick;

    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient grad (theme::panelAlt, 0.0f, 0.0f,
                                   theme::panel.darker (0.15f), 0.0f, (float) getHeight(), false);
        g.setGradientFill (grad);
        g.fillAll();
        g.setColour (theme::lineStrong);
        g.drawVerticalLine (0, 0.0f, (float) getHeight());

        if (hover)
        {
            g.setColour (theme::hoverFill);
            g.fillAll();
        }

        const int top = 16;
        paintIcon (g, Icon::rack, { 7.0f, (float) top, 12.0f, 12.0f }, theme::gold, 1.3f);

        const float letterY = (float) top + 30.0f;
        g.setColour (theme::textDim);
        g.setFont (theme::capsFont (9.0f, 0.20f));
        const char* letters = "DSP";
        for (int i = 0; i < 3; ++i)
            g.drawText (juce::String::charToString ((juce::juce_wchar) letters[i]),
                        juce::Rectangle<int> (0, (int) letterY + i * 15, getWidth(), 12),
                        juce::Justification::centred, false);

        paintIcon (g, Icon::chevronRight, { 8.0f, (float) getHeight() - 24.0f, 10.0f, 10.0f },
                   hover ? theme::gold : theme::silverDark, 1.3f);

        juce::ignoreUnused (ctx);
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override
    {
        if (onClick != nullptr)
            onClick();
    }

private:
    AppContext& ctx;
    bool hover = false;
};

//==============================================================================
class MainComponent::Gutter : public juce::Component
{
public:
    Gutter()
    {
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        setWantsKeyboardFocus (false);
    }

    std::function<void (int delta)> onDrag;
    std::function<void()> onDragEnd;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::background);
        if (hover || dragging)
        {
            g.setColour (theme::gold.withAlpha (0.55f));
            g.fillRect (getWidth() / 2, 0, 1, getHeight());
        }
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override { dragging = true; lastX = 0; repaint(); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const int delta = e.x - lastX;
        lastX = e.x;
        if (delta != 0 && onDrag != nullptr)
            onDrag (delta);
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        dragging = false;
        repaint();
        if (onDragEnd != nullptr)
            onDragEnd();
    }

private:
    bool hover = false;
    bool dragging = false;
    int lastX = 0;
};

//==============================================================================
MainComponent::MainComponent (AppContext& context)
    : ctx (context),
      sidebar (context),
      browser (context),
      transport (context)
{
    auto& settings = ctx.settings();

    sidebarWidth = juce::jlimit (theme::sidebarMinWidth, theme::sidebarMaxWidth,
                                 settings.getInt ("layout.sidebarWidth", theme::sidebarWidth));
    rightPanelWidth = juce::jlimit (theme::rightPanelMinWidth, theme::rightPanelMaxWidth,
                                    settings.getInt ("layout.rightWidth", theme::rightPanelWidth));
    rightPanelVisible = settings.getBool ("layout.rightPanel", true);
    fullPlayer = browser.isFullPlayer();

    //== Embedded DSP panels (owned by ui-dsp; guarded so the shell builds alone)
#if SP_HAVE_BASS_PANEL
    bassPanel = std::make_unique<BassControlPanel> (ctx);
#endif
#if SP_HAVE_EQ_CURVE
    eqCurvePanel = std::make_unique<EqCurveComponent> (ctx, true);
#endif
#if SP_HAVE_PROFILE_PANEL
    profilePanel = std::make_unique<ProfilePanel> (ctx);
#endif
#if SP_HAVE_SPECTRUM
    spectrumPanel = std::make_unique<SpectrumComponent> (ctx);
#endif

    rackContent = std::make_unique<RackContent> (ctx, bassPanel.get(), eqCurvePanel.get(),
                                                 profilePanel.get(), spectrumPanel.get());
    rackContent->setSize (theme::rightPanelWidth - 12, rackContent->getPreferredHeight());

    //== Children ============================================================
    addAndMakeVisible (sidebar);
    addAndMakeVisible (browser);
    addAndMakeVisible (transport);
    addAndMakeVisible (rackViewport);
    addAndMakeVisible (rackCollapseButton);
    addAndMakeVisible (eqModeBadge);

    rackViewport.setViewedComponent (rackContent.get(), false);
    rackViewport.setScrollBarsShown (true, false, true, false);
    rackViewport.setScrollBarThickness (8);
    rackViewport.setColour (juce::ScrollBar::thumbColourId, theme::silverDark);

    rackRail = std::make_unique<RackRail> (ctx);
    addChildComponent (*rackRail);
    rackRail->onClick = [this] { setRightPanelVisible (true); };

    sidebarGutter = std::make_unique<Gutter>();
    addAndMakeVisible (*sidebarGutter);
    sidebarGutter->onDrag = [this] (int delta)
    {
        sidebarWidth = juce::jlimit (theme::sidebarMinWidth, theme::sidebarMaxWidth, sidebarWidth + delta);
        resized();
    };
    sidebarGutter->onDragEnd = [this] { saveLayout(); };

    rackGutter = std::make_unique<Gutter>();
    addAndMakeVisible (*rackGutter);
    rackGutter->onDrag = [this] (int delta)
    {
        rightPanelWidth = juce::jlimit (theme::rightPanelMinWidth, theme::rightPanelMaxWidth,
                                        rightPanelWidth - delta);
        resized();
    };
    rackGutter->onDragEnd = [this] { saveLayout(); };

    rackCollapseButton.setTooltip ("Collapse the DSP rack");
    rackCollapseButton.onClick = [this] { setRightPanelVisible (false); };

    sidebar.setListener (this);
    sidebar.setSelection ({ SidebarPanel::Selection::Kind::songs });

    transport.onToggleRightPanel = [this] { setRightPanelVisible (! rightPanelVisible); };
    transport.onShowSignalPath = [this] { showDiagnostics(); };
    transport.onOpenDspStudio = [this] { openDspStudio(); };
    transport.onOpenSettings = [this] { openSettings(); };
    transport.onPlayRequested = [this]
    {
        const auto status = ctx.engine().getStatus();
        if (status.state == PlaybackState::playing)
            return false;                    // playing: Play always pauses
        const auto selection = browser.getSelectedTrackIds();
        if (selection.empty() || selection.front() == status.currentTrack.id)
            return false;                    // same track: resume
        browser.playContext();               // a different song is selected
        return true;
    };

    browser.onImportFolderRequested = [this] { importFolder(); };
    browser.onImportFilesRequested = [this] { importFiles(); };
    browser.onPlaylistDeleted = [this] (PlaylistId)
    {
        sidebar.refresh (true);
        updateCommandStates();
    };
    browser.onNavigateRequested = [this] (const SidebarPanel::Selection& selection)
    {
        sidebar.setSelection (selection);
        updateCommandStates();
    };

    //== Command manager / keyboard ==========================================
    commandManager.setFirstCommandTarget (this);
    commandManager.registerAllCommandsForTarget (this);
    addKeyListener (commandManager.getKeyMappings());
    sp::GlobalShortcutMonitor::install (commandManager, *this);
    reloadKeyboardShortcuts();
    setWantsKeyboardFocus (true);

    ctx.addListener (this);

    setRightPanelVisible (rightPanelVisible);
    transport.setRightPanelVisible (rightPanelVisible);
    setFullPlayer (fullPlayer);

    updateStatusLine (true);
    updateEqModeBadge();
    updateCommandStates();
    startTimerHz (10);
}

MainComponent::~MainComponent()
{
    stopTimer();
    ctx.removeListener (this);
    sidebar.setListener (nullptr);
    sp::GlobalShortcutMonitor::uninstall();
    removeKeyListener (commandManager.getKeyMappings());
    commandManager.setFirstCommandTarget (nullptr);

    rackViewport.setViewedComponent (nullptr, false);
    rackContent.reset();
    rackRail.reset();
    sidebarGutter.reset();
    rackGutter.reset();
    saveLayout();
}

//==============================================================================
juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Playback", "View", "DSP", "Settings", "Help" };
}

juce::PopupMenu MainComponent::getMenuForIndex (int topLevelMenuIndex, const juce::String&)
{
    juce::PopupMenu menu;

    auto actionId = [] (AppAction action) { return (int) action; };

    if (topLevelMenuIndex == 0) // File
    {
        menu.addCommandItem (&commandManager, cmdImportFiles);
        menu.addCommandItem (&commandManager, cmdImportFolder);
        menu.addSeparator();
        menu.addCommandItem (&commandManager, cmdImportPlaylist);
        menu.addCommandItem (&commandManager, cmdExportPlaylist);
        menu.addSeparator();
        menu.addCommandItem (&commandManager, cmdViewHealth);
        menu.addSeparator();
        menu.addCommandItem (&commandManager, cmdQuit);
    }
    else if (topLevelMenuIndex == 1) // Playback
    {
        menu.addCommandItem (&commandManager, actionId (AppAction::playPause), "Play / Pause");
        menu.addCommandItem (&commandManager, actionId (AppAction::stop), "Stop");
        menu.addCommandItem (&commandManager, actionId (AppAction::previousTrack), "Previous Track");
        menu.addCommandItem (&commandManager, actionId (AppAction::nextTrack), "Next Track");
        menu.addSeparator();
        menu.addCommandItem (&commandManager, actionId (AppAction::seekBack30), "Back 30 Seconds");
        menu.addCommandItem (&commandManager, actionId (AppAction::seekForward30), "Forward 30 Seconds");
        menu.addSeparator();
        menu.addCommandItem (&commandManager, actionId (AppAction::volumeUp), "Volume Up");
        menu.addCommandItem (&commandManager, actionId (AppAction::volumeDown), "Volume Down");
        menu.addCommandItem (&commandManager, actionId (AppAction::mute), "Mute");
        menu.addSeparator();
        menu.addCommandItem (&commandManager, actionId (AppAction::toggleReferenceMode), "Reference Mode");
        menu.addCommandItem (&commandManager, cmdToggleAb, "A/B Compare");
        menu.addCommandItem (&commandManager, cmdToggleAbLevelMatch, "A/B Level Matching");
    }
    else if (topLevelMenuIndex == 2) // View
    {
        menu.addCommandItem (&commandManager, cmdViewSongs, "Songs");
        menu.addCommandItem (&commandManager, cmdViewAlbums, "Albums");
        menu.addSeparator();
        menu.addCommandItem (&commandManager, cmdViewArtists, "Artists");
        menu.addCommandItem (&commandManager, cmdViewGenres, "Genres");
        menu.addCommandItem (&commandManager, cmdViewYears, "Years");
        menu.addCommandItem (&commandManager, cmdViewFormats, "Formats");
        menu.addCommandItem (&commandManager, cmdViewFolders, "Folders");
        menu.addSeparator();
        menu.addCommandItem (&commandManager, cmdViewQueue, "Queue");
        menu.addCommandItem (&commandManager, cmdViewHealth, "Library Health");
        menu.addSeparator();
        menu.addCommandItem (&commandManager, cmdViewFullPlayer, "Full Player");
        menu.addCommandItem (&commandManager, cmdViewRack, "DSP Rack");
        menu.addCommandItem (&commandManager, actionId (AppAction::toggleDspStudio), "DSP Studio");
    }
    else if (topLevelMenuIndex == 3) // DSP
    {
        menu.addSectionHeader ("EQ MODE");
        menu.addCommandItem (&commandManager, cmdDspManual, "Manual");
        menu.addCommandItem (&commandManager, cmdDspAutoGenre, "Auto Genre");
        menu.addCommandItem (&commandManager, cmdDspIntelligent, "Intelligent");
        menu.addSeparator();
        menu.addCommandItem (&commandManager, cmdDspBypassEq, "Bypass EQ");
        menu.addSeparator();

        juce::PopupMenu learn;
        learn.addCommandItem (&commandManager, cmdDspLearnOff, "Off");
        learn.addCommandItem (&commandManager, cmdDspLearnGenre, "Learn by Genre");
        learn.addCommandItem (&commandManager, cmdDspLearnAlbum, "Learn by Album");
        learn.addCommandItem (&commandManager, cmdDspLearnSong, "Learn by Song");
        menu.addSubMenu ("Learn Preference", learn);
        menu.addSeparator();
        menu.addCommandItem (&commandManager, cmdDspSaveDeviceProfile, "Save Device Profile");
        menu.addCommandItem (&commandManager, cmdDspLoadDeviceProfile, "Load Device Profile");
        menu.addSeparator();
        menu.addCommandItem (&commandManager, actionId (AppAction::toggleDspStudio), "Open DSP Studio");
    }
    else if (topLevelMenuIndex == 4) // Settings
    {
        menu.addCommandItem (&commandManager, actionId (AppAction::showSettings), "Preferences...");
    }
    else if (topLevelMenuIndex == 5) // Help
    {
        menu.addCommandItem (&commandManager, cmdHelpAbout, "About Snake Puppy Reference Player");
        menu.addCommandItem (&commandManager, cmdHelpDiagnostics, "Diagnostics...");
        menu.addCommandItem (&commandManager, cmdHelpShortcuts, "Keyboard Shortcuts...");
    }

    return menu;
}

void MainComponent::menuItemSelected (int menuItemID, int)
{
    if (menuItemID != 0)
        commandManager.invokeDirectly (menuItemID, false);
}

//==============================================================================
juce::ApplicationCommandTarget* MainComponent::getNextCommandTarget()
{
    return nullptr;
}

void MainComponent::getAllCommands (juce::Array<juce::CommandID>& commands)
{
    for (int i = (int) AppAction::playPause; i < (int) AppAction::numActions; ++i)
        commands.add (i);

    commands.addArray ({ cmdImportFiles, cmdImportFolder, cmdImportPlaylist, cmdExportPlaylist,
                         cmdQuit, cmdToggleAb, cmdToggleAbLevelMatch,
                         cmdViewSongs, cmdViewAlbums, cmdViewArtists, cmdViewGenres, cmdViewYears,
                         cmdViewFormats, cmdViewFolders, cmdViewQueue, cmdViewHealth, cmdViewFullPlayer,
                         cmdViewRack,
                         cmdDspManual, cmdDspAutoGenre, cmdDspIntelligent, cmdDspBypassEq,
                         cmdDspLearnOff, cmdDspLearnGenre, cmdDspLearnAlbum, cmdDspLearnSong,
                         cmdDspSaveDeviceProfile, cmdDspLoadDeviceProfile, cmdSettingsPreferences,
                         cmdHelpAbout, cmdHelpDiagnostics, cmdHelpShortcuts });
}

void MainComponent::getCommandInfo (juce::CommandID commandID, juce::ApplicationCommandInfo& info)
{
    auto setShortcut = [this] (juce::ApplicationCommandInfo& commandInfo, AppAction action)
    {
        auto text = ctx.settings().getShortcut (action);
        if (text.isNotEmpty())
        {
            const auto keyPress = juce::KeyPress::createFromDescription (text);
            if (keyPress.getKeyCode() != 0)
                commandInfo.addDefaultKeypress (keyPress.getKeyCode(), keyPress.getModifiers());
        }
    };

    if (commandID >= (juce::CommandID) AppAction::playPause
        && commandID < (juce::CommandID) AppAction::numActions)
    {
        const auto action = (AppAction) commandID;
        info.setInfo (ctx.getActionDescription (action), ctx.getActionDescription (action), "Application", 0);
        info.setActive (true);
        setShortcut (info, action);

        const auto status = ctx.engine().getStatus();
        switch (action)
        {
            case AppAction::playPause:
                info.setTicked (status.state == PlaybackState::playing);
                break;
            case AppAction::toggleReferenceMode:
                info.setTicked (ctx.isReferenceMode());
                break;
            case AppAction::toggleEqBypass:
                info.setTicked (ctx.engine().getDspParams().eqBypass);
                break;
            case AppAction::toggleFullPlayer:
                info.setTicked (fullPlayer);
                break;
            case AppAction::showLibrary:
                info.setTicked (browser.getSelection().kind == SidebarPanel::Selection::Kind::songs);
                break;
            case AppAction::showQueue:
                info.setTicked (browser.getSelection().kind == SidebarPanel::Selection::Kind::queue);
                break;
            case AppAction::showHealth:
                info.setTicked (browser.getSelection().kind == SidebarPanel::Selection::Kind::health);
                break;
            case AppAction::showSettings:
            case AppAction::toggleDspStudio:
            case AppAction::focusSearch:
            case AppAction::addToQueue:
            case AppAction::clearQueue:
            case AppAction::showPlaylists:
                break;
            default:
                break;
        }
        return;
    }

    switch (commandID)
    {
        case cmdImportFiles:
            info.setInfo ("Import Files...", "Add audio files to the library", "File", 0);
            info.addDefaultKeypress ('o', juce::ModifierKeys::commandModifier);
            break;
        case cmdImportFolder:
            info.setInfo ("Import Folder...", "Add a folder to the library and watch it", "File", 0);
            info.addDefaultKeypress ('o', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier);
            break;
        case cmdImportPlaylist:
            info.setInfo ("Import Playlist...", "Import an M3U, M3U8 or PLS playlist", "File", 0);
            break;
        case cmdExportPlaylist:
            info.setInfo ("Export Playlist...", "Export the selected playlist", "File", 0);
            info.setActive (browser.getSelection().kind == SidebarPanel::Selection::Kind::playlist);
            break;
        case cmdQuit:
            info.setInfo ("Quit", "Quit the player", "File", 0);
            break;

        case cmdToggleAb:
            info.setInfo ("A/B Compare", "Compare the processed sound to the original", "Playback", 0);
            info.setTicked (ctx.isAbComparisonActive());
            break;
        case cmdToggleAbLevelMatch:
            info.setInfo ("A/B Level Matching", "Level-match the A/B comparison", "Playback", 0);
            info.setTicked (ctx.isAbLevelMatching());
            break;

        case cmdViewSongs:
            info.setInfo ("Songs", "Show all songs", "View", 0);
            info.setTicked (browser.getSelection().kind == SidebarPanel::Selection::Kind::songs);
            break;
        case cmdViewAlbums:
            info.setInfo ("Albums", "Show the album grid", "View", 0);
            info.setTicked (browser.getSelection().kind == SidebarPanel::Selection::Kind::albums);
            break;
        case cmdViewArtists:
            info.setInfo ("Artists", "Browse artists in the sidebar", "View", 0);
            break;
        case cmdViewGenres:
            info.setInfo ("Genres", "Browse genres in the sidebar", "View", 0);
            break;
        case cmdViewYears:
            info.setInfo ("Years", "Browse years in the sidebar", "View", 0);
            break;
        case cmdViewFormats:
            info.setInfo ("Formats", "Browse formats in the sidebar", "View", 0);
            break;
        case cmdViewFolders:
            info.setInfo ("Folders", "Browse folders in the sidebar", "View", 0);
            break;
        case cmdViewQueue:
            info.setInfo ("Queue", "Show the play queue", "View", 0);
            info.setTicked (browser.getSelection().kind == SidebarPanel::Selection::Kind::queue);
            break;
        case cmdViewHealth:
            info.setInfo ("Library Health", "Run diagnostics on the library", "View", 0);
            info.setTicked (browser.getSelection().kind == SidebarPanel::Selection::Kind::health);
            break;
        case cmdViewFullPlayer:
            info.setInfo ("Full Player", "Hide the sidebar and DSP rack", "View", 0);
            info.setTicked (fullPlayer);
            break;
        case cmdViewRack:
            info.setInfo ("DSP Rack", "Show or hide the DSP rack", "View", 0);
            info.setTicked (rightPanelVisible);
            break;

        case cmdDspManual:
            info.setInfo ("Manual", "Use only the manually edited DSP", "DSP", 0);
            info.setTicked (ctx.getEqMode() == EqMode::manual && ! ctx.isReferenceMode());
            break;
        case cmdDspAutoGenre:
            info.setInfo ("Auto Genre", "Choose a profile from the track's genre", "DSP", 0);
            info.setTicked (ctx.getEqMode() == EqMode::autoGenre && ! ctx.isReferenceMode());
            break;
        case cmdDspIntelligent:
            info.setInfo ("Intelligent", "Refine the genre profile with audio analysis", "DSP", 0);
            info.setTicked (ctx.getEqMode() == EqMode::intelligent && ! ctx.isReferenceMode());
            break;
        case cmdDspBypassEq:
            info.setInfo ("Bypass EQ", "Bypass the EQ section only", "DSP", 0);
            info.setTicked (ctx.engine().getDspParams().eqBypass);
            break;
        case cmdDspLearnOff:
            info.setInfo ("Off", "Do not learn preferences", "DSP", 0);
            info.setTicked (ctx.getLearnScope() == LearnScope::off);
            break;
        case cmdDspLearnGenre:
            info.setInfo ("Learn by Genre", "Remember edits per genre", "DSP", 0);
            info.setTicked (ctx.getLearnScope() == LearnScope::genre);
            break;
        case cmdDspLearnAlbum:
            info.setInfo ("Learn by Album", "Remember edits per album", "DSP", 0);
            info.setTicked (ctx.getLearnScope() == LearnScope::album);
            break;
        case cmdDspLearnSong:
            info.setInfo ("Learn by Song", "Remember edits per song", "DSP", 0);
            info.setTicked (ctx.getLearnScope() == LearnScope::song);
            break;
        case cmdDspSaveDeviceProfile:
            info.setInfo ("Save Device Profile", "Store the DSP settings for the current output device", "DSP", 0);
            break;
        case cmdDspLoadDeviceProfile:
            info.setInfo ("Load Device Profile", "Restore the DSP settings for the current output device", "DSP", 0);
            break;

        case cmdSettingsPreferences:
            info.setInfo ("Preferences...", "Application settings", "Settings", 0);
            break;

        case cmdHelpAbout:
            info.setInfo ("About Snake Puppy Reference Player", "About this application", "Help", 0);
            break;
        case cmdHelpDiagnostics:
            info.setInfo ("Diagnostics...", "Show engine and signal-path diagnostics", "Help", 0);
            break;
        case cmdHelpShortcuts:
            info.setInfo ("Keyboard Shortcuts...", "Show the current keyboard shortcuts", "Help", 0);
            break;

        default:
            break;
    }
}

bool MainComponent::perform (const InvocationInfo& info)
{
    bool result = false;
    juce::String error;
    if (! sp::runGuardingObjCExceptions ([this, &info, &result]
        {
            result = performCommand ((int) info.commandID);
        }, error))
    {
        ctx.setStatusMessage ("UI action failed: " + error, 8000);
        return false;
    }
    return result;
}

//==============================================================================
bool MainComponent::performCommand (int commandID)
{
    //== AppAction command ids ================================================
    if (commandID >= (int) AppAction::playPause && commandID < (int) AppAction::numActions)
    {
        const auto action = (AppAction) commandID;
        switch (action)
        {
            case AppAction::showLibrary:
                navigateTo ({ SidebarPanel::Selection::Kind::songs });
                return true;
            case AppAction::showPlaylists:
            {
                auto playlists = ctx.playlists().getSnapshot();
                if (playlists != nullptr && ! playlists->playlists.empty())
                {
                    SidebarPanel::Selection sel;
                    sel.kind = SidebarPanel::Selection::Kind::playlist;
                    sel.playlistId = playlists->playlists.front().id;
                    navigateTo (sel);
                }
                else
                {
                    ctx.setStatusMessage ("No playlists yet - create one from the + button in the sidebar.", 6000);
                }
                return true;
            }
            case AppAction::showQueue:
                navigateTo ({ SidebarPanel::Selection::Kind::queue });
                return true;
            case AppAction::showHealth:
                navigateTo ({ SidebarPanel::Selection::Kind::health });
                return true;
            case AppAction::toggleDspStudio:
                openDspStudio();
                return true;
            case AppAction::toggleFullPlayer:
                toggleFullPlayer();
                return true;
            case AppAction::showSettings:
                openSettings();
                return true;
            case AppAction::focusSearch:
                browser.focusSearch();
                return true;
            case AppAction::addToQueue:
                browser.addContextToQueue();
                return true;
            case AppAction::clearQueue:
                ctx.engine().clearQueue();
                ctx.setStatusMessage ("Queue cleared.");
                return true;
            case AppAction::rateOneStar:   browser.rateContextTrack (1); return true;
            case AppAction::rateTwoStars:  browser.rateContextTrack (2); return true;
            case AppAction::rateThreeStars: browser.rateContextTrack (3); return true;
            case AppAction::rateFourStars: browser.rateContextTrack (4); return true;
            case AppAction::rateFiveStars: browser.rateContextTrack (5); return true;
            default:
                break;
        }

        if (action == AppAction::playPause
            && ctx.engine().getStatus().state != PlaybackState::playing)
        {
            const auto selection = browser.getSelectedTrackIds();
            if (! selection.empty()
                && selection.front() != ctx.engine().getStatus().currentTrack.id)
            {
                browser.playContext();
                updateCommandStates();
                return true;
            }
        }

        if (ctx.performAction (action))
        {
            updateCommandStates();
            return true;
        }
        return false;
    }

    switch (commandID)
    {
        case cmdImportFiles:    importFiles(); return true;
        case cmdImportFolder:   importFolder(); return true;
        case cmdImportPlaylist: importPlaylist(); return true;
        case cmdExportPlaylist:
            if (browser.getSelection().kind == SidebarPanel::Selection::Kind::playlist)
                exportPlaylist (browser.getSelection().playlistId);
            else
                ctx.setStatusMessage ("Select a playlist first.", 5000);
            return true;
        case cmdQuit:
            // Never terminate synchronously from a command/key callback; AppKit
            // can throw while terminating inside event dispatch. Defer a turn.
            if (auto* app = juce::JUCEApplication::getInstance())
                juce::MessageManager::callAsync ([app] { app->systemRequestedQuit(); });
            return true;

        case cmdToggleAb:
            ctx.setAbComparisonActive (! ctx.isAbComparisonActive());
            updateCommandStates();
            return true;
        case cmdToggleAbLevelMatch:
            ctx.setAbLevelMatching (! ctx.isAbLevelMatching());
            updateCommandStates();
            return true;

        case cmdViewSongs:  navigateTo ({ SidebarPanel::Selection::Kind::songs }); return true;
        case cmdViewAlbums: navigateTo ({ SidebarPanel::Selection::Kind::albums }); return true;
        case cmdViewArtists:
            sidebar.expandSection (SidebarPanel::Selection::Kind::artists);
            ctx.setStatusMessage ("Artists listed in the sidebar - choose an artist to filter the library.", 5000);
            return true;
        case cmdViewGenres:
            sidebar.expandSection (SidebarPanel::Selection::Kind::genres);
            ctx.setStatusMessage ("Genres listed in the sidebar - choose a genre to filter the library.", 5000);
            return true;
        case cmdViewYears:
            sidebar.expandSection (SidebarPanel::Selection::Kind::years);
            ctx.setStatusMessage ("Years listed in the sidebar - choose a year to filter the library.", 5000);
            return true;
        case cmdViewFormats:
            sidebar.expandSection (SidebarPanel::Selection::Kind::formats);
            ctx.setStatusMessage ("Formats listed in the sidebar - choose a format to filter the library.", 5000);
            return true;
        case cmdViewFolders:
            sidebar.expandSection (SidebarPanel::Selection::Kind::folders);
            ctx.setStatusMessage ("Folders listed in the sidebar - choose a folder to filter the library.", 5000);
            return true;
        case cmdViewQueue:  navigateTo ({ SidebarPanel::Selection::Kind::queue }); return true;
        case cmdViewHealth: navigateTo ({ SidebarPanel::Selection::Kind::health }); return true;
        case cmdViewFullPlayer: toggleFullPlayer(); return true;
        case cmdViewRack: setRightPanelVisible (! rightPanelVisible); return true;

        case cmdDspManual:      ctx.setEqMode (EqMode::manual); updateEqModeBadge(); return true;
        case cmdDspAutoGenre:   ctx.setEqMode (EqMode::autoGenre); updateEqModeBadge(); return true;
        case cmdDspIntelligent: ctx.setEqMode (EqMode::intelligent); updateEqModeBadge(); return true;
        case cmdDspBypassEq:
        {
            auto params = ctx.engine().getDspParams();
            params.eqBypass = ! params.eqBypass;
            ctx.applyUserDspParams (params, true);
            ctx.setStatusMessage (params.eqBypass ? "EQ bypassed." : "EQ enabled.");
            updateCommandStates();
            return true;
        }
        case cmdDspLearnOff:    ctx.setLearnScope (LearnScope::off); updateCommandStates(); return true;
        case cmdDspLearnGenre:  ctx.setLearnScope (LearnScope::genre); updateCommandStates(); return true;
        case cmdDspLearnAlbum:  ctx.setLearnScope (LearnScope::album); updateCommandStates(); return true;
        case cmdDspLearnSong:   ctx.setLearnScope (LearnScope::song); updateCommandStates(); return true;
        case cmdDspSaveDeviceProfile: ctx.saveDspForCurrentDevice(); return true;
        case cmdDspLoadDeviceProfile: ctx.applyDspForCurrentDevice(); return true;

        case cmdSettingsPreferences: openSettings(); return true;

        case cmdHelpAbout:       showAbout(); return true;
        case cmdHelpDiagnostics: showDiagnostics(); return true;
        case cmdHelpShortcuts:   showShortcutHelp(); return true;

        default:
            break;
    }
    return false;
}

//==============================================================================
void MainComponent::appContextChanged (AppContext::Change change)
{
    switch (change)
    {
        case AppContext::Change::libraryChanged:
            sidebar.refresh (true);
            browser.refresh();
            updateStatusLine (true);
            updateCommandStates();
            break;
        case AppContext::Change::playlistsChanged:
            sidebar.refresh (true);
            break;
        case AppContext::Change::trackChanged:
        case AppContext::Change::playbackStateChanged:
        {
            const auto status = ctx.engine().getStatus();
            browser.setPlayingTrack (status.currentTrack.id, status.state == PlaybackState::playing);
            updateStatusLine (false);
            updateCommandStates();
            break;
        }
        case AppContext::Change::referenceModeChanged:
        case AppContext::Change::dspChanged:
        case AppContext::Change::eqModeChanged:
        case AppContext::Change::profileDecisionChanged:
            updateEqModeBadge();
            updateCommandStates();
            break;
        case AppContext::Change::queueChanged:
            sidebar.refresh (false);
            break;
        case AppContext::Change::deviceChanged:
            transport.refreshDeviceList();
            updateCommandStates();
            break;
        case AppContext::Change::statusMessageChanged:
            updateStatusLine (true);
            break;
        case AppContext::Change::settingsChanged:
            reloadKeyboardShortcuts();
            break;
        default:
            break;
    }
}

//==============================================================================
void MainComponent::sidebarSelectionChanged (const SidebarPanel::Selection& selection)
{
    browser.navigate (selection);
    updateCommandStates();
}

void MainComponent::sidebarCreatePlaylistRequested() { createPlaylist(); }
void MainComponent::sidebarCreateSmartPlaylistRequested() { createSmartPlaylist(); }
void MainComponent::sidebarImportPlaylistRequested() { importPlaylist(); }
void MainComponent::sidebarRenamePlaylistRequested (PlaylistId id) { renamePlaylist (id); }
void MainComponent::sidebarDuplicatePlaylistRequested (PlaylistId id) { duplicatePlaylist (id); }
void MainComponent::sidebarDeletePlaylistRequested (PlaylistId id) { deletePlaylist (id); }
void MainComponent::sidebarExportPlaylistRequested (PlaylistId id) { exportPlaylist (id); }
void MainComponent::sidebarPlayPlaylistRequested (PlaylistId id) { playPlaylist (id); }
void MainComponent::sidebarQueuePlaylistRequested (PlaylistId id) { queuePlaylist (id); }

void MainComponent::sidebarAddTracksToPlaylist (PlaylistId id, const std::vector<TrackId>& ids)
{
    if (ctx.playlists().addTracks (id, ids, -1))
    {
        ctx.playlists().markChanged();
        ctx.setStatusMessage ("Added " + formatCount ((int) ids.size(), "track") + " to the playlist.");
        sidebar.refresh (true);
    }
    else
    {
        ctx.setStatusMessage ("Could not add those tracks to the playlist.", 6000);
    }
}

//==============================================================================
void MainComponent::navigateTo (const SidebarPanel::Selection& selection, bool selectInSidebar)
{
    browser.navigate (selection);
    if (selectInSidebar)
        sidebar.setSelection (selection);
    updateCommandStates();
}

void MainComponent::importFiles()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Import audio files",
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       audioFilePattern());
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& chooser)
                              {
                                  auto files = chooser.getResults();
                                  if (files.isEmpty())
                                      return;
                                  juce::StringArray paths;
                                  for (auto& file : files)
                                      paths.add (file.getFullPathName());
                                  ctx.library().importFiles (paths);
                                  ctx.setStatusMessage ("Importing " + formatCount (files.size(), "file") + "...", 0);
                              });
}

void MainComponent::importFolder()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Import a music folder",
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       juce::String());
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& chooser)
                              {
                                  auto folder = chooser.getResult();
                                  if (folder == juce::File())
                                      return;
                                  ctx.library().importFolder (folder, true);
                                  ctx.setStatusMessage ("Scanning \"" + folder.getFileName() + "\"...", 0);
                              });
}

void MainComponent::importPlaylist()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Import a playlist",
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       playlistFilePattern());
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& chooser)
                              {
                                  auto file = chooser.getResult();
                                  if (file == juce::File())
                                      return;

                                  juce::String error;
                                  PlaylistId newPlaylistId = kInvalidPlaylistId;
                                  if (ctx.playlists().importPlaylistFile (file, error, &newPlaylistId))
                                  {
                                      ctx.settings().addRecentPlaylist (file.getFullPathName());
                                      ctx.playlists().markChanged();
                                      sidebar.refresh (true);
                                      ctx.setStatusMessage ("Imported playlist: " + file.getFileName());
                                      if (newPlaylistId != kInvalidPlaylistId)
                                      {
                                          SidebarPanel::Selection sel;
                                          sel.kind = SidebarPanel::Selection::Kind::playlist;
                                          sel.playlistId = newPlaylistId;
                                          navigateTo (sel);
                                      }
                                  }
                                  else
                                  {
                                      ctx.setStatusMessage ("Playlist import failed: " + error, 8000);
                                  }
                              });
}

void MainComponent::exportPlaylist (PlaylistId id)
{
    auto playlists = ctx.playlists().getSnapshot();
    const Playlist* playlist = playlists != nullptr ? playlists->find (id) : nullptr;
    if (playlist == nullptr)
        return;

    const auto suggested = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                               .getChildFile (playlist->name.retainCharacters ("abcdefghijklmnopqrstuvwxyz"
                                                                              "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_")
                                              + ".m3u");
    fileChooser = std::make_unique<juce::FileChooser> ("Export playlist", suggested, "*.m3u;*.m3u8;*.pls");
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, id] (const juce::FileChooser& chooser)
                              {
                                  auto file = chooser.getResult();
                                  if (file == juce::File())
                                      return;
                                  juce::String error;
                                  if (ctx.playlists().exportPlaylist (id, file, error))
                                      ctx.setStatusMessage ("Exported playlist to " + file.getFileName());
                                  else
                                      ctx.setStatusMessage ("Playlist export failed: " + error, 8000);
                              });
}

void MainComponent::createPlaylist()
{
    showTextInputDialog ("New Playlist", "Name for the new playlist:", "New Playlist",
                         [this] (const juce::String& name)
                         {
                             const auto id = ctx.playlists().createPlaylist (name, false);
                             if (id == kInvalidPlaylistId)
                             {
                                 ctx.setStatusMessage ("Could not create the playlist.", 6000);
                                 return;
                             }
                             ctx.playlists().markChanged();
                             sidebar.refresh (true);
                             SidebarPanel::Selection sel;
                             sel.kind = SidebarPanel::Selection::Kind::playlist;
                             sel.playlistId = id;
                             navigateTo (sel);
                             ctx.setStatusMessage ("Created playlist \"" + name + "\".");
                         });
}

void MainComponent::createSmartPlaylist()
{
    auto* window = new juce::AlertWindow ("New Smart Playlist",
                                          "Smart playlists update automatically from one rule.",
                                          juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("name", "Smart Playlist", "Name");
    window->addComboBox ("field", { "Artist", "Album", "Album Artist", "Genre", "Year", "Rating",
                                    "Format", "Title", "Comment", "Folder" }, "Field");
    window->addComboBox ("op", { "is", "is not", "contains", "does not contain",
                                 "begins with", "ends with", "greater than", "less than" }, "Operator");
    window->addTextEditor ("value", "", "Value");
    window->addComboBox ("match", { "Match all rules", "Match any rule" }, "Match");
    window->addButton ("Create", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    if (auto* combo = window->getComboBoxComponent ("field")) combo->setSelectedId (1);
    if (auto* combo = window->getComboBoxComponent ("op")) combo->setSelectedId (1);

    window->enterModalState (true,
                             juce::ModalCallbackFunction::create ([this, window] (int result)
                             {
                                 if (result != 1)
                                     return;

                                 const auto name = window->getTextEditorContents ("name").trim();
                                 if (name.isEmpty())
                                     return;

                                 const auto fieldIndex = window->getComboBoxComponent ("field") != nullptr
                                                             ? window->getComboBoxComponent ("field")->getSelectedId() - 1
                                                             : 0;
                                 const auto opIndex = window->getComboBoxComponent ("op") != nullptr
                                                          ? window->getComboBoxComponent ("op")->getSelectedId() - 1
                                                          : 0;
                                 const auto matchIndex = window->getComboBoxComponent ("match") != nullptr
                                                             ? window->getComboBoxComponent ("match")->getSelectedId() - 1
                                                             : 0;
                                 const auto value = window->getTextEditorContents ("value");

                                 static const SmartField fields[] = {
                                     SmartField::artist, SmartField::album, SmartField::albumArtist,
                                     SmartField::genre, SmartField::year, SmartField::rating,
                                     SmartField::format, SmartField::title, SmartField::comment,
                                     SmartField::folder
                                 };
                                 static const SmartOp ops[] = {
                                     SmartOp::is, SmartOp::isNot, SmartOp::contains, SmartOp::notContains,
                                     SmartOp::beginsWith, SmartOp::endsWith, SmartOp::greaterThan,
                                     SmartOp::lessThan
                                 };

                                 SmartPlaylistDefinition definition;
                                 definition.matchAll = matchIndex == 0;

                                 SmartRule rule;
                                 rule.field = fields[juce::jlimit (0, 9, fieldIndex)];
                                 rule.op = ops[juce::jlimit (0, 7, opIndex)];
                                 rule.text = value;
                                 rule.number = value.getDoubleValue();
                                 definition.rules.push_back (rule);

                                 const auto id = ctx.playlists().createPlaylist (name, true);
                                 if (id == kInvalidPlaylistId)
                                 {
                                     ctx.setStatusMessage ("Could not create the smart playlist.", 6000);
                                     return;
                                 }
                                 ctx.playlists().setSmartDefinition (id, definition);
                                 ctx.playlists().markChanged();
                                 sidebar.refresh (true);
                                 SidebarPanel::Selection sel;
                                 sel.kind = SidebarPanel::Selection::Kind::playlist;
                                 sel.playlistId = id;
                                 navigateTo (sel);
                                 ctx.setStatusMessage ("Created smart playlist \"" + name + "\".");
                             }),
                             true);
}

void MainComponent::renamePlaylist (PlaylistId id)
{
    auto playlists = ctx.playlists().getSnapshot();
    const Playlist* playlist = playlists != nullptr ? playlists->find (id) : nullptr;
    if (playlist == nullptr)
        return;

    const auto currentName = playlist->name;
    showTextInputDialog ("Rename Playlist", "New name:", currentName,
                         [this, id] (const juce::String& name)
                         {
                             if (ctx.playlists().renamePlaylist (id, name))
                             {
                                 ctx.playlists().markChanged();
                                 sidebar.refresh (true);
                                 ctx.setStatusMessage ("Renamed playlist to \"" + name + "\".");
                             }
                             else
                             {
                                 ctx.setStatusMessage ("Could not rename the playlist.", 6000);
                             }
                         });
}

void MainComponent::deletePlaylist (PlaylistId id)
{
    auto playlists = ctx.playlists().getSnapshot();
    const Playlist* playlist = playlists != nullptr ? playlists->find (id) : nullptr;
    if (playlist == nullptr)
        return;

    const auto name = playlist->name;
    showConfirmDialog ("Delete Playlist", "Delete \"" + name + "\"?\nThis cannot be undone.", "Delete",
                       [this, id]
                       {
                           if (ctx.playlists().deletePlaylist (id))
                           {
                               ctx.playlists().markChanged();
                               sidebar.refresh (true);
                               ctx.setStatusMessage ("Deleted playlist.");
                               if (browser.getSelection().kind == SidebarPanel::Selection::Kind::playlist
                                   && browser.getSelection().playlistId == id)
                                   browser.showSongs();
                           }
                           else
                           {
                               ctx.setStatusMessage ("Could not delete the playlist.", 6000);
                           }
                       });
}

void MainComponent::duplicatePlaylist (PlaylistId id)
{
    const auto newId = ctx.playlists().duplicatePlaylist (id);
    if (newId == kInvalidPlaylistId)
    {
        ctx.setStatusMessage ("Could not duplicate the playlist.", 6000);
        return;
    }
    ctx.playlists().markChanged();
    sidebar.refresh (true);
    SidebarPanel::Selection sel;
    sel.kind = SidebarPanel::Selection::Kind::playlist;
    sel.playlistId = newId;
    navigateTo (sel);
    ctx.setStatusMessage ("Duplicated playlist.");
}

void MainComponent::playPlaylist (PlaylistId id)
{
    auto tracks = ctx.playlists().resolveTracks (id);
    if (tracks.empty())
    {
        ctx.setStatusMessage ("This playlist has no playable tracks.", 5000);
        return;
    }
    ctx.playTracks (tracks, 0);
    ctx.setStatusMessage ("Playing " + formatCount ((int) tracks.size(), "track") + " from the playlist.");
}

void MainComponent::queuePlaylist (PlaylistId id)
{
    auto tracks = ctx.playlists().resolveTracks (id);
    if (tracks.empty())
    {
        ctx.setStatusMessage ("This playlist has no playable tracks.", 5000);
        return;
    }
    ctx.engine().enqueueAfterCurrent (tracks);
    ctx.setStatusMessage ("Added " + formatCount ((int) tracks.size(), "track") + " to the queue.");
}

//==============================================================================
bool MainComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
    {
        const juce::File file (path);
        if (file.isDirectory())
            return true;

        const auto extension = file.getFileExtension().toLowerCase();
        if (extension == ".wav" || extension == ".aif" || extension == ".aiff" || extension == ".flac"
            || extension == ".m4a" || extension == ".alac" || extension == ".mp3" || extension == ".ogg"
            || extension == ".opus" || extension == ".dsf" || extension == ".dff" || extension == ".ape"
            || extension == ".wv" || extension == ".aac" || extension == ".caf"
            || extension == ".m3u" || extension == ".m3u8" || extension == ".pls")
            return true;
    }
    return false;
}

void MainComponent::fileDragEnter (const juce::StringArray&, int, int)
{
    fileDragActive = true;
    repaint();
}

void MainComponent::fileDragExit (const juce::StringArray&)
{
    fileDragActive = false;
    repaint();
}

void MainComponent::filesDropped (const juce::StringArray& files, int, int)
{
    fileDragActive = false;
    repaint();

    juce::StringArray audioFiles;
    juce::StringArray playlistFiles;

    for (const auto& path : files)
    {
        const juce::File file (path);
        if (file.isDirectory())
        {
            ctx.library().importFolder (file, true);
            ctx.setStatusMessage ("Scanning \"" + file.getFileName() + "\"...", 0);
            continue;
        }

        const auto extension = file.getFileExtension().toLowerCase();
        if (extension == ".m3u" || extension == ".m3u8" || extension == ".pls")
            playlistFiles.add (path);
        else if (isInterestedInFileDrag ({ path }))
            audioFiles.add (path);
    }

    if (! audioFiles.isEmpty())
    {
        ctx.library().importFiles (audioFiles);
        ctx.setStatusMessage ("Importing " + formatCount (audioFiles.size(), "file") + "...", 0);
    }

    for (const auto& path : playlistFiles)
    {
        juce::String error;
        PlaylistId newId = kInvalidPlaylistId;
        if (ctx.playlists().importPlaylistFile (juce::File (path), error, &newId))
        {
            ctx.playlists().markChanged();
            sidebar.refresh (true);
            SidebarPanel::Selection sel;
            sel.kind = SidebarPanel::Selection::Kind::playlist;
            sel.playlistId = newId;
            navigateTo (sel);
            ctx.setStatusMessage ("Imported playlist: " + juce::File (path).getFileNameWithoutExtension());
        }
        else
        {
            ctx.setStatusMessage ("Playlist import failed: " + error, 10000);
        }
    }
}

//==============================================================================
void MainComponent::setRightPanelVisible (bool visible)
{
    if (rightPanelVisible == visible)
    {
        rackCollapseButton.setVisible (visible);
        return;
    }

    rightPanelVisible = visible;
    rackCollapseButton.setVisible (visible);
    transport.setRightPanelVisible (visible);
    resized();
    repaint();
    saveLayout();
    updateCommandStates();
}

void MainComponent::setFullPlayer (bool full)
{
    fullPlayer = full;
    browser.setFullPlayer (full);
    resized();
    repaint();
}

void MainComponent::toggleFullPlayer()
{
    setFullPlayer (! fullPlayer);
    saveLayout();
    updateCommandStates();
}

void MainComponent::saveLayout()
{
    auto& settings = ctx.settings();
    settings.set ("layout.sidebarWidth", sidebarWidth);
    settings.set ("layout.rightWidth", rightPanelWidth);
    settings.set ("layout.rightPanel", rightPanelVisible);
    settings.set ("layout.fullPlayer", fullPlayer);
    settings.saveIfDirty();
}

void MainComponent::showView (const juce::String& view, PlaylistId playlistId)
{
    SidebarPanel::Selection selection;
    if (view == "albums")
        selection.kind = SidebarPanel::Selection::Kind::albums;
    else if (view == "queue")
        selection.kind = SidebarPanel::Selection::Kind::queue;
    else if (view == "health")
        selection.kind = SidebarPanel::Selection::Kind::health;
    else if (view == "playlist")
    {
        selection.kind = SidebarPanel::Selection::Kind::playlist;
        selection.playlistId = playlistId;
        if (selection.playlistId == kInvalidPlaylistId)
        {
            auto playlists = ctx.playlists().getSnapshot();
            if (playlists != nullptr && ! playlists->playlists.empty())
                selection.playlistId = playlists->playlists.back().id;
        }
    }
    else
        selection.kind = SidebarPanel::Selection::Kind::songs;

    navigateTo (selection);
}

void MainComponent::openDspStudio()
{
#if SP_HAVE_DSP_STUDIO_WINDOW
    DspStudioWindow::show (ctx);
#else
    ctx.setStatusMessage ("DSP Studio is not available in this build yet.", 6000);
#endif
}

void MainComponent::openSettings()
{
#if SP_HAVE_SETTINGS_WINDOW
    SettingsWindow::show (ctx);
#else
    ctx.setStatusMessage ("Settings are not available in this build yet.", 6000);
#endif
}

//==============================================================================
void MainComponent::reloadKeyboardShortcuts()
{
    auto shortcuts = ctx.settings().getAllShortcuts();
    const auto signature = shortcutSignature (shortcuts);
    if (signature == lastShortcutSignature)
        return;

    lastShortcutSignature = signature;

    if (auto* mappings = commandManager.getKeyMappings())
    {
        mappings->clearAllKeyPresses();
        for (auto& entry : shortcuts)
        {
            if (entry.first == AppAction::none || entry.second.isEmpty())
                continue;

            const auto keyPress = juce::KeyPress::createFromDescription (entry.second);
            if (keyPress.getKeyCode() != 0)
                mappings->addKeyPress ((juce::CommandID) (int) entry.first, keyPress);
        }
    }

    commandManager.commandStatusChanged();
    menuItemsChanged();
}

void MainComponent::updateCommandStates()
{
    commandManager.commandStatusChanged();
    menuItemsChanged();
}

void MainComponent::updateEqModeBadge()
{
    juce::String text;
    juce::Colour colour = theme::silverDim;

    if (ctx.isReferenceMode())
    {
        text = "REFERENCE";
        colour = theme::goldBright;
    }
    else
    {
        switch (ctx.getEqMode())
        {
            case EqMode::manual:      text = "MANUAL";      colour = theme::silverDim; break;
            case EqMode::autoGenre:   text = "AUTO GENRE";  colour = theme::gold; break;
            case EqMode::intelligent: text = "INTELLIGENT"; colour = theme::goldBright; break;
            case EqMode::reference:   text = "REFERENCE";   colour = theme::goldBright; break;
        }
    }

    if (eqModeBadge.getText() != text)
    {
        eqModeBadge.setText (text);
        eqModeBadge.setBadgeColour (colour);
        eqModeBadge.setSize (eqModeBadge.getPreferredWidth(), 17);
        resized();
    }
}

void MainComponent::updateStatusLine (bool force)
{
    const auto status = ctx.engine().getStatus();

    juce::String left;
    if (status.errorMessage.isNotEmpty())
        left = "Audio: " + status.errorMessage;
    else if (ctx.getStatusMessage().isNotEmpty())
        left = ctx.getStatusMessage();
    else if (ctx.library().isScanning())
        left = "Scanning library...";
    else if (browser.getSelection().kind == SidebarPanel::Selection::Kind::songs && ctx.library().getSnapshot() != nullptr
             && ctx.library().getSnapshot()->tracks.empty())
        left = "Library empty - File > Import Folder...";
    else
        left = "Ready";

    const auto snapshot = ctx.library().getSnapshot();
    const int trackCount = snapshot != nullptr ? (int) snapshot->tracks.size() : 0;
    const int queueCount = status.queueSize;

    juce::String right;
    right << trackCount << " tracks   |   queue " << queueCount;
    if (status.signalPath.outputSampleRate > 0)
        right << "   |   " << formatSampleRate (status.signalPath.outputSampleRate);

    if (! force && left == statusLeft && right == statusRight)
        return;

    statusLeft = left;
    statusRight = right;
    repaint (statusArea);
}

void MainComponent::showAbout()
{
    juce::String text;
    text << "Snake Puppy Reference Player\n"
         << "Version " << juce::JUCEApplication::getInstance()->getApplicationVersion() << "\n\n"
         << "Audiophile reference player with a real-time 64-bit DSP chain, "
         << "genre-aware EQ profiles and honest signal-path reporting.\n\n"
         << "Built with JUCE " << juce::SystemStats::getJUCEVersion() << "\n"
         << "(c) Snake Puppy Audio";

    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                           "About Snake Puppy Reference Player", text, "Close");
}

void MainComponent::showDiagnostics()
{
    juce::String text = ctx.getDiagnosticsText();
    text << "\nLibrary: " << (ctx.library().getSnapshot() != nullptr
                                 ? (int) ctx.library().getSnapshot()->tracks.size()
                                 : 0)
         << " tracks\n";
    text << "Queue: " << (int) ctx.getQueue().size() << " tracks\n";

    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                           "Diagnostics", text, "Close");
}

void MainComponent::showShortcutHelp()
{
    juce::String text;
    for (auto& entry : ctx.settings().getAllShortcuts())
    {
        if (entry.first == AppAction::none)
            continue;
        text << ctx.getActionDescription (entry.first) << "    "
             << entry.second << "\n";
    }

    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                           "Keyboard Shortcuts", text, "Close");
}

//==============================================================================
void MainComponent::timerCallback()
{
    updateStatusLine (false);

    if (++tickCounter >= 20)
    {
        tickCounter = 0;
        reloadKeyboardShortcuts();
        updateEqModeBadge();

        // Keep the sidebar counts honest for external changes.
        sidebar.refresh (false);
    }
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    if (fileDragActive)
    {
        g.setColour (theme::gold.withAlpha (0.65f));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (3.0f), 4.0f, 2.0f);
        g.setColour (theme::gold);
        g.setFont (theme::capsFont (12.0f, 0.14f));
        g.drawFittedText ("DROP MUSIC TO IMPORT", getLocalBounds().removeFromTop (getHeight() / 2 + 20).removeFromBottom (30),
                          juce::Justification::centred, 1);
    }

    //== Status line =========================================================
    if (! statusArea.isEmpty())
    {
        juce::Rectangle<int> area = statusArea;
        g.setColour (theme::bgDeep);
        g.fillRect (area);
        g.setColour (theme::line);
        g.drawHorizontalLine (area.getY(), 0.0f, (float) area.getWidth());

        auto status = ctx.engine().getStatus();
        const bool isError = status.errorMessage.isNotEmpty();

        if (isError)
            paintIcon (g, Icon::warning, { (float) area.getX() + 10.0f, (float) area.getCentreY() - 7.0f, 14.0f, 14.0f },
                       theme::error, 1.4f);

        g.setColour (isError ? theme::error : statusLeft == "Ready" ? theme::textFaint : theme::textDim);
        g.setFont (theme::uiFont (11.0f));
        g.drawFittedText (statusLeft, area.withTrimmedLeft (isError ? 32 : 12).withTrimmedRight (area.getWidth() / 2),
                          juce::Justification::centredLeft, 1);

        g.setColour (theme::textFaint);
        g.setFont (theme::monoFont (9.5f));
        g.drawFittedText (statusRight, area.withTrimmedRight (12), juce::Justification::centredRight, 1);
    }

    //== Rack header =========================================================
    if (! rackHeaderArea.isEmpty() && rightPanelVisible && ! fullPlayer)
    {
        juce::ColourGradient grad (theme::panelAlt.brighter (0.01f), 0.0f, (float) rackHeaderArea.getY(),
                                   theme::panel.darker (0.12f), 0.0f, (float) rackHeaderArea.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRect (rackHeaderArea);
        g.setColour (theme::line);
        g.drawHorizontalLine (rackHeaderArea.getBottom() - 1, (float) rackHeaderArea.getX(),
                              (float) rackHeaderArea.getRight());
        g.setColour (theme::silver);
        g.setFont (theme::capsFont (10.5f, 0.16f));
        g.drawText ("DSP RACK", rackHeaderArea.withTrimmedLeft (14), juce::Justification::centredLeft, false);
    }
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    statusArea = area.removeFromBottom (theme::statusHeight);
    transport.setBounds (area.removeFromBottom (theme::transportHeight));

    sidebar.setVisible (! fullPlayer);
    sidebarGutter->setVisible (! fullPlayer);
    rackGutter->setVisible (rightPanelVisible && ! fullPlayer);
    rackCollapseButton.setVisible (rightPanelVisible && ! fullPlayer);
    eqModeBadge.setVisible (rightPanelVisible && ! fullPlayer);
    rackRail->setVisible (! rightPanelVisible && ! fullPlayer);

    if (fullPlayer)
    {
        rackHeaderArea = {};
        railArea = {};
        rackViewport.setVisible (false);
        browser.setBounds (area);
        return;
    }

    area.removeFromLeft (0);
    sidebar.setBounds (area.removeFromLeft (sidebarWidth));
    sidebarGutter->setBounds (area.removeFromLeft (5));

    if (rightPanelVisible)
    {
        auto rack = area.removeFromRight (rightPanelWidth);
        rackGutter->setBounds (area.removeFromRight (5));
        rackHeaderArea = rack.removeFromTop (34);
        rackCollapseButton.setBounds (rackHeaderArea.getRight() - 34, rackHeaderArea.getY() + 3, 28, 28);
        eqModeBadge.setBounds (rackHeaderArea.getRight() - 34 - 12 - eqModeBadge.getPreferredWidth(),
                               rackHeaderArea.getY() + 8, eqModeBadge.getPreferredWidth(), 17);
        rackViewport.setVisible (true);
        rackViewport.setBounds (rack);
        railArea = {};
        if (rackContent != nullptr)
        {
            const int width = juce::jmax (200, rackViewport.getMaximumVisibleWidth());
            rackContent->setSize (width, juce::jmax (rackViewport.getHeight(), rackContent->getPreferredHeight()));
        }
    }
    else
    {
        rackHeaderArea = {};
        rackViewport.setVisible (false);
        railArea = area.removeFromRight (theme::rightRailWidth);
        rackRail->setBounds (railArea);
    }

    browser.setBounds (area);
}

} // namespace sp
