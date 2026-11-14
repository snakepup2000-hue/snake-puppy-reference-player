/*
    Snake Puppy Reference Player - shared instrument widgets implementation.
*/
#include "UI/Widgets.h"

namespace sp
{

//==============================================================================
// Icon geometry
//==============================================================================
namespace
{
float measureTextWidth (const juce::Font& font, const juce::String& text)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText (font, text, 0.0f, 0.0f);
    return ga.getBoundingBox (0, -1, true).getWidth();
}

juce::Path starPath (juce::Rectangle<float> area, int points = 5)
{
    juce::Path p;
    const auto centre = area.getCentre();
    const float outer = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f;
    const float inner = outer * 0.42f;
    for (int i = 0; i < points * 2; ++i)
    {
        const float angle = juce::MathConstants<float>::pi * -0.5f
                            + (float) i * juce::MathConstants<float>::pi / (float) points;
        const float r = (i % 2 == 0) ? outer : inner;
        auto pt = juce::Point<float> (centre.x + r * std::cos (angle),
                                      centre.y + r * std::sin (angle));
        if (i == 0) p.startNewSubPath (pt);
        else p.lineTo (pt);
    }
    p.closeSubPath();
    return p;
}

juce::Path makeIconPath (Icon icon)
{
    juce::Path p;
    using P = juce::Point<float>;

    switch (icon)
    {
        case Icon::play:
            p.addTriangle (7.0f, 4.5f, 19.5f, 12.0f, 7.0f, 19.5f);
            break;
        case Icon::pause:
            p.addRoundedRectangle (6.5f, 5.0f, 4.0f, 14.0f, 1.0f);
            p.addRoundedRectangle (13.5f, 5.0f, 4.0f, 14.0f, 1.0f);
            break;
        case Icon::stop:
            p.addRoundedRectangle (6.5f, 6.5f, 11.0f, 11.0f, 1.5f);
            break;
        case Icon::previous:
            p.addRoundedRectangle (4.5f, 5.0f, 2.6f, 14.0f, 1.0f);
            p.addTriangle (19.5f, 5.0f, 8.5f, 12.0f, 19.5f, 19.0f);
            break;
        case Icon::next:
            p.addRoundedRectangle (16.9f, 5.0f, 2.6f, 14.0f, 1.0f);
            p.addTriangle (4.5f, 5.0f, 15.5f, 12.0f, 4.5f, 19.0f);
            break;
        case Icon::search:
            p.addEllipse (4.0f, 4.0f, 12.5f, 12.5f);
            p.startNewSubPath (15.0f, 15.0f);
            p.lineTo (20.0f, 20.0f);
            break;
        case Icon::close:
            p.startNewSubPath (6.0f, 6.0f); p.lineTo (18.0f, 18.0f);
            p.startNewSubPath (18.0f, 6.0f); p.lineTo (6.0f, 18.0f);
            break;
        case Icon::settings:
        {
            p.addEllipse (8.5f, 8.5f, 7.0f, 7.0f);
            p.addEllipse (5.0f, 5.0f, 14.0f, 14.0f);
            for (int i = 0; i < 8; ++i)
            {
                const float a = (float) i * juce::MathConstants<float>::twoPi / 8.0f;
                p.startNewSubPath (12.0f + 6.2f * std::cos (a), 12.0f + 6.2f * std::sin (a));
                p.lineTo (12.0f + 9.2f * std::cos (a), 12.0f + 9.2f * std::sin (a));
            }
            break;
        }
        case Icon::dsp:
            for (int i = 0; i < 3; ++i)
            {
                const float yy = 6.0f + 6.0f * (float) i;
                p.startNewSubPath (3.5f, yy);
                p.lineTo (20.5f, yy);
            }
            p.addEllipse (6.0f, 3.5f, 5.0f, 5.0f);
            p.addEllipse (13.5f, 9.5f, 5.0f, 5.0f);
            p.addEllipse (7.5f, 15.5f, 5.0f, 5.0f);
            break;
        case Icon::rack:
        {
            const float heights[] { 9.0f, 15.0f, 6.0f, 12.0f, 8.0f };
            for (int i = 0; i < 5; ++i)
                p.addRoundedRectangle (4.0f + (float) i * 3.6f, 21.0f - heights[i], 2.2f, heights[i], 1.0f);
            break;
        }
        case Icon::volume:
            p.startNewSubPath (4.0f, 9.0f);
            p.lineTo (8.0f, 9.0f);
            p.lineTo (13.0f, 4.5f);
            p.lineTo (13.0f, 19.5f);
            p.lineTo (8.0f, 15.0f);
            p.lineTo (4.0f, 15.0f);
            p.closeSubPath();
            p.addCentredArc (13.0f, 12.0f, 5.0f, 5.0f, 0.0f, -0.9f, 0.9f, true);
            p.addCentredArc (13.0f, 12.0f, 8.5f, 8.5f, 0.0f, -0.8f, 0.8f, true);
            break;
        case Icon::mute:
            p.startNewSubPath (3.0f, 9.0f);
            p.lineTo (7.0f, 9.0f);
            p.lineTo (12.0f, 4.5f);
            p.lineTo (12.0f, 19.5f);
            p.lineTo (7.0f, 15.0f);
            p.lineTo (3.0f, 15.0f);
            p.closeSubPath();
            p.startNewSubPath (15.0f, 9.0f); p.lineTo (20.0f, 15.0f);
            p.startNewSubPath (20.0f, 9.0f); p.lineTo (15.0f, 15.0f);
            break;
        case Icon::chevronDown:
            p.startNewSubPath (7.0f, 10.0f); p.lineTo (12.0f, 15.0f); p.lineTo (17.0f, 10.0f);
            break;
        case Icon::chevronRight:
            p.startNewSubPath (10.0f, 7.0f); p.lineTo (15.0f, 12.0f); p.lineTo (10.0f, 17.0f);
            break;
        case Icon::plus:
            p.startNewSubPath (12.0f, 5.5f); p.lineTo (12.0f, 18.5f);
            p.startNewSubPath (5.5f, 12.0f); p.lineTo (18.5f, 12.0f);
            break;
        case Icon::minus:
            p.startNewSubPath (5.5f, 12.0f); p.lineTo (18.5f, 12.0f);
            break;
        case Icon::star:
            p = starPath ({ 3.0f, 3.0f, 18.0f, 18.0f });
            break;
        case Icon::folder:
            p.startNewSubPath (3.0f, 7.0f);
            p.lineTo (9.0f, 7.0f);
            p.lineTo (11.0f, 9.5f);
            p.lineTo (21.0f, 9.5f);
            p.lineTo (21.0f, 18.0f);
            p.lineTo (3.0f, 18.0f);
            p.closeSubPath();
            break;
        case Icon::queue:
            for (int i = 0; i < 3; ++i)
            {
                const float yy = 7.0f + 5.0f * (float) i;
                p.startNewSubPath (4.0f, yy); p.lineTo (14.0f, yy);
            }
            p.addTriangle (17.0f, 8.5f, 21.5f, 12.0f, 17.0f, 15.5f);
            break;
        case Icon::health:
            p.startNewSubPath (3.0f, 12.0f);
            p.lineTo (8.0f, 12.0f);
            p.lineTo (10.5f, 6.5f);
            p.lineTo (13.5f, 17.5f);
            p.lineTo (16.0f, 12.0f);
            p.lineTo (21.0f, 12.0f);
            break;
        case Icon::grid:
            p.addRoundedRectangle (3.5f, 3.5f, 7.0f, 7.0f, 1.0f);
            p.addRoundedRectangle (13.5f, 3.5f, 7.0f, 7.0f, 1.0f);
            p.addRoundedRectangle (3.5f, 13.5f, 7.0f, 7.0f, 1.0f);
            p.addRoundedRectangle (13.5f, 13.5f, 7.0f, 7.0f, 1.0f);
            break;
        case Icon::list:
            for (int i = 0; i < 3; ++i)
            {
                const float yy = 6.0f + 6.0f * (float) i;
                p.startNewSubPath (4.0f, yy); p.lineTo (20.0f, yy);
            }
            break;
        case Icon::sortUp:
            p.addTriangle (7.0f, 16.0f, 17.0f, 16.0f, 12.0f, 8.0f);
            break;
        case Icon::sortDown:
            p.addTriangle (7.0f, 8.0f, 17.0f, 8.0f, 12.0f, 16.0f);
            break;
        case Icon::warning:
            p.addTriangle (12.0f, 4.0f, 21.0f, 20.0f, 3.0f, 20.0f);
            p.startNewSubPath (12.0f, 9.5f); p.lineTo (12.0f, 14.5f);
            p.startNewSubPath (12.0f, 17.0f); p.lineTo (12.0f, 17.2f);
            break;
        case Icon::music:
            p.addEllipse (4.0f, 15.0f, 5.5f, 4.5f);
            p.startNewSubPath (9.2f, 17.0f);
            p.lineTo (9.2f, 5.5f);
            p.lineTo (19.0f, 4.0f);
            p.lineTo (19.0f, 14.0f);
            p.addEllipse (14.0f, 12.5f, 5.5f, 4.5f);
            break;
        case Icon::refresh:
            p.addCentredArc (12.0f, 12.0f, 7.0f, 7.0f, 0.0f, -1.2f, 4.2f, true);
            p.addTriangle (17.0f, 3.5f, 20.5f, 8.0f, 14.5f, 7.5f);
            break;
        case Icon::external:
            p.addRectangle (4.0f, 4.0f, 10.0f, 10.0f);
            p.startNewSubPath (10.5f, 13.5f); p.lineTo (20.0f, 4.0f);
            p.startNewSubPath (14.0f, 4.0f); p.lineTo (20.0f, 4.0f); p.lineTo (20.0f, 10.0f);
            break;
        case Icon::person:
            p.addEllipse (8.5f, 4.5f, 7.0f, 7.0f);
            p.startNewSubPath (4.5f, 20.0f);
            p.cubicTo (4.5f, 15.0f, 8.0f, 13.0f, 12.0f, 13.0f);
            p.cubicTo (16.0f, 13.0f, 19.5f, 15.0f, 19.5f, 20.0f);
            break;
        case Icon::calendar:
            p.addRoundedRectangle (4.0f, 6.0f, 16.0f, 14.0f, 1.5f);
            p.startNewSubPath (4.0f, 10.0f); p.lineTo (20.0f, 10.0f);
            p.startNewSubPath (8.0f, 3.5f); p.lineTo (8.0f, 7.5f);
            p.startNewSubPath (16.0f, 3.5f); p.lineTo (16.0f, 7.5f);
            break;
        case Icon::disc:
            p.addEllipse (3.5f, 3.5f, 17.0f, 17.0f);
            p.addEllipse (9.5f, 9.5f, 5.0f, 5.0f);
            break;
        case Icon::waveform:
            p.startNewSubPath (3.0f, 12.0f);
            p.lineTo (6.0f, 12.0f);
            p.lineTo (8.0f, 6.0f);
            p.lineTo (10.0f, 18.0f);
            p.lineTo (12.0f, 9.0f);
            p.lineTo (14.0f, 15.0f);
            p.lineTo (16.0f, 11.0f);
            p.lineTo (18.0f, 13.0f);
            p.lineTo (21.0f, 13.0f);
            break;
        case Icon::heart:
        {
            p.startNewSubPath (12.0f, 20.0f);
            p.cubicTo (4.0f, 14.0f, 3.0f, 9.0f, 6.0f, 6.0f);
            p.cubicTo (8.5f, 3.5f, 11.0f, 5.5f, 12.0f, 7.5f);
            p.cubicTo (13.0f, 5.5f, 15.5f, 3.5f, 18.0f, 6.0f);
            p.cubicTo (21.0f, 9.0f, 20.0f, 14.0f, 12.0f, 20.0f);
            p.closeSubPath();
            break;
        }
    }

    return p;
}

bool isFilledIcon (Icon icon)
{
    switch (icon)
    {
        case Icon::play: case Icon::pause: case Icon::stop:
        case Icon::previous: case Icon::next:
        case Icon::star: case Icon::sortUp: case Icon::sortDown:
        case Icon::heart: case Icon::dsp:
            return true;
        default:
            return false;
    }
}
} // namespace

void paintIcon (juce::Graphics& g, Icon icon, juce::Rectangle<float> area, juce::Colour colour,
                float strokeWidth)
{
    auto path = makeIconPath (icon);
    const auto scale = juce::jmin (area.getWidth(), area.getHeight()) / 24.0f;
    auto transform = juce::AffineTransform::scale (scale)
                        .translated (area.getCentreX() - 12.0f * scale,
                                     area.getCentreY() - 12.0f * scale);

    g.setColour (colour);
    if (isFilledIcon (icon))
    {
        g.fillPath (path, transform);
    }
    else
    {
        g.strokePath (path, juce::PathStrokeType (strokeWidth, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded),
                      transform);
    }
}

//==============================================================================
// SpButton
//==============================================================================
SpButton::SpButton (const juce::String& text, Style s) : juce::TextButton (text)
{
    setWantsKeyboardFocus (false);
    setStyle (s);
}

void SpButton::setStyle (Style s)
{
    style = s;
    setColour (juce::TextButton::textColourOffId, theme::silver);
    setColour (juce::TextButton::textColourOnId, theme::bgDeep);

    switch (s)
    {
        case Style::standard:
            setColour (juce::TextButton::buttonColourId, theme::panelRaised);
            setColour (juce::TextButton::buttonOnColourId, theme::gold);
            break;
        case Style::primary:
            setColour (juce::TextButton::buttonColourId, theme::gold);
            setColour (juce::TextButton::buttonOnColourId, theme::goldBright);
            setColour (juce::TextButton::textColourOffId, theme::bgDeep);
            setColour (juce::TextButton::textColourOnId, theme::bgDeep);
            break;
        case Style::ghost:
            setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
            setColour (juce::TextButton::buttonOnColourId, theme::gold);
            setColour (juce::TextButton::textColourOffId, theme::textDim);
            break;
        case Style::danger:
            setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
            setColour (juce::TextButton::buttonOnColourId, theme::error);
            setColour (juce::TextButton::textColourOffId, theme::error);
            break;
        case Style::toggle:
            setColour (juce::TextButton::buttonColourId, theme::panelRaised);
            setColour (juce::TextButton::buttonOnColourId, theme::gold);
            setClickingTogglesState (true);
            break;
    }
    repaint();
}

//==============================================================================
// SpIconButton
//==============================================================================
SpIconButton::SpIconButton (Icon i, const juce::String& tooltip)
    : juce::Button ("sp-icon")
{
    icon = i;
    setTooltip (tooltip);
    setWantsKeyboardFocus (false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void SpIconButton::setIcon (Icon newIcon)
{
    if (icon == newIcon) return;
    icon = newIcon;
    repaint();
}

void SpIconButton::setIconSize (float size)
{
    iconSize = size;
    repaint();
}

void SpIconButton::setProminent (bool shouldBeProminent)
{
    prominent = shouldBeProminent;
    repaint();
}

void SpIconButton::setOnColour (juce::Colour c)
{
    onColour = c;
    repaint();
}

void SpIconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto bounds = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = getToggleState();

    if (prominent)
    {
        auto circle = bounds.withSizeKeepingCentre (juce::jmin (bounds.getWidth(), bounds.getHeight()),
                                                   juce::jmin (bounds.getWidth(), bounds.getHeight()));
        auto top = down ? theme::gold.darker (0.20f)
                        : highlighted ? theme::goldBright : theme::gold;
        auto bottom = down ? theme::gold.darker (0.32f)
                           : highlighted ? theme::gold.darker (0.05f) : theme::goldDim;
        juce::ColourGradient grad (top, circle.getCentreX(), circle.getY(),
                                   bottom, circle.getCentreX(), circle.getBottom(), false);
        g.setGradientFill (grad);
        g.fillEllipse (circle);
        g.setColour (theme::goldBright.withAlpha (0.65f));
        g.drawEllipse (circle.reduced (0.5f), 1.0f);
        if (on)
        {
            g.setColour (theme::goldGlow);
            g.drawEllipse (circle.expanded (2.0f), 1.5f);
        }
        paintIcon (g, icon, circle.reduced (circle.getWidth() * 0.27f), theme::bgDeep, 1.8f);
        return;
    }

    if (on)
    {
        g.setColour (onColour.withAlpha (0.16f));
        g.fillRoundedRectangle (bounds, 3.0f);
        g.setColour (onColour.withAlpha (0.55f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.0f);
    }
    else if (highlighted)
    {
        g.setColour (down ? theme::pressedFill : theme::hoverFill);
        g.fillRoundedRectangle (bounds, 3.0f);
        g.setColour (theme::lineStrong.withAlpha (0.7f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.0f);
    }

    auto colour = ! isEnabled() ? theme::textFaint
                               : on ? onColour
                                    : highlighted ? theme::text : theme::silverDim;
    auto iconArea = bounds.withSizeKeepingCentre (iconSize, iconSize);
    paintIcon (g, icon, iconArea, colour, 1.7f);
}

//==============================================================================
// SpToggleButton
//==============================================================================
SpToggleButton::SpToggleButton (const juce::String& text) : juce::Button (text)
{
    setWantsKeyboardFocus (false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void SpToggleButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto bounds = getLocalBounds().toFloat();
    const bool on = getToggleState();

    if (highlighted || down)
    {
        g.setColour (down ? theme::pressedFill : theme::hoverFill);
        g.fillRoundedRectangle (bounds.reduced (0.5f), 3.0f);
    }

    const float textWidth = juce::jmin (bounds.getWidth() - 44.0f,
                                        measureTextWidth (theme::uiFont (11.5f), getButtonText().toUpperCase()) + 4.0f);
    auto textArea = bounds.withTrimmedRight (bounds.getWidth() - textWidth - 4.0f);
    g.setColour (! isEnabled() ? theme::textFaint : on ? theme::text : theme::textDim);
    g.setFont (theme::capsFont (11.0f, 0.08f));
    g.drawFittedText (getButtonText().toUpperCase(), textArea.toNearestInt(),
                      juce::Justification::centredLeft, 1);

    juce::Rectangle<float> pill (bounds.getRight() - 30.0f, bounds.getCentreY() - 8.0f, 28.0f, 16.0f);
    g.setColour (on ? theme::gold.withAlpha (0.22f) : theme::panelSunken);
    g.fillRoundedRectangle (pill, 8.0f);
    g.setColour (on ? theme::gold : highlighted ? theme::silverDim : theme::lineStrong);
    g.drawRoundedRectangle (pill, 8.0f, 1.0f);

    const float knobX = on ? pill.getRight() - 7.0f : pill.getX() + 7.0f;
    g.setColour (on ? theme::goldBright : theme::silverDim);
    g.fillEllipse (knobX - 5.0f, pill.getCentreY() - 5.0f, 10.0f, 10.0f);
}

//==============================================================================
// SpKnob / SpFader
//==============================================================================
SpKnob::SpKnob (const juce::String& labelText)
    : juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox)
{
    setLabelText (labelText);
    setDoubleClickReturnValue (true, 0.0);
    setMouseDragSensitivity (140);
    setWantsKeyboardFocus (false);
}

void SpKnob::setLabelText (const juce::String& t)
{
    setName (t);
    repaint();
}

SpFader::SpFader (const juce::String& labelText, bool vertical)
    : juce::Slider (vertical ? juce::Slider::LinearVertical : juce::Slider::LinearHorizontal,
                    juce::Slider::NoTextBox)
{
    setLabelText (labelText);
    setWantsKeyboardFocus (false);
}

void SpFader::setLabelText (const juce::String& t) { setName (t); repaint(); }

//==============================================================================
SpComboBox::SpComboBox (const juce::String& textWhenNothingSelected)
    : juce::ComboBox (textWhenNothingSelected)
{
    setWantsKeyboardFocus (false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

//==============================================================================
SpLabel::SpLabel (const juce::String& text, float fontHeight, juce::Colour colour,
                  juce::Justification justification)
    : juce::Label ({}, text)
{
    setTextStyle (fontHeight, colour);
    setJustificationType (justification);
    setInterceptsMouseClicks (false, false);
    setMinimumHorizontalScale (1.0f);
}

void SpLabel::setTextStyle (float fontHeight, juce::Colour colour, float extraKerning)
{
    setFont (theme::uiFont (fontHeight, false, extraKerning));
    setColour (juce::Label::textColourId, colour);
}

//==============================================================================
SectionHeader::SectionHeader (const juce::String& t, const juce::String& r)
    : title (t), right (r)
{
    setInterceptsMouseClicks (false, false);
}

void SectionHeader::setTitle (const juce::String& t) { title = t; repaint(); }
void SectionHeader::setRightText (const juce::String& t) { right = t; repaint(); }
void SectionHeader::setTitleColour (juce::Colour c) { titleColour = c; repaint(); }
void SectionHeader::setShowRule (bool b) { showRule = b; repaint(); }

void SectionHeader::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().reduced (0, 1);
    theme::drawSectionHeader (g, area, title, right, titleColour);
    if (showRule)
        theme::drawHairline (g, area.getX(), getLocalBounds().getBottom() - 1,
                             area.getRight(), getLocalBounds().getBottom() - 1);
}

//==============================================================================
Hairline::Hairline (juce::Colour c) : colour (c) {}

void Hairline::paint (juce::Graphics& g)
{
    g.fillAll (colour);
}

//==============================================================================
SpBadge::SpBadge (const juce::String& t, juce::Colour c) : text (t), colour (c)
{
    setInterceptsMouseClicks (false, false);
}

void SpBadge::setText (const juce::String& t) { text = t; repaint(); }
void SpBadge::setBadgeColour (juce::Colour c) { colour = c; repaint(); }
void SpBadge::setFilled (bool b) { filled = b; repaint(); }

int SpBadge::getPreferredWidth() const
{
    return juce::jmax (24, (int) std::ceil (measureTextWidth (theme::capsFont (9.5f), text.toUpperCase())) + 16);
}

void SpBadge::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (filled ? colour.withAlpha (0.85f) : colour.withAlpha (0.12f));
    g.fillRoundedRectangle (bounds, 2.0f);
    g.setColour (colour.withAlpha (filled ? 1.0f : 0.45f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 2.0f, 1.0f);
    g.setColour (filled ? theme::bgDeep : colour);
    g.setFont (theme::capsFont (9.5f, 0.12f));
    g.drawFittedText (text.toUpperCase(), getLocalBounds().reduced (6, 0),
                      juce::Justification::centred, 1);
}

//==============================================================================
SpSearchField::SpSearchField()
{
    addAndMakeVisible (editor);
    editor.setFont (theme::uiFont (12.5f));
    editor.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::textColourId, theme::text);
    editor.setColour (juce::TextEditor::highlightColourId, theme::selectionFill);
    editor.setColour (juce::TextEditor::highlightedTextColourId, theme::goldBright);
    editor.setJustification (juce::Justification::centredLeft);
    editor.setBorder ({ 0, 0, 0, 0 });
    editor.setIndents (24, 2);
    editor.setMouseCursor (juce::MouseCursor::IBeamCursor);

    editor.onTextChange = [this]
    {
        const bool shouldShowClear = editor.getText().isNotEmpty();
        if (shouldShowClear != showClear)
        {
            showClear = shouldShowClear;
            resized();
            repaint();
        }
        if (onTextChange)
            onTextChange (editor.getText());
    };

    editor.onEscapeKey = [this]
    {
        if (editor.getText().isNotEmpty())
            clearText();
        else if (onEscape)
            onEscape();
    };
}

SpSearchField::~SpSearchField() = default;

void SpSearchField::setPlaceholder (const juce::String& text)
{
    editor.setTextToShowWhenEmpty (text, theme::textFaint);
}

juce::String SpSearchField::getText() const { return editor.getText(); }

void SpSearchField::setText (const juce::String& text, bool sendChange)
{
    editor.setText (text, sendChange);
}

void SpSearchField::focusEditor()
{
    editor.grabKeyboardFocus();
    editor.selectAll();
}

void SpSearchField::clearText()
{
    editor.clear();
    if (onTextChange)
        onTextChange ({});
}

bool SpSearchField::isEditorFocused() const { return editor.hasKeyboardFocus (true); }

void SpSearchField::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (theme::panelSunken);
    g.fillRoundedRectangle (bounds, 3.0f);
    g.setColour (isEditorFocused() ? theme::goldDim : theme::line);
    g.drawRoundedRectangle (bounds, 3.0f, 1.0f);

    paintIcon (g, Icon::search, { bounds.getX() + 7.0f, bounds.getCentreY() - 7.0f, 14.0f, 14.0f },
               isEditorFocused() ? theme::gold : theme::silverDark, 1.5f);

    if (showClear)
    {
        clearArea = { (int) bounds.getRight() - 24, (int) bounds.getY() + 2, 20, (int) bounds.getHeight() - 4 };
        paintIcon (g, Icon::close, clearArea.toFloat().reduced (5.0f),
                   theme::silverDim, 1.4f);
    }
}

void SpSearchField::resized()
{
    auto area = getLocalBounds();
    editor.setBounds (area.withTrimmedLeft (4).withTrimmedRight (showClear ? 24 : 6).reduced (0, 3));
}

void SpSearchField::mouseDown (const juce::MouseEvent& e)
{
    if (showClear && clearArea.contains (e.getPosition()))
    {
        clearText();
        return;
    }
    focusEditor();
}

//==============================================================================
// ArtworkCache
//==============================================================================
ArtworkCache::ArtworkCache (LibraryService& lib)
    : juce::Thread ("sp-artwork"),
      library (lib),
      alive (std::make_shared<std::atomic<bool>> (true))
{
    startThread (juce::Thread::Priority::low);
}

ArtworkCache::~ArtworkCache()
{
    alive->store (false);
    signalThreadShouldExit();
    wake.signal();
    stopThread (2000);
}

juce::String ArtworkCache::keyFor (const Track& track)
{
    if (track.artworkId.isNotEmpty())
        return keyForArtworkId (track.artworkId);
    return "track:" + track.path;
}

juce::String ArtworkCache::keyForArtworkId (const juce::String& artworkId)
{
    return "art:" + artworkId;
}

void ArtworkCache::request (const Track& track)
{
    const auto key = keyFor (track);
    if (key.isEmpty())
        return;

    {
        const juce::ScopedLock sl (lock);
        if (cache.find (key.toStdString()) != cache.end())
            return;
    }

    Request r;
    r.key = key;
    r.artworkId = track.artworkId;
    r.track = track;

    {
        const juce::ScopedLock sl (lock);
        for (auto& existing : queue)
            if (existing.key == key)
                return;
        queue.push_back (std::move (r));
    }
    wake.signal();
}

juce::Image ArtworkCache::get (const juce::String& key) const
{
    const juce::ScopedLock sl (lock);
    auto it = cache.find (key.toStdString());
    if (it == cache.end())
        return {};
    return it->second;
}

void ArtworkCache::run()
{
    while (! threadShouldExit())
    {
        Request req;
        bool haveRequest = false;
        {
            const juce::ScopedLock sl (lock);
            if (! queue.empty())
            {
                req = std::move (queue.front());
                queue.erase (queue.begin());
                haveRequest = true;
            }
        }

        if (! haveRequest)
        {
            wake.wait (150);
            continue;
        }

        if (threadShouldExit())
            break;

        juce::Image image;
        juce::File file;
        if (req.artworkId.isNotEmpty())
            file = library.artworkFileFor (req.artworkId);

        if (file == juce::File() || ! file.existsAsFile())
            file = library.artworkForTrack (req.track);

        if (file.existsAsFile())
            image = juce::ImageFileFormat::loadFrom (file);

        {
            const juce::ScopedLock sl (lock);
            if (cache.size() > 512)
                cache.clear();
            cache[req.key.toStdString()] = image;
        }

        auto aliveFlag = alive;
        juce::MessageManager::callAsync ([this, aliveFlag]
        {
            if (aliveFlag->load() && onImageReady)
                onImageReady();
        });
    }
}

//==============================================================================
// Formatting helpers
//==============================================================================
juce::String formatDuration (std::int64_t ms)
{
    if (ms <= 0)
        return "--:--";

    const auto totalSeconds = ms / 1000;
    const auto hours = totalSeconds / 3600;
    const auto minutes = (totalSeconds % 3600) / 60;
    const auto seconds = totalSeconds % 60;

    if (hours > 0)
        return juce::String (hours) + ":" + juce::String (minutes).paddedLeft ('0', 2)
               + ":" + juce::String (seconds).paddedLeft ('0', 2);

    return juce::String (minutes) + ":" + juce::String (seconds).paddedLeft ('0', 2);
}

juce::String formatPosition (std::int64_t ms)
{
    if (ms < 0)
        ms = 0;

    const auto totalSeconds = ms / 1000;
    const auto hours = totalSeconds / 3600;
    const auto minutes = (totalSeconds % 3600) / 60;
    const auto seconds = totalSeconds % 60;
    const auto tenths = (ms % 1000) / 100;

    if (hours > 0)
        return juce::String (hours) + ":" + juce::String (minutes).paddedLeft ('0', 2)
               + ":" + juce::String (seconds).paddedLeft ('0', 2);

    return juce::String (minutes) + ":" + juce::String (seconds).paddedLeft ('0', 2)
           + "." + juce::String (tenths);
}

juce::String formatSampleRate (int hz)
{
    if (hz <= 0)
        return "-";

    if (hz >= 1000000)
    {
        const double mhz = (double) hz / 1000000.0;
        juce::String s (mhz, 4);
        return s.trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " MHz";
    }

    const double khz = (double) hz / 1000.0;
    juce::String s (khz, 3);
    if (s.containsChar ('.'))
    {
        s = s.trimCharactersAtEnd ("0");
        s = s.trimCharactersAtEnd (".");
    }
    return s + " kHz";
}

juce::String formatBitDepth (int bits, bool dsd, int dsdMultiple)
{
    if (dsd)
    {
        if (dsdMultiple > 0)
            return "DSD" + juce::String (dsdMultiple);
        return "DSD";
    }
    if (bits <= 0)
        return "-";
    return juce::String (bits) + "-bit";
}

juce::String formatTrackTechnical (const Track& t)
{
    juce::StringArray parts;
    if (t.formatName.isNotEmpty())
        parts.add (t.formatName.toUpperCase());
    if (t.isDsd)
        parts.add (formatBitDepth (0, true, t.dsdRateMultiple));
    else if (t.bitDepth > 0)
        parts.add (formatBitDepth (t.bitDepth));

    if (t.sampleRate > 0)
        parts.add (formatSampleRate (t.sampleRate));

    if (t.numChannels > 2)
        parts.add (juce::String (t.numChannels) + " ch");

    return parts.joinIntoString ("  |  ");
}

juce::String formatChannels (int numChannels)
{
    if (numChannels <= 0)
        return "-";
    if (numChannels == 1)
        return "Mono";
    if (numChannels == 2)
        return "Stereo";
    return juce::String (numChannels) + " channels";
}

juce::String formatDb (float db)
{
    if (db <= -99.5f)
        return "-inf dB";
    juce::String s (db, 1);
    if (! s.startsWithChar ('-'))
        s = "+" + s;
    return s + " dB";
}

juce::String formatCount (int n, const juce::String& singular, const juce::String& plural)
{
    auto word = (n == 1 || plural.isEmpty()) ? singular : plural;
    return juce::String (n) + " " + word;
}

juce::Colour qualityColour (SignalPathInfo::Quality q)
{
    switch (q)
    {
        case SignalPathInfo::Quality::bitPerfect:     return theme::gold;
        case SignalPathInfo::Quality::bitTransparent: return theme::success;
        case SignalPathInfo::Quality::processed:      return theme::warning;
        case SignalPathInfo::Quality::error:          return theme::error;
        case SignalPathInfo::Quality::unknown:        return theme::textDim;
    }
    return theme::textDim;
}

juce::String meteringText (float db)
{
    if (db <= -100.0f)
        return "-inf";
    return juce::String (db, 1);
}

//==============================================================================
void showTextInputDialog (const juce::String& title, const juce::String& message,
                          const juce::String& initialText,
                          std::function<void (const juce::String&)> onAccept)
{
    auto* window = new juce::AlertWindow (title, message, juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("text", initialText);
    window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window->enterModalState (true, juce::ModalCallbackFunction::create (
        [window, onAccept] (int result)
        {
            if (result == 1 && onAccept != nullptr)
            {
                auto text = window->getTextEditorContents ("text").trim();
                if (text.isNotEmpty())
                    onAccept (text);
            }
        }), true);
}

void showConfirmDialog (const juce::String& title, const juce::String& message,
                        const juce::String& acceptButtonText,
                        std::function<void()> onAccept)
{
    auto* window = new juce::AlertWindow (title, message, juce::MessageBoxIconType::WarningIcon);
    window->addButton (acceptButtonText.isNotEmpty() ? acceptButtonText : "OK", 1,
                       juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window->enterModalState (true, juce::ModalCallbackFunction::create (
        [onAccept] (int result)
        {
            if (result == 1 && onAccept != nullptr)
                onAccept();
        }), true);
}

//==============================================================================
void drawStars (juce::Graphics& g, juce::Rectangle<float> area, int rating,
                juce::Colour on, juce::Colour off, float starSize)
{
    const float gap = 2.0f;
    const int stars = 5;
    const float totalWidth = (float) stars * starSize + (float) (stars - 1) * gap;
    if (totalWidth > area.getWidth())
        starSize = (area.getWidth() - (float) (stars - 1) * gap) / (float) stars;

    float x = area.getCentreX() - ((float) stars * starSize + (float) (stars - 1) * gap) * 0.5f;
    const float y = area.getCentreY() - starSize * 0.5f;

    for (int i = 0; i < stars; ++i)
    {
        auto starArea = juce::Rectangle<float> (x, y, starSize, starSize);
        g.setColour (i < rating ? on : off);
        g.fillPath (starPath (starArea));
        x += starSize + gap;
    }
}

void drawPlayingGlyph (juce::Graphics& g, juce::Rectangle<float> area, bool playing,
                       juce::Colour colour)
{
    const float barWidth = juce::jmax (1.5f, area.getWidth() * 0.17f);
    const float gap = barWidth * 0.7f;
    const float totalWidth = barWidth * 3.0f + gap * 2.0f;
    float x = area.getCentreX() - totalWidth * 0.5f;
    const float heights[] { 0.45f, 0.9f, 0.62f };

    for (int i = 0; i < 3; ++i)
    {
        const float h = area.getHeight() * heights[i];
        g.setColour (playing ? colour : colour.withAlpha (0.35f));
        g.fillRoundedRectangle (x, area.getBottom() - h, barWidth, h, barWidth * 0.5f);
        x += barWidth + gap;
    }
}

} // namespace sp
