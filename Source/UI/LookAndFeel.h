/*
    Snake Puppy Reference Player - application LookAndFeel.

    Machined dark instrument: charcoal surfaces, engraved silver type, gold
    position marks. Every control the app and the DSP panels use is styled here.
*/
#pragma once

#include "UI/Theme.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace sp
{

class SpLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SpLookAndFeel();
    ~SpLookAndFeel() override;

    //== Buttons ==============================================================
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    //== Sliders ==============================================================
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;

    //== Combo boxes ==========================================================
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getLabelFont (juce::Label&) override;

    //== Scrollbars ===========================================================
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                        bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                        bool isMouseOver, bool isMouseDown) override;

    //== Menus / tooltips =====================================================
    void drawMenuBarBackground (juce::Graphics&, int width, int height, bool isMouseOverBar,
                                juce::MenuBarComponent&) override;
    void drawMenuBarItem (juce::Graphics&, int width, int height, int itemIndex,
                          const juce::String& itemText, bool isMouseOverItem, bool isMenuOpen,
                          bool isMouseOverBar, juce::MenuBarComponent&) override;
    int getDefaultMenuBarHeight() override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

    //== Text editors =========================================================
    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;

    //== Misc =================================================================
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowTitleFont() override;
    void drawAlertBox (juce::Graphics&, juce::AlertWindow&, const juce::Rectangle<int>& textArea,
                       juce::TextLayout&) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpLookAndFeel)
};

/** The look and feel every component is drawn with (installed by MainWindow). */
juce::LookAndFeel& appLookAndFeel();

} // namespace sp
