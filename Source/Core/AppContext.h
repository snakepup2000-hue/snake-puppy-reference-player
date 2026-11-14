/*
    Snake Puppy Reference Player
    Application context: owns every service, wires cross-service behaviour
    (track change -> profile decision -> DSP transition -> ReplayGain), persists
    the session, and exposes the coordinator API the UI calls.

    This file is owned by the Lead. UI code must not edit it; ask for additions.
*/
#pragma once

#include "Audio/AudioEngine.h"
#include "Core/AppSettings.h"
#include "Core/ApplicationPaths.h"
#include "Core/Types.h"
#include "Library/LibraryService.h"
#include "Network/NetworkOutput.h"
#include "Playlist/PlaylistManager.h"
#include "Profile/ProfileEngine.h"

namespace sp
{

class AppContext
{
public:
    enum class Change
    {
        libraryChanged,
        playlistsChanged,
        trackChanged,
        playbackStateChanged,
        dspChanged,
        eqModeChanged,
        profileDecisionChanged,
        referenceModeChanged,
        deviceChanged,
        queueChanged,
        statusMessageChanged,
        networkChanged,
        settingsChanged
    };

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void appContextChanged (Change) {}
    };

    AppContext();
    ~AppContext();

    juce::String initialise();
    void shutdown();

    //== Services =============================================================
    const ApplicationPaths& paths() const;
    AppSettings& settings();
    LibraryService& library();
    PlaylistManager& playlists();
    AudioEngine& engine();
    ProfileEngine& profiles();
    NetworkOutput& network();

    //== Playback coordination ================================================
    bool playTrack (TrackId id, bool autoPlay = true);
    void playTracks (const std::vector<Track>& tracks, int startIndex);
    void playNextNow();
    void playPreviousNow();
    void setQueue (const std::vector<Track>& tracks, int startIndex, bool autoPlay);
    std::vector<Track> getQueue() const;
    int getCurrentQueueIndex() const;

    //== DSP coordination =====================================================
    /** The params currently applied to the engine (what the user hears). */
    DspParams getCurrentDspParams() const;
    /** The user's manual working params (used as the base in manual mode). */
    DspParams getManualDspParams() const;
    /** Applies edited params: updates manual state, persists, transitions the
        chain smoothly. `userEdited=true` marks it as a user change for Learn. */
    void applyUserDspParams (const DspParams&, bool userEdited = true);
    void applyUserBassGain (float db);
    void setGraphicEqBandGain (int bandIndex, float gainDb);

    EqMode getEqMode() const;
    void setEqMode (EqMode);
    juce::String getActiveProfileId() const;
    void selectProfile (const juce::String& profileId);   // manual selection
    ProfileDecision getCurrentProfileDecision() const;

    void setReferenceMode (bool active);
    bool isReferenceMode() const;

    //== Learning =============================================================
    bool learnCurrentAdjustment (LearnScope scope, juce::String& messageOut);
    LearnScope getLearnScope() const;
    void setLearnScope (LearnScope);

    //== A/B comparison =======================================================
    /** Saves the current processed params as B and compares against bypassed
        (or vice versa). Returns active state. */
    bool isAbComparisonActive() const;
    void setAbComparisonActive (bool);
    /** When enabled, applies the measured RMS difference as a gain offset so
        the comparison evaluates tone, not loudness. */
    bool isAbLevelMatching() const;
    void setAbLevelMatching (bool);
    juce::String getAbStatusText() const;

    //== Device profiles ======================================================
    void refreshOutputDevices();
    bool selectOutputDevice (const juce::String& name, double rate, juce::String& errorOut);
    void applyDspForCurrentDevice();       // loads the per-device DSP profile
    void saveDspForCurrentDevice();        // stores the current DSP for this device
    std::vector<OutputDeviceInfo> getOutputDevices() const;

    //== Network ==============================================================
    bool playOnNetworkRenderer (const Track&, const NetworkEndpoint&, juce::String& errorOut);

    //== Session persistence ==================================================
    void saveSession();
    void restoreSession();
    void saveAllState();

    //== App actions / shortcuts =============================================
    bool performAction (AppAction);
    juce::String getActionDescription (AppAction) const;

    //== Status ===============================================================
    void setStatusMessage (const juce::String& message, int autoClearMs = 4000);

    /** Alternate outputs (PiBridge/network) register a stopper here. It is
        invoked just before local playback starts, so two different songs can
        never play on two outputs at once. */
    void setAuxiliaryPlaybackStopper (std::function<void()> stopper);

    /** Network output hooks: while the external output is active, starting a
        track routes it there instead of the local engine (so choosing another
        song keeps playing on the Pi). */
    void setExternalOutputHooks (std::function<bool()> isActive,
                                 std::function<bool(const Track&, juce::String&)> play);
    void setExternalVolumeHook (std::function<void(float, bool)> apply);
    void setExternalStopHook (std::function<void()> stopKeepingOutput);
    void setExternalStreamingHook (std::function<bool()> isStreaming);
    void setPiOutputRouter (std::function<void(bool)> selectPi);

    /** True when the transport output is the Pi K11 network stream. */
    bool isPiOutputSelected() const;
    void setPiOutputSelected (bool selected);

    bool isExternalOutputActive() const;

    //== Pi K11 network output (core service; independent of the Settings panel) ==
    bool startPiBridgeStream (const Track& track, juce::String& errorOut);
    void stopPiBridgeStream();
    bool isPiBridgeStreaming() const;
    int getPiBridgeState() const;
    bool isPiBridgeRunning() const;
    juce::String getPiBridgeStatus() const;
    int getPiBridgeRate() const;
    std::int64_t getPiBridgeFramesSent() const;
    std::int64_t getPiBridgeFramesTotal() const;
    void setPiBridgeVolume (float volumeDb, bool muted);
    std::int64_t getPiBridgePositionMs() const;

    /** Seek the current output (local engine or Pi K11 stream). */
    void seekMs (std::int64_t positionMs);

    /** Verification/screenshot hook: clear DSP/A-B bypass without touching EQ values. */
    void clearDspBypass();

    /** Master volume/mute; also forwards to an active network output. */
    void setMasterVolume (float volumeDb);
    void setMasterMuted (bool muted);
    juce::String getStatusMessage() const;
    juce::String getDiagnosticsText() const;

    //== Listeners ============================================================
    void addListener (Listener*);
    void removeListener (Listener*);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppContext)
};

} // namespace sp
