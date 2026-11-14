/*
    Snake Puppy Reference Player - bass management panel.
    Message thread only.
*/
#include "UI/BassControlPanel.h"

#include <cmath>
#include <cstring>
#include <functional>

namespace sp
{

namespace
{
using ui::configureLabel;
using ui::dbText;
using ui::hzText;
using ui::qText;

std::uint64_t hashDouble (std::uint64_t h, double v)
{
    std::uint64_t bits = 0;
    std::memcpy (&bits, &v, sizeof (bits));
    return h ^ (bits + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
}

const char* sectionGainField (int i) { return i == 0 ? "sub" : i == 1 ? "mid-bass" : "punch"; }

void setSectionEnabled (BassParams& b, int i, bool on)
{
    if (i == 0) b.subEnabled = on;
    else if (i == 1) b.midBassEnabled = on;
    else b.punchEnabled = on;
    if (on)
        b.enabled = true;
}

void setSectionGain (BassParams& b, int i, float v)
{
    if (i == 0) b.subGainDb = v;
    else if (i == 1) b.midBassGainDb = v;
    else b.punchGainDb = v;
    b.enabled = true;
}

void setSectionFreq (BassParams& b, int i, double v)
{
    if (i == 0) b.subFreqHz = v;
    else if (i == 1) b.midBassFreqHz = v;
    else b.punchFreqHz = v;
    b.enabled = true;
}

void setSectionQ (BassParams& b, int i, double v)
{
    if (i == 0) b.subQ = v;
    else if (i == 1) b.midBassQ = v;
    else b.punchQ = v;
    b.enabled = true;
}
} // namespace

//==============================================================================
BassControlPanel::BassControlPanel (AppContext& context)
    : ctx (context), curve (context, false)
{
    setOpaque (true);

    addAndMakeVisible (curve);
    curve.setEditableFamilies (false, false, true);
    curve.setCaption ("BASS RESPONSE");
    curve.setDbRange (-12.0f, 12.0f);

    //== The big gold BASS knob ==============================================
    bassKnob.setRange (-6.0, 6.0, 0.1);
    bassKnob.setDoubleClickReturnValue (true, 0.0);
    bassKnob.setMouseDragSensitivity (110);
    bassKnob.textFromValueFunction = [] (double v) { return dbText (v, 1); };
    bassKnob.onValueChange = [this]
    {
        if (updating)
            return;
        // The hero BASS knob uses the coordinator's dedicated path: it clamps
        // to the current range, persists the manual DSP state and ramps the
        // chain (exactly what the main-window knob and the bass shortcuts use).
        const float v = (float) bassKnob.getValue();
        ctx.applyUserBassGain (v);
        lastFingerprint = 0;
        syncFromContext (true);
        curve.refreshNow();
    };
    addAndMakeVisible (bassKnob);

    configureLabel (bassValueLabel, 24.0f, theme::goldBright, juce::Justification::centred);
    bassValueLabel.setFont (theme::monoFont (23.0f, true));
    addAndMakeVisible (bassValueLabel);

    configureLabel (bassHintLabel, 10.0f, theme::textFaint, juce::Justification::centred);
    bassHintLabel.setText ("DRAG OR SCROLL - APPLIES INSTANTLY", juce::dontSendNotification);
    addAndMakeVisible (bassHintLabel);

    enableToggle.onClick = [this]
    {
        if (updating)
            return;
        const bool on = enableToggle.getToggleState();
        applyBass ([on] (BassParams& b) { b.enabled = on; });
        updateEnabledStates();
        ctx.setStatusMessage (on ? "Bass management enabled." : "Bass management disabled.");
    };
    addAndMakeVisible (enableToggle);

    extendedToggle.onClick = [this]
    {
        if (updating)
            return;
        const bool extended = extendedToggle.getToggleState();
        const float range = extended ? 12.0f : 6.0f;
        applyBass ([range] (BassParams& b)
        {
            b.rangeDb = range;
            b.gainDb = juce::jlimit (-range, range, b.gainDb);
        });
        ctx.setStatusMessage (extended ? "Bass range: +/-12 dB." : "Bass range: +/-6 dB.");
    };
    addAndMakeVisible (extendedToggle);

    autoHeadroomToggle.setTooltip ("Auto Headroom lowers the preamp by the maximum EQ/bass boost "
                                   "so boosted curves cannot clip. Turn it off to keep the overall "
                                   "level; the output limiter still catches peaks.");
    autoHeadroomToggle.onClick = [this]
    {
        const bool on = autoHeadroomToggle.getToggleState();
        auto p = ctx.engine().getDspParams();
        p.preamp.autoHeadroom = on;
        ctx.applyUserDspParams (p);
        ctx.setStatusMessage (on ? "Auto headroom enabled: bass/EQ boost is compensated to prevent clipping."
                                 : "Auto headroom off: boost raises level; the limiter still protects peaks.", 7000);
    };
    addAndMakeVisible (autoHeadroomToggle);

    //== Shelf controls ======================================================
    configureKnob (shelfFreq, 20.0, 300.0, 80.0, 80.0,
                   [] (double v) { return hzText (v); },
                   [this] (double v) { applyBass ([v] (BassParams& b) { b.shelfFreqHz = v; b.enabled = true; }); });
    addAndMakeVisible (shelfFreq);

    configureKnob (shelfQ, 0.2, 4.0, 0.7, 0.707,
                   [] (double v) { return qText (v); },
                   [this] (double v) { applyBass ([v] (BassParams& b) { b.shelfQ = v; b.enabled = true; }); });
    addAndMakeVisible (shelfQ);

    configureKnob (shelfSlope, 0.5, 1.5, 1.0, 1.0,
                   [] (double v) { return juce::String (v, 2); },
                   [this] (double v) { applyBass ([v] (BassParams& b) { b.shelfS = v; b.enabled = true; }); });
    addAndMakeVisible (shelfSlope);

    //== Sub / mid-bass / punch sections =====================================
    Section* sections[3] { &sub, &midBass, &punch };
    for (int i = 0; i < 3; ++i)
    {
        auto& s = *sections[i];
        s.gain.setLabelText ("GAIN");
        s.freq.setLabelText ("FREQ");
        s.q.setLabelText ("Q");

        configureKnob (s.gain, -12.0, 12.0, 0.0, 0.0,
                       [] (double v) { return dbText (v, 1); },
                       [this, i] (double v) { applyBass ([i, v] (BassParams& b) { setSectionGain (b, i, (float) v); }); });
        configureKnob (s.freq, 20.0, 400.0, i == 0 ? 35.0 : i == 1 ? 100.0 : 140.0,
                       i == 0 ? 35.0 : i == 1 ? 100.0 : 140.0,
                       [] (double v) { return hzText (v); },
                       [this, i] (double v) { applyBass ([i, v] (BassParams& b) { setSectionFreq (b, i, v); }); });
        configureKnob (s.q, 0.2, 12.0, 0.9, 0.9,
                       [] (double v) { return qText (v); },
                       [this, i] (double v) { applyBass ([i, v] (BassParams& b) { setSectionQ (b, i, v); }); });

        s.enable.onClick = [this, i, &s]
        {
            if (updating)
                return;
            const bool on = s.enable.getToggleState();
            applyBass ([i, on] (BassParams& b) { setSectionEnabled (b, i, on); });
            updateEnabledStates();
            ctx.setStatusMessage (juce::String (sectionGainField (i)) + (on ? " enabled." : " disabled."));
        };

        addAndMakeVisible (s.enable);
        addAndMakeVisible (s.gain);
        addAndMakeVisible (s.freq);
        addAndMakeVisible (s.q);
    }

    // Tighten the section frequency ranges to the advertised bands.
    sub.freq.setRange (20.0, 120.0, 0.0);
    sub.freq.setSkewFactorFromMidPoint (35.0);
    midBass.freq.setRange (40.0, 250.0, 0.0);
    midBass.freq.setSkewFactorFromMidPoint (100.0);
    punch.freq.setRange (60.0, 400.0, 0.0);
    punch.freq.setSkewFactorFromMidPoint (140.0);

    configureLabel (headroomLabel, 10.5f, theme::textDim, juce::Justification::centredLeft);
    addAndMakeVisible (headroomLabel);

    setSize (940, 590);
    syncFromContext (true);
    startTimerHz (8);
}

BassControlPanel::~BassControlPanel()
{
    stopTimer();
}

//==============================================================================
void BassControlPanel::configureKnob (SpKnob& knob, double minValue, double maxValue,
                                      double midpoint, double doubleClickValue,
                                      std::function<juce::String (double)> formatter,
                                      std::function<void (double)> onChange)
{
    knob.setRange (minValue, maxValue, 0.0);
    if (midpoint > minValue && midpoint < maxValue)
        knob.setSkewFactorFromMidPoint (midpoint);
    knob.setDoubleClickReturnValue (true, doubleClickValue);
    knob.setMouseDragSensitivity (170);
    knob.textFromValueFunction = std::move (formatter);
    knob.setValue (doubleClickValue, juce::dontSendNotification);
    knob.onValueChange = [this, onChange, &knob]
    {
        if (updating)
            return;
        onChange (knob.getValue());
    };
}

void BassControlPanel::refreshNow()
{
    syncFromContext (true);
    curve.refreshNow();
}

void BassControlPanel::applyBass (const std::function<void (BassParams&)>& mutate)
{
    auto p = ctx.getCurrentDspParams();
    mutate (p.bass);
    ctx.applyUserDspParams (p);
    lastFingerprint = 0;
    syncFromContext (true);
    curve.refreshNow();
}

void BassControlPanel::timerCallback()
{
    syncFromContext (false);
}

void BassControlPanel::syncFromContext (bool force)
{
    const auto p = ctx.getCurrentDspParams();
    const auto& b = p.bass;
    const float range = b.rangeDb > 9.0f ? 12.0f : 6.0f;

    updating = true;
    enableToggle.setToggleState (b.enabled, juce::dontSendNotification);
    extendedToggle.setToggleState (range > 9.0f, juce::dontSendNotification);
    autoHeadroomToggle.setToggleState (p.preamp.autoHeadroom, juce::dontSendNotification);

    if (std::abs (bassKnob.getMaximum() - (double) range) > 0.01)
        bassKnob.setRange (-(double) range, (double) range, 0.1);
    bassKnob.setValue (b.gainDb, juce::dontSendNotification);

    shelfFreq.setValue (juce::jlimit (20.0, 300.0, b.shelfFreqHz), juce::dontSendNotification);
    shelfQ.setValue (juce::jlimit (0.2, 4.0, b.shelfQ), juce::dontSendNotification);
    shelfSlope.setValue (juce::jlimit (0.5, 1.5, b.shelfS), juce::dontSendNotification);

    Section* sections[3] { &sub, &midBass, &punch };
    const bool enables[3] { b.subEnabled, b.midBassEnabled, b.punchEnabled };
    const float gains[3] { b.subGainDb, b.midBassGainDb, b.punchGainDb };
    const double freqs[3] { b.subFreqHz, b.midBassFreqHz, b.punchFreqHz };
    const double qs[3] { b.subQ, b.midBassQ, b.punchQ };
    for (int i = 0; i < 3; ++i)
    {
        auto& s = *sections[i];
        s.enable.setToggleState (enables[i], juce::dontSendNotification);
        s.gain.setValue (juce::jlimit (-12.0f, 12.0f, gains[i]), juce::dontSendNotification);
        s.freq.setValue (freqs[i], juce::dontSendNotification);
        s.q.setValue (qs[i], juce::dontSendNotification);
    }
    updating = false;

    updateEnabledStates();
    updateReadout();

    // Indicators and headroom text only repaint when the bass state changed.
    auto fp = std::uint64_t (1469598103934665603ULL);
    fp = hashDouble (fp, b.enabled ? 1.0 : 0.0);
    fp = hashDouble (fp, b.gainDb);
    fp = hashDouble (fp, b.shelfFreqHz);
    fp = hashDouble (fp, b.shelfQ);
    fp = hashDouble (fp, b.shelfS);
    for (int i = 0; i < 3; ++i)
    {
        fp = hashDouble (fp, enables[i] ? 1.0 : 0.0);
        fp = hashDouble (fp, gains[i]);
        fp = hashDouble (fp, freqs[i]);
        fp = hashDouble (fp, qs[i]);
    }

    const auto status = ctx.engine().getStatus();
    juce::String head;
    auto headColour = theme::textDim;
    if (! status.deviceOpen && status.errorMessage.isNotEmpty())
    {
        head = "AUDIO ENGINE: " + status.errorMessage;
        headColour = theme::warning;
    }
    else
    {
        head << "BOOST " << dbText (status.dspStatus.maxPositiveBoostDb, 1) << " dB"
             << "   |   AUTO HEADROOM " << dbText (status.dspStatus.autoHeadroomDb, 1) << " dB"
             << "   |   LIMITER "
             << (status.dspStatus.limiterEngaged ? "ACTIVE " : "")
             << dbText (status.dspStatus.limiterReductionDb, 1) << " dB GR";
    }

    if (head != lastHeadroom)
    {
        lastHeadroom = head;
        headroomLabel.setText (head, juce::dontSendNotification);
        headroomLabel.setColour (juce::Label::textColourId, headColour);
        repaint();
    }

    if (force || fp != lastFingerprint)
    {
        lastFingerprint = fp;
        repaint();
    }
}

void BassControlPanel::updateEnabledStates()
{
    const auto p = ctx.getCurrentDspParams();
    const bool on = p.bass.enabled;

    bassKnob.setEnabled (on);
    shelfFreq.setEnabled (on);
    shelfQ.setEnabled (on);
    shelfSlope.setEnabled (on);
    sub.gain.setEnabled (on && p.bass.subEnabled);
    sub.freq.setEnabled (on && p.bass.subEnabled);
    sub.q.setEnabled (on && p.bass.subEnabled);
    midBass.gain.setEnabled (on && p.bass.midBassEnabled);
    midBass.freq.setEnabled (on && p.bass.midBassEnabled);
    midBass.q.setEnabled (on && p.bass.midBassEnabled);
    punch.gain.setEnabled (on && p.bass.punchEnabled);
    punch.freq.setEnabled (on && p.bass.punchEnabled);
    punch.q.setEnabled (on && p.bass.punchEnabled);

    // The section enable and range toggles stay usable so the panel is never
    // a dead end when bass management is off.
    extendedToggle.setEnabled (true);
}

void BassControlPanel::updateReadout()
{
    const auto p = ctx.getCurrentDspParams();
    bassValueLabel.setText (dbText (p.bass.gainDb, 1) + " dB", juce::dontSendNotification);
    bassValueLabel.setColour (juce::Label::textColourId,
                              p.bass.enabled ? theme::goldBright : theme::textFaint);
}

//==============================================================================
void BassControlPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    auto top = getLocalBounds().reduced (14, 8).removeFromTop (24);
    theme::drawSectionHeader (g, top, "BASS MANAGEMENT",
                              getWidth() < 520 ? juce::String()
                                               : juce::String ("LOW SHELF + SUB / MID-BASS / PUNCH"));

    paintCard (g, bassCard, "BASS", "LOW SHELF");
    paintCard (g, shelfCard, "SHELF SHAPING", "FREQUENCY / Q / SLOPE");
    paintCard (g, curveCard, "RESPONSE", "20 Hz - 400 Hz");

    Section* sections[3] { &sub, &midBass, &punch };
    for (auto* s : sections)
    {
        paintCard (g, s->card, s->title, s->subtitle);
        paintBandIndicator (g, *s);
    }

    paintHeadroom (g, headroomArea);
}

void BassControlPanel::paintCard (juce::Graphics& g, juce::Rectangle<int> card,
                                  const juce::String& title, const juce::String& right)
{
    if (card.isEmpty())
        return;

    theme::fillPanel (g, card);
    auto titleArea = card.reduced (10, 5).removeFromTop (16);
    theme::drawSectionHeader (g, titleArea, title, right);
}

void BassControlPanel::paintBandIndicator (juce::Graphics& g, const Section& s)
{
    if (s.indicator.isEmpty())
        return;

    const auto p = ctx.getCurrentDspParams();
    const auto& b = p.bass;

    const bool isSub = (&s == &sub);
    const bool isMid = (&s == &midBass);
    const double freq = isSub ? b.subFreqHz : isMid ? b.midBassFreqHz : b.punchFreqHz;
    const float gain = isSub ? b.subGainDb : isMid ? b.midBassGainDb : b.punchGainDb;
    const double q = isSub ? b.subQ : isMid ? b.midBassQ : b.punchQ;
    const bool enabled = isSub ? b.subEnabled : isMid ? b.midBassEnabled : b.punchEnabled;

    auto area = s.indicator;
    theme::fillSunken (g, area);

    auto axis = area.reduced (6, 4);
    auto labelArea = axis.removeFromTop (12);
    axis.removeFromTop (2);

    // Log axis 20..400 Hz.
    const double minF = 20.0, maxF = 400.0;
    auto freqToX = [&] (double hz)
    {
        const double t = std::log (juce::jlimit (minF, maxF, hz) / minF) / std::log (maxF / minF);
        return axis.getX() + (float) t * (float) axis.getWidth();
    };

    g.setColour (theme::lineFaint);
    for (double f : { 20.0, 50.0, 100.0, 200.0, 400.0 })
    {
        const float x = freqToX (f);
        g.drawVerticalLine (juce::roundToInt (x), (float) axis.getY(), (float) axis.getBottom());
        g.setFont (theme::monoFont (8.0f));
        g.setColour (theme::textFaint);
        auto freqLabel = juce::Rectangle<float> (x - 12.0f, (float) axis.getBottom() + 1.0f, 24.0f, 10.0f);
        const auto limits = area.toFloat().reduced (1.0f);
        freqLabel.setX (juce::jlimit (limits.getX(),
                                      juce::jmax (limits.getX(), limits.getRight() - freqLabel.getWidth()),
                                      freqLabel.getX()));
        g.drawText (f < 1000.0 ? juce::String ((int) f) : juce::String (f / 1000.0, 1) + "k",
                    freqLabel, juce::Justification::centredTop, false);
        g.setColour (theme::lineFaint);
    }

    const float zeroY = (float) axis.getCentreY();
    g.setColour (theme::lineStrong);
    g.drawHorizontalLine (juce::roundToInt (zeroY), (float) axis.getX(), (float) axis.getRight());

    // Gaussian band shape centred at the real frequency, width from Q (log axis).
    const double centreX = (double) freqToX (freq);
    const double octavesPerPixel = std::log2 (maxF / minF) / (double) juce::jmax (1, axis.getWidth());
    const double sigmaOct = juce::jlimit (0.08, 2.2, 0.5 / juce::jmax (0.1, q));
    const double sigmaPx = sigmaOct / octavesPerPixel;

    juce::Path shape;
    bool started = false;
    for (int x = axis.getX(); x <= axis.getRight(); x += 2)
    {
        const double d = ((double) x - centreX) / juce::jmax (1.0, sigmaPx);
        const double gauss = std::exp (-0.5 * d * d);
        const float y = (float) (zeroY - (double) gain / 12.0 * (double) axis.getHeight() * 0.5 * gauss);
        if (! started)
        {
            shape.startNewSubPath ((float) x, zeroY);
            started = true;
        }
        shape.lineTo ((float) x, y);
    }
    shape.lineTo ((float) axis.getRight(), zeroY);
    shape.closeSubPath();

    auto colour = enabled ? s.colour : theme::silverDark;
    if (! b.enabled)
        colour = theme::textFaint;

    g.setColour (colour.withAlpha (enabled && b.enabled ? 0.22f : 0.08f));
    g.fillPath (shape);
    g.setColour (colour.withAlpha (enabled && b.enabled ? 0.95f : 0.45f));
    g.strokePath (shape, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // Value label.
    g.setFont (theme::monoFont (9.0f));
    g.setColour (enabled && b.enabled ? theme::silver : theme::textFaint);
    juce::String text;
    text << hzText (freq) << "   " << dbText (gain, 1) << " dB   Q " << qText (q);
    g.drawText (text, labelArea, juce::Justification::centredLeft, false);

    if (! enabled || ! b.enabled)
    {
        g.setColour (theme::textFaint);
        g.setFont (theme::capsFont (8.0f, 0.12f));
        g.drawText ("OFF", labelArea, juce::Justification::centredRight, false);
    }
}

void BassControlPanel::paintHeadroom (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (area.isEmpty())
        return;
    theme::drawHairline (g, area.getX(), area.getY(), area.getRight(), area.getY(), theme::lineFaint);
}

//==============================================================================
void BassControlPanel::resized()
{
    auto r = getLocalBounds().reduced (14, 8);
    r.removeFromTop (24);
    r.removeFromBottom (4);

    // Hero-only rail layout (the fixed-height main-window rail): the big BASS
    // control is the point; every other control lives in the DSP Studio Bass tab.
    const bool heroMode = getHeight() < 620;
    if (heroMode)
    {
        curve.setVisible (false);
        shelfFreq.setVisible (false);
        shelfQ.setVisible (false);
        shelfSlope.setVisible (false);
        sub.enable.setVisible (false);  sub.gain.setVisible (false);  sub.freq.setVisible (false);  sub.q.setVisible (false);
        midBass.enable.setVisible (false); midBass.gain.setVisible (false); midBass.freq.setVisible (false); midBass.q.setVisible (false);
        punch.enable.setVisible (false); punch.gain.setVisible (false); punch.freq.setVisible (false); punch.q.setVisible (false);
        bassValueLabel.setVisible (false);
        bassHintLabel.setVisible (false);
        enableToggle.setVisible (false);
        extendedToggle.setVisible (false);
        curveCard = {};
        shelfCard = {};
        sub.card = {}; midBass.card = {}; punch.card = {};
        sub.indicator = {}; midBass.indicator = {}; punch.indicator = {};

        autoHeadroomToggle.setVisible (false);
        headroomArea = r.removeFromBottom (16);
        headroomLabel.setBounds (headroomArea);
        r.removeFromBottom (2);

        bassCard = r;
        auto knobArea = r;
        knobArea.removeFromTop (16);   // card title row
        const int knobSize = juce::jmax (60, juce::jmin (knobArea.getWidth() - 40, knobArea.getHeight() - 8));
        bassKnob.setBounds (knobArea.withSizeKeepingCentre (knobSize, knobSize));
        return;
    }

    // Restore visibility for the full layouts.
    curve.setVisible (true);
    shelfFreq.setVisible (true);
    shelfQ.setVisible (true);
    shelfSlope.setVisible (true);
    sub.enable.setVisible (true);  sub.gain.setVisible (true);  sub.freq.setVisible (true);  sub.q.setVisible (true);
    midBass.enable.setVisible (true); midBass.gain.setVisible (true); midBass.freq.setVisible (true); midBass.q.setVisible (true);
    punch.enable.setVisible (true); punch.gain.setVisible (true); punch.freq.setVisible (true); punch.q.setVisible (true);
    bassValueLabel.setVisible (true);
    bassHintLabel.setVisible (true);
    enableToggle.setVisible (true);
    extendedToggle.setVisible (true);
    autoHeadroomToggle.setVisible (true);

    // Narrow right-rail layout: one usable column instead of three cards.
    if (getWidth() < 700)
    {
        curve.setVisible (false);
        curveCard = {};

        headroomArea = r.removeFromBottom (22);
        autoHeadroomToggle.setBounds (headroomArea.removeFromLeft (juce::jmin (140, headroomArea.getWidth() / 2)));
        headroomLabel.setBounds (headroomArea);
        r.removeFromBottom (6);

        bassCard = r.removeFromTop (juce::jmin (214, juce::jmax (176, r.getHeight() / 3)));
        r.removeFromTop (8);
        shelfCard = r.removeFromTop (108);
        r.removeFromTop (8);

        {
            auto inner = bassCard.reduced (10, 6);
            inner.removeFromTop (18);
            const int knobSize = juce::jlimit (104, 138, juce::jmin (inner.getWidth() - 30, inner.getHeight() - 64));
            bassKnob.setBounds (inner.removeFromTop (knobSize).withSizeKeepingCentre (knobSize, knobSize));
            bassValueLabel.setBounds (inner.removeFromTop (28));
            bassHintLabel.setBounds (inner.removeFromTop (13));
            auto toggles = inner.removeFromTop (22);
            enableToggle.setBounds (toggles.removeFromLeft (toggles.getWidth() / 2 - 4));
            toggles.removeFromLeft (8);
            extendedToggle.setBounds (toggles);
        }

        {
            auto inner = shelfCard.reduced (10, 6);
            inner.removeFromTop (18);
            auto row = inner.removeFromTop (juce::jmin (84, inner.getHeight()));
            const int kw = juce::jmax (40, row.getWidth() / 3);
            shelfFreq.setBounds (row.removeFromLeft (kw).reduced (2));
            shelfQ.setBounds (row.removeFromLeft (kw).reduced (2));
            shelfSlope.setBounds (row.removeFromLeft (kw).reduced (2));
        }

        Section* sections[3] { &sub, &midBass, &punch };
        for (auto* sct : sections)
        {
            sct->card = r.removeFromTop (98);
            r.removeFromTop (8);

            auto inner = sct->card.reduced (10, 5);
            inner.removeFromTop (17);
            auto enableRow = inner.removeFromTop (20);
            sct->enable.setBounds (enableRow.removeFromLeft (juce::jmin (108, enableRow.getWidth())));
            auto row = inner.removeFromTop (juce::jmin (66, juce::jmax (0, inner.getHeight())));
            const int kw = juce::jmax (40, row.getWidth() / 3);
            sct->gain.setBounds (row.removeFromLeft (kw).reduced (2));
            sct->freq.setBounds (row.removeFromLeft (kw).reduced (2));
            sct->q.setBounds (row.removeFromLeft (kw).reduced (2));
            sct->indicator = {};
        }
        return;
    }

    curve.setVisible (true);
    curveCard = {};

    headroomArea = r.removeFromBottom (22);
    autoHeadroomToggle.setBounds (headroomArea.removeFromLeft (juce::jmin (140, headroomArea.getWidth() / 2)));
    headroomLabel.setBounds (headroomArea);

    r.removeFromBottom (6);

    const int sectionHeight = juce::jlimit (186, 248, r.getHeight() * 42 / 100);
    sectionRow = r.removeFromBottom (sectionHeight);
    r.removeFromBottom (10);

    bassCard = r.removeFromLeft (juce::jmin (300, r.getWidth() / 3));
    r.removeFromLeft (10);
    shelfCard = r.removeFromLeft (juce::jmin (272, r.getWidth() / 3));
    r.removeFromLeft (10);
    curveCard = r;

    //== Big BASS knob card ==================================================
    {
        auto inner = bassCard.reduced (10, 6);
        inner.removeFromTop (18);
        const int knobSize = juce::jlimit (130, 176, juce::jmin (inner.getWidth(), inner.getHeight() - 96));
        auto knobArea = inner.removeFromTop (knobSize);
        bassKnob.setBounds (knobArea.withSizeKeepingCentre (knobSize, knobSize));

        inner.removeFromTop (2);
        bassValueLabel.setBounds (inner.removeFromTop (30));
        bassHintLabel.setBounds (inner.removeFromTop (14));
        inner.removeFromTop (8);

        auto toggles = inner.removeFromTop (24);
        enableToggle.setBounds (toggles.removeFromLeft (juce::jmin (170, toggles.getWidth() / 2)));
        toggles.removeFromLeft (6);
        extendedToggle.setBounds (toggles);
    }

    //== Shelf card =========================================================
    {
        auto inner = shelfCard.reduced (10, 6);
        inner.removeFromTop (18);
        auto knobs = inner.removeFromTop (juce::jmin (110, inner.getHeight()));
        const int kw = juce::jmin (92, knobs.getWidth() / 3);
        auto row = knobs.withSizeKeepingCentre (kw * 3, knobs.getHeight());
        shelfFreq.setBounds (row.removeFromLeft (kw).reduced (2));
        shelfQ.setBounds (row.removeFromLeft (kw).reduced (2));
        shelfSlope.setBounds (row.removeFromLeft (kw).reduced (2));
    }

    //== Curve card =========================================================
    {
        auto inner = curveCard.reduced (10, 6);
        inner.removeFromTop (18);
        curve.setBounds (inner);
    }

    //== Sub / mid-bass / punch ==============================================
    Section* sections[3] { &sub, &midBass, &punch };
    const int cardWidth = juce::jmax (200, (sectionRow.getWidth() - 20) / 3);
    int x = sectionRow.getX();
    for (auto* s : sections)
    {
        s->card = juce::Rectangle<int> (x, sectionRow.getY(), cardWidth, sectionRow.getHeight());
        x += cardWidth + 10;

        auto inner = s->card.reduced (10, 6);
        inner.removeFromTop (18);
        auto enableRow = inner.removeFromTop (20);
        s->enable.setBounds (enableRow.removeFromLeft (juce::jmin (110, enableRow.getWidth())));
        inner.removeFromTop (2);

        auto knobs = inner.removeFromTop (juce::jmin (94, juce::jmax (60, inner.getHeight() - 52)));
        const int kw = juce::jmin (84, knobs.getWidth() / 3);
        auto row = knobs.withSizeKeepingCentre (kw * 3, knobs.getHeight());
        s->gain.setBounds (row.removeFromLeft (kw).reduced (2));
        s->freq.setBounds (row.removeFromLeft (kw).reduced (2));
        s->q.setBounds (row.removeFromLeft (kw).reduced (2));

        inner.removeFromTop (4);
        s->indicator = inner;
    }
}

} // namespace sp
