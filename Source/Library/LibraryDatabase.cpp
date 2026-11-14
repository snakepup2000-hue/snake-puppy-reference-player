/*
    Snake Puppy Reference Player
    SQLite schema for the music library.

    Everything here is idempotent: opening an existing database migrates it in
    place (missing columns are added) without dropping rows. WAL, foreign keys
    and the busy timeout are set by SqliteDb::open(). The schema is deliberately
    plain SQL (no blobs): artwork lives in the artwork cache directory and its
    content hash is stored in tracks.artwork_id.
*/
#include "Library/LibraryDatabase.h"

#include <vector>

namespace sp
{

namespace
{
struct ColumnSpec
{
    const char* name;
    const char* definition;         // includes the DEFAULT, so ALTER TABLE works
};

// tracks: every field of sp::Track except the runtime-only convenience methods.
const ColumnSpec kTrackColumns[] = {
    { "path",                 "TEXT NOT NULL DEFAULT ''" },
    { "folder",               "TEXT NOT NULL DEFAULT ''" },
    { "title",                "TEXT NOT NULL DEFAULT ''" },
    { "artist",               "TEXT NOT NULL DEFAULT ''" },
    { "album_artist",         "TEXT NOT NULL DEFAULT ''" },
    { "album",                "TEXT NOT NULL DEFAULT ''" },
    { "genre",                "TEXT NOT NULL DEFAULT ''" },
    { "composer",             "TEXT NOT NULL DEFAULT ''" },
    { "comment",              "TEXT NOT NULL DEFAULT ''" },
    { "year",                 "INTEGER NOT NULL DEFAULT 0" },
    { "track_number",         "INTEGER NOT NULL DEFAULT 0" },
    { "disc_number",          "INTEGER NOT NULL DEFAULT 0" },
    { "rating",               "INTEGER NOT NULL DEFAULT 0" },
    { "duration_ms",          "INTEGER NOT NULL DEFAULT 0" },
    { "sample_rate",          "INTEGER NOT NULL DEFAULT 0" },
    { "bit_depth",            "INTEGER NOT NULL DEFAULT 0" },
    { "num_channels",         "INTEGER NOT NULL DEFAULT 0" },
    { "bitrate_kbps",         "INTEGER NOT NULL DEFAULT 0" },
    { "format_name",          "TEXT NOT NULL DEFAULT ''" },
    { "family",               "INTEGER NOT NULL DEFAULT 5" },   // Family::unknown
    { "is_dsd",               "INTEGER NOT NULL DEFAULT 0" },
    { "dsd_rate_multiple",    "INTEGER NOT NULL DEFAULT 0" },
    { "dsd_gain_db",          "REAL NOT NULL DEFAULT -6.0" },
    { "file_size",            "INTEGER NOT NULL DEFAULT 0" },
    { "file_modified_ms",     "INTEGER NOT NULL DEFAULT 0" },
    { "content_hash",         "TEXT NOT NULL DEFAULT ''" },
    { "hash_computed",        "INTEGER NOT NULL DEFAULT 0" },
    { "missing",              "INTEGER NOT NULL DEFAULT 0" },
    { "replaygain_track_db",  "REAL NOT NULL DEFAULT 0" },
    { "has_replaygain_track", "INTEGER NOT NULL DEFAULT 0" },
    { "replaygain_album_db",  "REAL NOT NULL DEFAULT 0" },
    { "has_replaygain_album", "INTEGER NOT NULL DEFAULT 0" },
    { "peak_linear",          "REAL NOT NULL DEFAULT 0" },
    { "artwork_id",           "TEXT NOT NULL DEFAULT ''" },
    { "date_added_iso",       "TEXT NOT NULL DEFAULT ''" },
    { "date_added_ms",        "INTEGER NOT NULL DEFAULT 0" },
    { "play_count",           "INTEGER NOT NULL DEFAULT 0" },
    { "last_played_ms",       "INTEGER NOT NULL DEFAULT 0" }
};

const ColumnSpec kLocationColumns[] = {
    { "path",        "TEXT NOT NULL DEFAULT ''" },
    { "recursive",   "INTEGER NOT NULL DEFAULT 1" },
    { "watch",       "INTEGER NOT NULL DEFAULT 1" },
    { "online",      "INTEGER NOT NULL DEFAULT 1" },
    { "last_scan_ms","INTEGER NOT NULL DEFAULT 0" },
    { "added_ms",    "INTEGER NOT NULL DEFAULT 0" }
};

const ColumnSpec kAnalysisColumns[] = {
    { "track_id",    "INTEGER NOT NULL DEFAULT 0" },
    { "json",        "TEXT NOT NULL DEFAULT ''" },
    { "version",     "INTEGER NOT NULL DEFAULT 0" },
    { "analyzed_at", "INTEGER NOT NULL DEFAULT 0" }
};

bool tableExists (SqliteDb& db, const juce::String& table)
{
    auto st = db.prepare ("SELECT name FROM sqlite_master WHERE type='table' AND name=?");
    if (st == nullptr)
        return false;
    st->bind (1, table);
    return st->step();
}

bool columnExists (SqliteDb& db, const juce::String& table, const juce::String& column)
{
    auto st = db.prepare ("PRAGMA table_info(" + table + ")");
    if (st == nullptr)
        return false;

    while (st->step())
        if (st->getString (1).equalsIgnoreCase (column))
            return true;

    return false;
}

juce::String addMissingColumns (SqliteDb& db, const juce::String& table, const ColumnSpec* specs, int numSpecs)
{
    for (int i = 0; i < numSpecs; ++i)
    {
        const juce::String name (specs[i].name);
        if (columnExists (db, table, name))
            continue;

        const juce::String sql = "ALTER TABLE " + table + " ADD COLUMN " + name + " " + specs[i].definition;
        juce::String err;
        if (! db.execute (sql, &err))
            return "Could not add column " + table + "." + name + ": " + err;
    }
    return {};
}

juce::String createSchema (SqliteDb& db)
{
    juce::String err;

    const char* statements[] = {
        "CREATE TABLE IF NOT EXISTS meta ("
        " key TEXT PRIMARY KEY,"
        " value TEXT NOT NULL DEFAULT ''"
        ")",

        "CREATE TABLE IF NOT EXISTS tracks ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " path TEXT NOT NULL DEFAULT '',"
        " folder TEXT NOT NULL DEFAULT '',"
        " title TEXT NOT NULL DEFAULT '',"
        " artist TEXT NOT NULL DEFAULT '',"
        " album_artist TEXT NOT NULL DEFAULT '',"
        " album TEXT NOT NULL DEFAULT '',"
        " genre TEXT NOT NULL DEFAULT '',"
        " composer TEXT NOT NULL DEFAULT '',"
        " comment TEXT NOT NULL DEFAULT '',"
        " year INTEGER NOT NULL DEFAULT 0,"
        " track_number INTEGER NOT NULL DEFAULT 0,"
        " disc_number INTEGER NOT NULL DEFAULT 0,"
        " rating INTEGER NOT NULL DEFAULT 0,"
        " duration_ms INTEGER NOT NULL DEFAULT 0,"
        " sample_rate INTEGER NOT NULL DEFAULT 0,"
        " bit_depth INTEGER NOT NULL DEFAULT 0,"
        " num_channels INTEGER NOT NULL DEFAULT 0,"
        " bitrate_kbps INTEGER NOT NULL DEFAULT 0,"
        " format_name TEXT NOT NULL DEFAULT '',"
        " family INTEGER NOT NULL DEFAULT 5,"
        " is_dsd INTEGER NOT NULL DEFAULT 0,"
        " dsd_rate_multiple INTEGER NOT NULL DEFAULT 0,"
        " dsd_gain_db REAL NOT NULL DEFAULT -6.0,"
        " file_size INTEGER NOT NULL DEFAULT 0,"
        " file_modified_ms INTEGER NOT NULL DEFAULT 0,"
        " content_hash TEXT NOT NULL DEFAULT '',"
        " hash_computed INTEGER NOT NULL DEFAULT 0,"
        " missing INTEGER NOT NULL DEFAULT 0,"
        " replaygain_track_db REAL NOT NULL DEFAULT 0,"
        " has_replaygain_track INTEGER NOT NULL DEFAULT 0,"
        " replaygain_album_db REAL NOT NULL DEFAULT 0,"
        " has_replaygain_album INTEGER NOT NULL DEFAULT 0,"
        " peak_linear REAL NOT NULL DEFAULT 0,"
        " artwork_id TEXT NOT NULL DEFAULT '',"
        " date_added_iso TEXT NOT NULL DEFAULT '',"
        " date_added_ms INTEGER NOT NULL DEFAULT 0,"
        " play_count INTEGER NOT NULL DEFAULT 0,"
        " last_played_ms INTEGER NOT NULL DEFAULT 0"
        ")",

        "CREATE TABLE IF NOT EXISTS locations ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " path TEXT NOT NULL DEFAULT '',"
        " recursive INTEGER NOT NULL DEFAULT 1,"
        " watch INTEGER NOT NULL DEFAULT 1,"
        " online INTEGER NOT NULL DEFAULT 1,"
        " last_scan_ms INTEGER NOT NULL DEFAULT 0,"
        " added_ms INTEGER NOT NULL DEFAULT 0"
        ")",

        "CREATE TABLE IF NOT EXISTS analysis ("
        " track_id INTEGER PRIMARY KEY,"
        " json TEXT NOT NULL DEFAULT '',"
        " version INTEGER NOT NULL DEFAULT 0,"
        " analyzed_at INTEGER NOT NULL DEFAULT 0,"
        " FOREIGN KEY (track_id) REFERENCES tracks(id) ON DELETE CASCADE"
        ")"
    };

    for (auto* sql : statements)
        if (! db.execute (sql, &err))
            return juce::String ("Could not create library schema: ") + err;

    if (auto e = addMissingColumns (db, "tracks", kTrackColumns, (int) (sizeof (kTrackColumns) / sizeof (kTrackColumns[0]))); e.isNotEmpty())
        return e;
    if (auto e = addMissingColumns (db, "locations", kLocationColumns, (int) (sizeof (kLocationColumns) / sizeof (kLocationColumns[0]))); e.isNotEmpty())
        return e;
    if (auto e = addMissingColumns (db, "analysis", kAnalysisColumns, (int) (sizeof (kAnalysisColumns) / sizeof (kAnalysisColumns[0]))); e.isNotEmpty())
        return e;

    // Path is the natural key for tracks. Refuse (with a clear error) rather
    // than silently deleting rows if a legacy database contains duplicates.
    {
        auto dup = db.prepare ("SELECT COUNT(*) FROM (SELECT path FROM tracks GROUP BY path HAVING COUNT(*) > 1)");
        if (dup != nullptr && dup->step() && dup->getInt64 (0) > 0)
            return "Library database contains duplicate track paths; please move it aside and rescan.";
    }

    const char* indexes[] = {
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_tracks_path ON tracks(path)",
        "CREATE INDEX IF NOT EXISTS idx_tracks_path_nocase ON tracks(path COLLATE NOCASE)",
        "CREATE INDEX IF NOT EXISTS idx_tracks_folder ON tracks(folder)",
        "CREATE INDEX IF NOT EXISTS idx_tracks_artist ON tracks(artist COLLATE NOCASE)",
        "CREATE INDEX IF NOT EXISTS idx_tracks_album ON tracks(album COLLATE NOCASE)",
        "CREATE INDEX IF NOT EXISTS idx_tracks_genre ON tracks(genre COLLATE NOCASE)",
        "CREATE INDEX IF NOT EXISTS idx_tracks_content_hash ON tracks(content_hash)",
        "CREATE INDEX IF NOT EXISTS idx_tracks_missing ON tracks(missing)",
        "CREATE INDEX IF NOT EXISTS idx_tracks_last_played ON tracks(last_played_ms)",
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_locations_path ON locations(path)",
        "CREATE INDEX IF NOT EXISTS idx_analysis_analyzed_at ON analysis(analyzed_at)"
    };

    for (auto* sql : indexes)
        if (! db.execute (sql, &err))
            return juce::String ("Could not create library index: ") + err;

    auto setMeta = [&db] (const juce::String& key, const juce::String& value)
    {
        auto st = db.prepare ("INSERT INTO meta(key, value) VALUES(?, ?) "
                              "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        if (st == nullptr)
            return false;
        st->bind (1, key);
        st->bind (2, value);
        return st->exec();
    };

    if (! setMeta ("schema_version", "1"))
        return "Could not write the library schema version.";
    if (! setMeta ("watch_enabled", "1"))
        return "Could not write the library watch default.";

    return {};
}
} // namespace

//==============================================================================
struct LibraryDatabase::Impl
{
    SqliteDb database;
    juce::CriticalSection lock;
};

LibraryDatabase::LibraryDatabase() : impl (std::make_unique<Impl>()) {}
LibraryDatabase::~LibraryDatabase() { close(); }

juce::String LibraryDatabase::open (const juce::File& dbFile)
{
    const juce::ScopedLock sl (impl->lock);

    juce::String err;
    if (! impl->database.open (dbFile, err))
        return "Could not open library database: " + err;

    return {};
}

void LibraryDatabase::close() { impl->database.close(); }
bool LibraryDatabase::isOpen() const { return impl->database.isOpen(); }

juce::String LibraryDatabase::initialiseSchema()
{
    if (! impl->database.isOpen())
        return "Library database is not open.";

    const juce::ScopedLock sl (impl->lock);

    juce::String err;
    if (! impl->database.beginTransaction())
        return "Could not start the library schema transaction: " + impl->database.lastError();

    err = createSchema (impl->database);
    if (err.isEmpty())
    {
        if (! impl->database.commitTransaction())
            err = "Could not commit the library schema: " + impl->database.lastError();
    }

    if (err.isNotEmpty())
        impl->database.rollbackTransaction();

    return err;
}

SqliteDb& LibraryDatabase::db() { return impl->database; }
const SqliteDb& LibraryDatabase::db() const { return impl->database; }
juce::CriticalSection& LibraryDatabase::lock() { return impl->lock; }
bool LibraryDatabase::beginTransaction() { return impl->database.beginTransaction(); }
bool LibraryDatabase::commitTransaction() { return impl->database.commitTransaction(); }
void LibraryDatabase::rollbackTransaction() { impl->database.rollbackTransaction(); }

} // namespace sp
