// Network output tests — real SSDP/SOAP/HTTP protocol self-test against the
// in-process mock DLNA renderer.
#include "TestFramework.h"
#include "Core/ApplicationPaths.h"
#include "Network/NetworkOutput.h"
#include "Network/PiBridgeOutput.h"

#include <JuceHeader.h>
#include <cstring>

namespace
{
juce::File makeTempRoot (const juce::String& name)
{
    auto root = juce::File::getCurrentWorkingDirectory()
                    .getChildFile ("build-lead")
                    .getChildFile ("test-tmp")
                    .getChildFile (name);
    root.deleteRecursively();
    root.createDirectory();
    return root;
}
} // namespace

SP_TEST (Network, SelfTestAgainstMockRenderer)
{
    auto root = makeTempRoot ("network-selftest");
    auto paths = sp::ApplicationPaths::forRoot (root);
    juce::String dirError;
    SP_REQUIRE_MSG (paths.ensureDirectoriesExist (dirError), dirError);

    sp::NetworkOutput network (paths);

    SP_REQUIRE (network.getVerifiedState() == sp::NetworkOutput::VerificationState::untested);

    auto result = network.runSelfTest();
    juce::StringArray noteArray;
    for (auto& n : result.notes)
        noteArray.add (n);
    auto notes = noteArray.joinIntoString (" | ");

    SP_REQUIRE_MSG (result.ssdpSearchWorks, notes);
    SP_REQUIRE_MSG (result.mockRendererDiscovered, notes);
    SP_REQUIRE_MSG (result.soapControlWorks, notes);
    SP_REQUIRE_MSG (result.httpRangeServerWorks, notes);
    SP_REQUIRE_MSG (result.passed(), notes);
    SP_REQUIRE (network.getVerifiedState() == sp::NetworkOutput::VerificationState::selfTestPassed);
    SP_REQUIRE (network.getVerificationDetail().isNotEmpty());
    SP_REQUIRE (! network.isMediaServerRunning());

    std::cout << "  [network self-test] " << notes.toStdString() << "\n";
}

SP_TEST (Network, CapabilitiesAreHonest)
{
    auto supported = sp::NetworkOutput::supportedTransports();
    auto unavailable = sp::NetworkOutput::unavailableTransports();
    SP_REQUIRE (supported.size() > 0);
    SP_REQUIRE (unavailable.size() >= 4);
    SP_REQUIRE (supported.joinIntoString (" ").containsIgnoreCase ("UPnP"));
    SP_REQUIRE (unavailable.joinIntoString (" ").containsIgnoreCase ("AirPlay"));
}

SP_TEST (Network, ConfigPersistence)
{
    auto root = makeTempRoot ("network-config");
    auto paths = sp::ApplicationPaths::forRoot (root);
    juce::String dirError;
    SP_REQUIRE_MSG (paths.ensureDirectoriesExist (dirError), dirError);

    sp::NetworkOutputConfig config;
    config.enabled = true;
    config.preferredRendererUdn = "uuid:test-renderer";
    config.httpPort = 49990;
    config.bindAddress = "127.0.0.1";
    config.autoReconnectOnPlay = false;
    config.manualHost = "192.0.2.10";

    {
        sp::NetworkOutput first (paths);
        first.setConfig (config);
    }

    sp::NetworkOutput second (paths);
    second.loadConfig();
    auto loaded = second.getConfig();
    SP_REQUIRE (loaded.enabled);
    SP_REQUIRE_MSG (loaded.preferredRendererUdn == "uuid:test-renderer", loaded.preferredRendererUdn);
    SP_REQUIRE (loaded.httpPort == 49990);
    SP_REQUIRE (loaded.bindAddress == "127.0.0.1");
    SP_REQUIRE (! loaded.autoReconnectOnPlay);
    SP_REQUIRE_MSG (loaded.manualHost == "192.0.2.10", loaded.manualHost);
}

SP_TEST (Network, MediaServerRangeServing)
{
    auto root = makeTempRoot ("network-media-server");
    auto paths = sp::ApplicationPaths::forRoot (root);
    juce::String dirError;
    SP_REQUIRE_MSG (paths.ensureDirectoriesExist (dirError), dirError);

    // Create a small deterministic file with a .wav extension.
    juce::MemoryBlock payload;
    payload.setSize (4096);
    for (int i = 0; i < 4096; ++i)
        payload[(size_t) i] = (char) (i * 37 % 251);

    auto file = root.getChildFile ("range-test.wav");
    file.replaceWithData (payload.getData(), payload.getSize());

    sp::NetworkOutput network (paths);
    juce::String error;
    SP_REQUIRE_MSG (network.startMediaServer (error), error);
    SP_REQUIRE (network.isMediaServerRunning());
    SP_REQUIRE (network.getMediaServerPort() > 0);

    auto url = network.makeMediaUrl (file);
    SP_REQUIRE (url.isWellFormed());

    auto fetchRange = [&] (const juce::String& range, int& statusOut, juce::MemoryBlock& out)
    {
        int status = 0;
        auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                 .withConnectionTimeoutMs (3000)
                                                 .withExtraHeaders ("Range: " + range + "\r\n")
                                                 .withStatusCode (&status));
        statusOut = status;
        out.reset();
        if (stream == nullptr)
            return false;
        juce::MemoryOutputStream mos;
        mos.writeFromInputStream (*stream, 1 << 20);
        out = juce::MemoryBlock (mos.getData(), mos.getDataSize());
        return true;
    };

    int status = 0;
    juce::MemoryBlock bytes;
    SP_REQUIRE (fetchRange ("bytes=100-199", status, bytes));
    SP_REQUIRE (status == 206);
    SP_REQUIRE (bytes.getSize() == 100);
    SP_REQUIRE (std::memcmp (bytes.getData(), static_cast<const char*> (payload.getData()) + 100, 100) == 0);

    SP_REQUIRE (fetchRange ("bytes=4000-", status, bytes));
    SP_REQUIRE (status == 206);
    SP_REQUIRE (bytes.getSize() == 96);

    SP_REQUIRE (fetchRange ("bytes=-16", status, bytes));
    SP_REQUIRE (status == 206);
    SP_REQUIRE (bytes.getSize() == 16);
    SP_REQUIRE (std::memcmp (bytes.getData(), static_cast<const char*> (payload.getData()) + 4080, 16) == 0);

    network.stopMediaServer();
    SP_REQUIRE (! network.isMediaServerRunning());
    file.deleteFile();
}

// Probes one explicit host; when nothing answers, the message must be honest.
SP_TEST (Network, HostProbeWithoutRendererIsHonest)
{
    auto root = makeTempRoot ("network-probe-fail");
    auto paths = sp::ApplicationPaths::forRoot (root);
    juce::String dirError;
    SP_REQUIRE_MSG (paths.ensureDirectoriesExist (dirError), dirError);

    sp::NetworkOutput network (paths);
    juce::String message;
    auto endpoint = network.probeHostBlocking ("127.0.0.1", 600, message);

    SP_REQUIRE_MSG (endpoint.avTransportControlUrl.isEmpty(), "unexpected renderer found");
    SP_REQUIRE_MSG (message.containsIgnoreCase ("No UPnP/DLNA MediaRenderer"), message.toStdString());
    SP_REQUIRE_MSG (message.contains ("127.0.0.1"), message.toStdString());
    SP_REQUIRE_MSG (network.getEndpoints().empty(), "failed probe must not add endpoints");
    std::cout << "  [host probe] " << message.toStdString() << "\n";
}

// A device description URL can be probed directly (used for manual hosts whose
// description path is known); verifies endpoint parsing and list insertion.
SP_TEST (Network, HostProbeAcceptsDescriptionUrl)
{
    auto root = makeTempRoot ("network-probe-url");
    auto paths = sp::ApplicationPaths::forRoot (root);
    juce::String dirError;
    SP_REQUIRE_MSG (paths.ensureDirectoriesExist (dirError), dirError);

    juce::StreamingSocket listener;
    SP_REQUIRE_MSG (listener.createListener (0, "127.0.0.1"), "Could not create test HTTP listener");
    const int port = listener.getBoundPort();
    SP_REQUIRE (port > 0);

    juce::String xml;
    xml << "<?xml version=\"1.0\"?>"
        << "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
        << "<specVersion><major>1</major><minor>0</minor></specVersion>"
        << "<device>"
        << "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
        << "<friendlyName>Pi Test Renderer</friendlyName>"
        << "<manufacturer>Test</manufacturer>"
        << "<modelName>MockPi</modelName>"
        << "<UDN>uuid:pi-test-renderer</UDN>"
        << "<serviceList><service>"
        << "<serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>"
        << "<controlURL>/upnp/control/AVTransport</controlURL>"
        << "<eventSubURL>/upnp/event/AVTransport</eventSubURL>"
        << "<SCPDURL>/AVTransport.xml</SCPDURL>"
        << "</service></serviceList>"
        << "</device></root>";

    std::atomic<bool> running { true };
    std::thread server ([&]
    {
        while (running.load())
        {
            if (listener.waitUntilReady (true, 100) <= 0)
                continue;
            auto* connection = listener.waitForNextConnection();
            if (connection == nullptr)
                continue;
            char buffer[4096] = {};
            connection->read (buffer, (int) sizeof (buffer) - 1, false);
            juce::String response;
            response << "HTTP/1.1 200 OK\r\n"
                     << "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                     << "Connection: close\r\n"
                     << "Content-Length: " << xml.getNumBytesAsUTF8() << "\r\n\r\n"
                     << xml;
            connection->write (response.toRawUTF8(), response.getNumBytesAsUTF8());
            connection->close();
            delete connection;
        }
    });

    sp::NetworkOutput network (paths);
    juce::String message;
    auto endpoint = network.probeHostBlocking ("http://127.0.0.1:" + juce::String (port) + "/device.xml",
                                               2500, message);

    running = false;
    if (server.joinable())
        server.join();
    listener.close();

    SP_REQUIRE_MSG (endpoint.avTransportControlUrl.isNotEmpty(), message.toStdString());
    SP_REQUIRE_MSG (endpoint.friendlyName == "Pi Test Renderer", endpoint.friendlyName);
    SP_REQUIRE_MSG (endpoint.avTransportControlUrl.contains ("AVTransport"), endpoint.avTransportControlUrl);
    SP_REQUIRE_MSG (network.getEndpoints().size() == 1, "probed endpoint not added to list");
    std::cout << "  [host probe url] " << message.toStdString() << "\n";
}

SP_TEST (Network, PiBridgeUpsamplesToDacMaximum)
{
    using P = sp::PiBridgeOutput;
    SP_REQUIRE (P::chooseReceiverRate (44100) == 352800);
    SP_REQUIRE (P::chooseReceiverRate (88200) == 352800);
    SP_REQUIRE (P::chooseReceiverRate (176400) == 352800);
    SP_REQUIRE (P::chooseReceiverRate (352800) == 352800);
    SP_REQUIRE (P::chooseReceiverRate (48000) == 384000);
    SP_REQUIRE (P::chooseReceiverRate (96000) == 384000);
    SP_REQUIRE (P::chooseReceiverRate (192000) == 384000);
    SP_REQUIRE (P::chooseReceiverRate (384000) == 384000);
    SP_REQUIRE (P::chooseReceiverRate (32000) == 384000);     // exact 12x
    SP_REQUIRE (P::chooseReceiverRate (22050) == 352800);     // exact 16x
    SP_REQUIRE (P::chooseReceiverRate (705600) == 352800);    // DSD256 PCM -> 2x down
    std::cout << "  [pibridge rates] 44.1k->" << P::chooseReceiverRate (44100)
              << " 48k->" << P::chooseReceiverRate (48000)
              << " DSD256->" << P::chooseReceiverRate (705600) << "\n";
}

// The direct PiBridge path: command construction and a local end-to-end render
// through a stand-in receiver (cat), so the decode -> DSP -> S32_LE pipeline is
// exercised without needing the Pi attached during the test run.
SP_TEST (Network, PiBridgeCommandAndLocalStream)
{
    sp::PiBridgeConfig config;
    config.sshTarget = "pi";
    config.receiver = "~/hqdac";
    config.alsaDevice = "hw:CARD=R2R,DEV=0";
    config.bufferUs = 20000;
    config.targetUs = 10000;

    const auto command = sp::PiBridgeOutput::buildCommand (
        config, sp::PiBridgeOutput::chooseReceiverRate (44100));
    SP_REQUIRE_MSG (command.contains ("ssh"), command.toStdString());
    SP_REQUIRE_MSG (command.contains ("pi"), command.toStdString());
    SP_REQUIRE_MSG (command.contains ("hqdac"), command.toStdString());
    SP_REQUIRE_MSG (command.contains ("-D hw:CARD=R2R,DEV=0"), command.toStdString());
    SP_REQUIRE_MSG (command.contains ("-f S32_LE"), command.toStdString());
    SP_REQUIRE_MSG (command.contains ("-r 352800"), command.toStdString());
    SP_REQUIRE_MSG (command.contains ("--buffer-time=20000"), command.toStdString());
    SP_REQUIRE_MSG (command.contains ("--target-time=10000"), command.toStdString());

    auto root = makeTempRoot ("pibridge-local");
    auto rawFile = root.getChildFile ("pcm.raw");
    config.commandOverride = "cat > " + juce::String (rawFile.getFullPathName().replace (" ", "\\ "));

    const auto fixture = juce::File (SP_SOURCE_DIR)
                             .getChildFile ("Tests/Fixtures/Library/Album B/01 - Rhythm Section.wav");
    SP_REQUIRE_MSG (fixture.existsAsFile(), fixture.getFullPathName().toStdString());

    sp::PiBridgeOutput bridge;
    juce::String error;
    sp::DspParams params;   // flat chain
    const bool started = bridge.start (fixture, params, -6.0f, false, config, error);
    SP_REQUIRE_MSG (started, error.toStdString());

    const auto deadline = juce::Time::getMillisecondCounter() + 15000;
    while (bridge.isRunning() && juce::Time::getMillisecondCounter() < deadline)
        juce::Thread::sleep (20);

    SP_REQUIRE_MSG (! bridge.isRunning(), "PiBridge stream did not finish in time");
    SP_REQUIRE_MSG (bridge.getState() == sp::PiBridgeOutput::State::finished,
                    bridge.getStatusText().toStdString());
    SP_REQUIRE (bridge.getFramesSent() > 0);
    SP_REQUIRE (rawFile.existsAsFile());
    SP_REQUIRE (rawFile.getSize() > 0);
    SP_REQUIRE_MSG (rawFile.getSize() % 8 == 0, "PCM size is not whole S32 stereo frames");
    SP_REQUIRE_MSG (rawFile.getSize() == bridge.getFramesSent() * 8, "PCM size does not match frames sent");

    std::cout << "  [pibridge] " << bridge.getStatusText().toStdString()
              << " bytes=" << (long long) rawFile.getSize() << "\n";
    rawFile.deleteFile();
}
