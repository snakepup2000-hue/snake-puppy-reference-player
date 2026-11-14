/*
    Snake Puppy Reference Player - central library browser implementation.
*/
#include "UI/LibraryBrowser.h"

#include <algorithm>
#include <map>

namespace sp
{

namespace
{
float measureText (const juce::Font& font, const juce::String& text)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText (font, text, 0.0f, 0.0f);
    return ga.getBoundingBox (0, -1, true).getWidth();
}

juce::String numberWithCommas (int value)
{
    juce::String s (value);
    juce::String out;
    int count = 0;
    for (int i = s.length() - 1; i >= 0; --i)
    {
        out = juce::String::charToString (s[i]) + out;
        if (++count % 3 == 0 && i > 0)
            out = "," + out;
    }
    return out;
}

juce::String kindLabel (SidebarPanel::Selection::Kind kind)
{
    switch (kind)
    {
        case SidebarPanel::Selection::Kind::artists: return "Artist";
        case SidebarPanel::Selection::Kind::genres:  return "Genre";
        case SidebarPanel::Selection::Kind::years:   return "Year";
        case SidebarPanel::Selection::Kind::formats: return "Format";
        case SidebarPanel::Selection::Kind::folders: return "Folder";
        default: return {};
    }
}
} // namespace

//==============================================================================
class LibraryBrowser::AlbumGrid : public juce::Component
{
public:
    static constexpr int tileWidth = 176;
    static constexpr int tileHeight = 224;
    static constexpr int artSize = 158;
    static constexpr int gap = 14;
    static constexpr int padding = 20;

    explicit AlbumGrid (LibraryBrowser& o) : owner (o)
    {
        setWantsKeyboardFocus (false);
    }

    void rebuildContent()
    {
        const int parentWidth = getParentComponent() != nullptr ? getParentComponent()->getWidth() : 600;
        const int usable = juce::jmax (tileWidth, parentWidth - padding * 2);
        columns = juce::jmax (1, (usable + gap) / (tileWidth + gap));
        const int rows = ((int) owner.albums.size() + columns - 1) / columns;
        setSize (juce::jmax (parentWidth, 200), juce::jmax (1, rows * (tileHeight + gap) + padding * 2));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::background);

        if (owner.albums.empty())
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::uiFont (14.0f));
            g.drawFittedText ("No albums in this view", getLocalBounds().withHeight (120).translated (0, 40),
                              juce::Justification::centred, 1);
            return;
        }

        const int totalWidth = columns * tileWidth + (columns - 1) * gap;
        const int startX = juce::jmax (padding, (getWidth() - totalWidth) / 2);

        for (int i = 0; i < (int) owner.albums.size(); ++i)
        {
            const int col = i % columns;
            const int row = i / columns;
            juce::Rectangle<int> tile (startX + col * (tileWidth + gap), padding + row * (tileHeight + gap),
                                       tileWidth, tileHeight);
            paintTile (g, i, tile, i == hoverTile, i == owner.selectedAlbumIndex);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int tile = tileAt (e.getPosition());
        if (tile != hoverTile)
        {
            hoverTile = tile;
            repaint();
        }
        setMouseCursor (tile >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverTile >= 0)
        {
            hoverTile = -1;
            repaint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int tile = tileAt (e.getPosition());
        if (tile < 0)
            return;

        owner.selectedAlbumIndex = tile;
        if (tile < (int) owner.albums.size())
            owner.updateHeroFromTrack (owner.albums[(size_t) tile].representative);
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int tile = tileAt (e.getPosition());
        if (tile >= 0)
            owner.playAlbum (tile);
    }

private:
    int tileAt (juce::Point<int> point) const
    {
        if (owner.albums.empty())
            return -1;

        const int totalWidth = columns * tileWidth + (columns - 1) * gap;
        const int startX = juce::jmax (padding, (getWidth() - totalWidth) / 2);
        const int localX = point.x - startX;
        const int localY = point.y - padding;

        if (localX < 0 || localY < 0)
            return -1;

        const int col = localX / (tileWidth + gap);
        const int row = localY / (tileHeight + gap);
        if (col < 0 || col >= columns)
            return -1;
        if (localX % (tileWidth + gap) > tileWidth || localY % (tileHeight + gap) > tileHeight - 30)
            return -1;

        const int index = row * columns + col;
        return index < (int) owner.albums.size() ? index : -1;
    }

    void paintTile (juce::Graphics& g, int albumIndex, juce::Rectangle<int> tile, bool hovered, bool selected)
    {
        const auto& album = owner.albums[(size_t) albumIndex];
        juce::Rectangle<int> art (tile.getX(), tile.getY(), artSize, artSize);

        // Artwork or a machined placeholder.
        juce::Image image;
        if (album.artworkId.isNotEmpty())
            image = owner.artworkCache.get (ArtworkCache::keyForArtworkId (album.artworkId));

        juce::Rectangle<float> artF = art.toFloat();
        g.setColour (theme::panelSunken);
        g.fillRoundedRectangle (artF, 3.0f);

        if (image.isValid())
        {
            juce::Graphics::ScopedSaveState save (g);
            juce::Path clip;
            clip.addRoundedRectangle (artF, 3.0f);
            g.reduceClipRegion (clip);
            const float scale = juce::jmin (artF.getWidth() / (float) image.getWidth(),
                                            artF.getHeight() / (float) image.getHeight());
            const float w = (float) image.getWidth() * scale;
            const float h = (float) image.getHeight() * scale;
            g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
            g.drawImage (image, juce::Rectangle<float> (artF.getCentreX() - w * 0.5f,
                                                       artF.getCentreY() - h * 0.5f, w, h));
        }
        else
        {
            const auto centre = artF.getCentre();
            const float radius = artF.getWidth() * 0.36f;
            g.setColour (theme::line.withAlpha (0.5f));
            for (int i = 1; i <= 4; ++i)
                g.drawEllipse (centre.x - radius * (float) i / 4.0f, centre.y - radius * (float) i / 4.0f,
                               radius * 2.0f * (float) i / 4.0f, radius * 2.0f * (float) i / 4.0f, 0.8f);
            paintIcon (g, Icon::music, { centre.x - 16.0f, centre.y - 16.0f, 32.0f, 32.0f },
                       theme::silverDark.withAlpha (0.6f), 1.4f);
        }

        g.setColour (selected ? theme::gold : hovered ? theme::silverDim : theme::lineStrong.withAlpha (0.7f));
        g.drawRoundedRectangle (artF.reduced (0.5f), 3.0f, selected ? 1.6f : 1.0f);

        const int textX = tile.getX();
        const int textWidth = tile.getWidth();

        auto nameArea = juce::Rectangle<int> (textX, art.getBottom() + 9, textWidth, 20);
        g.setColour (selected ? theme::goldBright : theme::text);
        g.setFont (theme::uiFont (12.5f, true));
        g.drawFittedText (album.name, nameArea, juce::Justification::centredLeft, 1);

        auto artistArea = juce::Rectangle<int> (textX, nameArea.getBottom() + 1, textWidth, 18);
        g.setColour (theme::textDim);
        g.setFont (theme::uiFont (11.5f));
        g.drawFittedText (album.artist, artistArea, juce::Justification::centredLeft, 1);

        auto detailArea = juce::Rectangle<int> (textX, artistArea.getBottom() + 2, textWidth, 16);
        juce::String detail;
        if (album.year > 0)
            detail << album.year << "   |   ";
        detail << formatCount (album.trackCount, "track", "tracks");
        g.setColour (theme::textFaint);
        g.setFont (theme::monoFont (9.5f));
        g.drawFittedText (detail, detailArea, juce::Justification::centredLeft, 1);
    }

    LibraryBrowser& owner;
    int columns = 4;
    int hoverTile = -1;
};

//==============================================================================
LibraryBrowser::LibraryBrowser (AppContext& context)
    : ctx (context),
      artworkCache (context.library())
{
    addAndMakeVisible (heroArtwork);
    addAndMakeVisible (heroTitle);
    addAndMakeVisible (heroArtist);
    addAndMakeVisible (heroAlbum);
    addAndMakeVisible (heroTechnical);
    addAndMakeVisible (heroQuality);
    addAndMakeVisible (playAllButton);
    addAndMakeVisible (queueButton);
    addAndMakeVisible (importFolderButton);
    addAndMakeVisible (importFilesButton);
    addAndMakeVisible (clearFilterButton);
    addAndMakeVisible (searchField);
    addAndMakeVisible (listViewButton);
    addAndMakeVisible (gridViewButton);
    addAndMakeVisible (countLabel);
    addAndMakeVisible (table);
    addAndMakeVisible (albumViewport);
    albumGrid = std::make_unique<AlbumGrid> (*this);
    albumViewport.setViewedComponent (albumGrid.get(), false);
    albumViewport.setScrollBarsShown (true, false, true, false);
    albumViewport.setScrollBarThickness (9);
    addAndMakeVisible (albumViewport);

    playlistPanel = std::make_unique<PlaylistPanel> (ctx);
    queuePanel = std::make_unique<QueuePanel> (ctx);
    healthPanel = std::make_unique<HealthPanel> (ctx);
    addAndMakeVisible (*playlistPanel);
    addAndMakeVisible (*queuePanel);
    addAndMakeVisible (*healthPanel);

    //== Styles ===============================================================
    heroTitle.setFont (theme::uiFont (22.0f, true, 0.01f));
    heroTitle.setColour (juce::Label::textColourId, theme::text);
    heroArtist.setFont (theme::uiFont (14.0f));
    heroArtist.setColour (juce::Label::textColourId, theme::silver);
    heroAlbum.setFont (theme::uiFont (12.0f));
    heroAlbum.setColour (juce::Label::textColourId, theme::textDim);
    heroTechnical.setFont (theme::monoFont (10.5f));
    heroTechnical.setColour (juce::Label::textColourId, theme::textFaint);
    heroQuality.setBadgeColour (theme::gold);
    heroQuality.setVisible (false);

    countLabel.setFont (theme::monoFont (10.5f));
    countLabel.setColour (juce::Label::textColourId, theme::textFaint);
    countLabel.setJustificationType (juce::Justification::centredRight);

    searchField.setPlaceholder ("Search title, artist, album, genre");
    searchField.onTextChange = [this] (const juce::String& text)
    {
        searchText = text;
        rebuildFilteredList();
        rebuildAlbums();
        rebuildTableSource();
        updateToolbar();
        updateHeader();
        repaint();
    };
    searchField.onEscape = [this] { table.grabKeyboardFocus(); };

    listViewButton.setClickingTogglesState (true);
    gridViewButton.setClickingTogglesState (true);
    listViewButton.onClick = [this] { setViewMode (false); };
    gridViewButton.onClick = [this] { setViewMode (true); };

    playAllButton.onClick = [this] { playAll(); };
    queueButton.onClick = [this] { addAllToQueue(); };

    removeTracksButton.onClick = [this] { removeSelectedTracks(); };
    clearLibraryButton.onClick = [this] { clearLibraryContents(); };
    addAndMakeVisible (removeTracksButton);
    addAndMakeVisible (clearLibraryButton);

    importFolderButton.onClick = [this] { if (onImportFolderRequested != nullptr) onImportFolderRequested(); };
    importFilesButton.onClick = [this] { if (onImportFilesRequested != nullptr) onImportFilesRequested(); };

    clearFilterButton.onClick = [this]
    {
        SidebarPanel::Selection songs;
        songs.kind = SidebarPanel::Selection::Kind::songs;
        navigate (songs);
        if (onNavigateRequested != nullptr)
            onNavigateRequested (songs);
    };

    table.setListener (this);

    playlistPanel->onPlaylistDeleted = [this] (PlaylistId id)
    {
        if (onPlaylistDeleted != nullptr)
            onPlaylistDeleted (id);
        showSongs();
    };

    artworkCache.onImageReady = [this]
    {
        auto image = artworkCache.get (heroArtworkKey);
        if (image.isValid())
            heroArtwork.setImage (image);
        albumGrid->repaint();
        heroArtwork.repaint();
    };

    //== Restore layout preferences ==========================================
    auto& settings = ctx.settings();
    viewMode = settings.getString ("layout.viewMode", "list") == "grid" ? ViewMode::grid : ViewMode::list;
    fullPlayer = settings.getBool ("layout.fullPlayer", false);

    const int sortColumn = settings.getInt ("layout.sortColumn", (int) TrackTableComponent::Column::artist);
    const bool sortDescending = settings.getBool ("layout.sortDescending", false);
    table.setSort ((TrackTableComponent::Column) juce::jlimit (0, (int) TrackTableComponent::Column::rating - 1, sortColumn),
                   sortDescending);

    selection.kind = SidebarPanel::Selection::Kind::songs;
    refresh();
    startTimerHz (2);
}

LibraryBrowser::~LibraryBrowser()
{
    stopTimer();
    table.setListener (nullptr);
    artworkCache.onImageReady = nullptr;
    albumViewport.setViewedComponent (nullptr, false);
    albumGrid.reset();
    playlistPanel.reset();
    queuePanel.reset();
    healthPanel.reset();
}

//==============================================================================
void LibraryBrowser::refresh()
{
    auto newSnapshot = ctx.library().getSnapshot();
    snapshot = newSnapshot;

    if (snapshot != nullptr)
    {
        libraryEmpty = snapshot->tracks.empty();
        snapshotVersion = snapshot->version;
    }
    else
    {
        libraryEmpty = true;
        snapshotVersion = 0;
    }

    auto status = ctx.engine().getStatus();
    playingTrackId = status.currentTrack.id;
    playing = status.state == PlaybackState::playing;

    rebuildFilteredList();
    rebuildAlbums();
    rebuildTableSource();

    playlistPanel->refresh();
    queuePanel->refresh();

    updateToolbar();
    updateHeader();
    updatePageVisibility();
    resized();
    repaint();
}

std::vector<TrackId> LibraryBrowser::getSelectedTrackIds() const
{
    return table.getSelectedTrackIds();
}

TrackId LibraryBrowser::getContextTrackId() const
{
    auto selected = table.getSelectedTrackIds();
    if (! selected.empty())
        return selected.front();
    if (playingTrackId != kInvalidTrackId)
        return playingTrackId;
    return heroTrackId;
}

void LibraryBrowser::rateContextTrack (int rating)
{
    const auto id = getContextTrackId();
    if (id == kInvalidTrackId)
    {
        ctx.setStatusMessage ("Select a track to rate it.", 4000);
        return;
    }
    ctx.library().updateRating (id, juce::jlimit (0, 5, rating));
    ctx.setStatusMessage ("Rating: " + juce::String (juce::jlimit (0, 5, rating)) + " stars.");
    updateHeader();
    table.repaint();
}

void LibraryBrowser::addContextToQueue()
{
    auto selected = table.getSelectedTrackIds();
    if (! selected.empty())
    {
        std::vector<Track> tracks;
        if (snapshot != nullptr)
            for (auto id : selected)
                if (const auto* track = snapshot->findTrack (id))
                    tracks.push_back (*track);
        if (! tracks.empty())
        {
            ctx.engine().enqueueAfterCurrent (tracks);
            ctx.setStatusMessage ("Added " + formatCount ((int) tracks.size(), "track") + " to the queue.");
            return;
        }
    }
    addAllToQueue();
}

void LibraryBrowser::playContext()
{
    const auto selected = table.getSelectedTrackIds();
    if (! selected.empty())
    {
        trackDoubleClicked (selected.front());
        return;
    }
    playAll();
}

void LibraryBrowser::setPlayingTrack (TrackId trackId, bool isPlaying)
{
    if (playingTrackId == trackId && playing == isPlaying)
        return;

    playingTrackId = trackId;
    playing = isPlaying;
    table.setPlayingTrack (trackId, isPlaying);
    updateHeader();
    repaint();
}

//==============================================================================
void LibraryBrowser::navigate (const SidebarPanel::Selection& newSelection)
{
    const bool wasLibrary = page == Page::library;
    selection = newSelection;
    filterChipText.clear();
    searchText.clear();
    searchField.setText ({}, false);

    switch (selection.kind)
    {
        case SidebarPanel::Selection::Kind::songs:
            page = Page::library;
            viewMode = ViewMode::list;
            break;
        case SidebarPanel::Selection::Kind::albums:
            page = Page::library;
            viewMode = ViewMode::grid;
            break;
        case SidebarPanel::Selection::Kind::artists:
        case SidebarPanel::Selection::Kind::genres:
        case SidebarPanel::Selection::Kind::years:
        case SidebarPanel::Selection::Kind::formats:
        case SidebarPanel::Selection::Kind::folders:
            page = Page::library;
            filterChipText = kindLabel (selection.kind) + ": " + selection.value;
            break;
        case SidebarPanel::Selection::Kind::playlist:
            page = Page::playlist;
            playlistPanel->showPlaylist (selection.playlistId);
            break;
        case SidebarPanel::Selection::Kind::queue:
            page = Page::queue;
            queuePanel->refresh();
            break;
        case SidebarPanel::Selection::Kind::health:
            page = Page::health;
            break;
    }

    if (page == Page::library)
    {
        rebuildFilteredList();
        rebuildAlbums();
        rebuildTableSource();
        updateToolbar();
        updateHeader();
        selectedAlbumIndex = -1;
    }

    juce::ignoreUnused (wasLibrary);
    updatePageVisibility();
    resized();
    repaint();
}

void LibraryBrowser::showSongs()
{
    SidebarPanel::Selection songs;
    songs.kind = SidebarPanel::Selection::Kind::songs;
    navigate (songs);
}

void LibraryBrowser::showAlbums()
{
    SidebarPanel::Selection albums;
    albums.kind = SidebarPanel::Selection::Kind::albums;
    navigate (albums);
}

void LibraryBrowser::showPlaylist (PlaylistId id)
{
    SidebarPanel::Selection sel;
    sel.kind = SidebarPanel::Selection::Kind::playlist;
    sel.playlistId = id;
    navigate (sel);
}

void LibraryBrowser::showQueue()
{
    SidebarPanel::Selection sel;
    sel.kind = SidebarPanel::Selection::Kind::queue;
    navigate (sel);
}

void LibraryBrowser::showHealth()
{
    SidebarPanel::Selection sel;
    sel.kind = SidebarPanel::Selection::Kind::health;
    navigate (sel);
}

void LibraryBrowser::focusSearch()
{
    if (page != Page::library)
        showSongs();
    searchField.focusEditor();
}

void LibraryBrowser::setViewMode (bool grid)
{
    const auto mode = grid ? ViewMode::grid : ViewMode::list;
    if (viewMode == mode)
        return;
    viewMode = mode;
    saveViewPreferences();
    updatePageVisibility();
    resized();
    repaint();
}

void LibraryBrowser::setFullPlayer (bool full)
{
    if (fullPlayer == full)
        return;
    fullPlayer = full;
    updateHeader();
    saveViewPreferences();
    resized();
    repaint();
}

void LibraryBrowser::saveViewPreferences()
{
    auto& settings = ctx.settings();
    settings.set ("layout.viewMode", viewMode == ViewMode::grid ? "grid" : "list");
    settings.set ("layout.fullPlayer", fullPlayer);
    settings.set ("layout.sortColumn", (int) table.getSortColumn());
    settings.set ("layout.sortDescending", table.isSortDescending());
    settings.saveIfDirty();
}

//==============================================================================
bool LibraryBrowser::trackMatchesSelection (const Track& track) const
{
    switch (selection.kind)
    {
        case SidebarPanel::Selection::Kind::artists:
            return track.displayArtist().equalsIgnoreCase (selection.value);
        case SidebarPanel::Selection::Kind::genres:
            return track.displayGenre().equalsIgnoreCase (selection.value);
        case SidebarPanel::Selection::Kind::years:
            return juce::String (track.year) == selection.value;
        case SidebarPanel::Selection::Kind::formats:
            return track.formatName.equalsIgnoreCase (selection.value);
        case SidebarPanel::Selection::Kind::folders:
            return track.folder == selection.value || track.folder.startsWith (selection.value + "/");
        default:
            return true;
    }
}

bool LibraryBrowser::trackMatchesSearch (const Track& track) const
{
    if (searchText.trim().isEmpty())
        return true;

    juce::String haystack;
    haystack << track.displayTitle() << " " << track.displayArtist() << " " << track.displayAlbum()
             << " " << track.displayGenre() << " " << track.formatName << " " << juce::String (track.year);

    auto terms = juce::StringArray::fromTokens (searchText.toLowerCase(), " ", "");
    for (auto& term : terms)
    {
        if (term.isEmpty())
            continue;
        if (! haystack.containsIgnoreCase (term))
            return false;
    }
    return true;
}

void LibraryBrowser::rebuildFilteredList()
{
    filteredIndices.clear();
    if (snapshot == nullptr)
        return;

    for (int i = 0; i < (int) snapshot->tracks.size(); ++i)
    {
        const auto& track = snapshot->tracks[(size_t) i];
        if (trackMatchesSelection (track) && trackMatchesSearch (track))
            filteredIndices.push_back (i);
    }
}

void LibraryBrowser::rebuildAlbums()
{
    albums.clear();
    if (snapshot == nullptr)
        return;

    std::map<juce::String, int> indexByKey;

    for (auto index : filteredIndices)
    {
        if (index < 0 || index >= (int) snapshot->tracks.size())
            continue;

        const auto& track = snapshot->tracks[(size_t) index];
        const auto artistKey = track.albumArtist.isNotEmpty() ? track.albumArtist : track.displayArtist();
        const auto key = track.displayAlbum() + "\x1f" + artistKey;

        auto it = indexByKey.find (key);
        if (it == indexByKey.end())
        {
            AlbumItem album;
            album.name = track.displayAlbum();
            album.artist = artistKey;
            album.artistKey = artistKey;
            album.artworkId = track.artworkId;
            album.year = track.year;
            album.trackCount = 0;
            album.durationMs = 0;
            album.representative = track;
            indexByKey[key] = (int) albums.size();
            albums.push_back (std::move (album));
            it = indexByKey.find (key);
        }

        auto& album = albums[(size_t) it->second];
        ++album.trackCount;
        album.durationMs += track.durationMs;
        if (album.artworkId.isEmpty() && track.artworkId.isNotEmpty())
        {
            album.artworkId = track.artworkId;
            album.representative = track;
        }
        if (album.year == 0 && track.year > 0)
            album.year = track.year;
    }

    std::sort (albums.begin(), albums.end(),
               [] (const AlbumItem& a, const AlbumItem& b)
               {
                   const int byArtist = a.artist.compareIgnoreCase (b.artist);
                   if (byArtist != 0)
                       return byArtist < 0;
                   return a.name.compareIgnoreCase (b.name) < 0;
               });

    if (selectedAlbumIndex >= (int) albums.size())
        selectedAlbumIndex = -1;

    // Prime the artwork cache for the tiles (background thread).
    for (auto& album : albums)
        if (! album.artworkId.isEmpty())
            artworkCache.request (album.representative);

    if (albumGrid != nullptr)
        albumGrid->rebuildContent();
}

void LibraryBrowser::rebuildTableSource()
{
    TrackTableComponent::RowSource source;
    source.snapshot = snapshot;
    source.indices = filteredIndices;
    table.setRowSource (std::move (source));

    if (libraryEmpty)
    {
        table.setEmptyMessage ("Your library is empty", {});
    }
    else if (filteredIndices.empty())
    {
        table.setEmptyMessage ("No tracks match",
                               searchText.isNotEmpty() || filterChipText.isNotEmpty()
                                   ? "Try a different search, or clear the filter."
                                   : "Import more music to see tracks here.");
    }
    else
    {
        table.setEmptyMessage ("No tracks", {});
    }
}

void LibraryBrowser::updateToolbar()
{
    const int total = snapshot != nullptr ? (int) snapshot->tracks.size() : 0;
    const int shown = (int) filteredIndices.size();

    if (shown == total)
        countLabel.setText (numberWithCommas (shown) + " tracks", juce::dontSendNotification);
    else
        countLabel.setText (numberWithCommas (shown) + " of " + numberWithCommas (total) + " tracks",
                            juce::dontSendNotification);

    filterActive = selection.kind == SidebarPanel::Selection::Kind::artists
                   || selection.kind == SidebarPanel::Selection::Kind::genres
                   || selection.kind == SidebarPanel::Selection::Kind::years
                   || selection.kind == SidebarPanel::Selection::Kind::formats
                   || selection.kind == SidebarPanel::Selection::Kind::folders;

    clearFilterButton.setVisible (filterActive && page == Page::library && ! libraryEmpty);
    if (filterActive)
        clearFilterButton.setButtonText (filterChipText + "     x");

    listViewButton.setToggleState (viewMode == ViewMode::list, juce::dontSendNotification);
    gridViewButton.setToggleState (viewMode == ViewMode::grid, juce::dontSendNotification);

    playAllButton.setEnabled (shown > 0);
    queueButton.setEnabled (shown > 0);

    importFolderButton.setVisible (libraryEmpty && page == Page::library);
    importFilesButton.setVisible (libraryEmpty && page == Page::library);

    searchField.setVisible (page == Page::library && ! libraryEmpty);
    clearFilterButton.setVisible (filterActive && page == Page::library && ! libraryEmpty);
    removeTracksButton.setVisible (page == Page::library);
    clearLibraryButton.setVisible (page == Page::library);
    removeTracksButton.setEnabled (! table.getSelectedTrackIds().empty());
    clearLibraryButton.setEnabled (snapshot != nullptr && (! snapshot->tracks.empty()
                                                          || ! ctx.library().getLocations().empty()));
}

void LibraryBrowser::updateHeader()
{
    if (page != Page::library)
        return;

    // Hero track: playing track first, then selection, then the first visible track.
    Track hero;
    bool haveHero = false;

    if (playingTrackId != kInvalidTrackId)
    {
        if (snapshot != nullptr)
            if (const auto* track = snapshot->findTrack (playingTrackId))
            {
                hero = *track;
                haveHero = true;
            }

        if (! haveHero)
        {
            const auto status = ctx.engine().getStatus();
            if (status.currentTrack.path.isNotEmpty())
            {
                hero = status.currentTrack;
                haveHero = true;
            }
        }
    }

    if (! haveHero)
    {
        auto selectedIds = table.getSelectedTrackIds();
        if (! selectedIds.empty() && snapshot != nullptr)
            if (const auto* track = snapshot->findTrack (selectedIds.front()))
            {
                hero = *track;
                haveHero = true;
            }
    }

    if (! haveHero && selectedAlbumIndex >= 0 && selectedAlbumIndex < (int) albums.size())
    {
        hero = albums[(size_t) selectedAlbumIndex].representative;
        haveHero = true;
    }

    if (! haveHero && ! filteredIndices.empty() && table.getNumRows() > 0)
        if (const auto* track = table.getTrackForRow (0))
        {
            hero = *track;
            haveHero = true;
        }

    updateHeroFromTrack (haveHero ? hero : Track {});
    table.setPlayingTrack (playingTrackId, playing);
}

void LibraryBrowser::updateHeroFromTrack (const Track& track)
{
    const bool haveTrack = track.id != kInvalidTrackId || track.path.isNotEmpty();
    heroTrack = track;
    heroTrackId = haveTrack ? track.id : kInvalidTrackId;

    const float titleSize = fullPlayer ? 31.0f : 22.0f;
    const float artistSize = fullPlayer ? 16.0f : 14.0f;

    heroTitle.setFont (theme::uiFont (titleSize, true, 0.01f));
    heroArtist.setFont (theme::uiFont (artistSize));

    if (! haveTrack)
    {
        heroTitle.setText (libraryEmpty ? "Your library is empty" : "Nothing selected", juce::dontSendNotification);
        heroArtist.setText (libraryEmpty ? "Import music to begin" : "Select a track to see its details",
                            juce::dontSendNotification);
        heroAlbum.setText ({}, juce::dontSendNotification);
        heroTechnical.setText (libraryEmpty ? "File > Import Folder...   or drag a folder onto the window"
                                            : "Double-click a track to play it",
                               juce::dontSendNotification);
        heroQualityActive = false;
        heroQuality.setVisible (false);
        heroArtwork.setImage ({});
        heroArtwork.setPlaceholder ("SNAKE PUPPY", "REFERENCE PLAYER");
        heroArtworkKey.clear();
        return;
    }

    heroTitle.setText (track.displayTitle(), juce::dontSendNotification);
    heroArtist.setText (track.displayArtist().isNotEmpty() ? track.displayArtist()
                                                           : juce::String ("Unknown Artist"),
                        juce::dontSendNotification);

    juce::String albumLine = track.displayAlbum();
    if (track.year > 0)
        albumLine << "   |   " << track.year;
    if (track.genre.isNotEmpty())
        albumLine << "   |   " << track.genre;
    heroAlbum.setText (albumLine, juce::dontSendNotification);

    juce::String technical;
    if (track.isDsd)
        technical << formatBitDepth (0, true, track.dsdRateMultiple) << "   |   "
                  << formatSampleRate (track.sampleRate);
    else
        technical << formatBitDepth (track.bitDepth) << "   |   " << formatSampleRate (track.sampleRate);
    if (track.numChannels > 2)
        technical << "   |   " << formatChannels (track.numChannels);
    if (track.missing)
        technical << "   |   FILE MISSING";
    heroTechnical.setText (technical, juce::dontSendNotification);

    // Quality badge: honest live signal path when this is the playing track,
    // otherwise the file's own format.
    const auto status = ctx.engine().getStatus();
    if (playingTrackId == track.id && status.currentTrack.id == track.id && status.signalPath.quality != SignalPathInfo::Quality::unknown)
    {
        const auto quality = status.signalPath.quality;
        heroQualityActive = true;
        heroQuality.setVisible (true);
        heroQuality.setBadgeColour (qualityColour (quality));
        switch (quality)
        {
            case SignalPathInfo::Quality::bitPerfect:     heroQuality.setText ("BIT-PERFECT"); break;
            case SignalPathInfo::Quality::bitTransparent: heroQuality.setText ("BIT-TRANSPARENT"); break;
            case SignalPathInfo::Quality::processed:      heroQuality.setText ("PROCESSED"); break;
            case SignalPathInfo::Quality::error:          heroQuality.setText ("ERROR"); break;
            default:                                      heroQuality.setText ("SIGNAL UNKNOWN"); break;
        }
        heroQuality.setSize (heroQuality.getPreferredWidth(), 17);
    }
    else
    {
        heroQualityActive = track.formatName.isNotEmpty();
        heroQuality.setVisible (heroQualityActive);
        heroQuality.setBadgeColour (track.isDsd ? theme::gold : theme::silverDim);
        heroQuality.setText (track.formatName.isNotEmpty() ? track.formatName.toUpperCase() : "-");
        heroQuality.setSize (heroQuality.getPreferredWidth(), 17);
    }

    // Artwork (asynchronously loaded and cached).
    const auto key = ArtworkCache::keyFor (track);
    if (key != heroArtworkKey)
    {
        heroArtworkKey = key;
        heroArtwork.setImage (artworkCache.get (key));
        artworkCache.request (track);
    }

    heroArtwork.setPlaceholder (track.displayAlbum(), track.displayArtist());
    heroArtwork.repaint();
}

//==============================================================================
std::vector<Track> LibraryBrowser::collectVisibleTracks() const
{
    std::vector<Track> tracks;
    tracks.reserve ((size_t) table.getNumRows());
    for (int row = 0; row < table.getNumRows(); ++row)
        if (auto* track = table.getTrackForRow (row))
            tracks.push_back (*track);
    return tracks;
}

void LibraryBrowser::playAll()
{
    auto tracks = collectVisibleTracks();
    if (tracks.empty())
        return;
    ctx.playTracks (tracks, 0);
    ctx.setStatusMessage ("Playing " + formatCount ((int) tracks.size(), "track")
                          + (filterChipText.isNotEmpty() ? " (" + filterChipText + ")" : juce::String()));
}

void LibraryBrowser::addAllToQueue()
{
    auto tracks = collectVisibleTracks();
    if (tracks.empty())
        return;
    ctx.engine().enqueueAfterCurrent (tracks);
    ctx.setStatusMessage ("Added " + formatCount ((int) tracks.size(), "track") + " to the queue.");
}

void LibraryBrowser::playAlbum (int albumIndex)
{
    if (albumIndex < 0 || albumIndex >= (int) albums.size() || snapshot == nullptr)
        return;

    const auto& album = albums[(size_t) albumIndex];
    std::vector<Track> tracks;
    for (auto index : filteredIndices)
    {
        if (index < 0 || index >= (int) snapshot->tracks.size())
            continue;
        const auto& track = snapshot->tracks[(size_t) index];
        const auto trackArtist = track.albumArtist.isNotEmpty() ? track.albumArtist : track.displayArtist();
        if (track.displayAlbum().equalsIgnoreCase (album.name)
            && trackArtist.equalsIgnoreCase (album.artistKey))
            tracks.push_back (track);
    }

    if (tracks.empty())
        return;

    ctx.playTracks (tracks, 0);
    ctx.setStatusMessage ("Playing album: " + album.name);
}

//==============================================================================
void LibraryBrowser::trackSelectionChanged (TrackTableComponent&)
{
    removeTracksButton.setEnabled (! table.getSelectedTrackIds().empty());
    updateHeader();
}

void LibraryBrowser::trackDoubleClicked (TrackId trackId)
{
    auto tracks = collectVisibleTracks();
    if (tracks.empty())
        return;

    const int row = juce::jmax (0, table.getRowForTrack (trackId));
    ctx.playTracks (tracks, row);
}

void LibraryBrowser::trackSortChanged (TrackTableComponent::Column, bool)
{
    saveViewPreferences();
}

void LibraryBrowser::tracksAddToQueue (const std::vector<TrackId>&)
{
    addAllToQueue();
}

void LibraryBrowser::tracksRemoveRequested (const std::vector<TrackId>& ids)
{
    if (ids.empty())
        return;

    juce::Component::SafePointer<LibraryBrowser> safe (this);
    const auto count = (int) ids.size();
    showConfirmDialog ("REMOVE FROM LIBRARY",
                       "Remove " + formatCount (count, "selected track", "selected tracks")
                       + " from the library?\n\nFiles on disk are not deleted.",
                       "Remove",
                       [safe, ids]
                       {
                           if (safe == nullptr)
                               return;
                           safe->ctx.library().removeTracks (ids);
                           safe->ctx.setStatusMessage ("Removed "
                                                       + formatCount ((int) ids.size(), "track", "tracks")
                                                       + " from the library.");
                           safe->refresh();
                           safe->updateHeader();
                       });
}

void LibraryBrowser::removeSelectedTracks()
{
    tracksRemoveRequested (table.getSelectedTrackIds());
}

void LibraryBrowser::clearLibraryContents()
{
    std::vector<LocationId> locationIds;
    for (auto& location : ctx.library().getLocations())
        locationIds.push_back (location.id);

    std::vector<TrackId> trackIds;
    if (snapshot != nullptr)
        for (auto& track : snapshot->tracks)
            trackIds.push_back (track.id);

    if (locationIds.empty() && trackIds.empty())
        return;

    juce::Component::SafePointer<LibraryBrowser> safe (this);
    showConfirmDialog ("CLEAR LIBRARY",
                       "Remove all library folders and every indexed track from the browser?\n\n"
                       "Files on disk are not deleted. Playlists are kept but will show missing "
                       "entries until you add folders again.",
                       "Clear Library",
                       [safe, locationIds, trackIds]
                       {
                           if (safe == nullptr)
                               return;
                           for (auto id : locationIds)
                               safe->ctx.library().removeLocation (id, false);
                           if (! trackIds.empty())
                               safe->ctx.library().removeTracks (trackIds);
                           safe->ctx.setStatusMessage ("Library cleared. Add a folder to build it again.");
                           safe->refresh();
                           safe->updateHeader();
                       });
}

void LibraryBrowser::trackRatingChanged (TrackId trackId, int rating)
{
    ctx.library().updateRating (trackId, rating);
    updateHeader();
}

//==============================================================================
void LibraryBrowser::timerCallback()
{
    auto& settings = ctx.settings();
    juce::ignoreUnused (settings);

    auto latest = ctx.library().getSnapshot();
    const std::uint64_t latestVersion = latest != nullptr ? latest->version : 0;
    if (latestVersion != snapshotVersion)
        refresh();

    const auto status = ctx.engine().getStatus();
    if (status.currentTrack.id != playingTrackId || (status.state == PlaybackState::playing) != playing)
    {
        playingTrackId = status.currentTrack.id;
        playing = status.state == PlaybackState::playing;
        table.setPlayingTrack (playingTrackId, playing);
        updateHeader();
    }
}

//==============================================================================
void LibraryBrowser::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    if (page == Page::library)
    {
        // Header backplate.
        juce::ColourGradient grad (theme::panel.brighter (0.02f), 0.0f, (float) headerArea.getY(),
                                   theme::panel.darker (0.06f), 0.0f, (float) headerArea.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRect (headerArea);
        g.setColour (theme::line);
        g.drawHorizontalLine (headerArea.getBottom() - 1, 0.0f, (float) headerArea.getWidth());

        // Toolbar backplate.
        g.setColour (theme::panel.darker (0.10f));
        g.fillRect (toolbarArea);
        g.setColour (theme::line);
        g.drawHorizontalLine (toolbarArea.getBottom() - 1, 0.0f, (float) toolbarArea.getWidth());

        if (libraryEmpty)
        {
            auto block = contentArea.withSizeKeepingCentre (juce::jmin (juce::jmax (320, contentArea.getWidth() - 80), 660), 340);

            paintIcon (g, Icon::folder, { (float) block.getCentreX() - 26.0f, (float) block.getY(), 52.0f, 52.0f },
                       theme::silverDark, 1.6f);

            g.setColour (theme::text);
            g.setFont (theme::uiFont (22.0f, true));
            g.drawFittedText ("Your library is empty",
                              block.withTrimmedTop (70).withHeight (30), juce::Justification::centred, 1);

            g.setColour (theme::textDim);
            g.setFont (theme::uiFont (13.0f));
            g.drawFittedText ("Add a folder of music to build your library. FLAC, ALAC, WAV, AIFF, "
                              "DSF/DFF, MP3, AAC and OGG are supported - or drag files/folders onto this window.",
                              block.withTrimmedTop (108).withHeight (50), juce::Justification::centred, 3);

            g.setColour (theme::textFaint);
            g.setFont (theme::monoFont (10.0f));
            g.drawFittedText ("File  >  Import Folder...    (Cmd+O)",
                              block.withTrimmedTop (274).withHeight (20), juce::Justification::centred, 1);
        }
    }
}

void LibraryBrowser::updatePageVisibility()
{
    const bool library = page == Page::library;

    heroArtwork.setVisible (library);
    heroTitle.setVisible (library);
    heroArtist.setVisible (library);
    heroAlbum.setVisible (library);
    heroTechnical.setVisible (library);
    heroQuality.setVisible (library && heroQualityActive);
    playAllButton.setVisible (library);
    queueButton.setVisible (library);
    searchField.setVisible (library && ! libraryEmpty);
    listViewButton.setVisible (library);
    gridViewButton.setVisible (library);
    countLabel.setVisible (library);
    clearFilterButton.setVisible (library && filterActive && ! libraryEmpty);
    removeTracksButton.setVisible (library);
    clearLibraryButton.setVisible (library);
    removeTracksButton.setEnabled (library && ! table.getSelectedTrackIds().empty());
    clearLibraryButton.setEnabled (library && (snapshot != nullptr && ! snapshot->tracks.empty()));

    const bool showTable = library && viewMode == ViewMode::list && ! libraryEmpty;
    const bool showGrid = library && viewMode == ViewMode::grid && ! libraryEmpty;

    table.setVisible (showTable);
    albumViewport.setVisible (showGrid);

    importFolderButton.setVisible (library && libraryEmpty);
    importFilesButton.setVisible (library && libraryEmpty);

    playlistPanel->setVisible (page == Page::playlist);
    queuePanel->setVisible (page == Page::queue);
    healthPanel->setVisible (page == Page::health);

    updateToolbar();
}

void LibraryBrowser::applyViewMode()
{
    updatePageVisibility();
    resized();
}

void LibraryBrowser::resized()
{
    auto area = getLocalBounds();

    if (page == Page::library)
    {
        headerArea = area.removeFromTop (fullPlayer ? theme::headerHeightFull : theme::headerHeight);
        toolbarArea = area.removeFromTop (theme::toolbarHeight);
        contentArea = area;

        //== Header ===========================================================
        auto header = headerArea.reduced (18, 14);
        const int artSize = juce::jmin (header.getHeight(),
                                        fullPlayer ? 232 : juce::jmin (148, header.getHeight() - 6));
        heroArtwork.setBounds (header.removeFromLeft (juce::jmax (60, artSize)));
        header.removeFromLeft (fullPlayer ? 26 : 20);

        auto actions = header.removeFromRight (juce::jmin (236, juce::jmax (140, header.getWidth() / 2)));
        auto actionRow = actions.removeFromTop (juce::jmin (34, actions.getHeight()));
        playAllButton.setBounds (actionRow.removeFromLeft (juce::jmin (96, actionRow.getWidth() / 2))
                                         .withSizeKeepingCentre (juce::jmin (96, actionRow.getWidth() / 2), 30));
        actionRow.removeFromLeft (8);
        queueButton.setBounds (actionRow.withSizeKeepingCentre (juce::jmax (0, actionRow.getWidth()), 30));

        auto textArea = header.withTrimmedRight (16);
        const int titleHeight = fullPlayer ? 42 : 32;
        heroTitle.setBounds (textArea.removeFromTop (titleHeight).translated (0, fullPlayer ? 4 : 0));
        heroArtist.setBounds (textArea.removeFromTop (fullPlayer ? 26 : 21));
        auto albumLine = textArea.removeFromTop (20);
        heroAlbum.setBounds (albumLine);
        auto techRow = textArea.removeFromTop (20);
        if (heroQuality.isVisible())
        {
            heroQuality.setBounds (techRow.removeFromLeft (heroQuality.getPreferredWidth())
                                             .withSizeKeepingCentre (heroQuality.getPreferredWidth(), 17));
            techRow.removeFromLeft (10);
        }
        heroTechnical.setBounds (techRow);

        //== Toolbar ==========================================================
        auto toolbar = toolbarArea.reduced (18, 7);
        auto right = toolbar.removeFromRight (juce::jmin (520, juce::jmax (330, toolbar.getWidth() * 2 / 3)));
        gridViewButton.setBounds (right.removeFromRight (30).withSizeKeepingCentre (28, 28));
        right.removeFromRight (4);
        listViewButton.setBounds (right.removeFromRight (30).withSizeKeepingCentre (28, 28));
        right.removeFromRight (10);
        clearLibraryButton.setBounds (right.removeFromRight (126).withSizeKeepingCentre (126, 28));
        right.removeFromRight (6);
        removeTracksButton.setBounds (right.removeFromRight (86).withSizeKeepingCentre (86, 28));
        right.removeFromRight (10);
        countLabel.setBounds (right);

        const int searchWidth = juce::jlimit (180, 360, toolbar.getWidth() / 3);
        searchField.setBounds (toolbar.removeFromLeft (searchWidth).withSizeKeepingCentre (searchWidth, 28));
        toolbar.removeFromLeft (10);

        if (clearFilterButton.isVisible())
        {
            const int chipWidth = juce::jlimit (110, 260,
                                                (int) measureText (theme::uiFont (11.0f, true, 0.05f),
                                                                   clearFilterButton.getButtonText()) + 36);
            clearFilterButton.setBounds (toolbar.removeFromLeft (chipWidth).withSizeKeepingCentre (chipWidth, 26));
        }
    }
    else
    {
        headerArea = {};
        toolbarArea = {};
        contentArea = area;
    }

    //== Content pages =========================================================
    table.setBounds (contentArea.reduced (8, 6));
    albumViewport.setBounds (contentArea.reduced (8, 6));
    if (albumGrid != nullptr)
        albumGrid->rebuildContent();

    playlistPanel->setBounds (area);
    queuePanel->setBounds (area);
    healthPanel->setBounds (area);

    if (libraryEmpty)
    {
        emptyStateArea = contentArea;
        auto block = contentArea.withSizeKeepingCentre (juce::jmin (juce::jmax (320, contentArea.getWidth() - 80), 660), 340);
        auto buttons = block.withTrimmedTop (178).withHeight (36);
        buttons = buttons.withSizeKeepingCentre (444, 36);
        importFolderButton.setBounds (buttons.removeFromLeft (218).withSizeKeepingCentre (214, 36));
        buttons.removeFromLeft (12);
        importFilesButton.setBounds (buttons.removeFromLeft (214).withSizeKeepingCentre (214, 36));
    }

    updatePageVisibility();
}

} // namespace sp
