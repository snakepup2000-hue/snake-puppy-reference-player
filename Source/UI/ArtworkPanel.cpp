/*
    Snake Puppy Reference Player - album artwork display implementation.
*/
#include "UI/ArtworkPanel.h"

namespace sp
{

ArtworkPanel::ArtworkPanel()
{
    setInterceptsMouseClicks (false, false);
}

void ArtworkPanel::setImage (const juce::Image& newImage)
{
    image = newImage;
    repaint();
}

void ArtworkPanel::clearImage()
{
    image = {};
    repaint();
}

void ArtworkPanel::setPlaceholder (const juce::String& primary, const juce::String& secondary)
{
    placeholderTitle = primary;
    placeholderSubtitle = secondary;
    repaint();
}

void ArtworkPanel::setCornerRadius (float radius)
{
    cornerRadius = juce::jmax (0.0f, radius);
    repaint();
}

void ArtworkPanel::setDrawFrame (bool shouldDraw)
{
    drawFrame = shouldDraw;
    repaint();
}

void ArtworkPanel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    if (bounds.isEmpty())
        return;

    juce::Path clip;
    clip.addRoundedRectangle (bounds, cornerRadius);

    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (clip);

    // Background is always the recessed well.
    juce::ColourGradient back (theme::panelSunken.brighter (0.03f), bounds.getCentreX(), bounds.getY(),
                               theme::panelSunken, bounds.getCentreX(), bounds.getBottom(), false);
    g.setGradientFill (back);
    g.fillRect (bounds);

    if (image.isValid())
    {
        auto target = bounds.reduced (1.0f);
        const float scale = juce::jmin (target.getWidth() / (float) image.getWidth(),
                                        target.getHeight() / (float) image.getHeight());
        const float w = (float) image.getWidth() * scale;
        const float h = (float) image.getHeight() * scale;
        juce::Rectangle<float> dest (target.getCentreX() - w * 0.5f, target.getCentreY() - h * 0.5f, w, h);
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (image, dest);
    }
    else
    {
        // Machined "no artwork" disc: grooves, spindle and a faint note.
        const auto centre = bounds.getCentre();
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.42f;

        g.setColour (theme::line.withAlpha (0.55f));
        for (int i = 1; i <= 6; ++i)
            g.drawEllipse (centre.x - radius * (float) i / 6.0f,
                           centre.y - radius * (float) i / 6.0f,
                           radius * 2.0f * (float) i / 6.0f,
                           radius * 2.0f * (float) i / 6.0f,
                           0.8f);

        g.setColour (theme::panelRaised);
        g.fillEllipse (centre.x - radius * 0.30f, centre.y - radius * 0.30f,
                       radius * 0.60f, radius * 0.60f);
        g.setColour (theme::lineStrong);
        g.drawEllipse (centre.x - radius * 0.30f, centre.y - radius * 0.30f,
                       radius * 0.60f, radius * 0.60f, 1.0f);

        g.setColour (theme::gold.withAlpha (0.65f));
        g.fillEllipse (centre.x - 2.0f, centre.y - 2.0f, 4.0f, 4.0f);

        paintIcon (g, Icon::music, { centre.x - radius * 0.16f, centre.y + radius * 0.42f,
                                     radius * 0.32f, radius * 0.32f },
                   theme::silverDark.withAlpha (0.7f), 1.2f);

        if (placeholderTitle.isNotEmpty())
        {
            auto overlay = bounds.removeFromBottom (juce::jmin (bounds.getHeight() * 0.30f, 46.0f));
            juce::ColourGradient shade (juce::Colours::transparentBlack, overlay.getCentreX(), overlay.getY(),
                                        theme::bgDeep.withAlpha (0.85f), overlay.getCentreX(), overlay.getBottom(),
                                        false);
            g.setGradientFill (shade);
            g.fillRect (overlay);

            auto textArea = overlay.reduced (10.0f, 5.0f);
            g.setColour (theme::silver);
            g.setFont (theme::uiFont (12.5f, true));
            g.drawFittedText (placeholderTitle, textArea.removeFromTop (textArea.getHeight() * 0.6f).toNearestInt(),
                              juce::Justification::centredLeft, 1);

            if (placeholderSubtitle.isNotEmpty())
            {
                g.setColour (theme::textFaint);
                g.setFont (theme::capsFont (9.0f, 0.10f));
                g.drawFittedText (placeholderSubtitle.toUpperCase(), textArea.toNearestInt(),
                                  juce::Justification::centredLeft, 1);
            }
        }
    }

    if (drawFrame)
    {
        g.setColour (theme::lineStrong.withAlpha (0.8f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), cornerRadius, 1.0f);
        // top edge sheen
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawLine (bounds.getX() + cornerRadius, bounds.getY() + 1.0f,
                    bounds.getRight() - cornerRadius, bounds.getY() + 1.0f, 1.0f);
    }
}

} // namespace sp
