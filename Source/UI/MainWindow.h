/*
    Snake Puppy Reference Player - main application window.
    Owns the shell component, the menu bar and the persisted window bounds.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/MainComponent.h"

namespace sp
{

class MainWindow : public juce::DocumentWindow
{
public:
    explicit MainWindow (AppContext&);
    ~MainWindow() override;

    void closeButtonPressed() override;

    MainComponent& getMainComponent() { return *main; }

    void saveWindowState();

private:
    void moved() override;
    void resized() override;
    void captureNormalBounds();
    bool isWindowMaximised() const;
    void restoreWindowState();

    AppContext& ctx;
    MainComponent* main = nullptr;
    juce::TooltipWindow tooltipWindow { nullptr, 650 };
    juce::Rectangle<int> lastNormalBounds;
    bool ready = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

} // namespace sp
