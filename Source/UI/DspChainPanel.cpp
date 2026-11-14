/*
    Snake Puppy Reference Player - DSP chain panel.
    Message thread only.
*/
#include "UI/DspChainPanel.h"

namespace sp
{

namespace
{
constexpr int kRowHeight = 58;

juce::String arrowUp() { return juce::String::fromUTF8 ("\xe2\x96\xb2"); }
juce::String arrowDown() { return juce::String::fromUTF8 ("\xe2\x96\xbc"); }
} // namespace

//==============================================================================
class DspChainPanel::SlotRow : public juce::Component
{
public:
    explicit SlotRow (DspSlot slotType) : slot (slotType)
    {
        setOpaque (false);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);

        enableToggle.setTooltip ("Enable or bypass this processor");
        enableToggle.onClick = [this]
        {
            if (updating || onEnableChanged == nullptr)
                return;
            onEnableChanged (slot, enableToggle.getToggleState());
        };
        addAndMakeVisible (enableToggle);
    }

    void setState (int orderIndex, bool enabled, bool active, bool canToggle, const juce::String& statusText)
    {
        updating = true;
        order = orderIndex;
        isEnabled = enabled;
        isActive = active;
        toggleAvailable = canToggle;
        status = statusText;

        enableToggle.setVisible (canToggle);
        enableToggle.setToggleState (enabled && canToggle, juce::dontSendNotification);
        updating = false;
        repaint();
    }

    void setMoveEnabled (bool canUp, bool canDown)
    {
        canMoveUp = canUp;
        canMoveDown = canDown;
        repaint();
    }

    std::function<void (DspSlot, bool)> onEnableChanged;
    std::function<void (DspSlot)> onMoveUp;
    std::function<void (DspSlot)> onMoveDown;
    DspSlot getSlot() const noexcept { return slot; }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        const bool hover = isMouseOverOrDragging();

        g.setColour (hover ? theme::panelRaised.withAlpha (0.55f) : theme::panel.withAlpha (0.35f));
        g.fillRect (bounds);
        g.setColour (theme::lineFaint);
        g.fillRect (bounds.getX(), bounds.getBottom() - 1.0f, bounds.getWidth(), 1.0f);

        // Order number.
        g.setFont (theme::monoFont (12.0f, true));
        g.setColour (isActive ? theme::gold : theme::textFaint);
        g.drawText (juce::String (order + 1).paddedLeft ('0', 2),
                    bounds.withWidth (30.0f).toNearestInt(), juce::Justification::centred, false);

        // Status LED.
        const auto ledCentre = juce::Point<float> (58.0f, bounds.getCentreY());
        if (isActive && isEnabled)
        {
            g.setColour (theme::gold.withAlpha (0.18f));
            g.fillEllipse (juce::Rectangle<float> (16.0f, 16.0f).withCentre (ledCentre));
            g.setColour (theme::gold);
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (ledCentre));
        }
        else
        {
            g.setColour (theme::lineStrong);
            g.drawEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (ledCentre), 1.2f);
        }

        // Name and description.
        auto text = bounds.withTrimmedLeft (74.0f).withTrimmedRight (64.0f);
        auto nameArea = text.removeFromTop (bounds.getHeight() * 0.52f);
        g.setColour (isEnabled ? theme::text : theme::textDim);
        g.setFont (theme::uiFont (12.0f, true, 0.03f));
        g.drawText (dspSlotName (slot), nameArea.toNearestInt(), juce::Justification::bottomLeft, true);

        g.setColour (theme::textFaint);
        g.setFont (theme::uiFont (9.5f));
        g.drawText (dspSlotDescription (slot), text.toNearestInt(), juce::Justification::topLeft, true);

        if (status.isNotEmpty())
        {
            g.setFont (theme::capsFont (8.0f, 0.08f));
            g.setColour (isActive ? theme::goldDim : theme::textFaint);
            auto statusArea = getLocalBounds().withTrimmedRight (46).removeFromRight (112).removeFromTop (16);
            g.drawText (status, statusArea, juce::Justification::centredRight, false);
        }

        // Up / down chevrons.
        auto drawGlyph = [&] (juce::Rectangle<int> area, const juce::String& glyph, bool enabledGlyph)
        {
            g.setFont (theme::monoFont (13.0f, true));
            g.setColour (enabledGlyph ? (hover ? theme::silver : theme::textDim) : theme::textFaint.withAlpha (0.4f));
            g.drawText (glyph, area, juce::Justification::centred, false);
        };
        drawGlyph (upArea, arrowUp(), canMoveUp);
        drawGlyph (downArea, arrowDown(), canMoveDown);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        enableToggle.setBounds (30, bounds.getCentreY() - 10, 20, 20);

        const int buttonX = bounds.getRight() - 40;
        upArea = { buttonX, bounds.getY() + 6, 24, bounds.getHeight() / 2 - 8 };
        downArea = { buttonX, bounds.getCentreY() + 2, 24, bounds.getHeight() / 2 - 8 };
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (upArea.contains (e.getPosition()) && canMoveUp && onMoveUp != nullptr)
            onMoveUp (slot);
        else if (downArea.contains (e.getPosition()) && canMoveDown && onMoveDown != nullptr)
            onMoveDown (slot);
    }

private:
    DspSlot slot;
    juce::ToggleButton enableToggle;
    int order = 0;
    bool isEnabled = false;
    bool isActive = false;
    bool toggleAvailable = true;
    bool canMoveUp = false;
    bool canMoveDown = false;
    bool updating = false;
    juce::String status;
    juce::Rectangle<int> upArea, downArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlotRow)
};

//==============================================================================
DspChainPanel::DspChainPanel (AppContext& context) : ctx (context)
{
    setOpaque (true);

    masterBypass.setClickingTogglesState (true);
    masterBypass.setTooltip ("Bypass every processor (Reference Mode also sets volume to unity)");
    masterBypass.onClick = [this]
    {
        if (updating)
            return;
        auto p = ctx.getCurrentDspParams();
        p.bypassed = masterBypass.getToggleState();
        applyParams (p);
        ctx.setStatusMessage (p.bypassed ? "DSP chain bypassed." : "DSP chain enabled.");
    };
    addAndMakeVisible (masterBypass);

    eqBypass.setClickingTogglesState (true);
    eqBypass.setTooltip ("Bypass graphic, parametric and bass EQ only");
    eqBypass.onClick = [this]
    {
        if (updating)
            return;
        auto p = ctx.getCurrentDspParams();
        p.eqBypass = eqBypass.getToggleState();
        applyParams (p);
        ctx.setStatusMessage (p.eqBypass ? "EQ bypassed." : "EQ enabled.");
    };
    addAndMakeVisible (eqBypass);

    defaultOrderButton.setTooltip ("Restore processing order: ReplayGain, EQs, bass, filters, convolution, stereo, preamp, limiter");
    defaultOrderButton.onClick = [this]
    {
        auto p = ctx.getCurrentDspParams();
        p.chainOrder = { DspSlot::replayGain, DspSlot::graphicEq, DspSlot::parametricEq,
                         DspSlot::bass, DspSlot::filters, DspSlot::convolution,
                         DspSlot::crossfeed, DspSlot::stereoWidth, DspSlot::balance,
                         DspSlot::preamp, DspSlot::limiter };
        applyParams (p);
        ctx.setStatusMessage ("DSP chain order restored to default.");
    };
    addAndMakeVisible (defaultOrderButton);

    addAndMakeVisible (hintLabel);
    ui::configureSmallLabel (hintLabel, theme::textFaint);
    hintLabel.setText ("DRAG ORDER MOVES THE ACTUAL PROCESSING ORDER - RATE, EQ AND BASS STAY PHASE-CONSISTENT",
                       juce::dontSendNotification);

    viewport.setScrollBarsShown (true, false);
    viewport.setViewedComponent (&rowHolder, false);
    addAndMakeVisible (viewport);

    setSize (860, 620);
    rebuildRows();
    refreshNow();
    startTimerHz (5);
}

DspChainPanel::~DspChainPanel()
{
    stopTimer();
    viewport.setViewedComponent (nullptr, false);
    rows.clear();
}

//==============================================================================
void DspChainPanel::refreshNow()
{
    refreshRows();
    repaint();
}

void DspChainPanel::timerCallback()
{
    refreshRows();
}

void DspChainPanel::rebuildRows()
{
    const auto p = ctx.getCurrentDspParams();

    bool orderChanged = false;
    for (size_t i = 0; i < shownOrder.size(); ++i)
    {
        if (shownOrder[i] != p.chainOrder[i] || rows.size() != shownOrder.size())
            orderChanged = true;
    }
    if (rows.size() == shownOrder.size() && ! orderChanged)
    {
        refreshRows();
        return;
    }

    rows.clear();
    rowHolder.removeAllChildren();

    const size_t count = static_cast<size_t> (DspSlot::numSlots);
    for (size_t i = 0; i < count; ++i)
    {
        const auto slot = p.chainOrder[i];
        auto row = std::make_unique<SlotRow> (slot);
        row->onEnableChanged = [this] (DspSlot s, bool on)
        {
            if (updating)
                return;
            setSlotEnabled (s, on);
        };
        row->onMoveUp = [this] (DspSlot s)
        {
            for (int j = 0; j < (int) shownOrder.size(); ++j)
                if (shownOrder[(size_t) j] == s)
                    moveSlot (j, -1);
        };
        row->onMoveDown = [this] (DspSlot s)
        {
            for (int j = 0; j < (int) shownOrder.size(); ++j)
                if (shownOrder[(size_t) j] == s)
                    moveSlot (j, +1);
        };
        rowHolder.addAndMakeVisible (*row);
        rows.push_back (std::move (row));
    }

    shownOrder = p.chainOrder;
    resized();
    refreshRows();
}

void DspChainPanel::refreshRows()
{
    const auto p = ctx.getCurrentDspParams();

    bool orderChanged = false;
    for (size_t i = 0; i < shownOrder.size(); ++i)
        if (shownOrder[i] != p.chainOrder[i])
            orderChanged = true;

    if (orderChanged)
    {
        rebuildRows();
        return;
    }

    updating = true;
    masterBypass.setToggleState (p.bypassed, juce::dontSendNotification);
    eqBypass.setToggleState (p.eqBypass, juce::dontSendNotification);
    updating = false;

    for (int i = 0; i < (int) rows.size(); ++i)
    {
        const auto slot = rows[(size_t) i]->getSlot();
        juce::String status;
        const bool active = slotActive (slot);
        if (p.bypassed)
            status = "BYPASSED";
        else
            status = active ? "ACTIVE" : "IDLE";

        rows[(size_t) i]->setState (i, slotEnabled (slot), active, slotCanToggle (slot), status);
        rows[(size_t) i]->setMoveEnabled (i > 0, i + 1 < (int) rows.size());
    }

    hintLabel.setColour (juce::Label::textColourId, p.bypassed ? theme::warning : theme::textFaint);
}

bool DspChainPanel::slotEnabled (DspSlot slot) const
{
    const auto p = ctx.getCurrentDspParams();
    switch (slot)
    {
        case DspSlot::replayGain:   return p.replayGain.enabled;
        case DspSlot::graphicEq:    return p.graphic.enabled;
        case DspSlot::parametricEq: return p.parametric.enabled;
        case DspSlot::bass:         return p.bass.enabled;
        case DspSlot::filters:      return p.filters.isActive();
        case DspSlot::convolution:  return p.convolution.enabled;
        case DspSlot::crossfeed:    return p.stereo.crossfeedEnabled;
        case DspSlot::stereoWidth:  return std::abs (p.stereo.widthPercent - 100.0f) > 0.01f;
        case DspSlot::balance:      return std::abs (p.stereo.balance) > 0.0001f;
        case DspSlot::preamp:       return p.preamp.autoHeadroom || std::abs (p.preamp.manualPreampDb) > 0.0001f;
        case DspSlot::limiter:      return p.preamp.limiterEnabled;
        case DspSlot::numSlots:     break;
    }
    return false;
}

bool DspChainPanel::slotCanToggle (DspSlot slot) const
{
    // Width and balance are continuous values, edited in the Stereo tab.
    return slot != DspSlot::stereoWidth && slot != DspSlot::balance;
}

bool DspChainPanel::slotActive (DspSlot slot) const
{
    const auto p = ctx.getCurrentDspParams();
    if (p.bypassed)
        return false;

    switch (slot)
    {
        case DspSlot::replayGain:   return p.replayGain.enabled;
        case DspSlot::graphicEq:    return p.graphic.enabled && ! p.eqBypass && ! p.graphic.isFlat();
        case DspSlot::parametricEq: return p.parametric.enabled && ! p.eqBypass && p.parametric.hasActiveBands();
        case DspSlot::bass:         return p.bass.isActive() && ! p.eqBypass;
        case DspSlot::filters:      return p.filters.isActive();
        case DspSlot::convolution:  return p.convolution.enabled && p.convolution.irPath.isNotEmpty();
        case DspSlot::crossfeed:    return p.stereo.crossfeedEnabled;
        case DspSlot::stereoWidth:  return std::abs (p.stereo.widthPercent - 100.0f) > 0.01f;
        case DspSlot::balance:      return std::abs (p.stereo.balance) > 0.0001f;
        case DspSlot::preamp:       return std::abs (p.preamp.manualPreampDb) > 0.0001f
                                           || (p.preamp.autoHeadroom && p.maxPositiveBoostDb() > 0.001);
        case DspSlot::limiter:      return p.preamp.limiterEnabled;
        case DspSlot::numSlots:     break;
    }
    return false;
}

void DspChainPanel::setSlotEnabled (DspSlot slot, bool on)
{
    auto p = ctx.getCurrentDspParams();
    switch (slot)
    {
        case DspSlot::replayGain:   p.replayGain.enabled = on; break;
        case DspSlot::graphicEq:    p.graphic.enabled = on; break;
        case DspSlot::parametricEq: p.parametric.enabled = on; break;
        case DspSlot::bass:         p.bass.enabled = on; break;
        case DspSlot::filters:
            p.filters.highPassEnabled = on;
            p.filters.lowPassEnabled = on;
            break;
        case DspSlot::convolution:  p.convolution.enabled = on; break;
        case DspSlot::crossfeed:    p.stereo.crossfeedEnabled = on; break;
        case DspSlot::stereoWidth:  p.stereo.widthPercent = on ? 120.0f : 100.0f; break;
        case DspSlot::balance:      p.stereo.balance = 0.0f; break;
        case DspSlot::preamp:
            p.preamp.autoHeadroom = on;
            if (! on)
                p.preamp.manualPreampDb = 0.0f;
            break;
        case DspSlot::limiter:      p.preamp.limiterEnabled = on; break;
        case DspSlot::numSlots:     return;
    }
    applyParams (p);
    refreshRows();
}

void DspChainPanel::moveSlot (int rowIndex, int delta)
{
    const int target = rowIndex + delta;
    if (rowIndex < 0 || target < 0 || target >= (int) rows.size())
        return;

    auto p = ctx.getCurrentDspParams();
    std::swap (p.chainOrder[(size_t) rowIndex], p.chainOrder[(size_t) target]);
    applyParams (p);
    rebuildRows();
    ctx.setStatusMessage (juce::String ("Moved ") + dspSlotName (p.chainOrder[(size_t) target])
                          + (delta < 0 ? " earlier" : " later") + " in the DSP chain.");
}

void DspChainPanel::applyParams (const DspParams& p)
{
    ctx.applyUserDspParams (p);
}

//==============================================================================
void DspChainPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    theme::drawSectionHeader (g, headerArea, "PROCESSING CHAIN", "ORDERED - SIGNAL FLOWS TOP TO BOTTOM");

    if (listArea.isEmpty())
        return;

    theme::fillSunken (g, listArea);
    g.setColour (theme::line);
    g.drawRect (listArea, 1);

    const auto p = ctx.getCurrentDspParams();
    if (p.bypassed)
    {
        g.setColour (theme::warning.withAlpha (0.75f));
        g.setFont (theme::capsFont (9.0f, 0.10f));
        g.drawText ("ALL PROCESSING BYPASSED", listArea.reduced (10, 6).removeFromTop (12),
                    juce::Justification::centredRight, false);
    }
}

void DspChainPanel::resized()
{
    auto r = getLocalBounds().reduced (14, 10);
    headerArea = r.removeFromTop (22);
    r.removeFromTop (8);

    toolbarArea = r.removeFromTop (30);
    r.removeFromTop (8);

    footerArea = r.removeFromBottom (18);
    r.removeFromBottom (4);
    listArea = r;

    masterBypass.setBounds (toolbarArea.removeFromLeft (160));
    toolbarArea.removeFromLeft (10);
    eqBypass.setBounds (toolbarArea.removeFromLeft (128));
    toolbarArea.removeFromLeft (10);
    defaultOrderButton.setBounds (toolbarArea.removeFromLeft (140));
    hintLabel.setBounds (footerArea);

    auto listInner = listArea.reduced (1);
    viewport.setBounds (listInner);
    const int rowWidth = juce::jmax (520, viewport.getWidth() - 14);
    rowHolder.setSize (rowWidth, (int) rows.size() * kRowHeight);
    for (int i = 0; i < (int) rows.size(); ++i)
        rows[(size_t) i]->setBounds (0, i * kRowHeight, rowWidth, kRowHeight);
}

} // namespace sp
