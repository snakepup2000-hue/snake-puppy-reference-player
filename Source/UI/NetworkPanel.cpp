/*
    Snake Puppy Reference Player - network output panel.
    Message thread only (self-test on a worker thread).
*/
#include "UI/NetworkPanel.h"
#include "Network/PiBridgeOutput.h"

namespace sp
{

using ui::drawEmptyState;

NetworkPanel::NetworkPanel (AppContext& context) : ctx (context)
{
    setOpaque (true);

    addAndMakeVisible (endpointList);
    endpointList.setRowHeight (40);
    endpointList.setColour (juce::ListBox::backgroundColourId, theme::panelSunken);

    discoverButton.onClick = [this]
    {
        if (ctx.network().isDiscovering())
        {
            ctx.network().stopDiscovery();
            setResultStatus ("Discovery stopped.", false);
        }
        else
        {
            ctx.network().startDiscovery (3000);
            setResultStatus ("Searching for UPnP/DLNA media renderers...", false);
        }
        updateStatusTexts();
    };
    addAndMakeVisible (discoverButton);

    hostField.setText (ctx.network().getConfig().manualHost, false);
    hostField.setTextToShowWhenEmpty ("192.168.1.50", theme::textFaint);
    hostField.setFont (theme::monoFont (11.5f));
    hostField.setColour (juce::TextEditor::backgroundColourId, theme::panelSunken);
    hostField.setColour (juce::TextEditor::textColourId, theme::text);
    hostField.setColour (juce::TextEditor::outlineColourId, theme::line);
    hostField.setColour (juce::TextEditor::focusedOutlineColourId, theme::goldDim);
    hostField.setColour (juce::TextEditor::highlightColourId, theme::gold.withAlpha (0.22f));
    hostField.setTooltip ("Renderer host on your LAN (e.g. 192.168.1.50), or a device description URL");
    addAndMakeVisible (hostField);

    probeHostButton.onClick = [this] { startHostProbe(); };
    addAndMakeVisible (probeHostButton);

    selfTestButton.onClick = [this] { runSelfTest(); };
    addAndMakeVisible (selfTestButton);

    connectButton.onClick = [this] { connectSelected(); };
    addAndMakeVisible (connectButton);

    disconnectButton.onClick = [this]
    {
        ctx.network().disconnect();
        setResultStatus ("Disconnected from the renderer.", false);
        updateStatusTexts();
    };
    addAndMakeVisible (disconnectButton);

    playButton.onClick = [this] { playOnSelected(); };
    addAndMakeVisible (playButton);

    stopButton.onClick = [this] { stopNetworkPlayback(); };
    addAndMakeVisible (stopButton);

    piPlayButton.onClick = [this]
    {
        const auto track = ctx.engine().getStatus().currentTrack;
        if (track.path.isEmpty())
        {
            setResultStatus ("Load a track in the player first, then play it on the Pi K11.", true);
            return;
        }

        if (! ctx.isPiOutputSelected())
            ctx.setPiOutputSelected (true);
        if (! ctx.isPiBridgeStreaming())
        {
            juce::String error;
            startPiStream (track, error);
        }
        updateStatusTexts();
    };
    addAndMakeVisible (piPlayButton);

    piStopButton.onClick = [this]
    {
        ctx.setPiOutputSelected (false);
        setResultStatus (ctx.getPiBridgeStatus(), false);
        updateStatusTexts();
    };
    addAndMakeVisible (piStopButton);

    mediaServerButton.onClick = [this]
    {
        juce::String error;
        if (ctx.network().isMediaServerRunning())
        {
            ctx.network().stopMediaServer();
            setResultStatus ("Local HTTP media server stopped.", false);
        }
        else if (! ctx.network().startMediaServer (error))
        {
            setResultStatus ("Media server could not start: " + error, true);
        }
        else
        {
            setResultStatus ("Media server serving on " + ctx.network().getMediaServerBaseUrl(), false);
        }
        updateStatusTexts();
    };
    addAndMakeVisible (mediaServerButton);

    enableToggle.setClickingTogglesState (true);
    enableToggle.onClick = [this]
    {
        auto config = ctx.network().getConfig();
        config.enabled = enableToggle.getToggleState();
        ctx.network().setConfig (config);
        ctx.network().saveConfig();
        setResultStatus (config.enabled ? "Network output enabled." : "Network output disabled.",
                         false);
    };
    addAndMakeVisible (enableToggle);

    ui::configureSmallLabel (statusLabel, theme::textDim);
    statusLabel.setMinimumHorizontalScale (0.6f);

    ui::configureSmallLabel (resultLabel, theme::textDim);
    resultLabel.setMinimumHorizontalScale (0.6f);

    ui::configureSmallLabel (endpointDetailLabel, theme::silverDim);
    addAndMakeVisible (endpointDetailLabel);

    warningLabel.setText ("DLNA NETWORK PLAYBACK BYPASSES LOCAL DSP (the renderer decodes the original file). "
                          "The direct PI K11 (SSH) path is different: it streams DSP-processed PCM, so EQ, bass, "
                          "volume and the limiter apply. The K11 is single-tenant: quit HQPlayer/NAA first.",
                          juce::dontSendNotification);
    warningLabel.setFont (theme::uiFont (10.5f));
    warningLabel.setColour (juce::Label::textColourId, theme::warning);
    warningLabel.setMinimumHorizontalScale (0.7f);

    enableToggle.setToggleState (ctx.network().getConfig().enabled, juce::dontSendNotification);

    setSize (880, 700);
    refreshEndpointList();
    updateStatusTexts();
    startTimerHz (2);
}

NetworkPanel::~NetworkPanel()
{
    stopTimer();
    if (probeThread.joinable())
        probeThread.join();
    if (testThread.joinable())
        testThread.join();
}

//==============================================================================
void NetworkPanel::refreshNow()
{
    refreshEndpointList();
    updateStatusTexts();
    repaint();
}

void NetworkPanel::startHostProbe()
{
    if (probeRunning.exchange (true))
        return;

    auto config = ctx.network().getConfig();
    config.manualHost = hostField.getText().trim();
    ctx.network().setConfig (config);

    const auto target = config.manualHost;
    if (target.isEmpty())
    {
        probeRunning = false;
        setResultStatus ("Enter a renderer host address (for example 192.168.1.50).", true);
        return;
    }

    probeHostButton.setButtonText ("PROBING...");
    setResultStatus ("Probing " + target + " for a UPnP/DLNA renderer...", false);

    juce::Component::SafePointer<NetworkPanel> safeThis (this);
    if (probeThread.joinable())
        probeThread.join();

    probeThread = std::thread ([safeThis, target]
    {
        if (safeThis == nullptr)
            return;
        juce::String message;
        auto endpoint = safeThis->ctx.network().probeHostBlocking (target, 3000, message);
        if (safeThis == nullptr)
            return;

        juce::MessageManager::callAsync ([safeThis, message, endpoint]
        {
            if (safeThis == nullptr)
                return;
            safeThis->probeRunning = false;
            safeThis->probeHostButton.setButtonText ("PROBE HOST");
            const bool ok = endpoint.avTransportControlUrl.isNotEmpty();
            safeThis->setResultStatus (message, ! ok);
            safeThis->refreshEndpointList();
            safeThis->updateStatusTexts();
        });
    });
}

void NetworkPanel::timerCallback()
{
    refreshEndpointList();
    updateStatusTexts();

    const int piState = ctx.getPiBridgeState();
    if (piState == (int) sp::PiBridgeOutput::State::streaming
        || piState == (int) sp::PiBridgeOutput::State::preparing)
        setResultStatus (ctx.getPiBridgeStatus(), false);
    else if (piState == (int) sp::PiBridgeOutput::State::failed)
        setResultStatus (ctx.getPiBridgeStatus(), true);
}

void NetworkPanel::refreshEndpointList()
{
    auto latest = ctx.network().getEndpoints();

    juce::String signature;
    for (auto& e : latest)
        signature << e.udn << "|" << e.friendlyName << "|" << e.hostAddress << "|" << e.port
                  << "|" << (int) e.reachable << ";";

    if (signature != lastEndpointSignature)
    {
        lastEndpointSignature = signature;
        endpoints = std::move (latest);
        endpointList.updateContent();

        if (selectedRow >= (int) endpoints.size())
            selectedRow = endpoints.empty() ? -1 : 0;
        if (selectedRow < 0 && ! endpoints.empty())
            selectedRow = 0;
        endpointList.selectRow (selectedRow, /*dontScroll*/ true, true);
        repaint();
    }

    // Update selected row if the endpoint list changed underneath us.
    if (selectedRow >= 0 && selectedRow < (int) endpoints.size())
    {
        juce::String detail;
        const auto& e = endpoints[(size_t) selectedRow];
        detail << e.friendlyName;
        if (e.hostAddress.isNotEmpty())
            detail << "  |  " << e.hostAddress << (e.port > 0 ? ":" + juce::String (e.port) : "");
        if (e.manufacturer.isNotEmpty())
            detail << "  |  " << e.manufacturer;
        if (e.modelName.isNotEmpty())
            detail << " " << e.modelName;
        endpointDetailLabel.setText (detail, juce::dontSendNotification);
    }
    else
    {
        endpointDetailLabel.setText (endpoints.empty() ? "No renderers discovered yet - press DISCOVER."
                                                       : "Select a renderer.",
                                      juce::dontSendNotification);
    }
}

void NetworkPanel::updateStatusTexts()
{
    enableToggle.setToggleState (ctx.network().getConfig().enabled, juce::dontSendNotification);
    discoverButton.setButtonText (ctx.network().isDiscovering() ? "STOP DISCOVERY" : "DISCOVER");
    mediaServerButton.setButtonText (ctx.network().isMediaServerRunning() ? "STOP MEDIA SERVER"
                                                                         : "START MEDIA SERVER");

    const bool connected = ctx.network().isConnected();
    connectButton.setEnabled (! connected && selectedRow >= 0);
    disconnectButton.setEnabled (connected);
    playButton.setEnabled (selectedRow >= 0);
    stopButton.setEnabled (connected);

    juce::String status;
    status << ctx.network().getStatusText();
    const auto verification = ctx.network().getVerifiedState();
    if (verification == NetworkOutput::VerificationState::failed && status.isEmpty())
        status = "Network output has not been verified.";
    statusLabel.setText (status, juce::dontSendNotification);
}

void NetworkPanel::setResultStatus (const juce::String& message, bool warning)
{
    resultLabel.setText (message, juce::dontSendNotification);
    resultLabel.setColour (juce::Label::textColourId, warning ? theme::warning : theme::silverDim);
    resultIsWarning = warning;
    repaint();
}

void NetworkPanel::connectSelected()
{
    if (selectedRow < 0 || selectedRow >= (int) endpoints.size())
    {
        setResultStatus ("Select a renderer first.", true);
        return;
    }

    juce::String error;
    if (ctx.network().connectTo (endpoints[(size_t) selectedRow], error))
        setResultStatus ("Connected to " + endpoints[(size_t) selectedRow].friendlyName + ".", false);
    else
        setResultStatus ("Connection failed: " + error, true);

    updateStatusTexts();
    repaint();
}

bool NetworkPanel::startPiStream (const Track& track, juce::String& errorOut)
{
    if (! ctx.startPiBridgeStream (track, errorOut))
    {
        setResultStatus ("Pi K11: " + errorOut, true);
        return false;
    }
    setResultStatus (ctx.getPiBridgeStatus(), false);
    return true;
}

void NetworkPanel::playOnSelected()
{
    const auto track = ctx.engine().getStatus().currentTrack;
    if (track.path.isEmpty())
    {
        setResultStatus ("No track is loaded in the player.", true);
        return;
    }
    if (selectedRow < 0 || selectedRow >= (int) endpoints.size())
    {
        setResultStatus ("Select a renderer first.", true);
        return;
    }

    juce::String error;
    if (ctx.playOnNetworkRenderer (track, endpoints[(size_t) selectedRow], error))
        setResultStatus ("Sending \"" + track.displayTitle() + "\" to "
                             + endpoints[(size_t) selectedRow].friendlyName + " (local DSP bypassed).",
                         false);
    else
        setResultStatus ("Network playback failed: " + error, true);

    updateStatusTexts();
}

void NetworkPanel::stopNetworkPlayback()
{
    juce::String error;
    if (ctx.network().stop (error))
        setResultStatus ("Network playback stopped.", false);
    else
        setResultStatus ("Could not stop the renderer: " + error, true);
    updateStatusTexts();
}

//==============================================================================
void NetworkPanel::runSelfTest()
{
    if (testRunning.exchange (true))
        return;

    if (testThread.joinable())
        testThread.join();

    setResultStatus ("Running local UPnP self-test (SSDP + mock renderer + SOAP + HTTP range)...", false);
    testRunning = true;

    // The self-test is intentionally local and isolated; it is safe to run on a
    // worker so the UI keeps painting.
    juce::Component::SafePointer<NetworkPanel> safeThis (this);
    testThread = std::thread ([safeThis, this]
    {
        auto result = ctx.network().runSelfTest();

        juce::String notes;
        for (auto& n : result.notes)
            notes << (notes.isNotEmpty() ? "  " : "") << n;

        juce::MessageManager::callAsync ([safeThis, result, notes]
        {
            if (auto* panel = safeThis.getComponent())
                panel->onSelfTestFinished (result, notes);
        });
    });

    updateStatusTexts();
}

void NetworkPanel::onSelfTestFinished (const NetworkOutput::SelfTestResult& result,
                                       const juce::String& notes)
{
    testRunning = false;
    lastSelfTest = result;
    lastSelfTestNotes = notes;
    hasSelfTest = true;

    if (result.passed())
        setResultStatus ("Self-test passed: local UPnP/DLNA control and HTTP range server work.", false);
    else
        setResultStatus ("Self-test did not pass: " + (notes.isNotEmpty() ? notes
                                                                          : juce::String ("see the checks above.")),
                         true);

    updateStatusTexts();
    repaint();
}

juce::String NetworkPanel::verificationLabel() const
{
    switch (ctx.network().getVerifiedState())
    {
        case NetworkOutput::VerificationState::untested:          return "UNTESTED";
        case NetworkOutput::VerificationState::selfTestPassed:    return "SELF-TEST PASSED";
        case NetworkOutput::VerificationState::realDeviceConfirmed: return "REAL DEVICE CONFIRMED";
        case NetworkOutput::VerificationState::failed:            return "FAILED";
    }
    return "UNTESTED";
}

//==============================================================================
int NetworkPanel::getNumRows()
{
    return (int) endpoints.size();
}

void NetworkPanel::paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height,
                                     bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= (int) endpoints.size())
        return;

    const auto& e = endpoints[(size_t) rowNumber];
    juce::Rectangle<int> area (0, 0, width, height);

    if (rowIsSelected)
    {
        g.setColour (theme::selectionFill);
        g.fillRect (area);
        g.setColour (theme::gold);
        g.fillRect (0, area.getY() + 4, 2, area.getHeight() - 8);
    }
    else if (rowNumber % 2 == 1)
    {
        g.setColour (theme::rowAlternate);
        g.fillRect (area);
    }

    const auto led = juce::Point<float> (16.0f, (float) area.getCentreY());
    g.setColour (e.reachable ? theme::success : theme::textFaint);
    g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (led));

    auto text = area.withTrimmedLeft (30).withTrimmedRight (10);
    auto nameArea = text.removeFromTop (height / 2);
    g.setColour (rowIsSelected ? theme::goldBright : theme::text);
    g.setFont (theme::uiFont (12.0f, true));
    g.drawText (e.friendlyName.isNotEmpty() ? e.friendlyName : e.udn,
                nameArea, juce::Justification::bottomLeft, true);

    juce::String sub;
    if (e.hostAddress.isNotEmpty())
        sub << e.hostAddress << (e.port > 0 ? ":" + juce::String (e.port) : "");
    if (e.manufacturer.isNotEmpty())
        sub << (sub.isNotEmpty() ? "  |  " : "") << e.manufacturer;
    if (e.modelName.isNotEmpty())
        sub << " " << e.modelName;
    if (! e.reachable)
        sub << "  |  UNREACHABLE";

    g.setColour (theme::textDim);
    g.setFont (theme::uiFont (9.5f));
    g.drawText (sub, text, juce::Justification::topLeft, true);
}

void NetworkPanel::selectedRowsChanged (int lastRowSelected)
{
    selectedRow = lastRowSelected;
    connectButton.setEnabled (selectedRow >= 0 && ! ctx.network().isConnected());
    playButton.setEnabled (selectedRow >= 0);
    refreshEndpointList();
    repaint();
}

//==============================================================================
void NetworkPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    auto header = headerArea;
    theme::drawSectionHeader (g, header, "NETWORK OUTPUT", {});
    header.removeFromRight (176);

    // Verification chip.
    {
        auto chip = headerArea.withTrimmedLeft (headerArea.getWidth() - 172);
        const auto state = ctx.network().getVerifiedState();
        const auto colour = state == NetworkOutput::VerificationState::selfTestPassed
                                || state == NetworkOutput::VerificationState::realDeviceConfirmed
                                ? theme::success
                                : state == NetworkOutput::VerificationState::failed ? theme::error
                                                                                    : theme::textDim;
        g.setColour (colour.withAlpha (0.13f));
        g.fillRoundedRectangle (chip.reduced (0, 2).toFloat(), 2.5f);
        g.setColour (colour.withAlpha (0.7f));
        g.drawRoundedRectangle (chip.reduced (0, 2).toFloat(), 2.5f, 1.0f);
        g.setColour (colour);
        g.setFont (theme::capsFont (9.0f, 0.10f));
        g.drawText (verificationLabel(), chip, juce::Justification::centred, false);
    }

    // Warning banner.
    {
        theme::fillSunken (g, warningArea);
        g.setColour (theme::warning.withAlpha (0.8f));
        g.fillRect (warningArea.getX(), warningArea.getY(), 3, warningArea.getHeight());
        auto text = warningArea.reduced (12, 6);
        g.setColour (theme::warning);
        g.setFont (theme::capsFont (9.5f, 0.10f));
        g.drawText ("HEADS UP", text.removeFromTop (13), juce::Justification::centredLeft, false);
        g.setColour (theme::textDim);
        g.setFont (theme::uiFont (10.5f));
        g.drawFittedText (warningLabel.getText(), text, juce::Justification::topLeft, 2);
    }

    // Renderer detail line.
    theme::drawSectionHeader (g, detailArea, "SELECTED RENDERER", {});

    if (endpoints.empty() && ! listArea.isEmpty())
    {
        auto emptyArea = listArea.reduced (14, 18);
        theme::fillSunken (g, emptyArea);
        drawEmptyState (g, emptyArea, "NO RENDERERS DISCOVERED",
                        "Press DISCOVER to search for UPnP/DLNA media renderers on the local network. "
                        "Nothing is shown as available until it is actually found.",
                        theme::textDim);
    }

    // Status card.
    {
        theme::fillPanel (g, statusCard);
        auto inner = statusCard.reduced (10, 6);
        theme::drawSectionHeader (g, inner.removeFromTop (14), "OUTPUT STATUS", verificationLabel());

        juce::String text;
        text << "Discovery: " << (ctx.network().isDiscovering() ? "searching" : "idle") << "\n";
        text << "Renderers: " << (int) endpoints.size() << "\n";
        text << "Connected: " << (ctx.network().isConnected()
                                      ? ctx.network().getConnectedRendererName() : juce::String ("no")) << "\n";
        text << "Media server: " << (ctx.network().isMediaServerRunning()
                                          ? ctx.network().getMediaServerBaseUrl() : juce::String ("stopped")) << "\n";
        const auto detail = ctx.network().getVerificationDetail();
        text << "Verification: " << (detail.isNotEmpty() ? detail : juce::String ("not verified yet"));

        g.setColour (theme::textDim);
        g.setFont (theme::monoFont (9.5f));
        g.drawFittedText (text, inner, juce::Justification::topLeft,
                          juce::jmax (2, inner.getHeight() / 13));
    }

    // Result card.
    {
        theme::fillPanel (g, resultCard);
        auto inner = resultCard.reduced (10, 6);
        theme::drawSectionHeader (g, inner.removeFromTop (14), "SELF-TEST", hasSelfTest ? "LOCAL PROTOCOL TEST" : "");
        inner.removeFromTop (2);

        if (! hasSelfTest)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::uiFont (10.5f));
            g.drawFittedText ("Not tested yet. The self-test spins up a local mock DLNA renderer, "
                              "runs SSDP discovery and a SOAP AVTransport command, and fetches media "
                              "through the embedded HTTP range server. No network hardware required.",
                              inner, juce::Justification::topLeft, 4);
        }
        else
        {
            struct Check { const char* label; bool passed; };
            const Check checks[4] {
                { "SSDP M-SEARCH", lastSelfTest.ssdpSearchWorks },
                { "Mock renderer discovered", lastSelfTest.mockRendererDiscovered },
                { "SOAP AVTransport control", lastSelfTest.soapControlWorks },
                { "HTTP byte-range server", lastSelfTest.httpRangeServerWorks }
            };

            auto rowArea = inner.removeFromTop (juce::jmin (4 * 17, inner.getHeight()));
            for (auto& c : checks)
            {
                auto row = rowArea.removeFromTop (17);
                g.setColour (c.passed ? theme::success : theme::error);
                g.setFont (theme::monoFont (10.0f, true));
                g.drawText (c.passed ? juce::String::fromUTF8 ("\xe2\x9c\x93")
                                     : juce::String::fromUTF8 ("\xe2\x9c\x97"),
                            row.removeFromLeft (16), juce::Justification::centredLeft, false);
                g.setColour (c.passed ? theme::text : theme::textDim);
                g.setFont (theme::uiFont (10.0f));
                g.drawText (c.label, row, juce::Justification::centredLeft, true);
            }

            if (lastSelfTestNotes.isNotEmpty())
            {
                g.setColour (theme::textFaint);
                g.setFont (theme::uiFont (9.0f));
                g.drawFittedText (lastSelfTestNotes, inner, juce::Justification::topLeft,
                                  juce::jmax (1, inner.getHeight() / 12));
            }
        }
    }

    // Capabilities card.
    {
        theme::fillPanel (g, capabilityCard);
        auto inner = capabilityCard.reduced (10, 6);
        theme::drawSectionHeader (g, inner.removeFromTop (14), "TRANSPORTS", "IMPLEMENTED vs UNAVAILABLE");
        inner.removeFromTop (2);

        g.setFont (theme::capsFont (8.5f, 0.10f));
        g.setColour (theme::goldDim);
        g.drawText ("IMPLEMENTED", inner.removeFromTop (12), juce::Justification::centredLeft, false);

        for (auto& t : NetworkOutput::supportedTransports())
        {
            auto line = inner.removeFromTop (14);
            auto tick = line.removeFromLeft (14);
            g.setColour (theme::success);
            g.setFont (theme::monoFont (9.5f, true));
            g.drawText (juce::String::fromUTF8 ("\xe2\x9c\x93"), tick,
                        juce::Justification::centredLeft, false);
            g.setColour (theme::text);
            g.setFont (theme::uiFont (10.0f));
            g.drawText (t, line, juce::Justification::centredLeft, true);
        }

        inner.removeFromTop (4);
        g.setFont (theme::capsFont (8.5f, 0.10f));
        g.setColour (theme::textFaint);
        g.drawText ("UNAVAILABLE", inner.removeFromTop (12), juce::Justification::centredLeft, false);

        const auto unavailable = NetworkOutput::unavailableTransports();
        for (auto& t : unavailable)
        {
            auto line = inner.removeFromTop (juce::jmin (24, juce::jmax (0, inner.getHeight())));
            if (line.getHeight() <= 0)
                break;
            g.setColour (theme::textFaint);
            g.setFont (theme::monoFont (9.5f, true));
            g.drawText (juce::String::fromUTF8 ("\xe2\x9c\x97"), line.removeFromLeft (14),
                        juce::Justification::centredTop, false);
            g.setFont (theme::uiFont (9.0f));
            g.drawFittedText (t, line, juce::Justification::topLeft, 2);
        }
    }

    // Media server card.
    {
        theme::fillPanel (g, serverCard);
        auto inner = serverCard.reduced (10, 6);
        theme::drawSectionHeader (g, inner.removeFromTop (14), "LOCAL MEDIA SERVER", "");
        juce::String text;
        text << (ctx.network().isMediaServerRunning()
                     ? "Serving: " + ctx.network().getMediaServerBaseUrl()
                     : juce::String ("Stopped. The embedded HTTP server serves the source file to the renderer."));
        g.setColour (ctx.network().isMediaServerRunning() ? theme::success : theme::textFaint);
        g.setFont (theme::uiFont (9.5f));
        g.drawFittedText (text, inner, juce::Justification::topLeft, 2);
    }

    // Result line under the list.
    if (resultLabel.getText().isNotEmpty())
    {
        g.setColour (resultIsWarning ? theme::warning : theme::silverDim);
        g.setFont (theme::uiFont (10.0f));
        g.drawFittedText (resultLabel.getText(),
                          juce::Rectangle<int> (listArea.getX(), listArea.getBottom() + 6,
                                                buttonsArea.getWidth(), buttonsArea.getY() - listArea.getBottom() - 8),
                          juce::Justification::topLeft, 2);
    }
}

//==============================================================================
void NetworkPanel::resized()
{
    auto r = getLocalBounds().reduced (14, 10);
    headerArea = r.removeFromTop (22);
    r.removeFromTop (8);

    warningArea = r.removeFromTop (46);
    r.removeFromTop (10);

    detailArea = r.removeFromTop (18);
    endpointDetailLabel.setBounds (detailArea.withTrimmedLeft (176));
    r.removeFromTop (6);

    // Left column: renderer list + buttons.
    auto left = r.removeFromLeft (juce::jmax (420, (int) ((float) r.getWidth() * 0.56f)));
    r.removeFromLeft (10);
    auto right = r;

    auto leftBottom = left.removeFromBottom (196);
    listArea = left;
    endpointList.setBounds (listArea);
    buttonsArea = leftBottom.withTrimmedTop (44);   // leaves room for the 2-line result message

    {
        auto hostRow = buttonsArea.removeFromTop (28);
        hostField.setBounds (hostRow.removeFromLeft (juce::jmin (250, hostRow.getWidth() - 118)));
        hostRow.removeFromLeft (8);
        probeHostButton.setBounds (hostRow.removeFromLeft (110));

        buttonsArea.removeFromTop (6);

        auto row1 = buttonsArea.removeFromTop (30);
        discoverButton.setBounds (row1.removeFromLeft (120));
        row1.removeFromLeft (8);
        selfTestButton.setBounds (row1.removeFromLeft (140));
        row1.removeFromLeft (8);
        connectButton.setBounds (row1.removeFromLeft (110));
        row1.removeFromLeft (8);
        disconnectButton.setBounds (row1.removeFromLeft (120));

        buttonsArea.removeFromTop (8);
        auto row2 = buttonsArea.removeFromTop (30);
        playButton.setBounds (row2.removeFromLeft (176));
        row2.removeFromLeft (8);
        stopButton.setBounds (row2.removeFromLeft (88));
        row2.removeFromLeft (8);
        mediaServerButton.setBounds (row2.removeFromLeft (170));

        buttonsArea.removeFromTop (8);
        auto row3 = buttonsArea.removeFromTop (30);
        piPlayButton.setBounds (row3.removeFromLeft (200));
        row3.removeFromLeft (8);
        piStopButton.setBounds (row3.removeFromLeft (100));
    }

    // Right column cards (server pinned to the bottom so nothing is clipped).
    statusCard = right.removeFromTop (juce::jmin (132, juce::jmax (110, right.getHeight() / 4)));
    right.removeFromTop (8);
    resultCard = right.removeFromTop (juce::jmin (150, juce::jmax (120, right.getHeight() / 3)));
    right.removeFromTop (8);
    serverCard = right.removeFromBottom (66);
    right.removeFromBottom (8);
    capabilityCard = right;

    // Status card children: enable toggle.
    auto statusInner = statusCard.reduced (10, 6);
    statusInner.removeFromTop (16);
    auto toggleRow = statusInner.removeFromBottom (24);
    enableToggle.setBounds (toggleRow.removeFromLeft (juce::jmin (170, toggleRow.getWidth())));

    // Media server card children.
    auto serverInner = serverCard.reduced (10, 6);
    serverInner.removeFromTop (16);
    mediaServerButton.setBounds (serverInner.removeFromBottom (26));
}

} // namespace sp
