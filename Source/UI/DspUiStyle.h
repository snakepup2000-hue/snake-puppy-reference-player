/*
    Snake Puppy Reference Player - DSP/metering UI styling adapter (ui-dsp).

    The visual identity lives in Source/UI/Theme.h (colours, fonts, surfaces),
    Source/UI/LookAndFeel.* (SpLookAndFeel) and Source/UI/Widgets.* (knobs,
    faders, buttons, headers), owned by ui-shell. This header is the one adapter
    the DSP panels include: it re-exports the shared theme, adds the small
    formatting/drawing helpers the DSP panels need, and guarantees the shared
    look-and-feel is installed even when a DSP window is opened standalone
    (e.g. from a verification harness).
*/
#pragma once

#include "UI/LookAndFeel.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

namespace sp::ui
{

// Re-export the shared identities so DSP panels have exactly one include.
namespace theme = sp::theme;

/** Alias used by every DSP component. */
using LookAndFeel = sp::SpLookAndFeel;

//==============================================================================
// Formatting helpers
//==============================================================================
inline juce::String dbText (double db, int decimals = 1)
{
    if (db <= -100.0)
        return juce::String::fromUTF8 ("-inf");
    juce::String s (db, decimals);
    if (db > 0.0)
        s = "+" + s;
    return s;
}

inline juce::String dbTextWithUnit (double db, int decimals = 1)
{
    return db <= -100.0 ? juce::String::fromUTF8 ("-inf dB") : dbText (db, decimals) + " dB";
}

inline juce::String freqText (double hz)
{
    if (hz <= 0.0)
        return "-";
    if (hz >= 1000.0)
        return juce::String (hz / 1000.0, hz >= 10000.0 ? 1 : 2) + " kHz";
    return juce::String (juce::roundToInt (hz)) + " Hz";
}

inline juce::String hzText (double hz)
{
    if (hz <= 0.0)
        return "-";
    return hz >= 1000.0 ? juce::String (hz / 1000.0, 2) + " kHz" : juce::String (hz, 0) + " Hz";
}

inline juce::String qText (double q) { return juce::String (q, q < 10.0 ? 2 : 1); }

inline juce::String percentText (double v, int decimals = 0) { return juce::String (v, decimals) + " %"; }

inline juce::String msText (double ms) { return juce::String (ms, ms < 10.0 ? 2 : 1) + " ms"; }

inline juce::Colour qualityColour (sp::SignalPathInfo::Quality q)
{
    switch (q)
    {
        case sp::SignalPathInfo::Quality::bitPerfect:     return theme::gold;
        case sp::SignalPathInfo::Quality::bitTransparent: return theme::silver;
        case sp::SignalPathInfo::Quality::processed:      return theme::silverDim;
        case sp::SignalPathInfo::Quality::error:          return theme::error;
        case sp::SignalPathInfo::Quality::unknown:        break;
    }
    return theme::textFaint;
}

//==============================================================================
// Surface / label helpers (thin wrappers over Theme.h)
//==============================================================================
inline void drawPanel (juce::Graphics& g, juce::Rectangle<int> area, bool alternate = false)
{
    theme::fillPanel (g, area, alternate);
}

inline void drawInset (juce::Graphics& g, juce::Rectangle<int> area)
{
    theme::fillSunken (g, area);
}

inline void configureLabel (juce::Label& label, float height = 12.0f,
                            juce::Colour colour = theme::text,
                            juce::Justification justification = juce::Justification::centredLeft)
{
    label.setFont (theme::uiFont (height));
    label.setColour (juce::Label::textColourId, colour);
    label.setJustificationType (justification);
    label.setMinimumHorizontalScale (0.6f);
    label.setBorderSize ({});
    label.getProperties().set ("spFont", true);
}

inline void configureSmallLabel (juce::Label& label, juce::Colour colour = theme::textDim)
{
    configureLabel (label, 10.5f, colour, juce::Justification::centredLeft);
}

inline void styleButton (juce::TextButton& b)
{
    b.setColour (juce::TextButton::buttonColourId, theme::panelRaised);
    b.setColour (juce::TextButton::buttonOnColourId, theme::gold);
    b.setColour (juce::TextButton::textColourOffId, theme::silver);
    b.setColour (juce::TextButton::textColourOnId, theme::bgDeep);
}

/** Honest empty/unavailable state: centred primary line plus wrapped detail. */
inline void drawEmptyState (juce::Graphics& g, juce::Rectangle<int> area,
                            const juce::String& primary,
                            const juce::String& detail = {},
                            juce::Colour accent = theme::silverDim)
{
    g.setColour (accent.withAlpha (0.8f));
    g.setFont (theme::uiFont (13.0f, true, 0.03f));
    g.drawFittedText (primary, area.removeFromTop (area.getHeight() / 2 + 8),
                      juce::Justification::centredBottom, 2);

    if (detail.isNotEmpty())
    {
        g.setColour (theme::textFaint);
        g.setFont (theme::uiFont (11.5f));
        g.drawFittedText (detail, area.reduced (18, 2), juce::Justification::centredTop, 4);
    }
}

/** Ensure the shared SpLookAndFeel is the default. MainWindow normally installs
    its own instance at startup; this only installs a fallback when another
    look-and-feel is active (e.g. a standalone DSP verification harness). */
inline void ensureSharedLookAndFeel()
{
    static LookAndFeel fallback;
    if (dynamic_cast<LookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel()) == nullptr)
        juce::LookAndFeel::setDefaultLookAndFeel (&fallback);
}

} // namespace sp::ui
