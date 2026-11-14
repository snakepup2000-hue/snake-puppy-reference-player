/*
    Snake Puppy Reference Player - left navigation sidebar.

    Library dimensions (songs / albums / artists / genres / years / formats /
    folders tree), playlists with create/rename/delete/import/export, and pinned
    Queue + Library Health entries. Playlist rows accept dragged tracks.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/Widgets.h"
#include <set>

namespace sp
{

class SidebarPanel : public juce::Component,
                     public juce::DragAndDropTarget,
                     private juce::Timer,
                     private juce::ChangeListener
{
public:
    struct Selection
    {
        enum class Kind { songs = 0, albums, artists, genres, years, formats, folders, playlist, queue, health };

        Kind kind = Kind::songs;
        juce::String value;                 // artist / genre / year / format / folder path
        PlaylistId playlistId = kInvalidPlaylistId;

        bool operator== (const Selection& o) const
        {
            return kind == o.kind && value == o.value && playlistId == o.playlistId;
        }
        bool operator!= (const Selection& o) const { return ! (*this == o); }
    };

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void sidebarSelectionChanged (const Selection&) {}
        virtual void sidebarCreatePlaylistRequested() {}
        virtual void sidebarCreateSmartPlaylistRequested() {}
        virtual void sidebarImportPlaylistRequested() {}
        virtual void sidebarRenamePlaylistRequested (PlaylistId) {}
        virtual void sidebarDuplicatePlaylistRequested (PlaylistId) {}
        virtual void sidebarDeletePlaylistRequested (PlaylistId) {}
        virtual void sidebarExportPlaylistRequested (PlaylistId) {}
        virtual void sidebarPlayPlaylistRequested (PlaylistId) {}
        virtual void sidebarQueuePlaylistRequested (PlaylistId) {}
        virtual void sidebarAddTracksToPlaylist (PlaylistId, const std::vector<TrackId>&) {}
    };

    explicit SidebarPanel (AppContext&);
    ~SidebarPanel() override;

    /** Rebuilds counts and playlist rows from the current snapshots. */
    void refresh (bool force = false);

    void setSelection (const Selection&, bool scrollIntoView = true);
    void expandSection (Selection::Kind);
    void collapseAllSections();
    Selection getSelection() const { return selection; }

    void setListener (Listener*);
    int getPreferredWidth() const { return theme::sidebarWidth; }

    //== Drag and drop target =================================================
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    //== Timer / broadcaster ==================================================
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

private:
    class Content;
    friend class Content;

    struct CategoryEntry
    {
        juce::String value;
        int count = 0;
    };

    struct FolderEntry
    {
        juce::String path;
        juce::String label;
        int depth = 0;
        int count = 0;
    };

    struct Row
    {
        enum class Type { section, nav, child, playlist, hint };

        Type type = Type::nav;
        Selection selection;
        juce::String label;
        juce::String detail;
        Icon icon = Icon::music;
        bool expandable = false;
        bool expanded = false;
        int indent = 0;
        int height = 30;
        juce::Rectangle<int> bounds;
        PlaylistId playlistId = kInvalidPlaylistId;
        juce::String sectionName;
    };

    void rebuildAggregates();
    void rebuildRows();
    void layoutRows();
    void updateContent();
    void toggleExpanded (const Selection&);
    void showPlaylistMenu (PlaylistId);
    void showPlaylistsSectionMenu();
    void paintFooter (juce::Graphics&);
    int rowAtY (int y) const;
    void selectRow (const Row&);
    juce::Rectangle<int> getPlusButtonBounds() const;
    bool isExpandedFor (Selection::Kind) const;

    AppContext& ctx;
    Listener* listener = nullptr;
    Selection selection;

    std::shared_ptr<const LibrarySnapshot> snapshot;
    std::shared_ptr<const PlaylistSnapshot> playlistsSnapshot;
    std::uint64_t libraryVersion = 0;
    std::uint64_t playlistVersion = 0;

    std::vector<CategoryEntry> artists, genres, formats;
    std::vector<int> years;
    std::vector<FolderEntry> folders;
    int totalTracks = 0;
    int albumCount = 0;
    int queueCount = 0;

    bool artistsExpanded = false, genresExpanded = false, yearsExpanded = false,
         formatsExpanded = false, foldersExpanded = false;
    std::set<juce::String> expandedFolders;

    std::vector<Row> rows;
    int contentHeight = 0;
    int hoverRow = -1;
    int dragHoverRow = -1;
    juce::Rectangle<int> brandArea;
    juce::Rectangle<int> footerArea;
    juce::Rectangle<int> queueRowArea, healthRowArea;

    juce::Viewport viewport;
    std::unique_ptr<Content> content;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SidebarPanel)
};

} // namespace sp
