/*
    Snake Puppy Reference Player - left navigation sidebar implementation.
*/
#include "UI/SidebarPanel.h"
#include <algorithm>
#include <map>

namespace sp
{

namespace
{
std::vector<TrackId> dragIds (const juce::var& description)
{
    std::vector<TrackId> ids;
    if (auto* obj = description.getDynamicObject())
    {
        if (obj->getProperty ("type").toString() != "sp.tracks")
            return ids;
        if (auto* arr = obj->getProperty ("ids").getArray())
            for (auto& v : *arr)
                ids.push_back ((TrackId) (juce::int64) v);
    }
    return ids;
}

bool caseInsensitiveLess (const juce::String& a, const juce::String& b)
{
    return a.compareIgnoreCase (b) < 0;
}

} // namespace

//==============================================================================
class SidebarPanel::Content : public juce::Component,
                             public juce::SettableTooltipClient
{
public:
    explicit Content (SidebarPanel& o) : owner (o)
    {
        setWantsKeyboardFocus (false);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);

        const auto clip = g.getClipBounds();
        for (size_t i = 0; i < owner.rows.size(); ++i)
        {
            const auto& row = owner.rows[i];
            if (! row.bounds.intersects (clip))
                continue;
            paintRow (g, row, (int) i);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int row = owner.rowAtY (e.y);
        if (row != owner.hoverRow)
        {
            owner.hoverRow = row;
            repaint();
        }

        if (row >= 0)
        {
            const auto& r = owner.rows[(size_t) row];
            setMouseCursor (r.type == Row::Type::section || r.type == Row::Type::hint
                                ? juce::MouseCursor::NormalCursor
                                : juce::MouseCursor::PointingHandCursor);
            if (r.type == Row::Type::child && r.selection.kind == SidebarPanel::Selection::Kind::folders)
                setTooltip (r.selection.value);
            else
                setTooltip ({});
        }
        else
        {
            setTooltip ({});
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (owner.hoverRow >= 0)
        {
            owner.hoverRow = -1;
            repaint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int rowIndex = owner.rowAtY (e.y);
        if (rowIndex < 0)
            return;

        auto& row = owner.rows[(size_t) rowIndex];

        if (e.mods.isPopupMenu())
        {
            if (row.type == Row::Type::playlist)
                owner.showPlaylistMenu (row.playlistId);
            else if (row.type == Row::Type::section && row.sectionName == "PLAYLISTS")
                owner.showPlaylistsSectionMenu();
            return;
        }

        // "+" button drawn on the PLAYLISTS section header.
        if (row.type == Row::Type::section && row.sectionName == "PLAYLISTS")
        {
            if (owner.getPlusButtonBounds().contains (e.getPosition()))
                if (owner.listener != nullptr)
                    owner.listener->sidebarCreatePlaylistRequested();
            return;
        }

        switch (row.type)
        {
            case Row::Type::section:
            case Row::Type::hint:
                return;
            case Row::Type::playlist:
                owner.selectRow (row);
                return;
            case Row::Type::nav:
                if (row.expandable)
                    owner.toggleExpanded (row.selection);
                else
                    owner.selectRow (row);
                return;
            case Row::Type::child:
                owner.selectRow (row);
                return;
        }
    }

private:
    void paintRow (juce::Graphics& g, const Row& row, int rowIndex)
    {
        if (row.type == Row::Type::section)
        {
            auto area = row.bounds.reduced (12, 0);
            g.setColour (theme::textFaint);
            g.setFont (theme::capsFont (9.0f, 0.18f));
            g.drawFittedText (row.label.toUpperCase(), area, juce::Justification::centredLeft, 1);

            if (row.sectionName == "PLAYLISTS")
            {
                auto plus = owner.getPlusButtonBounds();
                const bool hovered = plus.contains (getMouseXYRelative());
                g.setColour (hovered ? theme::gold : theme::silverDark);
                paintIcon (g, Icon::plus, plus.toFloat().reduced (2.0f), hovered ? theme::gold : theme::silverDark, 1.5f);
            }
            return;
        }

        if (row.type == Row::Type::hint)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::uiFont (11.5f));
            g.drawFittedText (row.label, row.bounds.reduced (16, 0).withTrimmedLeft (22),
                              juce::Justification::centredLeft, 1);
            return;
        }

        const bool selected = isSelected (row);
        const bool hovered = rowIndex == owner.hoverRow;

        if (selected)
        {
            g.setColour (theme::selectionFill);
            g.fillRect (row.bounds);
            g.setColour (theme::gold);
            g.fillRect (row.bounds.getX(), row.bounds.getY() + 3, 2, row.bounds.getHeight() - 6);
        }
        else if (hovered || rowIndex == owner.dragHoverRow)
        {
            g.setColour (rowIndex == owner.dragHoverRow ? theme::gold.withAlpha (0.10f) : theme::hoverFill);
            g.fillRect (row.bounds);
        }

        // Leading icons live at x=24..39. Text must start clear of them
        // (previously nav/playlist rows used 30/38, so the icon was drawn over
        // the first letter and made labels unreadable).
        const int left = row.type == Row::Type::child ? 12 + 22 + row.indent * 12
                        : 45;   // icon 24..39 + 6 px gap, for nav and playlist rows
        const int rowCentreY = row.bounds.getCentreY();

        // Expansion chevron.
        if (row.expandable)
        {
            auto chevron = juce::Rectangle<float> (13.0f, (float) rowCentreY - 4.5f, 9.0f, 9.0f);
            paintIcon (g, row.expanded ? Icon::chevronDown : Icon::chevronRight, chevron,
                       selected ? theme::gold : theme::silverDark, 1.4f);
        }

        // Leading icon.
        if (row.type == Row::Type::nav || row.type == Row::Type::playlist)
        {
            auto iconArea = juce::Rectangle<float> (24.0f, (float) rowCentreY - 7.5f, 15.0f, 15.0f);
            paintIcon (g, row.icon, iconArea,
                       selected ? theme::gold : hovered ? theme::silver : theme::silverDim, 1.4f);
        }
        else if (row.type == Row::Type::child && row.selection.kind == Selection::Kind::folders)
        {
            auto iconArea = juce::Rectangle<float> ((float) left - 18.0f, (float) rowCentreY - 6.5f, 13.0f, 13.0f);
            paintIcon (g, Icon::folder, iconArea, selected ? theme::gold : theme::silverDim, 1.2f);
        }
        else if (row.type == Row::Type::child)
        {
            g.setColour (selected ? theme::gold : theme::lineStrong);
            g.fillEllipse ((float) left - 11.0f, (float) rowCentreY - 1.5f, 3.0f, 3.0f);
        }

        // Label.
        auto textArea = row.bounds.withTrimmedLeft (left).withTrimmedRight (row.detail.isNotEmpty() ? 46 : 12);
        g.setColour (selected ? theme::goldBright
                              : row.type == Row::Type::child ? theme::textDim
                                                             : theme::silver);
        g.setFont (row.type == Row::Type::child ? theme::uiFont (11.5f)
                                                : theme::uiFont (12.0f, row.type == Row::Type::playlist));
        g.drawFittedText (row.label, textArea, juce::Justification::centredLeft, 1);

        if (row.detail.isNotEmpty())
        {
            g.setColour (selected ? theme::gold.withAlpha (0.75f) : theme::textFaint);
            g.setFont (theme::monoFont (9.5f));
            g.drawFittedText (row.detail, row.bounds.withTrimmedRight (10).withTrimmedLeft (row.bounds.getWidth() - 44),
                              juce::Justification::centredRight, 1);
        }
    }

    bool isSelected (const Row& row) const
    {
        const auto& sel = owner.selection;
        if (row.type == Row::Type::playlist)
            return sel.kind == Selection::Kind::playlist && sel.playlistId == row.playlistId;

        if (row.type == Row::Type::nav)
            return sel.kind == row.selection.kind
                   && (row.selection.kind == Selection::Kind::songs
                       || row.selection.kind == Selection::Kind::albums);

        return sel == row.selection;
    }

    SidebarPanel& owner;
};

//==============================================================================
SidebarPanel::SidebarPanel (AppContext& context) : ctx (context)
{
    content = std::make_unique<Content> (*this);
    addAndMakeVisible (viewport);
    viewport.setViewedComponent (content.get(), false);
    viewport.setScrollBarsShown (true, false, true, false);
    viewport.setScrollBarThickness (8);
    viewport.setColour (juce::ScrollBar::thumbColourId, theme::silverDark);

    ctx.playlists().addChangeListener (this);
    setWantsKeyboardFocus (false);

    refresh (true);
    startTimerHz (1);
}

SidebarPanel::~SidebarPanel()
{
    stopTimer();
    ctx.playlists().removeChangeListener (this);
    viewport.setViewedComponent (nullptr, false);
    content.reset();
}

void SidebarPanel::setListener (Listener* l) { listener = l; }

//==============================================================================
void SidebarPanel::refresh (bool force)
{
    auto newLibrary = ctx.library().getSnapshot();
    auto newPlaylists = ctx.playlists().getSnapshot();

    const auto libVersion = newLibrary != nullptr ? newLibrary->version : 0;
    const auto plVersion = newPlaylists != nullptr ? newPlaylists->version : 0;

    if (! force && libVersion == libraryVersion && plVersion == playlistVersion)
    {
        const int newQueueCount = ctx.engine().getStatus().queueSize;
        if (newQueueCount != queueCount)
        {
            queueCount = newQueueCount;
            repaint();
        }
        return;
    }

    libraryVersion = libVersion;
    playlistVersion = plVersion;
    snapshot = newLibrary;
    playlistsSnapshot = newPlaylists;
    queueCount = ctx.engine().getStatus().queueSize;

    // Drop a stale playlist selection.
    if (selection.kind == Selection::Kind::playlist)
    {
        bool found = false;
        if (playlistsSnapshot != nullptr)
            for (auto& p : playlistsSnapshot->playlists)
                if (p.id == selection.playlistId)
                    found = true;
        if (! found)
            selection = {};
    }

    rebuildAggregates();
    rebuildRows();
    updateContent();
    repaint();
}

void SidebarPanel::rebuildAggregates()
{
    artists.clear();
    genres.clear();
    formats.clear();
    years.clear();
    folders.clear();
    totalTracks = 0;
    albumCount = 0;

    if (snapshot == nullptr)
        return;

    std::map<juce::String, int> artistCounts, albumCounts, genreCounts, formatCounts, folderCounts;
    std::map<int, int> yearCounts;

    for (const auto& track : snapshot->tracks)
    {
        ++totalTracks;

        artistCounts[track.displayArtist().isNotEmpty() ? track.displayArtist() : "(Unknown Artist)"]++;
        albumCounts[track.displayAlbum() + "\x1f"
                    + (track.albumArtist.isNotEmpty() ? track.albumArtist : track.displayArtist())]++;
        genreCounts[track.displayGenre()]++;
        if (track.formatName.isNotEmpty())
            formatCounts[track.formatName.toUpperCase()]++;
        if (track.year > 0)
            yearCounts[track.year]++;
        if (track.folder.isNotEmpty())
            folderCounts[track.folder]++;
    }

    albumCount = (int) albumCounts.size();

    for (auto& entry : artistCounts) artists.push_back ({ entry.first, entry.second });
    for (auto& entry : genreCounts) genres.push_back ({ entry.first, entry.second });
    for (auto& entry : formatCounts) formats.push_back ({ entry.first, entry.second });

    std::sort (artists.begin(), artists.end(),
               [] (auto& a, auto& b) { return caseInsensitiveLess (a.value, b.value); });
    std::sort (genres.begin(), genres.end(),
               [] (auto& a, auto& b) { return caseInsensitiveLess (a.value, b.value); });
    std::sort (formats.begin(), formats.end(),
               [] (auto& a, auto& b) { return caseInsensitiveLess (a.value, b.value); });

    for (auto it = yearCounts.rbegin(); it != yearCounts.rend(); ++it)
        years.push_back (it->first);

    for (auto& entry : folderCounts)
    {
        FolderEntry folder;
        folder.path = entry.first;
        folder.count = entry.second;
        folder.label = juce::File (entry.first).getFileName();
        if (folder.label.isEmpty())
            folder.label = entry.first;

        folder.depth = 0;
        if (snapshot != nullptr)
        {
            for (const auto& location : snapshot->locations)
            {
                if (location.path.isEmpty())
                    continue;

                if (entry.first == location.path)
                {
                    folder.depth = 0;
                    break;
                }

                if (entry.first.startsWith (location.path + "/"))
                {
                    auto relative = entry.first.substring (location.path.length() + 1);
                    folder.depth = juce::StringArray::fromTokens (relative, "/", "").size();
                    break;
                }
            }
        }

        folders.push_back (std::move (folder));
    }
}

void SidebarPanel::rebuildRows()
{
    rows.clear();

    auto addSection = [this] (const juce::String& name)
    {
        Row row;
        row.type = Row::Type::section;
        row.label = name;
        row.sectionName = name;
        row.height = 30;
        rows.push_back (std::move (row));
    };

    auto addNav = [this] (Selection sel, const juce::String& label, Icon icon, int count,
                          bool expandable = false, bool expanded = false)
    {
        Row row;
        row.type = Row::Type::nav;
        row.selection = sel;
        row.label = label;
        row.icon = icon;
        row.detail = count >= 0 ? juce::String (count) : juce::String();
        row.expandable = expandable;
        row.expanded = expanded;
        row.height = 30;
        rows.push_back (std::move (row));
    };

    addSection ("LIBRARY");
    addNav ({ Selection::Kind::songs }, "Songs", Icon::music, totalTracks);
    addNav ({ Selection::Kind::albums }, "Albums", Icon::grid, albumCount);

    addNav ({ Selection::Kind::artists }, "Artists", Icon::person, (int) artists.size(),
            true, artistsExpanded);
    if (artistsExpanded)
        for (auto& entry : artists)
        {
            Row child;
            child.type = Row::Type::child;
            child.selection = { Selection::Kind::artists, entry.value };
            child.label = entry.value;
            child.detail = juce::String (entry.count);
            child.height = 24;
            rows.push_back (std::move (child));
        }

    addNav ({ Selection::Kind::genres }, "Genres", Icon::disc, (int) genres.size(),
            true, genresExpanded);
    if (genresExpanded)
        for (auto& entry : genres)
        {
            Row child;
            child.type = Row::Type::child;
            child.selection = { Selection::Kind::genres, entry.value };
            child.label = entry.value;
            child.detail = juce::String (entry.count);
            child.height = 24;
            rows.push_back (std::move (child));
        }

    addNav ({ Selection::Kind::years }, "Years", Icon::calendar, (int) years.size(),
            true, yearsExpanded);
    if (yearsExpanded)
        for (auto year : years)
        {
            Row child;
            child.type = Row::Type::child;
            child.selection = { Selection::Kind::years, juce::String (year) };
            child.label = juce::String (year);
            child.height = 24;
            rows.push_back (std::move (child));
        }

    addNav ({ Selection::Kind::formats }, "Formats", Icon::waveform, (int) formats.size(),
            true, formatsExpanded);

    addNav ({ Selection::Kind::folders }, "Folders", Icon::folder, (int) folders.size(),
            true, foldersExpanded);
    if (foldersExpanded)
        for (auto& folder : folders)
        {
            Row child;
            child.type = Row::Type::child;
            child.selection = { Selection::Kind::folders, folder.path };
            child.label = folder.label;
            child.detail = juce::String (folder.count);
            child.height = 24;
            child.indent = juce::jmin (folder.depth, 6);
            rows.push_back (std::move (child));
        }

    addSection ("PLAYLISTS");
    if (playlistsSnapshot == nullptr || playlistsSnapshot->playlists.empty())
    {
        Row hint;
        hint.type = Row::Type::hint;
        hint.label = "No playlists yet - click +";
        hint.height = 24;
        rows.push_back (std::move (hint));
    }
    else
    {
        for (auto& playlist : playlistsSnapshot->playlists)
        {
            Row row;
            row.type = Row::Type::playlist;
            row.selection = { Selection::Kind::playlist, {}, playlist.id };
            row.playlistId = playlist.id;
            row.label = playlist.name.isNotEmpty() ? playlist.name : "(Unnamed Playlist)";
            row.icon = playlist.smart ? Icon::star : Icon::list;
            row.detail = playlist.smart ? juce::String ("SMART")
                                        : juce::String ((int) playlist.trackIds.size());
            row.height = 27;
            rows.push_back (std::move (row));
        }
    }

    layoutRows();
}

void SidebarPanel::layoutRows()
{
    int y = 8;
    for (auto& row : rows)
    {
        row.bounds = { 0, y, juce::jmax (100, content != nullptr ? content->getWidth() : 0), row.height };
        y += row.height;
    }
    contentHeight = y + 14;
}

void SidebarPanel::updateContent()
{
    layoutRows();
    const int width = juce::jmax (120, viewport.getMaximumVisibleWidth());
    if (content != nullptr)
    {
        content->setSize (width, juce::jmax (contentHeight, viewport.getHeight()));
        layoutRows();
        content->repaint();
    }
}

juce::Rectangle<int> SidebarPanel::getPlusButtonBounds() const
{
    for (auto& row : rows)
        if (row.type == Row::Type::section && row.sectionName == "PLAYLISTS")
            return { row.bounds.getRight() - 28, row.bounds.getY() + 8, 16, 16 };
    return {};
}

int SidebarPanel::rowAtY (int y) const
{
    for (int i = 0; i < (int) rows.size(); ++i)
        if (rows[(size_t) i].bounds.contains (rows[(size_t) i].bounds.getX() + 2, y))
            return i;
    return -1;
}

bool SidebarPanel::isExpandedFor (Selection::Kind kind) const
{
    switch (kind)
    {
        case Selection::Kind::artists: return artistsExpanded;
        case Selection::Kind::genres:  return genresExpanded;
        case Selection::Kind::years:   return yearsExpanded;
        case Selection::Kind::formats: return formatsExpanded;
        case Selection::Kind::folders: return foldersExpanded;
        default: return false;
    }
}

void SidebarPanel::toggleExpanded (const Selection& sel)
{
    switch (sel.kind)
    {
        case Selection::Kind::artists: artistsExpanded = ! artistsExpanded; break;
        case Selection::Kind::genres:  genresExpanded = ! genresExpanded; break;
        case Selection::Kind::years:   yearsExpanded = ! yearsExpanded; break;
        case Selection::Kind::formats: formatsExpanded = ! formatsExpanded; break;
        case Selection::Kind::folders: foldersExpanded = ! foldersExpanded; break;
        default: return;
    }
    rebuildRows();
    updateContent();
}

void SidebarPanel::expandSection (Selection::Kind kind)
{
    switch (kind)
    {
        case Selection::Kind::artists: artistsExpanded = true; break;
        case Selection::Kind::genres:  genresExpanded = true; break;
        case Selection::Kind::years:   yearsExpanded = true; break;
        case Selection::Kind::formats: formatsExpanded = true; break;
        case Selection::Kind::folders: foldersExpanded = true; break;
        default: return;
    }
    rebuildRows();
    updateContent();
}

void SidebarPanel::collapseAllSections()
{
    artistsExpanded = genresExpanded = yearsExpanded = formatsExpanded = foldersExpanded = false;
    rebuildRows();
    updateContent();
}

void SidebarPanel::selectRow (const Row& row)
{
    const auto newSelection = row.selection;
    const bool changed = newSelection != selection;
    selection = newSelection;
    content->repaint();
    repaint();
    if (changed && listener != nullptr)
        listener->sidebarSelectionChanged (selection);
}

void SidebarPanel::setSelection (const Selection& newSelection, bool scrollIntoView)
{
    selection = newSelection;
    content->repaint();
    repaint();

    if (scrollIntoView)
    {
        for (int i = 0; i < (int) rows.size(); ++i)
        {
            const auto& row = rows[(size_t) i];
            bool matches = false;
            if (selection.kind == Selection::Kind::playlist)
                matches = row.type == Row::Type::playlist && row.playlistId == selection.playlistId;
            else if (row.type == Row::Type::nav || row.type == Row::Type::child)
                matches = row.selection == selection;

            if (matches)
            {
                const int top = row.bounds.getY();
                if (top < viewport.getViewPositionY())
                    viewport.setViewPosition (0, top);
                else if (top + row.height > viewport.getViewPositionY() + viewport.getViewHeight())
                    viewport.setViewPosition (0, top + row.height - viewport.getViewHeight() + 4);
                break;
            }
        }
    }
}

//==============================================================================
void SidebarPanel::showPlaylistMenu (PlaylistId playlistId)
{
    juce::PopupMenu menu;
    menu.addItem (1, "Play");
    menu.addItem (2, "Add to Queue");
    menu.addSeparator();
    menu.addItem (3, "Rename...");
    menu.addItem (4, "Duplicate");
    menu.addItem (5, "Export Playlist...");
    menu.addSeparator();
    menu.addItem (6, "Delete...");

    juce::Component::SafePointer<SidebarPanel> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (content.get()),
                        [safe, playlistId] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;
                            auto* l = safe->listener;
                            if (l == nullptr)
                                return;
                            switch (result)
                            {
                                case 1: l->sidebarPlayPlaylistRequested (playlistId); break;
                                case 2: l->sidebarQueuePlaylistRequested (playlistId); break;
                                case 3: l->sidebarRenamePlaylistRequested (playlistId); break;
                                case 4: l->sidebarDuplicatePlaylistRequested (playlistId); break;
                                case 5: l->sidebarExportPlaylistRequested (playlistId); break;
                                case 6: l->sidebarDeletePlaylistRequested (playlistId); break;
                                default: break;
                            }
                        });
}

void SidebarPanel::showPlaylistsSectionMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "New Playlist...");
    menu.addItem (2, "New Smart Playlist...");
    menu.addSeparator();
    menu.addItem (3, "Import Playlist...");

    juce::Component::SafePointer<SidebarPanel> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (content.get()),
                        [safe] (int result)
                        {
                            if (safe == nullptr || safe->listener == nullptr)
                                return;
                            switch (result)
                            {
                                case 1: safe->listener->sidebarCreatePlaylistRequested(); break;
                                case 2: safe->listener->sidebarCreateSmartPlaylistRequested(); break;
                                case 3: safe->listener->sidebarImportPlaylistRequested(); break;
                                default: break;
                            }
                        });
}

//==============================================================================
void SidebarPanel::paint (juce::Graphics& g)
{
    juce::Rectangle<int> area = getLocalBounds();
    juce::ColourGradient grad (theme::panel.brighter (0.02f), 0.0f, 0.0f,
                               theme::panel.darker (0.12f), 0.0f, (float) area.getHeight(), false);
    g.setGradientFill (grad);
    g.fillRect (area);

    // Right edge.
    g.setColour (theme::lineStrong);
    g.drawVerticalLine (area.getRight() - 1, 0.0f, (float) area.getHeight());

    //== Brand plate =========================================================
    if (! brandArea.isEmpty())
    {
        juce::ColourGradient brandGrad (theme::panel.brighter (0.04f), 0.0f, (float) brandArea.getY(),
                                        theme::panel.darker (0.10f), 0.0f, (float) brandArea.getBottom(), false);
        g.setGradientFill (brandGrad);
        g.fillRect (brandArea);

        g.setColour (theme::gold);
        g.fillRect (brandArea.getX() + 16, brandArea.getY() + 18, 3, 22);

        g.setColour (theme::silver);
        g.setFont (theme::uiFont (15.0f, true, 0.20f));
        g.drawText ("SNAKE PUPPY", brandArea.withTrimmedLeft (28).withTrimmedTop (12).withHeight (24),
                    juce::Justification::centredLeft, false);

        g.setColour (theme::gold.withAlpha (0.9f));
        g.setFont (theme::capsFont (8.5f, 0.24f));
        g.drawText ("REFERENCE PLAYER", brandArea.withTrimmedLeft (29).withTrimmedTop (34).withHeight (16),
                    juce::Justification::centredLeft, false);

        g.setColour (theme::lineStrong);
        g.drawHorizontalLine (brandArea.getBottom() - 1, (float) brandArea.getX(), (float) brandArea.getRight());
    }

    paintFooter (g);
}

void SidebarPanel::paintFooter (juce::Graphics& g)
{
    const auto footer = footerArea;
    if (footer.isEmpty())
        return;

    g.setColour (theme::panel.darker (0.25f));
    g.fillRect (footer);
    g.setColour (theme::lineStrong);
    g.drawHorizontalLine (footer.getY(), 0.0f, (float) footer.getWidth());

    auto paintFooterRow = [&] (juce::Rectangle<int> rowArea, Icon icon, const juce::String& label,
                               const juce::String& detail, bool selected)
    {
        if (selected)
        {
            g.setColour (theme::selectionFill);
            g.fillRect (rowArea);
            g.setColour (theme::gold);
            g.fillRect (rowArea.getX(), rowArea.getY() + 4, 2, rowArea.getHeight() - 8);
        }
        paintIcon (g, icon, { 24.0f, (float) rowArea.getCentreY() - 7.5f, 15.0f, 15.0f },
                   selected ? theme::gold : theme::silverDim, 1.4f);
        g.setColour (selected ? theme::goldBright : theme::silver);
        g.setFont (theme::uiFont (12.0f));
        g.drawFittedText (label, rowArea.withTrimmedLeft (48).withTrimmedRight (44),
                          juce::Justification::centredLeft, 1);
        g.setColour (theme::textFaint);
        g.setFont (theme::monoFont (9.5f));
        g.drawFittedText (detail, rowArea.withTrimmedRight (10),
                          juce::Justification::centredRight, 1);
    };

    paintFooterRow (queueRowArea, Icon::queue, "Queue", juce::String (queueCount),
                    selection.kind == Selection::Kind::queue);
    paintFooterRow (healthRowArea, Icon::health, "Library Health", {},
                    selection.kind == Selection::Kind::health);

    g.setColour (theme::line);
    g.drawHorizontalLine (healthRowArea.getY(), 12.0f, (float) footer.getWidth() - 12.0f);
}

void SidebarPanel::resized()
{
    auto area = getLocalBounds();
    const int footerHeight = 78;
    footerArea = area.removeFromBottom (footerHeight);
    brandArea = area.removeFromTop (58);

    queueRowArea = footerArea.withHeight (34).translated (0, 6);
    healthRowArea = footerArea.withHeight (34).translated (0, 40);

    viewport.setBounds (area);
    updateContent();
}

void SidebarPanel::mouseDown (const juce::MouseEvent& e)
{
    if (queueRowArea.contains (e.getPosition()) || healthRowArea.contains (e.getPosition()))
    {
        selection = {};
        selection.kind = queueRowArea.contains (e.getPosition()) ? Selection::Kind::queue
                                                                  : Selection::Kind::health;
        content->repaint();
        repaint();
        if (listener != nullptr)
            listener->sidebarSelectionChanged (selection);
    }
}

//==============================================================================
void SidebarPanel::timerCallback()
{
    refresh (false);
}

void SidebarPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh (true);
}

//==============================================================================
bool SidebarPanel::isInterestedInDragSource (const SourceDetails& details)
{
    return details.description.getDynamicObject() != nullptr
           && details.description.getDynamicObject()->getProperty ("type").toString() == "sp.tracks";
}

void SidebarPanel::itemDragEnter (const SourceDetails& details)
{
    const auto pos = content->getLocalPoint (this, details.localPosition);
    const int row = rowAtY (pos.y);
    dragHoverRow = (row >= 0 && rows[(size_t) row].type == Row::Type::playlist) ? row : -1;
    content->repaint();
}

void SidebarPanel::itemDragMove (const SourceDetails& details)
{
    itemDragEnter (details);
}

void SidebarPanel::itemDragExit (const SourceDetails&)
{
    dragHoverRow = -1;
    content->repaint();
}

void SidebarPanel::itemDropped (const SourceDetails& details)
{
    const auto pos = content->getLocalPoint (this, details.localPosition);
    const int row = rowAtY (pos.y);
    dragHoverRow = -1;
    content->repaint();

    if (row < 0 || rows[(size_t) row].type != Row::Type::playlist || listener == nullptr)
        return;

    const auto ids = dragIds (details.description);
    if (! ids.empty())
        listener->sidebarAddTracksToPlaylist (rows[(size_t) row].playlistId, ids);
}

} // namespace sp
