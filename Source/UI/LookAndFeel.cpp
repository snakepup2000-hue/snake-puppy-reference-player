/*
    Snake Puppy Reference Player - application LookAndFeel implementation.
*/
#include "UI/LookAndFeel.h"

namespace sp
{

namespace
{
constexpr float kCorner = 3.0f;

void drawChevronDown (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour c)
{
    juce::Path p;
    p.startNewSubPath (area.getX(), area.getY());
    p.lineTo (area.getCentreX(), area.getBottom());
    p.lineTo (area.getRight(), area.getY());
    g.setColour (c);
    g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded));
}
} // namespace

//==============================================================================
SpLookAndFeel::SpLookAndFeel()
{
    // Base identities.
    setColour (juce::ResizableWindow::backgroundColourId, theme::background);
    setColour (juce::DocumentWindow::textColourId, theme::silver);

    // Text buttons - the defaults; SpButton restyles its own colour ids.
    setColour (juce::TextButton::buttonColourId, theme::panelRaised);
    setColour (juce::TextButton::buttonOnColourId, theme::gold);
    setColour (juce::TextButton::textColourOffId, theme::silver);
    setColour (juce::TextButton::textColourOnId, theme::bgDeep);

    // Labels.
    setColour (juce::Label::textColourId, theme::text);
    setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textWhenEditingColourId, theme::text);
    setColour (juce::Label::backgroundWhenEditingColourId, theme::panelSunken);
    setColour (juce::Label::outlineWhenEditingColourId, theme::line);

    // Sliders.
    setColour (juce::Slider::backgroundColourId, theme::panelSunken);
    setColour (juce::Slider::trackColourId, theme::gold);
    setColour (juce::Slider::thumbColourId, theme::silver);
    setColour (juce::Slider::rotarySliderFillColourId, theme::gold);
    setColour (juce::Slider::rotarySliderOutlineColourId, theme::lineStrong);
    setColour (juce::Slider::textBoxTextColourId, theme::silver);
    setColour (juce::Slider::textBoxBackgroundColourId, theme::panelSunken);
    setColour (juce::Slider::textBoxHighlightColourId, theme::selectionFill);
    setColour (juce::Slider::textBoxOutlineColourId, theme::line);

    // Combo boxes.
    setColour (juce::ComboBox::backgroundColourId, theme::panelSunken);
    setColour (juce::ComboBox::textColourId, theme::text);
    setColour (juce::ComboBox::outlineColourId, theme::line);
    setColour (juce::ComboBox::buttonColourId, theme::panelAlt);
    setColour (juce::ComboBox::arrowColourId, theme::silverDim);
    setColour (juce::ComboBox::focusedOutlineColourId, theme::goldDim);

    // Popup menus.
    setColour (juce::PopupMenu::backgroundColourId, theme::panelAlt);
    setColour (juce::PopupMenu::textColourId, theme::text);
    setColour (juce::PopupMenu::headerTextColourId, theme::textDim);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::selectionFill);
    setColour (juce::PopupMenu::highlightedTextColourId, theme::goldBright);

    // Scroll bars.
    setColour (juce::ScrollBar::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::ScrollBar::thumbColourId, theme::silverDark);
    setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);

    // Text editors.
    setColour (juce::TextEditor::backgroundColourId, theme::panelSunken);
    setColour (juce::TextEditor::textColourId, theme::text);
    setColour (juce::TextEditor::highlightColourId, theme::selectionFill);
    setColour (juce::TextEditor::highlightedTextColourId, theme::goldBright);
    setColour (juce::TextEditor::outlineColourId, theme::line);
    setColour (juce::TextEditor::focusedOutlineColourId, theme::goldDim);
    setColour (juce::CaretComponent::caretColourId, theme::gold);

    // Tooltips.
    setColour (juce::TooltipWindow::backgroundColourId, theme::panelRaised);
    setColour (juce::TooltipWindow::textColourId, theme::text);
    setColour (juce::TooltipWindow::outlineColourId, theme::lineStrong);

    // Toggle buttons.
    setColour (juce::ToggleButton::textColourId, theme::silver);
    setColour (juce::ToggleButton::tickColourId, theme::gold);
    setColour (juce::ToggleButton::tickDisabledColourId, theme::textFaint);

    // Lists / tables.
    setColour (juce::ListBox::backgroundColourId, theme::panelSunken);
    setColour (juce::ListBox::outlineColourId, theme::line);
    setColour (juce::ListBox::textColourId, theme::text);
    setColour (juce::TableHeaderComponent::backgroundColourId, theme::panelAlt);
    setColour (juce::TableHeaderComponent::textColourId, theme::textDim);
    setColour (juce::TableHeaderComponent::outlineColourId, theme::line);
    setColour (juce::TableHeaderComponent::highlightColourId, theme::gold);

    // Alerts.
    setColour (juce::AlertWindow::backgroundColourId, theme::panelAlt);
    setColour (juce::AlertWindow::textColourId, theme::text);
    setColour (juce::AlertWindow::outlineColourId, theme::lineStrong);
}

SpLookAndFeel::~SpLookAndFeel() = default;

//==============================================================================
juce::Font SpLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return theme::uiFont (juce::jlimit (11.0f, 13.5f, (float) buttonHeight * 0.36f), true, 0.06f);
}

void SpLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                          const juce::Colour& backgroundColour,
                                          bool shouldDrawButtonAsHighlighted,
                                          bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = button.getToggleState();
    auto base = on ? button.findColour (juce::TextButton::buttonOnColourId) : backgroundColour;

    const bool isGold = base.getARGB() == theme::gold.getARGB();
    const bool isGhost = base.getAlpha() == 0;

    if (isGhost)
    {
        g.setColour (shouldDrawButtonAsDown ? theme::pressedFill
                                           : shouldDrawButtonAsHighlighted ? theme::hoverFill
                                                                           : juce::Colours::transparentBlack);
        g.fillRoundedRectangle (bounds, kCorner);
        g.setColour (button.findColour (juce::TextButton::textColourOffId).withAlpha (0.35f));
        g.drawRoundedRectangle (bounds, kCorner, 1.0f);
        return;
    }

    auto top = base.brighter (0.10f);
    auto bottom = base.darker (0.18f);
    if (shouldDrawButtonAsDown)
    {
        top = base.darker (0.10f);
        bottom = base.darker (0.24f);
    }
    else if (shouldDrawButtonAsHighlighted)
    {
        top = top.brighter (0.10f);
        bottom = bottom.brighter (0.06f);
    }

    juce::ColourGradient grad (top, bounds.getCentreX(), bounds.getY(),
                               bottom, bounds.getCentreX(), bounds.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (bounds, kCorner);

    // Top sheen line - the "machined" edge.
    g.setColour (juce::Colours::white.withAlpha (isGold ? 0.28f : 0.09f));
    g.drawLine (bounds.getX() + 1.0f, bounds.getY() + 1.0f,
                bounds.getRight() - 1.0f, bounds.getY() + 1.0f, 1.0f);

    g.setColour (isGold ? theme::goldBright.withAlpha (0.85f) : theme::lineStrong);
    g.drawRoundedRectangle (bounds, kCorner, 1.0f);
}

void SpLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                    bool, bool)
{
    const bool on = button.getToggleState();
    auto colour = on ? button.findColour (juce::TextButton::textColourOnId)
                     : button.findColour (juce::TextButton::textColourOffId);
    if (! button.isEnabled())
        colour = colour.withAlpha (0.4f);

    g.setColour (colour);
    g.setFont (getTextButtonFont (button, button.getHeight()));
    g.drawFittedText (button.getButtonText(), button.getLocalBounds().reduced (6, 2),
                      juce::Justification::centred, 1);
}

void SpLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                      bool highlighted, bool)
{
    auto bounds = button.getLocalBounds();
    const int boxSize = 15;
    auto box = juce::Rectangle<float> ((float) bounds.getX() + 1.0f,
                                       (float) bounds.getCentreY() - boxSize * 0.5f,
                                       (float) boxSize, (float) boxSize);

    g.setColour (theme::panelSunken);
    g.fillRoundedRectangle (box, 2.0f);
    g.setColour (button.getToggleState() ? theme::gold
                                         : highlighted ? theme::silverDark : theme::lineStrong);
    g.drawRoundedRectangle (box, 2.0f, 1.0f);

    if (button.getToggleState())
    {
        juce::Path tick;
        tick.startNewSubPath (box.getX() + 3.5f, box.getCentreY());
        tick.lineTo (box.getCentreX() - 1.0f, box.getBottom() - 4.5f);
        tick.lineTo (box.getRight() - 3.0f, box.getY() + 4.5f);
        g.setColour (theme::goldBright);
        g.strokePath (tick, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    }

    auto textArea = bounds.withTrimmedLeft (boxSize + 9);
    g.setColour (button.isEnabled() ? theme::silver : theme::textFaint);
    g.setFont (theme::uiFont (12.0f));
    g.drawFittedText (button.getButtonText(), textArea, juce::Justification::centredLeft, 1);
}

//==============================================================================
juce::Label* SpLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* label = LookAndFeel_V4::createSliderTextBox (slider);
    if (label != nullptr)
    {
        label->setFont (theme::monoFont (11.5f));
        label->setColour (juce::Label::textColourId, theme::silver);
        label->setColour (juce::Label::backgroundColourId, theme::panelSunken);
        label->setColour (juce::Label::outlineColourId, theme::line);
        label->setJustificationType (juce::Justification::centred);
    }
    return label;
}

void SpLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                      float sliderPosProportional, float rotaryStartAngle,
                                      float rotaryEndAngle, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const auto label = slider.getName();
    const bool hasLabel = label.isNotEmpty();

    if (hasLabel)
        bounds = bounds.withTrimmedBottom (15.0f);

    auto dial = bounds.reduced (4.0f);
    auto radius = juce::jmin (dial.getWidth(), dial.getHeight()) * 0.5f - 5.0f;
    if (radius < 5.0f)
        return;

    auto centre = dial.getCentre();
    const float angle = rotaryStartAngle
                        + juce::jlimit (0.0f, 1.0f, sliderPosProportional)
                              * (rotaryEndAngle - rotaryStartAngle);

    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;

    //== Tick ring ============================================================
    constexpr int numTicks = 21;
    for (int i = 0; i < numTicks; ++i)
    {
        const float t = (float) i / (float) (numTicks - 1);
        const float a = rotaryStartAngle + t * (rotaryEndAngle - rotaryStartAngle);
        const float inner = radius + 3.0f;
        const float outer = radius + (i % 5 == 0 ? 6.5f : 5.0f);
        auto pt1 = centre.getPointOnCircumference (inner, a);
        auto pt2 = centre.getPointOnCircumference (outer, a);
        const bool passed = bipolar
                                ? (a >= juce::jmin (angle, rotaryStartAngle + (rotaryEndAngle - rotaryStartAngle)
                                                                         * (float) ((0.0 - slider.getMinimum())
                                                                                    / (slider.getMaximum() - slider.getMinimum())))
                                       && a <= juce::jmax (angle, rotaryStartAngle + (rotaryEndAngle - rotaryStartAngle)
                                                                         * (float) ((0.0 - slider.getMinimum())
                                                                                    / (slider.getMaximum() - slider.getMinimum()))))
                                : (a <= angle);
        g.setColour (passed ? theme::gold.withAlpha (0.85f)
                            : (i % 5 == 0 ? theme::silverDark : theme::lineStrong));
        g.drawLine (pt1.x, pt1.y, pt2.x, pt2.y, i % 5 == 0 ? 1.4f : 1.0f);
    }

    //== Track ring ===========================================================
    const float ringThickness = juce::jlimit (2.5f, 4.0f, radius * 0.11f);
    juce::Rectangle<float> ring (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);
    g.setColour (theme::panelSunken);
    g.drawEllipse (ring, ringThickness);

    //== Value arc ============================================================
    auto arcAngleStart = rotaryStartAngle;
    if (bipolar)
    {
        const auto zeroProp = (float) ((0.0 - slider.getMinimum())
                                       / (slider.getMaximum() - slider.getMinimum()));
        arcAngleStart = rotaryStartAngle + zeroProp * (rotaryEndAngle - rotaryStartAngle);
    }

    if (std::abs (angle - arcAngleStart) > 0.001f)
    {
        juce::Path arc;
        arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                           juce::jmin (arcAngleStart, angle), juce::jmax (arcAngleStart, angle), true);
        g.setColour (theme::gold);
        g.strokePath (arc, juce::PathStrokeType (ringThickness, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    }

    //== Knob body ============================================================
    const float bodyRadius = radius - ringThickness - 3.0f;
    if (bodyRadius < 4.0f)
        return;

    juce::Rectangle<float> body (centre.x - bodyRadius, centre.y - bodyRadius,
                                 bodyRadius * 2.0f, bodyRadius * 2.0f);

    juce::ColourGradient bodyGrad (theme::panelRaised.brighter (0.22f),
                                   centre.x - bodyRadius * 0.35f, centre.y - bodyRadius * 0.45f,
                                   theme::panelSunken.brighter (0.02f),
                                   centre.x + bodyRadius * 0.45f, centre.y + bodyRadius * 0.55f,
                                   true);
    g.setGradientFill (bodyGrad);
    g.fillEllipse (body);

    // Concentric machining rings.
    g.setColour (theme::line.withAlpha (0.5f));
    g.drawEllipse (body.reduced (bodyRadius * 0.18f), 0.8f);
    g.setColour (theme::silverDark.withAlpha (0.55f));
    g.drawEllipse (body, 1.0f);

    // Pointer.
    auto ptrOuter = centre.getPointOnCircumference (bodyRadius * 0.86f, angle);
    auto ptrInner = centre.getPointOnCircumference (bodyRadius * 0.34f, angle);
    g.setColour (theme::goldBright);
    g.drawLine ({ ptrInner, ptrOuter }, juce::jmax (1.8f, bodyRadius * 0.09f));

    //== Centre value readout =================================================
    if (slider.getTextBoxPosition() == juce::Slider::NoTextBox)
    {
        auto valueFont = theme::monoFont (juce::jlimit (9.0f, 13.5f, bodyRadius * 0.34f), false);
        g.setColour (theme::text);
        g.setFont (valueFont);
        auto valueArea = body.reduced (bodyRadius * 0.42f).translated (0.0f, bodyRadius * 0.38f);
        g.drawFittedText (slider.getTextFromValue (slider.getValue()),
                          valueArea.toNearestInt(), juce::Justification::centred, 1);
    }

    //== Engraved label =======================================================
    if (hasLabel)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::capsFont (9.5f, 0.14f));
        g.drawText (label.toUpperCase(),
                    bounds.withTrimmedTop (bounds.getHeight() - 15.0f).toNearestInt(),
                    juce::Justification::centred, false);
    }
}

void SpLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                      float sliderPos, float minSliderPos, float maxSliderPos,
                                      juce::Slider::SliderStyle style, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const bool vertical = style == juce::Slider::LinearVertical
                          || style == juce::Slider::LinearBarVertical;

    const bool highlighted = slider.isMouseOverOrDragging();
    const auto activeColour = slider.isEnabled() ? theme::gold : theme::silverDark;

    if (vertical)
    {
        auto track = bounds.withSizeKeepingCentre (5.0f, bounds.getHeight()).reduced (0.0f, 2.0f);
        g.setColour (theme::panelSunken);
        g.fillRoundedRectangle (track, 2.5f);
        g.setColour (theme::line);
        g.drawRoundedRectangle (track, 2.5f, 1.0f);

        auto filled = track.withTop (juce::jlimit (track.getY(), track.getBottom(), sliderPos));
        g.setGradientFill (juce::ColourGradient (activeColour.darker (0.15f), 0.0f, filled.getY(),
                                                 activeColour.brighter (0.10f), 0.0f, filled.getBottom(),
                                                 false));
        g.fillRoundedRectangle (filled, 2.5f);

        // Machined thumb.
        auto thumb = juce::Rectangle<float> (bounds.getCentreX() - 11.0f, sliderPos - 5.5f, 22.0f, 11.0f);
        g.setGradientFill (juce::ColourGradient (theme::silver.brighter (0.18f), 0.0f, thumb.getY(),
                                                 theme::silverDark, 0.0f, thumb.getBottom(), false));
        g.fillRoundedRectangle (thumb, 2.0f);
        g.setColour (highlighted ? theme::goldBright : theme::lineStrong);
        g.drawRoundedRectangle (thumb, 2.0f, 1.0f);
        g.setColour (theme::background.withAlpha (0.55f));
        g.drawLine (thumb.getX() + 3.0f, thumb.getCentreY(), thumb.getRight() - 3.0f, thumb.getCentreY(), 1.0f);
        return;
    }

    auto track = bounds.withSizeKeepingCentre (bounds.getWidth(), 6.0f).reduced (2.0f, 0.0f);
    g.setColour (theme::panelSunken.brighter (0.04f));
    g.fillRoundedRectangle (track, 3.0f);
    g.setColour (highlighted ? theme::goldDim : theme::lineStrong);
    g.drawRoundedRectangle (track, 3.0f, 1.0f);

    auto filled = track.withRight (juce::jlimit (track.getX(), track.getRight(), sliderPos));
    g.setGradientFill (juce::ColourGradient (activeColour.darker (0.20f), filled.getX(), 0.0f,
                                             activeColour.brighter (0.15f), filled.getRight(), 0.0f,
                                             false));
    g.fillRoundedRectangle (filled, 3.0f);

    // Larger machined thumb so the seek/volume faders are unmistakable.
    auto thumb = juce::Rectangle<float> (sliderPos - 6.0f, bounds.getCentreY() - 11.0f, 12.0f, 22.0f);
    g.setGradientFill (juce::ColourGradient (theme::silver.brighter (0.22f), 0.0f, thumb.getY(),
                                             theme::silverDark, 0.0f, thumb.getBottom(), false));
    g.fillRoundedRectangle (thumb, 2.0f);
    g.setColour (highlighted ? theme::goldBright : theme::lineStrong);
    g.drawRoundedRectangle (thumb, 2.0f, 1.0f);
    g.setColour (theme::background.withAlpha (0.55f));
    g.drawLine (thumb.getCentreX(), thumb.getY() + 4.0f, thumb.getCentreX(), thumb.getBottom() - 4.0f, 1.0f);

    juce::ignoreUnused (minSliderPos, maxSliderPos);
}

//==============================================================================
void SpLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool,
                                  int, int, int, int, juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);
    juce::ColourGradient grad (theme::panelSunken.brighter (0.03f), 0.0f, 0.0f,
                               theme::panelSunken, 0.0f, (float) height, false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (bounds, kCorner);

    g.setColour (box.hasKeyboardFocus (true) ? theme::goldDim : theme::line);
    g.drawRoundedRectangle (bounds, kCorner, 1.0f);

    drawChevronDown (g, { (float) width - 16.0f, (float) height * 0.5f - 2.5f, 7.0f, 5.0f },
                     box.isEnabled() ? theme::silverDim : theme::textFaint);
}

juce::Font SpLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return theme::uiFont (juce::jlimit (11.0f, 13.0f, (float) box.getHeight() * 0.40f), false, 0.02f);
}

juce::Font SpLookAndFeel::getLabelFont (juce::Label& label)
{
    if (label.getProperties().contains ("spFont"))
        return label.getFont();

    return theme::uiFont (juce::jlimit (10.0f, 15.0f, (float) label.getHeight() * 0.62f));
}

//==============================================================================
void SpLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar& bar,
                                   int x, int y, int width, int height, bool isScrollbarVertical,
                                   int thumbStartPosition, int thumbSize,
                                   bool isMouseOver, bool isMouseDown)
{
    juce::ignoreUnused (bar);

    if (thumbSize <= 0)
        return;

    juce::Rectangle<float> thumb;
    if (isScrollbarVertical)
        thumb = { (float) x + (float) width * 0.5f - 3.0f, (float) thumbStartPosition, 6.0f, (float) thumbSize };
    else
        thumb = { (float) thumbStartPosition, (float) y + (float) height * 0.5f - 3.0f, (float) thumbSize, 6.0f };

    auto colour = isMouseDown ? theme::gold
                              : isMouseOver ? theme::silverDim
                                            : theme::silverDark;
    g.setColour (colour.withAlpha (isMouseOver || isMouseDown ? 0.95f : 0.7f));
    g.fillRoundedRectangle (thumb, 3.0f);
}

//==============================================================================
void SpLookAndFeel::drawMenuBarBackground (juce::Graphics& g, int width, int height,
                                           bool, juce::MenuBarComponent&)
{
    juce::Rectangle<int> area (0, 0, width, height);
    juce::ColourGradient grad (theme::panelAlt.darker (0.10f), 0.0f, 0.0f,
                               theme::panel.darker (0.20f), 0.0f, (float) height, false);
    g.setGradientFill (grad);
    g.fillRect (area);
    g.setColour (theme::line);
    g.drawHorizontalLine (height - 1, 0.0f, (float) width);
}

void SpLookAndFeel::drawMenuBarItem (juce::Graphics& g, int width, int height, int,
                                     const juce::String& itemText, bool isMouseOverItem,
                                     bool isMenuOpen, bool, juce::MenuBarComponent&)
{
    if (isMouseOverItem || isMenuOpen)
    {
        g.setColour (theme::selectionFill);
        g.fillRect (0, 0, width, height);
        g.setColour (theme::gold);
        g.fillRect (0, height - 2, width, 2);
    }

    g.setColour (isMenuOpen ? theme::goldBright
                            : isMouseOverItem ? theme::text : theme::textDim);
    g.setFont (theme::uiFont (12.0f, false, 0.04f));
    g.drawFittedText (itemText, juce::Rectangle<int> (0, 0, width, height).reduced (9, 1),
                      juce::Justification::centred, 1);
}

int SpLookAndFeel::getDefaultMenuBarHeight() { return 28; }

void SpLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    juce::Rectangle<float> bounds (0.0f, 0.0f, (float) width, (float) height);
    juce::ColourGradient grad (theme::panelAlt, 0.0f, 0.0f,
                               theme::panel.darker (0.15f), 0.0f, (float) height, false);
    g.setGradientFill (grad);
    g.fillRect (bounds);
    g.setColour (theme::lineStrong);
    g.drawRect (bounds, 1.0f);
}

void SpLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    juce::Rectangle<float> bounds (0.0f, 0.0f, (float) width, (float) height);
    g.setColour (theme::panelRaised);
    g.fillRoundedRectangle (bounds, 3.0f);
    g.setColour (theme::lineStrong);
    g.drawRoundedRectangle (bounds, 3.0f, 1.0f);
    g.setColour (theme::text);
    g.setFont (theme::uiFont (12.0f));
    g.drawFittedText (text, bounds.toNearestInt().reduced (8, 4), juce::Justification::centredLeft, 3);
}

//==============================================================================
void SpLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height,
                                              juce::TextEditor&)
{
    g.setColour (theme::panelSunken);
    g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height), 2.0f);
}

void SpLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height,
                                           juce::TextEditor& editor)
{
    auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    g.setColour (editor.hasKeyboardFocus (true) ? theme::goldDim : theme::line);
    g.drawRoundedRectangle (bounds, 2.0f, 1.0f);
}

//==============================================================================
juce::Font SpLookAndFeel::getAlertWindowMessageFont()
{
    return theme::uiFont (13.0f);
}

juce::Font SpLookAndFeel::getAlertWindowTitleFont()
{
    return theme::uiFont (17.0f, true, 0.05f);
}

void SpLookAndFeel::drawAlertBox (juce::Graphics& g, juce::AlertWindow& alert,
                                  const juce::Rectangle<int>& textArea, juce::TextLayout& layout)
{
    auto bounds = alert.getLocalBounds().toFloat().reduced (1.0f);
    juce::ColourGradient grad (theme::panelAlt, 0.0f, 0.0f,
                               theme::panel.darker (0.12f), 0.0f, bounds.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (theme::lineStrong);
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

    auto titleArea = textArea.withHeight (24).translated (0, -30);
    if (alert.getName().isNotEmpty() && titleArea.getY() > 0)
    {
        g.setColour (theme::gold);
        g.setFont (getAlertWindowTitleFont());
        g.drawText (alert.getName().toUpperCase(), titleArea, juce::Justification::centredLeft, false);
    }

    g.setColour (theme::text);
    layout.draw (g, textArea.toFloat());
}

//==============================================================================
juce::LookAndFeel& appLookAndFeel()
{
    return juce::LookAndFeel::getDefaultLookAndFeel();
}

} // namespace sp
