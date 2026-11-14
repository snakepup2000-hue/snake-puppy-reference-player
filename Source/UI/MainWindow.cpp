/*
    Snake Puppy Reference Player - main application window implementation.
*/
#include "UI/MainWindow.h"

#if __has_include("UI/DspStudioWindow.h")
    #include "UI/DspStudioWindow.h"
    #define SP_HAVE_DSP_STUDIO_WINDOW 1
#else
    #define SP_HAVE_DSP_STUDIO_WINDOW 0
#endif

#if __has_include("UI/SettingsWindow.h")
    #include "UI/SettingsWindow.h"
    #define SP_HAVE_SETTINGS_WINDOW 1
#else
    #define SP_HAVE_SETTINGS_WINDOW 0
#endif

namespace sp
{

MainWindow::MainWindow (AppContext& context)
    : juce::DocumentWindow ("Snake Puppy Reference Player", theme::background, DocumentWindow::allButtons),
      ctx (context)
{
    setUsingNativeTitleBar (true);
    setResizable (true, false);
    setResizeLimits (1100, 700, 12000, 12000);

    main = new MainComponent (ctx);
    setContentOwned (main, true);
    setMenuBar (main);

    restoreWindowState();
    lastNormalBounds = getBounds();

    setVisible (true);
    ready = true;
}

MainWindow::~MainWindow()
{
    saveWindowState();

#if SP_HAVE_DSP_STUDIO_WINDOW
    DspStudioWindow::destroyInstance();
#endif
#if SP_HAVE_SETTINGS_WINDOW
    SettingsWindow::destroyInstance();
#endif
}

void MainWindow::closeButtonPressed()
{
    saveWindowState();
    ctx.saveSession();
    if (auto* app = juce::JUCEApplication::getInstance())
        juce::MessageManager::callAsync ([app] { app->systemRequestedQuit(); });
}

void MainWindow::restoreWindowState()
{
    if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const auto userArea = display->userArea;
        const int width = juce::jmin (1440, userArea.getWidth() - 40);
        const int height = juce::jmin (900, userArea.getHeight() - 40);
        const juce::Rectangle<int> defaultBounds = userArea.withSizeKeepingCentre (width, height);

        bool maximised = false;
        auto bounds = ctx.settings().loadWindowBounds ("main", defaultBounds, maximised);

        if (maximised)
        {
            setBounds (userArea);
            return;
        }

        if (auto* target = juce::Desktop::getInstance().getDisplays().getDisplayForRect (bounds))
            bounds = bounds.constrainedWithin (target->userArea);
        else
            bounds = defaultBounds;

        if (bounds.getWidth() < 1100 || bounds.getHeight() < 700)
            bounds = defaultBounds;

        setBounds (bounds);
    }
    else
    {
        centreWithSize (1440, 900);
    }
}

bool MainWindow::isWindowMaximised() const
{
    if (isFullScreen())
        return true;

    if (auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getBounds()))
    {
        const auto userArea = display->userArea;
        return std::abs (getBounds().getX() - userArea.getX()) <= 4
               && std::abs (getBounds().getY() - userArea.getY()) <= 4
               && std::abs (getBounds().getWidth() - userArea.getWidth()) <= 4
               && std::abs (getBounds().getHeight() - userArea.getHeight()) <= 4;
    }
    return false;
}

void MainWindow::captureNormalBounds()
{
    if (! ready || isFullScreen())
        return;
    lastNormalBounds = getBounds();
}

void MainWindow::saveWindowState()
{
    juce::Rectangle<int> bounds = lastNormalBounds;
    if (bounds.isEmpty())
        bounds = getBounds();

    if (bounds.isEmpty())
        return;

    ctx.settings().saveWindowBounds ("main", bounds, isWindowMaximised());
    if (auto* component = main)
        component->saveLayout();
    ctx.settings().saveIfDirty();
}

void MainWindow::moved()
{
    juce::DocumentWindow::moved();
    captureNormalBounds();
}

void MainWindow::resized()
{
    juce::DocumentWindow::resized();
    captureNormalBounds();
}

} // namespace sp
