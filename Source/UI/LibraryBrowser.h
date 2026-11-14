/*
    Snake Puppy Reference Player - central library browser.

    Hosts the four centre pages: the library (now-playing header + search /
    filter toolbar + sortable track list or album grid), the playlist contents,
    the play queue and library health. Owns the artwork cache used by the header
    and the album grid.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/ArtworkPanel.h"
#include "UI/HealthPanel.h"
#include "UI/PlaylistPanel.h"
#include "UI/QueuePanel.h"
#include "UI/SidebarPanel.h"
#include "UI/TrackTableComponent.h"
#include "UI/Widgets.h"

namespace sp
{

class LibraryBrowser : public juce::Component,
                       private juce::Timer,
                       private TrackTableComponent::Listener
{
public:
    explicit LibraryBrowser (AppContext&);
    ~LibraryBrowser() override;

    //== Navigation ===========================================================
    void navigate (const SidebarPanel::Selection&);
    const SidebarPanel::Selection& getSelection() const { return selection; }
    void showSongs();
    void showAlbums();
    void showPlaylist (PlaylistId);
    void showQueue();
    void showHealth();
    void focusSearch();

    //== Layout ===============================================================
    void setFullPlayer (bool full);
    bool isFullPlayer() const { return fullPlayer; }
    void setViewMode (bool grid);
    bool isGridView() const { return viewMode == ViewMode::grid; }
    void refresh();
    void setPlayingTrack (TrackId, bool playing);

    /** Selected track, else the currently playing track, else the hero track. */
    TrackId getContextTrackId() const;
    std::vector<TrackId> getSelectedTrackIds() const;
    void rateContextTrack (int rating);
    void addContextToQueue();
    void playContext();

    std::function<void()> onImportFolderRequested;
    std::function<void()> onImportFilesRequested;
    std::function<void (PlaylistId)> onPlaylistDeleted;
    /** Fired when the browser itself changes navigation (e.g. clears a filter),
        so the shell can keep the sidebar in sync. */
    std::function<void (const SidebarPanel::Selection&)> onNavigateRequested;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    //== TrackTableComponent::Listener =======================================
    void trackSelectionChanged (TrackTableComponent&) override;
    void trackDoubleClicked (TrackId) override;
    void trackSortChanged (TrackTableComponent::Column, bool descending) override;
    void tracksDroppedOnRow (const std::vector<TrackId>&, int) override {}
    void tracksAddToQueue (const std::vector<TrackId>&) override;
    void tracksRemoveRequested (const std::vector<TrackId>&) override;
    void trackRatingChanged (TrackId, int) override;

    void timerCallback() override;
    void removeSelectedTracks();
    void clearLibraryContents();

    struct AlbumItem
    {
        juce::String name, artist, artistKey, artworkId;
        int year = 0;
        int trackCount = 0;
        std::int64_t durationMs = 0;
        Track representative;
    };

    enum class ViewMode { list, grid };
    enum class Page { library, playlist, queue, health };

    //== Data =================================================================
    void rebuildFilteredList();
    void rebuildAlbums();
    void rebuildTableSource();
    void updateToolbar();
    void updateHeader();
    void updatePageVisibility();
    void applyViewMode();
    void playAll();
    void addAllToQueue();
    void playAlbum (int albumIndex);
    std::vector<Track> collectVisibleTracks() const;
    bool trackMatchesSelection (const Track&) const;
    bool trackMatchesSearch (const Track&) const;
    void updateHeroFromTrack (const Track&);
    void saveViewPreferences();

    AppContext& ctx;
    ArtworkCache artworkCache;

    std::shared_ptr<const LibrarySnapshot> snapshot;
    std::uint64_t snapshotVersion = 0;
    SidebarPanel::Selection selection;
    Page page = Page::library;
    ViewMode viewMode = ViewMode::list;
    bool fullPlayer = false;
    juce::String searchText;
    juce::String filterChipText;

    std::vector<int> filteredIndices;
    std::vector<AlbumItem> albums;
    int selectedAlbumIndex = -1;

    TrackId heroTrackId = kInvalidTrackId;
    TrackId playingTrackId = kInvalidTrackId;
    bool playing = false;
    Track heroTrack;
    juce::String heroArtworkKey;

    //== Components ===========================================================
    ArtworkPanel heroArtwork;
    SpLabel heroTitle, heroArtist, heroAlbum, heroTechnical;
    SpBadge heroQuality;
    SpButton playAllButton { "Play All", SpButton::Style::primary };
    SpButton queueButton { "Add to Queue", SpButton::Style::standard };
    SpButton importFolderButton { "Import Music Folder...", SpButton::Style::primary };
    SpButton importFilesButton { "Import Files...", SpButton::Style::ghost };
    SpButton clearFilterButton { "Clear filter", SpButton::Style::ghost };
    SpSearchField searchField;
    SpButton removeTracksButton { "REMOVE", SpButton::Style::standard };
    SpButton clearLibraryButton { "CLEAR LIBRARY", SpButton::Style::standard };
    SpIconButton listViewButton { Icon::list, "List view" };
    SpIconButton gridViewButton { Icon::grid, "Album grid" };
    SpLabel countLabel;

    TrackTableComponent table;
    class AlbumGrid;
    std::unique_ptr<AlbumGrid> albumGrid;
    juce::Viewport albumViewport;

    std::unique_ptr<PlaylistPanel> playlistPanel;
    std::unique_ptr<QueuePanel> queuePanel;
    std::unique_ptr<HealthPanel> healthPanel;

    juce::Rectangle<int> headerArea, toolbarArea, contentArea, emptyStateArea;
    bool libraryEmpty = false;
    bool filterActive = false;
    bool heroQualityActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryBrowser)
};

} // namespace sp
