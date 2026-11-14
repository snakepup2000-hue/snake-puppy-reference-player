/*
    Snake Puppy Reference Player - visual identity.

    One place for every colour, font, metric and small drawing primitive so the
    whole application (ui-shell and ui-dsp panels) looks like one instrument:
    black / charcoal surfaces, brushed silver type, restrained gold accents.

    Header-only on purpose: ui-dsp panels include this and nothing else.
*/
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace sp::theme
{

//==============================================================================
// Surfaces (darkest -> lightest)
//==============================================================================
inline const juce::Colour bgDeep        { 0xff0a0b0d };
inline const juce::Colour background    { 0xff0e0f11 };
inline const juce::Colour panel         { 0xff15171b };
inline const juce::Colour panelAlt      { 0xff1d2026 };
inline const juce::Colour panelRaised   { 0xff23262c };
inline const juce::Colour panelSunken   { 0xff101215 };

inline const juce::Colour line          { 0xff2a2e35 };
inline const juce::Colour lineStrong    { 0xff3a3f48 };
inline const juce::Colour lineFaint     { 0xff202329 };

//==============================================================================
// Type colours
//==============================================================================
inline const juce::Colour text          { 0xffe8eaed };
inline const juce::Colour textDim       { 0xff9aa0a8 };
inline const juce::Colour textFaint     { 0xff5f656e };
inline const juce::Colour textEngraved  { 0xffb6bcc4 };

//==============================================================================
// Brushed metal
//==============================================================================
inline const juce::Colour silver        { 0xffc9cdd3 };
inline const juce::Colour silverDim     { 0xff8a9099 };
inline const juce::Colour silverDark    { 0xff565c65 };

//==============================================================================
// Accents
//==============================================================================
inline const juce::Colour gold          { 0xffc9a227 };
inline const juce::Colour goldBright    { 0xffe3c457 };
inline const juce::Colour goldDim       { 0xff8a711c };
inline const juce::Colour goldGlow      { 0x40c9a227 };

inline const juce::Colour success       { 0xff6fae7d };
inline const juce::Colour warning       { 0xffd09a3f };
inline const juce::Colour error         { 0xffc95b50 };
inline const juce::Colour info          { 0xff7c9db5 };

//==============================================================================
// Interaction
//==============================================================================
inline const juce::Colour hoverFill     { 0x0fffffff };
inline const juce::Colour pressedFill   { 0x08ffffff };
inline const juce::Colour selectionFill { 0x24c9a227 };   // gold @ 14%
inline const juce::Colour selectionLine { 0xffc9a227 };
inline const juce::Colour rowAlternate  { 0x06ffffff };
inline const juce::Colour shadow        { 0x66000000 };

//==============================================================================
// Metres
//==============================================================================
inline constexpr int sidebarWidth      = 232;
inline constexpr int sidebarMinWidth   = 196;
inline constexpr int sidebarMaxWidth   = 360;
inline constexpr int rightPanelWidth   = 360;
inline constexpr int rightPanelMinWidth = 300;
inline constexpr int rightPanelMaxWidth = 560;
inline constexpr int rightRailWidth    = 26;
inline constexpr int transportHeight   = 112;
inline constexpr int statusHeight      = 22;
inline constexpr int toolbarHeight     = 44;
inline constexpr int headerHeight      = 164;
inline constexpr int headerHeightFull  = 258;
inline constexpr int tableRowHeight    = 28;
inline constexpr int tableHeaderHeight = 30;

//==============================================================================
// Fonts
//==============================================================================
inline juce::Font uiFont (float height, bool bold = false, float extraKerning = 0.0f)
{
    juce::Font f (juce::FontOptions (juce::Font::getDefaultSansSerifFontName(), height,
                                     bold ? juce::Font::bold : juce::Font::plain));
    if (extraKerning != 0.0f)
        f = f.withExtraKerningFactor (extraKerning);
    return f;
}

inline juce::Font monoFont (float height, bool bold = false, float extraKerning = 0.0f)
{
    juce::Font f (juce::FontOptions ("Menlo", height,
                                     bold ? juce::Font::bold : juce::Font::plain));
    if (extraKerning != 0.0f)
        f = f.withExtraKerningFactor (extraKerning);
    return f;
}

/** Letterspaced capital labels - the "engraved" section headers. */
inline juce::Font capsFont (float height = 10.5f, float extraKerning = 0.16f)
{
    return uiFont (height, true, extraKerning);
}

//==============================================================================
// Small drawing primitives
//==============================================================================
inline void fillSurface (juce::Graphics& g, juce::Rectangle<int> area,
                         juce::Colour top, juce::Colour bottom,
                         juce::Colour outline = line)
{
    auto r = area.toFloat();
    juce::ColourGradient grad (top, r.getX(), r.getY(), bottom, r.getX(), r.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRect (r);
    g.setColour (outline);
    g.drawRect (r, 1.0f);
}

/** Standard raised instrument panel. */
inline void fillPanel (juce::Graphics& g, juce::Rectangle<int> area, bool alternate = false)
{
    fillSurface (g, area,
                 alternate ? theme::panelAlt.brighter (0.015f) : theme::panel.brighter (0.012f),
                 alternate ? theme::panelAlt.darker (0.18f) : theme::panel.darker (0.25f),
                 theme::line);
}

/** Recessed display area (lists, tables, readouts). */
inline void fillSunken (juce::Graphics& g, juce::Rectangle<int> area,
                        juce::Colour outline = theme::line)
{
    auto r = area.toFloat();
    juce::ColourGradient grad (theme::panelSunken.brighter (0.02f), r.getX(), r.getY(),
                               theme::panelSunken, r.getX(), r.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRect (r);
    g.setColour (outline);
    g.drawRect (r, 1.0f);
    // faint top inner shadow, like a machined recess
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillRect (r.withHeight (1.0f));
}

inline void drawHairline (juce::Graphics& g, int x1, int y1, int x2, int y2,
                          juce::Colour c = line)
{
    g.setColour (c);
    g.drawLine ((float) x1, (float) y1, (float) x2, (float) y2, 1.0f);
}

inline void drawSectionHeader (juce::Graphics& g, juce::Rectangle<int> area,
                               const juce::String& title,
                               const juce::String& rightText = {},
                               juce::Colour titleColour = textDim)
{
    g.setColour (titleColour);
    g.setFont (capsFont (10.5f));
    g.drawText (title.toUpperCase(), area, juce::Justification::centredLeft, false);

    if (rightText.isNotEmpty())
    {
        g.setColour (textFaint);
        g.setFont (monoFont (10.5f));
        g.drawText (rightText, area, juce::Justification::centredRight, false);
    }
}

/** Gold selection marker used on lists and tabs. */
inline void drawGoldMarker (juce::Graphics& g, juce::Rectangle<int> row)
{
    g.setColour (gold);
    g.fillRect (row.getX(), row.getY() + 3, 2, juce::jmax (1, row.getHeight() - 6));
}

/** Thin silver top highlight for "machined" edges. */
inline void drawEdgeHighlight (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawHorizontalLine (area.getY(), (float) area.getX(), (float) area.getRight());
}

inline juce::Colour withAlpha (juce::Colour c, float a) { return c.withAlpha (a); }

} // namespace sp::theme
