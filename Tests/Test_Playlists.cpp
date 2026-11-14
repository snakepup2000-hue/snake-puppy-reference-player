/*
    Snake Puppy Reference Player
    Tests for the playlist manager: persistence/CRUD/ordering, smart playlists,
    playlist file parsing/writing, import resolution and M3U/M3U8/PLS export ->
    re-import behaviour.

    Suites: "Playlists" and "PlaylistIO" (both match --filter Playlist).
*/
#include "TestFramework.h"

#include "Core/ApplicationPaths.h"
#include "Core/Types.h"
#include "Library/LibraryDatabase.h"
#include "Library/LibraryService.h"
#include "Playlist/PlaylistIO.h"
#include "Playlist/PlaylistManager.h"
#include "Playlist/PlaylistResolver.h"
#include "Playlist/SmartPlaylist.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{

using namespace sp;

//==============================================================================
void print (const juce::String& text)
{
    std::cout << "        " << text.toStdString() << "\n";
}

juce::File sourceDirectory()
{
    return juce::File (juce::String (SP_SOURCE_DIR));
}

juce::File fixtureDirectory()
{
    return sourceDirectory().getChildFile ("Tests/Fixtures");
}

juce::File fixturePlaylist (const juce::String& name)
{
    return fixtureDirectory().getChildFile ("Playlists").getChildFile (name);
}

//==============================================================================
/** Creates a throw-away test root. Prefers /tmp because sandboxed test runners
    can write there; falls back to the platform temp directory. */
juce::File makeSandboxRoot (const juce::String& label)
{
    static std::atomic<int> sequence { 0 };
    const auto unique = juce::String (++sequence) + "-"
                        + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30));

    std::vector<juce::File> candidates;
    candidates.push_back (juce::File ("/tmp"));

    const auto environmentTemp = juce::SystemStats::getEnvironmentVariable ("TMPDIR", {});
    if (environmentTemp.isNotEmpty())
        candidates.push_back (juce::File (environmentTemp));

    candidates.push_back (juce::File::getSpecialLocation (juce::File::tempDirectory));

    for (const auto& candidate : candidates)
    {
        const auto root = candidate.getChildFile ("sp-playlist-tests").getChildFile (label + "-" + unique);
        if (root.createDirectory().wasOk())
            return root;
    }

    return candidates.front().getChildFile ("sp-playlist-tests").getChildFile (label + "-" + unique);
}

//==============================================================================
/** A sandboxed database + library + playlist manager, destroyed with the test. */
class TestEnvironment
{
public:
    explicit TestEnvironment (const juce::String& label)
    {
        root = makeSandboxRoot (label);

        paths = ApplicationPaths::forRoot (root);

        juce::String pathError;
        if (! paths.ensureDirectoriesExist (pathError))
            std::cerr << "TestEnvironment: " << pathError << "\n";

        const auto databaseError = database.open (paths.libraryDb);
        if (databaseError.isNotEmpty())
            std::cerr << "TestEnvironment: " << databaseError << "\n";

        const auto schemaError = database.initialiseSchema();
        if (schemaError.isNotEmpty())
            std::cerr << "TestEnvironment: schema: " << schemaError << "\n";

        library = std::make_unique<LibraryService> (paths, database);
        const auto libraryError = library->initialise();
        if (libraryError.isNotEmpty())
            std::cerr << "TestEnvironment: library: " << libraryError << "\n";

        createManager();
    }

    ~TestEnvironment()
    {
        manager.reset();
        library.reset();
        database.close();
        root.deleteRecursively();
    }

    void createManager()
    {
        manager = std::make_unique<PlaylistManager> (database, *library);

        const auto error = manager->initialise();
        if (error.isNotEmpty())
            std::cerr << "PlaylistManager::initialise: " << error << "\n";
    }

    void restartManager()
    {
        manager.reset();
        createManager();
    }

    PlaylistManager& playlists() { return *manager; }
    LibraryService& libraryService() { return *library; }
    LibraryDatabase& db() { return database; }
    const ApplicationPaths& appPaths() const { return paths; }
    juce::File dir() const { return root; }
    juce::File file (const juce::String& relativePath) const { return root.getChildFile (relativePath); }

    juce::File writeTextFile (const juce::String& relativePath, const juce::String& text)
    {
        auto f = file (relativePath);
        f.getParentDirectory().createDirectory();
        const bool ok = f.replaceWithText (text);
        if (! ok)
            std::cerr << "TestEnvironment: could not write " << f.getFullPathName() << "\n";
        return f;
    }

    juce::File writeBytes (const juce::String& relativePath, const void* data, int numBytes)
    {
        auto f = file (relativePath);
        f.getParentDirectory().createDirectory();

        juce::FileOutputStream stream (f);
        if (stream.openedOk())
            stream.write (data, (size_t) numBytes);

        return f;
    }

private:
    juce::File root;
    ApplicationPaths paths;
    LibraryDatabase database;
    std::unique_ptr<LibraryService> library;
    std::unique_ptr<PlaylistManager> manager;
};

//==============================================================================
std::vector<TrackId> snapshotTrackIds (PlaylistManager& manager, PlaylistId id)
{
    const auto snapshot = manager.getSnapshot();
    if (const auto* playlist = snapshot->find (id))
        return playlist->trackIds;

    return {};
}

const Playlist* snapshotPlaylist (PlaylistManager& manager, PlaylistId id)
{
    return manager.getSnapshot()->find (id);
}

std::vector<TrackId> databaseTrackIds (LibraryDatabase& database, PlaylistId id)
{
    std::vector<TrackId> ids;

    const juce::ScopedLock sl (database.lock());
    auto statement = database.db().prepare (
        "SELECT track_id FROM playlist_items WHERE playlist_id = ? ORDER BY position");

    if (statement == nullptr || ! statement->isValid())
        return ids;

    statement->bind (1, id);
    while (statement->step())
        ids.push_back (statement->getInt64 (0));

    return ids;
}

struct DatabasePlaylistRow
{
    PlaylistId id = kInvalidPlaylistId;
    juce::String name;
    bool smart = false;
};

std::vector<DatabasePlaylistRow> databasePlaylists (LibraryDatabase& database)
{
    std::vector<DatabasePlaylistRow> rows;

    const juce::ScopedLock sl (database.lock());
    auto statement = database.db().prepare (
        "SELECT id, name, smart FROM playlists ORDER BY name COLLATE NOCASE, id");

    if (statement == nullptr || ! statement->isValid())
        return rows;

    while (statement->step())
        rows.push_back ({ statement->getInt64 (0), statement->getString (1), statement->getInt (2) != 0 });

    return rows;
}

void requireSnapshotMatchesDatabase (PlaylistManager& manager, LibraryDatabase& database)
{
    const auto snapshot = manager.getSnapshot();
    const auto rows = databasePlaylists (database);

    SP_REQUIRE_MSG (snapshot->playlists.size() == rows.size(),
                    (juce::String ("snapshot playlists=") + juce::String ((int) snapshot->playlists.size())
                     + " database rows=" + juce::String ((int) rows.size())).toStdString());

    for (size_t i = 0; i < rows.size(); ++i)
    {
        SP_REQUIRE (snapshot->playlists[i].id == rows[i].id);
        SP_REQUIRE (snapshot->playlists[i].name == rows[i].name);
        SP_REQUIRE (snapshot->playlists[i].smart == rows[i].smart);
        SP_REQUIRE (snapshotTrackIds (manager, rows[i].id) == databaseTrackIds (database, rows[i].id));
    }
}

//==============================================================================
Track makeTrack (TrackId id, const juce::String& title, const juce::String& artist,
                 const juce::String& genre, int year, int rating, int sampleRate, int bitDepth,
                 std::int64_t durationMs, const juce::String& format, int playCount = 0,
                 const juce::String& dateAddedIso = {})
{
    Track track;
    track.id = id;
    track.title = title;
    track.artist = artist;
    track.albumArtist = artist;
    track.album = "Test Album";
    track.genre = genre;
    track.year = year;
    track.rating = rating;
    track.sampleRate = sampleRate;
    track.bitDepth = bitDepth;
    track.durationMs = durationMs;
    track.formatName = format;
    track.playCount = playCount;
    track.dateAddedIso = dateAddedIso;
    track.path = "/tmp/sp-smart-tracks/" + juce::File::createLegalFileName (artist + " - " + title)
                 + "." + format.toLowerCase();
    track.folder = juce::File (track.path).getParentDirectory().getFullPathName();
    return track;
}

SmartRule makeRule (SmartField field, SmartOp op, const juce::String& text = {},
                    double number = 0.0, double number2 = 0.0)
{
    SmartRule rule;
    rule.field = field;
    rule.op = op;
    rule.text = text;
    rule.number = number;
    rule.number2 = number2;
    return rule;
}

bool sameSmartDefinition (const SmartPlaylistDefinition& a, const SmartPlaylistDefinition& b)
{
    if (a.matchAll != b.matchAll || a.sortColumn != b.sortColumn
        || a.sortDescending != b.sortDescending || a.limit != b.limit
        || a.rules.size() != b.rules.size())
        return false;

    for (size_t i = 0; i < a.rules.size(); ++i)
    {
        const auto& ra = a.rules[i];
        const auto& rb = b.rules[i];
        if (ra.field != rb.field || ra.op != rb.op || ra.text != rb.text
            || std::abs (ra.number - rb.number) > 1.0e-9
            || std::abs (ra.number2 - rb.number2) > 1.0e-9
            || ra.list.size() != rb.list.size())
            return false;

        for (int j = 0; j < ra.list.size(); ++j)
            if (ra.list[j] != rb.list[j])
                return false;
    }

    return true;
}

//==============================================================================
std::vector<const Track*> filterTracks (const std::vector<Track>& tracks,
                                        const SmartPlaylistDefinition& definition)
{
    std::vector<const Track*> selected;
    smartplay::select (tracks, definition, selected);
    return selected;
}

std::vector<TrackId> selectedIds (const std::vector<Track>& tracks,
                                  const SmartPlaylistDefinition& definition)
{
    std::vector<TrackId> ids;
    for (const auto* track : filterTracks (tracks, definition))
        ids.push_back (track->id);
    return ids;
}

bool sameEntries (const std::vector<playlistio::Entry>& a, const std::vector<playlistio::Entry>& b,
                  bool compareTitles, bool compareLengths)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].path != b[i].path)
            return false;
        if (compareTitles && a[i].title != b[i].title)
            return false;
        if (compareLengths && std::abs (a[i].lengthSeconds - b[i].lengthSeconds) > 1.0e-9)
            return false;
    }

    return true;
}

} // namespace

//==============================================================================
// Core CRUD / persistence
//==============================================================================
SP_TEST (Playlists, CreateRenameDelete)
{
    TestEnvironment env ("crud");
    auto& manager = env.playlists();

    const auto original = manager.createPlaylist ("Evening");
    SP_REQUIRE (original != kInvalidPlaylistId);

    const auto* created = snapshotPlaylist (manager, original);
    SP_REQUIRE (created != nullptr);
    SP_REQUIRE_MSG (created->name == "Evening", created->name.toStdString());
    SP_REQUIRE (! created->smart);
    SP_REQUIRE (created->createdMs > 0);
    SP_REQUIRE (created->modifiedMs == created->createdMs);

    // Case-insensitive uniqueness.
    const auto duplicateName = manager.createPlaylist ("evening");
    SP_REQUIRE (duplicateName != kInvalidPlaylistId);
    SP_REQUIRE_MSG (snapshotPlaylist (manager, duplicateName)->name == "evening (2)",
                    snapshotPlaylist (manager, duplicateName)->name.toStdString());

    // Rename, including a case-insensitive collision with another playlist.
    SP_REQUIRE (manager.renamePlaylist (duplicateName, "Late Night"));
    SP_REQUIRE (snapshotPlaylist (manager, duplicateName)->name == "Late Night");

    SP_REQUIRE (manager.renamePlaylist (duplicateName, "EVENING"));
    SP_REQUIRE_MSG (snapshotPlaylist (manager, duplicateName)->name == "EVENING (2)",
                    snapshotPlaylist (manager, duplicateName)->name.toStdString());

    // Renaming to the current name is a no-op success.
    const auto modifiedBefore = snapshotPlaylist (manager, duplicateName)->modifiedMs;
    SP_REQUIRE (manager.renamePlaylist (duplicateName, "EVENING (2)"));
    SP_REQUIRE (snapshotPlaylist (manager, duplicateName)->modifiedMs == modifiedBefore);

    SP_REQUIRE (! manager.renamePlaylist (duplicateName, "   "));
    SP_REQUIRE (! manager.renamePlaylist (999999, "Ghost"));

    // Delete removes the row and frees the name.
    SP_REQUIRE (manager.deletePlaylist (original));
    SP_REQUIRE (snapshotPlaylist (manager, original) == nullptr);
    SP_REQUIRE (! manager.deletePlaylist (original));

    const auto reusedName = manager.createPlaylist ("Evening");
    SP_REQUIRE (reusedName != kInvalidPlaylistId);
    SP_REQUIRE (snapshotPlaylist (manager, reusedName)->name == "Evening");

    // Duplicate copies name, items and timestamps are new.
    const auto dup = manager.duplicatePlaylist (reusedName);
    SP_REQUIRE (dup != kInvalidPlaylistId);
    SP_REQUIRE_MSG (snapshotPlaylist (manager, dup)->name == "Evening copy",
                    snapshotPlaylist (manager, dup)->name.toStdString());

    requireSnapshotMatchesDatabase (manager, env.db());

    print ("playlists=" + juce::String ((int) manager.getSnapshot()->playlists.size())
           + " names=" + juce::String (manager.getSnapshot()->playlists[0].name) + "..");
}

SP_TEST (Playlists, AddMoveRemoveClearAndSnapshotOrder)
{
    TestEnvironment env ("editing");
    auto& manager = env.playlists();

    const auto id = manager.createPlaylist ("Order Test");
    SP_REQUIRE (id != kInvalidPlaylistId);

    const auto createdMs = snapshotPlaylist (manager, id)->createdMs;
    const auto modifiedBeforeAdd = snapshotPlaylist (manager, id)->modifiedMs;
    juce::Thread::sleep (3);

    std::vector<TrackId> initial { 10, 20, 30, 40, 50 };
    SP_REQUIRE (manager.addTracks (id, initial));

    SP_REQUIRE (snapshotPlaylist (manager, id)->createdMs == createdMs);
    SP_REQUIRE_MSG (snapshotPlaylist (manager, id)->modifiedMs > modifiedBeforeAdd,
                    (juce::String ("modified ") + juce::String ((juce::int64) snapshotPlaylist (manager, id)->modifiedMs)
                     + " vs " + juce::String ((juce::int64) modifiedBeforeAdd)).toStdString());

    SP_REQUIRE (snapshotTrackIds (manager, id) == initial);
    SP_REQUIRE (databaseTrackIds (env.db(), id) == initial);

    // Insert in the middle (duplicates are intentionally allowed).
    SP_REQUIRE (manager.addTracks (id, { 99 }, 1));
    std::vector<TrackId> expected { 10, 99, 20, 30, 40, 50 };
    SP_REQUIRE (snapshotTrackIds (manager, id) == expected);
    SP_REQUIRE (databaseTrackIds (env.db(), id) == expected);

    SP_REQUIRE (manager.addTracks (id, { 99, 99 }));
    SP_REQUIRE (snapshotTrackIds (manager, id).size() == expected.size() + 2);

    // Reorder: move the first item to index 3 of the resulting list.
    SP_REQUIRE (manager.moveTrack (id, 0, 3));
    std::vector<TrackId> afterMove { 99, 20, 30, 10, 40, 50, 99, 99 };
    SP_REQUIRE_MSG (snapshotTrackIds (manager, id) == afterMove,
                    (int) snapshotTrackIds (manager, id).size());
    SP_REQUIRE (databaseTrackIds (env.db(), id) == afterMove);

    // Remove.
    SP_REQUIRE (manager.removeTrackAt (id, 2));
    std::vector<TrackId> afterRemove { 99, 20, 10, 40, 50, 99, 99 };
    SP_REQUIRE (snapshotTrackIds (manager, id) == afterRemove);
    SP_REQUIRE (databaseTrackIds (env.db(), id) == afterRemove);

    // Invalid indices fail without changing anything.
    SP_REQUIRE (! manager.removeTrackAt (id, -1));
    SP_REQUIRE (! manager.removeTrackAt (id, 999));
    SP_REQUIRE (! manager.moveTrack (id, 0, 999));
    SP_REQUIRE (! manager.moveTrack (id, 999, 0));
    SP_REQUIRE (snapshotTrackIds (manager, id) == afterRemove);

    // Clear.
    SP_REQUIRE (manager.clearPlaylist (id));
    SP_REQUIRE (snapshotTrackIds (manager, id).empty());
    SP_REQUIRE (databaseTrackIds (env.db(), id).empty());
    SP_REQUIRE (manager.clearPlaylist (id));    // idempotent

    requireSnapshotMatchesDatabase (manager, env.db());

    print ("final track count=" + juce::String ((int) snapshotTrackIds (manager, id).size())
           + " db count=" + juce::String ((int) databaseTrackIds (env.db(), id).size()));
}

SP_TEST (Playlists, PersistenceAcrossRestart)
{
    TestEnvironment env ("persistence");
    auto& manager = env.playlists();

    const auto manual = manager.createPlaylist ("Zeta Manual");
    SP_REQUIRE (manual != kInvalidPlaylistId);
    SP_REQUIRE (manager.addTracks (manual, { 5, 3, 9, 1 }));
    SP_REQUIRE (manager.moveTrack (manual, 0, 2));
    SP_REQUIRE (manager.removeTrackAt (manual, 1));
    const auto manualIds = snapshotTrackIds (manager, manual);

    const auto smart = manager.createPlaylist ("Alpha Smart", true);
    SP_REQUIRE (smart != kInvalidPlaylistId);

    SmartPlaylistDefinition definition;
    definition.matchAll = false;
    definition.sortColumn = "year";
    definition.sortDescending = true;
    definition.limit = 25;
    definition.rules.push_back (makeRule (SmartField::genre, SmartOp::is, "Jazz"));
    definition.rules.push_back (makeRule (SmartField::year, SmartOp::between, {}, 1970, 1980));
    definition.rules[1].list.add ("x");     // exercise list round-trip too
    SP_REQUIRE (manager.setSmartDefinition (smart, definition));

    const auto beta = manager.createPlaylist ("beta Manual");
    SP_REQUIRE (manager.addTracks (beta, { 7 }));

    // Snapshot ordering is case-insensitive by name.
    const auto before = manager.getSnapshot();
    SP_REQUIRE_MSG (before->playlists.size() == 3, (int) before->playlists.size());
    SP_REQUIRE (before->playlists[0].id == smart);      // "Alpha Smart"
    SP_REQUIRE (before->playlists[1].id == beta);       // "beta Manual"
    SP_REQUIRE (before->playlists[2].id == manual);     // "Zeta Manual"

    const auto createdMs = snapshotPlaylist (manager, manual)->createdMs;
    const auto modifiedMs = snapshotPlaylist (manager, manual)->modifiedMs;

    // saveAll must be idempotent.
    SP_REQUIRE (manager.saveAll());

    // Simulate an application restart on the same database file.
    env.restartManager();
    auto& reopened = env.playlists();

    // initialise() is idempotent: a second call reloads the same state.
    SP_REQUIRE (reopened.initialise().isEmpty());
    SP_REQUIRE (reopened.getSnapshot()->playlists.size() == 3);

    const auto after = reopened.getSnapshot();
    SP_REQUIRE_MSG (after->playlists.size() == 3, (int) after->playlists.size());
    SP_REQUIRE (after->playlists[0].id == smart);
    SP_REQUIRE (after->playlists[1].id == beta);
    SP_REQUIRE (after->playlists[2].id == manual);

    const auto* reopenedManual = snapshotPlaylist (reopened, manual);
    SP_REQUIRE (reopenedManual != nullptr);
    SP_REQUIRE_MSG (reopenedManual->name == "Zeta Manual", reopenedManual->name.toStdString());
    SP_REQUIRE (reopenedManual->trackIds == manualIds);
    SP_REQUIRE (reopenedManual->createdMs == createdMs);
    SP_REQUIRE (reopenedManual->modifiedMs == modifiedMs);

    const auto* reopenedSmart = snapshotPlaylist (reopened, smart);
    SP_REQUIRE (reopenedSmart != nullptr);
    SP_REQUIRE (reopenedSmart->smart);
    SP_REQUIRE_MSG (sameSmartDefinition (reopenedSmart->smartDef, definition),
                    (int) reopenedSmart->smartDef.rules.size());

    SP_REQUIRE (databaseTrackIds (env.db(), manual) == manualIds);
    requireSnapshotMatchesDatabase (reopened, env.db());

    print ("persisted playlists=" + juce::String ((int) after->playlists.size())
           + " manualIds=" + juce::String ((int) manualIds.size())
           + " smartRules=" + juce::String ((int) reopenedSmart->smartDef.rules.size()));
}

SP_TEST (Playlists, DuplicateCopiesSmartDefinitionAndItems)
{
    TestEnvironment env ("duplicate");
    auto& manager = env.playlists();

    const auto manual = manager.createPlaylist ("Mix");
    SP_REQUIRE (manager.addTracks (manual, { 4, 2, 8 }));

    const auto manualCopy = manager.duplicatePlaylist (manual);
    SP_REQUIRE (manualCopy != kInvalidPlaylistId);
    SP_REQUIRE (snapshotTrackIds (manager, manualCopy) == snapshotTrackIds (manager, manual));
    SP_REQUIRE_MSG (snapshotPlaylist (manager, manualCopy)->name == "Mix copy",
                    snapshotPlaylist (manager, manualCopy)->name.toStdString());

    const auto smart = manager.createPlaylist ("Smart", true);
    SmartPlaylistDefinition definition;
    definition.matchAll = true;
    definition.sortColumn = "rating";
    definition.sortDescending = true;
    definition.limit = 10;
    definition.rules.push_back (makeRule (SmartField::rating, SmartOp::greaterThan, {}, 2));
    SP_REQUIRE (manager.setSmartDefinition (smart, definition));

    const auto smartCopy = manager.duplicatePlaylist (smart);
    SP_REQUIRE (smartCopy != kInvalidPlaylistId);
    const auto* copied = snapshotPlaylist (manager, smartCopy);
    SP_REQUIRE (copied != nullptr);
    SP_REQUIRE (copied->smart);
    SP_REQUIRE (sameSmartDefinition (copied->smartDef, definition));

    SP_REQUIRE (! manager.addTracks (smart, { 1 }));     // smart playlists reject manual editing
    SP_REQUIRE (! manager.clearPlaylist (smart));
    requireSnapshotMatchesDatabase (manager, env.db());

    print ("duplicates=" + juce::String ((int) manager.getSnapshot()->playlists.size())
           + " smartCopyRules=" + juce::String ((int) copied->smartDef.rules.size()));
}

//==============================================================================
// Smart playlists
//==============================================================================
SP_TEST (Playlists, SmartRuleMatching)
{
    const std::vector<Track> library
    {
        makeTrack (1, "So What", "Miles Davis", "Jazz", 1959, 5, 96000, 24, 545000, "FLAC",
                   12, "2024-01-02T03:04:05Z"),
        makeTrack (2, "Birdland", "Weather Report", "Jazz", 1977, 4, 44100, 16, 340000, "MP3",
                   3, "2024-02-02T03:04:05Z"),
        makeTrack (3, "Superstition", "Stevie Wonder", "Soul", 1972, 3, 48000, 24, 245000, "FLAC",
                   0, "2024-03-02T03:04:05Z"),
        makeTrack (4, "Unknown Year", "Test", "Funk", 0, 0, 0, 0, 0, ""),
        makeTrack (5, "Late", "", "", 2020, 1, 96000, 24, 120000, "ALAC",
                   1, "2025-01-02T03:04:05Z"),
    };

    auto expect = [&library] (const SmartPlaylistDefinition& definition, std::vector<TrackId> expected,
                              const char* label)
    {
        auto actual = selectedIds (library, definition);
        std::sort (actual.begin(), actual.end());
        std::sort (expected.begin(), expected.end());

        if (actual != expected)
        {
            std::cerr << "        smart rule failed: " << label << " expected "
                      << expected.size() << " got " << actual.size() << "\n";
        }
        SP_REQUIRE_MSG (actual == expected, juce::String (label).toStdString());
    };

    SmartPlaylistDefinition def;

    def.rules = { makeRule (SmartField::genre, SmartOp::is, "jazz") };
    expect (def, { 1, 2 }, "genre is jazz (case-insensitive)");

    def.rules = { makeRule (SmartField::genre, SmartOp::isNot, "Jazz") };
    expect (def, { 3, 4, 5 }, "genre isNot Jazz");

    def.rules = { makeRule (SmartField::genre, SmartOp::contains, "oul") };
    expect (def, { 3 }, "genre contains oul");

    def.rules = { makeRule (SmartField::genre, SmartOp::notContains, "oul") };
    expect (def, { 1, 2, 4, 5 }, "genre notContains oul");

    def.rules = { makeRule (SmartField::artist, SmartOp::beginsWith, "miles") };
    expect (def, { 1 }, "artist beginsWith miles");

    def.rules = { makeRule (SmartField::artist, SmartOp::endsWith, "report") };
    expect (def, { 2 }, "artist endsWith report");

    def.rules = { makeRule (SmartField::year, SmartOp::between, {}, 1970, 1980) };
    expect (def, { 2, 3 }, "year between 1970 and 1980 (inclusive)");

    // Between tolerates reversed bounds.
    def.rules = { makeRule (SmartField::year, SmartOp::between, {}, 1980, 1970) };
    expect (def, { 2, 3 }, "year between reversed bounds");

    def.rules = { makeRule (SmartField::sampleRate, SmartOp::greaterThan, {}, 48000) };
    expect (def, { 1, 5 }, "sampleRate > 48000");

    def.rules = { makeRule (SmartField::rating, SmartOp::greaterThan, {}, 3) };
    expect (def, { 1, 2 }, "rating > 3");

    def.rules = { makeRule (SmartField::rating, SmartOp::is, {}, 5) };
    expect (def, { 1 }, "rating is 5");

    def.rules = { makeRule (SmartField::format, SmartOp::is, "flac") };
    expect (def, { 1, 3 }, "format is flac");

    def.rules = { makeRule (SmartField::duration, SmartOp::lessThan, {}, 300) };
    expect (def, { 3, 5 }, "duration < 300 seconds");

    def.rules = { makeRule (SmartField::playCount, SmartOp::greaterThan, {}, 2) };
    expect (def, { 1, 2 }, "playCount > 2");

    def.rules = { makeRule (SmartField::dateAdded, SmartOp::greaterThan, {}, 0) };
    const auto datedTracks = selectedIds (library, def);
    SP_REQUIRE (datedTracks.size() == 4);    // track 4 has no date

    SmartRule listRule = makeRule (SmartField::genre, SmartOp::inList);
    listRule.list = { "jazz", "SOUL" };
    def.rules = { listRule };
    expect (def, { 1, 2, 3 }, "genre inList {Jazz, Soul}");

    // Numeric field with a text operator, and string field with a numeric op.
    def.rules = { makeRule (SmartField::sampleRate, SmartOp::contains, "48000") };
    expect (def, { 3 }, "sampleRate contains 48000");

    // matchAll = false means OR.
    def.matchAll = false;
    def.rules = { makeRule (SmartField::genre, SmartOp::is, "Jazz"),
                  makeRule (SmartField::year, SmartOp::between, {}, 1970, 1980) };
    expect (def, { 1, 2, 3 }, "matchAny (Jazz OR 1970-1980)");

    // Missing numeric metadata never satisfies a numeric comparison.
    def.matchAll = true;
    def.rules = { makeRule (SmartField::year, SmartOp::lessThan, {}, 1970) };
    expect (def, { 1 }, "year < 1970 excludes unknown years");

    // Empty rule lists: matchAll => everything, matchAny => nothing.
    def.rules.clear();
    def.matchAll = true;
    expect (def, { 1, 2, 3, 4, 5 }, "empty rules + matchAll");

    def.matchAll = false;
    expect (def, {}, "empty rules + matchAny");

    print ("smart rule cases passed on " + juce::String ((int) library.size()) + " synthetic tracks");
}

SP_TEST (Playlists, SmartSortAndLimit)
{
    const std::vector<Track> library
    {
        makeTrack (1, "B Title", "Miles Davis", "Jazz", 1959, 5, 96000, 24, 545000, "FLAC"),
        makeTrack (2, "A Title", "Weather Report", "Jazz", 1977, 4, 44100, 16, 340000, "MP3"),
        makeTrack (3, "C Title", "Stevie Wonder", "Soul", 1972, 3, 48000, 24, 245000, "FLAC"),
        makeTrack (4, "D Title", "", "Funk", 0, 0, 0, 0, 0, ""),
        makeTrack (5, "E Title", "A Artist", "", 2020, 1, 96000, 24, 120000, "ALAC"),
    };

    SmartPlaylistDefinition def;
    def.rules = { makeRule (SmartField::genre, SmartOp::isNot, "Funk") };   // 1,2,3,5

    auto idsText = [] (const std::vector<TrackId>& ids)
    {
        juce::String text;

        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (i > 0)
                text << ",";
            text << juce::String (ids[i]);
        }

        return text;
    };

    auto expectOrder = [&library, &def, &idsText] (std::vector<TrackId> expected)
    {
        const auto actual = selectedIds (library, def);
        SP_REQUIRE_MSG (actual == expected,
                        (juce::String ("expected [") + idsText (expected)
                         + "] got [" + idsText (actual) + "]").toStdString());
    };

    def.sortColumn = "artist";
    def.sortDescending = false;
    // Empty artist sorts last: A Artist, Miles Davis, Stevie Wonder, Weather Report.
    expectOrder ({ 5, 1, 3, 2 });

    def.sortDescending = true;
    // Empty artist still sorts last: Weather Report, Stevie Wonder, Miles Davis, A Artist.
    expectOrder ({ 2, 3, 1, 5 });

    def.sortColumn = "year";
    def.sortDescending = false;
    // Years 1959, 1972, 1977, 2020.
    expectOrder ({ 1, 3, 2, 5 });

    def.sortColumn = "year";
    def.sortDescending = true;
    expectOrder ({ 5, 2, 3, 1 });

    def.sortColumn = "not-a-real-field";
    def.sortDescending = false;
    expectOrder ({ 5, 1, 3, 2 });   // falls back to artist

    def.sortColumn = "artist";
    def.limit = 2;
    SP_REQUIRE (selectedIds (library, def).size() == 2);

    def.limit = 0;
    def.rules = { makeRule (SmartField::genre, SmartOp::is, "Jazz") };
    def.sortColumn = "rating";
    def.sortDescending = true;
    expectOrder ({ 1, 2 });

    // applySmartDefinition is the same evaluation the manager uses.
    const auto applied = PlaylistManager::applySmartDefinition (library, def);
    SP_REQUIRE (applied.size() == 2);
    SP_REQUIRE (applied[0].id == 1 && applied[1].id == 2);

    // Direct static rule matching.
    SP_REQUIRE (PlaylistManager::trackMatchesRules (library[0], def));
    SP_REQUIRE (! PlaylistManager::trackMatchesRules (library[2], def));

    print ("smart sort/limit cases passed; applied=" + juce::String ((int) applied.size()));
}

SP_TEST (Playlists, SmartPlaylistResolutionAgainstLibrarySnapshot)
{
    TestEnvironment env ("smart-resolution");
    auto& manager = env.playlists();

    const auto smart = manager.createPlaylist ("Jazz Only", true);
    SP_REQUIRE (smart != kInvalidPlaylistId);

    SmartPlaylistDefinition definition;
    definition.matchAll = true;
    definition.sortColumn = "artist";
    definition.rules.push_back (makeRule (SmartField::genre, SmartOp::is, "Jazz"));
    SP_REQUIRE (manager.setSmartDefinition (smart, definition));

    // The library is empty in this sandbox (no scan is ever requested): the smart
    // playlist must resolve to nothing, must not crash and must not trigger a scan.
    SP_REQUIRE (! env.libraryService().isScanning());
    SP_REQUIRE (manager.resolveTrackIds (smart).empty());
    SP_REQUIRE (manager.resolveTracks (smart).empty());
    SP_REQUIRE (! env.libraryService().isScanning());

    // A manual playlist pointing at ids that do not exist resolves gracefully.
    const auto manual = manager.createPlaylist ("Missing Tracks");
    SP_REQUIRE (manager.addTracks (manual, { 424242, 777 }));
    SP_REQUIRE (manager.resolveTrackIds (manual).size() == 2);
    SP_REQUIRE (manager.resolveTracks (manual).empty());
    SP_REQUIRE (! env.libraryService().isScanning());

    SP_REQUIRE (! manager.setSmartDefinition (manual, definition));    // manual playlist rejects rules

    print ("smart resolve with empty library: 0 tracks, no scan triggered");
}

//==============================================================================
// Playlist file codec
//==============================================================================
SP_TEST (PlaylistIO, CodecRoundTrip)
{
    TestEnvironment env ("codec");
    const std::vector<playlistio::Entry> entries
    {
        { "/tmp/music/Artist - One.flac", "Artist - One", 123.0 },
        { "relative/Two.mp3", "Two", -1.0 },
        { "/tmp/music with spaces/Three \xC3\xA9.wav", "Bj\xC3\xB6rk - Three", -1.0 },
    };

    // M3U8: #EXTM3U/#EXTINF with UTF-8 titles round-trips.
    const auto m3u8File = env.writeTextFile ("codec/mix.m3u8", playlistio::serialiseM3U8 (entries));
    playlistio::Document parsed;
    juce::String error;
    SP_REQUIRE_MSG (playlistio::parse (m3u8File, parsed, error), error.toStdString());
    SP_REQUIRE (parsed.format == playlistio::Format::m3u8);
    SP_REQUIRE_MSG (sameEntries (parsed.entries, entries, true, true),
                    (juce::String ("M3U8 roundtrip mismatch; entries=")
                     + juce::String ((int) parsed.entries.size())).toStdString());

    // M3U basic: paths round-trip, titles/lengths intentionally absent.
    const auto m3uFile = env.writeTextFile ("codec/mix.m3u", playlistio::serialiseM3UBasic (entries));
    SP_REQUIRE_MSG (playlistio::parse (m3uFile, parsed, error), error.toStdString());
    SP_REQUIRE (parsed.format == playlistio::Format::m3u);
    bool basicPathsEqual = parsed.entries.size() == entries.size();
    for (size_t i = 0; basicPathsEqual && i < entries.size(); ++i)
        basicPathsEqual = parsed.entries[i].path == entries[i].path;
    SP_REQUIRE (basicPathsEqual);
    SP_REQUIRE (parsed.entries[0].lengthSeconds < 0.0);

    // PLS: paths, titles and lengths round-trip.
    const auto plsFile = env.writeTextFile ("codec/mix.pls", playlistio::serialisePLS (entries));
    SP_REQUIRE_MSG (playlistio::parse (plsFile, parsed, error), error.toStdString());
    SP_REQUIRE (parsed.format == playlistio::Format::pls);
    SP_REQUIRE_MSG (sameEntries (parsed.entries, entries, true, true),
                    (juce::String ("PLS roundtrip mismatch; entries=")
                     + juce::String ((int) parsed.entries.size())).toStdString());

    print ("codec roundtrip equality: m3u8=" + juce::String ((int) entries.size())
           + " m3u=" + juce::String ((int) parsed.entries.size())
           + " pls=" + juce::String ((int) parsed.entries.size()) + " entries");
}

SP_TEST (PlaylistIO, RobustParsing)
{
    TestEnvironment env ("robust");

    // UTF-8 BOM + CRLF + comments + blank lines + #PLAYLIST + relative paths.
    juce::String m3uText;
    m3uText << "\xEF\xBB\xBF#EXTM3U\r\n"
            << "# a comment\r\n"
            << "\r\n"
            << "#PLAYLIST:Road Trip\r\n"
            << "#EXTINF:2,Artist - One\r\n"
            << "music/one.flac\r\n"
            << "#EXTINF:-1,Two\r\n"
            << "music/two.mp3\r\n"
            << "\r\n"
            << "#EXTINF:12.5,Three\r\n"
            << "music/three.wav\r\n";

    const auto m3u8 = env.writeTextFile ("robust/bom.m3u8", m3uText);
    playlistio::Document doc;
    juce::String error;
    SP_REQUIRE_MSG (playlistio::parse (m3u8, doc, error), error.toStdString());
    SP_REQUIRE (doc.title == "Road Trip");
    SP_REQUIRE_MSG (doc.entries.size() == 3, (int) doc.entries.size());
    SP_REQUIRE (doc.entries[0].path == "music/one.flac");
    SP_REQUIRE (doc.entries[0].title == "Artist - One");
    SP_REQUIRE_NEAR ((double) doc.entries[0].lengthSeconds, 2.0, 1.0e-9);
    SP_REQUIRE (doc.entries[2].lengthSeconds > 12.4 && doc.entries[2].lengthSeconds < 12.6);

    // UTF-16 LE with BOM.
    const juce::String utf16Text = "#EXTM3U\n#EXTINF:1,Unicode \xC3\xA9\nfolder/song.flac\n";
    const auto utf8Bytes = utf16Text.toRawUTF8();
    const auto utf8Size = (int) utf16Text.getNumBytesAsUTF8();
    std::vector<juce::uint8> utf16Bytes;
    utf16Bytes.push_back (0xFF);
    utf16Bytes.push_back (0xFE);
    for (int i = 0; i < utf8Size; ++i)
    {
        // ASCII-only test text, so one byte maps to one UTF-16 code unit.
        utf16Bytes.push_back ((juce::uint8) utf8Bytes[i]);
        utf16Bytes.push_back (0);
    }

    const auto utf16File = env.writeBytes ("robust/utf16.m3u8", utf16Bytes.data(), (int) utf16Bytes.size());
    SP_REQUIRE_MSG (playlistio::parse (utf16File, doc, error), error.toStdString());
    SP_REQUIRE (doc.entries.size() == 1);
    SP_REQUIRE (doc.entries[0].path == "folder/song.flac");

    // PLS: BOM, CRLF, mixed-case keys, out-of-order N.
    juce::String plsText;
    plsText << "\xEF\xBB\xBF[playlist]\r\n"
            << "NumberOfEntries=2\r\n"
            << "Title2=Second\r\n"
            << "file2=/music/two.mp3\r\n"
            << "Length2=42\r\n"
            << "FILE1=/music/one.flac\r\n"
            << "title1=First\r\n"
            << "length1=-1\r\n"
            << "Version=2\r\n";
    const auto pls = env.writeTextFile ("robust/order.pls", plsText);
    SP_REQUIRE_MSG (playlistio::parse (pls, doc, error), error.toStdString());
    SP_REQUIRE (doc.entries.size() == 2);
    SP_REQUIRE (doc.entries[0].path == "/music/one.flac");
    SP_REQUIRE (doc.entries[0].title == "First");
    SP_REQUIRE (doc.entries[0].lengthSeconds < 0.0);
    SP_REQUIRE (doc.entries[1].path == "/music/two.mp3");
    SP_REQUIRE (doc.entries[1].title == "Second");
    SP_REQUIRE_NEAR ((double) doc.entries[1].lengthSeconds, 42.0, 1.0e-9);

    // Empty / missing / unrecognised files are hard failures with clear errors.
    const auto empty = env.writeTextFile ("robust/empty.m3u8", "#EXTM3U\n");
    SP_REQUIRE (playlistio::parse (empty, doc, error));
    SP_REQUIRE (doc.entries.empty());

    SP_REQUIRE (! playlistio::parse (env.file ("robust/missing.m3u8"), doc, error));
    SP_REQUIRE (error.isNotEmpty());

    const auto nonsense = env.writeTextFile ("robust/nonsense.xyz", "this is not a playlist\n");
    SP_REQUIRE (! playlistio::parse (nonsense, doc, error));
    SP_REQUIRE (error.containsIgnoreCase ("unrecognised"));

    print ("robust parsing: BOM/CRLF/comments/UTF-16/PLS-key-casing verified");
}

//==============================================================================
// Entry resolution against an explicit library snapshot
//==============================================================================
SP_TEST (PlaylistIO, ResolverAgainstSyntheticLibrary)
{
    TestEnvironment env ("resolver");

    const auto albumDir = env.file ("library/Album");
    const auto otherDir = env.file ("library/Other");
    albumDir.createDirectory();
    otherDir.createDirectory();

    const auto one = env.writeTextFile ("library/Album/01 - One.flac", "dummy");
    const auto two = env.writeTextFile ("library/Album/02 - Two.flac", "dummy");
    const auto dupOne = env.writeTextFile ("library/Other/dup.flac", "dummy");
    const auto dupTwo = env.writeTextFile ("library/Other/Sub/dup.flac", "dummy");

    LibrarySnapshot snapshot;
    auto libTrack = [] (TrackId id, const juce::File& file, std::int64_t durationMs)
    {
        Track track;
        track.id = id;
        track.path = file.getFullPathName();
        track.title = file.getFileNameWithoutExtension();
        track.durationMs = durationMs;
        return track;
    };

    snapshot.tracks.push_back (libTrack (11, one, 2000));
    snapshot.tracks.push_back (libTrack (12, two, 3000));
    snapshot.tracks.push_back (libTrack (13, dupOne, 1000));
    snapshot.tracks.push_back (libTrack (14, dupTwo, 2000));

    const PlaylistPathResolver resolver (snapshot);
    const auto playlistDirectory = env.dir();

    // 1. Absolute path.
    auto match = resolver.resolve (one.getFullPathName(), -1.0, playlistDirectory);
    SP_REQUIRE_MSG (match.resolved && match.trackId == 11,
                    (match.reason.isNotEmpty() ? match.reason.toStdString() : std::string ("wrong id")));

    // 2. Relative path resolved against the playlist's directory.
    match = resolver.resolve ("library/Album/02 - Two.flac", -1.0, playlistDirectory);
    SP_REQUIRE_MSG (match.resolved && match.trackId == 12,
                    match.reason.toStdString());

    // 3. Symlink-insensitive normalisation (macOS /var -> /private/var).
    const auto normalised = PlaylistPathResolver::normalisePath (one.getFullPathName());
    const auto viaPrivate = PlaylistPathResolver::normalisePath (juce::String ("/private")
                                                                  + one.getFullPathName());
    SP_REQUIRE_MSG (normalised == viaPrivate,
                    (juce::String ("normalise mismatch: ") + normalised + " vs " + viaPrivate).toStdString());

    // 4. Path traversal / redundant components.
    match = resolver.resolve (one.getFullPathName() + "/../Album/01 - One.flac", -1.0, playlistDirectory);
    SP_REQUIRE_MSG (match.resolved && match.trackId == 11, match.reason.toStdString());

    // 5. file:// URI.
    match = resolver.resolve ("file://" + one.getFullPathName(), -1.0, playlistDirectory);
    SP_REQUIRE_MSG (match.resolved && match.trackId == 11, match.reason.toStdString());

    // 6. Unique filename fallback (path from another machine).
    match = resolver.resolve ("/somewhere/else/02 - Two.flac", -1.0, playlistDirectory);
    SP_REQUIRE_MSG (match.resolved && match.trackId == 12, match.reason.toStdString());

    // 7. Filename + duration disambiguates duplicate filenames.
    match = resolver.resolve ("/somewhere/else/dup.flac", 2.0, playlistDirectory);
    SP_REQUIRE_MSG (match.resolved && match.trackId == 14, match.reason.toStdString());
    match = resolver.resolve ("/somewhere/else/dup.flac", 1.0, playlistDirectory);
    SP_REQUIRE_MSG (match.resolved && match.trackId == 13, match.reason.toStdString());

    // 8. Ambiguous duplicates without a duration hint stay unresolved with a reason.
    match = resolver.resolve ("/somewhere/else/dup.flac", -1.0, playlistDirectory);
    SP_REQUIRE (! match.resolved);
    SP_REQUIRE_MSG (match.reason.containsIgnoreCase ("ambiguous"), match.reason.toStdString());

    // 9. Missing entry / network URL / Windows path.
    match = resolver.resolve (env.file ("nope/none.flac").getFullPathName(), -1.0, playlistDirectory);
    SP_REQUIRE (! match.resolved);
    SP_REQUIRE_MSG (match.reason.containsIgnoreCase ("not found"), match.reason.toStdString());

    match = resolver.resolve ("https://example.com/stream.mp3", -1.0, playlistDirectory);
    SP_REQUIRE (! match.resolved);
    SP_REQUIRE_MSG (match.reason.containsIgnoreCase ("network"), match.reason.toStdString());

    match = resolver.resolve ("C:\\Music\\song.flac", -1.0, playlistDirectory);
    SP_REQUIRE (! match.resolved);
    SP_REQUIRE_MSG (match.reason.containsIgnoreCase ("Windows"), match.reason.toStdString());

    print ("resolver: 4 tracks, absolute/relative/normalised/filename/duration/ambiguous cases verified");
}

//==============================================================================
// Manager-level import
//==============================================================================
SP_TEST (Playlists, ImportFixturePlaylistsReportsCounts)
{
    TestEnvironment env ("import");
    auto& manager = env.playlists();

    PlaylistImportResult result;
    juce::String error;
    SP_REQUIRE_MSG (manager.importPlaylistFile (fixturePlaylist ("fixture.m3u8"), error, nullptr, &result),
                    error.toStdString());

    SP_REQUIRE_MSG (result.entriesRead == 3, result.entriesRead);
    SP_REQUIRE (result.format == "M3U8");
    SP_REQUIRE_MSG (result.resolved + result.unresolved == result.entriesRead,
                    (juce::String ("resolved=") + juce::String (result.resolved)
                     + " unresolved=" + juce::String (result.unresolved)).toStdString());
    // No library locations were added in this sandbox, so nothing can resolve;
    // the important behaviour is that every entry is reported, not dropped.
    SP_REQUIRE (result.resolved == 0);
    SP_REQUIRE (result.unresolved == 3);
    SP_REQUIRE (result.unresolvedEntries.size() == 3);
    SP_REQUIRE (result.unresolvedReasons.size() == 3);
    SP_REQUIRE (result.playlistId != kInvalidPlaylistId);
    SP_REQUIRE (result.playlistName == "fixture");
    SP_REQUIRE_MSG (result.message().containsIgnoreCase ("0 of 3"), result.message().toStdString());

    const auto* imported = snapshotPlaylist (manager, result.playlistId);
    SP_REQUIRE (imported != nullptr);
    SP_REQUIRE (imported->name == "fixture");
    SP_REQUIRE (imported->trackIds.empty());

    // PLS fixture: two entries.
    PlaylistImportResult plsResult;
    SP_REQUIRE_MSG (manager.importPlaylistFile (fixturePlaylist ("fixture.pls"), error, nullptr, &plsResult),
                    error.toStdString());
    SP_REQUIRE_MSG (plsResult.entriesRead == 2, plsResult.entriesRead);
    SP_REQUIRE (plsResult.format == "PLS");

    print ("import m3u8: " + result.message());
    print ("import pls: " + plsResult.message());
}

SP_TEST (Playlists, ImportRelativePathsEmbeddedTitleAndUniqueNames)
{
    TestEnvironment env ("import-synthetic");
    auto& manager = env.playlists();

    // Create dummy files so the resolver would find them if a library existed.
    env.writeTextFile ("music/one.flac", "dummy");
    env.writeTextFile ("music/two.mp3", "dummy");

    juce::String text;
    text << "#EXTM3U\n"
         << "#PLAYLIST:My Mix\n"
         << "#EXTINF:1,One\n"
         << "music/one.flac\n"
         << "#EXTINF:2,Two\n"
         << "music/two.mp3\n"
         << "https://example.com/stream.mp3\n";

    const auto file = env.writeTextFile ("playlists/My Mix.m3u8", text);

    PlaylistImportResult result;
    juce::String error;
    SP_REQUIRE_MSG (manager.importPlaylistFile (file, error, nullptr, &result), error.toStdString());
    SP_REQUIRE (result.playlistName == "My Mix");
    SP_REQUIRE (result.entriesRead == 3);
    SP_REQUIRE (result.unresolvedEntries.size() == 3);
    SP_REQUIRE (result.unresolvedEntries[0] == "music/one.flac");   // relative path reported verbatim
    SP_REQUIRE (result.unresolvedReasons[2].containsIgnoreCase ("network"));

    // Importing the same file twice creates a uniquely named copy.
    PlaylistImportResult second;
    SP_REQUIRE (manager.importPlaylistFile (file, error, nullptr, &second));
    SP_REQUIRE_MSG (second.playlistName == "My Mix (2)", second.playlistName.toStdString());

    requireSnapshotMatchesDatabase (manager, env.db());

    print ("relative import: entries=" + juce::String (result.entriesRead)
           + " unresolved=" + juce::String (result.unresolved)
           + " names=" + result.playlistName + "," + second.playlistName);
}

SP_TEST (Playlists, ImportAndExportHardFailures)
{
    TestEnvironment env ("failures");
    auto& manager = env.playlists();

    juce::String error;
    PlaylistImportResult result;

    SP_REQUIRE (! manager.importPlaylistFile (env.file ("missing.m3u8"), error, nullptr, &result));
    SP_REQUIRE_MSG (error.containsIgnoreCase ("does not exist"), error.toStdString());

    const auto empty = env.writeTextFile ("empty.m3u8", "#EXTM3U\n");
    SP_REQUIRE (! manager.importPlaylistFile (empty, error, nullptr, &result));
    SP_REQUIRE_MSG (error.containsIgnoreCase ("no entries"), error.toStdString());

    const auto nonsense = env.writeTextFile ("notes.txt", "just some text\n");
    SP_REQUIRE (! manager.importPlaylistFile (nonsense, error, nullptr, &result));
    SP_REQUIRE (error.isNotEmpty());

    const auto id = manager.createPlaylist ("Export Me");
    SP_REQUIRE (manager.addTracks (id, { 123456, 234567 }));

    juce::File outFile = env.file ("out/mix.m3u8");
    SP_REQUIRE (! manager.exportPlaylist (id, outFile, error));   // tracks not in library
    SP_REQUIRE_MSG (error.containsIgnoreCase ("no tracks"), error.toStdString());

    SP_REQUIRE (! manager.exportPlaylist (999999, env.file ("out/x.m3u8"), error));
    SP_REQUIRE_MSG (error.containsIgnoreCase ("not found"), error.toStdString());

    juce::File badExtension = env.file ("out/mix.txt");
    SP_REQUIRE (! manager.exportPlaylist (id, badExtension, error));
    SP_REQUIRE_MSG (error.containsIgnoreCase ("unsupported"), error.toStdString());

    print ("hard failure paths verified (missing/empty/unsupported/nothing-to-export)");
}

//==============================================================================
// Full export -> import roundtrip. The library scanner is owned by another
// teammate; when it is not available yet this test reports the skip instead of
// pretending the roundtrip happened. When it is available it is a real
// end-to-end check of the playlist files against the library snapshot.
//==============================================================================
SP_TEST (Playlists, ExportImportRoundTripWhenLibraryReady)
{
    TestEnvironment env ("roundtrip");
    auto& manager = env.playlists();

    // Build a tiny library of our own (a copy of three fixture tracks) so the
    // test exercises the real scanner, real track ids and relative-path export
    // without touching the repository fixtures. The scanner is owned by another
    // teammate: if it is cold/slow or only partially available we use whatever
    // tracks it published and skip the data-dependent assertions that follow.
    const auto fixtureLibrary = fixtureDirectory().getChildFile ("Library");
    const auto musicDirectory = env.dir().getChildFile ("musiccopy");

    const char* filesToCopy[]
    {
        "Album A/01 - Track One.flac",
        "Album A/02 - Track Two.flac",
        "Album B/01 - Rhythm Section.wav",
    };

    for (const auto* relativePath : filesToCopy)
    {
        const auto source = fixtureLibrary.getChildFile (relativePath);
        const auto target = musicDirectory.getChildFile (relativePath);
        target.getParentDirectory().createDirectory();

        if (! source.copyFileTo (target) && ! target.existsAsFile())
            std::cerr << "roundtrip: could not copy fixture " << relativePath << "\n";
    }

    env.libraryService().addLocation (musicDirectory, false);

    const auto started = juce::Time::getMillisecondCounter();
    int lastTrackCount = -1;
    juce::int64 lastChangeMs = started;
    juce::int64 emptySince = 0;
    auto snapshot = env.libraryService().getSnapshot();

    while (juce::Time::getMillisecondCounter() - started < 15000)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (25);
        snapshot = env.libraryService().getSnapshot();

        const auto now = (juce::int64) juce::Time::getMillisecondCounter();
        const int count = (int) snapshot->tracks.size();
        if (count != lastTrackCount)
        {
            lastTrackCount = count;
            lastChangeMs = now;
        }

        const bool scanning = env.libraryService().isScanning();

        if (count > 0 && (! scanning || count >= 3))
            break;

        // A slow/partial cold scan must not stall the whole suite: proceed with
        // whatever has been published once the count is stable.
        if (count > 0 && now - lastChangeMs > 750)
            break;

        if (count == 0 && ! scanning)
        {
            if (emptySince == 0)
                emptySince = now;
            else if (now - emptySince > 500)
                break;
        }
        else
        {
            emptySince = 0;
        }
    }

    snapshot = env.libraryService().getSnapshot();
    if (snapshot->tracks.empty())
    {
        print ("SKIP: library scanner produced no tracks; "
               "resolver-level coverage is provided by PlaylistIO.ResolverAgainstSyntheticLibrary");
        return;
    }

    const bool scannerStillRunning = env.libraryService().isScanning();

    std::vector<TrackId> sourceIds;
    for (const auto& track : snapshot->tracks)
    {
        if (sourceIds.size() >= 3)
            break;
        if (juce::File (track.path).existsAsFile())
            sourceIds.push_back (track.id);
    }
    SP_REQUIRE (! sourceIds.empty());

    print ("library scan exposed " + juce::String ((int) snapshot->tracks.size())
           + " track(s) for the roundtrip (scanner still running: "
           + (scannerStillRunning ? "yes" : "no") + ")");

    const auto original = manager.createPlaylist ("Roundtrip Source");
    SP_REQUIRE (original != kInvalidPlaylistId);
    SP_REQUIRE (manager.addTracks (original, sourceIds));

    // Exporting into the library directory must use relative paths (all tracks
    // live inside that directory tree), and re-importing must resolve them.
    {
        const auto relativeFile = musicDirectory.getChildFile ("out.m3u8");
        juce::String error;
        SP_REQUIRE_MSG (manager.exportPlaylist (original, relativeFile, error), error.toStdString());

        playlistio::Document document;
        juce::String parseError;
        SP_REQUIRE_MSG (playlistio::parse (relativeFile, document, parseError), parseError.toStdString());
        SP_REQUIRE (document.entries.size() == sourceIds.size());
        for (const auto& entry : document.entries)
            SP_REQUIRE_MSG (! juce::File::isAbsolutePath (entry.path), entry.path.toStdString());

        PlaylistImportResult result;
        PlaylistId importedId = kInvalidPlaylistId;
        SP_REQUIRE_MSG (manager.importPlaylistFile (relativeFile, error, &importedId, &result),
                        error.toStdString());
        SP_REQUIRE_MSG (result.resolved == (int) sourceIds.size(),
                        (juce::String ("relative import resolved=") + juce::String (result.resolved)
                         + " of " + juce::String (result.entriesRead)).toStdString());
        SP_REQUIRE (manager.resolveTrackIds (importedId) == sourceIds);
        print ("relative-path export/import: " + juce::String ((int) sourceIds.size())
               + " tracks identical (ids + order)");
    }

    // Same but through the public export API with absolute paths and M3U / PLS.
    const auto formats = { juce::String ("m3u8"), juce::String ("m3u"), juce::String ("pls") };
    for (const auto& extension : formats)
    {
        const auto outFile = env.file ("roundtrip/source." + extension);
        juce::String error;
        SP_REQUIRE_MSG (manager.exportPlaylist (original, outFile, error), error.toStdString());
        SP_REQUIRE (outFile.existsAsFile());

        PlaylistImportResult result;
        PlaylistId importedId = kInvalidPlaylistId;
        SP_REQUIRE_MSG (manager.importPlaylistFile (outFile, error, &importedId, &result), error.toStdString());
        SP_REQUIRE_MSG (result.entriesRead == (int) sourceIds.size(), result.entriesRead);
        SP_REQUIRE_MSG (result.resolved == (int) sourceIds.size(),
                        (juce::String ("resolved=") + juce::String (result.resolved)
                         + " of " + juce::String (result.entriesRead)).toStdString());
        SP_REQUIRE (result.unresolved == 0);
        SP_REQUIRE (manager.resolveTrackIds (importedId) == sourceIds);

        print ("roundtrip " + extension + ": " + juce::String ((int) sourceIds.size())
               + " tracks identical (ids + order)");
    }

    // Import the fixture M3U8 with its absolute paths rewritten to the temp copy.
    {
        const auto fixtureM3U8 = fixturePlaylist ("fixture.m3u8");
        auto text = fixtureM3U8.loadFileAsString();
        text = text.replace (fixtureLibrary.getFullPathName(), musicDirectory.getFullPathName());
        const auto rewritten = env.writeTextFile ("rewritten/fixture.m3u8", text);

        // Expected resolution is data-driven: count how many of the fixture's
        // absolute paths are actually present in the library snapshot.
        const auto currentSnapshot = env.libraryService().getSnapshot();
        int expectedResolved = 0;
        for (const auto* relativePath : filesToCopy)
        {
            const auto expected = PlaylistPathResolver::normalisePath (
                musicDirectory.getChildFile (relativePath).getFullPathName());

            for (const auto& track : currentSnapshot->tracks)
            {
                if (PlaylistPathResolver::normalisePath (track.path) == expected)
                {
                    ++expectedResolved;
                    break;
                }
            }
        }

        PlaylistImportResult result;
        juce::String error;
        SP_REQUIRE_MSG (manager.importPlaylistFile (rewritten, error, nullptr, &result), error.toStdString());
        SP_REQUIRE (result.entriesRead == 3);
        SP_REQUIRE_MSG (result.resolved == expectedResolved,
                        (juce::String ("fixture import resolved=") + juce::String (result.resolved)
                         + " expected=" + juce::String (expectedResolved)).toStdString());
        SP_REQUIRE (result.unresolved == 3 - expectedResolved);
        print ("absolute-path fixture.m3u8 import: " + juce::String (result.resolved)
               + " of 3 resolved (expected " + juce::String (expectedResolved) + ")");
    }

    const bool scanningBeforePlaylistOps = env.libraryService().isScanning();

    // Smart playlist evaluated against the real scanned library snapshot.
    {
        snapshot = env.libraryService().getSnapshot();

        bool libraryHasJazz = false;
        for (const auto& track : snapshot->tracks)
            if (track.genre.equalsIgnoreCase ("Jazz"))
                libraryHasJazz = true;

        if (! libraryHasJazz)
        {
            print ("SKIP smart Jazz assertion: the scan has not published a Jazz track");
        }
        else
        {
            const auto smart = manager.createPlaylist ("Jazz Smart", true);
            SP_REQUIRE (smart != kInvalidPlaylistId);

            SmartPlaylistDefinition definition;
            definition.matchAll = true;
            definition.sortColumn = "artist";
            definition.rules.push_back (makeRule (SmartField::genre, SmartOp::is, "jazz"));
            SP_REQUIRE (manager.setSmartDefinition (smart, definition));

            const auto smartIds = manager.resolveTrackIds (smart);
            SP_REQUIRE (! smartIds.empty());

            for (const auto trackId : smartIds)
            {
                const auto* track = snapshot->findTrack (trackId);
                SP_REQUIRE (track != nullptr);
                SP_REQUIRE_MSG (track->genre.equalsIgnoreCase ("Jazz"), track->genre.toStdString());
            }

            const auto smartTracks = manager.resolveTracks (smart);
            SP_REQUIRE (smartTracks.size() == smartIds.size());

            print ("smart playlist against real library: " + juce::String ((int) smartIds.size())
                   + " Jazz tracks");
        }
    }

    // Rating rules: the library owns rating persistence, playlists only read it.
    {
        const auto ratedTrackId = sourceIds.front();
        env.libraryService().updateRating (ratedTrackId, 4);

        snapshot = env.libraryService().getSnapshot();
        const auto* ratedTrack = snapshot->findTrack (ratedTrackId);
        SP_REQUIRE (ratedTrack != nullptr);
        SP_REQUIRE_MSG (ratedTrack->rating == 4, ratedTrack->rating);

        int expectedRated = 0;
        for (const auto& track : snapshot->tracks)
            if (track.rating > 2)
                ++expectedRated;

        const auto ratingPlaylist = manager.createPlaylist ("Rated 3+", true);
        SP_REQUIRE (ratingPlaylist != kInvalidPlaylistId);

        SmartPlaylistDefinition ratingDefinition;
        ratingDefinition.rules.push_back (makeRule (SmartField::rating, SmartOp::greaterThan, {}, 2));
        SP_REQUIRE (manager.setSmartDefinition (ratingPlaylist, ratingDefinition));

        const auto ratingIds = manager.resolveTrackIds (ratingPlaylist);
        SP_REQUIRE_MSG ((int) ratingIds.size() == expectedRated,
                        (juce::String ("resolved ") + juce::String ((int) ratingIds.size())
                         + " expected " + juce::String (expectedRated)).toStdString());
        SP_REQUIRE (std::find (ratingIds.begin(), ratingIds.end(), ratedTrackId) != ratingIds.end());

        for (const auto trackId : ratingIds)
        {
            const auto* track = snapshot->findTrack (trackId);
            SP_REQUIRE (track != nullptr);
            SP_REQUIRE (track->rating > 2);
        }

        print ("smart rating > 2 against real library: " + juce::String ((int) ratingIds.size())
               + " tracks");
    }

    // High-resolution rules (Track One is 24-bit/96 kHz in the fixtures).
    {
        snapshot = env.libraryService().getSnapshot();

        int expectedHiRes = 0;
        for (const auto& track : snapshot->tracks)
            if (track.sampleRate > 48000)
                ++expectedHiRes;

        if (expectedHiRes == 0)
        {
            print ("SKIP smart sampleRate assertion: no >48 kHz track published yet");
        }
        else
        {
            const auto hiResPlaylist = manager.createPlaylist ("Hi-Res", true);
            SP_REQUIRE (hiResPlaylist != kInvalidPlaylistId);

            SmartPlaylistDefinition hiResDefinition;
            hiResDefinition.rules.push_back (makeRule (SmartField::sampleRate, SmartOp::greaterThan, {}, 48000));
            SP_REQUIRE (manager.setSmartDefinition (hiResPlaylist, hiResDefinition));

            const auto hiResIds = manager.resolveTrackIds (hiResPlaylist);
            SP_REQUIRE_MSG ((int) hiResIds.size() == expectedHiRes,
                            (juce::String ("resolved ") + juce::String ((int) hiResIds.size())
                             + " expected " + juce::String (expectedHiRes)).toStdString());

            for (const auto trackId : hiResIds)
            {
                const auto* track = snapshot->findTrack (trackId);
                SP_REQUIRE (track != nullptr);
                SP_REQUIRE (track->sampleRate > 48000);
            }

            print ("smart sampleRate > 48000 against real library: "
                   + juce::String ((int) hiResIds.size()) + " tracks");
        }
    }

    // Playlist mutation/evaluation must never start a library rescan.
    SP_REQUIRE (! (env.libraryService().isScanning() && ! scanningBeforePlaylistOps));
}
