/*
    Snake Puppy Reference Player - playlist contents panel implementation.
*/
#include "UI/PlaylistPanel.h"
#include <algorithm>
#include <functional>

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

juce::String sanitiseFilename (const juce::String& name)
{
    return name.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_()[]");
}
} // namespace

//==============================================================================
PlaylistPanel::PlaylistPanel (AppContext& context) : ctx (context)
{
    addAndMakeVisible (titleLabel);
    addAndMakeVisible (metaLabel);
    addAndMakeVisible (smartBadge);
    addAndMakeVisible (playButton);
    addAndMakeVisible (queueButton);
    addAndMakeVisible (clearButton);
    addAndMakeVisible (renameButton);
    addAndMakeVisible (exportButton);
    addAndMakeVisible (deleteButton);
    addAndMakeVisible (table);

    titleLabel.setFont (theme::uiFont (19.0f, true, 0.01f));
    titleLabel.setColour (juce::Label::textColourId, theme::text);
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setText ("Playlist", juce::dontSendNotification);

    metaLabel.setFont (theme::uiFont (11.5f));
    metaLabel.setColour (juce::Label::textColourId, theme::textDim);
    metaLabel.setJustificationType (juce::Justification::centredLeft);

    smartBadge.setBadgeColour (theme::gold);
    smartBadge.setFilled (false);
    smartBadge.setVisible (false);

    table.setListener (this);
    table.setEmptyMessage ("This playlist is empty",
                           "Drag tracks here from the library, or use Add to Queue.");

    playButton.onClick = [this] { playAll(); };
    queueButton.onClick = [this] { addAllToQueue(); };

    renameButton.onClick = [this]
    {
        auto snapshot = ctx.playlists().getSnapshot();
        const Playlist* playlist = snapshot != nullptr ? snapshot->find (playlistId) : nullptr;
        if (playlist == nullptr)
            return;

        const auto currentName = playlist->name;
        const auto id = playlistId;
        showTextInputDialog ("Rename Playlist", "New name for \"" + currentName + "\":",
                             currentName,
                             [this, id] (const juce::String& newName)
                             {
                                 if (ctx.playlists().renamePlaylist (id, newName))
                                 {
                                     ctx.playlists().markChanged();
                                     ctx.setStatusMessage ("Renamed playlist to \"" + newName + "\"");
                                     refresh();
                                 }
                                 else
                                 {
                                     ctx.setStatusMessage ("Could not rename the playlist.", 6000);
                                 }
                             });
    };

    exportButton.onClick = [this]
    {
        auto snapshot = ctx.playlists().getSnapshot();
        const Playlist* playlist = snapshot != nullptr ? snapshot->find (playlistId) : nullptr;
        if (playlist == nullptr)
            return;

        const auto suggested = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                   .getChildFile (sanitiseFilename (playlist->name) + ".m3u");
        chooser = std::make_unique<juce::FileChooser> ("Export Playlist", suggested,
                                                       "*.m3u;*.m3u8;*.pls");

        juce::Component::SafePointer<PlaylistPanel> safe (this);
        const auto id = playlistId;
        chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe, id] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr)
                                      return;
                                  auto file = fc.getResult();
                                  if (file == juce::File())
                                      return;

                                  juce::String error;
                                  if (safe->ctx.playlists().exportPlaylist (id, file, error))
                                  {
                                      safe->ctx.setStatusMessage ("Exported playlist to " + file.getFileName());
                                      safe->ctx.settings().addRecentPlaylist (file.getFullPathName());
                                  }
                                  else
                                  {
                                      safe->ctx.setStatusMessage ("Export failed: " + error, 8000);
                                  }
                              });
    };

    clearButton.onClick = [this]
    {
        auto snapshot = ctx.playlists().getSnapshot();
        const Playlist* playlist = snapshot != nullptr ? snapshot->find (playlistId) : nullptr;
        if (playlist == nullptr || playlist->smart || playlist->trackIds.empty())
            return;

        const auto name = playlist->name;
        const auto id = playlistId;
        juce::Component::SafePointer<PlaylistPanel> safe (this);
        showConfirmDialog ("Clear Playlist",
                           "Remove all tracks from \"" + name + "\"?\nThe playlist will be kept.",
                           "Clear",
                           [safe, id]
                           {
                               if (safe == nullptr)
                                   return;
                               if (safe->ctx.playlists().clearPlaylist (id))
                               {
                                   safe->ctx.playlists().markChanged();
                                   safe->ctx.setStatusMessage ("Playlist cleared.");
                                   safe->refresh();
                               }
                               else
                               {
                                   safe->ctx.setStatusMessage ("Could not clear the playlist.", 6000);
                               }
                           });
    };

    deleteButton.onClick = [this]
    {
        auto snapshot = ctx.playlists().getSnapshot();
        const Playlist* playlist = snapshot != nullptr ? snapshot->find (playlistId) : nullptr;
        if (playlist == nullptr)
            return;

        const auto name = playlist->name;
        const auto id = playlistId;
        juce::Component::SafePointer<PlaylistPanel> safe (this);
        showConfirmDialog ("Delete Playlist", "Delete \"" + name + "\"?\nThis cannot be undone.",
                           "Delete",
                           [safe, id]
                           {
                               if (safe == nullptr)
                                   return;
                               if (safe->ctx.playlists().deletePlaylist (id))
                               {
                                   safe->ctx.playlists().markChanged();
                                   safe->ctx.setStatusMessage ("Deleted playlist.");
                                   if (safe->onPlaylistDeleted != nullptr)
                                       safe->onPlaylistDeleted (id);
                               }
                               else
                               {
                                   safe->ctx.setStatusMessage ("Could not delete the playlist.", 6000);
                               }
                           });
    };

    ctx.playlists().addChangeListener (this);
    startTimerHz (2);
    refresh();
}

PlaylistPanel::~PlaylistPanel()
{
    stopTimer();
    ctx.playlists().removeChangeListener (this);
    table.setListener (nullptr);
}

void PlaylistPanel::showPlaylist (PlaylistId id)
{
    playlistId = id;
    refresh();
}

void PlaylistPanel::refresh()
{
    auto playlists = ctx.playlists().getSnapshot();
    auto library = ctx.library().getSnapshot();

    const Playlist* playlist = playlists != nullptr ? playlists->find (playlistId) : nullptr;
    if (playlist == nullptr)
    {
        titleLabel.setText ("Playlist unavailable", juce::dontSendNotification);
        metaLabel.setText ({}, juce::dontSendNotification);
        smartBadge.setVisible (false);
        table.setRowSource ({});
        table.setEmptyMessage ("This playlist no longer exists", {});
        for (juce::Button* b : { static_cast<juce::Button*> (&playButton),
                                 static_cast<juce::Button*> (&queueButton),
                                 static_cast<juce::Button*> (&renameButton),
                                 static_cast<juce::Button*> (&exportButton),
                                 static_cast<juce::Button*> (&deleteButton),
                                 static_cast<juce::Button*> (&clearButton) })
            b->setEnabled (false);
        return;
    }

    for (juce::Button* b : { static_cast<juce::Button*> (&playButton),
                             static_cast<juce::Button*> (&queueButton),
                             static_cast<juce::Button*> (&clearButton),
                             static_cast<juce::Button*> (&renameButton),
                             static_cast<juce::Button*> (&exportButton),
                             static_cast<juce::Button*> (&deleteButton) })
        b->setEnabled (true);

    lastPlaylistVersion = playlists->version;
    playlistId = playlist->id;

    titleLabel.setText (playlist->name, juce::dontSendNotification);
    smartBadge.setVisible (playlist->smart);
    smartBadge.setText ("SMART");
    smartBadge.setSize (smartBadge.getPreferredWidth(), 17);

    std::vector<TrackId> ids;
    if (playlist->smart)
        ids = ctx.playlists().resolveTrackIds (playlist->id);
    else
        ids = playlist->trackIds;

    TrackTableComponent::RowSource source;
    source.snapshot = library;
    missingEntries = 0;

    if (library != nullptr)
    {
        for (auto id : ids)
        {
            const int index = library->indexOfTrack (id);
            if (index >= 0)
                source.indices.push_back (index);
            else
                ++missingEntries;
        }
    }

    table.setRowSource (std::move (source));
    table.setReorderMode (! playlist->smart);

    // Header metadata.
    std::int64_t totalMs = 0;
    int trackCount = 0;
    for (int row = 0; row < table.getNumRows(); ++row)
        if (auto* track = table.getTrackForRow (row))
        {
            ++trackCount;
            totalMs += track->durationMs;
        }

    clearButton.setEnabled (! playlist->smart && trackCount > 0);

    juce::String meta;
    meta << formatCount (trackCount, "track", "tracks") << "   |   " << formatDuration (totalMs);
    if (playlist->smart)
        meta << "   |   rule-based";
    if (missingEntries > 0)
        meta << "   |   " << formatCount (missingEntries, "missing entry", "missing entries");
    metaLabel.setText (meta, juce::dontSendNotification);

    auto status = ctx.engine().getStatus();
    lastPlayingId = status.currentTrack.id;
    playing = status.state == PlaybackState::playing;
    table.setPlayingTrack (lastPlayingId, playing);

    resized();
    repaint();
}

void PlaylistPanel::updateHeader()
{
    // Cheap path used by the timer.
    auto playlists = ctx.playlists().getSnapshot();
    const Playlist* playlist = playlists != nullptr ? playlists->find (playlistId) : nullptr;
    if (playlist == nullptr)
        return;

    if (playlists->version != lastPlaylistVersion)
    {
        refresh();
        return;
    }

    auto status = ctx.engine().getStatus();
    const bool nowPlaying = status.state == PlaybackState::playing;
    if (status.currentTrack.id != lastPlayingId || nowPlaying != playing)
    {
        lastPlayingId = status.currentTrack.id;
        playing = nowPlaying;
        table.setPlayingTrack (lastPlayingId, playing);
    }
}

//==============================================================================
void PlaylistPanel::playAll()
{
    auto tracks = collectTracksInViewOrder();
    if (tracks.empty())
    {
        ctx.setStatusMessage ("This playlist has no playable tracks.", 5000);
        return;
    }
    ctx.playTracks (tracks, 0);
    ctx.setStatusMessage ("Playing playlist: " + titleLabel.getText());
}

void PlaylistPanel::addAllToQueue()
{
    auto tracks = collectTracksInViewOrder();
    if (tracks.empty())
    {
        ctx.setStatusMessage ("This playlist has no playable tracks.", 5000);
        return;
    }
    ctx.engine().enqueueAfterCurrent (tracks);
    ctx.setStatusMessage ("Added " + formatCount ((int) tracks.size(), "track") + " to the queue.");
}

std::vector<Track> PlaylistPanel::collectTracksInViewOrder() const
{
    std::vector<Track> tracks;
    for (int row = 0; row < table.getNumRows(); ++row)
        if (auto* track = table.getTrackForRow (row))
            tracks.push_back (*track);
    return tracks;
}

//==============================================================================
void PlaylistPanel::trackDoubleClicked (TrackId trackId)
{
    auto tracks = collectTracksInViewOrder();
    if (tracks.empty())
        return;

    const int row = juce::jmax (0, table.getRowForTrack (trackId));
    ctx.playTracks (tracks, row);
}

void PlaylistPanel::tracksDroppedOnRow (const std::vector<TrackId>& ids, int targetRow)
{
    auto playlists = ctx.playlists().getSnapshot();
    const Playlist* playlist = playlists != nullptr ? playlists->find (playlistId) : nullptr;
    if (playlist == nullptr || playlist->smart || ids.empty())
        return;

    const auto& order = playlist->trackIds;
    int from = -1;
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == ids.front())
        {
            from = (int) i;
            break;
        }

    if (from < 0)
        return;

    int to = targetRow;
    if (to > from)
        --to;
    to = juce::jlimit (0, juce::jmax (0, (int) order.size() - 1), to);

    if (to != from && ctx.playlists().moveTrack (playlistId, from, to))
    {
        ctx.playlists().markChanged();
        refresh();
    }
}

void PlaylistPanel::tracksAddToQueue (const std::vector<TrackId>&)
{
    addAllToQueue();
}

void PlaylistPanel::tracksRemoveRequested (const std::vector<TrackId>& ids)
{
    auto playlists = ctx.playlists().getSnapshot();
    const Playlist* playlist = playlists != nullptr ? playlists->find (playlistId) : nullptr;
    if (playlist == nullptr || playlist->smart)
        return;

    // Remove from the end so earlier indices stay valid.
    std::vector<int> positions;
    for (size_t i = 0; i < playlist->trackIds.size(); ++i)
        for (auto id : ids)
            if (playlist->trackIds[i] == id)
                positions.push_back ((int) i);

    std::sort (positions.begin(), positions.end(), std::greater<int>());
    for (int index : positions)
        ctx.playlists().removeTrackAt (playlistId, index);

    if (! positions.empty())
    {
        ctx.playlists().markChanged();
        ctx.setStatusMessage ("Removed " + formatCount ((int) positions.size(), "track") + " from the playlist.");
        refresh();
    }
}

void PlaylistPanel::trackRatingChanged (TrackId trackId, int rating)
{
    ctx.library().updateRating (trackId, rating);
    refresh();
}

//==============================================================================
void PlaylistPanel::timerCallback()
{
    updateHeader();
}

void PlaylistPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

//==============================================================================
void PlaylistPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    auto header = getLocalBounds().removeFromTop (78);
    juce::ColourGradient grad (theme::panel.brighter (0.015f), 0.0f, 0.0f,
                               theme::panel.darker (0.10f), 0.0f, (float) header.getHeight(), false);
    g.setGradientFill (grad);
    g.fillRect (header);
    g.setColour (theme::line);
    g.drawHorizontalLine (header.getBottom() - 1, 0.0f, (float) header.getWidth());

    if (dropActive)
    {
        g.setColour (theme::gold.withAlpha (0.6f));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 3.0f, 1.5f);
    }
}

void PlaylistPanel::resized()
{
    auto area = getLocalBounds();
    auto header = area.removeFromTop (78).reduced (20, 10);

    auto buttonRow = header.removeFromTop (34);
    auto titleRow = buttonRow;

    deleteButton.setBounds (buttonRow.removeFromRight (30).withSizeKeepingCentre (28, 28));
    buttonRow.removeFromRight (4);
    exportButton.setBounds (buttonRow.removeFromRight (30).withSizeKeepingCentre (28, 28));
    buttonRow.removeFromRight (4);
    renameButton.setBounds (buttonRow.removeFromRight (30).withSizeKeepingCentre (28, 28));
    buttonRow.removeFromRight (8);
    clearButton.setBounds (buttonRow.removeFromRight (64).withSizeKeepingCentre (64, 30));
    buttonRow.removeFromRight (12);
    queueButton.setBounds (buttonRow.removeFromRight (126).withSizeKeepingCentre (126, 30));
    buttonRow.removeFromRight (8);
    playButton.setBounds (buttonRow.removeFromRight (92).withSizeKeepingCentre (92, 30));
    buttonRow.removeFromRight (16);
    titleLabel.setBounds (buttonRow);

    auto metaRow = header.removeFromTop (24);
    auto badgeArea = metaRow.removeFromRight (70);
    if (smartBadge.isVisible())
        smartBadge.setBounds (badgeArea.withSizeKeepingCentre (smartBadge.getPreferredWidth(), 17));
    metaLabel.setBounds (metaRow);

    juce::ignoreUnused (titleRow);

    table.setBounds (area.reduced (8, 2).withTrimmedBottom (6));
}

//==============================================================================
bool PlaylistPanel::isInterestedInDragSource (const SourceDetails& details)
{
    if (details.description.getDynamicObject() == nullptr)
        return false;
    if (details.description.getDynamicObject()->getProperty ("type").toString() != "sp.tracks")
        return false;

    auto playlists = ctx.playlists().getSnapshot();
    const Playlist* playlist = playlists != nullptr ? playlists->find (playlistId) : nullptr;
    return playlist != nullptr && ! playlist->smart;
}

void PlaylistPanel::itemDragEnter (const SourceDetails&) { dropActive = true; repaint(); }
void PlaylistPanel::itemDragMove (const SourceDetails&) { }
void PlaylistPanel::itemDragExit (const SourceDetails&) { dropActive = false; repaint(); }

void PlaylistPanel::itemDropped (const SourceDetails& details)
{
    dropActive = false;
    repaint();

    const auto ids = dragIds (details.description);
    if (ids.empty())
        return;

    if (ctx.playlists().addTracks (playlistId, ids, -1))
    {
        ctx.playlists().markChanged();
        ctx.setStatusMessage ("Added " + formatCount ((int) ids.size(), "track") + " to the playlist.");
        refresh();
    }
    else
    {
        ctx.setStatusMessage ("Could not add those tracks to the playlist.", 6000);
    }
}

} // namespace sp
