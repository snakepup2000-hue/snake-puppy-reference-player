/*
    Snake Puppy Reference Player - metering panel (compact + expanded).
    Message thread only, 30 Hz polling of real AnalyzerHub measurements.
*/
#include "MetersPanel.h"

#include <cmath>
#include <cstring>

namespace sp
{

namespace
{
using ui::dbText;
using ui::drawEmptyState;

constexpr float kMeterFloorDb = -72.0f;
constexpr float kMeterTopDb = 0.0f;
constexpr juce::int64 kHoldMs = 1500;
constexpr float kHoldDecayDbPerSecond = 18.0f;

juce::String compactDb (float db)
{
    return db <= -100.0f ? juce::String::fromUTF8 ("-inf") : dbText (db, 1);
}
} // namespace

//==============================================================================
MetersPanel::MetersPanel (AppContext& context, Mode initialMode)
    : ctx (context), mode (initialMode)
{
    setOpaque (true);
    setSize (mode == Mode::compact ? 215 : 720, mode == Mode::compact ? 44 : 470);
    values = ctx.engine().analyzers().getMeterValues();
    startTimerHz (30);
}

MetersPanel::~MetersPanel()
{
    stopTimer();
}

void MetersPanel::setMode (Mode newMode)
{
    if (mode == newMode)
        return;
    mode = newMode;
    resized();
    repaint();
}

void MetersPanel::setShowInputComparison (bool shouldShow)
{
    showInput = shouldShow;
    resized();
    repaint();
}

void MetersPanel::resetPeakHolds()
{
    for (auto& h : holds)
        h = {};
    clipLatched = false;
    lastClipCount = values.clipCount;
    repaint();
}

//==============================================================================
void MetersPanel::timerCallback()
{
    const auto newValues = ctx.engine().analyzers().getMeterValues();
    const auto status = ctx.engine().getStatus();
    const auto now = juce::Time::currentTimeMillis();

    // Derive dt from the wall clock for hold decay.
    static thread_local juce::int64 lastTick = now;
    const juce::int64 dtMs = juce::jlimit (juce::int64 (1), juce::int64 (250), now - lastTick);
    lastTick = now;

    // Clip latch from real clip flags/counters.
    if (newValues.clipping || newValues.clipCount > lastClipCount)
    {
        clipLatched = true;
        clipLatchUntilMs = now + kHoldMs;
    }
    lastClipCount = newValues.clipCount;
    lastUnderrunCount = newValues.underrunCount;
    if (clipLatched && now > clipLatchUntilMs)
        clipLatched = false;

    for (int ch = 0; ch < 2; ++ch)
    {
        auto& h = holds[ch];
        const float peak = newValues.peakDb[ch];
        const float truePeak = newValues.truePeakDb[ch];

        if (peak >= h.peakDb || h.peakDb <= -140.0f)
        {
            h.peakDb = peak;
            h.holdUntilMs = now + kHoldMs;
        }
        else if (now > h.holdUntilMs)
        {
            h.peakDb = juce::jmax (peak, h.peakDb - kHoldDecayDbPerSecond * (float) dtMs / 1000.0f);
        }

        if (truePeak >= h.truePeakDb || h.truePeakDb <= -140.0f)
            h.truePeakDb = truePeak;
        else
            h.truePeakDb = juce::jmax (truePeak, h.truePeakDb - kHoldDecayDbPerSecond * (float) dtMs / 1000.0f);
    }

    const bool changed = std::memcmp (&newValues.peakDb, &values.peakDb, sizeof (values.peakDb)) != 0
                         || std::memcmp (&newValues.rmsDb, &values.rmsDb, sizeof (values.rmsDb)) != 0
                         || std::memcmp (&newValues.truePeakDb, &values.truePeakDb, sizeof (values.truePeakDb)) != 0
                         || std::memcmp (&newValues.momentaryLufs, &values.momentaryLufs, sizeof (float)) != 0
                         || std::memcmp (&newValues.shortTermLufs, &values.shortTermLufs, sizeof (float)) != 0
                         || std::memcmp (&newValues.integratedLufs, &values.integratedLufs, sizeof (float)) != 0
                         || std::abs (newValues.correlation - values.correlation) > 1.0e-6f
                         || newValues.clipping != values.clipping
                         || newValues.analysedFrames != values.analysedFrames
                         || newValues.clipCount != values.clipCount
                         || status.errorMessage != engineError
                         || status.deviceOpen != engineOnline;

    values = newValues;
    engineError = status.errorMessage;
    engineOnline = status.deviceOpen;

    if (changed || clipLatched || timeSinceLastRepaintMs() > 60)
        repaint();
}

juce::int64 MetersPanel::timeSinceLastRepaintMs()
{
    const auto now = juce::Time::currentTimeMillis();
    const auto delta = now - lastRepaintRequestMs;
    lastRepaintRequestMs = now;
    return delta;
}

//==============================================================================
void MetersPanel::resized()
{
    if (mode == Mode::compact)
        return;

    auto r = getLocalBounds().reduced (10, 8);
    headerArea = r.removeFromTop (20);
    r.removeFromTop (6);

    peakArea = r.removeFromTop (juce::jmin (96, juce::jmax (56, r.getHeight() / 3)));
    const int channelHeight = juce::jmax (20, (peakArea.getHeight() - 4) / 2);
    channelRows[0] = peakArea.removeFromTop (channelHeight);
    peakArea.removeFromTop (2);
    channelRows[1] = peakArea.removeFromTop (channelHeight);
    r.removeFromTop (8);

    lufsArea = r.removeFromTop (juce::jmin (58, r.getHeight() / 3));
    r.removeFromTop (8);

    statsArea = r.removeFromTop (juce::jmin (50, r.getHeight() / 3));
    r.removeFromTop (8);

    if (showInput)
    {
        ioArea = r.removeFromTop (juce::jmin (62, r.getHeight() / 2));
        r.removeFromTop (8);
    }
    else
    {
        ioArea = {};
    }

    flagsArea = r.removeFromBottom (18);
    r.removeFromBottom (6);
    bassArea = r.removeFromTop (juce::jmax (52, juce::jmin (150, r.getHeight())));
}

//==============================================================================
float MetersPanel::dbToX (float db, juce::Rectangle<int> bar)
{
    const float t = juce::jlimit (0.0f, 1.0f, (db - kMeterFloorDb) / (kMeterTopDb - kMeterFloorDb));
    return (float) bar.getX() + t * (float) bar.getWidth();
}

juce::String MetersPanel::lufsText (float v)
{
    return v <= -100.0f ? juce::String::fromUTF8 ("-inf") : juce::String (v, 1);
}

void MetersPanel::drawMeterBar (juce::Graphics& g, juce::Rectangle<int> bar,
                                float peakDb, float holdDb, float rmsDb, bool clipped)
{
    theme::fillSunken (g, bar);
    auto inner = bar.reduced (1);

    // Scale ticks.
    for (float db : { -60.0f, -48.0f, -36.0f, -24.0f, -18.0f, -12.0f, -6.0f, -3.0f })
    {
        const float x = dbToX (db, inner);
        g.setColour (theme::lineFaint.withAlpha (0.75f));
        g.drawVerticalLine (juce::roundToInt (x), (float) inner.getY() + 1.0f, (float) inner.getBottom());
    }

    if (peakDb > kMeterFloorDb)
    {
        const float x0 = (float) inner.getX();
        const float x1 = dbToX (peakDb, inner);
        juce::Rectangle<float> fill (x0, (float) inner.getY(), juce::jmax (1.0f, x1 - x0),
                                     (float) inner.getHeight());

        juce::ColourGradient grad (theme::silver.withAlpha (0.55f), x0, 0.0f,
                                   theme::gold, dbToX (-12.0f, inner), 0.0f, false);
        grad.addColour (1.0, theme::error);
        g.setGradientFill (grad);
        g.fillRect (fill);
    }

    if (rmsDb > kMeterFloorDb)
    {
        const float x = dbToX (rmsDb, inner);
        g.setColour (theme::text.withAlpha (0.85f));
        g.drawVerticalLine (juce::roundToInt (x), (float) inner.getY(), (float) inner.getBottom() - 1.0f);
    }

    if (holdDb > kMeterFloorDb)
    {
        const float x = dbToX (holdDb, inner);
        g.setColour (theme::goldBright);
        g.drawVerticalLine (juce::roundToInt (x), (float) bar.getY(), (float) bar.getBottom());
    }

    if (clipped)
    {
        g.setColour (theme::error);
        g.fillRect (juce::Rectangle<float> ((float) bar.getRight() - 5.0f, (float) bar.getY() + 1.0f,
                                            4.0f, (float) bar.getHeight() - 2.0f));
    }
}

//==============================================================================
void MetersPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);

    if (mode == Mode::compact)
    {
        paintCompact (g, getLocalBounds());
        return;
    }

    paintHeader (g, headerArea);
    for (int ch = 0; ch < 2; ++ch)
        paintChannelRow (g, channelRows[ch], ch == 0 ? "L" : "R", ch, true);
    paintLufs (g, lufsArea);
    paintStats (g, statsArea);
    if (showInput && ! ioArea.isEmpty())
        paintInputOutput (g, ioArea);
    paintBassEnergy (g, bassArea);

    // Footer flags.
    {
        const auto now = juce::Time::currentTimeMillis();
        const bool clipped = clipLatched || values.clipping;
        const auto flagColour = clipped ? theme::error : theme::textFaint;

        g.setFont (theme::monoFont (9.5f));
        g.setColour (flagColour);
        juce::String flags;
        flags << "CLIPS " << (long long) values.clipCount
              << "   |   UNDERRUNS " << (long long) values.underrunCount;

        g.drawText (flags, flagsArea, juce::Justification::centredLeft, false);

        g.setColour (theme::textFaint);
        g.drawText ("PEAK HOLD 1.5 s  |  DOUBLE-CLICK TO RESET",
                    flagsArea, juce::Justification::centredRight, false);
        juce::ignoreUnused (now);
    }

    // Honest unavailability overlay.
    if (! isDataAvailable())
    {
        auto area = getLocalBounds().reduced (10);
        area.removeFromTop (28);
        g.setColour (theme::background.withAlpha (0.72f));
        g.fillRect (area);
        juce::String detail;
        if (engineError.isNotEmpty())
            detail = engineError;
        else if (! engineOnline)
            detail = "The audio engine has not opened an output device yet.";
        else
            detail = "The analyser has not received audio frames yet.";

        drawEmptyState (g, area, "NO METERING DATA", detail, theme::textDim);
    }
}

bool MetersPanel::isDataAvailable() const
{
    // A prepared sample rate alone is not data: require frames actually analysed
    // or a measured (non-floor) level. Honest no-data beats decorative meters.
    if (values.analysedFrames > 0)
        return true;
    for (int ch = 0; ch < 2; ++ch)
        if (values.peakDb[ch] > -140.0f || values.rmsDb[ch] > -140.0f
            || values.truePeakDb[ch] > -140.0f)
            return true;
    return false;
}

void MetersPanel::paintHeader (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (area.isEmpty())
        return;

    theme::drawSectionHeader (g, area, "OUTPUT METERS", {});

    const auto& hub = ctx.engine().analyzers();
    juce::String right;
    right << "FFT " << hub.getFftSize() << "   |   " << hub.getDisplayUpdateRateHz() << " Hz";

    g.setFont (theme::monoFont (9.5f));
    g.setColour (engineOnline ? theme::silverDim : theme::warning);
    g.drawText (right, area, juce::Justification::centredRight, false);
}

void MetersPanel::paintChannelRow (juce::Graphics& g, juce::Rectangle<int> area,
                                   const juce::String& label, int channel, bool showTruePeak)
{
    if (area.isEmpty())
        return;

    auto r = area;
    g.setColour (theme::silver);
    g.setFont (theme::monoFont (12.0f, true));
    g.drawText (label, r.removeFromLeft (14), juce::Justification::centred, false);

    const int numbersWidth = juce::jlimit (110, 210, r.getWidth() / 2);
    auto numbers = r.removeFromRight (numbersWidth);
    auto bar = r.reduced (2, 6);

    drawMeterBar (g, bar,
                  values.peakDb[channel],
                  holds[channel].peakDb,
                  values.rmsDb[channel],
                  clipLatched || values.clipping);

    g.setFont (theme::monoFont (9.5f));
    g.setColour (theme::textDim);
    juce::String text;
    text << "P " << compactDb (values.peakDb[channel]);
    if (showTruePeak)
        text << "   TP " << compactDb (values.truePeakDb[channel]);
    text << "   RMS " << compactDb (values.rmsDb[channel]);
    g.drawText (text, numbers.withTrimmedLeft (8), juce::Justification::centredLeft, false);

    if (clipLatched || values.clipping)
    {
        g.setColour (theme::error);
        g.setFont (theme::capsFont (8.5f, 0.10f));
        g.drawText ("CLIP", numbers, juce::Justification::centredRight, false);
    }
}

void MetersPanel::paintLufs (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (area.isEmpty())
        return;

    struct Entry { const char* label; float value; juce::Colour colour; };
    const Entry entries[3] {
        { "MOMENTARY", values.momentaryLufs, theme::text },
        { "SHORT-TERM", values.shortTermLufs, theme::silver },
        { "INTEGRATED", values.integratedLufs, theme::goldBright }
    };

    auto r = area;
    const int bw = (r.getWidth() - 16) / 3;
    for (int i = 0; i < 3; ++i)
    {
        auto box = r.removeFromLeft (bw);
        if (i < 2)
            r.removeFromLeft (8);
        theme::fillSunken (g, box);

        auto inner = box.reduced (8, 4);
        g.setColour (theme::textDim);
        g.setFont (theme::capsFont (8.5f, 0.14f));
        g.drawText (entries[i].label, inner.removeFromTop (10), juce::Justification::centredLeft, false);

        g.setColour (entries[i].colour);
        g.setFont (theme::monoFont (juce::jlimit (15.0f, 22.0f, (float) inner.getHeight() * 0.72f), true));
        auto valueRow = inner.removeFromTop (juce::jmax (14, inner.getHeight() - 6));
        g.drawText (lufsText (entries[i].value), valueRow, juce::Justification::centredLeft, false);
        g.setFont (theme::monoFont (8.5f));
        g.setColour (theme::textFaint);
        g.drawText ("LUFS", valueRow, juce::Justification::centredRight, false);

        // Small scale -40..0 LUFS.
        auto bar = inner.reduced (0, 1);
        g.setColour (theme::panelSunken);
        g.fillRect (bar);
        const float t = juce::jlimit (0.0f, 1.0f, (entries[i].value + 40.0f) / 40.0f);
        g.setColour (entries[i].colour.withAlpha (0.6f));
        g.fillRect (bar.withWidth (juce::roundToInt ((float) bar.getWidth() * t)));
    }
}

void MetersPanel::paintStats (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (area.isEmpty())
        return;

    auto r = area;
    const int third = (r.getWidth() - 16) / 3;

    // Correlation: bar from -1 (left) through 0 to +1 (right).
    {
        auto box = r.removeFromLeft (third);
        r.removeFromLeft (8);
        theme::fillSunken (g, box);
        auto inner = box.reduced (8, 4);
        g.setColour (theme::textDim);
        g.setFont (theme::capsFont (8.5f, 0.14f));
        g.drawText ("STEREO CORRELATION", inner.removeFromTop (10), juce::Justification::centredLeft, false);

        auto bar = inner.removeFromTop (juce::jmax (6, inner.getHeight() - 10)).reduced (0, 2);
        g.setColour (theme::panelSunken);
        g.fillRect (bar);
        const float centre = (float) bar.getCentreX();
        g.setColour (theme::line);
        g.drawVerticalLine (juce::roundToInt (centre), (float) bar.getY(), (float) bar.getBottom());

        const float v = juce::jlimit (-1.0f, 1.0f, values.correlation);
        const float x = centre + v * (float) bar.getWidth() * 0.5f;
        g.setColour (v < 0.0f ? theme::warning : theme::success);
        g.fillRect (juce::Rectangle<float> (juce::jmin (centre, x), (float) bar.getY(),
                                            (float) std::abs ((double) x - (double) centre), (float) bar.getHeight()));
        g.setColour (theme::text);
        g.fillRect (juce::Rectangle<float> (x - 1.0f, (float) bar.getY() - 1.0f, 2.0f, (float) bar.getHeight() + 2.0f));

        g.setColour (theme::textFaint);
        g.setFont (theme::monoFont (8.5f));
        g.drawText ("-1", bar.getX(), bar.getY() - 10, 12, 10, juce::Justification::centredLeft);
        g.drawText ("+1", bar.getRight() - 12, bar.getY() - 10, 12, 10, juce::Justification::centredRight);
        g.setColour (theme::silver);
        g.setFont (theme::monoFont (10.5f, true));
        g.drawText (juce::String (v, 2), inner, juce::Justification::centredRight, false);
    }

    // Crest factor.
    {
        auto box = r.removeFromLeft (third);
        r.removeFromLeft (8);
        theme::fillSunken (g, box);
        auto inner = box.reduced (8, 4);
        g.setColour (theme::textDim);
        g.setFont (theme::capsFont (8.5f, 0.14f));
        g.drawText ("CREST FACTOR", inner.removeFromTop (10), juce::Justification::centredLeft, false);
        g.setColour (theme::text);
        g.setFont (theme::monoFont (16.0f, true));
        g.drawText (juce::String (values.crestFactorDb, 1) + " dB", inner,
                    juce::Justification::centredLeft, false);
    }

    // Loudness range.
    {
        auto box = r.removeFromLeft (r.getWidth());
        theme::fillSunken (g, box);
        auto inner = box.reduced (8, 4);
        g.setColour (theme::textDim);
        g.setFont (theme::capsFont (8.5f, 0.14f));
        g.drawText ("LOUDNESS RANGE", inner.removeFromTop (10), juce::Justification::centredLeft, false);
        g.setColour (theme::text);
        g.setFont (theme::monoFont (16.0f, true));
        g.drawText (juce::String (values.loudnessRangeLu, 1) + " LU", inner,
                    juce::Justification::centredLeft, false);
    }
}

void MetersPanel::paintInputOutput (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (area.isEmpty())
        return;

    theme::fillSunken (g, area);
    auto inner = area.reduced (8, 4);

    g.setColour (theme::textDim);
    g.setFont (theme::capsFont (8.5f, 0.14f));
    g.drawText ("INPUT (PRE-DSP) vs OUTPUT (POST-DSP)", inner.removeFromTop (10),
                juce::Justification::centredLeft, false);

    for (int row = 0; row < 2; ++row)
    {
        const bool input = row == 0;
        auto line = inner.removeFromTop ((inner.getHeight() - 2) / 2);
        inner.removeFromTop (2);

        auto label = line.removeFromLeft (44);
        g.setFont (theme::monoFont (9.0f, true));
        g.setColour (input ? theme::info : theme::gold);
        g.drawText (input ? "IN" : "OUT", label, juce::Justification::centredLeft, false);

        const int cellWidth = juce::jmax (40, (line.getWidth() - 10) / 2);
        for (int ch = 0; ch < 2; ++ch)
        {
            auto segment = line.removeFromLeft (cellWidth);
            if (ch == 0)
                line.removeFromLeft (10);

            auto chLabel = segment.removeFromLeft (12);
            g.setColour (theme::textDim);
            g.setFont (theme::monoFont (8.5f));
            g.drawText (ch == 0 ? "L" : "R", chLabel, juce::Justification::centred, false);

            auto numbers = segment.removeFromRight (juce::jmin (84, segment.getWidth() / 2));
            auto bar = segment.reduced (2, 3);

            const float peak = input ? values.inputPeakDb[ch] : values.peakDb[ch];
            const float rms = input ? values.inputRmsDb[ch] : values.rmsDb[ch];

            theme::fillSunken (g, bar);
            if (peak > kMeterFloorDb)
            {
                auto innerBar = bar.reduced (1);
                g.setColour ((input ? theme::info : theme::gold).withAlpha (0.75f));
                g.fillRect (juce::Rectangle<float> ((float) innerBar.getX(), (float) innerBar.getY(),
                                                    dbToX (peak, innerBar) - (float) innerBar.getX(),
                                                    (float) innerBar.getHeight()));
            }
            if (rms > kMeterFloorDb)
            {
                auto innerBar = bar.reduced (1);
                g.setColour (theme::silver);
                g.fillRect (juce::Rectangle<float> (dbToX (rms, innerBar), (float) innerBar.getY(),
                                                    (float) innerBar.getRight() - dbToX (rms, innerBar),
                                                    (float) innerBar.getHeight()));
            }

            g.setColour (theme::textDim);
            g.setFont (theme::monoFont (8.5f));
            g.drawText (compactDb (peak), numbers, juce::Justification::centredRight, false);
        }
    }
}

void MetersPanel::paintBassEnergy (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (area.isEmpty())
        return;

    theme::fillSunken (g, area);
    auto inner = area.reduced (8, 4);
    g.setColour (theme::textDim);
    g.setFont (theme::capsFont (8.5f, 0.14f));
    g.drawText ("BAND ENERGY", inner.removeFromTop (10), juce::Justification::centredLeft, false);

    struct Band { const char* label; float db; };
    const Band bands[3] {
        { "20-60", values.bassEnergyDb[0] },
        { "60-120", values.bassEnergyDb[1] },
        { "120-250", values.bassEnergyDb[2] }
    };

    auto row = inner;
    const int bw = (row.getWidth() - 70) / 3;
    for (int i = 0; i < 3; ++i)
    {
        auto segment = row.removeFromLeft (bw);
        if (i < 2)
            row.removeFromLeft (14);

        g.setColour (theme::textFaint);
        g.setFont (theme::monoFont (8.0f));
        g.drawText (bands[i].label, segment.removeFromTop (10), juce::Justification::centredLeft, false);

        auto bar = segment.reduced (0, 2);
        theme::fillSunken (g, bar);
        if (bands[i].db > kMeterFloorDb)
        {
            auto b = bar.reduced (1);
            g.setColour (theme::gold.withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> ((float) b.getX(), (float) b.getCentreY() - 3.0f,
                                                juce::jmax (1.0f, dbToX (bands[i].db, b) - (float) b.getX()),
                                                6.0f));
        }
    }

    // Low / mid / high wide-band comparison on the right (one value per line so
    // nothing is clipped in the narrow expanded layout).
    auto numbers = row.reduced (4, 2);
    const juce::String labels[3] { "LOW", "MID", "HIGH" };
    const float wide[3] { values.lowEnergyDb, values.midEnergyDb, values.highEnergyDb };
    g.setFont (theme::monoFont (8.0f));
    for (int i = 0; i < 3; ++i)
    {
        auto line = numbers.removeFromTop (juce::jmax (8, numbers.getHeight() / (3 - i)));
        g.setColour (theme::textFaint);
        g.drawText (labels[i], line, juce::Justification::centredLeft, false);
        g.setColour (theme::silverDim);
        g.drawText (compactDb (wide[i]), line, juce::Justification::centredRight, false);
    }
}

//==============================================================================
void MetersPanel::paintCompact (juce::Graphics& g, juce::Rectangle<int> area)
{
    const bool available = isDataAvailable();
    auto r = area.reduced (4, 1);

    const int rowH = juce::jmax (10, (r.getHeight() - 2) / 2);
    for (int ch = 0; ch < 2; ++ch)
    {
        auto row = r.removeFromTop (rowH);
        if (ch == 0)
            r.removeFromTop (2);

        auto label = row.removeFromLeft (11);
        g.setColour (theme::silverDim);
        g.setFont (theme::monoFont (8.5f, true));
        g.drawText (ch == 0 ? "L" : "R", label, juce::Justification::centred, false);

        const int numbersWidth = juce::jmin (44, row.getWidth() / 3);
        auto numbers = row.removeFromRight (numbersWidth);
        auto bar = row.reduced (1, 2);

        theme::fillSunken (g, bar);
        if (available && values.peakDb[ch] > kMeterFloorDb)
        {
            auto inner = bar.reduced (1);
            const float x1 = dbToX (values.peakDb[ch], inner);
            juce::ColourGradient grad (theme::silver.withAlpha (0.55f), (float) inner.getX(), 0.0f,
                                       theme::gold, dbToX (-12.0f, inner), 0.0f, false);
            grad.addColour (1.0, theme::error);
            g.setGradientFill (grad);
            g.fillRect (juce::Rectangle<float> ((float) inner.getX(), (float) inner.getY(),
                                                juce::jmax (1.0f, x1 - (float) inner.getX()),
                                                (float) inner.getHeight()));
        }
        if (available && holds[ch].peakDb > kMeterFloorDb)
        {
            auto inner = bar.reduced (1);
            g.setColour (theme::goldBright);
            g.drawVerticalLine (juce::roundToInt (dbToX (holds[ch].peakDb, inner)),
                                (float) bar.getY(), (float) bar.getBottom());
        }

        g.setFont (theme::monoFont (8.5f));
        g.setColour (available ? theme::textDim : theme::textFaint);
        g.drawText (available ? compactDb (values.peakDb[ch]) : juce::String ("--"),
                    numbers, juce::Justification::centredRight, false);
    }

    if (clipLatched || values.clipping)
    {
        g.setColour (theme::error);
        g.fillRect (juce::Rectangle<int> (area.getRight() - 3, area.getY() + 2, 2, area.getHeight() - 4));
    }

    if (! available)
    {
        g.setFont (theme::monoFont (8.0f));
        g.setColour (theme::textFaint.withAlpha (0.9f));
        g.drawText (engineError.isNotEmpty() ? "NO DATA" : "WAITING",
                    area.withTrimmedLeft (12), juce::Justification::centredRight, false);
    }
}

void MetersPanel::mouseDown (const juce::MouseEvent&)
{
    // Single click clears the clip latch (peak holds decay on their own).
    if (clipLatched)
    {
        clipLatched = false;
        repaint();
    }
}

void MetersPanel::mouseDoubleClick (const juce::MouseEvent&)
{
    resetPeakHolds();
}

} // namespace sp
