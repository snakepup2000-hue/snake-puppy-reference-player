/*
    Snake Puppy Reference Player - virtualised, sortable, multi-select track table.

    Used for the library song list, playlist contents and the play queue. Paints
    only visible rows, supports click/shift/cmd selection, double-click to play,
    header sorting, rating editing, drag-out of the selected tracks and (in
    reorder mode) internal drag reordering for playlists.
*/
#pragma once

#include "Library/LibraryService.h"
#include "UI/Widgets.h"

namespace sp
{

class TrackTableComponent : public juce::Component
{
public:
    enum class Column
    {
        title = 0, artist, album, genre, year, format, rate, bits, duration, rating, numColumns
    };

    /** A filtered/sorted view over an immutable library snapshot. The table
        keeps the shared_ptr alive, so indices stay valid. */
    struct RowSource
    {
        std::shared_ptr<const LibrarySnapshot> snapshot;
        std::vector<int> indices;
        bool isEmpty() const { return indices.empty(); }
    };

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void trackSelectionChanged (TrackTableComponent&) {}
        virtual void trackDoubleClicked (TrackId) {}
        virtual void trackSortChanged (Column, bool descending) {}
        virtual void tracksDroppedOnRow (const std::vector<TrackId>&, int targetRow) {}
        virtual void tracksAddToQueue (const std::vector<TrackId>&) {}
        virtual void tracksRemoveRequested (const std::vector<TrackId>&) {}
        virtual void trackRatingChanged (TrackId, int newRating) {}
    };

    TrackTableComponent();
    ~TrackTableComponent() override;

    //== Data =================================================================
    void setRowSource (RowSource newSource);
    const RowSource& getRowSource() const noexcept { return source; }
    void setEmptyMessage (const juce::String& message, const juce::String& subMessage = {});

    //== Playback state =======================================================
    void setPlayingTrack (TrackId trackId, bool isPlaying);
    void scrollToPlayingTrack();

    //== Sorting ==============================================================
    void setSort (Column column, bool descending);
    Column getSortColumn() const noexcept { return sortColumn; }
    bool isSortDescending() const noexcept { return sortDescending; }

    //== Reordering (playlist contents) =======================================
    void setReorderMode (bool shouldReorder);
    bool isReorderMode() const noexcept { return reorderMode; }

    //== Selection ============================================================
    void setListener (Listener*);
    std::vector<TrackId> getSelectedTrackIds() const { return selected; }
    std::vector<Track> getSelectedTracks() const;
    int getNumRows() const { return (int) source.indices.size(); }
    const Track* getTrackForRow (int row) const;
    int getRowForTrack (TrackId) const;
    void selectRow (int row, bool clearOthers = true);
    void selectRange (int fromRow, int toRow);
    void toggleRowSelection (int row);
    void selectAll();
    void clearSelection();
    void notifySelectionChanged();
    void scrollToRow (int row);
    int columnX (int column) const;
    juce::String getTableId() const { return tableId; }

    //== Painting =============================================================
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    static const char* columnName (Column);

private:
    class Rows;
    friend class Rows;

    void rebuildSortedIndices();
    void updateContentSize();
    void layoutColumns();
    std::array<int, (int) Column::numColumns> columnWidths {};
    int columnAtX (int x) const;
    void paintHeader (juce::Graphics&);

    RowSource source;
    std::vector<int> sortedIndices;
    Listener* listener = nullptr;

    Column sortColumn = Column::artist;
    bool sortDescending = false;
    bool reorderMode = false;

    TrackId playingTrackId = kInvalidTrackId;
    bool playing = false;

    juce::String emptyMessage { "No tracks" };
    juce::String emptySubMessage;

    std::vector<TrackId> selected;
    int anchorRow = -1;
    int hoverHeaderColumn = -1;
    juce::String tableId;

    juce::Viewport viewport;
    std::unique_ptr<Rows> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackTableComponent)
};

} // namespace sp
