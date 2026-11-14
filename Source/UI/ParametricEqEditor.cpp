/*
    Snake Puppy Reference Player - 12-band parametric EQ editor.
    Message thread only.
*/
#include "UI/ParametricEqEditor.h"

#include <cmath>

namespace sp
{

namespace
{
using ui::dbText;
using ui::qText;
using ui::styleButton;

constexpr int kRowHeight = 86;

juce::String compactFreq (double hz)
{
    if (hz >= 1000.0)
    {
        const double k = hz / 1000.0;
        return juce::String (k, k >= 10.0 ? 1 : 2) + "k";
    }
    return juce::String ((int) std::round (hz));
}

void configureKnob (SpKnob& knob, double minValue, double maxValue,
                    double midpoint, double doubleClickValue,
                    std::function<juce::String (double)> formatter)
{
    knob.setRange (minValue, maxValue, 0.0);
    if (midpoint > minValue && midpoint < maxValue)
        knob.setSkewFactorFromMidPoint (midpoint);
    knob.setDoubleClickReturnValue (true, doubleClickValue);
    knob.setMouseDragSensitivity (180);
    knob.textFromValueFunction = std::move (formatter);
    knob.setValue (doubleClickValue, juce::dontSendNotification);
}
} // namespace

//==============================================================================
class ParametricEqEditor::BandRow : public juce::Component
{
public:
    explicit BandRow (int bandIndex)
        : index (bandIndex),
          typeBox ("Type"),
          freqKnob ("FREQ"),
          gainKnob ("GAIN"),
          qKnob ("Q")
    {
        setOpaque (false);
        setInterceptsMouseClicks (true, true);

        enableToggle.setTooltip ("Enable this band");
        enableToggle.onClick = [this]
        {
            if (updating)
                return;
            band.enabled = enableToggle.getToggleState();
            notifyChange();
        };
        addAndMakeVisible (enableToggle);

        for (int i = 0; i < 8; ++i)
            typeBox.addItem (bandTypeName (static_cast<BandType> (i)), i + 1);
        typeBox.setSelectedId (1, juce::dontSendNotification);
        typeBox.onChange = [this]
        {
            if (updating)
                return;
            band.type = static_cast<BandType> (juce::jlimit (0, 7, typeBox.getSelectedId() - 1));
            notifyChange();
        };
        addAndMakeVisible (typeBox);

        bypassToggle.setButtonText ("BYP");
        bypassToggle.setClickingTogglesState (true);
        bypassToggle.setTooltip ("Bypass this band without disabling it");
        styleButton (bypassToggle);
        bypassToggle.onClick = [this]
        {
            if (updating)
                return;
            band.bypassed = bypassToggle.getToggleState();
            notifyChange();
        };
        addAndMakeVisible (bypassToggle);

        configureKnob (freqKnob, 20.0, 20000.0, 1000.0, 1000.0,
                       [] (double v) { return compactFreq (v); });
        freqKnob.onValueChange = [this]
        {
            if (updating)
                return;
            band.freqHz = freqKnob.getValue();
            notifyChange();
        };
        addAndMakeVisible (freqKnob);

        configureKnob (gainKnob, -15.0, 15.0, 0.0, 0.0,
                       [] (double v) { return dbText (v, 1); });
        gainKnob.onValueChange = [this]
        {
            if (updating)
                return;
            band.gainDb = gainKnob.getValue();
            notifyChange();
        };
        addAndMakeVisible (gainKnob);

        configureKnob (qKnob, 0.1, 18.0, 1.0, 0.707,
                       [] (double v) { return qText (v); });
        qKnob.onValueChange = [this]
        {
            if (updating)
                return;
            band.q = qKnob.getValue();
            notifyChange();
        };
        addAndMakeVisible (qKnob);

        resetButton.setTooltip ("Reset this band to its default type, frequency and Q");
        styleButton (resetButton);
        resetButton.onClick = [this]
        {
            ParametricEqParams defaults;
            auto def = defaults.bands[(size_t) index];
            def.enabled = true;
            def.bypassed = false;
            setFromBand (def);
            notifyChange();
        };
        addAndMakeVisible (resetButton);
    }

    void setFromBand (const EqBand& newBand)
    {
        band = newBand;
        updating = true;
        enableToggle.setToggleState (band.enabled, juce::dontSendNotification);
        bypassToggle.setToggleState (band.bypassed, juce::dontSendNotification);
        typeBox.setSelectedId ((int) band.type + 1, juce::dontSendNotification);
        freqKnob.setValue (juce::jlimit (20.0, 20000.0, band.freqHz), juce::dontSendNotification);
        gainKnob.setValue (juce::jlimit (-15.0, 15.0, band.gainDb), juce::dontSendNotification);
        qKnob.setValue (juce::jlimit (0.1, 18.0, band.q), juce::dontSendNotification);
        updating = false;
        repaint();
    }

    const EqBand& getBand() const noexcept { return band; }

    std::function<void (int, const EqBand&)> onBandChanged;

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        const bool active = band.enabled && ! band.bypassed;

        g.setColour (index % 2 == 0 ? theme::panel.withAlpha (0.55f) : theme::panelAlt.withAlpha (0.35f));
        g.fillRect (bounds);

        if (active)
        {
            g.setColour (theme::gold);
            g.fillRect (bounds.getX(), bounds.getY() + 8, 2, bounds.getHeight() - 16);
        }

        g.setColour (theme::lineFaint);
        g.fillRect (bounds.getX(), bounds.getBottom() - 1, bounds.getWidth(), 1);

        // Band index and state.
        auto numberArea = juce::Rectangle<int> (bounds.getX(), bounds.getY(), 30, bounds.getHeight());
        g.setColour (active ? theme::goldBright : theme::textFaint);
        g.setFont (theme::monoFont (13.0f, active));
        g.drawText (juce::String (index + 1).paddedLeft ('0', 2),
                    numberArea.withTrimmedBottom (13), juce::Justification::centred, false);
        g.setFont (theme::capsFont (7.5f, 0.08f));
        g.setColour (active ? theme::silverDim : theme::textFaint);
        g.drawText (active ? "ON" : "OFF",
                    numberArea.withTrimmedTop (numberArea.getHeight() - 14),
                    juce::Justification::centred, false);
    }

    void resized() override
    {
        // The first 30 px of the row is painted (index + state); controls start after it.
        auto controls = getLocalBounds().withTrimmedLeft (30).reduced (4, 5);

        enableToggle.setBounds (controls.removeFromLeft (20).withSizeKeepingCentre (20, 20));
        controls.removeFromLeft (4);
        typeBox.setBounds (controls.removeFromLeft (118));
        controls.removeFromLeft (8);
        bypassToggle.setBounds (controls.removeFromLeft (46));

        resetButton.setBounds (controls.removeFromRight (58));
        controls.removeFromRight (8);

        const int knobWidth = juce::jlimit (56, 92, controls.getWidth() / 3);
        auto knobs = controls;
        const int totalKnobs = knobWidth * 3;
        knobs = knobs.withSizeKeepingCentre (juce::jmin (totalKnobs, knobs.getWidth()), knobs.getHeight());
        freqKnob.setBounds (knobs.removeFromLeft (knobWidth).reduced (2));
        gainKnob.setBounds (knobs.removeFromLeft (knobWidth).reduced (2));
        qKnob.setBounds (knobs.removeFromLeft (knobWidth).reduced (2));
    }

private:
    void notifyChange()
    {
        repaint();
        if (onBandChanged != nullptr)
            onBandChanged (index, band);
    }

    int index = 0;
    EqBand band;
    bool updating = false;

    juce::ToggleButton enableToggle;
    SpComboBox typeBox;
    juce::TextButton bypassToggle;
    SpKnob freqKnob, gainKnob, qKnob;
    juce::TextButton resetButton { "RESET" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandRow)
};

//==============================================================================
ParametricEqEditor::ParametricEqEditor (AppContext& context)
    : ctx (context), curve (context, false)
{
    setOpaque (true);

    header.setShowRule (true);
    addAndMakeVisible (header);

    addAndMakeVisible (curve);
    curve.setEditableFamilies (false, true, false);
    curve.setCaption ("PARAMETRIC EQ");

    addAndMakeVisible (statusLabel);
    statusLabel.setTextStyle (10.5f, theme::textDim, 0.08f);

    addBandButton.onClick = [this]
    {
        const int index = firstDisabledBand();
        if (index < 0)
        {
            ctx.setStatusMessage ("All 12 parametric bands are already active.");
            return;
        }
        auto p = ctx.getCurrentDspParams();
        p.parametric.enabled = true;
        p.parametric.bands[(size_t) index].enabled = true;
        ctx.applyUserDspParams (p);
        syncFromContext (true);

        const int y = juce::jlimit (0, juce::jmax (0, rowHolder.getHeight() - viewport.getViewHeight()),
                                    index * kRowHeight - 20);
        viewport.setViewPosition (0, y);
        ctx.setStatusMessage ("Enabled parametric band " + juce::String (index + 1) + ".");
        curve.refreshNow();
    };
    addAndMakeVisible (addBandButton);

    resetAllButton.onClick = [this]
    {
        auto p = ctx.getCurrentDspParams();
        p.parametric.reset();
        ctx.applyUserDspParams (p);
        syncFromContext (true);
        ctx.setStatusMessage ("Parametric EQ reset to defaults (all bands off).");
        curve.refreshNow();
    };
    addAndMakeVisible (resetAllButton);

    sectionToggle.setClickingTogglesState (true);
    sectionToggle.onClick = [this] { applySectionEnabled (sectionToggle.getToggleState()); };
    addAndMakeVisible (sectionToggle);

    eqBypassToggle.setClickingTogglesState (true);
    eqBypassToggle.onClick = [this] { applyEqBypass (eqBypassToggle.getToggleState()); };
    addAndMakeVisible (eqBypassToggle);

    viewport.setScrollBarsShown (true, false);
    viewport.setViewedComponent (&rowHolder, false);
    addAndMakeVisible (viewport);

    rows.reserve ((size_t) ParametricEqParams::kNumBands);
    for (int i = 0; i < ParametricEqParams::kNumBands; ++i)
    {
        auto row = std::make_unique<BandRow> (i);
        row->onBandChanged = [this] (int bandIndex, const EqBand& band) { applyBand (bandIndex, band); };
        rowHolder.addAndMakeVisible (*row);
        rows.push_back (std::move (row));
    }

    setSize (1000, 660);
    syncFromContext (true);
    startTimerHz (5);
}

ParametricEqEditor::~ParametricEqEditor()
{
    stopTimer();
    viewport.setViewedComponent (nullptr, false);
    rows.clear();
}

//==============================================================================
void ParametricEqEditor::refreshNow()
{
    syncFromContext (true);
    curve.refreshNow();
}

void ParametricEqEditor::timerCallback()
{
    syncFromContext (false);
}

int ParametricEqEditor::firstDisabledBand() const
{
    const auto p = ctx.getCurrentDspParams();
    for (int i = 0; i < ParametricEqParams::kNumBands; ++i)
        if (! p.parametric.bands[(size_t) i].enabled)
            return i;
    return -1;
}

void ParametricEqEditor::applyBand (int index, const EqBand& band)
{
    if (updating || index < 0 || index >= ParametricEqParams::kNumBands)
        return;

    auto p = ctx.getCurrentDspParams();
    p.parametric.bands[(size_t) index] = band;
    p.parametric.enabled = true;
    ctx.applyUserDspParams (p);
    updateStatus();
    curve.refreshNow();
}

void ParametricEqEditor::applySectionEnabled (bool enabled)
{
    if (updating)
        return;
    auto p = ctx.getCurrentDspParams();
    p.parametric.enabled = enabled;
    ctx.applyUserDspParams (p);
    updateStatus();
    curve.refreshNow();
    ctx.setStatusMessage (enabled ? "Parametric EQ enabled." : "Parametric EQ disabled.");
}

void ParametricEqEditor::applyEqBypass (bool bypassed)
{
    if (updating)
        return;
    auto p = ctx.getCurrentDspParams();
    p.eqBypass = bypassed;
    ctx.applyUserDspParams (p);
    updateStatus();
    curve.refreshNow();
    ctx.setStatusMessage (bypassed ? "EQ bypassed." : "EQ enabled.");
}

void ParametricEqEditor::syncFromContext (bool force)
{
    if (updating)
        return;

    const auto p = ctx.getCurrentDspParams();
    updating = true;
    for (int i = 0; i < (int) rows.size() && i < ParametricEqParams::kNumBands; ++i)
        rows[(size_t) i]->setFromBand (p.parametric.bands[(size_t) i]);
    sectionToggle.setToggleState (p.parametric.enabled, juce::dontSendNotification);
    eqBypassToggle.setToggleState (p.eqBypass, juce::dontSendNotification);
    updating = false;

    updateStatus();
    if (force)
        resized();
}

void ParametricEqEditor::updateStatus()
{
    const auto p = ctx.getCurrentDspParams();
    int active = 0;
    for (auto& b : p.parametric.bands)
        if (b.isActive())
            ++active;

    juce::String status;
    if (p.bypassed)
        status << "DSP BYPASSED";
    else if (p.eqBypass)
        status << "EQ BYPASSED";
    else if (! p.parametric.enabled)
        status << "SECTION OFF";
    else
        status << active << " / " << ParametricEqParams::kNumBands << " BANDS ACTIVE";

    if (status != lastStatus)
    {
        lastStatus = status;
        statusLabel.setText (status, juce::dontSendNotification);
    }
}

//==============================================================================
void ParametricEqEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
}

void ParametricEqEditor::resized()
{
    auto r = getLocalBounds().reduced (14, 10);
    header.setBounds (r.removeFromTop (30));
    r.removeFromTop (8);

    auto controls = r.removeFromTop (34);
    eqBypassToggle.setBounds (controls.removeFromRight (132));
    controls.removeFromRight (8);
    sectionToggle.setBounds (controls.removeFromRight (142));
    controls.removeFromRight (8);
    resetAllButton.setBounds (controls.removeFromRight (96));
    controls.removeFromRight (8);
    addBandButton.setBounds (controls.removeFromRight (98));
    controls.removeFromRight (12);
    statusLabel.setBounds (controls);

    r.removeFromTop (10);
    const int curveHeight = juce::jlimit (120, 230, r.getHeight() * 36 / 100);
    curve.setBounds (r.removeFromTop (curveHeight));
    r.removeFromTop (10);

    viewport.setBounds (r);
    const int rowWidth = juce::jmax (600, viewport.getWidth() - 14);
    rowHolder.setSize (rowWidth, (int) rows.size() * kRowHeight);
    for (int i = 0; i < (int) rows.size(); ++i)
        rows[(size_t) i]->setBounds (0, i * kRowHeight, rowWidth, kRowHeight);
}

} // namespace sp
