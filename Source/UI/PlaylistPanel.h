/*
    Snake Puppy Reference Player - playlist contents panel.

    Shows a manual or smart playlist with play/queue/rename/export/delete,
    drag reordering for manual playlists and drag-and-drop of library tracks.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/TrackTableComponent.h"
#include "UI/Widgets.h"

namespace sp
{

class PlaylistPanel : public juce::Component,
                      public juce::DragAndDropTarget,
                      private juce::ChangeListener,
                      private juce::Timer,
                      private TrackTableComponent::Listener
{
public:
    explicit PlaylistPanel (AppContext&);
    ~PlaylistPanel() override;

    void showPlaylist (PlaylistId);
    PlaylistId getPlaylistId() const { return playlistId; }
    void refresh();

    /** Fired after the playlist was deleted through this panel. */
    std::function<void (PlaylistId)> onPlaylistDeleted;

    //== Drag and drop ========================================================
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    //== TrackTableComponent::Listener =======================================
    void trackSelectionChanged (TrackTableComponent&) override {}
    void trackDoubleClicked (TrackId) override;
    void trackSortChanged (TrackTableComponent::Column, bool) override {}
    void tracksDroppedOnRow (const std::vector<TrackId>&, int targetRow) override;
    void tracksAddToQueue (const std::vector<TrackId>&) override;
    void tracksRemoveRequested (const std::vector<TrackId>&) override;
    void trackRatingChanged (TrackId, int) override;

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void updateHeader();
    void playAll();
    void addAllToQueue();
    std::vector<Track> collectTracksInViewOrder() const;

    AppContext& ctx;
    PlaylistId playlistId = kInvalidPlaylistId;

    SpLabel titleLabel, metaLabel;
    SpBadge smartBadge;
    SpButton playButton { "Play All", SpButton::Style::primary };
    SpButton queueButton { "Add to Queue", SpButton::Style::standard };
    SpButton clearButton { "Clear", SpButton::Style::standard };
    SpIconButton renameButton { Icon::settings, "Rename playlist" };
    SpIconButton exportButton { Icon::external, "Export playlist" };
    SpIconButton deleteButton { Icon::close, "Delete playlist" };

    TrackTableComponent table;
    std::unique_ptr<juce::FileChooser> chooser;

    int missingEntries = 0;
    std::uint64_t lastPlaylistVersion = 0;
    TrackId lastPlayingId = kInvalidTrackId;
    bool playing = false;
    bool dropActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaylistPanel)
};

} // namespace sp
