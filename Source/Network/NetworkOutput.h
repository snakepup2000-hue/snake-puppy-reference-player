/*
    Snake Puppy Reference Player
    Network audio output.

    Implemented transport: UPnP/DLNA AVTransport control point (SSDP M-SEARCH
    discovery, device description fetch, SOAP AVTransport control) plus an
    embedded HTTP server with byte-range support that serves the source file to
    the renderer.

    Honest capability model:
      - The app discovers real MediaRenderers and can start/stop/seek/query
        playback on them.
      - Local DSP does NOT apply to network playback: the renderer fetches and
        decodes the original file itself.
      - `getVerifiedState()` is only "verified" after a self-test or a
        successful real command this session. Until then the UI must not claim
        network output works.
      - AirPlay / Snapcast / Roon / NAA are NOT implemented and are reported as
        unavailable rather than shown as working.
*/
#pragma once

#include "Core/ApplicationPaths.h"
#include "Core/Types.h"
#include <juce_events/juce_events.h>

namespace sp
{

struct NetworkEndpoint
{
    juce::String udn;
    juce::String friendlyName;
    juce::String manufacturer;
    juce::String modelName;
    juce::String hostAddress;
    int port = 0;
    juce::String descriptionUrl;
    juce::String avTransportControlUrl;
    juce::String connectionManagerControlUrl;
    bool reachable = false;
};

struct NetworkOutputConfig
{
    bool enabled = false;
    juce::String preferredRendererUdn;
    bool serveMediaOverHttp = true;
    int httpPort = 0;               // 0 = auto-pick a free port
    juce::String bindAddress = "0.0.0.0";
    bool autoReconnectOnPlay = true;
    /** Renderer host on the LAN (e.g. a Raspberry Pi running a UPnP/DLNA
        renderer), or a full device-description URL. Also probed by DISCOVER. */
    juce::String manualHost;
};

class NetworkOutput : public juce::ChangeBroadcaster
{
public:
    explicit NetworkOutput (const ApplicationPaths& paths);
    ~NetworkOutput() override;

    //== Discovery ============================================================
    /** Starts a background SSDP M-SEARCH discovery. Results arrive through
        change notifications; poll getEndpoints(). */
    void startDiscovery (int timeoutMs = 3000);
    void stopDiscovery();
    bool isDiscovering() const;
    std::vector<NetworkEndpoint> getEndpoints() const;

    /** Probes one explicit host (unicast SSDP M-SEARCH to port 1900) or a
        device-description URL. Blocking; call from a worker thread.
        Returns the discovered endpoint (empty on failure) and an honest
        status message in messageOut. */
    NetworkEndpoint probeHostBlocking (const juce::String& hostOrUrl, int timeoutMs,
                                       juce::String& messageOut);

    //== Configuration ========================================================
    NetworkOutputConfig getConfig() const;
    void setConfig (const NetworkOutputConfig&);
    void loadConfig();
    void saveConfig();

    //== Connection / control =================================================
    bool connectTo (const NetworkEndpoint&, juce::String& errorOut);
    void disconnect();
    bool isConnected() const;
    juce::String getConnectedRendererName() const;

    bool playUri (const juce::URL& mediaUri, const juce::String& title,
                  const juce::String& artist, const juce::String& album,
                  juce::String& errorOut);
    bool pause (juce::String& errorOut);
    bool stop (juce::String& errorOut);
    bool seekToMs (std::int64_t positionMs, juce::String& errorOut);
    bool queryTransportState (juce::String& stateOut, std::int64_t* positionMs = nullptr);

    //== Embedded HTTP media server ==========================================
    bool startMediaServer (juce::String& errorOut);
    void stopMediaServer();
    bool isMediaServerRunning() const;
    juce::String getMediaServerBaseUrl() const;
    juce::URL makeMediaUrl (const juce::File& file) const;
    int getMediaServerPort() const;

    /** Supplies the tracks shown on the phone/web page and restricts /media
        to files that are actually in the library. Call once after the library
        has loaded. */
    void setMediaLibraryProvider (std::function<std::vector<Track>()> provider);

    //== Capability + verification ===========================================
    static juce::StringArray supportedTransports();
    static juce::StringArray unavailableTransports();   // with reasons
    juce::StringArray getLastDiscoveryLog() const;

    enum class VerificationState { untested, selfTestPassed, realDeviceConfirmed, failed };
    VerificationState getVerifiedState() const;
    juce::String getVerificationDetail() const;

    struct SelfTestResult
    {
        bool ssdpSearchWorks = false;
        bool mockRendererDiscovered = false;
        bool soapControlWorks = false;
        bool httpRangeServerWorks = false;
        std::vector<juce::String> notes;
        bool passed() const;
    };
    /** Runs a fully local protocol test against a mock DLNA renderer that this
        method spins up (SSDP responder + SOAP + HTTP fetch). No network
        hardware required. */
    SelfTestResult runSelfTest();

    //== Status text for the UI ==============================================
    juce::String getStatusText() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkOutput)
};

} // namespace sp
