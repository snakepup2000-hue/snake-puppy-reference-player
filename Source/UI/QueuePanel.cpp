/*
    Snake Puppy Reference Player - play queue panel implementation.
*/
#include "UI/QueuePanel.h"

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
} // namespace

//==============================================================================
QueuePanel::QueuePanel (AppContext& context) : ctx (context)
{
    addAndMakeVisible (titleLabel);
    addAndMakeVisible (metaLabel);
    addAndMakeVisible (clearButton);
    addAndMakeVisible (table);

    titleLabel.setFont (theme::uiFont (19.0f, true));
    titleLabel.setColour (juce::Label::textColourId, theme::text);
    titleLabel.setText ("Queue", juce::dontSendNotification);

    metaLabel.setFont (theme::uiFont (11.5f));
    metaLabel.setColour (juce::Label::textColourId, theme::textDim);

    table.setListener (this);
    table.setEmptyMessage ("The queue is empty",
                           "Double-click a library track to play it, or drag tracks here.");

    clearButton.onClick = [this]
    {
        if (ctx.getQueue().empty())
            return;
        ctx.engine().clearQueue();
        ctx.setStatusMessage ("Queue cleared.");
        refresh();
    };

    startTimerHz (2);
    refresh();
}

QueuePanel::~QueuePanel()
{
    stopTimer();
    table.setListener (nullptr);
}

void QueuePanel::refresh()
{
    rebuildFromEngine();
}

void QueuePanel::rebuildFromEngine()
{
    auto queue = ctx.getQueue();
    auto status = ctx.engine().getStatus();

    std::vector<TrackId> ids;
    ids.reserve (queue.size());
    for (auto& t : queue)
        ids.push_back (t.id);

    if (ids != cachedIds || status.currentQueueIndex != cachedIndex || syntheticSnapshot == nullptr
        || (int) syntheticSnapshot->tracks.size() != (int) queue.size())
    {
        cachedIds = ids;
        cachedIndex = status.currentQueueIndex;

        syntheticSnapshot = std::make_shared<LibrarySnapshot>();
        syntheticSnapshot->version = ++syntheticVersion;
        syntheticSnapshot->tracks = queue;

        TrackTableComponent::RowSource source;
        source.snapshot = syntheticSnapshot;
        source.indices.reserve (queue.size());
        for (int i = 0; i < (int) queue.size(); ++i)
            source.indices.push_back (i);

        table.setRowSource (std::move (source));
        table.setReorderMode (false);

        juce::String meta;
        meta << formatCount ((int) queue.size(), "track");
        if (status.currentQueueIndex >= 0 && status.currentQueueIndex < (int) queue.size())
            meta << "   |   playing " << (status.currentQueueIndex + 1) << " of " << (int) queue.size();
        metaLabel.setText (meta, juce::dontSendNotification);

        clearButton.setEnabled (! queue.empty());
    }

    const bool nowPlaying = status.state == PlaybackState::playing;
    TrackId current = kInvalidTrackId;
    if (status.currentQueueIndex >= 0 && status.currentQueueIndex < (int) queue.size())
        current = queue[(size_t) status.currentQueueIndex].id;

    if (current != playingId || nowPlaying != playing)
    {
        playingId = current;
        playing = nowPlaying;
        table.setPlayingTrack (playingId, playing);
    }
}

void QueuePanel::timerCallback()
{
    rebuildFromEngine();
}

//==============================================================================
void QueuePanel::trackDoubleClicked (TrackId trackId)
{
    const int row = table.getRowForTrack (trackId);
    if (row >= 0)
        ctx.engine().playQueueIndex (row, true);
}

void QueuePanel::tracksRemoveRequested (const std::vector<TrackId>&)
{
    ctx.setStatusMessage ("Use Clear Queue to empty the queue.", 6000);
}

void QueuePanel::trackRatingChanged (TrackId trackId, int rating)
{
    if (trackId != kInvalidTrackId)
        ctx.library().updateRating (trackId, rating);
}

//==============================================================================
void QueuePanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    auto header = getLocalBounds().removeFromTop (70);
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

void QueuePanel::resized()
{
    auto area = getLocalBounds();
    auto header = area.removeFromTop (70).reduced (20, 10);
    auto row = header.removeFromTop (32);

    clearButton.setBounds (row.removeFromRight (112).withSizeKeepingCentre (112, 28));
    row.removeFromRight (16);
    titleLabel.setBounds (row);

    auto metaRow = header.removeFromTop (22);
    metaLabel.setBounds (metaRow);

    table.setBounds (area.reduced (8, 2).withTrimmedBottom (6));
}

//==============================================================================
bool QueuePanel::isInterestedInDragSource (const SourceDetails& details)
{
    return details.description.getDynamicObject() != nullptr
           && details.description.getDynamicObject()->getProperty ("type").toString() == "sp.tracks";
}

void QueuePanel::itemDragEnter (const SourceDetails&) { dropActive = true; repaint(); }
void QueuePanel::itemDragMove (const SourceDetails&) { }
void QueuePanel::itemDragExit (const SourceDetails&) { dropActive = false; repaint(); }

void QueuePanel::itemDropped (const SourceDetails& details)
{
    dropActive = false;
    repaint();

    const auto ids = dragIds (details.description);
    if (ids.empty())
        return;

    auto library = ctx.library().getSnapshot();
    std::vector<Track> tracks;
    if (library != nullptr)
        for (auto id : ids)
            if (const auto* track = library->findTrack (id))
                tracks.push_back (*track);

    if (tracks.empty())
    {
        ctx.setStatusMessage ("Those tracks are not in the library.", 5000);
        return;
    }

    ctx.engine().enqueueAfterCurrent (tracks);
    ctx.setStatusMessage ("Added " + formatCount ((int) tracks.size(), "track") + " to the queue.");
    refresh();
}

} // namespace sp
