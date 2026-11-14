/*
    Snake Puppy Reference Player - library health panel.
*/
#pragma once

#include "Core/AppContext.h"
#include "UI/Widgets.h"
#include <atomic>
#include <memory>

namespace sp
{

class HealthPanel : public juce::Component,
                    private juce::Timer
{
public:
    explicit HealthPanel (AppContext&);
    ~HealthPanel() override;

    void runHealthCheck();
    void computeMissingHashes();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Results;

    void timerCallback() override;
    void applyReport (const HealthReport&);
    void setBusy (bool busy, const juce::String& what);

    AppContext& ctx;
    SpLabel titleLabel, metaLabel;
    SpButton runButton { "Run Health Check", SpButton::Style::primary };
    SpButton hashButton { "Compute Missing Hashes", SpButton::Style::standard };
    std::unique_ptr<Results> results;
    juce::Viewport viewport;

    HealthReport report;
    bool hasReport = false;
    bool busy = false;
    juce::String busyText;
    juce::String errorText;
    std::int64_t lastRunMs = 0;

    juce::ThreadPool pool { 1 };
    std::shared_ptr<std::atomic<bool>> alive;
    int spinnerFrame = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HealthPanel)
};

} // namespace sp
