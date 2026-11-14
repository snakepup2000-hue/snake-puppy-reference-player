/*
    Snake Puppy Reference Player - shared instrument widgets.

    Small, style-first components used by the shell and by the DSP panels:
    engraved section headers, machined buttons, rotary knobs, faders, pills,
    badges, search field, plus the format/formatting helpers and the artwork
    cache. Everything here draws with the application LookAndFeel colours.
*/
#pragma once

#include "Library/LibraryService.h"
#include "UI/Theme.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace sp
{

//==============================================================================
// Icons
//==============================================================================
enum class Icon
{
    previous, play, pause, stop, next,
    search, close, settings, dsp, rack, volume, mute,
    chevronDown, chevronRight, plus, minus, star,
    folder, queue, health, grid, list, sortUp, sortDown,
    warning, music, refresh, external, heart, person, calendar, disc, waveform
};

void paintIcon (juce::Graphics&, Icon, juce::Rectangle<float> area, juce::Colour, float strokeWidth = 1.6f);

//==============================================================================
// Buttons
//==============================================================================
class SpButton : public juce::TextButton
{
public:
    enum class Style { standard, primary, ghost, danger, toggle };

    explicit SpButton (const juce::String& text = {}, Style style = Style::standard);
    void setStyle (Style);

    Style getStyle() const noexcept { return style; }

private:
    Style style = Style::standard;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpButton)
};

class SpIconButton : public juce::Button
{
public:
    SpIconButton (Icon icon, const juce::String& tooltip = {});
    ~SpIconButton() override = default;

    void setIcon (Icon newIcon);
    Icon getIcon() const noexcept { return icon; }
    void setIconSize (float size);
    /** Gold filled circle - the transport play button. */
    void setProminent (bool shouldBeProminent);
    /** Colour used when the button is toggled on (for toggles). */
    void setOnColour (juce::Colour);

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    Icon icon;
    float iconSize = 17.0f;
    bool prominent = false;
    juce::Colour onColour = theme::gold;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpIconButton)
};

/** Pill switch with a label, used for Reference Mode / A-B style toggles. */
class SpToggleButton : public juce::Button
{
public:
    explicit SpToggleButton (const juce::String& text = {});
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpToggleButton)
};

//==============================================================================
// Knobs / faders
//==============================================================================
/** Rotary control; the label is engraved under the dial and the value is shown
    in the machined centre. Configure range/suffix on the Slider directly. */
class SpKnob : public juce::Slider
{
public:
    explicit SpKnob (const juce::String& labelText = {});
    void setLabelText (const juce::String&);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpKnob)
};

/** Fader; horizontal by default, vertical when requested. */
class SpFader : public juce::Slider
{
public:
    explicit SpFader (const juce::String& labelText = {}, bool vertical = false);
    void setLabelText (const juce::String&);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpFader)
};

//==============================================================================
// Small components
//==============================================================================
class SpComboBox : public juce::ComboBox
{
public:
    explicit SpComboBox (const juce::String& textWhenNothingSelected = {});
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpComboBox)
};

class SpLabel : public juce::Label
{
public:
    SpLabel (const juce::String& text = {}, float fontHeight = 13.0f,
             juce::Colour colour = theme::text, juce::Justification = juce::Justification::centredLeft);
    void setTextStyle (float fontHeight, juce::Colour colour, float extraKerning = 0.0f);
};

class SectionHeader : public juce::Component
{
public:
    explicit SectionHeader (const juce::String& title = {}, const juce::String& rightText = {});
    void setTitle (const juce::String&);
    void setRightText (const juce::String&);
    void setTitleColour (juce::Colour);
    void setShowRule (bool);
    void paint (juce::Graphics&) override;

private:
    juce::String title, right;
    juce::Colour titleColour = theme::textDim;
    bool showRule = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SectionHeader)
};

class Hairline : public juce::Component
{
public:
    explicit Hairline (juce::Colour = theme::line);
    void paint (juce::Graphics&) override;

private:
    juce::Colour colour;
};

class SpBadge : public juce::Component
{
public:
    explicit SpBadge (const juce::String& text = {}, juce::Colour = theme::gold);
    void setText (const juce::String&);
    const juce::String& getText() const noexcept { return text; }
    void setBadgeColour (juce::Colour);
    void setFilled (bool);
    void paint (juce::Graphics&) override;
    int getPreferredWidth() const;

private:
    juce::String text;
    juce::Colour colour = theme::gold;
    bool filled = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpBadge)
};

class SpSearchField : public juce::Component
{
public:
    SpSearchField();
    ~SpSearchField() override;

    void setPlaceholder (const juce::String&);
    juce::String getText() const;
    void setText (const juce::String&, bool sendChange = false);
    void focusEditor();
    void clearText();
    bool isEditorFocused() const;

    std::function<void (const juce::String&)> onTextChange;
    std::function<void()> onEscape;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::TextEditor editor;
    bool showClear = false;
    juce::Rectangle<int> clearArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpSearchField)
};

//==============================================================================
// Artwork
//==============================================================================
/** Asynchronous artwork loader + small LRU-ish cache. All public methods are
    message thread only; decoding happens on an internal thread and results are
    delivered back on the message thread through onImageReady. */
class ArtworkCache : private juce::Thread
{
public:
    explicit ArtworkCache (LibraryService&);
    ~ArtworkCache() override;

    /** Ask for the artwork for a track. Call from the message thread. */
    void request (const Track&);

    /** Cached image for an artwork id (or track key), or a null image. */
    juce::Image get (const juce::String& key) const;
    static juce::String keyFor (const Track&);
    static juce::String keyForArtworkId (const juce::String& artworkId);

    /** Called on the message thread when new artwork is available. */
    std::function<void()> onImageReady;

private:
    void run() override;

    struct Request
    {
        juce::String key;
        juce::String artworkId;
        Track track;
    };

    LibraryService& library;
    juce::CriticalSection lock;
    std::vector<Request> queue;
    std::unordered_map<std::string, juce::Image> cache;
    std::shared_ptr<std::atomic<bool>> alive;
    juce::WaitableEvent wake;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArtworkCache)
};

//==============================================================================
// Formatting / drawing helpers
//==============================================================================
juce::String formatDuration (std::int64_t ms);                    // 4:05 / 1:02:03
juce::String formatPosition (std::int64_t ms);                    // 4:05.2 / 1:02:03.4
juce::String formatSampleRate (int hz);                           // 44.1 kHz / 2.8224 MHz
juce::String formatBitDepth (int bits, bool dsd = false, int dsdMultiple = 0);
juce::String formatTrackTechnical (const Track&);                 // FLAC | 24-bit | 96 kHz
juce::String formatChannels (int numChannels);
juce::String formatDb (float db);
juce::String formatCount (int n, const juce::String& singular, const juce::String& plural = {});
juce::Colour qualityColour (SignalPathInfo::Quality);
juce::String meteringText (float db);                             // "-inf" when silent

//== Asynchronous dialogs =======================================================
/** Text-entry dialog (message thread, non-blocking). */
void showTextInputDialog (const juce::String& title, const juce::String& message,
                          const juce::String& initialText,
                          std::function<void (const juce::String&)> onAccept);

/** Confirmation dialog (message thread, non-blocking). */
void showConfirmDialog (const juce::String& title, const juce::String& message,
                        const juce::String& acceptButtonText,
                        std::function<void()> onAccept);

void drawStars (juce::Graphics&, juce::Rectangle<float> area, int rating,
                juce::Colour on = theme::gold, juce::Colour off = theme::lineStrong,
                float starSize = 11.0f);

/** Draws the small "now playing" equaliser glyph used in track lists. */
void drawPlayingGlyph (juce::Graphics&, juce::Rectangle<float> area, bool playing,
                       juce::Colour = theme::gold);

} // namespace sp
