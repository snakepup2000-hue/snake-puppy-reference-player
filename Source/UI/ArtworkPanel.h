/*
    Snake Puppy Reference Player - album artwork display.
*/
#pragma once

#include "UI/Theme.h"
#include "UI/Widgets.h"

namespace sp
{

class ArtworkPanel : public juce::Component
{
public:
    ArtworkPanel();
    ~ArtworkPanel() override = default;

    void setImage (const juce::Image& newImage);
    void clearImage();
    juce::Image getImage() const { return image; }
    bool hasImage() const { return image.isValid(); }

    /** Text shown in the placeholder (e.g. album title); the first letter is
        also engraved into the placeholder disc. */
    void setPlaceholder (const juce::String& primary, const juce::String& secondary = {});

    void setCornerRadius (float);
    void setDrawFrame (bool);

    void paint (juce::Graphics&) override;

private:
    juce::Image image;
    juce::String placeholderTitle, placeholderSubtitle;
    float cornerRadius = 3.0f;
    bool drawFrame = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArtworkPanel)
};

} // namespace sp
