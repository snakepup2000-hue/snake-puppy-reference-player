/*
    Snake Puppy Reference Player - real-time FFT spectrum.
    Message thread only.
*/
#include "UI/SpectrumComponent.h"

#include <cmath>
#include <type_traits>

namespace sp
{

namespace
{
using ui::drawEmptyState;

constexpr double kMinFreq = 20.0;
constexpr double kMaxFreq = 20000.0;

template <typename T, typename = void>
struct HasPerChannelSpectrum : std::false_type {};

template <typename T>
struct HasPerChannelSpectrum<T, std::void_t<decltype (
    std::declval<const T&>().getSpectrumDbPerChannel (0,
                                                      std::declval<std::vector<float>&>(),
                                                      std::declval<std::vector<float>&>()))>> : std::true_type {};

constexpr bool kHasPerChannel = HasPerChannelSpectrum<AnalyzerHub>::value;

/** Only instantiated when the analyser actually exposes per-channel spectra
    (the DSP engineer may extend AnalyzerHub later without breaking this UI). */
template <typename Hub>
void fetchPerChannelSpectra (const Hub& hub, std::vector<float>& left,
                             std::vector<float>& right, std::vector<float>& freqsOut)
{
    if constexpr (HasPerChannelSpectrum<Hub>::value)
    {
        hub.getSpectrumDbPerChannel (0, left, freqsOut);
        hub.getSpectrumDbPerChannel (1, right, freqsOut);
    }
    else
    {
        juce::ignoreUnused (hub, left, right, freqsOut);
    }
}

juce::String freqAxisLabel (double hz)
{
    if (hz >= 1000.0)
    {
        const double k = hz / 1000.0;
        return (std::abs (k - std::round (k)) < 0.05 ? juce::String ((int) std::round (k))
                                                     : juce::String (k, 1)) + "k";
    }
    return juce::String ((int) hz);
}
} // namespace

//==============================================================================
SpectrumComponent::SpectrumComponent (AppContext& context) : ctx (context)
{
    setOpaque (true);
    perChannelSupported = kHasPerChannel;

    addAndMakeVisible (fftBox);
    static const int sizes[] { 1024, 2048, 4096, 8192, 16384 };
    for (int i = 0; i < 5; ++i)
        fftBox.addItem (juce::String (sizes[i]), i + 1);
    {
        const int current = ctx.engine().analyzers().getFftSize();
        int selected = 3;
        for (int i = 0; i < 5; ++i)
            if (sizes[i] == current)
                selected = i + 1;
        fftBox.setSelectedId (selected, juce::dontSendNotification);
    }
    fftBox.setTooltip ("FFT window size");
    fftBox.onChange = [this]
    {
        static const int kFftSizes[] { 1024, 2048, 4096, 8192, 16384 };
        const int index = juce::jlimit (0, 4, fftBox.getSelectedId() - 1);
        ctx.engine().analyzers().setFftSize (kFftSizes[index]);
        peakDb.clear();
        repaint();
    };

    addAndMakeVisible (rateBox);
    static const int rates[] { 20, 30, 60 };
    for (int i = 0; i < 3; ++i)
        rateBox.addItem (juce::String (rates[i]) + " Hz", i + 1);
    {
        const int current = ctx.engine().analyzers().getDisplayUpdateRateHz();
        int selected = 2;
        for (int i = 0; i < 3; ++i)
            if (rates[i] == current)
                selected = i + 1;
        rateBox.setSelectedId (selected, juce::dontSendNotification);
    }
    rateBox.setTooltip ("Display refresh rate");
    rateBox.onChange = [this]
    {
        static const int kRates[] { 20, 30, 60 };
        const int index = juce::jlimit (0, 2, rateBox.getSelectedId() - 1);
        ctx.engine().analyzers().setDisplayUpdateRateHz (kRates[index]);
        applyTimerRate();
    };

    peakHoldToggle.setClickingTogglesState (true);
    peakHoldToggle.setToggleState (true, juce::dontSendNotification);
    peakHoldToggle.onClick = [this] { peakHoldOn = peakHoldToggle.getToggleState(); repaint(); };
    addAndMakeVisible (peakHoldToggle);

    stereoToggle.setClickingTogglesState (true);
    stereoToggle.setToggleState (false, juce::dontSendNotification);
    if (perChannelSupported)
    {
        stereoToggle.setTooltip ("Show left and right spectra separately");
        stereoToggle.onClick = [this] { stereoOn = stereoToggle.getToggleState(); repaint(); };
    }
    else
    {
        stereoToggle.setEnabled (false);
        stereoToggle.setTooltip ("Unavailable: AnalyzerHub exposes a combined spectrum only");
    }
    addAndMakeVisible (stereoToggle);

    setSize (760, 340);
    applyTimerRate();
}

SpectrumComponent::~SpectrumComponent()
{
    stopTimer();
}

void SpectrumComponent::setPeakHoldEnabled (bool enabled)
{
    peakHoldOn = enabled;
    peakHoldToggle.setToggleState (enabled, juce::dontSendNotification);
    repaint();
}

void SpectrumComponent::setDisplayRange (float floor, float top)
{
    floorDb = juce::jmin (floor, top - 10.0f);
    topDb = juce::jmax (top, floorDb + 10.0f);
    repaint();
}

void SpectrumComponent::setStereoOverlay (bool enabled)
{
    if (! perChannelSupported)
        return;
    stereoOn = enabled;
    stereoToggle.setToggleState (enabled, juce::dontSendNotification);
    repaint();
}

void SpectrumComponent::applyTimerRate()
{
    stopTimer();
    const int rate = juce::jlimit (10, 60, ctx.engine().analyzers().getDisplayUpdateRateHz());
    startTimerHz (rate);
    lastUpdateMs = 0.0;
}

//==============================================================================
void SpectrumComponent::timerCallback()
{
    if (frozen)
        return;

    const auto now = (double) juce::Time::currentTimeMillis();
    ctx.engine().analyzers().getSpectrumDb (db, freqs);

    if (perChannelSupported && stereoOn)
    {
        fetchPerChannelSpectra (ctx.engine().analyzers(), dbLeft, dbRight, freqs);
    }
    else
    {
        dbLeft.clear();
        dbRight.clear();
    }

    // Peak hold with time-based decay.
    const double dt = lastUpdateMs > 0.0 ? juce::jlimit (0.0, 0.5, (now - lastUpdateMs) / 1000.0) : 0.0;
    lastUpdateMs = now;

    if (db.empty())
    {
        hasData = false;
    }
    else
    {
        if (peakDb.size() != db.size())
            peakDb.assign (db.size(), -144.0f);

        const float decay = (float) (24.0 * dt);
        bool anyAbove = false;
        for (size_t i = 0; i < db.size(); ++i)
        {
            if (peakDb[i] > -140.0f)
                peakDb[i] = juce::jmax (db[i], peakDb[i] - decay);
            else
                peakDb[i] = db[i];
            if (db[i] > -110.0f)
                anyAbove = true;
        }
        hasData = anyAbove;
    }

    const auto status = ctx.engine().getStatus();
    if (status.errorMessage.isNotEmpty())
        unavailableDetail = status.errorMessage;
    else
        unavailableDetail = "The analyser has not received audio frames yet.";

    repaint();
}

//==============================================================================
void SpectrumComponent::resized()
{
    auto r = getLocalBounds().reduced (8, 6);
    headerArea = r.removeFromTop (20);
    r.removeFromTop (4);

    // Responsive controls: hide what does not fit instead of overlapping, and
    // only reserve vertical space when the strip is actually shown (rail).
    const int width = getWidth();
    const bool showAll = width >= 560;
    const bool showSome = width >= 380;

    if (showSome)
    {
        controlsArea = r.removeFromTop (26);
        r.removeFromTop (4);
    }
    else
    {
        controlsArea = {};
    }

    graphArea = r;

    stereoToggle.setVisible (showAll && perChannelSupported);
    peakHoldToggle.setVisible (showAll);
    rateBox.setVisible (showSome);
    fftBox.setVisible (showSome);

    int x = controlsArea.getRight();
    auto place = [&] (juce::Component& c, int w)
    {
        x -= w;
        c.setBounds (x, controlsArea.getY(), w, controlsArea.getHeight());
        x -= 6;
    };
    if (stereoToggle.isVisible())
        place (stereoToggle, 64);
    if (peakHoldToggle.isVisible())
        place (peakHoldToggle, 94);
    if (rateBox.isVisible())
        place (rateBox, 76);
    if (fftBox.isVisible())
        place (fftBox, 88);

    if (graphArea.getWidth() <= 0 || graphArea.getHeight() <= 0)
        graphArea = {};
}

//==============================================================================
void SpectrumComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);

    {
        auto title = headerArea;
        theme::drawSectionHeader (g, title, "SPECTRUM", {});

        juce::String right;
        right << ctx.engine().analyzers().getFftSize() << "-POINT FFT   |   "
              << ctx.engine().analyzers().getDisplayUpdateRateHz() << " Hz";
        g.setFont (theme::monoFont (9.0f));
        g.setColour (theme::textFaint);
        g.drawText (right, title, juce::Justification::centredRight, false);
    }

    paintGraph (g, graphArea);
}

void SpectrumComponent::paintGraph (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (area.isEmpty())
        return;

    theme::fillSunken (g, area);

    auto plot = area.reduced (2);
    plot.removeFromLeft (32);
    plot.removeFromBottom (14);

    // dB grid.
    const float dbStep = 12.0f;
    for (float dbValue = topDb; dbValue >= floorDb - 0.01f; dbValue -= dbStep)
    {
        const float t = (dbValue - floorDb) / juce::jmax (1.0f, topDb - floorDb);
        const float y = (float) plot.getBottom() - t * (float) plot.getHeight();
        g.setColour (std::abs (dbValue) < 0.01f ? theme::lineStrong : theme::lineFaint);
        g.drawHorizontalLine (juce::roundToInt (y), (float) plot.getX(), (float) plot.getRight());
        g.setColour (theme::textFaint);
        g.setFont (theme::monoFont (8.0f));
        g.drawText (juce::String ((int) dbValue),
                    juce::Rectangle<float> ((float) area.getX(), y - 5.0f, 28.0f, 10.0f),
                    juce::Justification::centredRight, false);
    }

    // Frequency grid.
    auto freqToX = [&] (double hz)
    {
        const double t = std::log (juce::jlimit (kMinFreq, kMaxFreq, hz) / kMinFreq)
                         / std::log (kMaxFreq / kMinFreq);
        return (float) plot.getX() + (float) t * (float) plot.getWidth();
    };

    for (double f : { 20.0, 30.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        const float x = freqToX (f);
        g.setColour (theme::lineFaint.withAlpha (0.9f));
        g.drawVerticalLine (juce::roundToInt (x), (float) plot.getY(), (float) plot.getBottom());
        g.setColour (theme::textFaint);
        g.setFont (theme::monoFont (8.0f));
        auto label = juce::Rectangle<float> (x - 14.0f, (float) area.getBottom() - 13.0f, 28.0f, 11.0f);
        const auto limits = area.toFloat().reduced (1.0f);
        label.setX (juce::jlimit (limits.getX(),
                                  juce::jmax (limits.getX(), limits.getRight() - label.getWidth()),
                                  label.getX()));
        g.drawText (freqAxisLabel (f), label, juce::Justification::centredTop, false);
    }

    if (! hasData || db.empty() || freqs.empty())
    {
        paintEmpty (g, area);
        return;
    }

    const bool drawStereo = stereoOn && dbLeft.size() == freqs.size() && dbRight.size() == freqs.size();

    auto buildPath = [&] (const std::vector<float>& data)
    {
        juce::Path p;
        for (size_t i = 0; i < data.size() && i < freqs.size(); ++i)
        {
            const float x = freqToX (freqs[i]);
            const float t = (juce::jlimit (floorDb, topDb, data[i]) - floorDb)
                            / juce::jmax (1.0f, topDb - floorDb);
            const float y = (float) plot.getBottom() - t * (float) plot.getHeight();
            if (i == 0)
                p.startNewSubPath (x, y);
            else
                p.lineTo (x, y);
        }
        return p;
    };

    auto fillPath = [&] (const juce::Path& p, juce::Colour colour)
    {
        juce::Path filled (p);
        filled.lineTo ((float) plot.getRight(), (float) plot.getBottom());
        filled.lineTo ((float) plot.getX(), (float) plot.getBottom());
        filled.closeSubPath();
        g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.30f), 0.0f, (float) plot.getY(),
                                                 colour.withAlpha (0.015f), 0.0f, (float) plot.getBottom(),
                                                 false));
        g.fillPath (filled);
    };

    if (drawStereo)
    {
        auto leftPath = buildPath (dbLeft);
        auto rightPath = buildPath (dbRight);
        fillPath (leftPath, theme::info);
        fillPath (rightPath, theme::gold);

        g.setColour (theme::info.withAlpha (0.95f));
        g.strokePath (leftPath, juce::PathStrokeType (1.3f));
        g.setColour (theme::gold.withAlpha (0.95f));
        g.strokePath (rightPath, juce::PathStrokeType (1.3f));

        // Legend.
        g.setFont (theme::monoFont (8.5f));
        g.setColour (theme::info);
        g.drawText ("L", plot.withTrimmedRight (plot.getWidth() - 14).withTrimmedTop (2),
                    juce::Justification::centred, false);
        g.setColour (theme::gold);
        g.drawText ("R", plot.withTrimmedRight (plot.getWidth() - 30).withTrimmedTop (2),
                    juce::Justification::centred, false);
    }
    else
    {
        auto dbPath = buildPath (db);
        fillPath (dbPath, theme::gold);
        g.setColour (theme::gold.withAlpha (0.12f));
        g.strokePath (dbPath, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
        g.setColour (theme::gold.withAlpha (0.95f));
        g.strokePath (dbPath, juce::PathStrokeType (1.5f));
    }

    if (peakHoldOn && peakDb.size() == freqs.size())
    {
        auto peakPath = buildPath (peakDb);
        g.setColour (theme::silver.withAlpha (0.55f));
        g.strokePath (peakPath, juce::PathStrokeType (1.0f));
    }

    g.setColour (theme::line);
    g.drawRect (plot.toFloat(), 1.0f);
}

void SpectrumComponent::paintEmpty (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto plot = area.reduced (2);
    plot.removeFromLeft (32);
    plot.removeFromBottom (14);
    g.setColour (theme::background.withAlpha (0.55f));
    g.fillRect (plot);
    drawEmptyState (g, plot, "NO SPECTRUM DATA", unavailableDetail, theme::textDim);
}

} // namespace sp
