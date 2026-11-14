/*
    Snake Puppy Reference Player
    Interactive combined EQ response graph.

    Draws the live magnitude response reported by the DSP chain
    (DspChain::getMagnitudeResponseDb over a log frequency axis, +/-15 dB) and
    overlays draggable target nodes for the active graphic, parametric and bass
    bands. While the chain is smoothing a parameter change the node (target)
    and the live curve separate; a dashed connector shows the difference.

    All edits go through AppContext::applyUserDspParams, so the displayed curve
    always reflects what the engine is actually processing.

    Threading: message thread only. Polls AppContext with a timer; no audio
    thread access.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/DspUiStyle.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>

namespace sp
{

class EqCurveComponent : public juce::Component,
                         private juce::Timer
{
public:
    /** `mini` draws a compact graph (no axis labels, smaller margins) for the
        main-window right rail. */
    explicit EqCurveComponent (AppContext&, bool mini = false);
    ~EqCurveComponent() override;

    void setEditable (bool shouldBeEditable);
    bool isEditable() const noexcept { return editable; }

    /** Choose which node families are shown and draggable. */
    void setEditableFamilies (bool graphicBands, bool parametricBands, bool bassBands);
    void setShowGraphicBands (bool);
    void setShowParametricBands (bool);
    void setShowBassBands (bool);

    void setCaption (juce::String text);
    void setMini (bool shouldBeMini);
    void setDbRange (float minDb, float maxDb);

    /** Re-read AppContext and repaint immediately (message thread). */
    void refreshNow();

    /** Called after a node drag changed parameters. */
    std::function<void()> onUserEdited;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    enum class NodeKind { graphic, parametric, bassShelf, bassSub, bassMidBass, bassPunch };

    struct Node
    {
        NodeKind kind = NodeKind::graphic;
        int index = 0;
        double freqHz = 1000.0;
        float gainDb = 0.0f;
        bool active = true;      // enabled and not bypassed
        bool highlighted = false;
        juce::String tag;        // "HPF"/"LPF" for pass filters
    };

    void timerCallback() override;
    void rebuildNodes();
    void computeResponse();
    void refreshIfChanged (bool force);

    juce::Rectangle<float> graphArea() const;
    float freqToX (double hz, juce::Rectangle<float> r) const;
    double xToFreq (float x, juce::Rectangle<float> r) const;
    float dbToY (float db, juce::Rectangle<float> r) const;
    float yToDb (float y, juce::Rectangle<float> r) const;
    int findNodeAt (juce::Point<float> p, juce::Rectangle<float> r) const;
    void applyNodeDrag (const Node& node, double freqHz, float gainDb);
    float responseAt (double hz) const;

    void paintGrid (juce::Graphics&, juce::Rectangle<float>);
    void paintCurve (juce::Graphics&, juce::Rectangle<float>);
    void paintNodes (juce::Graphics&, juce::Rectangle<float>);
    void paintReadout (juce::Graphics&, juce::Rectangle<float>);

    AppContext& ctx;
    bool mini = false;
    bool editable = true;
    bool showGraphic = true;
    bool showParametric = true;
    bool showBass = true;

    float minDb = -15.0f;
    float maxDb = 15.0f;
    juce::String caption { "COMBINED RESPONSE" };

    DspParams params;
    std::vector<Node> nodes;
    std::vector<double> responseFreqs;
    std::vector<float> responseDb;

    std::uint64_t lastFingerprint = 0;
    juce::int64 lastChangeMs = 0;
    juce::int64 lastTickMs = 0;

    int dragNode = -1;
    int hoverNode = -1;
    juce::Point<float> dragStartMouse;
    float dragStartGain = 0.0f;
    double dragStartFreq = 1000.0;
    bool cursorValid = false;
    juce::Point<float> cursor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqCurveComponent)
};

} // namespace sp
