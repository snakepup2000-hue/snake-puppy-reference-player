/*
    Snake Puppy Reference Player - track table implementation.
*/
#include "UI/TrackTableComponent.h"

namespace sp
{

namespace
{
int compareText (const juce::String& a, const juce::String& b)
{
    if (a.isEmpty() && b.isEmpty()) return 0;
    if (a.isEmpty()) return 1;      // empty values sort last
    if (b.isEmpty()) return -1;
    return a.compareIgnoreCase (b);
}

int compareInts (juce::int64 a, juce::int64 b)
{
    return a < b ? -1 : (a > b ? 1 : 0);
}

std::vector<TrackId> dragTrackIds (const juce::var& description)
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

bool dragIsInternalReorder (const juce::var& description)
{
    if (auto* obj = description.getDynamicObject())
        return obj->getProperty ("type").toString() == "sp.tracks"
               && (bool) obj->getProperty ("reorder");
    return false;
}

juce::String dragSourceId (const juce::var& description)
{
    if (auto* obj = description.getDynamicObject())
        return obj->getProperty ("source").toString();
    return {};
}
} // namespace

//==============================================================================
const char* TrackTableComponent::columnName (Column column)
{
    switch (column)
    {
        case Column::title:    return "TITLE";
        case Column::artist:   return "ARTIST";
        case Column::album:    return "ALBUM";
        case Column::genre:    return "GENRE";
        case Column::year:     return "YEAR";
        case Column::format:   return "FORMAT";
        case Column::rate:     return "RATE";
        case Column::bits:     return "BITS";
        case Column::duration: return "LENGTH";
        case Column::rating:   return "RATING";
        default:               return "";
    }
}

//==============================================================================
class TrackTableComponent::Rows : public juce::Component,
                                  public juce::DragAndDropTarget
{
public:
    explicit Rows (TrackTableComponent& o) : owner (o)
    {
        setWantsKeyboardFocus (true);
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    //== Painting =============================================================
    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);

        const int rowH = theme::tableRowHeight;
        const int numRows = (int) owner.sortedIndices.size();
        if (numRows == 0)
            return;

        const auto clip = g.getClipBounds();
        const int first = juce::jlimit (0, numRows - 1, clip.getY() / rowH);
        const int last = juce::jlimit (0, numRows - 1, (clip.getBottom() - 1) / rowH);

        for (int row = first; row <= last; ++row)
            paintRow (g, row, juce::Rectangle<int> (0, row * rowH, getWidth(), rowH));
    }

    //== Mouse ================================================================
    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        const int row = rowAt (e.y);
        const int numRows = (int) owner.sortedIndices.size();

        if (row < 0 || row >= numRows)
        {
            owner.clearSelection();
            return;
        }

        // Rating stars are editable directly in the table.
        if (owner.columnAtX (e.x) == (int) Column::rating)
        {
            owner.selectRow (row, ! e.mods.isShiftDown() && ! e.mods.isCommandDown() && ! e.mods.isCtrlDown());
            hitRating (row, e.x);
            return;
        }

        dragStartRow = row;
        dragStartPos = e.getPosition();
        dragStarted = false;
        anchorPicked = true;

        if (e.mods.isShiftDown() && owner.anchorRow >= 0)
        {
            owner.selectRange (owner.anchorRow, row);
        }
        else if (e.mods.isCommandDown() || e.mods.isCtrlDown())
        {
            owner.toggleRowSelection (row);
            owner.anchorRow = row;
        }
        else
        {
            owner.selectRow (row);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragStarted || dragStartRow < 0)
            return;

        if (! e.mouseWasDraggedSinceMouseDown()
            && e.getDistanceFromDragStart() < 6)
            return;

        if (owner.selected.empty())
            return;

        dragStarted = true;

        juce::DynamicObject::Ptr desc (new juce::DynamicObject());
        desc->setProperty ("type", "sp.tracks");
        desc->setProperty ("reorder", owner.reorderMode);

        juce::Array<juce::var> ids;
        for (auto id : owner.selected)
            ids.add ((juce::int64) id);
        desc->setProperty ("ids", ids);
        desc->setProperty ("source", owner.getTableId());
        auto selectedTracks = owner.getSelectedTracks();
        desc->setProperty ("label",
                           selectedTracks.size() == 1 && ! selectedTracks.empty()
                               ? selectedTracks.front().displayTitle()
                               : juce::String (owner.selected.size()) + " tracks");

        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
            container->startDragging (juce::var (desc.get()), this);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragStartRow = -1;
        dragStarted = false;
        anchorPicked = false;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int row = rowAt (e.y);
        if (auto* track = owner.getTrackForRow (row))
            if (owner.listener != nullptr)
                owner.listener->trackDoubleClicked (track->id);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int row = rowAt (e.y);
        if (row != hoverRow)
        {
            hoverRow = row;
            repaint();
        }

        setMouseCursor (row >= 0 && row < (int) owner.sortedIndices.size()
                            ? juce::MouseCursor::PointingHandCursor
                            : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverRow >= 0)
        {
            hoverRow = -1;
            repaint();
        }
    }

    //== Keyboard =============================================================
    bool keyPressed (const juce::KeyPress& key) override
    {
        const int numRows = (int) owner.sortedIndices.size();
        if (numRows == 0)
            return false;

        const int current = owner.anchorRow >= 0 ? owner.anchorRow
                                                 : (owner.selected.empty() ? -1
                                                                           : owner.getRowForTrack (owner.selected.front()));

        if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey)
        {
            const int step = key == juce::KeyPress::upKey ? -1 : 1;
            int target = juce::jlimit (0, numRows - 1, (current < 0 ? (step > 0 ? -1 : numRows) : current) + step);

            if (key.getModifiers().isShiftDown() && owner.anchorRow >= 0)
            {
                owner.selectRange (owner.anchorRow, target);
            }
            else
            {
                owner.selectRow (target);
            }

            owner.anchorRow = target;
            owner.scrollToRow (target);
            return true;
        }

        if (key == juce::KeyPress::returnKey)
        {
            if (auto* track = owner.getTrackForRow (current))
                if (owner.listener != nullptr)
                    owner.listener->trackDoubleClicked (track->id);
            return true;
        }

        if (key == juce::KeyPress (juce::KeyPress::deleteKey)
            || key == juce::KeyPress (juce::KeyPress::backspaceKey))
        {
            if (owner.listener != nullptr && ! owner.selected.empty())
                owner.listener->tracksRemoveRequested (owner.selected);
            return true;
        }

        if (key == juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0))
        {
            owner.selectAll();
            return true;
        }

        if (key.getKeyCode() == juce::KeyPress::escapeKey)
        {
            owner.clearSelection();
            return true;
        }

        return false;
    }

    //== Internal drag reordering (playlists) =================================
    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        return owner.reorderMode
               && dragIsInternalReorder (details.description)
               && dragSourceId (details.description) == owner.getTableId();
    }

    void itemDragEnter (const SourceDetails& details) override { updateDropTarget (details); }
    void itemDragMove (const SourceDetails& details) override { updateDropTarget (details); }
    void itemDragExit (const SourceDetails&) override { setDropRow (-1); }

    void itemDropped (const SourceDetails& details) override
    {
        const int target = dropRow;
        setDropRow (-1);
        const auto ids = dragTrackIds (details.description);
        if (! ids.empty() && owner.listener != nullptr)
            owner.listener->tracksDroppedOnRow (ids, juce::jlimit (0, (int) owner.sortedIndices.size(), target));
    }

    //== Helpers ==============================================================
    int rowAt (int y) const
    {
        const int row = y / theme::tableRowHeight;
        if (row < 0 || row >= (int) owner.sortedIndices.size())
            return -1;
        return row;
    }

    int hitRow = -1;

private:
    void hitRating (int row, int x)
    {
        auto* track = owner.getTrackForRow (row);
        if (track == nullptr || owner.listener == nullptr)
            return;

        const int cellX = owner.columnX ((int) Column::rating);
        const int cellW = owner.columnWidths[(int) Column::rating];
        const float starSize = 11.0f, gap = 2.0f;
        const float total = starSize * 5.0f + gap * 4.0f;
        const float startX = (float) cellX + 8.0f;
        juce::ignoreUnused (cellW);

        int star = (int) std::floor (((float) x - startX) / (starSize + gap));
        star = juce::jlimit (0, 4, star);
        int rating = star + 1;
        if (track->rating == rating)
            rating = 0;

        owner.listener->trackRatingChanged (track->id, rating);
    }

    void updateDropTarget (const SourceDetails& details)
    {
        const int row = rowAt (details.localPosition.y);
        setDropRow (row < 0 ? (int) owner.sortedIndices.size() : row);
    }

    void setDropRow (int row)
    {
        if (dropRow != row)
        {
            dropRow = row;
            repaint();
        }
    }

    void paintRow (juce::Graphics& g, int row, juce::Rectangle<int> rect)
    {
        auto* track = owner.getTrackForRow (row);
        if (track == nullptr)
            return;

        const bool isSelected = std::find (owner.selected.begin(), owner.selected.end(), track->id)
                                != owner.selected.end();
        const bool isPlaying = track->id == owner.playingTrackId;

        if (isSelected)
        {
            g.setColour (theme::selectionFill);
            g.fillRect (rect);
            g.setColour (theme::gold);
            g.fillRect (rect.getX(), rect.getY(), 2, rect.getHeight());
        }
        else if (isPlaying)
        {
            g.setColour (theme::gold.withAlpha (0.06f));
            g.fillRect (rect);
            g.setColour (theme::gold);
            g.fillRect (rect.getX(), rect.getY(), 2, rect.getHeight());
        }
        else if (row % 2 == 1)
        {
            g.setColour (theme::rowAlternate);
            g.fillRect (rect);
        }

        if (row == hoverRow && ! isSelected)
        {
            g.setColour (theme::hoverFill);
            g.fillRect (rect);
        }

        if (row == dropRow)
        {
            g.setColour (theme::gold);
            g.fillRect (rect.getX(), rect.getY(), rect.getWidth(), 2);
        }

        // Row separator.
        g.setColour (theme::lineFaint.withAlpha (0.55f));
        g.fillRect (rect.getX() + 8, rect.getBottom() - 1, rect.getWidth() - 16, 1);

        int x = 0;
        for (int c = 0; c < (int) Column::numColumns; ++c)
        {
            const int w = owner.columnWidths[(size_t) c];
            juce::Rectangle<int> cell (x, rect.getY(), w, rect.getHeight());
            paintCell (g, *track, (Column) c, cell, isSelected, isPlaying);
            x += w;
        }
    }

    void paintCell (juce::Graphics& g, const Track& track, Column column,
                    juce::Rectangle<int> cell, bool selected, bool isPlaying)
    {
        auto inner = cell.reduced (8, 0);
        const bool missing = track.missing;

        juce::Colour primary = selected ? theme::text : theme::text.brighter (0.02f);
        juce::Colour secondary = selected ? theme::silver : theme::textDim;

        if (missing)
        {
            primary = theme::error;
            secondary = theme::error.withAlpha (0.65f);
        }

        if (column == Column::title)
        {
            int textX = inner.getX();
            if (isPlaying)
            {
                drawPlayingGlyph (g, { (float) inner.getX(), (float) inner.getY() + 7.0f, 12.0f, 14.0f },
                                  owner.playing, theme::gold);
                textX += 18;
            }
            else if (missing)
            {
                paintIcon (g, Icon::warning, { (float) inner.getX(), (float) inner.getY() + 8.0f, 12.0f, 12.0f },
                           theme::error, 1.4f);
                textX += 18;
            }

            auto textArea = inner.withTrimmedLeft (textX - inner.getX());
            g.setColour (isPlaying ? theme::goldBright : primary);
            g.setFont (theme::uiFont (12.5f, isPlaying));
            g.drawFittedText (track.displayTitle(), textArea, juce::Justification::centredLeft, 1);
            return;
        }

        switch (column)
        {
            case Column::artist:
                g.setColour (secondary);
                g.setFont (theme::uiFont (12.0f));
                g.drawFittedText (track.displayArtist(), inner, juce::Justification::centredLeft, 1);
                break;
            case Column::album:
                g.setColour (secondary);
                g.setFont (theme::uiFont (12.0f));
                g.drawFittedText (track.displayAlbum(), inner, juce::Justification::centredLeft, 1);
                break;
            case Column::genre:
                g.setColour (secondary);
                g.setFont (theme::uiFont (11.5f));
                g.drawFittedText (track.displayGenre(), inner, juce::Justification::centredLeft, 1);
                break;
            case Column::year:
                g.setColour (secondary);
                g.setFont (theme::monoFont (11.0f));
                g.drawFittedText (track.year > 0 ? juce::String (track.year) : juce::String ("-"),
                                  inner, juce::Justification::centredLeft, 1);
                break;
            case Column::format:
            {
                juce::Colour fmt = secondary;
                if (! missing)
                {
                    if (track.isDsd) fmt = theme::gold.withAlpha (0.9f);
                    else if (track.family == Track::Family::lossy) fmt = secondary.withAlpha (0.8f);
                    else fmt = theme::silverDim;
                }
                g.setColour (fmt);
                g.setFont (theme::capsFont (9.5f, 0.06f));
                g.drawFittedText (track.formatName.isNotEmpty() ? track.formatName.toUpperCase()
                                                                : juce::String ("-"),
                                  inner, juce::Justification::centredLeft, 1);
                break;
            }
            case Column::rate:
                g.setColour (secondary);
                g.setFont (theme::monoFont (10.5f));
                g.drawFittedText (formatSampleRate (track.sampleRate), inner, juce::Justification::centredLeft, 1);
                break;
            case Column::bits:
                g.setColour (secondary);
                g.setFont (theme::monoFont (10.5f));
                g.drawFittedText (track.isDsd ? formatBitDepth (0, true, track.dsdRateMultiple)
                                              : formatBitDepth (track.bitDepth),
                                  inner, juce::Justification::centredLeft, 1);
                break;
            case Column::duration:
                g.setColour (secondary);
                g.setFont (theme::monoFont (11.0f));
                g.drawFittedText (formatDuration (track.durationMs), inner, juce::Justification::right, 1);
                break;
            case Column::rating:
                drawStars (g, { (float) inner.getX() + 8.0f, (float) inner.getCentreY() - 5.0f,
                                90.0f, 11.0f },
                           track.rating, theme::gold, theme::lineStrong, 11.0f);
                break;
            default:
                break;
        }
    }

    TrackTableComponent& owner;
    int hoverRow = -1;
    int dropRow = -1;
    int dragStartRow = -1;
    juce::Point<int> dragStartPos;
    bool dragStarted = false;
    bool anchorPicked = false;
};

//==============================================================================
TrackTableComponent::TrackTableComponent()
{
    tableId = "tt-" + juce::String::toHexString ((juce::pointer_sized_int) this);
    rows = std::make_unique<Rows> (*this);
    addAndMakeVisible (viewport);
    viewport.setViewedComponent (rows.get(), false);
    viewport.setScrollBarsShown (true, false, true, false);
    viewport.setScrollBarThickness (9);
    viewport.setColour (juce::ScrollBar::thumbColourId, theme::silverDark);
    setWantsKeyboardFocus (false);
}

TrackTableComponent::~TrackTableComponent()
{
    viewport.setViewedComponent (nullptr, false);
    rows.reset();
}

void TrackTableComponent::setRowSource (RowSource newSource)
{
    source = std::move (newSource);
    rebuildSortedIndices();

    // Keep the selection for tracks that survived the filter update.
    if (! selected.empty())
    {
        std::vector<TrackId> kept;
        for (auto id : selected)
            if (getRowForTrack (id) >= 0)
                kept.push_back (id);
        selected = std::move (kept);
    }

    if (selected.empty())
        anchorRow = -1;

    updateContentSize();
    if (rows != nullptr)
        rows->repaint();
    repaint();
}

void TrackTableComponent::setEmptyMessage (const juce::String& message, const juce::String& subMessage)
{
    emptyMessage = message;
    emptySubMessage = subMessage;
    repaint();
}

void TrackTableComponent::setPlayingTrack (TrackId trackId, bool isPlaying)
{
    if (playingTrackId == trackId && playing == isPlaying)
        return;
    playingTrackId = trackId;
    playing = isPlaying;
    if (rows != nullptr)
        rows->repaint();
    repaint();
}

void TrackTableComponent::scrollToPlayingTrack()
{
    const int row = getRowForTrack (playingTrackId);
    if (row >= 0)
        scrollToRow (row);
}

void TrackTableComponent::setSort (Column column, bool descending)
{
    if (sortColumn == column && sortDescending == descending)
        return;
    sortColumn = column;
    sortDescending = descending;
    rebuildSortedIndices();
    updateContentSize();
    repaint();
}

void TrackTableComponent::setReorderMode (bool shouldReorder)
{
    if (reorderMode == shouldReorder)
        return;
    reorderMode = shouldReorder;
    rebuildSortedIndices();
    updateContentSize();
    repaint();
}

void TrackTableComponent::setListener (Listener* newListener) { listener = newListener; }

std::vector<Track> TrackTableComponent::getSelectedTracks() const
{
    std::vector<Track> result;
    if (source.snapshot == nullptr)
        return result;

    for (auto id : selected)
        if (const auto* track = source.snapshot->findTrack (id))
            result.push_back (*track);
    return result;
}

const Track* TrackTableComponent::getTrackForRow (int row) const
{
    if (row < 0 || row >= (int) sortedIndices.size() || source.snapshot == nullptr)
        return nullptr;

    const int index = sortedIndices[(size_t) row];
    if (index < 0 || index >= (int) source.snapshot->tracks.size())
        return nullptr;
    return &source.snapshot->tracks[(size_t) index];
}

int TrackTableComponent::getRowForTrack (TrackId id) const
{
    for (int row = 0; row < (int) sortedIndices.size(); ++row)
        if (auto* track = getTrackForRow (row))
            if (track->id == id)
                return row;
    return -1;
}

void TrackTableComponent::selectRow (int row, bool clearOthers)
{
    auto* track = getTrackForRow (row);
    if (track == nullptr)
        return;

    if (clearOthers)
        selected.clear();

    if (std::find (selected.begin(), selected.end(), track->id) == selected.end())
        selected.push_back (track->id);

    anchorRow = row;
    if (rows != nullptr)
        rows->repaint();
    notifySelectionChanged();
}

void TrackTableComponent::selectRange (int fromRow, int toRow)
{
    if (fromRow > toRow)
        std::swap (fromRow, toRow);

    fromRow = juce::jmax (0, fromRow);
    toRow = juce::jmin ((int) sortedIndices.size() - 1, toRow);
    if (fromRow > toRow)
        return;

    selected.clear();
    for (int row = fromRow; row <= toRow; ++row)
        if (auto* track = getTrackForRow (row))
            selected.push_back (track->id);

    if (rows != nullptr)
        rows->repaint();
    notifySelectionChanged();
}

void TrackTableComponent::toggleRowSelection (int row)
{
    auto* track = getTrackForRow (row);
    if (track == nullptr)
        return;

    auto it = std::find (selected.begin(), selected.end(), track->id);
    if (it != selected.end())
        selected.erase (it);
    else
        selected.push_back (track->id);

    if (rows != nullptr)
        rows->repaint();
    notifySelectionChanged();
}

void TrackTableComponent::selectAll()
{
    selected.clear();
    for (int row = 0; row < (int) sortedIndices.size(); ++row)
        if (auto* track = getTrackForRow (row))
            selected.push_back (track->id);

    if (rows != nullptr)
        rows->repaint();
    notifySelectionChanged();
}

void TrackTableComponent::clearSelection()
{
    if (selected.empty() && anchorRow < 0)
        return;
    selected.clear();
    anchorRow = -1;
    if (rows != nullptr)
        rows->repaint();
    notifySelectionChanged();
}

void TrackTableComponent::notifySelectionChanged()
{
    if (listener != nullptr)
        listener->trackSelectionChanged (*this);
}

void TrackTableComponent::scrollToRow (int row)
{
    if (row < 0)
        return;

    const int rowH = theme::tableRowHeight;
    const int y = row * rowH;
    if (y < viewport.getViewPositionY())
        viewport.setViewPosition (0, y);
    else if (y + rowH > viewport.getViewPositionY() + viewport.getViewHeight() - 4)
        viewport.setViewPosition (0, y + rowH - viewport.getViewHeight() + 4);
}

//== Private implementation helpers ===========================================
void TrackTableComponent::rebuildSortedIndices()
{
    sortedIndices = source.indices;

    if (reorderMode || source.snapshot == nullptr)
        return;

    auto* snapshot = source.snapshot.get();

    auto compareDefault = [snapshot] (int ia, int ib) -> int
    {
        const auto& a = snapshot->tracks[(size_t) ia];
        const auto& b = snapshot->tracks[(size_t) ib];
        int r = compareText (a.displayArtist(), b.displayArtist());
        if (r == 0) r = compareText (a.displayAlbum(), b.displayAlbum());
        if (r == 0) r = compareInts (a.discNumber, b.discNumber);
        if (r == 0) r = compareInts (a.trackNumber, b.trackNumber);
        if (r == 0) r = compareText (a.displayTitle(), b.displayTitle());
        if (r == 0) r = compareInts (a.id, b.id);
        return r;
    };

    auto compareColumn = [snapshot, this] (int ia, int ib) -> int
    {
        const auto& a = snapshot->tracks[(size_t) ia];
        const auto& b = snapshot->tracks[(size_t) ib];

        switch (sortColumn)
        {
            case Column::title:    return compareText (a.displayTitle(), b.displayTitle());
            case Column::artist:   return compareText (a.displayArtist(), b.displayArtist());
            case Column::album:    return compareText (a.displayAlbum(), b.displayAlbum());
            case Column::genre:    return compareText (a.displayGenre(), b.displayGenre());
            case Column::year:     return compareInts (a.year, b.year);
            case Column::format:   return compareText (a.formatName, b.formatName);
            case Column::rate:     return compareInts (a.sampleRate, b.sampleRate);
            case Column::bits:     return compareInts (a.bitDepth, b.bitDepth);
            case Column::duration: return compareInts (a.durationMs, b.durationMs);
            case Column::rating:   return compareInts (a.rating, b.rating);
            default:               return 0;
        }
    };

    std::stable_sort (sortedIndices.begin(), sortedIndices.end(),
                      [&] (int ia, int ib)
                      {
                          int r = compareColumn (ia, ib);
                          if (sortDescending)
                              r = -r;
                          if (r == 0)
                              r = compareDefault (ia, ib);
                          return r < 0;
                      });
}

void TrackTableComponent::updateContentSize()
{
    if (rows == nullptr)
        return;

    const int width = juce::jmax (100, viewport.getMaximumVisibleWidth());
    const int height = juce::jmax (1, (int) sortedIndices.size() * theme::tableRowHeight);
    rows->setSize (width, height);
}

void TrackTableComponent::layoutColumns()
{
    const int total = juce::jmax (300, getWidth());

    columnWidths.fill (0);
    columnWidths[(size_t) Column::year] = 52;
    columnWidths[(size_t) Column::format] = 66;
    columnWidths[(size_t) Column::rate] = 84;
    columnWidths[(size_t) Column::bits] = 54;
    columnWidths[(size_t) Column::duration] = 58;
    columnWidths[(size_t) Column::rating] = 86;

    int fixed = 0;
    for (auto w : columnWidths)
        fixed += w;

    int remaining = juce::jmax (120, total - fixed);
    int title = (int) (remaining * 0.36f);
    int artist = (int) (remaining * 0.24f);
    int album = (int) (remaining * 0.28f);
    int genre = remaining - title - artist - album;

    columnWidths[(size_t) Column::title] = title;
    columnWidths[(size_t) Column::artist] = artist;
    columnWidths[(size_t) Column::album] = album;
    columnWidths[(size_t) Column::genre] = genre;
}

void TrackTableComponent::resized()
{
    layoutColumns();
    viewport.setBounds (getLocalBounds().withTrimmedTop (theme::tableHeaderHeight));
    updateContentSize();
}

int TrackTableComponent::columnAtX (int x) const
{
    int position = 0;
    for (int c = 0; c < (int) Column::numColumns; ++c)
    {
        const int w = columnWidths[(size_t) c];
        if (x >= position && x < position + w)
            return c;
        position += w;
    }
    return -1;
}

int TrackTableComponent::columnX (int column) const
{
    int position = 0;
    for (int c = 0; c < column; ++c)
        position += columnWidths[(size_t) c];
    return position;
}

void TrackTableComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    paintHeader (g);

    if (sortedIndices.empty())
    {
        auto area = getLocalBounds().withTrimmedTop (theme::tableHeaderHeight).reduced (40);
        g.setColour (theme::textDim);
        g.setFont (theme::uiFont (14.0f));
        g.drawFittedText (emptyMessage, area.removeFromTop (area.getHeight() / 2).removeFromBottom (26),
                          juce::Justification::centred, 1);

        if (emptySubMessage.isNotEmpty())
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::uiFont (12.0f));
            g.drawFittedText (emptySubMessage, area.removeFromTop (40), juce::Justification::centred, 2);
        }
    }
}

void TrackTableComponent::paintHeader (juce::Graphics& g)
{
    auto header = getLocalBounds().withHeight (theme::tableHeaderHeight);
    juce::ColourGradient grad (theme::panelAlt.brighter (0.02f), 0.0f, 0.0f,
                               theme::panelAlt.darker (0.12f), 0.0f, (float) header.getHeight(), false);
    g.setGradientFill (grad);
    g.fillRect (header);

    int x = 0;
    for (int c = 0; c < (int) Column::numColumns; ++c)
    {
        const int w = columnWidths[(size_t) c];
        juce::Rectangle<int> cell (x, 0, w, header.getHeight());
        const bool sorted = ! reorderMode && (int) sortColumn == c;
        const bool hovered = ! reorderMode && hoverHeaderColumn == c;

        g.setColour (sorted ? theme::goldBright : hovered ? theme::silver : theme::textDim);
        g.setFont (theme::capsFont (9.5f, 0.12f));
        g.drawFittedText (columnName ((Column) c), cell.reduced (8, 0).withTrimmedRight (sorted ? 12 : 0),
                          juce::Justification::centredLeft, 1);

        if (sorted)
        {
            paintIcon (g, sortDescending ? Icon::sortDown : Icon::sortUp,
                       { (float) cell.getRight() - 16.0f, (float) cell.getCentreY() - 5.0f, 10.0f, 10.0f },
                       theme::gold, 1.3f);
            g.setColour (theme::gold);
            g.fillRect (cell.getX(), header.getBottom() - 2, cell.getWidth(), 2);
        }

        x += w;
    }

    g.setColour (theme::line);
    g.drawHorizontalLine (header.getBottom() - 1, 0.0f, (float) getWidth());
}

void TrackTableComponent::mouseDown (const juce::MouseEvent& e)
{
    if (e.y >= theme::tableHeaderHeight)
        return;

    if (reorderMode)
        return;      // playlist order is manual

    const int column = columnAtX (e.x);
    if (column < 0 || column >= (int) Column::numColumns)
        return;

    if ((int) sortColumn == column)
        sortDescending = ! sortDescending;
    else
    {
        sortColumn = (Column) column;
        sortDescending = false;
    }

    rebuildSortedIndices();
    updateContentSize();
    repaint();

    if (listener != nullptr)
        listener->trackSortChanged (sortColumn, sortDescending);
}

void TrackTableComponent::mouseMove (const juce::MouseEvent& e)
{
    if (e.y < theme::tableHeaderHeight)
    {
        const int column = columnAtX (e.x);
        if (column != hoverHeaderColumn)
        {
            hoverHeaderColumn = column;
            repaint();
        }
    }
    else if (hoverHeaderColumn >= 0)
    {
        hoverHeaderColumn = -1;
        repaint();
    }
}

void TrackTableComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoverHeaderColumn >= 0)
    {
        hoverHeaderColumn = -1;
        repaint();
    }
}

} // namespace sp
