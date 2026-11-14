/*
    Snake Puppy Reference Player - DSP Studio window implementation.

    Contains the workspace tabs plus the stereo, filters/convolution,
    gain/limiter, A/B preset and device-profile panels, and the profile/preset
    persistence plumbing.
    Message thread only.
*/
#include "UI/DspStudioWindow.h"

#include "Core/Serialization.h"
#include "UI/BassControlPanel.h"
#include "UI/DspChainPanel.h"
#include "UI/GraphicEqEditor.h"
#include "UI/MetersPanel.h"
#include "UI/ParametricEqEditor.h"
#include "UI/ProfilePanel.h"
#include "UI/SignalPathPanel.h"
#include "UI/SpectrogramComponent.h"
#include "UI/SpectrumComponent.h"

#include <cmath>

namespace sp
{

namespace
{
using ui::configureLabel;
using ui::dbText;
using ui::hzText;
using ui::qText;

//==============================================================================
juce::String promptForName (juce::Component* associated, const juce::String& title,
                            const juce::String& initialText)
{
    juce::AlertWindow window (title, "Enter a name:", juce::MessageBoxIconType::QuestionIcon,
                              associated);
    window.addTextEditor ("name", initialText);
    window.addButton ("SAVE", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window.addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    if (window.runModalLoop() == 1)
        return window.getTextEditor ("name") != nullptr ? window.getTextEditor ("name")->getText().trim()
                                                        : juce::String();
    return {};
}

void configureSpKnob (SpKnob& knob, double minValue, double maxValue, double midpoint,
                      double doubleClickValue, std::function<juce::String (double)> formatter)
{
    knob.setRange (minValue, maxValue, 0.0);
    if (midpoint > minValue && midpoint < maxValue)
        knob.setSkewFactorFromMidPoint (midpoint);
    knob.setDoubleClickReturnValue (true, doubleClickValue);
    knob.setMouseDragSensitivity (180);
    knob.textFromValueFunction = std::move (formatter);
    knob.setValue (doubleClickValue, juce::dontSendNotification);
}

void applyStereo (AppContext& ctx, const std::function<void (StereoParams&)>& mutate)
{
    auto p = ctx.getCurrentDspParams();
    mutate (p.stereo);
    ctx.applyUserDspParams (p);
}

void applyFilters (AppContext& ctx, const std::function<void (FilterParams&)>& mutate)
{
    auto p = ctx.getCurrentDspParams();
    mutate (p.filters);
    ctx.applyUserDspParams (p);
}

juce::File presetDir (AppContext& ctx)
{
    auto dir = ctx.paths().presetsDir;
    if (! dir.isDirectory())
        dir.createDirectory();
    return dir;
}

juce::String presetFileName (const juce::String& name)
{
    juce::String safe = name.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_");
    safe = safe.trim();
    if (safe.isEmpty())
        safe = "preset";
    return safe.replaceCharacter (' ', '_') + ".dsp.json";
}

//==============================================================================
/** Stereo image / crossfeed controls. */
class StereoPanel : public juce::Component,
                    private juce::Timer
{
public:
    explicit StereoPanel (AppContext& context) : ctx (context)
    {
        setOpaque (true);

        crossfeedToggle.setButtonText ("ENABLE");
        crossfeedToggle.onClick = [this]
        {
            const bool on = crossfeedToggle.getToggleState();
            applyStereo (ctx, [on] (StereoParams& s) { s.crossfeedEnabled = on; });
        };
        addAndMakeVisible (crossfeedToggle);

        configureSpKnob (amountKnob, -12.0, -3.0, -6.0, -6.0,
                         [] (double v) { return dbText (v, 1); });
        amountKnob.setLabelText ("AMOUNT");
        amountKnob.onValueChange = [this]
        {
            const float v = (float) amountKnob.getValue();
            applyStereo (ctx, [v] (StereoParams& s) { s.crossfeedAmountDb = v; });
        };
        addAndMakeVisible (amountKnob);

        configureSpKnob (cutoffKnob, 200.0, 2000.0, 700.0, 700.0,
                         [] (double v) { return juce::String ((int) v) + " Hz"; });
        cutoffKnob.setLabelText ("CUTOFF");
        cutoffKnob.onValueChange = [this]
        {
            const double v = cutoffKnob.getValue();
            applyStereo (ctx, [v] (StereoParams& s) { s.crossfeedCutoffHz = v; });
        };
        addAndMakeVisible (cutoffKnob);

        configureSpKnob (delayKnob, 0.0, 500.0, 250.0, 250.0,
                         [] (double v) { return juce::String ((int) v) + " us"; });
        delayKnob.setLabelText ("DELAY");
        delayKnob.onValueChange = [this]
        {
            const float v = (float) delayKnob.getValue();
            applyStereo (ctx, [v] (StereoParams& s) { s.crossfeedDelayUs = v; });
        };
        addAndMakeVisible (delayKnob);

        configureSpKnob (widthKnob, 0.0, 200.0, 100.0, 100.0,
                         [] (double v) { return juce::String ((int) std::round (v)) + " %"; });
        widthKnob.setLabelText ("WIDTH");
        widthKnob.onValueChange = [this]
        {
            const float v = (float) widthKnob.getValue();
            applyStereo (ctx, [v] (StereoParams& s) { s.widthPercent = v; });
        };
        addAndMakeVisible (widthKnob);

        configureSpKnob (balanceKnob, -1.0, 1.0, 0.0, 0.0,
                         [] (double v)
                         {
                             if (std::abs (v) < 0.01)
                                 return juce::String ("C");
                             return juce::String (v < 0.0 ? "L " : "R ") + juce::String (std::abs (v) * 100.0, 0);
                         });
        balanceKnob.setLabelText ("BALANCE");
        balanceKnob.onValueChange = [this]
        {
            const float v = (float) balanceKnob.getValue();
            applyStereo (ctx, [v] (StereoParams& s) { s.balance = v; });
        };
        addAndMakeVisible (balanceKnob);

        configureSpKnob (monoBassKnob, 40.0, 300.0, 120.0, 120.0,
                         [] (double v) { return juce::String ((int) v) + " Hz"; });
        monoBassKnob.setLabelText ("MONO BELOW");
        monoBassKnob.onValueChange = [this]
        {
            const double v = monoBassKnob.getValue();
            applyStereo (ctx, [v] (StereoParams& s) { s.monoBassHz = v; });
        };
        addAndMakeVisible (monoBassKnob);

        monoBassToggle.setButtonText ("MONO BASS");
        monoBassToggle.onClick = [this]
        {
            const bool on = monoBassToggle.getToggleState();
            applyStereo (ctx, [on] (StereoParams& s) { s.monoBassEnabled = on; });
        };
        addAndMakeVisible (monoBassToggle);

        protectMonoToggle.setButtonText ("PROTECT MONO COMPATIBILITY");
        protectMonoToggle.onClick = [this]
        {
            const bool on = protectMonoToggle.getToggleState();
            applyStereo (ctx, [on] (StereoParams& s) { s.protectMono = on; });
        };
        addAndMakeVisible (protectMonoToggle);

        infoLabel.setText ("Crossfeed is for headphones; width and balance are real mid/side and gain "
                           "operations. Mono protection limits extreme width on mono playback.",
                           juce::dontSendNotification);
        configureLabel (infoLabel, 10.0f, theme::textFaint);
        infoLabel.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (infoLabel);

        setSize (900, 560);
        sync();
        startTimerHz (4);
    }

    ~StereoPanel() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::background);
        auto top = getLocalBounds().reduced (14, 8).removeFromTop (22);
        theme::drawSectionHeader (g, top, "STEREO / CROSSFEED", "HEADPHONE AND IMAGE CONTROL");

        theme::fillPanel (g, crossfeedCard);
        auto titleA = crossfeedCard.reduced (10, 5).removeFromTop (16);
        theme::drawSectionHeader (g, titleA, "CROSSFEED", "AIWA / BS2B STYLE");

        theme::fillPanel (g, imageCard);
        auto titleB = imageCard.reduced (10, 5).removeFromTop (16);
        theme::drawSectionHeader (g, titleB, "STEREO IMAGE", "WIDTH / BALANCE / MONO BASS");

        g.setColour (theme::textFaint);
        g.setFont (theme::uiFont (10.0f));
        g.drawFittedText (infoLabel.getText(), infoArea, juce::Justification::topLeft, 2);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 8);
        r.removeFromTop (22);
        r.removeFromTop (8);

        const int cardsHeight = juce::jmin (322, juce::jmax (230, r.getHeight() * 47 / 100));
        auto cards = r.removeFromTop (cardsHeight);
        const int cardWidth = juce::jmax (300, (cards.getWidth() - 10) / 2);
        crossfeedCard = cards.removeFromLeft (cardWidth);
        cards.removeFromLeft (10);
        imageCard = cards.removeFromLeft (cardWidth);

        r.removeFromTop (10);
        infoArea = r.removeFromTop (juce::jmin (36, r.getHeight()));

        {
            auto inner = crossfeedCard.reduced (12, 8);
            inner.removeFromTop (20);
            crossfeedToggle.setBounds (inner.removeFromTop (22).removeFromLeft (juce::jmin (150, inner.getWidth())));
            inner.removeFromTop (6);
            inner.removeFromTop (26);
            auto row = inner.removeFromTop (juce::jmin (140, inner.getHeight()));
            const int kw = juce::jmin (140, row.getWidth() / 3);
            amountKnob.setBounds (row.removeFromLeft (kw).reduced (6));
            cutoffKnob.setBounds (row.removeFromLeft (kw).reduced (6));
            delayKnob.setBounds (row.removeFromLeft (kw).reduced (6));
        }
        {
            auto inner = imageCard.reduced (12, 8);
            inner.removeFromTop (20);
            inner.removeFromTop (26);
            auto row = inner.removeFromTop (juce::jmin (140, inner.getHeight()));
            const int kw = juce::jmin (140, row.getWidth() / 3);
            widthKnob.setBounds (row.removeFromLeft (kw).reduced (6));
            balanceKnob.setBounds (row.removeFromLeft (kw).reduced (6));
            monoBassKnob.setBounds (row.removeFromLeft (kw).reduced (6));
            inner.removeFromTop (8);
            monoBassToggle.setBounds (inner.removeFromTop (22).removeFromLeft (juce::jmin (150, inner.getWidth())));
            inner.removeFromTop (4);
            protectMonoToggle.setBounds (inner.removeFromTop (22).removeFromLeft (juce::jmin (250, inner.getWidth())));
        }
    }

private:
    void timerCallback() override { sync(); }

    void sync()
    {
        const auto p = ctx.getCurrentDspParams();
        const auto& s = p.stereo;
        crossfeedToggle.setToggleState (s.crossfeedEnabled, juce::dontSendNotification);
        amountKnob.setValue (s.crossfeedAmountDb, juce::dontSendNotification);
        cutoffKnob.setValue (s.crossfeedCutoffHz, juce::dontSendNotification);
        delayKnob.setValue (s.crossfeedDelayUs, juce::dontSendNotification);
        widthKnob.setValue (s.widthPercent, juce::dontSendNotification);
        balanceKnob.setValue (s.balance, juce::dontSendNotification);
        monoBassKnob.setValue (s.monoBassHz, juce::dontSendNotification);
        monoBassToggle.setToggleState (s.monoBassEnabled, juce::dontSendNotification);
        protectMonoToggle.setToggleState (s.protectMono, juce::dontSendNotification);
        monoBassKnob.setEnabled (s.monoBassEnabled);
    }

    AppContext& ctx;
    SpKnob amountKnob, cutoffKnob, delayKnob, widthKnob, balanceKnob, monoBassKnob;
    juce::ToggleButton crossfeedToggle, monoBassToggle, protectMonoToggle;
    juce::Label infoLabel;
    juce::Rectangle<int> crossfeedCard, imageCard, infoArea;
};

//==============================================================================
/** High/low pass filters and convolution. */
class FiltersPanel : public juce::Component,
                     private juce::Timer
{
public:
    explicit FiltersPanel (AppContext& context) : ctx (context)
    {
        setOpaque (true);

        highPassToggle.setButtonText ("ENABLE");
        highPassToggle.onClick = [this]
        {
            const bool on = highPassToggle.getToggleState();
            if (on && highPassKnob.getValue() < 35.0)
                highPassKnob.setValue (40.0, juce::sendNotificationSync);
            applyFilters (ctx, [on] (FilterParams& f) { f.highPassEnabled = on; });
            ctx.setStatusMessage (on ? "High-pass enabled at " + juce::String ((int) highPassKnob.getValue())
                                           + " Hz - the low end is being cut."
                                     : "High-pass disabled.", 6000);
        };
        addAndMakeVisible (highPassToggle);

        configureSpKnob (highPassKnob, 10.0, 500.0, 80.0, 20.0,
                         [] (double v) { return juce::String ((int) std::round (v)) + " Hz"; });
        highPassKnob.setLabelText ("FREQUENCY");
        highPassKnob.onValueChange = [this]
        {
            const double v = highPassKnob.getValue();
            applyFilters (ctx, [v] (FilterParams& f) { f.highPassHz = v; });
        };
        addAndMakeVisible (highPassKnob);

        highPassSlope.addItem ("12 dB/oct", 12);
        highPassSlope.addItem ("24 dB/oct", 24);
        highPassSlope.addItem ("48 dB/oct", 48);
        highPassSlope.setSelectedId (12, juce::dontSendNotification);
        highPassSlope.onChange = [this]
        {
            const int slope = highPassSlope.getSelectedId();
            applyFilters (ctx, [slope] (FilterParams& f) { f.highPassSlopeDbPerOct = slope; });
        };
        addAndMakeVisible (highPassSlope);

        lowPassToggle.setButtonText ("ENABLE");
        lowPassToggle.onClick = [this]
        {
            const bool on = lowPassToggle.getToggleState();
            if (on && lowPassKnob.getValue() > 19500.0)
                lowPassKnob.setValue (18000.0, juce::sendNotificationSync);
            applyFilters (ctx, [on] (FilterParams& f) { f.lowPassEnabled = on; });
            ctx.setStatusMessage (on ? "Low-pass enabled at " + juce::String ((int) (lowPassKnob.getValue() / 1000.0))
                                           + " kHz - the top end is being cut."
                                     : "Low-pass disabled.", 6000);
        };
        addAndMakeVisible (lowPassToggle);

        configureSpKnob (lowPassKnob, 2000.0, 20000.0, 10000.0, 20000.0,
                         [] (double v) { return v >= 1000.0 ? juce::String (v / 1000.0, 0) + "k"
                                                             : juce::String ((int) std::round (v)); });
        lowPassKnob.setLabelText ("FREQUENCY");
        lowPassKnob.onValueChange = [this]
        {
            const double v = lowPassKnob.getValue();
            applyFilters (ctx, [v] (FilterParams& f) { f.lowPassHz = v; });
        };
        addAndMakeVisible (lowPassKnob);

        lowPassSlope.addItem ("12 dB/oct", 12);
        lowPassSlope.addItem ("24 dB/oct", 24);
        lowPassSlope.addItem ("48 dB/oct", 48);
        lowPassSlope.setSelectedId (12, juce::dontSendNotification);
        lowPassSlope.onChange = [this]
        {
            const int slope = lowPassSlope.getSelectedId();
            applyFilters (ctx, [slope] (FilterParams& f) { f.lowPassSlopeDbPerOct = slope; });
        };
        addAndMakeVisible (lowPassSlope);

        convolutionToggle.setButtonText ("ENABLE");
        convolutionToggle.onClick = [this]
        {
            const bool on = convolutionToggle.getToggleState();
            if (on && ! ctx.engine().dspChain().getStatus().convolutionLoaded)
            {
                convolutionToggle.setToggleState (false, juce::dontSendNotification);
                ctx.setStatusMessage ("Convolution needs an impulse response: use LOAD IR first.", 8000);
                return;
            }
            auto p = ctx.getCurrentDspParams();
            p.convolution.enabled = on;
            ctx.applyUserDspParams (p);
            ctx.setStatusMessage (on ? "Convolution enabled." : "Convolution disabled.", 6000);
        };
        addAndMakeVisible (convolutionToggle);

        loadIrButton.onClick = [this] { chooseImpulseResponse(); };
        addAndMakeVisible (loadIrButton);

        clearIrButton.onClick = [this]
        {
            ctx.engine().dspChain().clearImpulseResponse();
            auto p = ctx.getCurrentDspParams();
            p.convolution.enabled = false;
            p.convolution.irPath = {};
            p.convolution.irName = {};
            ctx.applyUserDspParams (p);
            irStatusLabel.setText ("No impulse response loaded.", juce::dontSendNotification);
        };
        addAndMakeVisible (clearIrButton);

        configureSpKnob (mixKnob, 0.0, 100.0, 100.0, 100.0,
                         [] (double v) { return juce::String ((int) std::round (v)) + " %"; });
        mixKnob.setLabelText ("MIX");
        mixKnob.onValueChange = [this]
        {
            const float v = (float) mixKnob.getValue();
            auto p = ctx.getCurrentDspParams();
            p.convolution.mixPercent = v;
            ctx.applyUserDspParams (p);
        };
        addAndMakeVisible (mixKnob);

        trueStereoToggle.setButtonText ("TRUE STEREO IR");
        trueStereoToggle.onClick = [this]
        {
            const bool on = trueStereoToggle.getToggleState();
            auto p = ctx.getCurrentDspParams();
            p.convolution.trueStereo = on;
            ctx.applyUserDspParams (p);
        };
        addAndMakeVisible (trueStereoToggle);

        configureLabel (irStatusLabel, 10.0f, theme::textDim);
        irStatusLabel.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (irStatusLabel);

        setSize (900, 620);
        sync();
        startTimerHz (4);
    }

    ~FiltersPanel() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::background);
        auto top = getLocalBounds().reduced (14, 8).removeFromTop (22);
        theme::drawSectionHeader (g, top, "FILTERS / CONVOLUTION", "SUB-SONIC, ULTRASONIC AND ROOM CORRECTION");

        theme::fillPanel (g, hpCard);
        theme::drawSectionHeader (g, hpCard.reduced (10, 5).removeFromTop (16), "HIGH PASS", "PROTECT SUBSONIC CONTENT");
        theme::fillPanel (g, lpCard);
        theme::drawSectionHeader (g, lpCard.reduced (10, 5).removeFromTop (16), "LOW PASS", "TAME ULTRASONIC CONTENT");
        theme::fillPanel (g, convCard);
        theme::drawSectionHeader (g, convCard.reduced (10, 5).removeFromTop (16), "CONVOLUTION",
                                  "IMPULSE RESPONSE / ROOM CORRECTION");

        g.setColour (theme::textFaint);
        g.setFont (theme::uiFont (10.0f));
        g.drawFittedText ("Filters run before convolution in the default chain. Both are real biquads; "
                          "convolution requires a compatible mono or true-stereo impulse response.",
                          noteArea, juce::Justification::topLeft, 2);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 8);
        r.removeFromTop (22);
        r.removeFromTop (8);

        auto cards = r.removeFromTop (juce::jmin (300, juce::jmax (240, r.getHeight() * 55 / 100)));
        auto topRow = cards;
        const int cardWidth = juce::jmax (260, (topRow.getWidth() - 20) / 3);
        hpCard = topRow.removeFromLeft (cardWidth);
        topRow.removeFromLeft (10);
        lpCard = topRow.removeFromLeft (cardWidth);
        topRow.removeFromLeft (10);
        convCard = topRow.removeFromLeft (cardWidth);

        r.removeFromTop (12);
        noteArea = r.removeFromTop (juce::jmin (40, r.getHeight()));

        {
            auto inner = hpCard.reduced (12, 8);
            inner.removeFromTop (20);
            highPassToggle.setBounds (inner.removeFromTop (22).removeFromLeft (juce::jmin (140, inner.getWidth())));
            inner.removeFromTop (4);
            inner.removeFromTop (18);
            auto row = inner.removeFromTop (juce::jmin (132, inner.getHeight()));
            highPassKnob.setBounds (row.removeFromLeft (juce::jmin (124, row.getWidth() / 2)).reduced (6));
            highPassSlope.setBounds (row.removeFromTop (26).withWidth (juce::jmin (136, row.getWidth())));
        }
        {
            auto inner = lpCard.reduced (12, 8);
            inner.removeFromTop (20);
            lowPassToggle.setBounds (inner.removeFromTop (22).removeFromLeft (juce::jmin (140, inner.getWidth())));
            inner.removeFromTop (4);
            inner.removeFromTop (18);
            auto row = inner.removeFromTop (juce::jmin (132, inner.getHeight()));
            lowPassKnob.setBounds (row.removeFromLeft (juce::jmin (124, row.getWidth() / 2)).reduced (6));
            lowPassSlope.setBounds (row.removeFromTop (26).withWidth (juce::jmin (136, row.getWidth())));
        }
        {
            auto inner = convCard.reduced (12, 8);
            inner.removeFromTop (20);
            convolutionToggle.setBounds (inner.removeFromTop (22).removeFromLeft (juce::jmin (150, inner.getWidth())));
            inner.removeFromTop (4);
            auto row = inner.removeFromTop (26);
            loadIrButton.setBounds (row.removeFromLeft (juce::jmin (110, row.getWidth() / 2)));
            row.removeFromLeft (6);
            clearIrButton.setBounds (row.removeFromLeft (juce::jmin (86, row.getWidth())));
            inner.removeFromTop (4);
            trueStereoToggle.setBounds (inner.removeFromTop (22).removeFromLeft (juce::jmin (160, inner.getWidth())));
            inner.removeFromTop (2);
            mixKnob.setBounds (inner.removeFromTop (juce::jmin (72, juce::jmax (0, inner.getHeight() - 16)))
                                    .removeFromLeft (juce::jmin (88, inner.getWidth())));
            irStatusLabel.setBounds (inner.removeFromTop (juce::jmin (28, juce::jmax (0, inner.getHeight()))));
        }
    }

private:
    void timerCallback() override { sync(); }

    void sync()
    {
        const auto p = ctx.getCurrentDspParams();
        const auto& f = p.filters;
        highPassToggle.setToggleState (f.highPassEnabled, juce::dontSendNotification);
        highPassKnob.setValue (f.highPassHz, juce::dontSendNotification);
        highPassSlope.setSelectedId (f.highPassSlopeDbPerOct, juce::dontSendNotification);
        lowPassToggle.setToggleState (f.lowPassEnabled, juce::dontSendNotification);
        lowPassKnob.setValue (f.lowPassHz, juce::dontSendNotification);
        lowPassSlope.setSelectedId (f.lowPassSlopeDbPerOct, juce::dontSendNotification);
        convolutionToggle.setToggleState (p.convolution.enabled, juce::dontSendNotification);
        mixKnob.setValue (p.convolution.mixPercent, juce::dontSendNotification);
        trueStereoToggle.setToggleState (p.convolution.trueStereo, juce::dontSendNotification);

        const auto dsp = ctx.engine().getStatus().dspStatus;
        juce::String text;
        if (dsp.convolutionLoaded)
            text = "Loaded: " + (p.convolution.irName.isNotEmpty() ? p.convolution.irName
                                                                   : juce::File (p.convolution.irPath).getFileName());
        else if (p.convolution.irPath.isNotEmpty())
            text = "IR file set but not loaded: " + dsp.convolutionError;
        else
            text = "No impulse response loaded (convolution unavailable until an IR is loaded).";

        if (text != irStatusLabel.getText())
            irStatusLabel.setText (text, juce::dontSendNotification);
    }

    void chooseImpulseResponse()
    {
        auto chooser = std::make_shared<juce::FileChooser> ("Load impulse response",
                                                           juce::File(), "*.wav;*.aif;*.aiff;*.flac");
        juce::Component::SafePointer<FiltersPanel> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe, chooser] (const juce::FileChooser& fc)
        {
            auto* panel = safe.getComponent();
            if (panel == nullptr)
                return;

            const auto file = fc.getResult();
            if (file == juce::File())
                return;

            juce::String error;
            panel->ctx.engine().dspChain().loadImpulseResponse (file, error);
            if (error.isNotEmpty())
            {
                panel->irStatusLabel.setText ("Could not load IR: " + error, juce::dontSendNotification);
                return;
            }

            auto p = panel->ctx.getCurrentDspParams();
            p.convolution.enabled = true;
            p.convolution.irPath = file.getFullPathName();
            p.convolution.irName = file.getFileNameWithoutExtension();
            panel->ctx.applyUserDspParams (p);
            panel->irStatusLabel.setText ("Loaded: " + file.getFileName(), juce::dontSendNotification);
            panel->ctx.setStatusMessage ("Convolution impulse response loaded: " + file.getFileName());
        });
    }

    AppContext& ctx;
    juce::ToggleButton highPassToggle { "HIGH PASS" }, lowPassToggle { "LOW PASS" };
    SpKnob highPassKnob, lowPassKnob;
    SpComboBox highPassSlope, lowPassSlope;
    juce::ToggleButton convolutionToggle { "CONVOLUTION" };
    SpButton loadIrButton { "LOAD IR..." };
    SpButton clearIrButton { "CLEAR" };
    SpKnob mixKnob;
    juce::ToggleButton trueStereoToggle { "TRUE STEREO IR" };
    juce::Label irStatusLabel;
    juce::Rectangle<int> hpCard, lpCard, convCard, noteArea;
};

//==============================================================================
/** ReplayGain, auto headroom and the output limiter. */
class GainPanel : public juce::Component,
                  private juce::Timer
{
public:
    explicit GainPanel (AppContext& context) : ctx (context)
    {
        setOpaque (true);

        replayGainToggle.setButtonText ("REPLAYGAIN");
        replayGainToggle.onClick = [this] { applyReplayGain(); };
        addAndMakeVisible (replayGainToggle);
        albumModeToggle.setButtonText ("ALBUM MODE");
        albumModeToggle.onClick = [this] { applyReplayGain(); };
        addAndMakeVisible (albumModeToggle);
        preventClipToggle.setButtonText ("PREVENT CLIPPING");
        preventClipToggle.onClick = [this] { applyReplayGain(); };
        addAndMakeVisible (preventClipToggle);

        configureSpKnob (rgPreampKnob, -12.0, 12.0, 0.0, 0.0,
                         [] (double v) { return dbText (v, 1); });
        rgPreampKnob.setLabelText ("PREAMP");
        rgPreampKnob.onValueChange = [this] { applyReplayGain(); };
        addAndMakeVisible (rgPreampKnob);

        configureSpKnob (rgFallbackKnob, -12.0, 12.0, 0.0, 0.0,
                         [] (double v) { return dbText (v, 1); });
        rgFallbackKnob.setLabelText ("NO RG TAG");
        rgFallbackKnob.onValueChange = [this] { applyReplayGain(); };
        addAndMakeVisible (rgFallbackKnob);

        autoHeadroomToggle.setButtonText ("AUTO HEADROOM");
        autoHeadroomToggle.onClick = [this] { applyPreamp(); };
        addAndMakeVisible (autoHeadroomToggle);

        configureSpKnob (manualPreampKnob, -24.0, 12.0, 0.0, 0.0,
                         [] (double v) { return dbText (v, 1); });
        manualPreampKnob.setLabelText ("MANUAL TRIM");
        manualPreampKnob.onValueChange = [this] { applyPreamp(); };
        addAndMakeVisible (manualPreampKnob);

        limiterToggle.setButtonText ("LIMITER");
        limiterToggle.onClick = [this] { applyPreamp(); };
        addAndMakeVisible (limiterToggle);

        configureSpKnob (ceilingKnob, -6.0, 0.0, -0.3, -0.3,
                         [] (double v) { return dbText (v, 1); });
        ceilingKnob.setLabelText ("CEILING");
        ceilingKnob.onValueChange = [this] { applyPreamp(); };
        addAndMakeVisible (ceilingKnob);

        configureSpKnob (releaseKnob, 10.0, 1000.0, 150.0, 150.0,
                         [] (double v) { return juce::String ((int) std::round (v)) + " ms"; });
        releaseKnob.setLabelText ("RELEASE");
        releaseKnob.onValueChange = [this] { applyPreamp(); };
        addAndMakeVisible (releaseKnob);

        configureLabel (rgStatusLabel, 10.0f, theme::textDim);
        addAndMakeVisible (rgStatusLabel);

        setSize (900, 620);
        sync();
        startTimerHz (5);
    }

    ~GainPanel() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::background);
        auto top = getLocalBounds().reduced (14, 8).removeFromTop (22);
        theme::drawSectionHeader (g, top, "GAIN / REPLAYGAIN / LIMITER", "HEADROOM AND LEVEL MANAGEMENT");

        theme::fillPanel (g, rgCard);
        theme::drawSectionHeader (g, rgCard.reduced (10, 5).removeFromTop (16), "REPLAYGAIN", "TRACK OR ALBUM");
        theme::fillPanel (g, preampCard);
        theme::drawSectionHeader (g, preampCard.reduced (10, 5).removeFromTop (16), "PREAMP / HEADROOM", "MAKE BOOST SAFE");
        theme::fillPanel (g, limiterCard);
        theme::drawSectionHeader (g, limiterCard.reduced (10, 5).removeFromTop (16), "OUTPUT LIMITER", "TRUE-PEAK CEILING");

        auto status = ctx.engine().getStatus();
        g.setColour (theme::textFaint);
        g.setFont (theme::uiFont (10.0f));
        juce::String live;
        live << "Live: max boost " << dbText (status.dspStatus.maxPositiveBoostDb, 1)
             << " dB  |  auto headroom " << dbText (status.dspStatus.autoHeadroomDb, 1)
             << " dB  |  effective preamp " << dbText (status.dspStatus.effectivePreampDb, 1)
             << " dB  |  limiter " << (status.dspStatus.limiterEngaged ? "engaged " : "idle ")
             << dbText (status.dspStatus.limiterReductionDb, 1) << " dB GR";
        g.drawFittedText (live, liveArea, juce::Justification::centredLeft, 2);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 8);
        r.removeFromTop (22);
        r.removeFromTop (8);

        auto cards = r.removeFromTop (juce::jmin (320, juce::jmax (260, r.getHeight() * 58 / 100)));
        const int cardWidth = juce::jmax (250, (cards.getWidth() - 20) / 3);
        rgCard = cards.removeFromLeft (cardWidth);
        cards.removeFromLeft (10);
        preampCard = cards.removeFromLeft (cardWidth);
        cards.removeFromLeft (10);
        limiterCard = cards.removeFromLeft (cardWidth);

        r.removeFromTop (12);
        liveArea = r.removeFromTop (juce::jmin (30, r.getHeight()));

        {
            auto inner = rgCard.reduced (12, 8);
            inner.removeFromTop (20);
            replayGainToggle.setBounds (inner.removeFromTop (22).removeFromLeft (juce::jmin (140, inner.getWidth())));
            albumModeToggle.setBounds (inner.removeFromTop (22).removeFromLeft (juce::jmin (140, inner.getWidth())));
            preventClipToggle.setBounds (inner.removeFromTop (22).removeFromLeft (juce::jmin (180, inner.getWidth())));
            inner.removeFromTop (4);
            inner.removeFromTop (14);
            auto row = inner.removeFromTop (juce::jmin (116, inner.getHeight()));
            rgPreampKnob.setBounds (row.removeFromLeft (juce::jmin (112, row.getWidth() / 2)).reduced (6));
            rgFallbackKnob.setBounds (row.removeFromLeft (juce::jmin (112, row.getWidth())).reduced (6));
            rgStatusLabel.setBounds (inner.removeFromTop (juce::jmin (30, juce::jmax (0, inner.getHeight()))));
        }
        {
            auto inner = preampCard.reduced (12, 8);
            inner.removeFromTop (20);
            autoHeadroomToggle.setBounds (inner.removeFromTop (22).removeFromLeft (juce::jmin (160, inner.getWidth())));
            inner.removeFromTop (4);
            inner.removeFromTop (14);
            auto row = inner.removeFromTop (juce::jmin (130, inner.getHeight()));
            manualPreampKnob.setBounds (row.removeFromLeft (juce::jmin (124, row.getWidth())).reduced (6));
        }
        {
            auto inner = limiterCard.reduced (12, 8);
            inner.removeFromTop (20);
            limiterToggle.setBounds (inner.removeFromTop (22).removeFromLeft (juce::jmin (140, inner.getWidth())));
            inner.removeFromTop (4);
            inner.removeFromTop (14);
            auto row = inner.removeFromTop (juce::jmin (116, inner.getHeight()));
            ceilingKnob.setBounds (row.removeFromLeft (juce::jmin (112, row.getWidth() / 2)).reduced (6));
            releaseKnob.setBounds (row.removeFromLeft (juce::jmin (112, row.getWidth())).reduced (6));
        }
    }

private:
    void timerCallback() override { sync(); }

    void applyReplayGain()
    {
        auto p = ctx.getCurrentDspParams();
        p.replayGain.enabled = replayGainToggle.getToggleState();
        p.replayGain.albumMode = albumModeToggle.getToggleState();
        p.replayGain.preventClipping = preventClipToggle.getToggleState();
        p.replayGain.preampDb = (float) rgPreampKnob.getValue();
        p.replayGain.fallbackGainDb = (float) rgFallbackKnob.getValue();
        ctx.applyUserDspParams (p);
    }

    void applyPreamp()
    {
        auto p = ctx.getCurrentDspParams();
        p.preamp.autoHeadroom = autoHeadroomToggle.getToggleState();
        p.preamp.manualPreampDb = (float) manualPreampKnob.getValue();
        p.preamp.limiterEnabled = limiterToggle.getToggleState();
        p.preamp.outputCeilingDb = (float) ceilingKnob.getValue();
        p.preamp.limiterReleaseMs = (float) releaseKnob.getValue();
        ctx.applyUserDspParams (p);
    }

    void sync()
    {
        const auto p = ctx.getCurrentDspParams();
        replayGainToggle.setToggleState (p.replayGain.enabled, juce::dontSendNotification);
        albumModeToggle.setToggleState (p.replayGain.albumMode, juce::dontSendNotification);
        preventClipToggle.setToggleState (p.replayGain.preventClipping, juce::dontSendNotification);
        rgPreampKnob.setValue (p.replayGain.preampDb, juce::dontSendNotification);
        rgFallbackKnob.setValue (p.replayGain.fallbackGainDb, juce::dontSendNotification);
        autoHeadroomToggle.setToggleState (p.preamp.autoHeadroom, juce::dontSendNotification);
        manualPreampKnob.setValue (p.preamp.manualPreampDb, juce::dontSendNotification);
        limiterToggle.setToggleState (p.preamp.limiterEnabled, juce::dontSendNotification);
        ceilingKnob.setValue (p.preamp.outputCeilingDb, juce::dontSendNotification);
        releaseKnob.setValue (p.preamp.limiterReleaseMs, juce::dontSendNotification);

        const auto status = ctx.engine().getStatus();
        const auto track = status.currentTrack;
        juce::String rg;
        if (p.replayGain.enabled)
        {
            const float gain = p.replayGainFor (track.hasReplayGainTrack, track.replayGainTrackDb,
                                                track.hasReplayGainAlbum, track.replayGainAlbumDb);
            rg << "Applying " << dbText (gain, 1) << " dB";
            if (! track.hasReplayGainTrack && ! track.hasReplayGainAlbum)
                rg << " (fallback: no RG tags on this track)";
        }
        else
        {
            rg = "ReplayGain off.";
        }
        if (rgStatusLabel.getText() != rg)
            rgStatusLabel.setText (rg, juce::dontSendNotification);
    }

    AppContext& ctx;
    juce::ToggleButton replayGainToggle { "REPLAYGAIN" }, albumModeToggle { "ALBUM MODE" },
                        preventClipToggle { "PREVENT CLIPPING" };
    SpKnob rgPreampKnob, rgFallbackKnob;
    juce::ToggleButton autoHeadroomToggle { "AUTO HEADROOM" }, limiterToggle { "LIMITER" };
    SpKnob manualPreampKnob, ceilingKnob, releaseKnob;
    juce::Label rgStatusLabel;
    juce::Rectangle<int> rgCard, preampCard, limiterCard, liveArea;
};

//==============================================================================
/** A/B preset storage and comparison. */
class AbPresetsPanel : public juce::Component,
                       private juce::Timer
{
public:
    explicit AbPresetsPanel (AppContext& context) : ctx (context)
    {
        setOpaque (true);

        storeAButton.onClick = [this]
        {
            ctx.settings().set ("dsp.ab.a", juce::JSON::toString (serialize::toVar (ctx.getCurrentDspParams())));
            ctx.settings().set ("dsp.ab.aTime", juce::Time::getCurrentTime().toString (true, true));
            refreshLabels();
        };
        recallAButton.onClick = [this] { recall ("dsp.ab.a"); };
        storeBButton.onClick = [this]
        {
            ctx.settings().set ("dsp.ab.b", juce::JSON::toString (serialize::toVar (ctx.getCurrentDspParams())));
            ctx.settings().set ("dsp.ab.bTime", juce::Time::getCurrentTime().toString (true, true));
            refreshLabels();
        };
        recallBButton.onClick = [this] { recall ("dsp.ab.b"); };

        for (auto* b : { &storeAButton, &recallAButton, &storeBButton, &recallBButton })
            addAndMakeVisible (*b);

        abToggle.setClickingTogglesState (true);
        abToggle.onClick = [this]
        {
            ctx.setAbComparisonActive (abToggle.getToggleState());
            refreshLabels();
        };
        addAndMakeVisible (abToggle);

        levelMatchToggle.setClickingTogglesState (true);
        levelMatchToggle.onClick = [this]
        {
            ctx.setAbLevelMatching (levelMatchToggle.getToggleState());
            refreshLabels();
        };
        addAndMakeVisible (levelMatchToggle);

        configureLabel (statusALabel, 10.0f, theme::textDim);
        configureLabel (statusBLabel, 10.0f, theme::textDim);
        configureLabel (abStatusLabel, 10.5f, theme::silverDim);
        addAndMakeVisible (statusALabel);
        addAndMakeVisible (statusBLabel);
        addAndMakeVisible (abStatusLabel);

        setSize (860, 520);
        refreshLabels();
        startTimerHz (4);
    }

    ~AbPresetsPanel() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::background);
        auto top = getLocalBounds().reduced (14, 8).removeFromTop (22);
        theme::drawSectionHeader (g, top, "A/B PRESETS", "STORE, RECALL AND COMPARE");

        theme::fillPanel (g, cardA);
        theme::drawSectionHeader (g, cardA.reduced (10, 6).removeFromTop (16), "A", "REFERENCE SLOT");
        theme::fillPanel (g, cardB);
        theme::drawSectionHeader (g, cardB.reduced (10, 6).removeFromTop (16), "B", "ALTERNATE SLOT");
        theme::fillPanel (g, compareCard);
        theme::drawSectionHeader (g, compareCard.reduced (10, 6).removeFromTop (16), "COMPARE", "BYPASS A - PROCESSED B");

        g.setColour (theme::textFaint);
        g.setFont (theme::uiFont (10.0f));
        g.drawFittedText ("A/B COMPARE bypasses the whole chain for A and restores the processed parameters for B. "
                          "Level matching applies the measured RMS difference so the comparison judges tone, not loudness. "
                          "Stored slots persist in the application settings.",
                          compareCard.reduced (10, 6).withTrimmedTop (66),
                          juce::Justification::topLeft, 4);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 8);
        r.removeFromTop (22);
        r.removeFromTop (8);

        auto topRow = r.removeFromTop (juce::jmin (200, juce::jmax (150, r.getHeight() / 3)));
        cardA = topRow.removeFromLeft ((topRow.getWidth() - 10) / 2);
        topRow.removeFromLeft (10);
        cardB = topRow;

        r.removeFromTop (10);
        compareCard = r.removeFromTop (juce::jmin (250, juce::jmax (180, r.getHeight())));

        auto layoutCard = [this] (juce::Rectangle<int> card, SpButton& store, SpButton& recall,
                                  juce::Label& status)
        {
            auto inner = card.reduced (12, 8);
            inner.removeFromTop (22);
            auto row = inner.removeFromTop (28);
            store.setBounds (row.removeFromLeft (juce::jmin (110, row.getWidth() / 2)));
            row.removeFromLeft (8);
            recall.setBounds (row.removeFromLeft (juce::jmin (110, row.getWidth())));
            inner.removeFromTop (8);
            status.setBounds (inner.removeFromTop (juce::jmin (32, inner.getHeight())));
        };
        layoutCard (cardA, storeAButton, recallAButton, statusALabel);
        layoutCard (cardB, storeBButton, recallBButton, statusBLabel);

        auto inner = compareCard.reduced (12, 8);
        inner.removeFromTop (22);
        auto row = inner.removeFromTop (26);
        abToggle.setBounds (row.removeFromLeft (juce::jmin (160, row.getWidth() / 2)));
        row.removeFromLeft (10);
        levelMatchToggle.setBounds (row.removeFromLeft (juce::jmin (150, row.getWidth())));
        inner.removeFromTop (6);
        abStatusLabel.setBounds (inner.removeFromTop (20));
    }

private:
    void timerCallback() override { refreshLabels(); }

    void recall (const juce::String& key)
    {
        const auto text = ctx.settings().getString (key);
        if (text.isEmpty())
            return;

        juce::var v;
        if (juce::JSON::parse (text, v).failed())
            return;

        auto p = serialize::dspFromVar (v, ctx.getCurrentDspParams());
        p.bypassed = false;
        ctx.applyUserDspParams (p);
        ctx.setStatusMessage ("Recalled A/B slot " + key.fromLastOccurrenceOf (".", false, false)
                              + ".");
        refreshLabels();
    }

    void refreshLabels()
    {
        auto describe = [this] (const juce::String& key) -> juce::String
        {
            const auto text = ctx.settings().getString (key);
            if (text.isEmpty())
                return "Empty slot.";

            juce::var v;
            if (juce::JSON::parse (text, v).failed())
                return "Stored data could not be read.";

            auto p = serialize::dspFromVar (v, {});
            const auto processors = p.activeProcessorNames();
            juce::String text2;
            text2 << (processors.empty() ? juce::String ("Flat / bypassed")
                                         : juce::String ((int) processors.size()) + " processors active");
            if (p.maxPositiveBoostDb() > 0.01)
                text2 << "  |  up to " << dbText (p.maxPositiveBoostDb(), 1) << " dB boost";
            return text2;
        };

        const auto a = describe ("dsp.ab.a");
        const auto b = describe ("dsp.ab.b");
        if (a != statusALabel.getText())
            statusALabel.setText (a, juce::dontSendNotification);
        if (b != statusBLabel.getText())
            statusBLabel.setText (b, juce::dontSendNotification);

        abStatusLabel.setText (ctx.getAbStatusText(), juce::dontSendNotification);
        abToggle.setToggleState (ctx.isAbComparisonActive(), juce::dontSendNotification);
        levelMatchToggle.setToggleState (ctx.isAbLevelMatching(), juce::dontSendNotification);
    }

    AppContext& ctx;
    SpButton storeAButton { "STORE A" }, recallAButton { "RECALL A" };
    SpButton storeBButton { "STORE B" }, recallBButton { "RECALL B" };
    SpToggleButton abToggle { "A/B COMPARE" };
    SpToggleButton levelMatchToggle { "LEVEL MATCH" };
    juce::Label statusALabel, statusBLabel, abStatusLabel;
    juce::Rectangle<int> cardA, cardB, compareCard;
};

//==============================================================================
/** Device DSP profiles. */
class DeviceProfilesPanel : public juce::Component,
                            private juce::Timer
{
public:
    explicit DeviceProfilesPanel (AppContext& context) : ctx (context)
    {
        setOpaque (true);

        deviceBox.setTextWhenNothingSelected ("Output devices");
        deviceBox.onChange = [this]
        {
            const int index = deviceBox.getSelectedItemIndex();
            if (index == (int) deviceNames.size())
            {
                ctx.setPiOutputSelected (true);
                refresh();
                return;
            }
            if (index < 0 || index >= (int) deviceNames.size())
                return;
            if (ctx.isPiOutputSelected())
                ctx.setPiOutputSelected (false);
            juce::String error;
            if (ctx.selectOutputDevice (deviceNames[(size_t) index], 0.0, error))
                ctx.setStatusMessage ("Output device: " + deviceNames[(size_t) index]);
            else
                setInfo ("Could not select output device: " + error);
            refresh();
        };
        addAndMakeVisible (deviceBox);

        refreshDevicesButton.onClick = [this]
        {
            ctx.refreshOutputDevices();
            refresh();
            ctx.setStatusMessage ("Output devices refreshed.");
        };
        addAndMakeVisible (refreshDevicesButton);

        profileBox.setTextWhenNothingSelected ("Device profiles");
        profileBox.onChange = [this] { refreshInfo(); };
        addAndMakeVisible (profileBox);

        saveButton.onClick = [this]
        {
            ctx.saveDspForCurrentDevice();
            refresh();
            ctx.setStatusMessage ("Saved DSP profile for "
                                  + (ctx.isPiOutputSelected()
                                         ? juce::String ("Pi K11 (Network)")
                                         : ctx.engine().devices().getCurrentDeviceName()));
        };
        applyButton.onClick = [this]
        {
            const int index = profileBox.getSelectedItemIndex();
            if (index < 0 || index >= (int) profiles.size())
                return;
            ctx.applyUserDspParams (profiles[(size_t) index].params);
            ctx.setStatusMessage ("Applied DSP profile \"" + profiles[(size_t) index].name + "\".");
        };
        deleteButton.onClick = [this]
        {
            const int index = profileBox.getSelectedItemIndex();
            if (index < 0 || index >= (int) profiles.size())
                return;
            std::vector<DeviceDspProfile> keep;
            for (auto& p : profiles)
                if (p.id != profiles[(size_t) index].id)
                    keep.push_back (p);
            ctx.settings().saveDeviceProfiles (keep);
            refresh();
            ctx.setStatusMessage ("Device profile deleted.");
        };
        addAndMakeVisible (saveButton);
        addAndMakeVisible (applyButton);
        addAndMakeVisible (deleteButton);

        configureLabel (currentLabel, 11.0f, theme::text);
        configureLabel (infoLabel, 10.0f, theme::textDim);
        infoLabel.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (currentLabel);
        addAndMakeVisible (infoLabel);

        setSize (860, 520);
        refresh();
        startTimerHz (2);
    }

    ~DeviceProfilesPanel() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::background);
        auto top = getLocalBounds().reduced (14, 8).removeFromTop (22);
        theme::drawSectionHeader (g, top, "DEVICE PROFILES", "PER-OUTPUT DSP MEMORY");

        theme::fillPanel (g, deviceCard);
        theme::drawSectionHeader (g, deviceCard.reduced (10, 6).removeFromTop (16), "OUTPUT DEVICE", "SELECT AND REFRESH");
        theme::fillPanel (g, profileCard);
        theme::drawSectionHeader (g, profileCard.reduced (10, 6).removeFromTop (16), "STORED PROFILES", "SAVE / APPLY / DELETE");

        auto inner = deviceCard.reduced (12, 8);
        g.setColour (theme::textFaint);
        g.setFont (theme::uiFont (10.0f));
        g.drawFittedText ("A device profile binds the complete DSP state to one output device and is loaded "
                          "when that device is selected.",
                          inner.removeFromBottom (juce::jmin (34, inner.getHeight())),
                          juce::Justification::topLeft, 2);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 8);
        r.removeFromTop (22);
        r.removeFromTop (8);

        auto topRow = r.removeFromTop (juce::jmin (170, r.getHeight() / 2));
        deviceCard = topRow.removeFromLeft ((topRow.getWidth() - 10) / 2);
        topRow.removeFromLeft (10);
        profileCard = topRow;

        {
            auto inner = deviceCard.reduced (12, 8);
            inner.removeFromTop (20);
            auto row = inner.removeFromTop (28);
            deviceBox.setBounds (row.removeFromLeft (juce::jmin (280, row.getWidth() - 140)));
            row.removeFromLeft (8);
            refreshDevicesButton.setBounds (row.removeFromLeft (juce::jmin (120, row.getWidth())));
            inner.removeFromTop (8);
            currentLabel.setBounds (inner.removeFromTop (18));
            inner.removeFromTop (6);
            inner.removeFromBottom (juce::jmin (34, inner.getHeight()));
        }
        {
            auto inner = profileCard.reduced (12, 8);
            inner.removeFromTop (22);
            profileBox.setBounds (inner.removeFromTop (28).removeFromLeft (juce::jmin (320, inner.getWidth())));
            inner.removeFromTop (6);
            auto row = inner.removeFromTop (28);
            saveButton.setBounds (row.removeFromLeft (juce::jmin (110, row.getWidth() / 3)));
            row.removeFromLeft (8);
            applyButton.setBounds (row.removeFromLeft (juce::jmin (100, row.getWidth() / 2)));
            row.removeFromLeft (8);
            deleteButton.setBounds (row.removeFromLeft (juce::jmin (100, row.getWidth())));
            inner.removeFromTop (8);
            infoLabel.setBounds (inner.removeFromTop (juce::jmin (60, inner.getHeight())));
        }
    }

private:
    void timerCallback() override { refresh(); }

    void setInfo (const juce::String& text) { infoLabel.setText (text, juce::dontSendNotification); }

    void refresh()
    {
        // Output devices.
        auto devices = ctx.getOutputDevices();
        juce::String deviceSignature;
        for (auto& d : devices)
            deviceSignature << d.name << "|" << d.deviceId << ";";

        if (deviceSignature != lastDeviceSignature)
        {
            lastDeviceSignature = deviceSignature;
            deviceNames.clear();
            deviceBox.clear (juce::dontSendNotification);
            int id = 1;
            int selected = -1;
            const auto current = ctx.engine().devices().getCurrentDeviceName();
            for (auto& d : devices)
            {
                deviceBox.addItem (d.name.isNotEmpty() ? d.name : d.deviceId, id);
                deviceNames.push_back (d.name.isNotEmpty() ? d.name : d.deviceId);
                if (d.name == current || d.deviceId == current)
                    selected = id - 1;
                ++id;
            }
            deviceBox.addItem ("Pi K11 (network, SSH)", (int) deviceNames.size() + 1);
            deviceBox.setSelectedItemIndex (selected, juce::dontSendNotification);
        }

        if (ctx.isPiOutputSelected())
            deviceBox.setSelectedItemIndex ((int) deviceNames.size(), juce::dontSendNotification);
        else
        {
            const auto current = ctx.engine().devices().getCurrentDeviceName();
            for (int i = 0; i < (int) deviceNames.size(); ++i)
                if (deviceNames[(size_t) i] == current)
                {
                    deviceBox.setSelectedItemIndex (i, juce::dontSendNotification);
                    break;
                }
        }

        // Profiles.
        profiles = ctx.settings().loadDeviceProfiles();
        juce::String profileSignature;
        for (auto& p : profiles)
            profileSignature << p.id << "|" << p.name << ";";

        if (profileSignature != lastProfileSignature)
        {
            lastProfileSignature = profileSignature;
            profileBox.clear (juce::dontSendNotification);
            for (int i = 0; i < (int) profiles.size(); ++i)
                profileBox.addItem (profiles[(size_t) i].name.isNotEmpty() ? profiles[(size_t) i].name
                                                                            : profiles[(size_t) i].id,
                                    i + 1);
            if (! profiles.empty())
                profileBox.setSelectedItemIndex (0, juce::dontSendNotification);
        }

        currentLabel.setText (ctx.isPiOutputSelected()
                                  ? juce::String ("CURRENT OUTPUT: Pi K11 (network) - local Core Audio is inactive")
                                  : ("CURRENT: " + ctx.engine().devices().getCurrentDeviceName()),
                              juce::dontSendNotification);
        refreshInfo();
    }

    void refreshInfo()
    {
        const int index = profileBox.getSelectedItemIndex();
        if (index < 0 || index >= (int) profiles.size())
        {
            setInfo (profiles.empty() ? "No device profiles saved yet. Set up the sound you like, then press SAVE."
                                      : "Select a profile.");
            return;
        }

        const auto& p = profiles[(size_t) index];
        auto diffs = ProfileEngine::compare (ctx.getCurrentDspParams(), p.params);
        juce::String text;
        text << "Device: " << p.key.deviceName << "\n";
        if (diffs.empty())
            text << "Identical to the current DSP state.";
        else
        {
            text << (int) diffs.size() << " differences: ";
            int shown = 0;
            for (auto& d : diffs)
            {
                if (shown++ >= 3)
                {
                    text << " ...";
                    break;
                }
                text << d.label << " " << d.a << " -> " << d.b << "; ";
            }
        }
        setInfo (text);
    }

    AppContext& ctx;
    SpComboBox deviceBox, profileBox;
    SpButton refreshDevicesButton { "REFRESH" };
    SpButton saveButton { "SAVE CURRENT" }, applyButton { "APPLY" }, deleteButton { "DELETE" };
    juce::Label currentLabel, infoLabel;
    std::vector<juce::String> deviceNames;
    std::vector<DeviceDspProfile> profiles;
    juce::String lastDeviceSignature, lastProfileSignature;
    juce::Rectangle<int> deviceCard, profileCard;
};

//==============================================================================
/** Meters tab: meters + spectrum + spectrogram. */
class MetersTab : public juce::Component
{
public:
    explicit MetersTab (AppContext& context)
        : meters (context, MetersPanel::Mode::expanded),
          spectrum (context),
          spectrogram (context)
    {
        setOpaque (true);
        addAndMakeVisible (meters);
        addAndMakeVisible (spectrum);
        addAndMakeVisible (spectrogram);
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::background); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 8);
        if (r.getWidth() >= 980 && r.getHeight() >= 420)
        {
            auto left = r.removeFromLeft (juce::jmax (420, (int) ((float) r.getWidth() * 0.42f)));
            r.removeFromLeft (10);
            meters.setBounds (left);
            spectrum.setBounds (r.removeFromTop (r.getHeight() / 2 - 5));
            r.removeFromTop (10);
            spectrogram.setBounds (r);
        }
        else
        {
            meters.setBounds (r.removeFromTop (juce::jmin (juce::jmax (300, r.getHeight() / 2), r.getHeight())));
            r.removeFromTop (10);
            spectrum.setBounds (r.removeFromTop (r.getHeight() / 2 - 5));
            r.removeFromTop (10);
            spectrogram.setBounds (r);
        }
    }

private:
    MetersPanel meters;
    SpectrumComponent spectrum;
    SpectrogramComponent spectrogram;
};

} // namespace

//==============================================================================
class DspStudioWindow::Content : public juce::Component,
                                 private juce::Timer
{
public:
    explicit Content (AppContext& context) : ctx (context)
    {
        setOpaque (true);

        addAndMakeVisible (tabs);
        tabs.setIndent (0);

        graphicEditor = new GraphicEqEditor (ctx);
        parametricEditor = new ParametricEqEditor (ctx);
        bassPanel = new BassControlPanel (ctx);
        signalPath = new SignalPathPanel (ctx);
        profilePanel = new ProfilePanel (ctx);

        tabs.addTab ("GRAPHIC", theme::background, graphicEditor, true);
        tabs.addTab ("PARAMETRIC", theme::background, parametricEditor, true);
        tabs.addTab ("BASS", theme::background, bassPanel, true);
        tabs.addTab ("CHAIN", theme::background, new DspChainPanel (ctx), true);
        tabs.addTab ("STEREO", theme::background, new StereoPanel (ctx), true);
        tabs.addTab ("FILTERS", theme::background, new FiltersPanel (ctx), true);
        tabs.addTab ("GAIN / LIMIT", theme::background, new GainPanel (ctx), true);
        tabs.addTab ("A/B", theme::background, new AbPresetsPanel (ctx), true);
        tabs.addTab ("METERS", theme::background, new MetersTab (ctx), true);
        tabs.addTab ("SIGNAL PATH", theme::background, signalPath, true);
        tabs.addTab ("DEVICES", theme::background, new DeviceProfilesPanel (ctx), true);
        tabs.addTab ("PROFILES", theme::background, profilePanel, true);

        // Toolbar.
        presetBox.setTextWhenNothingSelected ("Presets & profiles");
        presetBox.onChange = [this] { onPresetSelected(); };
        addAndMakeVisible (presetBox);

        savePresetButton.setTooltip ("Save the current DSP state as a JSON preset");
        savePresetButton.onClick = [this] { savePresetToFile(); };
        addAndMakeVisible (savePresetButton);

        saveProfileButton.setTooltip ("Save the current DSP state as a user profile");
        saveProfileButton.onClick = [this] { saveUserProfile(); };
        addAndMakeVisible (saveProfileButton);

        deletePresetButton.setTooltip ("Delete the selected preset or user profile");
        deletePresetButton.onClick = [this] { deleteSelectedPreset(); };
        addAndMakeVisible (deletePresetButton);

        reloadButton.setTooltip ("Rescan presets and profiles");
        reloadButton.onClick = [this] { refreshPresetList (true); };
        addAndMakeVisible (reloadButton);

        referenceToggle.setClickingTogglesState (true);
        referenceToggle.onClick = [this]
        {
            ctx.setReferenceMode (referenceToggle.getToggleState());
        };
        addAndMakeVisible (referenceToggle);

        setSize (1180, 820);
        refreshPresetList (true);
        startTimerHz (2);
    }

    ~Content() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::background);

        auto header = getLocalBounds().reduced (14, 0).removeFromTop (78);
        auto titleRow = header.removeFromTop (44);
        g.setColour (theme::gold);
        g.setFont (theme::uiFont (15.0f, true, 0.14f));
        g.drawText ("DSP STUDIO", titleRow.removeFromTop (26), juce::Justification::centredLeft, false);

        g.setColour (theme::textFaint);
        g.setFont (theme::capsFont (9.5f, 0.12f));
        g.drawText ("REFERENCE PROCESSING WORKSPACE", titleRow.removeFromTop (14),
                    juce::Justification::centredLeft, false);

        // Status message (from the coordinator) on the right of the title row.
        g.setFont (theme::monoFont (9.5f));
        g.setColour (theme::silverDim);
        g.drawFittedText (ctx.getStatusMessage(),
                          getLocalBounds().reduced (14, 0).removeFromTop (44)
                              .withTrimmedLeft (juce::jmax (240, getWidth() / 2))
                              .withTrimmedRight (0).withTrimmedTop (12).withHeight (16),
                          juce::Justification::centredRight, 1);

        const auto p = ctx.getCurrentDspParams();
        if (p.bypassed || ctx.isReferenceMode())
        {
            g.setColour (theme::warning);
            g.setFont (theme::capsFont (9.5f, 0.10f));
            // Keep the bypass warning on its own line below the subtitle;
            // previously it overlapped "REFERENCE PROCESSING WORKSPACE".
            g.drawText (ctx.isReferenceMode() ? "REFERENCE MODE - ALL DSP BYPASSED"
                                              : "GLOBAL DSP BYPASS ACTIVE",
                        getLocalBounds().reduced (14, 0).removeFromTop (78)
                            .withTrimmedTop (44).withHeight (18),
                        juce::Justification::centredLeft, false);
        }

        theme::drawHairline (g, 14, 77, getWidth() - 14, 77, theme::line);
    }

    void setTab (int index)
    {
        tabs.setCurrentTabIndex (juce::jlimit (0, juce::jmax (0, tabs.getNumTabs() - 1), index));
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto header = r.removeFromTop (78);
        tabs.setBounds (r.reduced (0, 0));

        auto controls = header.reduced (14, 0).removeFromBottom (32).withTrimmedBottom (2);
        auto placeRight = [&] (juce::Component& c, int width)
        {
            auto area = controls.removeFromRight (width);
            c.setBounds (area);
            controls.removeFromRight (8);
        };

        placeRight (reloadButton, 84);
        placeRight (deletePresetButton, 82);
        placeRight (saveProfileButton, 126);
        placeRight (savePresetButton, 112);
        placeRight (presetBox, 240);
        placeRight (referenceToggle, 162);
    }

private:
    struct PresetTarget
    {
        enum class Kind { profile, file } kind = Kind::file;
        juce::String id;      // profile id or file path
        juce::String name;
        bool builtIn = false;
    };

    void timerCallback() override
    {
        referenceToggle.setToggleState (ctx.isReferenceMode(), juce::dontSendNotification);
        refreshPresetList (false);
    }

    void refreshPresetList (bool force)
    {
        std::vector<PresetTarget> targets;
        juce::String signature;

        for (auto& p : ctx.profiles().getAllProfiles())
        {
            PresetTarget t;
            t.kind = PresetTarget::Kind::profile;
            t.id = p.id;
            t.name = p.name.isNotEmpty() ? p.name : p.id;
            t.builtIn = p.builtIn;
            signature << "p:" << p.id << ":" << p.name << ";";
            targets.push_back (t);
        }

        const auto dir = presetDir (ctx);
        auto files = dir.findChildFiles (juce::File::findFiles, false, "*.dsp.json");
        files.sort();
        for (auto& f : files)
        {
            PresetTarget t;
            t.kind = PresetTarget::Kind::file;
            t.id = f.getFullPathName();
            t.name = f.getFileNameWithoutExtension().upToLastOccurrenceOf (".dsp", false, false);
            signature << "f:" << t.id << ":" << f.getLastModificationTime().toMilliseconds() << ";";
            targets.push_back (t);
        }

        if (! force && signature == presetSignature)
            return;

        presetSignature = signature;
        presetTargets = std::move (targets);

        const auto previousId = selectedPresetId;
        updatingPresets = true;
        presetBox.clear (juce::dontSendNotification);
        int id = 1;
        int selected = -1;
        for (auto& t : presetTargets)
        {
            juce::String label;
            label << (t.kind == PresetTarget::Kind::profile ? "PROFILE  |  " : "PRESET  |  ")
                  << t.name;
            if (t.builtIn)
                label << "  (built-in)";
            presetBox.addItem (label, id);
            if (t.id == previousId)
                selected = id - 1;
            ++id;
        }
        presetBox.setSelectedItemIndex (selected, juce::dontSendNotification);
        updatingPresets = false;
    }

    void onPresetSelected()
    {
        if (updatingPresets)
            return;

        const int index = presetBox.getSelectedItemIndex();
        if (index < 0 || index >= (int) presetTargets.size())
            return;

        const auto& target = presetTargets[(size_t) index];
        selectedPresetId = target.id;

        if (target.kind == PresetTarget::Kind::profile)
        {
            ctx.selectProfile (target.id);
            ctx.setStatusMessage ("Loaded profile: " + target.name);
        }
        else
        {
            loadPresetFile (juce::File (target.id));
        }
        refreshTabs();
    }

    void loadPresetFile (const juce::File& file)
    {
        juce::var v;
        const auto parseResult = juce::JSON::parse (file.loadFileAsString(), v);
        if (parseResult.failed())
        {
            ctx.setStatusMessage ("Could not read preset: " + parseResult.getErrorMessage());
            return;
        }

        const auto dspVar = v.getProperty ("dsp", juce::var());
        if (dspVar.isVoid())
        {
            ctx.setStatusMessage ("Preset file does not contain DSP parameters.");
            return;
        }

        auto p = serialize::dspFromVar (dspVar, ctx.getCurrentDspParams());
        p.bypassed = false;
        ctx.applyUserDspParams (p);
        ctx.setStatusMessage ("Loaded preset: " + v.getProperty ("name", file.getFileNameWithoutExtension()).toString());
    }

    void savePresetToFile()
    {
        const auto name = promptForName (this, "SAVE DSP PRESET",
                                         "Preset " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M"));
        if (name.isEmpty())
            return;

        auto* object = new juce::DynamicObject();
        object->setProperty ("name", name);
        object->setProperty ("app", "Snake Puppy Reference Player");
        object->setProperty ("version", 1);
        object->setProperty ("created", juce::Time::getCurrentTime().toISO8601 (true));
        object->setProperty ("dsp", serialize::toVar (ctx.getCurrentDspParams()));

        const auto file = presetDir (ctx).getChildFile (presetFileName (name));
        if (! file.replaceWithText (juce::JSON::toString (juce::var (object), false)))
        {
            ctx.setStatusMessage ("Could not write preset file: " + file.getFullPathName());
            return;
        }

        refreshPresetList (true);
        ctx.setStatusMessage ("Preset saved: " + name);
    }

    void saveUserProfile()
    {
        const auto name = promptForName (this, "SAVE USER PROFILE", "My Sound");
        if (name.isEmpty())
            return;

        EqProfile profile;
        profile.id = juce::Uuid().toString();
        profile.name = name;
        profile.description = "Saved from DSP Studio";
        profile.builtIn = false;
        profile.params = ctx.getCurrentDspParams();

        const auto track = ctx.engine().getStatus().currentTrack;
        if (track.path.isNotEmpty())
            profile.genre = track.displayGenre();

        if (! ctx.profiles().saveUserProfile (profile))
            ctx.setStatusMessage ("Could not save the user profile.");
        else
            ctx.setStatusMessage ("User profile saved: " + name);

        refreshPresetList (true);
    }

    void deleteSelectedPreset()
    {
        const int index = presetBox.getSelectedItemIndex();
        if (index < 0 || index >= (int) presetTargets.size())
            return;

        const auto target = presetTargets[(size_t) index];
        if (target.kind == PresetTarget::Kind::profile)
        {
            if (target.builtIn)
            {
                ctx.setStatusMessage ("Built-in profiles cannot be deleted.");
                return;
            }
            if (ctx.profiles().deleteUserProfile (target.id))
                ctx.setStatusMessage ("Profile deleted: " + target.name);
            else
                ctx.setStatusMessage ("Could not delete the profile.");
        }
        else
        {
            const juce::File file (target.id);
            if (file.deleteFile())
                ctx.setStatusMessage ("Preset deleted: " + target.name);
            else
                ctx.setStatusMessage ("Could not delete the preset file.");
        }

        selectedPresetId.clear();
        presetBox.setSelectedItemIndex (-1, juce::dontSendNotification);
        refreshPresetList (true);
    }

    void refreshTabs()
    {
        if (graphicEditor != nullptr) graphicEditor->refreshNow();
        if (parametricEditor != nullptr) parametricEditor->refreshNow();
        if (bassPanel != nullptr) bassPanel->refreshNow();
        if (profilePanel != nullptr) profilePanel->refreshNow();
        if (signalPath != nullptr) signalPath->refreshNow();
    }

    AppContext& ctx;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    SpComboBox presetBox;
    SpButton savePresetButton { "SAVE PRESET" };
    SpButton saveProfileButton { "SAVE PROFILE" };
    SpButton deletePresetButton { "DELETE" };
    SpButton reloadButton { "RELOAD" };
    SpToggleButton referenceToggle { "REFERENCE MODE" };

    std::vector<PresetTarget> presetTargets;
    juce::String presetSignature;
    juce::String selectedPresetId;
    bool updatingPresets = false;

    GraphicEqEditor* graphicEditor = nullptr;
    ParametricEqEditor* parametricEditor = nullptr;
    BassControlPanel* bassPanel = nullptr;
    SignalPathPanel* signalPath = nullptr;
    ProfilePanel* profilePanel = nullptr;
};

//==============================================================================
namespace
{
DspStudioWindow* gDspStudioInstance = nullptr;
}

DspStudioWindow::DspStudioWindow (AppContext& context)
    : DocumentWindow ("DSP Studio", theme::background, DocumentWindow::closeButton),
      ctx (context)
{
    ui::ensureSharedLookAndFeel();

    setUsingNativeTitleBar (true);
    content = new Content (ctx);
    setContentOwned (content, false);
    setResizable (true, true);
    setResizeLimits (1000, 700, 3400, 2400);

    bool maximised = false;
    auto bounds = ctx.settings().loadWindowBounds ("dspStudio", { 120, 80, 1200, 820 }, maximised);
    if (maximised)
        bounds = { 80, 60, 1400, 900 };
    setBounds (bounds);
    lastSavedBounds = getBounds();

    startTimer (2000);
}

DspStudioWindow::~DspStudioWindow()
{
    stopTimer();
}

DspStudioWindow* DspStudioWindow::show (AppContext& context)
{
    if (gDspStudioInstance == nullptr)
        gDspStudioInstance = new DspStudioWindow (context);

    gDspStudioInstance->setVisible (true);
    gDspStudioInstance->toFront (true);
    return gDspStudioInstance;
}

void DspStudioWindow::destroyInstance()
{
    // Called by the application shell before the AppContext is destroyed. The
    // window destructor never touches the context, so this is also safe later.
    delete gDspStudioInstance;
    gDspStudioInstance = nullptr;
}

void DspStudioWindow::setTab (int index)
{
    if (content != nullptr)
        content->setTab (index);
}

void DspStudioWindow::closeButtonPressed()
{
    saveBounds();
    setVisible (false);
}

void DspStudioWindow::timerCallback()
{
    saveBounds();
}

void DspStudioWindow::saveBounds()
{
    if (! isVisible() || getBounds() == lastSavedBounds)
        return;

    lastSavedBounds = getBounds();
    ctx.settings().saveWindowBounds ("dspStudio", getBounds(), isFullScreen());
}

} // namespace sp
