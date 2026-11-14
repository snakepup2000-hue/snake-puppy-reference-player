/*
    Snake Puppy Reference Player
    Library core tests: scanning, tags/artwork integration, persistence,
    incremental rescans, missing files, duplicate detection, analysis cache,
    ratings/play counts, snapshots, schema migration, DSD headers and the
    folder watcher.

    Every test works on a throw-away copy of Tests/Fixtures under the system
    temporary directory; the checked-in fixtures are never modified.

    Measured values are printed so a human can see what the scanner really did.
*/
#include "TestFramework.h"

#include "Core/ApplicationPaths.h"
#include "Library/LibraryDatabase.h"
#include "Library/LibraryScanner.h"
#include "Library/LibraryService.h"
#include "Library/LibraryStore.h"
#include "Library/Tags.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <sstream>
#include <set>
#include <string>
#include <vector>

namespace
{

//==============================================================================
juce::File repoRoot() { return juce::File (SP_SOURCE_DIR); }
juce::File fixturesDir() { return repoRoot().getChildFile ("Tests").getChildFile ("Fixtures"); }

void pumpMessages (int milliseconds)
{
    if (auto* manager = juce::MessageManager::getInstanceWithoutCreating())
        manager->runDispatchLoopUntil (milliseconds);
}

bool waitFor (const std::function<bool()>& predicate, int timeoutMs = 30000)
{
    const auto start = juce::Time::getMillisecondCounter();

    while (! predicate())
    {
        if (juce::Time::getMillisecondCounter() - start > (juce::uint32) timeoutMs)
            return false;
        pumpMessages (10);
    }

    // Deliver notifications that were posted just before the predicate became true.
    pumpMessages (20);
    return true;
}

//==============================================================================
juce::File testBaseDirectory()
{
    // Tests must never write inside Tests/Fixtures. A repo-local override is
    // available for CI, otherwise use the system temporary directory.
    if (auto overridePath = juce::SystemStats::getEnvironmentVariable ("SP_TEST_TMP", {});
        overridePath.isNotEmpty())
        return juce::File (overridePath).getChildFile ("snake-puppy-library-tests");

    return juce::File ("/tmp").getChildFile ("snake-puppy-library-tests");
}

struct TempDir
{
    juce::File dir;

    explicit TempDir (const juce::String& label)
    {
        auto base = testBaseDirectory();
        base.createDirectory();

        dir = base.getChildFile (label + "-"
                                 + juce::String::toHexString (juce::Time::currentTimeMillis())
                                 + "-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
        dir.deleteRecursively();
        dir.createDirectory();
    }

    ~TempDir() { dir.deleteRecursively(); }

    JUCE_DECLARE_NON_COPYABLE (TempDir)
};

bool copyTree (const juce::File& source, const juce::File& destination)
{
    if (! source.isDirectory())
        return false;

    for (const auto& entry : juce::RangedDirectoryIterator (source, true, "*", juce::File::findFiles))
    {
        const auto file = entry.getFile();
        const auto relative = file.getRelativePathFrom (source);
        const auto target = destination.getChildFile (relative);
        if (target.getParentDirectory().createDirectory().failed() && ! target.getParentDirectory().isDirectory())
            return false;
        if (! file.copyFileTo (target))
            return false;
    }
    return true;
}

//==============================================================================
struct Recorder : sp::LibraryService::Listener
{
    int changed = 0;
    int locationsChanged = 0;
    int scanFinishedCount = 0;
    int added = 0, updated = 0, removed = 0, missing = 0;
    int errors = 0;
    juce::String lastError;

    int progressCount = 0;
    sp::ScanProgress lastProgress;

    bool metadataFinished = false;
    bool metadataSuccess = false;
    sp::TrackId metadataTrackId = 0;
    juce::String metadataError;

    void libraryChanged() override { ++changed; }
    void libraryScanProgress (const sp::ScanProgress& p) override { lastProgress = p; ++progressCount; }
    void libraryScanFinished (int a, int u, int r, int m) override
    {
        ++scanFinishedCount;
        added = a; updated = u; removed = r; missing = m;
    }
    void libraryLocationsChanged() override { ++locationsChanged; }
    void libraryError (const juce::String& e) override { ++errors; lastError = e; }
    void libraryMetadataWriteFinished (sp::TrackId id, bool success, const juce::String& error) override
    {
        metadataFinished = true;
        metadataSuccess = success;
        metadataTrackId = id;
        metadataError = error;
    }
};

//==============================================================================
struct LibraryHarness
{
    TempDir temp;
    sp::ApplicationPaths paths;
    std::unique_ptr<sp::LibraryDatabase> database;
    std::unique_ptr<sp::LibraryService> service;
    juce::File copyRoot;
    Recorder recorder;

    bool ok = true;
    juce::String error;

    explicit LibraryHarness (const juce::String& label) : temp ("library-" + label)
    {
        paths = sp::ApplicationPaths::forRoot (temp.dir);

        juce::String dirError;
        if (! paths.ensureDirectoriesExist (dirError))
        {
            ok = false;
            error = dirError;
            return;
        }

        copyRoot = temp.dir.getChildFile ("fixtures");
        if (! copyTree (fixturesDir(), copyRoot))
        {
            ok = false;
            error = "Could not copy Tests/Fixtures to " + copyRoot.getFullPathName();
            return;
        }

        open();
    }

    ~LibraryHarness() { close(); }

    void open()
    {
        database = std::make_unique<sp::LibraryDatabase>();
        if (auto e = database->open (paths.libraryDb); e.isNotEmpty())
        {
            ok = false;
            error = e;
            return;
        }
        if (auto e = database->initialiseSchema(); e.isNotEmpty())
        {
            ok = false;
            error = e;
            return;
        }

        service = std::make_unique<sp::LibraryService> (paths, *database);
        service->addListener (&recorder);

        if (auto e = service->initialise(); e.isNotEmpty())
        {
            ok = false;
            error = e;
        }
    }

    void close()
    {
        if (service != nullptr)
        {
            service->removeListener (&recorder);
            service->shutdown();
            service.reset();
        }
        if (database != nullptr)
        {
            database->close();
            database.reset();
        }
    }

    juce::File libraryRoot() const { return copyRoot.getChildFile ("Library"); }

    juce::File fileInLibrary (const juce::String& relativePath) const
    {
        return libraryRoot().getChildFile (relativePath);
    }

    bool waitForIdle (int timeoutMs = 30000)
    {
        return waitFor ([this] { return ! service->isScanning(); }, timeoutMs);
    }

    bool addLocationAndScan (bool watch, sp::LocationId* idOut = nullptr)
    {
        sp::LocationId id = 0;
        service->addLocation (libraryRoot(), watch, [&id] (sp::LocationId newId) { id = newId; });

        const bool idle = waitForIdle();
        waitFor ([&id] { return id != 0; }, 3000);

        if (idOut != nullptr)
            *idOut = id;

        return idle && id != 0;
    }

    int expectedAudioFileCount() const
    {
        int count = 0;
        for (auto& file : libraryRoot().findChildFiles (juce::File::findFiles, true, "*"))
            if (sp::scanner::isSupportedAudioFile (file))
                ++count;
        return count;
    }

    sp::Track trackNamed (const juce::String& fileName) const
    {
        auto snapshot = service->getSnapshot();
        for (auto& track : snapshot->tracks)
            if (juce::File (track.path).getFileName().equalsIgnoreCase (fileName))
                return track;
        return {};
    }

    std::set<sp::TrackId> trackIds() const
    {
        std::set<sp::TrackId> ids;
        auto snapshot = service->getSnapshot();
        for (auto& track : snapshot->tracks)
            ids.insert (track.id);
        return ids;
    }

    bool rescanAndWait (bool refreshMetadata = false)
    {
        service->rescanAll (refreshMetadata);
        return waitForIdle();
    }

    bool refreshMissingAndWait()
    {
        service->refreshMissingState();
        pumpMessages (30);
        return true;
    }
};

//==============================================================================
struct TagSupport
{
    bool read = false;
    bool write = false;
    bool artwork = false;
    juce::String note;
};

TagSupport detectTagSupport (const LibraryHarness& harness)
{
    TagSupport support;
    const auto flacFile = harness.fileInLibrary ("Album A/01 - Track One.flac");

    {
        sp::tags::TagData data;
        juce::String error;
        support.read = sp::tags::readTags (flacFile, data, error) && data.readOk && data.title.isNotEmpty();
        if (! support.read)
            support.note = error.isNotEmpty() ? error : juce::String ("tag reader returned no data");
    }

    {
        juce::String error;
        support.artwork = ! sp::tags::readArtwork (flacFile, error).empty();
    }

    {
        // Capability probe on a throw-away copy so fixtures stay untouched.
        auto probe = harness.temp.dir.getChildFile ("tag-write-probe.flac");
        probe.deleteFile();
        if (flacFile.copyFileTo (probe))
        {
            sp::tags::TagData data;
            data.title = "probe";
            juce::String error;
            support.write = sp::tags::writeTags (probe, data, error);
            probe.deleteFile();
        }
    }

    return support;
}

std::string trackDescription (const sp::Track& track)
{
    std::ostringstream details;
    details << juce::File (track.path).getFileName() << "  format=" << track.formatName
            << " rate=" << track.sampleRate << " depth=" << track.bitDepth
            << " ch=" << track.numChannels << " dur=" << track.durationMs << "ms"
            << " family=" << (int) track.family << " dsd=" << (track.isDsd ? "yes" : "no");
    return details.str();
}

} // namespace

//==============================================================================
SP_TEST (Library, ScanFixtures)
{
    LibraryHarness harness ("scan");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());

    sp::LocationId locationId = 0;
    SP_REQUIRE_MSG (harness.addLocationAndScan (false, &locationId), "location scan did not finish");

    auto snapshot = harness.service->getSnapshot();
    const int expected = harness.expectedAudioFileCount();

    std::cout << "[Library] playable fixture files: " << expected
              << ", snapshot tracks: " << (int) snapshot->tracks.size()
              << ", full scan took " << harness.recorder.lastProgress.filesProcessed
              << " metadata reads\n";

    SP_REQUIRE_MSG ((int) snapshot->tracks.size() == expected,
                    "track count " + std::to_string ((int) snapshot->tracks.size())
                        + " != files on disk " + std::to_string (expected));
    SP_REQUIRE_MSG (expected >= 10, "fixtures look incomplete: " + std::to_string (expected) + " playable files");

    SP_REQUIRE_MSG (harness.recorder.scanFinishedCount > 0, "no scanFinished callback was delivered");
    SP_REQUIRE_MSG (harness.recorder.lastProgress.filesFound == expected,
                    "final progress filesFound " + std::to_string (harness.recorder.lastProgress.filesFound)
                        + " != " + std::to_string (expected));

    // Nested folders are all represented.
    std::set<juce::String> folders;
    for (auto& track : snapshot->tracks)
        folders.insert (juce::File (track.path).getParentDirectory().getRelativePathFrom (harness.libraryRoot()));

    for (auto* folder : { "Album A", "Album B", "Album B/Disc 2", "DSD", "Duplicates" })
        SP_REQUIRE_MSG (folders.count (folder) == 1, std::string ("missing nested folder: ") + folder);

    // Formats / rates / durations.
    struct Expectation
    {
        const char* fileName;
        int sampleRate;
        const char* formatContains;
        int minBitDepth;        // lossy codecs report 0 (no meaningful integer depth)
        int minDurationMs;      // MP3/AAC include encoder priming/padding
        int maxDurationMs;
    };

    const Expectation expectations[] = {
        { "01 - Track One.flac",      96000, "FLAC", 1, 1950, 2050 },
        { "02 - Track Two.flac",      44100, "FLAC", 1, 1950, 2050 },
        { "03 - Track Three.mp3",     44100, "MP3",  0, 2000, 2100 },
        { "01 - Rhythm Section.wav",  48000, "WAV",  1, 1950, 2050 },
        { "02 - Big Room.aiff",       44100, "AIFF", 1, 1950, 2050 },
        { "03 - Slow Burn.m4a",       44100, "M4A",  1, 1950, 2050 },
        { "04 - Pocket Groove.ogg",   44100, "OGG",  0, 1950, 2050 },
        { "01 - Late Night.m4a",      44100, "M4A",  0, 1950, 2050 },
        { "Track One (copy).flac",    96000, "FLAC", 1, 1950, 2050 }
    };

    for (auto& expectation : expectations)
    {
        const auto track = harness.trackNamed (expectation.fileName);
        SP_REQUIRE_MSG (track.id != 0, std::string ("missing track: ") + expectation.fileName);

        std::cout << "[Library]   " << trackDescription (track) << "\n";

        SP_REQUIRE_MSG (track.sampleRate == expectation.sampleRate,
                        trackDescription (track) + " sample rate mismatch");
        SP_REQUIRE_MSG (track.formatName.containsIgnoreCase (expectation.formatContains),
                        trackDescription (track) + " format name mismatch");
        SP_REQUIRE_MSG (track.durationMs >= expectation.minDurationMs
                            && track.durationMs <= expectation.maxDurationMs,
                        trackDescription (track) + " duration outside the expected window");
        SP_REQUIRE_MSG (track.bitDepth >= expectation.minBitDepth,
                        trackDescription (track) + " bit depth was not reported");
        SP_REQUIRE_MSG (track.numChannels > 0, trackDescription (track) + " channel count was not reported");
        SP_REQUIRE_MSG (track.fileSize > 0, trackDescription (track) + " file size missing");
        SP_REQUIRE_MSG (track.dateAddedIso.isNotEmpty(), trackDescription (track) + " date added missing");
        SP_REQUIRE_MSG (track.bitrateKbps > 0, trackDescription (track) + " bitrate not estimated");
    }

    // DSD fixtures are detected natively even before the DSD decoder exists.
    for (auto* dsdName : { "01 - DSD Test Tone.dsf", "02 - DSD Test Tone.dff" })
    {
        const auto track = harness.trackNamed (dsdName);
        SP_REQUIRE_MSG (track.id != 0, std::string ("missing DSD track: ") + dsdName);
        std::cout << "[Library]   " << trackDescription (track) << "\n";
        SP_REQUIRE_MSG (track.isDsd, trackDescription (track) + " not flagged as DSD");
        SP_REQUIRE_MSG (track.family == sp::Track::Family::dsd, trackDescription (track) + " wrong family");
        SP_REQUIRE_MSG (track.sampleRate == 2822400, trackDescription (track) + " wrong DSD rate");
        SP_REQUIRE_MSG (track.dsdRateMultiple == 64, trackDescription (track) + " wrong DSD multiple");
        SP_REQUIRE_MSG (track.bitDepth == 1, trackDescription (track) + " wrong DSD bit depth");
        SP_REQUIRE_MSG (track.durationMs >= 900 && track.durationMs <= 1100,
                        trackDescription (track) + " DSD duration outside 900..1100 ms");
    }

    // Tag fields when the tag layer supports them; fallback title otherwise.
    const auto support = detectTagSupport (harness);
    const auto trackOne = harness.trackNamed ("01 - Track One.flac");

    if (support.read)
    {
        SP_REQUIRE_MSG (trackOne.title == "Track One", "title '" + trackOne.title.toStdString() + "'");
        SP_REQUIRE_MSG (trackOne.artist == "Test Artist", "artist '" + trackOne.artist.toStdString() + "'");
        SP_REQUIRE_MSG (trackOne.albumArtist == "Test Artist", "album artist mismatch");
        SP_REQUIRE_MSG (trackOne.album == "Album A", "album '" + trackOne.album.toStdString() + "'");
        SP_REQUIRE_MSG (trackOne.genre == "Jazz", "genre '" + trackOne.genre.toStdString() + "'");
        SP_REQUIRE_MSG (trackOne.year == 1975, "year " + std::to_string (trackOne.year));
        SP_REQUIRE_MSG (trackOne.trackNumber == 1, "track number " + std::to_string (trackOne.trackNumber));

        const auto discTwo = harness.trackNamed ("01 - Late Night.m4a");
        SP_REQUIRE_MSG (discTwo.album == "Album B/Disc 2", "disc 2 album '" + discTwo.album.toStdString() + "'");
        std::cout << "[Library] tag fields verified against the FLAC/M4A fixtures\n";
    }
    else
    {
        std::cout << "[Library] tag layer not available yet (" << support.note
                  << "); asserting filename fallback title instead\n";
        SP_REQUIRE_MSG (trackOne.title.isNotEmpty(), "fallback title was empty");
    }

    SP_REQUIRE_MSG (snapshot->locations.size() == 1, "expected exactly one location");
    SP_REQUIRE_MSG (snapshot->locations[0].id == locationId, "location id mismatch in snapshot");
    SP_REQUIRE_MSG (snapshot->locations[0].path == harness.libraryRoot().getFullPathName(),
                    "location path mismatch");
    SP_REQUIRE_MSG (snapshot->locations[0].trackCount == expected,
                    "location track count " + std::to_string (snapshot->locations[0].trackCount)
                        + " != " + std::to_string (expected));

    std::cout << "[Library] scanFinished(added=" << harness.recorder.added
              << ", updated=" << harness.recorder.updated
              << ", removed=" << harness.recorder.removed
              << ", missing=" << harness.recorder.missing << ")\n";
    SP_REQUIRE_MSG (harness.recorder.added == expected, "fresh scan should add every file");
}

//==============================================================================
SP_TEST (Library, ArtworkCache)
{
    LibraryHarness harness ("artwork");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());
    SP_REQUIRE_MSG (harness.addLocationAndScan (false), "scan failed");

    const auto support = detectTagSupport (harness);
    auto track = harness.trackNamed ("01 - Track One.flac");
    SP_REQUIRE (track.id != 0);

    const auto flacFile = harness.fileInLibrary ("Album A/01 - Track One.flac");
    juce::String artworkError;
    const auto embedded = sp::tags::readArtwork (flacFile, artworkError);

    if (embedded.empty())
    {
        std::cout << "[Library] tag layer exposes no artwork yet (" << artworkError
                  << "); checking graceful empty result\n";
        SP_REQUIRE (harness.service->artworkForTrack (track) == juce::File());
        return;
    }

    SP_REQUIRE_MSG (support.artwork, "readArtwork returned data but detectTagSupport disagreed");

    const auto cached = harness.service->artworkForTrack (track);
    SP_REQUIRE_MSG (cached.existsAsFile(), "artworkForTrack did not produce a cached file");
    SP_REQUIRE_MSG (cached.getSize() > 0, "cached artwork file is empty");
    SP_REQUIRE_MSG (cached.isAChildOf (harness.paths.artworkDir), "cached artwork is outside the artwork dir");

    if (track.artworkId.isNotEmpty())
    {
        const auto byId = harness.service->artworkFileFor (track.artworkId);
        SP_REQUIRE_MSG (byId.existsAsFile(), "artworkFileFor did not find the cached artwork");
        SP_REQUIRE_MSG (byId == cached, "artworkFileFor and artworkForTrack disagree");
        SP_REQUIRE_MSG (track.artworkId.length() == 32, "artwork id is not a 32 char hash");
        SP_REQUIRE_MSG (! harness.service->artworkFileFor ("00000000000000000000000000000000").existsAsFile(),
                        "artworkFileFor invented a cache entry for an unknown id");
        std::cout << "[Library] cached artwork: " << cached.getFileName()
                  << " (" << (int) cached.getSize() << " bytes) id=" << track.artworkId << "\n";
    }
    else
    {
        std::cout << "[Library] artwork extracted on demand (scanner had not cached it): "
                  << cached.getFileName() << "\n";
    }
}

//==============================================================================
SP_TEST (Library, PersistenceAcrossReopen)
{
    LibraryHarness harness ("persist");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());

    sp::LocationId locationId = 0;
    SP_REQUIRE_MSG (harness.addLocationAndScan (true, &locationId), "initial scan failed");

    const auto firstSnapshot = harness.service->getSnapshot();
    const auto firstIds = harness.trackIds();
    const int firstCount = (int) firstSnapshot->tracks.size();
    const int firstExpected = harness.expectedAudioFileCount();
    SP_REQUIRE (firstCount == firstExpected);

    const auto locationPath = harness.libraryRoot().getFullPathName();
    harness.close();
    harness.open();
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());

    // The persisted library is visible immediately (snapshot built from the DB),
    // and the startup incremental rescan must not change it.
    SP_REQUIRE_MSG ((int) harness.service->getSnapshot()->tracks.size() == firstCount,
                    "track count changed immediately after reopen");
    SP_REQUIRE_MSG (harness.waitForIdle(), "startup rescan did not finish");

    auto snapshot = harness.service->getSnapshot();
    SP_REQUIRE_MSG ((int) snapshot->tracks.size() == firstCount, "track count changed after startup rescan");
    SP_REQUIRE_MSG (harness.trackIds() == firstIds, "track ids changed across reopen");

    SP_REQUIRE_MSG (snapshot->locations.size() == 1, "location was not persisted");
    SP_REQUIRE_MSG (snapshot->locations[0].path == locationPath, "persisted location path mismatch");
    SP_REQUIRE_MSG (snapshot->locations[0].watch, "location watch flag was not persisted");
    SP_REQUIRE_MSG (juce::File (harness.paths.libraryDb).existsAsFile(), "database file does not exist");

    std::cout << "[Library] reopened database: " << firstCount
              << " tracks, " << (int) snapshot->locations.size() << " location(s), ids stable\n";
}

//==============================================================================
SP_TEST (Library, OfflineLocationKeepsRows)
{
    LibraryHarness harness ("offline");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());

    sp::LocationId locationId = 0;
    SP_REQUIRE_MSG (harness.addLocationAndScan (false, &locationId), "initial scan failed");

    const int trackCount = (int) harness.service->getSnapshot()->tracks.size();
    SP_REQUIRE (trackCount > 0);

    harness.close();

    // Simulate an unmounted drive: the location folder disappears.
    const auto mounted = harness.libraryRoot();
    const auto unmounted = harness.copyRoot.getChildFile ("Library-unmounted");
    SP_REQUIRE_MSG (mounted.moveFileTo (unmounted), "could not move the fixture folder");

    harness.open();
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());
    SP_REQUIRE (harness.waitForIdle());

    auto snapshot = harness.service->getSnapshot();
    SP_REQUIRE_MSG ((int) snapshot->tracks.size() == trackCount,
                    "offline location lost track rows");
    SP_REQUIRE_MSG (snapshot->locations.size() == 1, "offline location was dropped");
    SP_REQUIRE_MSG (! snapshot->locations[0].online, "offline location is still marked online");
    int missingCount = 0;
    for (auto& track : snapshot->tracks)
        if (track.missing)
            ++missingCount;
    SP_REQUIRE_MSG (missingCount == trackCount, "offline tracks were not all marked missing");

    // Bring the drive back: the startup rescan clears the missing flags and keeps ids.
    const auto idsBefore = harness.trackIds();
    harness.close();
    SP_REQUIRE_MSG (unmounted.moveFileTo (mounted), "could not restore the fixture folder");
    harness.open();
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());
    SP_REQUIRE (harness.waitForIdle());

    snapshot = harness.service->getSnapshot();
    SP_REQUIRE_MSG ((int) snapshot->tracks.size() == trackCount, "restored location lost rows");
    SP_REQUIRE_MSG (snapshot->locations[0].online, "restored location is still offline");
    SP_REQUIRE_MSG (harness.trackIds() == idsBefore, "track ids changed across offline/online cycle");
    for (auto& track : snapshot->tracks)
        SP_REQUIRE_MSG (! track.missing, "restored track is still marked missing");

    std::cout << "[Library] offline location: " << trackCount
              << " rows kept and marked missing, restored with the same ids\n";
}

//==============================================================================
SP_TEST (Library, LocationWatchTogglePersists)
{
    LibraryHarness harness ("watchtoggle");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());

    sp::LocationId locationId = 0;
    SP_REQUIRE_MSG (harness.addLocationAndScan (true, &locationId), "initial scan failed");
    SP_REQUIRE (harness.service->getSnapshot()->locations[0].watch);

    SP_REQUIRE_MSG (harness.service->setLocationWatch (locationId, false), "could not disable watching");
    pumpMessages (20);
    SP_REQUIRE_MSG (! harness.service->getSnapshot()->locations[0].watch, "watch flag did not change");

    harness.close();
    harness.open();
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());
    SP_REQUIRE (harness.waitForIdle());
    SP_REQUIRE_MSG (! harness.service->getSnapshot()->locations[0].watch,
                    "watch flag did not persist across reopen");

    SP_REQUIRE_MSG (harness.service->setLocationWatch (locationId, true), "could not re-enable watching");
    pumpMessages (20);
    SP_REQUIRE (harness.service->getSnapshot()->locations[0].watch);

    std::cout << "[Library] location watch toggle persisted (off across reopen, on again)\n";
}

//==============================================================================
SP_TEST (Library, IncrementalRescanNoDuplicates)
{
    LibraryHarness harness ("incremental");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());
    SP_REQUIRE_MSG (harness.addLocationAndScan (false), "initial scan failed");

    const auto firstIds = harness.trackIds();
    const int firstCount = (int) firstIds.size();
    const int firstReads = harness.recorder.lastProgress.filesProcessed;
    SP_REQUIRE_MSG (firstReads == firstCount, "fresh scan should read every file");
    SP_REQUIRE_MSG (harness.recorder.lastProgress.filesFound == firstCount, "filesFound mismatch");

    SP_REQUIRE_MSG (harness.rescanAndWait(), "incremental rescan did not finish");

    const int secondCount = (int) harness.service->getSnapshot()->tracks.size();
    const int secondReads = harness.recorder.lastProgress.filesProcessed;
    const int secondFound = harness.recorder.lastProgress.filesFound;

    std::cout << "[Library] rescan: found " << secondFound << ", metadata re-reads " << secondReads
              << " (first scan read " << firstReads << ")\n";

    SP_REQUIRE_MSG (secondCount == firstCount, "rescan duplicated or dropped rows");
    SP_REQUIRE_MSG (harness.trackIds() == firstIds, "track ids changed on rescan");
    SP_REQUIRE_MSG (secondReads == 0, "unchanged files were re-read on the second scan");

    SP_REQUIRE_MSG (harness.recorder.added == 0, "incremental rescan should not add tracks");
    SP_REQUIRE_MSG (harness.recorder.updated == 0, "incremental rescan should not update tracks");
}

//==============================================================================
SP_TEST (Library, MissingFileKeptInLibrary)
{
    LibraryHarness harness ("missing");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());
    SP_REQUIRE_MSG (harness.addLocationAndScan (false), "scan failed");

    const auto track = harness.trackNamed ("03 - Track Three.mp3");
    SP_REQUIRE (track.id != 0);
    const auto file = juce::File (track.path);

    juce::MemoryBlock saved;
    SP_REQUIRE_MSG (file.loadFileAsData (saved), "could not read the test file");
    SP_REQUIRE_MSG (file.deleteFile(), "could not delete the test file");

    harness.refreshMissingAndWait();

    auto snapshot = harness.service->getSnapshot();
    const auto* after = snapshot->findTrack (track.id);
    SP_REQUIRE_MSG (after != nullptr, "missing track disappeared from the snapshot");
    SP_REQUIRE_MSG (after->missing, "track was not marked missing");
    SP_REQUIRE_MSG (juce::File (after->path) == file, "missing track path changed");

    // A rescan must keep the row marked missing, not delete it.
    SP_REQUIRE_MSG (harness.rescanAndWait(), "rescan failed");
    snapshot = harness.service->getSnapshot();
    SP_REQUIRE_MSG (snapshot->findTrack (track.id) != nullptr, "rescan deleted the missing track");
    SP_REQUIRE_MSG (snapshot->findTrack (track.id)->missing, "rescan cleared the missing flag");

    // Restore the file: the exact same row becomes present again.
    SP_REQUIRE_MSG (file.replaceWithData (saved.getData(), saved.getSize()), "could not restore the test file");
    harness.refreshMissingAndWait();
    snapshot = harness.service->getSnapshot();
    SP_REQUIRE_MSG (snapshot->findTrack (track.id) != nullptr, "restored track missing from snapshot");
    SP_REQUIRE_MSG (! snapshot->findTrack (track.id)->missing, "restored track is still marked missing");

    std::cout << "[Library] missing detection kept row id " << track.id
              << " through delete/rescan/restore\n";
}

//==============================================================================
SP_TEST (Library, DuplicateDetection)
{
    LibraryHarness harness ("duplicates");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());
    SP_REQUIRE_MSG (harness.addLocationAndScan (false), "scan failed");

    const int expected = harness.expectedAudioFileCount();
    const int hashed = harness.service->computeMissingHashes (0);
    pumpMessages (20);

    std::cout << "[Library] hashed " << hashed << " of " << expected << " track(s)\n";
    SP_REQUIRE_MSG (hashed == expected, "not all tracks were hashed");

    const auto groups = harness.service->findDuplicates (true);
    SP_REQUIRE_MSG (groups.size() == 1, "expected exactly one duplicate group, got "
                                        + std::to_string ((int) groups.size()));

    const auto original = harness.trackNamed ("01 - Track One.flac");
    const auto copy = harness.trackNamed ("Track One (copy).flac");
    SP_REQUIRE (original.id != 0);
    SP_REQUIRE (copy.id != 0);

    const auto& group = groups.front();
    SP_REQUIRE_MSG (group.size() == 2, "duplicate group should have 2 members");
    SP_REQUIRE_MSG (std::find (group.begin(), group.end(), original.id) != group.end(),
                    "duplicate group does not contain the original");
    SP_REQUIRE_MSG (std::find (group.begin(), group.end(), copy.id) != group.end(),
                    "duplicate group does not contain the copy");

    const auto track = harness.service->getTrack (original.id);
    SP_REQUIRE_MSG (track.hashComputed, "hash flag was not stored");
    SP_REQUIRE_MSG (track.contentHash.length() == 32, "content hash is not 32 hex chars");
    SP_REQUIRE_MSG (harness.service->getTrack (copy.id).contentHash == track.contentHash,
                    "identical files produced different content hashes");

    // The size/duration heuristic finds the same byte-identical pair too.
    const auto heuristicGroups = harness.service->findDuplicates (false);
    bool heuristicFoundPair = false;
    for (auto& heuristicGroup : heuristicGroups)
        if (std::find (heuristicGroup.begin(), heuristicGroup.end(), original.id) != heuristicGroup.end()
            && std::find (heuristicGroup.begin(), heuristicGroup.end(), copy.id) != heuristicGroup.end())
            heuristicFoundPair = true;
    SP_REQUIRE_MSG (heuristicFoundPair, "size/duration duplicate heuristic missed the identical files");

    // Health check reports the same duplicate group.
    const auto report = harness.service->runHealthCheck();
    SP_REQUIRE_MSG (! report.duplicateGroups.empty(), "health check did not report duplicates");

    std::cout << "[Library] duplicate group: " << group.size() << " tracks, hash "
              << track.contentHash << "\n";
}

//==============================================================================
SP_TEST (Library, AnalysisCacheRoundtrip)
{
    LibraryHarness harness ("analysis");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());
    SP_REQUIRE_MSG (harness.addLocationAndScan (false), "scan failed");

    const auto track = harness.trackNamed ("01 - Rhythm Section.wav");
    SP_REQUIRE (track.id != 0);

    sp::TrackAnalysis analysis;
    analysis.valid = true;
    analysis.version = sp::TrackAnalysis::kVersion;
    analysis.analyzedSeconds = 1.75;
    analysis.integratedLufs = -14.25;
    analysis.loudnessRangeLu = 3.5;
    analysis.samplePeakDb = -1.5;
    analysis.truePeakDb = -0.75;
    analysis.crestFactorDb = 11.25;
    analysis.spectralCentroidHz = 1234.5;
    analysis.lowEnergyRatio = 0.11;
    analysis.subEnergyRatio = 0.05;
    analysis.midEnergyRatio = 0.62;
    analysis.highEnergyRatio = 0.22;
    analysis.lowBandDb = -30.0;
    analysis.midBandDb = -18.0;
    analysis.highBandDb = -26.0;
    analysis.analyzer = "test-analyzer/1";

    harness.service->saveAnalysis (track.id, analysis);

    sp::TrackAnalysis loaded;
    SP_REQUIRE_MSG (harness.service->getAnalysis (track.id, loaded), "analysis was not found");
    SP_REQUIRE_MSG (loaded.valid, "loaded analysis is not valid");
    SP_REQUIRE_NEAR (loaded.integratedLufs, analysis.integratedLufs, 1e-9);
    SP_REQUIRE_NEAR (loaded.truePeakDb, analysis.truePeakDb, 1e-9);
    SP_REQUIRE_NEAR (loaded.spectralCentroidHz, analysis.spectralCentroidHz, 1e-9);
    SP_REQUIRE_MSG (loaded.analyzer == analysis.analyzer, "analyzer name did not round-trip");

    // Still there after reopening the database.
    harness.close();
    harness.open();
    SP_REQUIRE (harness.waitForIdle());

    sp::TrackAnalysis reloaded;
    SP_REQUIRE_MSG (harness.service->getAnalysis (track.id, reloaded), "analysis did not persist");
    SP_REQUIRE_NEAR (reloaded.lowEnergyRatio, analysis.lowEnergyRatio, 1e-9);
    SP_REQUIRE_MSG (reloaded.analyzer == analysis.analyzer, "persisted analyzer name mismatch");

    // Unknown track ids simply report nothing.
    sp::TrackAnalysis absent;
    SP_REQUIRE_MSG (! harness.service->getAnalysis (999999, absent), "unknown track had analysis");

    std::cout << "[Library] analysis cache roundtrip: LUFS " << loaded.integratedLufs
              << ", centroid " << loaded.spectralCentroidHz << " Hz\n";
}

//==============================================================================
SP_TEST (Library, RatingAndPlayCountPersist)
{
    LibraryHarness harness ("stats");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());
    SP_REQUIRE_MSG (harness.addLocationAndScan (false), "scan failed");

    const auto track = harness.trackNamed ("02 - Track Two.flac");
    SP_REQUIRE (track.id != 0);

    harness.service->updateRating (track.id, 4);
    harness.service->incrementPlayCount (track.id);
    harness.service->incrementPlayCount (track.id);
    pumpMessages (20);

    const auto inMemory = harness.service->getTrack (track.id);
    SP_REQUIRE_MSG (inMemory.rating == 4, "rating was not saved");
    SP_REQUIRE_MSG (inMemory.playCount == 2, "play count was not incremented twice");
    SP_REQUIRE_MSG (inMemory.lastPlayedMs > 0, "last played timestamp was not stored");

    auto snapshotTrack = harness.service->getSnapshot()->findTrack (track.id);
    SP_REQUIRE (snapshotTrack != nullptr);
    SP_REQUIRE_MSG (snapshotTrack->rating == 4, "snapshot rating not updated");
    SP_REQUIRE_MSG (snapshotTrack->playCount == 2, "snapshot play count not updated");

    harness.close();
    harness.open();
    SP_REQUIRE (harness.waitForIdle());

    const auto persisted = harness.service->getTrack (track.id);
    SP_REQUIRE_MSG (persisted.rating == 4, "rating did not persist across reopen");
    SP_REQUIRE_MSG (persisted.playCount == 2, "play count did not persist across reopen");

    std::cout << "[Library] rating/play count persisted: " << persisted.rating
              << " stars, " << persisted.playCount << " plays\n";
}

//==============================================================================
SP_TEST (Library, SnapshotImmutability)
{
    LibraryHarness harness ("snapshot");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());
    SP_REQUIRE_MSG (harness.addLocationAndScan (false), "scan failed");

    const auto first = harness.service->getSnapshot();
    const std::uint64_t firstVersion = first->version;
    const int firstCount = (int) first->tracks.size();
    const sp::TrackId firstId = first->tracks.empty() ? 0 : first->tracks.front().id;
    SP_REQUIRE (firstCount > 0);

    SP_REQUIRE_MSG (harness.rescanAndWait(), "second scan failed");

    const auto second = harness.service->getSnapshot();
    SP_REQUIRE_MSG (first.get() != second.get(), "a new snapshot was not published");
    SP_REQUIRE_MSG (second->version > firstVersion, "snapshot version did not increase");
    SP_REQUIRE_MSG ((int) first->tracks.size() == firstCount, "old snapshot was mutated");
    SP_REQUIRE_MSG (first->version == firstVersion, "old snapshot version changed");
    SP_REQUIRE_MSG (first->findTrack (firstId) != nullptr, "old snapshot lost a track");

    std::cout << "[Library] snapshot versions: " << firstVersion << " -> " << second->version
              << ", old snapshot still has " << (int) first->tracks.size() << " tracks\n";
}

//==============================================================================
SP_TEST (Library, SchemaMigrationKeepsRows)
{
    TempDir temp ("schema");
    auto paths = sp::ApplicationPaths::forRoot (temp.dir);
    juce::String dirError;
    SP_REQUIRE_MSG (paths.ensureDirectoriesExist (dirError), dirError.toStdString());

    sp::LibraryDatabase database;
    SP_REQUIRE_MSG (database.open (paths.libraryDb).isEmpty(), "could not open the database");

    // Simulate a very old library with only the two baseline columns.
    juce::String sqlError;
    SP_REQUIRE_MSG (database.db().execute ("CREATE TABLE tracks ("
                                          "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                          "path TEXT NOT NULL DEFAULT '')", &sqlError),
                    sqlError.toStdString());
    SP_REQUIRE_MSG (database.db().execute ("INSERT INTO tracks(path) VALUES('/legacy/file.flac')"), "legacy insert failed");

    SP_REQUIRE_MSG (database.initialiseSchema().isEmpty(), "first schema pass failed");
    SP_REQUIRE_MSG (database.initialiseSchema().isEmpty(), "schema pass is not idempotent");

    const auto tracks = sp::librarystore::loadAllTracks (database);
    SP_REQUIRE_MSG (tracks.size() == 1, "migration lost the legacy row");
    SP_REQUIRE_MSG (tracks[0].path == "/legacy/file.flac", "legacy row path changed");

    std::vector<juce::String> columns;
    {
        auto statement = database.db().prepare ("PRAGMA table_info(tracks)");
        SP_REQUIRE (statement != nullptr);
        while (statement->step())
            columns.push_back (statement->getString (1).toLowerCase());
    }

    auto hasColumn = [&columns] (const juce::String& name)
    {
        return std::find (columns.begin(), columns.end(), name) != columns.end();
    };

    for (auto* required : { "content_hash", "hash_computed", "missing", "rating", "artwork_id",
                            "date_added_iso", "play_count", "last_played_ms", "is_dsd",
                            "replaygain_track_db", "file_modified_ms" })
        SP_REQUIRE_MSG (hasColumn (required), std::string ("migration missed column: ") + required);

    SP_REQUIRE_MSG (sp::librarystore::getMeta (database, "schema_version") == "1",
                    "schema version was not recorded");
    database.close();

    std::cout << "[Library] legacy schema migrated in place: " << (int) tracks.size()
              << " row kept, " << (int) columns.size() << " track columns\n";
}

//==============================================================================
SP_TEST (Library, DsdHeadersHashedCorrectly)
{
    const auto dsf = fixturesDir().getChildFile ("Library/DSD/01 - DSD Test Tone.dsf");
    const auto dff = fixturesDir().getChildFile ("Library/DSD/02 - DSD Test Tone.dff");
    SP_REQUIRE_MSG (dsf.existsAsFile(), "DSF fixture missing");
    SP_REQUIRE_MSG (dff.existsAsFile(), "DFF fixture missing");

    for (auto* file : { &dsf, &dff })
    {
        sp::AudioFileInfo info;
        juce::String error;
        SP_REQUIRE_MSG (sp::scanner::parseDsdHeader (*file, info, error),
                        file->getFileName().toStdString() + ": " + error.toStdString());
        SP_REQUIRE_MSG (info.isDsd, file->getFileName().toStdString() + " not flagged DSD");
        SP_REQUIRE_MSG (info.sampleRate == 2822400, file->getFileName().toStdString() + " rate");
        SP_REQUIRE_MSG (info.bitDepth == 1, file->getFileName().toStdString() + " bit depth");
        SP_REQUIRE_MSG (info.numChannels == 2, file->getFileName().toStdString() + " channels");
        SP_REQUIRE_MSG (info.dsdMultiple == 64, file->getFileName().toStdString() + " multiple");
        SP_REQUIRE_MSG (info.durationMs >= 900 && info.durationMs <= 1100,
                        file->getFileName().toStdString() + " duration");
    }

    // MD5 known-answer tests (the scanner hashes content/artwork with it).
    SP_REQUIRE_MSG (sp::scanner::md5Bytes ("", 0) == "d41d8cd98f00b204e9800998ecf8427e",
                    "MD5 of empty input is wrong");
    SP_REQUIRE_MSG (sp::scanner::md5Bytes ("abc", 3) == "900150983cd24fb0d6963f7d28e17f72",
                    "MD5 of 'abc' is wrong");
    SP_REQUIRE_MSG (sp::scanner::md5Bytes ("The quick brown fox jumps over the lazy dog", 43)
                        == "9e107d9d372bb6826bd81d3542a419d6",
                    "MD5 of the quick brown fox is wrong");

    // Format probing falls back to the native DSD parser when no decoder exists.
    sp::AudioFileInfo probed;
    juce::String probeError;
    SP_REQUIRE_MSG (sp::scanner::probeAudioFile (dsf, probed, probeError), probeError.toStdString());
    SP_REQUIRE_MSG (probed.isDsd && probed.sampleRate == 2822400, "probeAudioFile DSD fallback failed");

    std::cout << "[Library] DSD headers parsed natively (2822400 Hz, 1-bit, 2 ch, ~1 s); MD5 vectors match\n";
}

//==============================================================================
SP_TEST (Library, ImportFilesWithoutLocation)
{
    LibraryHarness harness ("import");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());

    juce::StringArray files;
    files.add (harness.fileInLibrary ("Album A/02 - Track Two.flac").getFullPathName());
    files.add (harness.fileInLibrary ("Album B/01 - Rhythm Section.wav").getFullPathName());

    harness.service->importFiles (files);
    SP_REQUIRE_MSG (harness.waitForIdle(), "import scan did not finish");

    auto snapshot = harness.service->getSnapshot();
    SP_REQUIRE_MSG (snapshot->tracks.size() == 2, "import should add exactly the two files");
    SP_REQUIRE_MSG (snapshot->locations.empty(), "importFiles must not create a location");
    SP_REQUIRE_MSG (harness.trackNamed ("02 - Track Two.flac").id != 0, "imported FLAC missing");
    SP_REQUIRE_MSG (harness.trackNamed ("01 - Rhythm Section.wav").id != 0, "imported WAV missing");

    std::cout << "[Library] imported " << (int) snapshot->tracks.size()
              << " explicit files without creating a location\n";
}

//==============================================================================
SP_TEST (Library, RemoveLocationAndTracks)
{
    LibraryHarness harness ("removeloc");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());

    sp::LocationId locationId = 0;
    SP_REQUIRE_MSG (harness.addLocationAndScan (false, &locationId), "scan failed");
    SP_REQUIRE (harness.service->getSnapshot()->tracks.size() > 0);

    harness.service->removeLocation (locationId, true);
    pumpMessages (30);

    auto snapshot = harness.service->getSnapshot();
    SP_REQUIRE_MSG (snapshot->tracks.empty(), "tracks from the removed location are still present");
    SP_REQUIRE_MSG (snapshot->locations.empty(), "location was not removed");

    harness.close();
    harness.open();
    SP_REQUIRE (harness.waitForIdle());
    snapshot = harness.service->getSnapshot();
    SP_REQUIRE_MSG (snapshot->tracks.empty(), "removed tracks came back after reopen");
    SP_REQUIRE_MSG (snapshot->locations.empty(), "removed location came back after reopen");

    std::cout << "[Library] removed location + tracks persisted across reopen\n";
}

//==============================================================================
SP_TEST (Library, HealthCheckHonest)
{
    LibraryHarness harness ("health");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());
    SP_REQUIRE_MSG (harness.addLocationAndScan (false), "scan failed");

    auto report = harness.service->runHealthCheck();
    pumpMessages (20);

    SP_REQUIRE_MSG (report.missingTracks.empty(), "fresh library reported missing tracks");
    SP_REQUIRE_MSG (report.brokenPlaylists.empty(), "no playlist tables exist, so none can be broken");
    SP_REQUIRE_MSG (report.generatedMs > 0, "health report timestamp missing");

    const auto track = harness.trackNamed ("04 - Pocket Groove.ogg");
    SP_REQUIRE (track.id != 0);
    SP_REQUIRE (juce::File (track.path).deleteFile());
    harness.service->refreshMissingState();
    pumpMessages (20);

    report = harness.service->runHealthCheck();
    pumpMessages (20);
    SP_REQUIRE_MSG (report.missingTracks.size() == 1, "health check did not report the missing file");
    SP_REQUIRE_MSG (report.missingTracks[0].id == track.id, "health reported the wrong missing track");

    harness.service->computeMissingHashes (0);
    pumpMessages (20);
    report = harness.service->runHealthCheck();
    pumpMessages (20);
    SP_REQUIRE_MSG (! report.duplicateGroups.empty(), "health check missed the duplicate pair");

    // Playlist health: use the same table names PlaylistManager creates and add
    // one entry referencing a track that does not exist.
    juce::String sqlError;
    SP_REQUIRE_MSG (harness.database->db().execute (
                        "CREATE TABLE IF NOT EXISTS playlists ("
                        "id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL)", &sqlError),
                    sqlError.toStdString());
    SP_REQUIRE_MSG (harness.database->db().execute (
                        "CREATE TABLE IF NOT EXISTS playlist_items ("
                        "playlist_id INTEGER NOT NULL, position INTEGER NOT NULL,"
                        "track_id INTEGER NOT NULL, PRIMARY KEY (playlist_id, position))", &sqlError),
                    sqlError.toStdString());
    SP_REQUIRE (harness.database->db().execute ("INSERT INTO playlists(name) VALUES('Broken Playlist')"));
    SP_REQUIRE (harness.database->db().execute ("INSERT INTO playlist_items VALUES(1, 0, 999999)"));

    report = harness.service->runHealthCheck();
    pumpMessages (20);
    SP_REQUIRE_MSG (report.brokenPlaylists.size() == 1,
                    "health check did not report the broken playlist");
    SP_REQUIRE_MSG (report.brokenPlaylists[0].name == "Broken Playlist",
                    "health check reported the wrong playlist");
    SP_REQUIRE_MSG (report.brokenPlaylists[0].missingEntries == 1,
                    "broken playlist missing-entry count was wrong");

    std::cout << "[Library] health: " << (int) report.missingTracks.size() << " missing, "
              << (int) report.brokenPlaylists.size() << " broken playlist(s), "
              << (int) report.duplicateGroups.size() << " duplicate group(s), "
              << (int) report.issues.size() << " issue(s)\n";
}

//==============================================================================
SP_TEST (Library, MetadataWritePath)
{
    LibraryHarness harness ("metadata");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());
    SP_REQUIRE_MSG (harness.addLocationAndScan (false), "scan failed");

    const auto support = detectTagSupport (harness);
    auto track = harness.trackNamed ("02 - Track Two.flac");
    SP_REQUIRE (track.id != 0);

    sp::Track updated = track;
    updated.title = "Library Test Title";
    updated.genre = "Test Genre";
    updated.rating = 5;

    harness.recorder.metadataFinished = false;
    harness.service->updateMetadata (updated);
    SP_REQUIRE_MSG (waitFor ([&harness] { return harness.recorder.metadataFinished; }, 15000),
                    "metadata write never reported completion");

    SP_REQUIRE_MSG (harness.recorder.metadataTrackId == track.id, "metadata callback used the wrong track id");

    if (support.write)
    {
        SP_REQUIRE_MSG (harness.recorder.metadataSuccess,
                        "tag write failed: " + harness.recorder.metadataError.toStdString());
        const auto after = harness.service->getTrack (track.id);
        SP_REQUIRE_MSG (after.title == "Library Test Title", "title was not updated");
        SP_REQUIRE_MSG (after.genre == "Test Genre", "genre was not updated");
        SP_REQUIRE_MSG (after.rating == 5, "rating was not written");

        sp::tags::TagData reread;
        juce::String error;
        if (sp::tags::readTags (juce::File (after.path), reread, error) && reread.readOk)
            SP_REQUIRE_MSG (reread.title == "Library Test Title", "tag file did not receive the new title");

        std::cout << "[Library] metadata write succeeded: title '" << after.title
                  << "', genre '" << after.genre << "'\n";
    }
    else
    {
        SP_REQUIRE_MSG (! harness.recorder.metadataSuccess,
                        "tag writer reported success but the capability probe failed");
        SP_REQUIRE_MSG (harness.recorder.metadataError.isNotEmpty(),
                        "tag writer failure was not explained to the listener");
        std::cout << "[Library] tag writer not available yet; failure reported honestly: "
                  << harness.recorder.metadataError << "\n";
    }
}

//==============================================================================
SP_TEST (Library, WatchDetectsNewFile)
{
    LibraryHarness harness ("watch");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());

    sp::LocationId locationId = 0;
    SP_REQUIRE_MSG (harness.addLocationAndScan (true, &locationId), "initial watched scan failed");

    const int before = (int) harness.service->getSnapshot()->tracks.size();
    const auto source = harness.fileInLibrary ("Album B/01 - Rhythm Section.wav");
    const auto added = harness.libraryRoot().getChildFile ("05 - Watched Copy.wav");
    SP_REQUIRE_MSG (source.copyFileTo (added), "could not add a file to the watched folder");

    const bool appeared = waitFor ([&harness, before]
    {
        return (int) harness.service->getSnapshot()->tracks.size() == before + 1;
    }, 20000);

    SP_REQUIRE_MSG (appeared, "the folder watcher did not pick up the new file within 20 s");
    SP_REQUIRE_MSG (harness.waitForIdle(), "watcher scan did not finish");
    SP_REQUIRE_MSG (harness.trackNamed ("05 - Watched Copy.wav").id != 0, "watched file was not indexed");

    // Removing it again should mark the row missing (not delete it).
    const auto track = harness.trackNamed ("05 - Watched Copy.wav");
    SP_REQUIRE (added.deleteFile());
    SP_REQUIRE (waitFor ([&harness, &track]
    {
        auto snapshot = harness.service->getSnapshot();
        const auto* found = snapshot->findTrack (track.id);
        return found != nullptr && found->missing;
    }, 20000));

    std::cout << "[Library] watcher picked up a new file in "
              << (before + 1) << " tracks and marked its deletion missing\n";
}

//==============================================================================
SP_TEST (Library, ErrorCallbacksAreHonest)
{
    LibraryHarness harness ("errors");
    SP_REQUIRE_MSG (harness.ok, harness.error.toStdString());

    harness.recorder.errors = 0;
    harness.recorder.lastError.clear();

    harness.service->addLocation (harness.copyRoot.getChildFile ("Does Not Exist"), false);
    pumpMessages (30);

    SP_REQUIRE_MSG (harness.recorder.errors > 0, "adding a missing folder did not report an error");
    SP_REQUIRE_MSG (harness.recorder.lastError.isNotEmpty(), "error callback had no message");

    // Importing a non-audio file is rejected with an explanation.
    const auto textFile = harness.copyRoot.getChildFile ("not-audio.txt");
    SP_REQUIRE (textFile.replaceWithText ("hello"));
    juce::StringArray files;
    files.add (textFile.getFullPathName());

    harness.recorder.errors = 0;
    harness.service->importFiles (files);
    pumpMessages (30);
    SP_REQUIRE_MSG (harness.recorder.errors > 0, "importing a text file did not report an error");

    std::cout << "[Library] errors reported: " << harness.recorder.lastError << "\n";
}
