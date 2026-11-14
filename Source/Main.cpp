/*
    Snake Puppy Reference Player - application entry point.

    Creates the application context (all audio/library/DSP services), installs
    the shared LookAndFeel and shows the main window. Everything visible in the
    player is real: the UI reads service snapshots and calls the coordinator.

    Command-line options (used by integration/visual test rigs):
        --import <folder>            add a folder to the library and scan it
        --rescan                     refresh metadata for every known location
        --import-playlist <file>     import an M3U/M3U8/PLS playlist
        --view songs|albums|queue|health|playlist
                                     initial centre view
        --play                       start playback of the first library track
        --play-library               start playback of the whole library (shuffle off)
        --play-title <text>          with --play-library, start at the first matching title
        --volume <dB>                set the master volume on startup
        --seek-ms <ms>               seek to this position after playback starts
        --dsp-active                 clear global DSP/A-B bypass on startup
        --reference                  enable Reference Mode on startup
        --bass <dB>                  apply a bass shelf gain on startup
        --eq-mode manual|autogenre|intelligent
                                     select the EQ mode
        --profile <profileId>        select an EQ profile
        --record <path.wav>          start the engine debug recording after play
        --status-log <path.jsonl>    append an engine status line every 500 ms
        --quit-after <ms>            quit gracefully after N ms (persistence tests)
        --full-player                start in full-player layout
        --windowed                   force the normal (sidebar + rack) layout
        --dsp-studio                 open the DSP Studio window on startup
        --settings                   open the Settings window on startup
        --output-device <substring>  select the first output device whose name matches
        --hog on|off                 exclusive (hog) device mode
        --native-rate on|off         follow each file's sample rate
        --buffer <frames>            device buffer size in frames
        --dither on|off              TPDF dither when reducing bit depth
*/
#include <JuceHeader.h>

#include "Core/AppContext.h"
#include "UI/LookAndFeel.h"
#include "UI/MainWindow.h"
#include "UI/ObjCExceptionGuard.h"

#if __has_include("UI/DspStudioWindow.h")
    #include "UI/DspStudioWindow.h"
    #define SP_MAIN_HAVE_DSP_STUDIO 1
#else
    #define SP_MAIN_HAVE_DSP_STUDIO 0
#endif

#if __has_include("UI/SettingsWindow.h")
    #include "UI/SettingsWindow.h"
    #define SP_MAIN_HAVE_SETTINGS 1
#else
    #define SP_MAIN_HAVE_SETTINGS 0
#endif

namespace
{

class StartupController : private juce::Timer
{
public:
    StartupController (sp::AppContext& context, sp::MainWindow& window,
                       juce::String viewToShow, sp::PlaylistId playlistToShow,
                       juce::File playlistToImport,
                       bool shouldPlay, bool playWholeLibrary, juce::String playTitle,
                       bool hasVolume, double volumeDb, juce::String recordPath,
                       std::int64_t seekAfterStartMsIn)
        : ctx (context), mainWindow (window), recordFile (recordPath), seekAfterStartMs (seekAfterStartMsIn),
          view (viewToShow), playlistId (playlistToShow), playlistFile (playlistToImport),
          playLibrary (playWholeLibrary), playTitleFilter (playTitle), wantsPlay (shouldPlay)
    {
        if (hasVolume)
            ctx.engine().setVolumeDb ((float) juce::jlimit (-60.0, 12.0, volumeDb));

        if (! wantsPlay && playlistFile == juce::File() && view.isNotEmpty())
            applyView();

        startTimerHz (10);
    }

private:
    void timerCallback() override
    {
        const auto snapshot = ctx.library().getSnapshot();
        const bool libraryReady = snapshot != nullptr && ! snapshot->tracks.empty();

        if (! libraryReady)
        {
            if (++attempts > 400)
            {
                stopTimer();
                ctx.setStatusMessage ("Startup: library did not become ready.", 8000);
            }
            return;
        }

        attempts = 0;

        if (playlistFile != juce::File() && ! playlistDone)
        {
            playlistDone = true;
            juce::String error;
            sp::PlaylistId newId = sp::kInvalidPlaylistId;
            if (ctx.playlists().importPlaylistFile (playlistFile, error, &newId))
            {
                ctx.playlists().markChanged();
                ctx.setStatusMessage ("Imported playlist: " + playlistFile.getFileNameWithoutExtension());
                playlistId = newId;
                view = "playlist";
            }
            else
            {
                ctx.setStatusMessage ("Playlist import failed: " + error, 10000);
            }
        }

        if (! view.isEmpty() && ! viewApplied)
        {
            viewApplied = true;
            applyView();
        }

        if (wantsPlay && ! playStarted)
        {
            playStarted = true;
            if (playLibrary)
            {
                int startIndex = 0;
                if (playTitleFilter.isNotEmpty())
                {
                    for (size_t i = 0; i < snapshot->tracks.size(); ++i)
                    {
                        if (snapshot->tracks[i].displayTitle().containsIgnoreCase (playTitleFilter))
                        {
                            startIndex = (int) i;
                            break;
                        }
                    }
                }
                ctx.playTracks (snapshot->tracks, startIndex);
            }
            else
            {
                ctx.playTrack (snapshot->tracks.front().id, true);
            }
            if (seekAfterStartMs > 0)
                ctx.engine().seekMs (seekAfterStartMs);

            startRecordingIfRequested();
        }

        if (playlistDone || playlistFile == juce::File())
            if ((! wantsPlay || playStarted) && (view.isEmpty() || viewApplied))
                stopTimer();
    }

    void applyView()
    {
        if (view.isEmpty())
            return;
        mainWindow.getMainComponent().showView (view, playlistId);
    }

    void startRecordingIfRequested()
    {
        if (recordFile == juce::File() || recordingStarted)
            return;

        recordingStarted = true;
        juce::String error;
        if (! ctx.engine().startDebugRecording (recordFile, error))
            juce::Logger::writeToLog ("Debug recording failed: " + error);
        else
            juce::Logger::writeToLog ("Debug recording started: " + recordFile.getFullPathName());
    }

    sp::AppContext& ctx;
    sp::MainWindow& mainWindow;
    juce::File recordFile;
    std::int64_t seekAfterStartMs = 0;
    juce::String view;
    sp::PlaylistId playlistId = sp::kInvalidPlaylistId;
    juce::File playlistFile;
    bool playLibrary = false;
    juce::String playTitleFilter;
    bool wantsPlay = false;
    bool playStarted = false;
    bool playlistDone = false;
    bool viewApplied = false;
    bool recordingStarted = false;
    int attempts = 0;
};

class DelayedQuit : private juce::Timer
{
public:
    explicit DelayedQuit (int delayMs) : delay (juce::jmax (100, delayMs))
    {
        startTimer (delay);
    }

    void timerCallback() override
    {
        stopTimer();
        if (auto* app = juce::JUCEApplication::getInstance())
            app->systemRequestedQuit();
    }

private:
    int delay = 0;
};

class StatusLogger : private juce::Timer
{
public:
    StatusLogger (sp::AppContext& context, const juce::File& logFile) : ctx (context)
    {
        logFile.deleteFile();
        file = logFile;
        startTimer (500);
    }

    void timerCallback() override
    {
        const auto status = ctx.engine().getStatus();
        const auto& path = status.signalPath;

        juce::DynamicObject::Ptr line (new juce::DynamicObject());
        line->setProperty ("t", juce::Time::currentTimeMillis());
        line->setProperty ("state", sp::playbackStateName (status.state));
        line->setProperty ("trackId", (juce::int64) status.currentTrack.id);
        line->setProperty ("track", status.currentTrack.displayTitle());
        line->setProperty ("posMs", (juce::int64) status.positionMs);
        line->setProperty ("durMs", (juce::int64) status.durationMs);
        line->setProperty ("queueSize", status.queueSize);
        line->setProperty ("underruns", (juce::int64) status.underrunCount);
        line->setProperty ("cpu", status.cpuPercent);
        line->setProperty ("volumeDb", (double) status.volumeDb);
        line->setProperty ("muted", status.muted);
        line->setProperty ("reference", status.referenceMode);
        line->setProperty ("rmsL", (double) status.meters.rmsDb[0]);
        line->setProperty ("rmsR", (double) status.meters.rmsDb[1]);
        line->setProperty ("peakL", (double) status.meters.peakDb[0]);
        line->setProperty ("peakR", (double) status.meters.peakDb[1]);
        line->setProperty ("lufsM", (double) status.meters.momentaryLufs);
        line->setProperty ("correlation", (double) status.meters.correlation);
        line->setProperty ("device", path.outputDeviceName);
        line->setProperty ("deviceRate", path.outputSampleRate);
        line->setProperty ("sourceFormat", path.sourceFormat);
        line->setProperty ("sourceRate", path.sourceSampleRate);
        line->setProperty ("quality", path.qualityLabel());
        line->setProperty ("resampling", path.resampling);
        line->setProperty ("error", status.errorMessage);
        line->setProperty ("bufferedMs", (juce::int64) status.bufferedMs);

        juce::Array<juce::var> dspSteps;
        for (auto& step : path.activeDspSteps)
            dspSteps.add (step);
        line->setProperty ("dsp", dspSteps);

        file.appendText (juce::JSON::toString (juce::var (line.get()), true) + "\n");
    }

private:
    sp::AppContext& ctx;
    juce::File file;
};

} // namespace

class SnakePuppyApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Snake Puppy Reference Player"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        // File log (also used by integration rigs to observe startup actions).
        const auto paths = sp::ApplicationPaths::resolve();
        juce::String pathError;
        paths.ensureDirectoriesExist (pathError);
        logger = std::make_unique<juce::FileLogger> (paths.logFile, "Snake Puppy Reference Player", 512 * 1024);
        juce::Logger::setCurrentLogger (logger.get());
        juce::Logger::writeToLog ("Command line: [" + commandLine + "]");

        context = std::make_unique<sp::AppContext>();
        bootError = context->initialise();

        if (bootError.isNotEmpty())
            context->setStatusMessage ("Startup: " + bootError, 12000);

        auto args = juce::StringArray::fromTokens (commandLine, true);

        // Some automation environments cannot deliver argv to a GUI app (for
        // example when it is started through LaunchServices). A startup JSON
        // file is accepted as an equivalent channel; it is consumed on read.
        const juce::File startupFile ("/tmp/spui-startup.json");
        if (startupFile.existsAsFile())
        {
            juce::var startupConfig;
            if (juce::JSON::parse (startupFile.loadFileAsString(), startupConfig).wasOk())
            {
                if (auto* obj = startupConfig.getDynamicObject())
                {
                    auto addValueArg = [&args, obj] (const char* key, const char* flag)
                    {
                        const auto value = obj->getProperty (key);
                        if (value.isVoid())
                            return;
                        const auto text = value.toString();
                        if (text.isNotEmpty())
                        {
                            args.add (flag);
                            args.add (text);
                        }
                    };
                    auto addFlag = [&args, obj] (const char* key, const char* flag)
                    {
                        if ((bool) obj->getProperty (key))
                            args.add (flag);
                    };

                    addValueArg ("import", "--import");
                    addValueArg ("importPlaylist", "--import-playlist");
                    addValueArg ("playTitle", "--play-title");
                    addValueArg ("view", "--view");
                    addValueArg ("volume", "--volume");
                    addValueArg ("record", "--record");
                    addValueArg ("statusLog", "--status-log");
                    addValueArg ("quitAfter", "--quit-after");
                    addValueArg ("seekMs", "--seek-ms");
                    addValueArg ("bass", "--bass");
                    addValueArg ("eqMode", "--eq-mode");
                    addValueArg ("profile", "--profile");
                    addValueArg ("outputDevice", "--output-device");
                    addValueArg ("buffer", "--buffer");
                    addValueArg ("hog", "--hog");
                    addValueArg ("nativeRate", "--native-rate");
                    addValueArg ("dither", "--dither");
                    addFlag ("play", "--play");
                    addFlag ("playLibrary", "--play-library");
                    addFlag ("reference", "--reference");
                    addFlag ("fullPlayer", "--full-player");
                    addFlag ("windowed", "--windowed");
                    addFlag ("dspStudio", "--dsp-studio");
                    addFlag ("settings", "--settings");
                    addFlag ("network-settings", "--network-settings");

                    juce::Logger::writeToLog ("Consumed startup file: " + startupFile.getFullPathName());
                }
            }
            startupFile.deleteFile();
        }

        auto valueForFlag = [&args] (const juce::String& flag) -> juce::String
        {
            const int index = args.indexOf (flag);
            if (index >= 0 && index + 1 < args.size())
                return args[index + 1].unquoted();
            return {};
        };

        const auto importPath = valueForFlag ("--import");
        if (importPath.isNotEmpty())
        {
            const juce::File folder (importPath);
            if (folder.isDirectory())
            {
                juce::Logger::writeToLog ("Importing folder: " + folder.getFullPathName());
                context->library().importFolder (folder, true);
            }
            else
            {
                juce::Logger::writeToLog ("--import path is not a directory: " + folder.getFullPathName());
            }
        }

        if (args.contains ("--rescan"))
            context->library().rescanAll (true);

        sp::PlaylistId startupPlaylist = sp::kInvalidPlaylistId;
        const auto playlistPath = valueForFlag ("--import-playlist");
        const juce::File startupPlaylistFile (playlistPath);

        const auto viewToShow = valueForFlag ("--view");
        const bool shouldPlay = args.contains ("--play") || args.contains ("--play-library");
        const bool playWholeLibrary = args.contains ("--play-library");
        const auto playTitle = valueForFlag ("--play-title");
        const auto volumeText = valueForFlag ("--volume");
        const bool hasVolume = volumeText.isNotEmpty();
        const auto bassText = valueForFlag ("--bass");
        const auto eqModeText = valueForFlag ("--eq-mode").toLowerCase();
        const auto profileId = valueForFlag ("--profile");
        const auto recordPath = valueForFlag ("--record");
        const auto statusLogPath = valueForFlag ("--status-log");
        const auto quitAfterText = valueForFlag ("--quit-after");
        const auto seekMsText = valueForFlag ("--seek-ms");
        const auto seekAfterStartMs = (std::int64_t) juce::jmax ((juce::int64) 0,
                                                                 seekMsText.getLargeIntValue());
        const bool forceDspActive = args.contains ("--dsp-active");
        if (forceDspActive)
            context->clearDspBypass();

        // Engine / device options (same public API the Settings window uses).
        auto parseOnOff = [] (const juce::String& text, bool fallback)
        {
            const auto lower = text.toLowerCase();
            if (lower == "on" || lower == "true" || lower == "1")
                return true;
            if (lower == "off" || lower == "false" || lower == "0")
                return false;
            return fallback;
        };

        const auto outputDevice = valueForFlag ("--output-device");
        if (outputDevice.isNotEmpty())
        {
            juce::String chosen;
            for (const auto& device : context->getOutputDevices())
                if (device.name.containsIgnoreCase (outputDevice))
                {
                    chosen = device.name;
                    break;
                }

            if (chosen.isNotEmpty())
            {
                juce::String error;
                if (! context->selectOutputDevice (chosen, 0.0, error))
                    juce::Logger::writeToLog ("Output device selection failed: " + error);
                else
                    juce::Logger::writeToLog ("Output device: " + chosen);
            }
            else
            {
                juce::Logger::writeToLog ("No output device matches: " + outputDevice);
            }
        }

        const auto bufferText = valueForFlag ("--buffer");
        if (bufferText.isNotEmpty())
        {
            auto options = context->engine().getOptions();
            options.bufferFrames = juce::jlimit (32, 8192, bufferText.getIntValue());
            context->engine().setOptions (options);
        }
        if (valueForFlag ("--hog").isNotEmpty())
            context->engine().setHogModeEnabled (parseOnOff (valueForFlag ("--hog"), false));
        if (valueForFlag ("--native-rate").isNotEmpty())
            context->engine().setNativeSampleRateSwitching (parseOnOff (valueForFlag ("--native-rate"), true));
        if (valueForFlag ("--dither").isNotEmpty())
            context->engine().setDitherEnabled (parseOnOff (valueForFlag ("--dither"), true));

        // DSP / reference startup state (same public API the UI controls use).
        if (args.contains ("--reference"))
            context->setReferenceMode (true);
        if (bassText.isNotEmpty())
            context->applyUserBassGain ((float) juce::jlimit (-12.0, 12.0, bassText.getDoubleValue()));
        if (eqModeText == "manual")
            context->setEqMode (sp::EqMode::manual);
        else if (eqModeText == "autogenre" || eqModeText == "auto")
            context->setEqMode (sp::EqMode::autoGenre);
        else if (eqModeText == "intelligent")
            context->setEqMode (sp::EqMode::intelligent);
        if (profileId.isNotEmpty())
            context->selectProfile (profileId);

        mainWindow = std::make_unique<sp::MainWindow> (*context);
        mainWindow->setVisible (true);

        if (statusLogPath.isNotEmpty())
        {
            const juce::File logFile (statusLogPath);
            logFile.getParentDirectory().createDirectory();
            statusLogger = std::make_unique<StatusLogger> (*context, logFile);
            juce::Logger::writeToLog ("Status log: " + logFile.getFullPathName());
        }

        if (quitAfterText.isNotEmpty())
            delayedQuit = std::make_unique<DelayedQuit> (quitAfterText.getIntValue());

        if (args.contains ("--full-player"))
            mainWindow->getMainComponent().setFullPlayer (true);
        if (args.contains ("--windowed"))
            mainWindow->getMainComponent().setFullPlayer (false);

#if SP_MAIN_HAVE_DSP_STUDIO
        if (args.contains ("--dsp-studio"))
            sp::DspStudioWindow::show (*context);
#endif
#if SP_MAIN_HAVE_SETTINGS
        if (args.contains ("--settings") || args.contains ("--network-settings"))
        {
            auto* settings = sp::SettingsWindow::show (*context);
            if (settings != nullptr && args.contains ("--network-settings"))
                settings->setTab (4);   // NETWORK tab
        }
#endif

        startup = std::make_unique<StartupController> (*context, *mainWindow, viewToShow, startupPlaylist,
                                                      startupPlaylistFile, shouldPlay, playWholeLibrary,
                                                      playTitle, hasVolume, volumeText.getDoubleValue(),
                                                      recordPath, seekAfterStartMs);
    }

    void shutdown() override
    {
        delayedQuit.reset();
        startup.reset();
        statusLogger.reset();

        // The window destroys any auxiliary DSP/settings windows before the
        // AppContext goes away. Guard the window teardown: an AppKit exception
        // during native window destruction must not abort the process.
        juce::String windowError;
        sp::runGuardingObjCExceptions ([this] { mainWindow.reset(); }, windowError);
        if (windowError.isNotEmpty())
            juce::Logger::writeToLog ("Window teardown: " + windowError);

        if (context != nullptr)
        {
            context->shutdown();
            context = nullptr;
        }

        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        juce::Logger::setCurrentLogger (nullptr);
        logger.reset();
    }

    void systemRequestedQuit() override
    {
        if (quitPending.exchange (true))
            return;
        // Defer so AppKit termination never runs inside a key/menu command.
        juce::MessageManager::callAsync ([this]
        {
            juce::String error;
            if (! sp::runGuardingObjCExceptions ([this] { quit(); }, error))
                juce::Logger::writeToLog ("Quit: " + error);
        });
    }

    void anotherInstanceStarted (const juce::String&) override
    {
        if (mainWindow != nullptr)
        {
            mainWindow->setVisible (true);
            mainWindow->toFront (true);
        }
    }

private:
    sp::SpLookAndFeel lookAndFeel;
    std::unique_ptr<sp::AppContext> context;
    std::unique_ptr<sp::MainWindow> mainWindow;
    std::unique_ptr<StartupController> startup;
    std::unique_ptr<StatusLogger> statusLogger;
    std::unique_ptr<DelayedQuit> delayedQuit;
    std::unique_ptr<juce::FileLogger> logger;
    std::atomic<bool> quitPending { false };
    juce::String bootError;
};

START_JUCE_APPLICATION (SnakePuppyApplication)
