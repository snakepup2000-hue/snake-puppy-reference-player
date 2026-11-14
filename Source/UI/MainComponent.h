/*
    Snake Puppy Reference Player - application shell.

    Owns the menu model, command/keyboard mapping, the layout (sidebar, browser,
    DSP rack, transport, status line), the UI wiring for AppContext actions and
    the persistence of layout preferences.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/LibraryBrowser.h"
#include "UI/SidebarPanel.h"
#include "UI/TransportBar.h"
#include "UI/Widgets.h"

namespace sp
{

class MainComponent : public juce::Component,
                      public juce::MenuBarModel,
                      public juce::ApplicationCommandTarget,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer,
                      private AppContext::Listener,
                      private SidebarPanel::Listener
{
public:
    explicit MainComponent (AppContext&);
    ~MainComponent() override;

    //== MenuBarModel ========================================================
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int topLevelMenuIndex, const juce::String& menuName) override;
    void menuItemSelected (int menuItemID, int topLevelMenuIndex) override;

    //== ApplicationCommandTarget ============================================
    juce::ApplicationCommandTarget* getNextCommandTarget() override;
    void getAllCommands (juce::Array<juce::CommandID>&) override;
    void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override;
    bool perform (const InvocationInfo&) override;

    //== AppContext::Listener ================================================
    void appContextChanged (AppContext::Change) override;

    //== Layout ==============================================================
    void setFullPlayer (bool full);
    bool isFullPlayer() const { return fullPlayer; }
    void toggleFullPlayer();
    void setRightPanelVisible (bool visible);
    bool isRightPanelVisible() const { return rightPanelVisible; }
    void saveLayout();
    void openDspStudio();
    void openSettings();

    /** Navigate the centre view (used by startup/automation hooks):
        "songs", "albums", "queue", "health" or "playlist". */
    void showView (const juce::String& view, PlaylistId playlistId = kInvalidPlaylistId);

    juce::ApplicationCommandManager& getCommandManager() { return commandManager; }

    //== FileDragAndDropTarget ===============================================
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class RackContent;
    class RackRail;
    class Gutter;

    //== SidebarPanel::Listener ==============================================
    void sidebarSelectionChanged (const SidebarPanel::Selection&) override;
    void sidebarCreatePlaylistRequested() override;
    void sidebarCreateSmartPlaylistRequested() override;
    void sidebarImportPlaylistRequested() override;
    void sidebarRenamePlaylistRequested (PlaylistId) override;
    void sidebarDuplicatePlaylistRequested (PlaylistId) override;
    void sidebarDeletePlaylistRequested (PlaylistId) override;
    void sidebarExportPlaylistRequested (PlaylistId) override;
    void sidebarPlayPlaylistRequested (PlaylistId) override;
    void sidebarQueuePlaylistRequested (PlaylistId) override;
    void sidebarAddTracksToPlaylist (PlaylistId, const std::vector<TrackId>&) override;

    //== Wiring helpers ======================================================
    void navigateTo (const SidebarPanel::Selection&, bool selectInSidebar = true);
    void importFiles();
    void importFolder();
    void importPlaylist();
    void exportPlaylist (PlaylistId);
    void createPlaylist();
    void createSmartPlaylist();
    void renamePlaylist (PlaylistId);
    void deletePlaylist (PlaylistId);
    void duplicatePlaylist (PlaylistId);
    void playPlaylist (PlaylistId);
    void queuePlaylist (PlaylistId);
    void showAbout();
    void showDiagnostics();
    void showShortcutHelp();
    void reloadKeyboardShortcuts();
    void updateStatusLine (bool force = false);
    void updateCommandStates();
    void updateEqModeBadge();

    //== Commands ============================================================
    bool performCommand (int commandID);

    void timerCallback() override;

    AppContext& ctx;

    SidebarPanel sidebar;
    LibraryBrowser browser;
    TransportBar transport;

    //== DSP rack ============================================================
    std::unique_ptr<juce::Component> bassPanel, eqCurvePanel, profilePanel, spectrumPanel;
    std::unique_ptr<RackContent> rackContent;
    std::unique_ptr<RackRail> rackRail;
    juce::Viewport rackViewport;
    SpIconButton rackCollapseButton { Icon::chevronRight, "Collapse the DSP rack" };
    SpBadge eqModeBadge;

    std::unique_ptr<Gutter> sidebarGutter;
    std::unique_ptr<Gutter> rackGutter;

    juce::ApplicationCommandManager commandManager;
    juce::String lastShortcutSignature;
    int tickCounter = 0;
    juce::String statusLeft, statusRight;
    std::unique_ptr<juce::FileChooser> fileChooser;

    int sidebarWidth = theme::sidebarWidth;
    int rightPanelWidth = theme::rightPanelWidth;
    bool rightPanelVisible = true;
    bool fullPlayer = false;
    bool fileDragActive = false;

    juce::Rectangle<int> statusArea, rackHeaderArea, railArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace sp
