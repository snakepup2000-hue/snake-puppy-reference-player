/*
    Snake Puppy Reference Player - library health panel implementation.
*/
#include "UI/HealthPanel.h"

namespace sp
{

//==============================================================================
class HealthPanel::Results : public juce::Component
{
public:
    explicit Results (HealthPanel& o) : owner (o) { setInterceptsMouseClicks (false, false); }

    void updateSize()
    {
        const int width = juce::jmax (200, getParentComponent() != nullptr
                                               ? getParentComponent()->getWidth()
                                               : 600);
        setSize (width, juce::jmax (240, computeHeight (width)));
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().reduced (6, 4);
        g.setColour (theme::background);
        g.fillAll();

        if (! owner.hasReport)
        {
            auto guide = area.removeFromTop (juce::jmin (area.getHeight(), 220));
            paintIcon (g, Icon::health, { (float) guide.getCentreX() - 20.0f, (float) guide.getY() + 28.0f, 40.0f, 40.0f },
                       theme::silverDark, 1.6f);

            g.setColour (theme::textDim);
            g.setFont (theme::uiFont (16.0f, true));
            g.drawFittedText ("No health report yet",
                              guide.withTrimmedTop (82).withHeight (28),
                              juce::Justification::centred, 1);

            g.setColour (theme::textFaint);
            g.setFont (theme::uiFont (12.0f));
            g.drawFittedText (owner.errorText.isNotEmpty()
                                  ? owner.errorText
                                  : "Run a health check to scan for missing files, duplicate content, "
                                    "broken playlists and other library issues.",
                              guide.withTrimmedTop (114),
                              juce::Justification::centred, 3);
            return;
        }

        int y = area.getY() + 4;
        const int width = area.getWidth();

        auto section = [&] (const juce::String& title, int count, juce::Colour colour)
        {
            auto row = juce::Rectangle<int> (area.getX(), y, width, 26);
            g.setColour (colour);
            g.setFont (theme::capsFont (10.0f, 0.14f));
            g.drawFittedText (title.toUpperCase(), row, juce::Justification::centredLeft, 1);

            g.setColour (theme::textFaint);
            g.setFont (theme::monoFont (10.0f));
            g.drawFittedText (juce::String (count), row, juce::Justification::centredRight, 1);

            y += 26;
            g.setColour (theme::line);
            g.drawHorizontalLine (y - 1, (float) area.getX(), (float) area.getRight());
        };

        if (! owner.report.missingTracks.empty())
        {
            section ("Missing files", (int) owner.report.missingTracks.size(), theme::error);
            for (auto& missing : owner.report.missingTracks)
            {
                auto row = juce::Rectangle<int> (area.getX() + 4, y, width - 8, 38);
                g.setColour (theme::error);
                g.setFont (theme::uiFont (12.0f, true));
                g.drawFittedText (missing.title.isNotEmpty() ? missing.title : juce::String ("(unknown track)"),
                                  row.removeFromTop (18), juce::Justification::centredLeft, 1);
                g.setColour (theme::textFaint);
                g.setFont (theme::uiFont (10.5f));
                g.drawFittedText (missing.path, row, juce::Justification::centredLeft, 1);
                y += 38;
            }
            y += 10;
        }

        if (! owner.report.duplicateGroups.empty())
        {
            section ("Duplicate content", (int) owner.report.duplicateGroups.size(), theme::warning);
            auto library = owner.ctx.library().getSnapshot();
            for (auto& group : owner.report.duplicateGroups)
            {
                juce::String title;
                if (library != nullptr && ! group.ids.empty())
                    if (const auto* track = library->findTrack (group.ids.front()))
                        title = track->displayTitle() + "  -  " + track->displayArtist();

                if (title.isEmpty())
                    title = formatCount ((int) group.ids.size(), "copy", "copies");

                auto row = juce::Rectangle<int> (area.getX() + 4, y, width - 8, 36);
                g.setColour (theme::warning);
                g.setFont (theme::uiFont (12.0f));
                g.drawFittedText (title, row.removeFromTop (17), juce::Justification::centredLeft, 1);
                g.setColour (theme::textFaint);
                g.setFont (theme::uiFont (10.5f));
                g.drawFittedText (formatCount ((int) group.ids.size(), "copy", "copies")
                                      + (group.reason.isNotEmpty() ? "  |  " + group.reason : juce::String()),
                                  row, juce::Justification::centredLeft, 1);
                y += 36;
            }
            y += 10;
        }

        if (! owner.report.brokenPlaylists.empty())
        {
            section ("Broken playlists", (int) owner.report.brokenPlaylists.size(), theme::warning);
            for (auto& broken : owner.report.brokenPlaylists)
            {
                auto row = juce::Rectangle<int> (area.getX() + 4, y, width - 8, 24);
                g.setColour (theme::silver);
                g.setFont (theme::uiFont (12.0f));
                g.drawFittedText (broken.name, row, juce::Justification::centredLeft, 1);
                g.setColour (theme::warning);
                g.setFont (theme::monoFont (10.5f));
                g.drawFittedText (formatCount (broken.missingEntries, "missing entry", "missing entries"),
                                  row, juce::Justification::centredRight, 1);
                y += 24;
            }
            y += 10;
        }

        if (! owner.report.issues.empty())
        {
            section ("Other issues", (int) owner.report.issues.size(), theme::info);
            for (auto& issue : owner.report.issues)
            {
                auto row = juce::Rectangle<int> (area.getX() + 4, y, width - 8, 24);
                auto colour = issue.severity.equalsIgnoreCase ("critical") ? theme::error
                            : issue.severity.equalsIgnoreCase ("warning") ? theme::warning
                                                                          : theme::info;
                g.setColour (colour);
                g.setFont (theme::capsFont (9.0f, 0.08f));
                g.drawFittedText (issue.severity.toUpperCase(), row.removeFromLeft (70),
                                  juce::Justification::centredLeft, 1);
                g.setColour (theme::textDim);
                g.setFont (theme::uiFont (11.5f));
                g.drawFittedText (issue.message, row, juce::Justification::centredLeft, 2);
                y += 24;
            }
            y += 10;
        }

        if (totalIssues() == 0)
        {
            auto row = juce::Rectangle<int> (area.getX(), y + 20, width, 90);
            g.setColour (theme::success);
            g.setFont (theme::uiFont (18.0f, true));
            g.drawFittedText ("Library is healthy", row.removeFromTop (34), juce::Justification::centred, 1);
            g.setColour (theme::textFaint);
            g.setFont (theme::uiFont (12.0f));
            g.drawFittedText ("No missing files, duplicate content or broken playlists were found.",
                              row, juce::Justification::centred, 2);
        }
    }

    int totalIssues() const
    {
        return (int) (owner.report.missingTracks.size() + owner.report.duplicateGroups.size()
                      + owner.report.brokenPlaylists.size() + owner.report.issues.size());
    }

    int computeHeight (int width) const
    {
        juce::ignoreUnused (width);
        if (! owner.hasReport)
            return 260;

        int height = 16 + 26;
        height += (int) owner.report.missingTracks.size() * 38 + 10;
        height += (int) owner.report.duplicateGroups.size() * 36 + 10;
        height += (int) owner.report.brokenPlaylists.size() * 24 + 10;
        height += (int) owner.report.issues.size() * 24 + 10;
        if (totalIssues() == 0)
            height += 130;
        return height + 20;
    }

private:
    HealthPanel& owner;
};

//==============================================================================
HealthPanel::HealthPanel (AppContext& context)
    : ctx (context),
      alive (std::make_shared<std::atomic<bool>> (true))
{
    addAndMakeVisible (titleLabel);
    addAndMakeVisible (metaLabel);
    addAndMakeVisible (runButton);
    addAndMakeVisible (hashButton);
    addAndMakeVisible (viewport);

    titleLabel.setFont (theme::uiFont (19.0f, true));
    titleLabel.setColour (juce::Label::textColourId, theme::text);
    titleLabel.setText ("Library Health", juce::dontSendNotification);

    metaLabel.setFont (theme::uiFont (11.5f));
    metaLabel.setColour (juce::Label::textColourId, theme::textDim);
    metaLabel.setText ("Not checked yet", juce::dontSendNotification);

    results = std::make_unique<Results> (*this);
    viewport.setViewedComponent (results.get(), false);
    viewport.setScrollBarsShown (true, false, true, false);
    viewport.setScrollBarThickness (9);

    runButton.onClick = [this] { runHealthCheck(); };
    hashButton.onClick = [this] { computeMissingHashes(); };
    hashButton.setTooltip ("Hash tracks that have no content hash yet (needed for duplicate detection)");

    startTimerHz (2);
}

HealthPanel::~HealthPanel()
{
    stopTimer();
    if (alive != nullptr)
        alive->store (false);
    viewport.setViewedComponent (nullptr, false);
    results.reset();
    // The thread pool is joined when this object's members are destroyed.
}

//==============================================================================
void HealthPanel::runHealthCheck()
{
    if (busy)
        return;

    setBusy (true, "Scanning library metadata and files...");
    auto aliveFlag = alive;

    pool.addJob ([this, aliveFlag]
    {
        auto rep = ctx.library().runHealthCheck();
        juce::MessageManager::callAsync ([this, aliveFlag, rep]() mutable
        {
            if (! aliveFlag->load())
                return;
            setBusy (false, {});
            applyReport (rep);
        });
    });
}

void HealthPanel::computeMissingHashes()
{
    if (busy)
        return;

    setBusy (true, "Computing content hashes...");
    auto aliveFlag = alive;

    pool.addJob ([this, aliveFlag]
    {
        const int hashed = ctx.library().computeMissingHashes (0);
        juce::MessageManager::callAsync ([this, aliveFlag, hashed]
        {
            if (! aliveFlag->load())
                return;
            setBusy (false, {});
            ctx.setStatusMessage (hashed > 0 ? "Computed " + formatCount (hashed, "content hash", "content hashes") + "."
                                             : "All tracks already have content hashes.");
            metaLabel.setText (formatCount (hashed, "hash computed", "hashes computed"),
                               juce::dontSendNotification);
        });
    });
}

void HealthPanel::applyReport (const HealthReport& rep)
{
    report = rep;
    lastRunMs = juce::Time::currentTimeMillis();

    const bool hasData = ! report.missingTracks.empty() || ! report.duplicateGroups.empty()
                         || ! report.brokenPlaylists.empty() || ! report.issues.empty();

    errorText.clear();
    hasReport = hasData || report.generatedMs != 0;
    if (! hasReport)
        errorText = "The library service returned an empty report. Health checking is not "
                    "available in this build yet.";

    const auto checkedAt = juce::Time::getCurrentTime().formatted ("%H:%M:%S");
    if (hasReport)
    {
        const int issues = (int) (report.missingTracks.size() + report.duplicateGroups.size()
                                  + report.brokenPlaylists.size() + report.issues.size());
        metaLabel.setText (issues == 0 ? "Last checked " + checkedAt + "  |  no issues found"
                                       : "Last checked " + checkedAt + "  |  "
                                             + formatCount (issues, "issue") + " found",
                           juce::dontSendNotification);
    }
    else
    {
        metaLabel.setText ("Last checked " + checkedAt, juce::dontSendNotification);
    }

    resized();
    repaint();
}

void HealthPanel::setBusy (bool isBusy, const juce::String& what)
{
    busy = isBusy;
    busyText = what;
    runButton.setEnabled (! busy);
    hashButton.setEnabled (! busy);
    if (busy)
        metaLabel.setText (what, juce::dontSendNotification);
    repaint();
}

//==============================================================================
void HealthPanel::timerCallback()
{
    if (busy)
    {
        ++spinnerFrame;
        repaint();
    }
}

void HealthPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    auto header = getLocalBounds().removeFromTop (70);
    juce::ColourGradient grad (theme::panel.brighter (0.015f), 0.0f, 0.0f,
                               theme::panel.darker (0.10f), 0.0f, (float) header.getHeight(), false);
    g.setGradientFill (grad);
    g.fillRect (header);
    g.setColour (theme::line);
    g.drawHorizontalLine (header.getBottom() - 1, 0.0f, (float) header.getWidth());

    if (busy)
    {
        auto overlay = getLocalBounds();
        g.setColour (theme::bgDeep.withAlpha (0.55f));
        g.fillRect (overlay);

        auto box = juce::Rectangle<int> (0, 0, 320, 74).withCentre (overlay.getCentre());
        g.setColour (theme::panelRaised);
        g.fillRoundedRectangle (box.toFloat(), 4.0f);
        g.setColour (theme::lineStrong);
        g.drawRoundedRectangle (box.toFloat().reduced (0.5f), 4.0f, 1.0f);

        const int dots = (spinnerFrame / 2) % 4;
        juce::String text = busyText + juce::String (juce::String::repeatedString (".", dots));
        g.setColour (theme::silver);
        g.setFont (theme::uiFont (13.0f));
        g.drawFittedText (text, box.reduced (16, 8), juce::Justification::centred, 2);
    }
}

void HealthPanel::resized()
{
    auto area = getLocalBounds();
    auto header = area.removeFromTop (70).reduced (20, 10);
    auto row = header.removeFromTop (32);

    hashButton.setBounds (row.removeFromRight (172).withSizeKeepingCentre (172, 30));
    row.removeFromRight (8);
    runButton.setBounds (row.removeFromRight (138).withSizeKeepingCentre (138, 30));
    row.removeFromRight (16);
    titleLabel.setBounds (row.withTrimmedTop (2));

    auto metaRow = header.removeFromTop (22);
    metaLabel.setBounds (metaRow);

    viewport.setBounds (area.reduced (8, 4));
    if (results != nullptr)
        results->setSize (juce::jmax (200, viewport.getMaximumVisibleWidth()),
                          juce::jmax (viewport.getHeight(), results->computeHeight (getWidth())));
}

} // namespace sp
