/*
    Snake Puppy Reference Player - 15/31 band graphic EQ editor.
    Message thread only.
*/
#include "UI/GraphicEqEditor.h"

#include <cmath>

namespace sp
{

namespace
{
using ui::dbText;

juce::String compactFreqLabel (double hz)
{
    if (hz >= 1000.0)
    {
        const double k = hz / 1000.0;
        if (std::abs (k - std::round (k)) < 0.05)
            return juce::String ((int) std::round (k)) + "k";
        return juce::String (k, 1) + "k";
    }
    return juce::String ((int) std::round (hz));
}

float graphicGainFor (const GraphicEqParams& g, int bandIndex)
{
    if (g.bandSet == GraphicEqParams::BandSet::bands31)
        return g.gains31[(size_t) juce::jlimit (0, GraphicEqParams::kNumBands31 - 1, bandIndex)];
    return g.gains15[(size_t) juce::jlimit (0, GraphicEqParams::kNumBands15 - 1, bandIndex)];
}

double graphicCentreFor (const GraphicEqParams& g, int bandIndex)
{
    if (g.bandSet == GraphicEqParams::BandSet::bands31)
        return GraphicEqParams::centres31()[(size_t) juce::jlimit (0, GraphicEqParams::kNumBands31 - 1, bandIndex)];
    return GraphicEqParams::centres15()[(size_t) juce::jlimit (0, GraphicEqParams::kNumBands15 - 1, bandIndex)];
}
} // namespace

//==============================================================================
/** One machined, bipolar EQ fader. Drawn by hand so the centre detent reads as
    0 dB: boost fills gold upward, cut fills blue downward. */
class GraphicEqEditor::BandFader : public juce::Component
{
public:
    BandFader (int bandIndex, double centreHz, float minimumDb, float maximumDb)
        : index (bandIndex), freqHz (centreHz), minDb (minimumDb), maxDb (maximumDb)
    {
        setWantsKeyboardFocus (false);
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        setRepaintsOnMouseActivity (true);
    }

    void setGain (float newGain)
    {
        newGain = juce::jlimit (minDb, maxDb, newGain);
        if (std::abs (newGain - gain) < 0.001f)
            return;
        gain = newGain;
        repaint();
    }

    void setDbRange (float minimumDb, float maximumDb)
    {
        minDb = minimumDb;
        maxDb = maximumDb;
        gain = juce::jlimit (minDb, maxDb, gain);
        repaint();
    }

    float getGain() const noexcept { return gain; }
    int getBandIndex() const noexcept { return index; }

    std::function<void (int, float)> onGainChanged;

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        const bool hot = isMouseOverOrDragging();
        const bool dragging = isMouseButtonDown();

        const float labelH = 14.0f;
        const float readoutH = 14.0f;
        auto labelArea = bounds.removeFromTop (labelH);
        auto readoutArea = bounds.removeFromBottom (readoutH);
        auto track = bounds.reduced (1.0f, 3.0f);

        const float cx = track.getCentreX();
        const float trackW = juce::jmin (6.0f, track.getWidth() * 0.42f);
        const float usableH = juce::jmax (10.0f, track.getHeight() - 10.0f);
        const float dbPerPixel = (maxDb - minDb) / usableH;
        const float top = track.getY() + 5.0f;
        const float zeroY = top + (maxDb - 0.0f) / dbPerPixel;
        const float valueY = top + (maxDb - gain) / dbPerPixel;

        // Column background on hover.
        if (hot)
        {
            g.setColour (theme::hoverFill);
            g.fillRoundedRectangle (bounds.reduced (1.0f), 2.0f);
        }

        // Ticks at each quarter of the range, 0 dB accented.
        for (int i = 0; i <= 4; ++i)
        {
            const float db = minDb + (maxDb - minDb) * (float) i / 4.0f;
            const float y = top + (maxDb - db) / dbPerPixel;
            const bool zero = std::abs (db) < 0.01f;
            g.setColour (zero ? theme::goldDim.withAlpha (0.9f) : theme::line.withAlpha (0.85f));
            g.drawLine (cx - 8.0f, y, cx + 8.0f, y, zero ? 1.3f : 1.0f);
        }

        // Track.
        juce::Rectangle<float> trackRect (cx - trackW * 0.5f, top, trackW, usableH);
        g.setColour (theme::panelSunken);
        g.fillRoundedRectangle (trackRect, trackW * 0.5f);
        g.setColour (theme::line);
        g.drawRoundedRectangle (trackRect, trackW * 0.5f, 1.0f);

        // Bipolar fill: gold for boost, cool blue for cut.
        if (std::abs (gain) > 0.02f)
        {
            const float fillTop = juce::jmin (zeroY, valueY);
            const float fillBottom = juce::jmax (zeroY, valueY);
            juce::Rectangle<float> fill (cx - trackW * 0.5f, fillTop, trackW, fillBottom - fillTop);
            const auto colour = gain >= 0.0f ? theme::gold : theme::info;
            g.setGradientFill (juce::ColourGradient (colour.darker (0.25f), cx, fillTop,
                                                     colour.brighter (0.15f), cx, fillBottom, false));
            g.fillRoundedRectangle (fill, trackW * 0.5f);
        }

        // Machined thumb.
        const float capW = juce::jmin (bounds.getWidth() - 4.0f, 26.0f);
        juce::Rectangle<float> cap (cx - capW * 0.5f, valueY - 4.5f, capW, 9.0f);
        g.setGradientFill (juce::ColourGradient (theme::silver.brighter (0.20f), 0.0f, cap.getY(),
                                                 theme::silverDark, 0.0f, cap.getBottom(), false));
        g.fillRoundedRectangle (cap, 1.8f);
        g.setColour (dragging ? theme::goldBright : theme::lineStrong);
        g.drawRoundedRectangle (cap, 1.8f, 1.0f);
        g.setColour (theme::background.withAlpha (0.6f));
        g.drawLine (cap.getX() + 3.0f, cap.getCentreY(), cap.getRight() - 3.0f, cap.getCentreY(), 1.0f);

        // Frequency label.
        g.setColour (hot ? theme::silver : theme::textDim);
        g.setFont (theme::capsFont (juce::jlimit (7.5f, 9.5f, bounds.getWidth() * 0.30f), 0.04f));
        g.drawText (compactFreqLabel (freqHz), labelArea.toNearestInt(), juce::Justification::centred, false);

        // Gain readout.
        const bool narrow = bounds.getWidth() < 38.0f;
        g.setColour (std::abs (gain) < 0.05f ? theme::textFaint
                                             : (gain > 0.0f ? theme::goldBright : theme::info));
        g.setFont (theme::monoFont (juce::jlimit (8.0f, 10.5f, bounds.getWidth() * 0.30f)));
        g.drawText (dbText (gain, narrow ? 0 : 1), readoutArea.toNearestInt(),
                    juce::Justification::centred, false);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStartGain = gain;
        dragStartY = e.position.y;
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto bounds = getLocalBounds().toFloat();
        const float usableH = juce::jmax (10.0f, bounds.getHeight() - 14.0f - 14.0f - 16.0f);
        const float dbPerPixel = (maxDb - minDb) / usableH;
        const float newGain = juce::jlimit (minDb, maxDb,
                                            dragStartGain + (dragStartY - e.position.y) * dbPerPixel);
        if (std::abs (newGain - gain) < 0.001f)
            return;
        gain = newGain;
        repaint();
        if (onGainChanged != nullptr)
            onGainChanged (index, gain);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (std::abs (gain) < 0.01f)
            return;
        gain = 0.0f;
        repaint();
        if (onGainChanged != nullptr)
            onGainChanged (index, gain);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (std::abs (wheel.deltaY) < 0.001f)
            return;
        const float step = wheel.isReversed ? -0.5f : 0.5f;
        const float newGain = juce::jlimit (minDb, maxDb, gain + (wheel.deltaY > 0.0f ? step : -step));
        if (std::abs (newGain - gain) < 0.001f)
            return;
        gain = newGain;
        repaint();
        if (onGainChanged != nullptr)
            onGainChanged (index, gain);
        juce::ignoreUnused (e);
    }

private:
    int index = 0;
    double freqHz = 1000.0;
    float minDb = -6.0f, maxDb = 6.0f;
    float gain = 0.0f;
    float dragStartGain = 0.0f;
    float dragStartY = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandFader)
};

//==============================================================================
GraphicEqEditor::GraphicEqEditor (AppContext& context)
    : ctx (context), curve (context, false)
{
    setOpaque (true);

    header.setShowRule (true);
    addAndMakeVisible (header);

    addAndMakeVisible (curve);
    curve.setEditableFamilies (true, false, false);
    curve.setCaption ("GRAPHIC EQ");

    bandSetBox.addItem ("15 BAND", 1);
    bandSetBox.addItem ("31 BAND", 2);
    bandSetBox.setSelectedId (1, juce::dontSendNotification);
    bandSetBox.onChange = [this]
    {
        if (updating)
            return;
        auto p = currentParams();
        p.graphic.bandSet = bandSetBox.getSelectedId() == 2
                                ? GraphicEqParams::BandSet::bands31
                                : GraphicEqParams::BandSet::bands15;
        p.graphic.enabled = true;
        applyGraphicParams (p.graphic, true);
    };
    addAndMakeVisible (bandSetBox);

    rangeBox.addItem (juce::String::fromUTF8 ("\xc2\xb1") + "6 dB", 1);
    rangeBox.addItem (juce::String::fromUTF8 ("\xc2\xb1") + "12 dB", 2);
    rangeBox.setSelectedId (1, juce::dontSendNotification);
    rangeBox.onChange = [this]
    {
        if (updating)
            return;
        auto p = currentParams();
        p.graphic.rangeDb = rangeBox.getSelectedId() == 2 ? 12.0f : 6.0f;
        p.graphic.enabled = true;
        applyGraphicParams (p.graphic, true);
    };
    addAndMakeVisible (rangeBox);

    flatButton.onClick = [this]
    {
        if (updating)
            return;
        auto p = currentParams();
        p.graphic.enabled = true;
        p.graphic.reset();
        ctx.applyUserDspParams (p);
        syncFromContext (true);
        curve.refreshNow();
        ctx.setStatusMessage ("Graphic EQ flattened.");
    };
    addAndMakeVisible (flatButton);

    bypassButton.setClickingTogglesState (true);
    bypassButton.onClick = [this]
    {
        if (updating)
            return;
        auto p = currentParams();
        p.eqBypass = bypassButton.getToggleState();
        ctx.applyUserDspParams (p);
        syncFromContext (true);
        curve.refreshNow();
        ctx.setStatusMessage (p.eqBypass ? "EQ bypassed." : "EQ enabled.");
    };
    addAndMakeVisible (bypassButton);

    addAndMakeVisible (statusLabel);
    statusLabel.setTextStyle (10.5f, theme::textDim, 0.08f);

    setSize (1000, 610);
    syncFromContext (true);
    startTimerHz (6);
}

GraphicEqEditor::~GraphicEqEditor()
{
    stopTimer();
    faders.clear();
}

//==============================================================================
void GraphicEqEditor::refreshNow()
{
    syncFromContext (true);
    curve.refreshNow();
}

void GraphicEqEditor::timerCallback()
{
    syncFromContext (false);
}

void GraphicEqEditor::rebuildFaders()
{
    faders.clear();

    const auto p = currentParams();
    const int count = p.graphic.bandSet == GraphicEqParams::BandSet::bands31
                          ? GraphicEqParams::kNumBands31
                          : GraphicEqParams::kNumBands15;
    const float range = p.graphic.rangeDb > 9.0f ? 12.0f : 6.0f;

    faders.reserve ((size_t) count);
    for (int i = 0; i < count; ++i)
    {
        auto fader = std::make_unique<BandFader> (i, graphicCentreFor (p.graphic, i), -range, range);
        fader->setGain (graphicGainFor (p.graphic, i));
        fader->onGainChanged = [this] (int bandIndex, float gainDb)
        {
            if (updating)
                return;
            ctx.setGraphicEqBandGain (bandIndex, gainDb);
            updateHeaderState();
            curve.refreshNow();
        };
        addAndMakeVisible (*fader);
        faders.push_back (std::move (fader));
    }

    builtBandSet = (int) p.graphic.bandSet;
    builtRange = range;
    resized();
}

void GraphicEqEditor::layoutFaders (juce::Rectangle<int> area)
{
    if (faders.empty())
        return;

    const int count = (int) faders.size();
    const int totalWidth = juce::jmax (1, area.getWidth());
    int x = area.getX();
    for (int i = 0; i < count; ++i)
    {
        const int nextX = area.getX() + (int) std::round ((double) totalWidth * (double) (i + 1) / (double) count);
        const int width = nextX - x;
        faders[(size_t) i]->setBounds (x, area.getY(), width, area.getHeight());
        x = nextX;
    }
}

void GraphicEqEditor::syncFromContext (bool force)
{
    if (updating)
        return;

    const auto p = currentParams();
    const int bandSet = (int) p.graphic.bandSet;
    const float range = p.graphic.rangeDb > 9.0f ? 12.0f : 6.0f;

    bool needsRebuild = force || bandSet != builtBandSet || std::abs (range - builtRange) > 0.01f;

    if (needsRebuild)
        rebuildFaders();

    updating = true;
    bandSetBox.setSelectedId (bandSet == (int) GraphicEqParams::BandSet::bands31 ? 2 : 1,
                              juce::dontSendNotification);
    rangeBox.setSelectedId (range > 9.0f ? 2 : 1, juce::dontSendNotification);
    bypassButton.setToggleState (p.eqBypass, juce::dontSendNotification);

    const int count = p.graphic.bandSet == GraphicEqParams::BandSet::bands31
                          ? GraphicEqParams::kNumBands31
                          : GraphicEqParams::kNumBands15;
    for (int i = 0; i < (int) faders.size() && i < count; ++i)
        faders[(size_t) i]->setGain (graphicGainFor (p.graphic, i));
    updating = false;

    updateHeaderState();
}

void GraphicEqEditor::updateHeaderState()
{
    const auto p = currentParams();

    int setBands = 0;
    if (p.graphic.bandSet == GraphicEqParams::BandSet::bands31)
    {
        for (auto g : p.graphic.gains31)
            if (std::abs (g) > 0.05f)
                ++setBands;
    }
    else
    {
        for (auto g : p.graphic.gains15)
            if (std::abs (g) > 0.05f)
                ++setBands;
    }

    juce::String status;
    if (p.bypassed)
        status << "DSP BYPASSED";
    else if (p.eqBypass)
        status << "EQ BYPASSED";
    else
    {
        const int count = p.graphic.bandSet == GraphicEqParams::BandSet::bands31
                              ? GraphicEqParams::kNumBands31
                              : GraphicEqParams::kNumBands15;
        status << count << " BANDS  |  " << (p.graphic.rangeDb > 9.0f ? "12" : "6") << " dB  |  "
               << (setBands == 0 ? "FLAT" : juce::String (setBands) + " SET");
    }

    if (status != lastStatus)
    {
        lastStatus = status;
        statusLabel.setText (status, juce::dontSendNotification);
    }
}

void GraphicEqEditor::applyGraphicParams (const GraphicEqParams& g, bool)
{
    DspParams p = ctx.getCurrentDspParams();
    p.graphic = g;
    ctx.applyUserDspParams (p);
    syncFromContext (true);
    curve.refreshNow();
}

//==============================================================================
void GraphicEqEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
}

void GraphicEqEditor::resized()
{
    auto r = getLocalBounds().reduced (14, 10);
    header.setBounds (r.removeFromTop (30));
    r.removeFromTop (8);

    auto controls = r.removeFromTop (34);
    bypassButton.setBounds (controls.removeFromRight (132));
    controls.removeFromRight (8);
    flatButton.setBounds (controls.removeFromRight (72));
    controls.removeFromRight (8);
    rangeBox.setBounds (controls.removeFromRight (92));
    controls.removeFromRight (8);
    bandSetBox.setBounds (controls.removeFromRight (108));
    controls.removeFromRight (10);
    statusLabel.setBounds (controls.withTrimmedRight (4));

    r.removeFromTop (10);
    const int curveHeight = juce::jlimit (140, 280, r.getHeight() * 46 / 100);
    curve.setBounds (r.removeFromTop (curveHeight));
    r.removeFromTop (10);

    faderArea = r;
    layoutFaders (faderArea);
}

} // namespace sp
