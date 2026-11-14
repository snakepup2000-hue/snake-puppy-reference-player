/*
    Snake Puppy Reference Player - interactive combined EQ response graph.
    See EqCurveComponent.h for the contract. Message thread only.
*/
#include "UI/EqCurveComponent.h"

#include <cmath>
#include <cstring>

namespace sp
{

namespace
{
using ui::dbText;
using ui::freqText;
using ui::qText;

constexpr int kResponsePoints = 512;
constexpr double kMinFreq = 20.0;
constexpr double kMaxFreq = 20000.0;

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

void hashValue (std::uint64_t& h, double v)
{
    std::uint64_t bits = 0;
    std::memcpy (&bits, &v, sizeof (bits));
    h ^= bits + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
}

std::uint64_t fingerprint (const DspParams& p)
{
    std::uint64_t h = 1469598103934665603ULL;
    hashValue (h, p.bypassed ? 1.0 : 0.0);
    hashValue (h, p.eqBypass ? 1.0 : 0.0);
    hashValue (h, p.graphic.enabled ? 1.0 : 0.0);
    hashValue (h, (double) p.graphic.bandSet);
    hashValue (h, (double) p.graphic.rangeDb);
    for (auto g : p.graphic.gains15) hashValue (h, g);
    for (auto g : p.graphic.gains31) hashValue (h, g);

    hashValue (h, p.parametric.enabled ? 1.0 : 0.0);
    for (auto& b : p.parametric.bands)
    {
        hashValue (h, b.enabled ? 1.0 : 0.0);
        hashValue (h, b.bypassed ? 1.0 : 0.0);
        hashValue (h, (double) b.type);
        hashValue (h, b.freqHz);
        hashValue (h, b.gainDb);
        hashValue (h, b.q);
    }

    hashValue (h, p.bass.enabled ? 1.0 : 0.0);
    hashValue (h, p.bass.gainDb);
    hashValue (h, p.bass.shelfFreqHz);
    hashValue (h, p.bass.shelfQ);
    hashValue (h, p.bass.shelfS);
    hashValue (h, p.bass.subEnabled ? 1.0 : 0.0);
    hashValue (h, p.bass.subGainDb);
    hashValue (h, p.bass.subFreqHz);
    hashValue (h, p.bass.subQ);
    hashValue (h, p.bass.midBassEnabled ? 1.0 : 0.0);
    hashValue (h, p.bass.midBassGainDb);
    hashValue (h, p.bass.midBassFreqHz);
    hashValue (h, p.bass.midBassQ);
    hashValue (h, p.bass.punchEnabled ? 1.0 : 0.0);
    hashValue (h, p.bass.punchGainDb);
    hashValue (h, p.bass.punchFreqHz);
    hashValue (h, p.bass.punchQ);

    hashValue (h, p.filters.highPassEnabled ? 1.0 : 0.0);
    hashValue (h, p.filters.highPassHz);
    hashValue (h, p.filters.highPassSlopeDbPerOct);
    hashValue (h, p.filters.lowPassEnabled ? 1.0 : 0.0);
    hashValue (h, p.filters.lowPassHz);
    hashValue (h, p.filters.lowPassSlopeDbPerOct);
    return h;
}

juce::String freqAxisLabel (double hz)
{
    if (hz >= 1000.0)
    {
        auto k = hz / 1000.0;
        return (k == std::floor (k) ? juce::String ((int) k) : juce::String (k, 1)) + "k";
    }
    return juce::String ((int) hz);
}
} // namespace

//==============================================================================
EqCurveComponent::EqCurveComponent (AppContext& context, bool shouldBeMini)
    : ctx (context), mini (shouldBeMini)
{
    setOpaque (true);
    setWantsKeyboardFocus (false);
    setMouseCursor (juce::MouseCursor::CrosshairCursor);

    responseFreqs.resize (kResponsePoints);
    for (int i = 0; i < kResponsePoints; ++i)
    {
        const double t = (double) i / (double) (kResponsePoints - 1);
        responseFreqs[(size_t) i] = kMinFreq * std::pow (kMaxFreq / kMinFreq, t);
    }
    responseDb.assign ((size_t) kResponsePoints, 0.0f);

    setSize (mini ? 300 : 760, mini ? 180 : 320);
    refreshIfChanged (true);
    if (! mini)
        startTimerHz (30);
}

EqCurveComponent::~EqCurveComponent()
{
    stopTimer();
}

//==============================================================================
void EqCurveComponent::setEditable (bool shouldBeEditable)
{
    editable = shouldBeEditable;
    setMouseCursor (editable ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void EqCurveComponent::setEditableFamilies (bool graphicBands, bool parametricBands, bool bassBands)
{
    showGraphic = graphicBands;
    showParametric = parametricBands;
    showBass = bassBands;
    rebuildNodes();
    repaint();
}

void EqCurveComponent::setShowGraphicBands (bool shouldShow)
{
    showGraphic = shouldShow;
    rebuildNodes();
    repaint();
}

void EqCurveComponent::setShowParametricBands (bool shouldShow)
{
    showParametric = shouldShow;
    rebuildNodes();
    repaint();
}

void EqCurveComponent::setShowBassBands (bool shouldShow)
{
    showBass = shouldShow;
    rebuildNodes();
    repaint();
}

void EqCurveComponent::setCaption (juce::String text)
{
    caption = std::move (text);
    repaint();
}

void EqCurveComponent::setMini (bool shouldBeMini)
{
    if (mini == shouldBeMini)
        return;
    mini = shouldBeMini;
    if (mini)
        stopTimer();
    else
        startTimerHz (30);
    resized();
    repaint();
}

void EqCurveComponent::setDbRange (float minValue, float maxValue)
{
    minDb = juce::jmin (minValue, -1.0f);
    maxDb = juce::jmax (maxValue, 1.0f);
    repaint();
}

void EqCurveComponent::resized()
{
    repaint();
}

void EqCurveComponent::refreshNow()
{
    refreshIfChanged (true);
    repaint();
}

//==============================================================================
void EqCurveComponent::timerCallback()
{
    refreshIfChanged (false);
}

void EqCurveComponent::refreshIfChanged (bool force)
{
    params = ctx.getCurrentDspParams();
    const auto fp = fingerprint (params);
    const auto now = juce::Time::currentTimeMillis();

    bool changed = force || fp != lastFingerprint;
    if (changed)
    {
        lastFingerprint = fp;
        lastChangeMs = now;
        rebuildNodes();
    }

    // Keep sampling the live (smoothed) chain response for a moment after a
    // change so the curve glides from old to new exactly like the audio does.
    const bool smoothing = dragNode >= 0 || (now - lastChangeMs) < 450;
    if (changed || smoothing)
    {
        computeResponse();
        lastTickMs = now;
        repaint();
    }
}

void EqCurveComponent::rebuildNodes()
{
    nodes.clear();
    nodes.reserve (GraphicEqParams::kNumBands31 + ParametricEqParams::kNumBands + 5);

    const bool globalOn = ! params.bypassed && ! params.eqBypass;

    if (showGraphic)
    {
        const int numBands = params.graphic.bandSet == GraphicEqParams::BandSet::bands31
                                 ? GraphicEqParams::kNumBands31
                                 : GraphicEqParams::kNumBands15;
        const bool active = globalOn && params.graphic.enabled;
        for (int i = 0; i < numBands; ++i)
        {
            Node n;
            n.kind = NodeKind::graphic;
            n.index = i;
            n.freqHz = graphicCentreFor (params.graphic, i);
            n.gainDb = graphicGainFor (params.graphic, i);
            n.active = active;
            nodes.push_back (n);
        }
    }

    if (showParametric)
    {
        const bool sectionOn = globalOn && params.parametric.enabled;
        for (int i = 0; i < ParametricEqParams::kNumBands; ++i)
        {
            const auto& b = params.parametric.bands[(size_t) i];
            if (! b.enabled)
                continue;
            Node n;
            n.kind = NodeKind::parametric;
            n.index = i;
            n.freqHz = b.freqHz;
            n.gainDb = (float) b.gainDb;
            n.active = sectionOn && b.isActive();
            if (b.type == BandType::highPass)  n.tag = "HPF";
            if (b.type == BandType::lowPass)   n.tag = "LPF";
            if (b.type == BandType::allPass)   n.tag = "APF";
            if (b.type == BandType::bandPass)  n.tag = "BPF";
            if (b.type == BandType::notch)     n.tag = "NOTCH";
            nodes.push_back (n);
        }
    }

    if (showBass)
    {
        const bool sectionOn = globalOn && params.bass.enabled;
        {
            Node n;
            n.kind = NodeKind::bassShelf;
            n.freqHz = params.bass.shelfFreqHz;
            n.gainDb = params.bass.gainDb;
            n.active = sectionOn;
            nodes.push_back (n);
        }
        if (params.bass.subEnabled)
        {
            Node n;
            n.kind = NodeKind::bassSub;
            n.freqHz = params.bass.subFreqHz;
            n.gainDb = params.bass.subGainDb;
            n.active = sectionOn;
            nodes.push_back (n);
        }
        if (params.bass.midBassEnabled)
        {
            Node n;
            n.kind = NodeKind::bassMidBass;
            n.freqHz = params.bass.midBassFreqHz;
            n.gainDb = params.bass.midBassGainDb;
            n.active = sectionOn;
            nodes.push_back (n);
        }
        if (params.bass.punchEnabled)
        {
            Node n;
            n.kind = NodeKind::bassPunch;
            n.freqHz = params.bass.punchFreqHz;
            n.gainDb = params.bass.punchGainDb;
            n.active = sectionOn;
            nodes.push_back (n);
        }
    }

    if (dragNode >= (int) nodes.size())
        dragNode = -1;
    if (hoverNode >= (int) nodes.size())
        hoverNode = -1;
}

void EqCurveComponent::computeResponse()
{
    auto db = ctx.engine().dspChain().getMagnitudeResponseDb (responseFreqs);
    if (db.size() != responseDb.size())
        db.resize (responseDb.size(), 0.0f);
    responseDb = std::move (db);
}

//==============================================================================
juce::Rectangle<float> EqCurveComponent::graphArea() const
{
    auto r = getLocalBounds().toFloat().reduced (mini ? 3.0f : 6.0f);
    r.removeFromTop (mini ? 14.0f : 17.0f);
    if (mini)
        r.removeFromBottom (1.0f);
    if (! mini)
    {
        r.removeFromLeft (34.0f);
        r.removeFromBottom (18.0f);
    }
    return r;
}

float EqCurveComponent::freqToX (double hz, juce::Rectangle<float> r) const
{
    const double clamped = juce::jlimit (kMinFreq, kMaxFreq, hz);
    const double t = std::log (clamped / kMinFreq) / std::log (kMaxFreq / kMinFreq);
    return r.getX() + (float) t * r.getWidth();
}

double EqCurveComponent::xToFreq (float x, juce::Rectangle<float> r) const
{
    const double t = juce::jlimit (0.0, 1.0, (double) ((x - r.getX()) / juce::jmax (1.0f, r.getWidth())));
    return kMinFreq * std::pow (kMaxFreq / kMinFreq, t);
}

float EqCurveComponent::dbToY (float db, juce::Rectangle<float> r) const
{
    const float t = juce::jlimit (0.0f, 1.0f, (db - minDb) / juce::jmax (1.0f, maxDb - minDb));
    return r.getBottom() - t * r.getHeight();
}

float EqCurveComponent::yToDb (float y, juce::Rectangle<float> r) const
{
    const float t = juce::jlimit (0.0f, 1.0f, (r.getBottom() - y) / juce::jmax (1.0f, r.getHeight()));
    return minDb + t * (maxDb - minDb);
}

float EqCurveComponent::responseAt (double hz) const
{
    if (responseFreqs.empty() || responseDb.empty())
        return 0.0f;

    const double clamped = juce::jlimit (kMinFreq, kMaxFreq, hz);
    const double t = std::log (clamped / kMinFreq) / std::log (kMaxFreq / kMinFreq);
    const double pos = juce::jlimit (0.0, (double) responseFreqs.size() - 1.0, t * (double) (responseFreqs.size() - 1));
    const auto i0 = (size_t) pos;
    const auto i1 = juce::jmin (responseFreqs.size() - 1, i0 + 1);
    const float frac = (float) (pos - (double) i0);
    return responseDb[i0] + frac * (responseDb[i1] - responseDb[i0]);
}

int EqCurveComponent::findNodeAt (juce::Point<float> p, juce::Rectangle<float> r) const
{
    const float radius = (float) (mini ? 10 : 12);
    int best = -1;
    float bestDist = radius;

    for (int i = 0; i < (int) nodes.size(); ++i)
    {
        const auto& n = nodes[(size_t) i];
        const juce::Point<float> pos (freqToX (n.freqHz, r), dbToY (juce::jlimit (minDb, maxDb, n.gainDb), r));
        const float dist = pos.getDistanceFrom (p);
        if (dist <= bestDist)
        {
            bestDist = dist;
            best = i;
        }
    }
    return best;
}

//==============================================================================
void EqCurveComponent::applyNodeDrag (const Node& node, double freqHz, float gainDb)
{
    DspParams p = ctx.getCurrentDspParams();

    switch (node.kind)
    {
        case NodeKind::graphic:
            p.graphic.enabled = true;
            p.graphic.gainForSet (node.index) = gainDb;
            break;

        case NodeKind::parametric:
            if (node.index >= 0 && node.index < ParametricEqParams::kNumBands)
            {
                auto& b = p.parametric.bands[(size_t) node.index];
                b.freqHz = freqHz;
                b.gainDb = (double) gainDb;
                p.parametric.enabled = true;
            }
            break;

        case NodeKind::bassShelf:
            p.bass.enabled = true;
            p.bass.shelfFreqHz = freqHz;
            p.bass.gainDb = gainDb;
            break;

        case NodeKind::bassSub:
            p.bass.enabled = true;
            p.bass.subFreqHz = freqHz;
            p.bass.subGainDb = gainDb;
            break;

        case NodeKind::bassMidBass:
            p.bass.enabled = true;
            p.bass.midBassFreqHz = freqHz;
            p.bass.midBassGainDb = gainDb;
            break;

        case NodeKind::bassPunch:
            p.bass.enabled = true;
            p.bass.punchFreqHz = freqHz;
            p.bass.punchGainDb = gainDb;
            break;
    }

    ctx.applyUserDspParams (p);
    params = p;
    lastFingerprint = fingerprint (params);
    lastChangeMs = juce::Time::currentTimeMillis();
    rebuildNodes();
    computeResponse();

    // Keep local node geometry authoritative while the mouse is down.
    if (dragNode >= 0 && dragNode < (int) nodes.size())
    {
        nodes[(size_t) dragNode].freqHz = freqHz;
        nodes[(size_t) dragNode].gainDb = gainDb;
    }

    repaint();
    if (onUserEdited != nullptr)
        onUserEdited();
}

void EqCurveComponent::mouseMove (const juce::MouseEvent& e)
{
    const auto r = graphArea();
    cursor = e.position;
    cursorValid = r.contains (e.position);

    const int found = editable ? findNodeAt (e.position, r) : -1;
    if (found != hoverNode)
    {
        hoverNode = found;
        repaint();
    }
    else if (cursorValid)
    {
        repaint();
    }

    setMouseCursor (found >= 0 ? juce::MouseCursor::PointingHandCursor
                               : juce::MouseCursor::CrosshairCursor);
}

void EqCurveComponent::mouseDown (const juce::MouseEvent& e)
{
    const auto r = graphArea();
    if (! editable || ! r.contains (e.position))
        return;

    const int found = findNodeAt (e.position, r);
    if (found < 0)
        return;

    dragNode = found;
    hoverNode = found;
    dragStartMouse = e.position;
    dragStartGain = nodes[(size_t) found].gainDb;
    dragStartFreq = nodes[(size_t) found].freqHz;
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    repaint();
}

void EqCurveComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (dragNode < 0 || dragNode >= (int) nodes.size())
        return;

    const auto r = graphArea();
    const auto node = nodes[(size_t) dragNode];
    const float pixelsPerDb = juce::jmax (1.0f, r.getHeight()) / (maxDb - minDb);
    float gain = dragStartGain + (dragStartMouse.y - e.position.y) / pixelsPerDb;

    double freq = node.freqHz;
    switch (node.kind)
    {
        case NodeKind::graphic:
            gain = juce::jlimit (-params.graphic.rangeDb, params.graphic.rangeDb, gain);
            break;

        case NodeKind::parametric:
        {
            gain = juce::jlimit (minDb, maxDb, gain);
            const double ratio = std::pow (kMaxFreq / kMinFreq,
                                           (double) (e.position.x - dragStartMouse.x) / juce::jmax (1.0f, r.getWidth()));
            freq = juce::jlimit (kMinFreq, kMaxFreq, dragStartFreq * ratio);
            break;
        }

        case NodeKind::bassShelf:
        {
            gain = juce::jlimit (-params.bass.rangeDb, params.bass.rangeDb, gain);
            const double ratio = std::pow (300.0 / 20.0,
                                           (double) (e.position.x - dragStartMouse.x) / juce::jmax (1.0f, r.getWidth()));
            freq = juce::jlimit (20.0, 300.0, dragStartFreq * ratio);
            break;
        }

        case NodeKind::bassSub:
        {
            gain = juce::jlimit (-12.0f, 12.0f, gain);
            const double ratio = std::pow (120.0 / 20.0,
                                           (double) (e.position.x - dragStartMouse.x) / juce::jmax (1.0f, r.getWidth()));
            freq = juce::jlimit (20.0, 120.0, dragStartFreq * ratio);
            break;
        }

        case NodeKind::bassMidBass:
        {
            gain = juce::jlimit (-12.0f, 12.0f, gain);
            const double ratio = std::pow (250.0 / 40.0,
                                           (double) (e.position.x - dragStartMouse.x) / juce::jmax (1.0f, r.getWidth()));
            freq = juce::jlimit (40.0, 250.0, dragStartFreq * ratio);
            break;
        }

        case NodeKind::bassPunch:
        {
            gain = juce::jlimit (-12.0f, 12.0f, gain);
            const double ratio = std::pow (400.0 / 60.0,
                                           (double) (e.position.x - dragStartMouse.x) / juce::jmax (1.0f, r.getWidth()));
            freq = juce::jlimit (60.0, 400.0, dragStartFreq * ratio);
            break;
        }
    }

    cursor = e.position;
    cursorValid = r.contains (e.position);
    applyNodeDrag (node, freq, gain);
}

void EqCurveComponent::mouseUp (const juce::MouseEvent&)
{
    dragNode = -1;
    refreshNow();
}

void EqCurveComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto r = graphArea();
    if (! editable)
        return;

    const int found = findNodeAt (e.position, r);
    if (found < 0)
        return;

    const auto node = nodes[(size_t) found];
    if (std::abs (node.gainDb) < 0.01f)
        return;

    applyNodeDrag (node, node.freqHz, 0.0f);
}

void EqCurveComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto r = graphArea();
    if (! editable || ! r.contains (e.position))
    {
        juce::Component::mouseWheelMove (e, wheel);
        return;
    }

    const int found = findNodeAt (e.position, r);
    if (found < 0)
    {
        juce::Component::mouseWheelMove (e, wheel);
        return;
    }

    const auto node = nodes[(size_t) found];
    DspParams p = ctx.getCurrentDspParams();
    const double factor = wheel.deltaY > 0.0f ? 1.12 : 1.0 / 1.12;

    switch (node.kind)
    {
        case NodeKind::parametric:
            if (node.index >= 0 && node.index < ParametricEqParams::kNumBands)
                p.parametric.bands[(size_t) node.index].q =
                    juce::jlimit (0.1, 18.0, p.parametric.bands[(size_t) node.index].q * factor);
            break;

        case NodeKind::bassShelf:
            p.bass.shelfQ = juce::jlimit (0.2, 4.0, p.bass.shelfQ * factor);
            break;

        case NodeKind::bassSub:
            p.bass.subQ = juce::jlimit (0.2, 12.0, p.bass.subQ * factor);
            break;

        case NodeKind::bassMidBass:
            p.bass.midBassQ = juce::jlimit (0.2, 12.0, p.bass.midBassQ * factor);
            break;

        case NodeKind::bassPunch:
            p.bass.punchQ = juce::jlimit (0.2, 12.0, p.bass.punchQ * factor);
            break;

        case NodeKind::graphic:
            return;
    }

    ctx.applyUserDspParams (p);
    refreshNow();
    if (onUserEdited != nullptr)
        onUserEdited();
}

void EqCurveComponent::mouseExit (const juce::MouseEvent&)
{
    hoverNode = -1;
    cursorValid = false;
    repaint();
}

//==============================================================================
void EqCurveComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);

    auto bounds = getLocalBounds();
    const auto r = graphArea();
    const bool bypassed = params.bypassed || params.eqBypass;

    //== Caption / state ======================================================
    {
        auto top = bounds.removeFromTop (mini ? 14 : 17).reduced (6, 0);
        g.setColour (theme::textDim);
        g.setFont (theme::capsFont (mini ? 8.5f : 9.5f, 0.14f));
        g.drawText (caption, top, juce::Justification::centredLeft, false);

        if (! mini)
        {
            juce::String state;
            juce::Colour stateColour = theme::textFaint;
            if (params.bypassed)          { state = "DSP BYPASSED";  stateColour = theme::warning; }
            else if (params.eqBypass)     { state = "EQ BYPASSED";   stateColour = theme::warning; }
            else if (ctx.engine().isReferenceMode()) { state = "REFERENCE"; stateColour = theme::gold; }

            g.setFont (theme::monoFont (9.5f));
            g.setColour (stateColour);
            g.drawText (state, top.withTrimmedRight (2), juce::Justification::centredRight, false);
        }
    }

    //== Graph ================================================================
    theme::fillSunken (g, r.toNearestInt());
    paintGrid (g, r);
    paintCurve (g, r);
    paintNodes (g, r);
    paintReadout (g, r);

    if (! mini)
    {
        auto legend = r.toNearestInt().reduced (8, 5).removeFromBottom (13);
        g.setFont (theme::monoFont (9.0f));
        g.setColour (theme::textFaint);
        g.drawText (bypassed ? "CHAIN RESPONSE (BYPASSED)" : "LIVE CHAIN RESPONSE",
                    legend, juce::Justification::bottomRight, false);
    }
}

void EqCurveComponent::paintGrid (juce::Graphics& g, juce::Rectangle<float> r)
{
    // Axis labels are placed around graph-edge ticks; without clamping the
    // first/last labels are cut by the component bounds ("20k" -> "20!").
    auto clampLabel = [this] (juce::Rectangle<float> label)
    {
        const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
        label.setX (juce::jlimit (bounds.getX(),
                                  juce::jmax (bounds.getX(), bounds.getRight() - label.getWidth()),
                                  label.getX()));
        return label;
    };

    // Frequency axis.
    static const double majorFreqs[] { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 };
    static const double minorFreqs[] { 30.0, 40.0, 70.0, 300.0, 700.0, 3000.0, 7000.0 };

    for (double f : minorFreqs)
    {
        const float x = freqToX (f, r);
        g.setColour (theme::lineFaint);
        g.drawVerticalLine (juce::roundToInt (x), r.getY(), r.getBottom());
    }

    for (double f : majorFreqs)
    {
        const float x = freqToX (f, r);
        g.setColour (theme::line.withAlpha (0.9f));
        g.drawVerticalLine (juce::roundToInt (x), r.getY(), r.getBottom());

        if (! mini)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::monoFont (9.0f));
            auto label = clampLabel (r.withY (r.getBottom() + 2.0f).withHeight (14.0f)
                                      .withX (x - 16.0f).withWidth (32.0f));
            g.drawText (freqAxisLabel (f), label, juce::Justification::centredTop, false);
        }
        else if (f == 20.0 || f == 100.0 || f == 1000.0 || f == 10000.0 || f == 20000.0)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::monoFont (8.0f));
            auto label = clampLabel (r.withY (r.getBottom() - 12.0f).withHeight (11.0f)
                                      .withX (x - 13.0f).withWidth (26.0f));
            g.drawText (freqAxisLabel (f), label, juce::Justification::centredBottom, false);
        }
    }

    // dB axis: pick a step that always labels 0 dB (5 dB for +-15, 4 for +-12).
    const float dbSpan = maxDb - minDb;
    const int step = dbSpan > 34.0f ? 10 : (dbSpan > 26.0f ? 5 : (dbSpan > 14.0f ? 4 : 2));
    for (int db = (int) std::ceil (minDb); db <= (int) std::floor (maxDb); db += step)
    {
        const float y = dbToY ((float) db, r);
        g.setColour (db == 0 ? theme::lineStrong : theme::lineFaint.withAlpha (0.8f));
        g.drawHorizontalLine (juce::roundToInt (y), r.getX(), r.getRight());

        if (! mini)
        {
            g.setColour (db == 0 ? theme::silverDim : theme::textFaint);
            g.setFont (theme::monoFont (9.0f));
            g.drawText ((db > 0 ? "+" : "") + juce::String (db),
                        clampLabel (juce::Rectangle<float> (r.getX() - 30.0f, y - 6.0f, 26.0f, 12.0f)),
                        juce::Justification::centredRight, false);
        }
        else if (db == 0 || db == (int) std::floor (maxDb))
        {
            g.setColour (theme::silverDim.withAlpha (0.8f));
            g.setFont (theme::monoFont (7.0f));
            g.drawText ((db > 0 ? "+" : "") + juce::String (db),
                        juce::Rectangle<float> (r.getX() + 2.0f, y - 8.0f, 22.0f, 9.0f),
                        juce::Justification::centredLeft, false);
        }
    }

    g.setColour (theme::line);
    g.drawRect (r, 1.0f);
}

void EqCurveComponent::paintCurve (juce::Graphics& g, juce::Rectangle<float> r)
{
    if (responseDb.size() != responseFreqs.size() || responseDb.empty())
        return;

    juce::Path curve;
    for (size_t i = 0; i < responseDb.size(); ++i)
    {
        const float x = freqToX (responseFreqs[i], r);
        const float y = dbToY (juce::jlimit (minDb, maxDb, responseDb[i]), r);
        if (i == 0)
            curve.startNewSubPath (x, y);
        else
            curve.lineTo (x, y);
    }

    const bool bypassed = params.bypassed || params.eqBypass;
    const bool active = params.hasActiveProcessing (true);
    const auto curveColour = bypassed ? theme::silverDark : (active ? theme::gold : theme::silver);

    if (! bypassed)
    {
        juce::Path fill (curve);
        fill.lineTo (r.getRight(), dbToY (0.0f, r));
        fill.lineTo (r.getX(), dbToY (0.0f, r));
        fill.closeSubPath();
        g.setColour (curveColour.withAlpha (active ? 0.10f : 0.045f));
        g.fillPath (fill);
    }

    if (bypassed)
    {
        juce::Path dashed;
        const float dashes[] { 5.0f, 4.0f };
        juce::PathStrokeType (1.4f).createDashedStroke (dashed, curve, dashes, 2);
        g.setColour (theme::silverDark);
        g.fillPath (dashed);
        return;
    }

    // Soft glow then core.
    g.setColour (curveColour.withAlpha (0.10f));
    g.strokePath (curve, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    g.setColour (curveColour.withAlpha (0.22f));
    g.strokePath (curve, juce::PathStrokeType (3.4f, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    g.setColour (curveColour);
    g.strokePath (curve, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
}

void EqCurveComponent::paintNodes (juce::Graphics& g, juce::Rectangle<float> r)
{
    if (nodes.empty())
    {
        if (! mini && editable)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::uiFont (11.0f));
            g.drawText ("No active EQ bands", r.toNearestInt(), juce::Justification::centred, false);
        }
        return;
    }

    const bool bypassed = params.bypassed || params.eqBypass;
    const float radius = mini ? 4.0f : 5.0f;

    for (int i = 0; i < (int) nodes.size(); ++i)
    {
        const auto& n = nodes[(size_t) i];
        const float x = freqToX (n.freqHz, r);
        const float y = dbToY (juce::jlimit (minDb, maxDb, n.gainDb), r);
        const juce::Point<float> pos (x, y);
        const bool hot = (i == hoverNode || i == dragNode);
        const auto colour = bypassed || ! n.active ? theme::silverDark : theme::gold;

        if (hot)
        {
            g.setColour (theme::gold.withAlpha (0.16f));
            g.fillEllipse (juce::Rectangle<float> (radius * 6.0f, radius * 6.0f).withCentre (pos));
            g.setColour (theme::gold.withAlpha (0.55f));
            g.drawEllipse (juce::Rectangle<float> (radius * 4.2f, radius * 4.2f).withCentre (pos), 1.0f);
        }

        // Target-vs-current connector (the chain smooths parameter changes).
        if (! bypassed && n.active)
        {
            const float live = responseAt (n.freqHz);
            if (std::abs (live - n.gainDb) > 0.35f)
            {
                const float liveY = dbToY (juce::jlimit (minDb, maxDb, live), r);
                juce::Path dash;
                dash.startNewSubPath (x, y);
                dash.lineTo (x, liveY);
                const float dashes[] { 2.5f, 2.5f };
                juce::Path dashed;
                juce::PathStrokeType (1.0f).createDashedStroke (dashed, dash, dashes, 2);
                g.setColour (theme::silverDim.withAlpha (0.65f));
                g.fillPath (dashed);
                g.setColour (theme::silverDim);
                g.fillEllipse (juce::Rectangle<float> (3.2f, 3.2f).withCentre ({ x, liveY }));
            }
        }

        switch (n.kind)
        {
            case NodeKind::graphic:
            {
                g.setColour (colour);
                g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (pos));
                g.setColour (theme::background.withAlpha (0.65f));
                g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (pos), 0.9f);
                break;
            }

            case NodeKind::parametric:
            {
                juce::Path diamond;
                const float d = radius * 1.35f;
                diamond.startNewSubPath (x, y - d);
                diamond.lineTo (x + d, y);
                diamond.lineTo (x, y + d);
                diamond.lineTo (x - d, y);
                diamond.closeSubPath();
                g.setColour (colour);
                g.fillPath (diamond);
                g.setColour (theme::background.withAlpha (0.6f));
                g.strokePath (diamond, juce::PathStrokeType (1.0f));
                break;
            }

            case NodeKind::bassShelf:
            case NodeKind::bassSub:
            case NodeKind::bassMidBass:
            case NodeKind::bassPunch:
            {
                const bool glow = n.active && ! bypassed;
                if (glow)
                {
                    g.setColour (theme::gold.withAlpha (0.18f));
                    g.fillEllipse (juce::Rectangle<float> (radius * 4.6f, radius * 4.6f).withCentre (pos));
                }
                g.setColour (glow ? theme::goldBright : colour);
                g.fillEllipse (juce::Rectangle<float> (radius * 2.2f, radius * 2.2f).withCentre (pos));
                g.setColour (theme::background.withAlpha (0.7f));
                g.drawEllipse (juce::Rectangle<float> (radius * 2.2f, radius * 2.2f).withCentre (pos), 1.0f);
                g.setColour (glow ? theme::goldBright : colour);
                g.drawEllipse (juce::Rectangle<float> (radius * 3.2f, radius * 3.2f).withCentre (pos), 1.0f);
                break;
            }
        }

        if (! mini && n.tag.isNotEmpty())
        {
            g.setColour (colour);
            g.setFont (theme::monoFont (8.0f));
            g.drawText (n.tag, juce::Rectangle<float> (x - 18.0f, y - 21.0f, 36.0f, 10.0f),
                        juce::Justification::centred, false);
        }
    }
}

void EqCurveComponent::paintReadout (juce::Graphics& g, juce::Rectangle<float> r)
{
    if (mini || ! cursorValid)
        return;

    const bool overNode = hoverNode >= 0 && hoverNode < (int) nodes.size();
    const Node* n = overNode ? &nodes[(size_t) hoverNode] : nullptr;

    const double freq = n != nullptr ? n->freqHz : xToFreq (cursor.x, r);
    const float db = n != nullptr ? n->gainDb : yToDb (cursor.y, r);

    // Crosshair.
    g.setColour (theme::silverDim.withAlpha (n != nullptr ? 0.30f : 0.16f));
    g.drawVerticalLine (juce::roundToInt (cursor.x), r.getY(), r.getBottom());
    g.drawHorizontalLine (juce::roundToInt (cursor.y), r.getX(), r.getRight());

    juce::String text;
    text << freqText (freq) << "   " << dbText (db, 1) << " dB";
    if (n != nullptr && (n->kind == NodeKind::parametric || n->kind == NodeKind::bassShelf
                         || n->kind == NodeKind::bassSub || n->kind == NodeKind::bassMidBass
                         || n->kind == NodeKind::bassPunch))
    {
        double q = 0.0;
        if (n->kind == NodeKind::parametric && n->index < ParametricEqParams::kNumBands)
            q = params.parametric.bands[(size_t) n->index].q;
        else if (n->kind == NodeKind::bassShelf)   q = params.bass.shelfQ;
        else if (n->kind == NodeKind::bassSub)     q = params.bass.subQ;
        else if (n->kind == NodeKind::bassMidBass) q = params.bass.midBassQ;
        else if (n->kind == NodeKind::bassPunch)   q = params.bass.punchQ;

        if (q > 0.0)
            text << "   Q " << qText (q);
    }

    const auto font = theme::monoFont (10.5f);
    g.setFont (font);
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (font, text, 0.0f, 0.0f);
    const float textWidth = glyphs.getBoundingBox (0, -1, true).getWidth();
    const float width = juce::jlimit (150.0f, 260.0f, textWidth + 18.0f);
    auto pill = juce::Rectangle<float> (r.getRight() - 6.0f - width, r.getY() + 6.0f, width, 22.0f);
    g.setColour (theme::panelRaised.withAlpha (0.94f));
    g.fillRoundedRectangle (pill, 3.0f);
    g.setColour (theme::lineStrong);
    g.drawRoundedRectangle (pill, 3.0f, 1.0f);
    g.setColour (overNode ? theme::goldBright : theme::silver);
    g.drawText (text, pill.reduced (8.0f, 0.0f), juce::Justification::centredLeft, false);
}

} // namespace sp
