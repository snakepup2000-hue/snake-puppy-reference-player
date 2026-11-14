/*
    Snake Puppy Reference Player - application context.
    Owned by the Lead. Wires the services together and implements the
    coordinator behaviour the UI calls.
*/
#include "Core/AppContext.h"
#include "Core/Serialization.h"
#include "Network/PiBridgeOutput.h"

namespace sp
{

namespace
{
const char* kSessionKey = "session.playback";

juce::String dspToText (const DspParams& p) { return juce::JSON::toString (serialize::toVar (p)); }

juce::var prop (const juce::var& v, const char* name)
{
    if (auto* o = v.getDynamicObject())
        return o->getProperty (name);
    return {};
}
} // namespace

struct AppContext::Impl : public AudioEngine::Listener,
                          public LibraryService::Listener,
                          private juce::Timer
{
    ApplicationPaths paths;
    LibraryDatabase database;
    AppSettings appSettings;
    LibraryService library;
    PlaylistManager playlists;
    AudioEngine engine;
    ProfileEngine profiles;
    NetworkOutput network;

    std::list<AppContext::Listener*> listeners;
    juce::String statusMessage;
    std::function<void()> stopAuxiliaryPlayback;
    std::function<bool()> externalOutputActive;
    std::function<bool(const Track&, juce::String&)> externalOutputPlay;
    std::function<void(float, bool)> externalVolumeApply;
    std::function<void()> externalStopKeepOutput;
    std::function<bool()> externalStreamingActive;
    std::function<void(bool)> piOutputRouter;
    bool piOutputSelected = false;
    std::unique_ptr<PiBridgeOutput> piOutput;
    bool piAutoAdvanced = false;
    int piAnalyzerRate = 0;
    int statusClearMs = 0;
    juce::int64 statusSetAtMs = 0;

    Track currentTrack;
    ProfileDecision currentDecision;
    DspParams manualParams;
    bool referenceMode = false;
    bool abActive = false;
    bool abLevelMatching = false;
    DspParams abProcessedParams;
    float abLevelOffsetDb = 0.0f;
    bool initialised = false;

    std::unique_ptr<juce::ThreadPool> analysisPool;
    std::atomic<bool> shuttingDown { false };
    std::atomic<TrackId> analysisTrackId { 0 };

    // Developer verification hooks (environment variables, documented in BUILD.md):
    //   SP_IMPORT_ON_START=<folder>  import a folder at launch
    //   SP_PLAY_ON_START=1           play the first library track after scanning
    //   SP_DEBUG_RECORD=<file.wav>   record the post-volume output for verification
    //   SP_REFERENCE_MODE=1          start in Reference Mode
    //   SP_SET_EQ_MODE=manual|autogenre|intelligent
    //   SP_SET_BASS=<dB>
    //   SP_SET_PROFILE=<profile id>
    bool pendingAutoPlay = false;
    juce::String pendingRecordPath;
    bool pendingReferenceMode = false;
    int pendingEqMode = -1;
    float pendingBassGain = 0.0f;
    bool hasPendingBassGain = false;
    juce::String pendingProfileId;
    bool pendingApplied = false;
    juce::String statusLogPath;
    juce::int64 lastStatusLogMs = 0;

    Impl()
        : paths (ApplicationPaths::resolve()),
          appSettings (paths),
          library (paths, database),
          playlists (database, library),
          profiles (database, library),
          network (paths)
    {
        manualParams = appSettings.loadManualDsp();
        profiles.setMode (appSettings.loadEqMode());
        profiles.setLearnScope (appSettings.loadLearnScope());
        engine.addListener (this);
        library.addListener (this);
        analysisPool = std::make_unique<juce::ThreadPool> (1);
        startTimerHz (10);
    }

    ~Impl() override
    {
        shuttingDown = true;
        stopTimer();
        if (analysisPool != nullptr)
            analysisPool->removeAllJobs (true, 3000);
        engine.removeListener (this);
        library.removeListener (this);
    }

    void fireChange (Change c)
    {
        for (auto* l : listeners)
            l->appContextChanged (c);
    }

    //== AudioEngine::Listener ================================================
    void engineTrackChanged (const Track& t) override
    {
        currentTrack = t;
        resolveProfileForTrack (t);
        fireChange (Change::trackChanged);
        fireChange (Change::profileDecisionChanged);
    }

    void engineQueueChanged() override { fireChange (Change::queueChanged); }
    void engineDevicesChanged() override
    {
        applyDspForCurrentDeviceImpl();
        fireChange (Change::deviceChanged);
    }

    juce::String currentOutputName()
    {
        return piOutputSelected ? juce::String ("Pi K11 (Network)")
                                : engine.devices().getCurrentDeviceName();
    }

    void applyDspForCurrentDeviceImpl()
    {
        const auto name = currentOutputName();
        DeviceProfileKey key;
        key.deviceName = name;
        key.outputKind = piOutputSelected ? "network" : "default";
        auto profile = appSettings.findDeviceProfile (key);
        if (profile.id.isNotEmpty())
        {
            setDspBoth (profile.params, 250);
            manualParams = profile.params;
        }
    }

    void engineError (const juce::String& e) override
    {
        setStatus (e);
        fireChange (Change::statusMessageChanged);
    }

    void engineReferenceModeChanged (bool b) override
    {
        referenceMode = b;
        fireChange (Change::referenceModeChanged);
    }

    //== LibraryService::Listener =============================================
    void libraryChanged() override { fireChange (Change::libraryChanged); }
    void libraryLocationsChanged() override { fireChange (Change::libraryChanged); }
    void libraryScanProgress (const ScanProgress&) override {}

    void libraryScanFinished (int, int, int, int) override
    {
        fireChange (Change::libraryChanged);
        library.refreshMissingState();
        applyPendingVerificationHooks();
    }

    void libraryError (const juce::String& e) override { setStatus (e); }

    //== Coordination =========================================================
    void resolveProfileForTrack (const Track& t, bool countPlay = true)
    {
        if (referenceMode)
        {
            currentDecision = {};
            currentDecision.mode = EqMode::reference;
            currentDecision.profileName = "Reference";
            return;
        }

        TrackAnalysis analysis;
        bool haveAnalysis = library.getAnalysis (t.id, analysis)
                            && analysis.valid
                            && analysis.version == TrackAnalysis::kVersion;

        const auto mode = profiles.getMode();
        if (mode == EqMode::intelligent && ! haveAnalysis)
        {
            // Decide immediately from metadata, then refine once measured
            // analysis is available (never silently: the refined decision is
            // published and reported to the UI).
            requestAnalysis (t);
        }

        const TrackAnalysis* analysisPtr = haveAnalysis ? &analysis : nullptr;
        auto resolved = profiles.resolveParams (t, analysisPtr, manualParams, currentDecision);

        setDspBoth (resolved, 160);

        // ReplayGain for this track (honest: only when tags exist and RG enabled).
        bool hasTrack = t.hasReplayGainTrack;
        bool hasAlbum = t.hasReplayGainAlbum;
        bool rgEnabled = resolved.replayGain.enabled;

        if (rgEnabled && (hasTrack || hasAlbum))
            engine.setRateReplayGainForCurrentTrack (resolved.replayGainFor (hasTrack, t.replayGainTrackDb,
                                                                             hasAlbum, t.replayGainAlbumDb),
                                                    t.peakLinear, true);
        else
            engine.setRateReplayGainForCurrentTrack (0.0f, 0.0f, false);

        if (countPlay)
            library.incrementPlayCount (t.id);
    }

    /** Restores the visible profile state after a restart or a mode change
        without counting a play. */
    void primeProfileFromQueue()
    {
        if (referenceMode || currentTrack.path.isNotEmpty())
            return;

        auto queue = engine.getQueue();
        const int index = engine.getCurrentQueueIndex();
        if (index >= 0 && index < (int) queue.size())
        {
            currentTrack = queue[(size_t) index];
            resolveProfileForTrack (currentTrack, false);
        }
        else if (profiles.getMode() != EqMode::manual)
        {
            currentDecision = {};
            currentDecision.mode = profiles.getMode();
            currentDecision.profileName = "Waiting for playback";
            currentDecision.reasons.push_back ("Start playback to match a genre profile.");
        }
    }

    void requestAnalysis (const Track& t)
    {
        if (shuttingDown || analysisPool == nullptr)
            return;
        if (analysisTrackId.exchange (t.id) == t.id)   // already queued
            return;

        analysisPool->addJob ([this, t]
        {
            if (shuttingDown)
                return;
            TrackAnalysis analysis;
            juce::String error;
            if (profiles.getOrComputeAnalysis (t, analysis, error) && analysis.valid)
            {
                library.saveAnalysis (t.id, analysis);
                if (shuttingDown)
                    return;
                juce::MessageManager::callAsync ([this, id = t.id]
                {
                    if (shuttingDown || currentTrack.id != id)
                        return;
                    resolveProfileForTrack (currentTrack, false);
                    fireChange (Change::profileDecisionChanged);
                });
            }
            else
            {
                juce::MessageManager::callAsync ([this, error]
                {
                    if (shuttingDown)
                        return;
                    if (error.isNotEmpty())
                        setStatus ("Audio analysis unavailable: " + error);
                });
            }
        });
    }

    void readVerificationHooks()
    {
        const auto seed = juce::SystemStats::getEnvironmentVariable ("SP_IMPORT_ON_START", {});
        if (seed.isNotEmpty())
        {
            library.addLocation (juce::File (seed), false, nullptr);
            setStatus ("Importing " + seed + " (verification hook)");
        }
        pendingAutoPlay = juce::SystemStats::getEnvironmentVariable ("SP_PLAY_ON_START", {}) == "1";
        pendingRecordPath = juce::SystemStats::getEnvironmentVariable ("SP_DEBUG_RECORD", {});
        pendingReferenceMode = juce::SystemStats::getEnvironmentVariable ("SP_REFERENCE_MODE", {}) == "1";
        auto modeText = juce::SystemStats::getEnvironmentVariable ("SP_SET_EQ_MODE", {});
        if (modeText.equalsIgnoreCase ("manual")) pendingEqMode = (int) EqMode::manual;
        else if (modeText.equalsIgnoreCase ("autogenre")) pendingEqMode = (int) EqMode::autoGenre;
        else if (modeText.equalsIgnoreCase ("intelligent")) pendingEqMode = (int) EqMode::intelligent;
        auto bassText = juce::SystemStats::getEnvironmentVariable ("SP_SET_BASS", {});
        if (bassText.isNotEmpty())
        {
            pendingBassGain = (float) bassText.getDoubleValue();
            hasPendingBassGain = true;
        }
        pendingProfileId = juce::SystemStats::getEnvironmentVariable ("SP_SET_PROFILE", {});
        statusLogPath = juce::SystemStats::getEnvironmentVariable ("SP_STATUS_LOG", {});
        if (statusLogPath.isNotEmpty())
            juce::File (statusLogPath).deleteFile();
    }

    void applyPendingVerificationHooks()
    {
        if (pendingApplied)
            return;
        auto snapshot = library.getSnapshot();
        if (pendingAutoPlay && ! snapshot->tracks.empty())
        {
            engine.playTrack (snapshot->tracks.front(), true);
            pendingAutoPlay = false;
        }
        else if (pendingAutoPlay)
        {
            return;   // wait for a scan that actually finds tracks
        }

        pendingApplied = true;

        if (pendingReferenceMode)
        {
            referenceMode = true;
            engine.setReferenceMode (true);
        }

        if (pendingEqMode >= 0)
        {
            profiles.setMode ((EqMode) pendingEqMode);
            appSettings.saveEqMode ((EqMode) pendingEqMode);
        }

        if (pendingProfileId.isNotEmpty())
        {
            auto profile = profiles.getProfile (pendingProfileId);
            if (profile.id.isNotEmpty())
            {
                auto p = engine.getDspParams();
                p.graphic = profile.params.graphic;
                p.parametric = profile.params.parametric;
                p.bass = profile.params.bass;
                p.filters = profile.params.filters;
                p.stereo = profile.params.stereo;
                manualParams = p;
                setDspBoth (p, 0);
                currentDecision.profileId = profile.id;
                currentDecision.profileName = profile.name;
                setStatus ("Loaded profile " + profile.name + " (verification hook)");
            }
        }

        if (hasPendingBassGain)
        {
            auto p = engine.getDspParams();
            p.bass.enabled = true;
            p.bass.gainDb = pendingBassGain;
            manualParams = p;
            setDspBoth (p, 0);
            setStatus ("Bass set to " + juce::String (pendingBassGain, 1) + " dB (verification hook)");
        }

        if (pendingRecordPath.isNotEmpty())
        {
            juce::String recordError;
            if (engine.startDebugRecording (juce::File (pendingRecordPath), recordError))
                setStatus ("Recording output to " + pendingRecordPath);
            else
                setStatus ("Could not start debug recording: " + recordError);
        }

        fireChange (Change::dspChanged);
        fireChange (Change::profileDecisionChanged);
    }

    void setDspBoth (const DspParams& p, int rampMs)
    {
        engine.setDspParams (p, rampMs);
        if (piOutput != nullptr)
            piOutput->setDspParams (p, rampMs);
    }

    bool startPiStreamAt (const Track& track, std::int64_t startMs, juce::String& errorOut)
    {
        if (piOutput == nullptr || track.path.isEmpty())
        {
            errorOut = "No track selected.";
            return false;
        }
        if (engine.getStatus().state == PlaybackState::playing)
            engine.pause();
        PiBridgeConfig cfg;
        const bool ok = piOutput->start (juce::File (track.path), engine.getDspParams(),
                                         engine.getVolumeDb(), engine.isMuted(), cfg, errorOut, startMs);
        if (ok)
            setStatus ("Streaming " + track.displayTitle() + " from "
                       + juce::String (startMs / 1000) + " s to Pi K11.", 8000);
        return ok;
    }

    bool startPiStream (const Track& track, juce::String& errorOut)
    {
        return startPiStreamAt (track, 0, errorOut);
    }

    void stopPiStream (bool keepSelected)
    {
        if (piOutput != nullptr)
            piOutput->stop();
        const double deviceRate = engine.devices().getCurrentSampleRate();
        engine.analyzers().prepare (deviceRate > 0.0 ? deviceRate : 48000.0, 4096);
        piAnalyzerRate = (int) deviceRate;
        if (! keepSelected)
            piOutputSelected = false;
    }

    void setStatus (const juce::String& m, int autoClearMs = 4000)
    {
        statusMessage = m;
        statusClearMs = autoClearMs;
        statusSetAtMs = juce::Time::currentTimeMillis();
        fireChange (Change::statusMessageChanged);
    }

    void timerCallback() override
    {
        if (piOutput != nullptr && piOutputSelected)
        {
            const int piRate = piOutput->getOutputSampleRate();
            if (piRate > 0 && piRate != piAnalyzerRate)
            {
                // The Pi resampler emits blocks up to 32768 frames; the
                // analyzer FIFOs must be prepared for that size or every push
                // is dropped and the meters stay at -inf.
                engine.analyzers().prepare ((double) piRate, 65536);
                piAnalyzerRate = piRate;
            }

            const auto state = piOutput->getState();
            if (state == PiBridgeOutput::State::streaming
                || state == PiBridgeOutput::State::preparing)
                piAutoAdvanced = false;
            else if (state == PiBridgeOutput::State::finished && ! piAutoAdvanced)
            {
                piAutoAdvanced = true;
                const auto queue = engine.getQueue();
                const int index = engine.getCurrentQueueIndex();
                if (index + 1 < (int) queue.size())
                {
                    engine.setQueue (queue, index + 1, false);
                    juce::String error;
                    if (! startPiStream (queue[(size_t) index + 1], error))
                        setStatus ("Pi K11: " + error, 8000);
                }
                else
                    setStatus ("Pi K11: end of queue.", 6000);
            }
        }

        if (statusLogPath.isNotEmpty()
            && juce::Time::currentTimeMillis() - lastStatusLogMs >= 500)
        {
            lastStatusLogMs = juce::Time::currentTimeMillis();
            auto st = engine.getStatus();
            auto meters = engine.analyzers().getMeterValues();

            juce::DynamicObject::Ptr o (new juce::DynamicObject());
            o->setProperty ("t", lastStatusLogMs);
            o->setProperty ("state", playbackStateName (st.state));
            o->setProperty ("track", st.currentTrack.displayTitle());
            o->setProperty ("posMs", st.positionMs);
            o->setProperty ("durMs", st.durationMs);
            o->setProperty ("bufferedMs", st.bufferedMs);
            o->setProperty ("underruns", st.underrunCount);
            o->setProperty ("cpu", st.cpuPercent);
            o->setProperty ("volumeDb", st.volumeDb);
            o->setProperty ("reference", st.referenceMode);
            o->setProperty ("rmsL", meters.rmsDb[0]);
            o->setProperty ("rmsR", meters.rmsDb[1]);
            o->setProperty ("lufsM", meters.momentaryLufs);
            o->setProperty ("correlation", meters.correlation);
            o->setProperty ("device", st.signalPath.outputDeviceName);
            o->setProperty ("deviceRate", st.signalPath.outputSampleRate);
            o->setProperty ("sourceFormat", st.signalPath.sourceFormat);
            o->setProperty ("sourceRate", st.signalPath.sourceSampleRate);
            o->setProperty ("quality", st.signalPath.qualityLabel());
            o->setProperty ("resampling", st.signalPath.resampling);
            juce::Array<juce::var> steps;
            for (auto& step : st.dspStatus.activeSteps)
                steps.add (step);
            o->setProperty ("dsp", steps);

            juce::File (statusLogPath).appendText (juce::JSON::toString (juce::var (o.get()), false) + "\n");
        }

        if (statusClearMs > 0 && juce::Time::currentTimeMillis() - statusSetAtMs > statusClearMs)
        {
            statusMessage.clear();
            statusClearMs = 0;
            fireChange (Change::statusMessageChanged);
        }
    }
};

//==============================================================================
AppContext::AppContext() : impl (std::make_unique<Impl>()) {}
AppContext::~AppContext() { shutdown(); }

juce::String AppContext::initialise()
{
    if (impl->initialised)
        return {};

    juce::String err;
    if (! impl->paths.ensureDirectoriesExist (err))
        return err;

    if (auto e = impl->database.open (impl->paths.libraryDb); e.isNotEmpty())
        return e;

    if (auto e = impl->database.initialiseSchema(); e.isNotEmpty())
        return e;

    if (auto e = impl->library.initialise(); e.isNotEmpty())
        impl->setStatus ("Library: " + e);

    if (auto e = impl->playlists.initialise(); e.isNotEmpty())
        impl->setStatus ("Playlists: " + e);

    if (auto e = impl->profiles.initialise(); e.isNotEmpty())
        impl->setStatus ("Profiles: " + e);

    impl->network.loadConfig();

    // The embedded HTTP page lists the library and restricts /media to files
    // that are actually in it.
    impl->network.setMediaLibraryProvider ([this]
    {
        std::vector<Track> tracks;
        if (auto snapshot = impl->library.getSnapshot())
            for (auto& track : snapshot->tracks)
                if (! track.missing)
                    tracks.push_back (track);
        return tracks;
    });

    // Keep the embedded HTTP media server available from launch so the QR /
    // advertised network-output URL is always a working page (and renderers can
    // fetch media as soon as they are connected).
    if (impl->network.getConfig().serveMediaOverHttp)
    {
        juce::String mediaServerError;
        if (! impl->network.startMediaServer (mediaServerError))
            impl->setStatus ("Network media server: " + mediaServerError);
    }

    impl->engine.setOptions (impl->appSettings.loadEngineOptions());
    if (auto e = impl->engine.initialise(); e.isNotEmpty())
        impl->setStatus ("Audio: " + e);

    // Pi K11 output is a core service: available from launch, not only after
    // the Settings -> Network panel has been opened.
    impl->piOutput = std::make_unique<PiBridgeOutput>();
    impl->piOutput->setMonitorCallback ([this] (const float* l, const float* r, int frames)
    {
        // AnalyzerHub's FIFO holds 16384 frames per block; the Pi resampler can
        // emit 32256/32768-frame blocks, so split them instead of dropping.
        constexpr int chunk = 8192;
        for (int offset = 0; offset < frames; offset += chunk)
        {
            const int n = juce::jmin (chunk, frames - offset);
            impl->engine.analyzers().pushOutput (l + offset, r + offset, n);
        }
    });
    setExternalOutputHooks (
        [this]
        {
            const auto st = impl->piOutput != nullptr ? impl->piOutput->getState()
                                                      : PiBridgeOutput::State::idle;
            return impl->piOutputSelected || st == PiBridgeOutput::State::streaming
                   || st == PiBridgeOutput::State::preparing;
        },
        [this] (const Track& t, juce::String& error) { return impl->startPiStream (t, error); });
    setExternalVolumeHook ([this] (float db, bool muted)
    {
        if (impl->piOutput != nullptr)
            impl->piOutput->setVolume (db, muted);
    });
    setExternalStopHook ([this]
    {
        impl->stopPiStream (true);
        impl->setStatus ("Pi K11 stream stopped. Output remains Pi K11.", 6000);
    });
    setExternalStreamingHook ([this]
    {
        const auto st = impl->piOutput != nullptr ? impl->piOutput->getState()
                                                  : PiBridgeOutput::State::idle;
        return st == PiBridgeOutput::State::streaming || st == PiBridgeOutput::State::preparing;
    });
    setPiOutputRouter ([this] (bool selected)
    {
        if (! selected)
            impl->stopPiStream (false);
    });

    impl->profiles.setMode (impl->appSettings.loadEqMode());
    impl->manualParams = impl->appSettings.loadManualDsp();

    // Auto Headroom used to default on, so BASS boost lowered the whole level.
    // The limiter still protects peaks; migrate once and keep the user's level.
    if (! impl->appSettings.getBool ("dsp.autoHeadroomDefaultOff", false))
    {
        impl->manualParams.preamp.autoHeadroom = false;
        impl->appSettings.set ("dsp.autoHeadroomDefaultOff", true);
        impl->appSettings.saveManualDsp (impl->manualParams);
        impl->appSettings.save();
    }

    impl->setDspBoth (impl->manualParams, 0);
    impl->profiles.setLearnScope (impl->appSettings.loadLearnScope());
    impl->applyDspForCurrentDeviceImpl();

    restoreSession();
    impl->primeProfileFromQueue();

    impl->readVerificationHooks();
    impl->applyPendingVerificationHooks();

    impl->initialised = true;
    return {};
}

void AppContext::shutdown()
{
    if (impl->piOutput != nullptr)
        impl->piOutput->stop();

    if (! impl->initialised)
    {
        impl->engine.shutdown();
        return;
    }
    saveAllState();
    impl->engine.shutdown();
    impl->initialised = false;
}

const ApplicationPaths& AppContext::paths() const { return impl->paths; }
AppSettings& AppContext::settings() { return impl->appSettings; }
LibraryService& AppContext::library() { return impl->library; }
PlaylistManager& AppContext::playlists() { return impl->playlists; }
AudioEngine& AppContext::engine() { return impl->engine; }
ProfileEngine& AppContext::profiles() { return impl->profiles; }
NetworkOutput& AppContext::network() { return impl->network; }

//== Playback coordination ====================================================
void AppContext::setAuxiliaryPlaybackStopper (std::function<void()> stopper)
{
    impl->stopAuxiliaryPlayback = std::move (stopper);
}

void AppContext::setExternalOutputHooks (std::function<bool()> isActive,
                                         std::function<bool(const Track&, juce::String&)> play)
{
    impl->externalOutputActive = std::move (isActive);
    impl->externalOutputPlay = std::move (play);
}

void AppContext::setExternalVolumeHook (std::function<void(float, bool)> apply)
{
    impl->externalVolumeApply = std::move (apply);
}

void AppContext::setExternalStopHook (std::function<void()> stopKeepingOutput)
{
    impl->externalStopKeepOutput = std::move (stopKeepingOutput);
}

void AppContext::setExternalStreamingHook (std::function<bool()> isStreaming)
{
    impl->externalStreamingActive = std::move (isStreaming);
}

void AppContext::setPiOutputRouter (std::function<void(bool)> selectPi)
{
    impl->piOutputRouter = std::move (selectPi);
}

bool AppContext::isPiOutputSelected() const { return impl->piOutputSelected; }

void AppContext::setPiOutputSelected (bool selected)
{
    const auto before = impl->engine.getStatus();
    const bool wasPlaying = before.state == PlaybackState::playing;
    const auto track = before.currentTrack;

    impl->piOutputSelected = selected;
    if (impl->piOutputRouter != nullptr)
        impl->piOutputRouter (selected);

    if (selected)
    {
        // Switching output must never leave the Mac playing underneath the Pi.
        if (wasPlaying)
            impl->engine.pause();

        impl->applyDspForCurrentDeviceImpl();

        impl->setStatus ("Output: Pi K11 (network, SSH)", 6000);

        if (wasPlaying && track.path.isNotEmpty()
            && impl->externalOutputPlay != nullptr)
        {
            juce::String error;
            if (! impl->externalOutputPlay (track, error))
                impl->setStatus ("Pi K11: " + error, 10000);
        }
    }
    else
    {
        impl->applyDspForCurrentDeviceImpl();
        impl->setStatus ("Output: Mac audio device", 6000);
    }
}

bool AppContext::isExternalOutputActive() const
{
    return impl->externalOutputActive != nullptr && impl->externalOutputActive();
}

void AppContext::setMasterVolume (float volumeDb)
{
    impl->engine.setVolumeDb (volumeDb);
    if (impl->externalVolumeApply != nullptr)
        impl->externalVolumeApply (volumeDb, impl->engine.isMuted());
}

void AppContext::setMasterMuted (bool muted)
{
    impl->engine.setMuted (muted);
    if (impl->externalVolumeApply != nullptr)
        impl->externalVolumeApply (impl->engine.getVolumeDb(), muted);
}

bool AppContext::playTrack (TrackId id, bool autoPlay)
{
    auto snap = impl->library.getSnapshot();
    if (auto* t = snap->findTrack (id))
    {
        if (autoPlay && impl->externalOutputActive != nullptr && impl->externalOutputActive()
            && impl->externalOutputPlay != nullptr)
        {
            impl->engine.playTrack (*t, false);
            juce::String error;
            if (impl->externalOutputPlay (*t, error))
            {
                impl->setStatus ("Streaming " + t->displayTitle() + " to Pi K11.", 8000);
                return true;
            }
            impl->setStatus ("Pi K11: " + error, 10000);
            return true;
        }
        if (autoPlay && impl->stopAuxiliaryPlayback != nullptr)
            impl->stopAuxiliaryPlayback();
        impl->engine.playTrack (*t, autoPlay);
        return true;
    }
    impl->setStatus ("Track not found in the library.");
    return false;
}

void AppContext::playTracks (const std::vector<Track>& tracks, int startIndex)
{
    if (impl->externalOutputActive != nullptr && impl->externalOutputActive()
        && impl->externalOutputPlay != nullptr
        && startIndex >= 0 && startIndex < (int) tracks.size())
    {
        impl->engine.setQueue (tracks, startIndex, false);
        juce::String error;
        if (impl->externalOutputPlay (tracks[(size_t) startIndex], error))
        {
            impl->setStatus ("Streaming " + tracks[(size_t) startIndex].displayTitle() + " to Pi K11.", 8000);
            return;
        }
        impl->setStatus ("Pi K11: " + error, 10000);
        return;
    }
    if (impl->stopAuxiliaryPlayback != nullptr)
        impl->stopAuxiliaryPlayback();
    impl->engine.setQueue (tracks, startIndex, true);
}

void AppContext::playNextNow() { impl->engine.next(); }
void AppContext::playPreviousNow() { impl->engine.previous(); }

void AppContext::setQueue (const std::vector<Track>& tracks, int startIndex, bool autoPlay)
{
    if (autoPlay && impl->externalOutputActive != nullptr && impl->externalOutputActive()
        && impl->externalOutputPlay != nullptr
        && startIndex >= 0 && startIndex < (int) tracks.size())
    {
        impl->engine.setQueue (tracks, startIndex, false);
        juce::String error;
        if (impl->externalOutputPlay (tracks[(size_t) startIndex], error))
        {
            impl->setStatus ("Streaming " + tracks[(size_t) startIndex].displayTitle() + " to Pi K11.", 8000);
            return;
        }
        impl->setStatus ("Pi K11: " + error, 8000);
    }
    if (autoPlay && impl->stopAuxiliaryPlayback != nullptr)
        impl->stopAuxiliaryPlayback();
    impl->engine.setQueue (tracks, startIndex, autoPlay);
}

std::vector<Track> AppContext::getQueue() const { return impl->engine.getQueue(); }
int AppContext::getCurrentQueueIndex() const { return impl->engine.getCurrentQueueIndex(); }

//== DSP coordination =========================================================
DspParams AppContext::getCurrentDspParams() const { return impl->engine.getDspParams(); }
DspParams AppContext::getManualDspParams() const { return impl->manualParams; }

void AppContext::applyUserDspParams (const DspParams& p, bool)
{
    impl->manualParams = p;
    impl->appSettings.saveManualDsp (p);
    impl->setDspBoth (p, 80);
    impl->fireChange (AppContext::Change::dspChanged);
}

void AppContext::applyUserBassGain (float db)
{
    auto p = impl->engine.getDspParams();
    p.bass.enabled = true;
    p.bass.gainDb = juce::jlimit (-p.bass.rangeDb, p.bass.rangeDb, db);
    applyUserDspParams (p);
}

void AppContext::setGraphicEqBandGain (int bandIndex, float gainDb)
{
    auto p = impl->engine.getDspParams();
    p.graphic.enabled = true;
    auto& g = p.graphic.gainForSet (bandIndex);
    g = juce::jlimit (-p.graphic.rangeDb, p.graphic.rangeDb, gainDb);
    applyUserDspParams (p);
}

EqMode AppContext::getEqMode() const { return impl->profiles.getMode(); }

void AppContext::setEqMode (EqMode m)
{
    if (impl->referenceMode && m != EqMode::reference)
        setReferenceMode (false);

    impl->profiles.setMode (m);
    impl->appSettings.saveEqMode (m);
    impl->fireChange (Change::eqModeChanged);

    if (! impl->currentTrack.path.isEmpty())
        impl->resolveProfileForTrack (impl->currentTrack, false);
    else
        impl->primeProfileFromQueue();
    impl->fireChange (Change::profileDecisionChanged);
}

juce::String AppContext::getActiveProfileId() const { return impl->currentDecision.profileId; }

void AppContext::selectProfile (const juce::String& profileId)
{
    auto profile = impl->profiles.getProfile (profileId);
    if (profile.id.isEmpty())
    {
        impl->setStatus ("Profile not found: " + profileId);
        return;
    }
    auto p = impl->engine.getDspParams();
    p.graphic = profile.params.graphic;
    p.parametric = profile.params.parametric;
    p.bass = profile.params.bass;
    p.filters = profile.params.filters;
    p.stereo = profile.params.stereo;
    impl->manualParams = p;
    impl->appSettings.saveManualDsp (p);
    impl->appSettings.saveLastEqProfileId (profileId);
    impl->setDspBoth (p, 160);

    impl->currentDecision.profileId = profile.id;
    impl->currentDecision.profileName = profile.name;
    impl->currentDecision.mode = impl->profiles.getMode();
    impl->fireChange (Change::dspChanged);
    impl->fireChange (Change::profileDecisionChanged);
}

ProfileDecision AppContext::getCurrentProfileDecision() const { return impl->currentDecision; }

void AppContext::setReferenceMode (bool active)
{
    impl->referenceMode = active;
    impl->engine.setReferenceMode (active);
    if (active)
    {
        impl->currentDecision = {};
        impl->currentDecision.mode = EqMode::reference;
        impl->currentDecision.profileName = "Reference";
        impl->setStatus ("Reference mode: all DSP bypassed, volume set to unity.");
    }
    else
    {
        impl->setStatus ("Reference mode off.");
        if (! impl->currentTrack.path.isEmpty())
            impl->resolveProfileForTrack (impl->currentTrack, false);
    }
    impl->fireChange (Change::referenceModeChanged);
    impl->fireChange (Change::profileDecisionChanged);
}

bool AppContext::isReferenceMode() const { return impl->referenceMode; }

//== Learning =================================================================
bool AppContext::learnCurrentAdjustment (LearnScope scope, juce::String& messageOut)
{
    if (impl->currentTrack.path.isEmpty())
    {
        messageOut = "No track is loaded, so there is nothing to learn from.";
        return false;
    }
    auto applied = impl->engine.getDspParams();
    if (! impl->profiles.learnFromUserEdit (impl->currentTrack, applied, scope))
    {
        messageOut = "Could not save the learned preference.";
        return false;
    }
    messageOut = juce::String ("Learned this EQ for ") + learnScopeName (scope)
               + ": " + (scope == LearnScope::song ? impl->currentTrack.displayTitle()
                         : scope == LearnScope::album ? impl->currentTrack.displayAlbum()
                         : impl->currentTrack.displayGenre());
    impl->setStatus (messageOut);
    impl->fireChange (Change::profileDecisionChanged);
    return true;
}

LearnScope AppContext::getLearnScope() const { return impl->profiles.getLearnScope(); }

void AppContext::setLearnScope (LearnScope s)
{
    impl->profiles.setLearnScope (s);
    impl->appSettings.saveLearnScope (s);
}

//== A/B ======================================================================
bool AppContext::isAbComparisonActive() const { return impl->abActive; }

void AppContext::setAbComparisonActive (bool active)
{
    if (active == impl->abActive)
        return;

    if (active)
    {
        impl->abProcessedParams = impl->engine.getDspParams();

        // Level matching: compare the measured output RMS (processed) with the
        // measured pre-DSP input RMS and compensate the bypassed path so the
        // comparison is about tone, not loudness.
        impl->abLevelOffsetDb = 0.0f;
        if (impl->abLevelMatching)
        {
            auto meters = impl->engine.analyzers().getMeterValues();
            float in = juce::jmax (meters.inputRmsDb[0], meters.inputRmsDb[1]);
            float out = juce::jmax (meters.rmsDb[0], meters.rmsDb[1]);
            if (in > -100.0f && out > -100.0f)
                impl->abLevelOffsetDb = juce::jlimit (-12.0f, 12.0f, out - in);
        }

        auto flat = impl->abProcessedParams;
        flat.bypassed = true;
        impl->setDspBoth (flat, 30);
        impl->engine.setVolumeDb (impl->engine.getVolumeDb() + impl->abLevelOffsetDb);
        impl->setStatus (impl->abLevelMatching
                             ? "A/B: comparing original (level matched by measured RMS)."
                             : "A/B: comparing original (level not matched).");
    }
    else
    {
        impl->setDspBoth (impl->abProcessedParams, 30);
        if (impl->abLevelOffsetDb != 0.0f)
            impl->engine.setVolumeDb (impl->engine.getVolumeDb() - impl->abLevelOffsetDb);
        impl->abLevelOffsetDb = 0.0f;
    }
    impl->abActive = active;
    impl->fireChange (Change::dspChanged);
}

bool AppContext::isAbLevelMatching() const { return impl->abLevelMatching; }
void AppContext::setAbLevelMatching (bool enabled) { impl->abLevelMatching = enabled; }

juce::String AppContext::getAbStatusText() const
{
    if (! impl->abActive)
        return "A/B off";
    return impl->abLevelMatching ? "A/B: original (level matched)" : "A/B: original (level not matched)";
}

//== Device profiles ==========================================================
void AppContext::refreshOutputDevices()
{
    impl->engine.rescanDevices();
    impl->fireChange (Change::deviceChanged);
}

std::vector<OutputDeviceInfo> AppContext::getOutputDevices() const
{
    return const_cast<AudioEngine&> (impl->engine).devices().getOutputDevices();
}

bool AppContext::startPiBridgeStream (const Track& track, juce::String& errorOut)
{
    return impl->startPiStream (track, errorOut);
}

void AppContext::stopPiBridgeStream()
{
    impl->stopPiStream (true);
}

bool AppContext::isPiBridgeStreaming() const
{
    return getPiBridgeState() == (int) PiBridgeOutput::State::streaming
        || getPiBridgeState() == (int) PiBridgeOutput::State::preparing;
}

bool AppContext::isPiBridgeRunning() const
{
    return impl->piOutput != nullptr && impl->piOutput->isRunning();
}

int AppContext::getPiBridgeState() const
{
    return impl->piOutput != nullptr ? (int) impl->piOutput->getState() : 0;
}

juce::String AppContext::getPiBridgeStatus() const
{
    return impl->piOutput != nullptr ? impl->piOutput->getStatusText() : juce::String();
}

int AppContext::getPiBridgeRate() const
{
    return impl->piOutput != nullptr ? impl->piOutput->getOutputSampleRate() : 0;
}

std::int64_t AppContext::getPiBridgeFramesSent() const
{
    return impl->piOutput != nullptr ? impl->piOutput->getFramesSent() : 0;
}

std::int64_t AppContext::getPiBridgeFramesTotal() const
{
    return impl->piOutput != nullptr ? impl->piOutput->getFramesTotal() : 0;
}

void AppContext::setPiBridgeVolume (float volumeDb, bool muted)
{
    if (impl->piOutput != nullptr)
        impl->piOutput->setVolume (volumeDb, muted);
}

std::int64_t AppContext::getPiBridgePositionMs() const
{
    return impl->piOutput != nullptr ? impl->piOutput->getPositionMs() : 0;
}

void AppContext::clearDspBypass()
{
    impl->abActive = false;
    auto p = impl->engine.getDspParams();
    p.bypassed = false;
    p.eqBypass = false;
    impl->setDspBoth (p, 0);
}

void AppContext::seekMs (std::int64_t positionMs)
{
    positionMs = juce::jmax ((std::int64_t) 0, positionMs);

    if (impl->piOutputSelected)
    {
        const auto track = impl->engine.getStatus().currentTrack;
        if (track.path.isEmpty())
            return;

        // The Pi receiver has no transport protocol: seek by restarting the
        // stream at the new position (the DAC/SSH chain re-locks in ~1-2 s).
        impl->stopPiStream (true);
        juce::String error;
        if (! impl->startPiStreamAt (track, positionMs, error))
            impl->setStatus ("Pi K11 seek: " + error, 8000);
        return;
    }

    impl->engine.seekMs (positionMs);
}

bool AppContext::selectOutputDevice (const juce::String& name, double rate, juce::String& errorOut)
{
    if (! impl->engine.selectOutputDevice (name, rate, errorOut))
        return false;

    impl->appSettings.set ("output.device", name);
    applyDspForCurrentDevice();
    impl->fireChange (Change::deviceChanged);
    return true;
}

void AppContext::applyDspForCurrentDevice()
{
    const auto deviceName = impl->currentOutputName();

    // Headphone outputs get their own profile if one exists.
    DeviceProfileKey key;
    key.deviceName = deviceName;
    key.outputKind = impl->piOutputSelected ? "network" : "default";

    auto profile = impl->appSettings.findDeviceProfile (key);
    if (profile.id.isNotEmpty())
    {
        impl->manualParams = profile.params;
        impl->setDspBoth (profile.params, 250);
        impl->setStatus ("Loaded DSP profile for " + deviceName);
    }
}

void AppContext::saveDspForCurrentDevice()
{
    DeviceProfileKey key;
    key.deviceName = impl->currentOutputName();
    key.outputKind = impl->piOutputSelected ? "network" : "default";
    if (key.deviceName.isEmpty())
    {
        impl->setStatus ("No output device is open.");
        return;
    }
    DeviceDspProfile p;
    p.id = "dev-" + key.deviceName;
    p.key = key;
    p.name = key.deviceName;
    p.params = impl->engine.getDspParams();
    impl->appSettings.upsertDeviceProfile (p);
    impl->setStatus ("Saved DSP profile for " + key.deviceName);
}

//== Network ==================================================================
bool AppContext::playOnNetworkRenderer (const Track& track, const NetworkEndpoint& endpoint, juce::String& errorOut)
{
    if (! impl->network.isMediaServerRunning())
        if (! impl->network.startMediaServer (errorOut))
            return false;

    if (! impl->network.isConnected())
        if (! impl->network.connectTo (endpoint, errorOut))
            return false;

    impl->engine.pause();
    auto url = impl->network.makeMediaUrl (juce::File (track.path));
    if (! impl->network.playUri (url, track.title, track.displayArtist(), track.album, errorOut))
        return false;

    impl->setStatus ("Sending to network renderer: " + endpoint.friendlyName);
    return true;
}

//== Session ==================================================================
void AppContext::saveSession()
{
    auto state = impl->engine.capturePersistedState();
    juce::DynamicObject::Ptr o (new juce::DynamicObject());
    juce::Array<juce::var> tracks;
    for (auto& t : state.queue)
    {
        juce::DynamicObject::Ptr to (new juce::DynamicObject());
        to->setProperty ("id", (juce::int64) t.id);
        to->setProperty ("path", t.path);
        tracks.add (juce::var (to.get()));
    }
    o->setProperty ("queue", tracks);
    o->setProperty ("index", state.currentQueueIndex);
    o->setProperty ("positionMs", (juce::int64) state.positionMs);
    o->setProperty ("volumeDb", state.volumeDb);
    o->setProperty ("muted", state.muted);
    o->setProperty ("referenceMode", state.referenceMode);
    o->setProperty ("eqMode", (int) impl->profiles.getMode());
    impl->appSettings.set (kSessionKey, juce::JSON::toString (juce::var (o.get())));
    impl->appSettings.saveIfDirty();
}

void AppContext::restoreSession()
{
    auto text = impl->appSettings.getString (kSessionKey);
    if (text.isEmpty())
        return;
    juce::var v;
    if (juce::JSON::parse (text, v).failed())
        return;

    auto* o = v.getDynamicObject();
    if (o == nullptr)
        return;

    PersistedPlaybackState state;
    if (auto* arr = o->getProperty ("queue").getArray())
    {
        auto snap = impl->library.getSnapshot();
        for (auto& item : *arr)
        {
            Track t;
            const auto path = prop (item, "path").toString();
            if (const auto* found = snap->findTrack ((TrackId) (juce::int64) prop (item, "id")))
                t = *found;
            else
            {
                t.path = path;
                t.title = juce::File (path).getFileNameWithoutExtension();
            }
            if (t.path.isNotEmpty())
                state.queue.push_back (t);
        }
    }
    state.currentQueueIndex = (int) o->getProperty ("index");
    state.positionMs = (juce::int64) o->getProperty ("positionMs");
    state.volumeDb = (float) (double) o->getProperty ("volumeDb");
    state.muted = (bool) o->getProperty ("muted");
    state.referenceMode = (bool) o->getProperty ("referenceMode");

    impl->engine.restorePersistedState (state);
    impl->engine.setVolumeDb (state.volumeDb);
    impl->engine.setMuted (state.muted);
}

void AppContext::saveAllState()
{
    saveSession();
    impl->appSettings.save();
}

//== Actions ==================================================================
void AppContext::setStatusMessage (const juce::String& m, int ms) { impl->setStatus (m, ms); }
juce::String AppContext::getStatusMessage() const { return impl->statusMessage; }

bool AppContext::performAction (AppAction action)
{
    switch (action)
    {
        case AppAction::playPause:
            if (impl->piOutputSelected)
            {
                if (impl->externalStreamingActive != nullptr && impl->externalStreamingActive())
                {
                    if (impl->externalStopKeepOutput != nullptr)
                        impl->externalStopKeepOutput();
                    return true;
                }
                const auto track = impl->engine.getStatus().currentTrack;
                juce::String error;
                if (track.path.isNotEmpty() && impl->externalOutputPlay != nullptr
                    && impl->externalOutputPlay (track, error))
                    return true;
                if (error.isNotEmpty())
                    impl->setStatus ("Pi K11: " + error, 8000);
                return true;
            }
            if (impl->engine.getStatus().state != PlaybackState::playing
                && impl->stopAuxiliaryPlayback != nullptr)
                impl->stopAuxiliaryPlayback();
            impl->engine.togglePlayPause();
            return true;
        case AppAction::stop:
            if (impl->piOutputSelected)
            {
                if (impl->externalStreamingActive != nullptr && impl->externalStreamingActive()
                    && impl->externalStopKeepOutput != nullptr)
                    impl->externalStopKeepOutput();
                impl->engine.stop();
                return true;
            }
            if (impl->stopAuxiliaryPlayback != nullptr)
                impl->stopAuxiliaryPlayback();
            impl->engine.stop();
            return true;
        case AppAction::nextTrack:
        case AppAction::previousTrack:
        {
            if (impl->piOutputSelected && impl->externalOutputPlay != nullptr)
            {
                const auto queue = impl->engine.getQueue();
                int index = impl->engine.getCurrentQueueIndex();
                if (! queue.empty())
                {
                    if (index < 0) index = 0;
                    index += (action == AppAction::nextTrack) ? 1 : -1;
                    index = juce::jlimit (0, (int) queue.size() - 1, index);
                    impl->engine.setQueue (queue, index, false);
                    juce::String error;
                    if (! impl->externalOutputPlay (queue[(size_t) index], error))
                        impl->setStatus ("Pi K11: " + error, 8000);
                    return true;
                }
            }
            if (action == AppAction::nextTrack) impl->engine.next();
            else impl->engine.previous();
            return true;
        }
        case AppAction::seekForward5:
        case AppAction::seekBack5:
        case AppAction::seekForward30:
        case AppAction::seekBack30:
        {
            const auto position = impl->piOutputSelected ? getPiBridgePositionMs()
                                                         : impl->engine.getStatus().positionMs;
            const std::int64_t delta = (action == AppAction::seekForward5 || action == AppAction::seekBack5)
                                           ? 5000 : 30000;
            const bool forward = action == AppAction::seekForward5 || action == AppAction::seekForward30;
            seekMs (forward ? position + delta : juce::jmax ((std::int64_t) 0, position - delta));
            return true;
        }
        case AppAction::volumeUp: setMasterVolume (juce::jmin (12.0f, impl->engine.getVolumeDb() + 1.0f)); return true;
        case AppAction::volumeDown: setMasterVolume (juce::jmax (-60.0f, impl->engine.getVolumeDb() - 1.0f)); return true;
        case AppAction::mute: setMasterMuted (! impl->engine.isMuted()); return true;
        case AppAction::toggleEqBypass:
        {
            auto p = impl->engine.getDspParams();
            p.eqBypass = ! p.eqBypass;
            applyUserDspParams (p);
            impl->setStatus (p.eqBypass ? "EQ bypassed." : "EQ enabled.");
            return true;
        }
        case AppAction::toggleReferenceMode: setReferenceMode (! impl->referenceMode); return true;
        case AppAction::cycleEqMode:
            setEqMode (impl->profiles.getMode() == EqMode::manual ? EqMode::autoGenre
                     : impl->profiles.getMode() == EqMode::autoGenre ? EqMode::intelligent
                     : EqMode::manual);
            return true;
        case AppAction::bassUp: applyUserBassGain (impl->engine.getDspParams().bass.gainDb + 0.5f); return true;
        case AppAction::bassDown: applyUserBassGain (impl->engine.getDspParams().bass.gainDb - 0.5f); return true;
        default: return false;
    }
}

juce::String AppContext::getActionDescription (AppAction a) const
{
    return appActionDescription (a);
}

juce::String AppContext::getDiagnosticsText() const
{
    auto s = impl->engine.getStatus();
    juce::String t;
    t << "Snake Puppy Reference Player diagnostics\n";
    t << "----------------------------------------\n";
    t << "Engine state: " << playbackStateName (s.state) << "\n";
    t << "Output device: " << s.signalPath.outputDeviceName << " (" << s.signalPath.outputDeviceType << ")\n";
    t << "Output rate: " << s.signalPath.outputSampleRate << " Hz\n";
    t << "Signal path: " << s.signalPath.summary() << "\n";
    t << "Buffer: " << juce::String (s.signalPath.bufferMs, 1) << " ms\n";
    t << "Underruns: " << s.underrunCount << "\n";
    t << "CPU: " << juce::String (s.cpuPercent, 1) << " %\n";
    t << "Realtime: " << (s.decodeThreadActive ? "decode active" : "decode idle") << "\n";
    return t;
}

void AppContext::addListener (Listener* l) { impl->listeners.push_back (l); }
void AppContext::removeListener (Listener* l) { impl->listeners.remove (l); }

} // namespace sp
