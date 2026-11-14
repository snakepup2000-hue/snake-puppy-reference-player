/*
    Snake Puppy Reference Player - scrolling spectrogram.
    Message thread only.
*/
#include "UI/SpectrogramComponent.h"

#include <array>
#include <cmath>
#include <iterator>

namespace sp
{

namespace
{
using ui::drawEmptyState;

struct ColourStop
{
    float position;
    juce::uint8 r, g, b;
};

const ColourStop kStops[] {
    { 0.00f, 0x0e, 0x0f, 0x11 },   // charcoal silence
    { 0.16f, 0x12, 0x1c, 0x2b },   // deep blue
    { 0.34f, 0x2b, 0x2f, 0x55 },   // indigo
    { 0.52f, 0x5a, 0x3f, 0x6d },   // violet
    { 0.70f, 0x8f, 0x5c, 0x43 },   // burnt amber
    { 0.84f, 0xc9, 0xa2, 0x27 },   // gold
    { 1.00f, 0xf4, 0xef, 0xdc }    // warm white
};

juce::Colour interpolateStops (float t)
{
    t = juce::jlimit (0.0f, 1.0f, t);
    for (size_t i = 1; i < std::size (kStops); ++i)
    {
        if (t <= kStops[i].position)
        {
            const auto& a = kStops[i - 1];
            const auto& b = kStops[i];
            const float span = juce::jmax (0.0001f, b.position - a.position);
            const float f = (t - a.position) / span;
            return juce::Colour::fromFloatRGBA (
                (float) a.r / 255.0f + f * ((float) b.r - (float) a.r) / 255.0f,
                (float) a.g / 255.0f + f * ((float) b.g - (float) a.g) / 255.0f,
                (float) a.b / 255.0f + f * ((float) b.b - (float) a.b) / 255.0f,
                1.0f);
        }
    }
    return juce::Colour (0xfff4efdc);
}
} // namespace

//==============================================================================
SpectrogramComponent::SpectrogramComponent (AppContext& context) : ctx (context)
{
    setOpaque (true);

    floorSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    floorSlider.setRange (-130.0, -40.0, 1.0);
    floorSlider.setValue (floorDb, juce::dontSendNotification);
    floorSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 18);
    floorSlider.textFromValueFunction = [] (double v) { return juce::String ((int) v) + " dB"; };
    floorSlider.setTooltip ("Display floor for the colour map");
    floorSlider.onValueChange = [this]
    {
        floorDb = (float) floorSlider.getValue();
        rebuildColourMap();
        repaint();
    };
    addAndMakeVisible (floorSlider);

    freezeToggle.setClickingTogglesState (true);
    freezeToggle.setTooltip ("Hold the current waterfall image");
    freezeToggle.onClick = [this]
    {
        frozen = freezeToggle.getToggleState();
        repaint();
    };
    addAndMakeVisible (freezeToggle);

    rebuildColourMap();
    setSize (760, 300);
    startTimerHz (30);
}

SpectrogramComponent::~SpectrogramComponent()
{
    stopTimer();
}

void SpectrogramComponent::setFloorDb (float value)
{
    floorSlider.setValue (value, juce::sendNotificationSync);
}

void SpectrogramComponent::setFrozen (bool shouldFreeze)
{
    frozen = shouldFreeze;
    freezeToggle.setToggleState (shouldFreeze, juce::dontSendNotification);
    repaint();
}

void SpectrogramComponent::rebuildColourMap()
{
    for (int i = 0; i < (int) colourMap.size(); ++i)
        colourMap[(size_t) i] = interpolateStops ((float) i / (float) (colourMap.size() - 1));
}

juce::Colour SpectrogramComponent::colourForDb (float db) const
{
    if (db <= floorDb)
        return colourMap[0];
    const float t = juce::jlimit (0.0f, 0.999f, (db - floorDb) / juce::jmax (1.0f, topDb - floorDb));
    return colourMap[(size_t) (t * (float) (colourMap.size() - 1))];
}

//==============================================================================
void SpectrogramComponent::timerCallback()
{
    if (frozen)
        return;

    ctx.engine().analyzers().getSpectrogram (bands, rows, cols);

    const int history = AnalyzerHub::kSpectrogramHistory;
    const int bandsCount = AnalyzerHub::kSpectrogramBands;

    const auto status = ctx.engine().getStatus();
    unavailableDetail = status.errorMessage.isNotEmpty() ? status.errorMessage
                                                         : "The analyser has not produced any spectrogram rows yet.";

    if (rows <= 0 || cols <= 0 || bands.empty())
    {
        hasData = false;
        return;
    }

    rows = juce::jmin (rows, history);
    cols = juce::jmin (cols, bandsCount);

    if (! image.isValid() || image.getWidth() != cols || image.getHeight() != history)
        image = juce::Image (juce::Image::RGB, juce::jmax (1, cols), history, true);

    bool anyContent = false;
    for (int row = 0; row < rows; ++row)
    {
        // Oldest row at the top of the filled block, newest at the bottom (the
        // waterfall scrolls upward as new rows arrive).
        const int y = history - rows + row;
        const float* source = bands.data() + (size_t) row * (size_t) cols;
        for (int col = 0; col < cols; ++col)
        {
            const auto colour = colourForDb (source[col]);
            image.setPixelAt (col, y, colour);
            if (! anyContent && source[col] > floorDb + 4.0f)
                anyContent = true;
        }
    }

    hasData = anyContent || rows > 1;
    repaint();
}

//==============================================================================
void SpectrogramComponent::resized()
{
    auto r = getLocalBounds().reduced (8, 6);
    headerArea = r.removeFromTop (20);
    r.removeFromTop (4);

    const int width = getWidth();
    const bool showControls = width >= 420;

    if (showControls)
    {
        controlArea = r.removeFromTop (26);
        r.removeFromTop (4);
    }
    else
    {
        controlArea = {};
    }
    graphArea = r;
    floorSlider.setVisible (showControls);
    freezeToggle.setVisible (showControls);

    int x = controlArea.getRight();
    auto place = [&] (juce::Component& c, int w)
    {
        x -= w;
        c.setBounds (x, controlArea.getY(), w, controlArea.getHeight());
        x -= 8;
    };
    if (showControls)
    {
        place (freezeToggle, 92);
        place (floorSlider, 190);
    }
}

//==============================================================================
void SpectrogramComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);

    {
        auto title = headerArea;
        theme::drawSectionHeader (g, title, "SPECTROGRAM", {});
        g.setFont (theme::monoFont (9.0f));
        g.setColour (theme::textFaint);
        g.drawText ("96 LOG BANDS  |  OLDEST TOP / NEWEST BOTTOM", title,
                    juce::Justification::centredRight, false);
    }

    paintGraph (g, graphArea);
}

void SpectrogramComponent::paintGraph (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (area.isEmpty())
        return;

    theme::fillSunken (g, area);
    auto plot = area.reduced (2);

    if (! hasData || ! image.isValid())
    {
        drawEmptyState (g, plot, "NO SPECTROGRAM DATA", unavailableDetail, theme::textDim);
        return;
    }

    g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
    g.drawImage (image, plot.toFloat(), juce::RectanglePlacement::stretchToFit);

    // Frequency grid over the waterfall.
    for (double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        const double t = std::log (f / 20.0) / std::log (20000.0 / 20.0);
        const float x = (float) plot.getX() + (float) t * (float) plot.getWidth();
        g.setColour (juce::Colours::black.withAlpha (0.22f));
        g.drawVerticalLine (juce::roundToInt (x), (float) plot.getY(), (float) plot.getBottom());

        g.setColour (theme::silver.withAlpha (0.55f));
        g.setFont (theme::monoFont (7.5f));
        const juce::String label = f >= 1000.0 ? juce::String (f / 1000.0, 0) + "k" : juce::String ((int) f);
        g.drawText (label, juce::Rectangle<float> (x - 13.0f, (float) plot.getBottom() - 10.0f, 26.0f, 10.0f),
                    juce::Justification::centredTop, false);
    }

    // Colour-map scale on the right edge.
    auto legend = plot.removeFromRight (8).reduced (2, 0);
    if (legend.getWidth() >= 4)
    {
        for (int y = 0; y < legend.getHeight(); ++y)
        {
            const float t = 1.0f - (float) y / (float) juce::jmax (1, legend.getHeight());
            g.setColour (interpolateStops (t));
            g.fillRect (legend.getX(), legend.getY() + y, legend.getWidth(), 1);
        }
        g.setColour (theme::lineStrong);
        g.drawRect (legend, 1);
    }

    g.setColour (theme::line);
    g.drawRect (plot.toFloat(), 1.0f);

    if (frozen)
    {
        g.setColour (theme::gold);
        g.setFont (theme::capsFont (8.5f, 0.12f));
        g.drawText ("FROZEN", plot.reduced (5).removeFromTop (12), juce::Justification::centredLeft, false);
    }
}

void SpectrogramComponent::mouseDown (const juce::MouseEvent&)
{
    // Clicking the waterfall freezes/continues it, matching the toggle.
    setFrozen (! frozen);
}

} // namespace sp
