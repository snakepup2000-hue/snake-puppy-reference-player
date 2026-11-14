/*
    Snake Puppy Reference Player - transport bar implementation.
*/
#include "UI/TransportBar.h"

#if __has_include("UI/MetersPanel.h")
    #include "UI/MetersPanel.h"
    #define SP_HAS_DSP_METERS 1
#else
    #define SP_HAS_DSP_METERS 0
#endif

namespace sp
{

namespace
{
juce::String qualityText (SignalPathInfo::Quality quality, bool hasError)
{
    if (hasError)
        return "ENGINE ERROR";

    switch (quality)
    {
        case SignalPathInfo::Quality::bitPerfect:     return "BIT-PERFECT";
        case SignalPathInfo::Quality::bitTransparent: return "BIT-TRANSPARENT";
        case SignalPathInfo::Quality::processed:      return "PROCESSED";
        case SignalPathInfo::Quality::error:          return "SIGNAL PATH ERROR";
        case SignalPathInfo::Quality::unknown:        return "NO SIGNAL";
    }
    return "NO SIGNAL";
}
} // namespace

//==============================================================================
class TransportBar::SignalBadge : public juce::Component,
                                  public juce::SettableTooltipClient
{
public:
    SignalBadge (AppContext& c, std::function<void()> clickHandler)
        : ctx (c), onClick (std::move (clickHandler))
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paint (juce::Graphics& g) override
    {
        auto st = ctx.engine().getStatus();
        const bool piOutput = ctx.isPiOutputSelected();
        const bool piStreaming = ctx.isPiBridgeStreaming();
        const bool hasError = st.errorMessage.isNotEmpty()
                              || st.signalPath.quality == SignalPathInfo::Quality::error
                              || (piOutput && ctx.getPiBridgeStatus().containsIgnoreCase ("fail"));
        const auto colour = hasError ? theme::error
                                     : (piOutput ? theme::gold : qualityColour (st.signalPath.quality));

        auto bounds = getLocalBounds().toFloat();
        g.setColour (colour.withAlpha (0.10f));
        g.fillRoundedRectangle (bounds, 3.0f);
        g.setColour (colour.withAlpha (0.45f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.0f);

        // Quality lamp.
        g.setColour (colour);
        g.fillEllipse (10.0f, (float) getHeight() * 0.5f - 3.0f, 6.0f, 6.0f);

        auto titleArea = getLocalBounds().withTrimmedLeft (24).withTrimmedRight (10).withTrimmedTop (6).withHeight (14);
        g.setColour (colour);
        g.setFont (theme::capsFont (10.0f, 0.14f));
        g.drawFittedText (piOutput ? (piStreaming ? "PI K11 STREAMING" : "PI K11 OUTPUT")
                                   : qualityText (st.signalPath.quality, hasError),
                          titleArea, juce::Justification::centredLeft, 1);

        juce::String summary;
        if (piOutput)
        {
            const int rate = ctx.getPiBridgeRate();
            summary << (st.signalPath.sourceFormat.isNotEmpty() ? st.signalPath.sourceFormat : "Audio");
            if (st.signalPath.sourceSampleRate > 0)
                summary << " " << juce::String (st.signalPath.sourceSampleRate / 1000.0, 1) << " kHz";
            summary << " -> DSP";
            if (rate > 0)
                summary << " -> " << juce::String (rate / 1000.0, 1) << " kHz";
            summary << " -> Pi K11 (S32_LE over SSH)";
        }
        else
        {
            summary = hasError ? st.errorMessage : st.signalPath.summary();
            if (summary.isEmpty())
                summary = "No track loaded";
        }

        g.setColour (theme::textDim);
        g.setFont (theme::uiFont (10.5f));
        g.drawFittedText (summary, getLocalBounds().withTrimmedLeft (24).withTrimmedRight (10)
                                             .withTrimmedTop (20).withHeight (16),
                          juce::Justification::centredLeft, 1);

        setTooltip (piOutput ? (ctx.getPiBridgeStatus().isNotEmpty() ? ctx.getPiBridgeStatus() : summary)
                             : (hasError ? st.errorMessage : st.signalPath.summary()));
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (onClick != nullptr)
            onClick();
    }

private:
    AppContext& ctx;
    std::function<void()> onClick;
};

//==============================================================================
class TransportBar::FallbackMeters : public juce::Component,
                                     private juce::Timer
{
public:
    explicit FallbackMeters (AppContext& c) : ctx (c) { startTimerHz (30); }

    void paint (juce::Graphics& g) override
    {
        auto st = ctx.engine().getStatus();
        auto area = getLocalBounds();
        g.setColour (theme::panelSunken);
        g.fillRoundedRectangle (area.toFloat(), 2.0f);
        g.setColour (theme::line);
        g.drawRoundedRectangle (area.toFloat().reduced (0.5f), 2.0f, 1.0f);

        auto inner = area.reduced (6, 3);
        for (int ch = 0; ch < 2; ++ch)
        {
            auto row = inner.removeFromTop (inner.getHeight() / (ch == 0 ? 2 : 1));
            if (ch == 0)
                inner.removeFromTop (2);

            g.setColour (theme::textFaint);
            g.setFont (theme::monoFont (8.5f));
            g.drawText (ch == 0 ? "L" : "R", row.removeFromLeft (10), juce::Justification::centredLeft, false);

            auto meter = row.removeFromLeft (juce::jmax (20, row.getWidth() - 42));
            auto peakDb = st.meters.peakDb[ch];
            auto rmsDb = st.meters.rmsDb[ch];
            const float peakNorm = juce::jlimit (0.0f, 1.0f, (peakDb + 60.0f) / 60.0f);
            const float rmsNorm = juce::jlimit (0.0f, 1.0f, (rmsDb + 60.0f) / 60.0f);

            auto meterF = meter.toFloat();
            g.setColour (theme::panelSunken.brighter (0.06f));
            g.fillRect (meterF);
            g.setColour (theme::success.withAlpha (0.55f));
            g.fillRect (meterF.withWidth (meterF.getWidth() * rmsNorm));
            g.setColour (peakNorm > 0.92f ? theme::error : theme::gold);
            g.fillRect (meterF.withWidth (juce::jmax (2.0f, meterF.getWidth() * peakNorm)).withHeight (2.0f)
                            .withY (meterF.getBottom() - 2.0f));

            g.setColour (theme::textDim);
            g.setFont (theme::monoFont (8.5f));
            g.drawText (meteringText (peakDb), row, juce::Justification::centredRight, false);
        }
    }

    void timerCallback() override { repaint(); }

private:
    AppContext& ctx;
};

//==============================================================================
TransportBar::TransportBar (AppContext& context) : ctx (context)
{
    seekSlider.setRange (0.0, 1.0, 0.0);
    seekSlider.setTooltip ("Seek - click or drag the bar (Left/Right = 5 s, Cmd+Shift+Left/Right = 30 s)");
    volumeSlider.setRange (-60.0, 12.0, 0.5);
    volumeSlider.setTooltip ("Master volume (dB)");
    volumeSlider.setSkewFactorFromMidPoint (-12.0);

    for (auto* b : { &previousButton, &playPauseButton, &stopButton, &nextButton, &muteButton,
                     &rackButton, &settingsButton })
        addAndMakeVisible (*b);

    addAndMakeVisible (seekSlider);
    addAndMakeVisible (volumeSlider);
    addAndMakeVisible (positionLabel);
    addAndMakeVisible (totalLabel);
    addAndMakeVisible (volumeLabel);
    addAndMakeVisible (referenceToggle);
    addAndMakeVisible (dspStudioButton);
    addAndMakeVisible (deviceCombo);

    playPauseButton.setProminent (true);

    positionLabel.setFont (theme::monoFont (13.5f));
    positionLabel.setColour (juce::Label::textColourId, theme::text);
    positionLabel.setJustificationType (juce::Justification::centredRight);
    positionLabel.setInterceptsMouseClicks (false, false);
    positionLabel.setText ("0:00.0", juce::dontSendNotification);

    totalLabel.setFont (theme::monoFont (10.5f));
    totalLabel.setColour (juce::Label::textColourId, theme::textFaint);
    totalLabel.setJustificationType (juce::Justification::centredLeft);
    totalLabel.setInterceptsMouseClicks (false, false);
    totalLabel.setText ("--:--", juce::dontSendNotification);

    volumeLabel.setFont (theme::monoFont (11.0f));
    volumeLabel.setColour (juce::Label::textColourId, theme::textDim);
    volumeLabel.setJustificationType (juce::Justification::centredRight);
    volumeLabel.setInterceptsMouseClicks (false, false);

    referenceToggle.setClickingTogglesState (true);
    referenceToggle.setTooltip ("Reference mode: all DSP bypassed, volume at unity");

    //== Callbacks ============================================================
    previousButton.onClick = [this] { ctx.performAction (AppAction::previousTrack); };
    nextButton.onClick = [this] { ctx.performAction (AppAction::nextTrack); };
    stopButton.onClick = [this] { ctx.performAction (AppAction::stop); };
    playPauseButton.onClick = [this]
    {
        if (onPlayRequested != nullptr && onPlayRequested())
            return;
        ctx.performAction (AppAction::playPause);
    };
    muteButton.onClick = [this] { ctx.setMasterMuted (! ctx.engine().isMuted()); };

    seekSlider.onDragStart = [this] { scrubbing = true; };
    seekSlider.onDragEnd = [this]
    {
        scrubbing = false;
        if (durationMs > 0.0)
            ctx.seekMs ((std::int64_t) (seekSlider.getValue() * durationMs));
    };

    volumeSlider.onDragStart = [this] { draggingVolume = true; };
    volumeSlider.onDragEnd = [this]
    {
        draggingVolume = false;
        // Push the final value once more: onValueChange is suppressed while
        // draggingVolume was true in some JUCE slider interactions, and without
        // this the timer snapped the thumb back to the old engine value.
        ctx.setMasterVolume ((float) volumeSlider.getValue());
    };
    volumeSlider.onValueChange = [this]
    {
        if (updatingControls)
            return;
        // Apply live while dragging so the audio follows the thumb and the
        // periodic status refresh can never overwrite the user's position.
        ctx.setMasterVolume ((float) volumeSlider.getValue());
        volumeLabel.setText (formatDb ((float) volumeSlider.getValue()), juce::dontSendNotification);
    };

    referenceToggle.onClick = [this]
    {
        ctx.setReferenceMode (referenceToggle.getToggleState());
    };

    dspStudioButton.onClick = [this]
    {
        if (onOpenDspStudio != nullptr)
            onOpenDspStudio();
    };

    rackButton.setClickingTogglesState (true);
    rackButton.onClick = [this]
    {
        rightPanelVisible = rackButton.getToggleState();
        if (onToggleRightPanel != nullptr)
            onToggleRightPanel();
    };

    settingsButton.onClick = [this] { if (onOpenSettings != nullptr) onOpenSettings(); };

    deviceCombo.setTextWhenNothingSelected ("No output device");
    deviceCombo.onChange = [this]
    {
        if (updatingControls)
            return;
        const int index = deviceCombo.getSelectedId() - 1;
        if (index == cachedDeviceNames.size())
        {
            ctx.setPiOutputSelected (true);
            return;
        }

        if (index < 0 || index >= cachedDeviceNames.size())
            return;

        if (ctx.isPiOutputSelected())
            ctx.setPiOutputSelected (false);

        juce::String error;
        if (ctx.selectOutputDevice (cachedDeviceNames[index], 0.0, error))
            ctx.setStatusMessage ("Output: " + cachedDeviceNames[index]);
        else
            ctx.setStatusMessage ("Could not open " + cachedDeviceNames[index] + ": " + error, 8000);
    };

#if SP_HAS_DSP_METERS
    auto* meters = new MetersPanel (ctx);
    meters->setCompact (true);
    metersComponent.reset (meters);
#else
    fallbackMeters = std::make_unique<FallbackMeters> (ctx);
    metersComponent.reset (fallbackMeters.get());
#endif

    if (metersComponent != nullptr)
        addAndMakeVisible (*metersComponent);

    badge = std::make_unique<SignalBadge> (ctx, [this]
    {
        if (onShowSignalPath != nullptr)
            onShowSignalPath();
    });
    addAndMakeVisible (*badge);

    rebuildDeviceCombo();
    updateControls();
    startTimerHz (30);
}

TransportBar::~TransportBar()
{
    stopTimer();
    // fallbackMeters is owned by metersComponent in the fallback configuration.
    if (fallbackMeters != nullptr && metersComponent.get() == fallbackMeters.get())
        metersComponent.release();
}

void TransportBar::refreshDeviceList()
{
    rebuildDeviceCombo();
}

void TransportBar::setRightPanelVisible (bool visible)
{
    rightPanelVisible = visible;
    rackButton.setToggleState (visible, juce::dontSendNotification);
}

void TransportBar::rebuildDeviceCombo()
{
    const auto devices = ctx.getOutputDevices();
    auto st = ctx.engine().getStatus();

    // The live output rate is authoritative once the device is open: the
    // cached OutputDeviceInfo rate can be stale after a native-rate switch.
    int liveRate = st.signalPath.outputSampleRate;
    if (liveRate <= 0)
        liveRate = (int) ctx.engine().devices().getCurrentSampleRate();
    if (liveRate > 0)
        st.signalPath.outputSampleRate = liveRate;

    const auto liveDeviceName = st.signalPath.outputDeviceName.isNotEmpty()
                                    ? st.signalPath.outputDeviceName
                                    : ctx.engine().devices().getCurrentDeviceName();

    juce::StringArray names, display;
    int selectedIndex = -1;

    for (size_t i = 0; i < devices.size(); ++i)
    {
        const auto& d = devices[i];
        names.add (d.name);

        const int rate = (d.name == liveDeviceName && st.signalPath.outputSampleRate > 0)
                             ? st.signalPath.outputSampleRate
                             : (int) d.currentSampleRate;
        juce::String label = d.name;
        if (rate > 0)
            label += "  |  " + formatSampleRate (rate);
        display.add (label);
        if (d.isDefault)
            selectedIndex = (int) i;
    }

    const bool changed = names != cachedDeviceNames || display != cachedDeviceLabels
                         || st.signalPath.outputSampleRate != cachedOutputRate;
    cachedDeviceNames = names;
    cachedDeviceLabels = display;
    cachedOutputRate = st.signalPath.outputSampleRate;
    currentDeviceName = liveDeviceName;

    if (changed && ! deviceCombo.isPopupActive())
    {
        updatingControls = true;
        deviceCombo.clear (juce::dontSendNotification);
        for (int i = 0; i < display.size(); ++i)
            deviceCombo.addItem (display[i], i + 1);
        deviceCombo.addItem ("Pi K11  |  Network (SSH)", (int) display.size() + 1);
        updatingControls = false;
    }

    int indexToSelect = selectedIndex;
    if (ctx.isPiOutputSelected())
        indexToSelect = (int) cachedDeviceNames.size();
    else if (currentDeviceName.isNotEmpty())
    {
        const int found = cachedDeviceNames.indexOf (currentDeviceName);
        if (found >= 0)
            indexToSelect = found;
    }

    if (indexToSelect >= 0 && indexToSelect < cachedDeviceNames.size()
        && deviceCombo.getSelectedId() != indexToSelect + 1)
    {
        updatingControls = true;
        deviceCombo.setSelectedId (indexToSelect + 1, juce::dontSendNotification);
        updatingControls = false;
    }
}

void TransportBar::updateControls()
{
    auto status = ctx.engine().getStatus();

    // In Pi mode the audible position comes from the stream, not the paused
    // local engine, so the scrub bar and clock follow the Pi.
    if (ctx.isPiOutputSelected())
    {
        status.positionMs = ctx.getPiBridgePositionMs();
        if (ctx.isPiBridgeStreaming())
            status.state = PlaybackState::playing;
    }

    durationMs = status.durationMs > 0 ? (double) status.durationMs : (double) status.currentTrack.durationMs;

    const bool isPlaying = status.state == PlaybackState::playing;
    playPauseButton.setIcon (isPlaying ? Icon::pause : Icon::play);
    playPauseButton.setTooltip (isPlaying ? "Pause (Space)" : "Play (Space)");

    muteButton.setIcon (status.muted ? Icon::mute : Icon::volume);
    muteButton.setToggleState (status.muted, juce::dontSendNotification);
    muteButton.setOnColour (theme::warning);

    referenceToggle.setToggleState (ctx.isReferenceMode(), juce::dontSendNotification);

    if (! scrubbing)
    {
        const double progress = durationMs > 0.0 ? juce::jlimit (0.0, 1.0, (double) status.positionMs / durationMs) : 0.0;
        updatingControls = true;
        seekSlider.setValue (progress, juce::dontSendNotification);
        updatingControls = false;
        positionLabel.setText (formatPosition (status.positionMs), juce::dontSendNotification);
    }

    totalLabel.setText (durationMs > 0.0 ? formatDuration ((std::int64_t) durationMs) : juce::String ("--:--"),
                        juce::dontSendNotification);

    if (! draggingVolume)
    {
        updatingControls = true;
        volumeSlider.setValue (juce::jlimit (-60.0, 12.0, (double) status.volumeDb), juce::dontSendNotification);
        updatingControls = false;
    }

    volumeLabel.setText (status.muted ? "MUTED" : formatDb (status.volumeDb), juce::dontSendNotification);
    volumeLabel.setColour (juce::Label::textColourId, status.muted ? theme::warning : theme::textDim);

    if (++deviceRefreshTicks >= 60)
    {
        deviceRefreshTicks = 0;
        rebuildDeviceCombo();
    }

    if (badge != nullptr)
        badge->repaint();
}

void TransportBar::timerCallback()
{
    updateControls();
}

//==============================================================================
void TransportBar::paint (juce::Graphics& g)
{
    juce::Rectangle<int> area = getLocalBounds();
    juce::ColourGradient grad (theme::panelAlt.brighter (0.01f), 0.0f, 0.0f,
                               theme::panel.darker (0.18f), 0.0f, (float) area.getHeight(), false);
    g.setGradientFill (grad);
    g.fillRect (area);

    g.setColour (theme::lineStrong);
    g.drawHorizontalLine (0, 0.0f, (float) area.getWidth());
    g.setColour (juce::Colours::white.withAlpha (0.03f));
    g.drawHorizontalLine (1, 0.0f, (float) area.getWidth());

    juce::ignoreUnused (badgeArea, metersArea);
}

void TransportBar::resized()
{
    auto area = getLocalBounds().reduced (16, 8);
    auto top = area.removeFromTop (44);
    area.removeFromTop (6);
    auto bottom = area.removeFromTop (44);

    //== Top row: transport + seek + signal badge =============================
    top.removeFromLeft (4);
    previousButton.setBounds (top.removeFromLeft (34).withSizeKeepingCentre (30, 30));
    top.removeFromLeft (4);
    playPauseButton.setBounds (top.removeFromLeft (48).withSizeKeepingCentre (46, 46));
    top.removeFromLeft (4);
    stopButton.setBounds (top.removeFromLeft (32).withSizeKeepingCentre (28, 28));
    top.removeFromLeft (4);
    nextButton.setBounds (top.removeFromLeft (34).withSizeKeepingCentre (30, 30));

    top.removeFromLeft (18);
    badgeArea = top.removeFromRight (juce::jmin (310, juce::jmax (160, top.getWidth() / 2))).withSizeKeepingCentre (
        juce::jmin (310, juce::jmax (160, top.getWidth() / 2)), 38);
    top.removeFromRight (14);

    positionLabel.setBounds (top.removeFromLeft (62));
    top.removeFromLeft (4);
    auto totalArea = top.removeFromRight (48);
    totalLabel.setBounds (totalArea.withSizeKeepingCentre (48, 20));
    // Note: evaluate the width BEFORE removeFromLeft(). Passing
    // top.removeFromLeft(...).withSizeKeepingCentre(top.getWidth(), ...) used
    // the post-removal width (0), which collapsed the seek bar to 60 px in the
    // middle of the transport - the "invisible" seek control.
    auto seekArea = top.removeFromLeft (top.getWidth());
    seekSlider.setBounds (seekArea.withSizeKeepingCentre (juce::jmax (60, seekArea.getWidth()), 28));

    if (badge != nullptr)
        badge->setBounds (badgeArea);

    //== Bottom row: volume / reference / meters / device / buttons ===========
    bottom.removeFromLeft (4);
    muteButton.setBounds (bottom.removeFromLeft (30).withSizeKeepingCentre (26, 26));
    bottom.removeFromLeft (4);
    volumeSlider.setBounds (bottom.removeFromLeft (130).withSizeKeepingCentre (130, 22));
    bottom.removeFromLeft (6);
    volumeLabel.setBounds (bottom.removeFromLeft (84));   // fits "-60.0 dB" on one line
    bottom.removeFromLeft (14);
    referenceToggle.setBounds (bottom.removeFromLeft (156).withSizeKeepingCentre (156, 28));

    settingsButton.setBounds (bottom.removeFromRight (32).withSizeKeepingCentre (28, 28));
    bottom.removeFromRight (4);
    rackButton.setBounds (bottom.removeFromRight (32).withSizeKeepingCentre (28, 28));
    bottom.removeFromRight (8);
    dspStudioButton.setBounds (bottom.removeFromRight (104).withSizeKeepingCentre (104, 28));
    bottom.removeFromRight (12);
    deviceCombo.setBounds (bottom.removeFromRight (juce::jlimit (150, 220, bottom.getWidth() - 130))
                               .withSizeKeepingCentre (juce::jlimit (150, 220, bottom.getWidth() - 130), 28));

    bottom.removeFromRight (10);
    metersArea = bottom.withTrimmedLeft (10).withSizeKeepingCentre (juce::jmax (80, bottom.getWidth() - 10), 40);
    if (metersComponent != nullptr)
        metersComponent->setBounds (metersArea);
}

void TransportBar::mouseDown (const juce::MouseEvent& e)
{
    if (badgeArea.contains (e.getPosition()))
        if (onShowSignalPath != nullptr)
            onShowSignalPath();
}

} // namespace sp
