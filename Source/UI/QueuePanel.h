/*
    Snake Puppy Reference Player - play queue panel.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/TrackTableComponent.h"
#include "UI/Widgets.h"

namespace sp
{

class QueuePanel : public juce::Component,
                   public juce::DragAndDropTarget,
                   private juce::Timer,
                   private TrackTableComponent::Listener
{
public:
    explicit QueuePanel (AppContext&);
    ~QueuePanel() override;

    void refresh();

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
    void tracksDroppedOnRow (const std::vector<TrackId>&, int) override {}
    void tracksAddToQueue (const std::vector<TrackId>&) override {}
    void tracksRemoveRequested (const std::vector<TrackId>&) override;
    void trackRatingChanged (TrackId, int) override;

    void timerCallback() override;
    void rebuildFromEngine();

    AppContext& ctx;
    SpLabel titleLabel, metaLabel;
    SpButton clearButton { "Clear Queue", SpButton::Style::ghost };
    TrackTableComponent table;
    std::shared_ptr<LibrarySnapshot> syntheticSnapshot;
    std::vector<TrackId> cachedIds;
    int cachedIndex = -2;
    TrackId playingId = kInvalidTrackId;
    bool playing = false;
    bool dropActive = false;
    std::uint64_t syntheticVersion = 1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (QueuePanel)
};

} // namespace sp
