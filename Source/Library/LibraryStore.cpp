#include "Library/LibraryStore.h"

namespace sp::librarystore
{

namespace
{
// Order MUST match kTrackColumns in LibraryDatabase.cpp and trackColumns() below,
// which is also the order used by trackFromStatement().
const char* kTrackFieldNames[] = {
    "path", "folder", "title", "artist", "album_artist", "album", "genre", "composer", "comment",
    "year", "track_number", "disc_number", "rating",
    "duration_ms", "sample_rate", "bit_depth", "num_channels", "bitrate_kbps",
    "format_name", "family", "is_dsd", "dsd_rate_multiple", "dsd_gain_db",
    "file_size", "file_modified_ms", "content_hash", "hash_computed", "missing",
    "replaygain_track_db", "has_replaygain_track", "replaygain_album_db", "has_replaygain_album",
    "peak_linear", "artwork_id",
    "date_added_iso", "date_added_ms", "play_count", "last_played_ms"
};

constexpr int kNumTrackFields = (int) (sizeof (kTrackFieldNames) / sizeof (kTrackFieldNames[0]));
static_assert (kNumTrackFields == 38, "track column list changed - update the schema, binding and reading code together");

// Column positions inside a SELECT built with trackColumns(): index 0 is id.
enum FieldIndex
{
    f_path = 1, f_folder, f_title, f_artist, f_albumArtist, f_album, f_genre, f_composer, f_comment,
    f_year, f_trackNumber, f_discNumber, f_rating,
    f_durationMs, f_sampleRate, f_bitDepth, f_numChannels, f_bitrateKbps,
    f_formatName, f_family, f_isDsd, f_dsdMultiple, f_dsdGainDb,
    f_fileSize, f_fileModifiedMs, f_contentHash, f_hashComputed, f_missing,
    f_replayGainTrack, f_hasReplayGainTrack, f_replayGainAlbum, f_hasReplayGainAlbum,
    f_peakLinear, f_artworkId,
    f_dateAddedIso, f_dateAddedMs, f_playCount, f_lastPlayedMs
};

juce::String joinFieldNames (const char* const* names, int count, int first)
{
    juce::String out;
    for (int i = first; i < count; ++i)
    {
        if (out.isNotEmpty())
            out << ", ";
        out << names[i];
    }
    return out;
}

juce::String questionMarks (int count)
{
    juce::String out;
    for (int i = 0; i < count; ++i)
    {
        if (i > 0)
            out << ", ";
        out << "?";
    }
    return out;
}

juce::String likePrefixForFolder (const juce::String& folderPath)
{
    auto escaped = folderPath.replace ("\\", "\\\\").replace ("%", "\\%").replace ("_", "\\_");
    if (! escaped.endsWithChar ('/'))
        escaped << "/";
    return escaped + "%";
}

void setError (juce::String* errorOut, const juce::String& message)
{
    if (errorOut != nullptr)
        *errorOut = message;
}

} // namespace

//==============================================================================
juce::String trackColumns()
{
    return "id, " + joinFieldNames (kTrackFieldNames, kNumTrackFields, 0);
}

Track trackFromStatement (const SqliteStatement& st)
{
    Track t;
    t.id                = st.getInt64 (0);
    t.path              = st.getString (f_path);
    t.folder            = st.getString (f_folder);
    t.title             = st.getString (f_title);
    t.artist            = st.getString (f_artist);
    t.albumArtist       = st.getString (f_albumArtist);
    t.album             = st.getString (f_album);
    t.genre             = st.getString (f_genre);
    t.composer          = st.getString (f_composer);
    t.comment           = st.getString (f_comment);
    t.year              = st.getInt (f_year);
    t.trackNumber       = st.getInt (f_trackNumber);
    t.discNumber        = st.getInt (f_discNumber);
    t.rating            = st.getInt (f_rating);
    t.durationMs        = st.getInt64 (f_durationMs);
    t.sampleRate        = st.getInt (f_sampleRate);
    t.bitDepth          = st.getInt (f_bitDepth);
    t.numChannels       = st.getInt (f_numChannels);
    t.bitrateKbps       = st.getInt (f_bitrateKbps);
    t.formatName        = st.getString (f_formatName);
    t.family            = (Track::Family) st.getInt (f_family);
    t.isDsd             = st.getInt64 (f_isDsd) != 0;
    t.dsdRateMultiple   = st.getInt (f_dsdMultiple);
    t.dsdGainDb         = (float) st.getDouble (f_dsdGainDb);
    t.fileSize          = st.getInt64 (f_fileSize);
    t.fileModifiedMs    = st.getInt64 (f_fileModifiedMs);
    t.contentHash       = st.getString (f_contentHash);
    t.hashComputed      = st.getInt64 (f_hashComputed) != 0;
    t.missing           = st.getInt64 (f_missing) != 0;
    t.replayGainTrackDb = (float) st.getDouble (f_replayGainTrack);
    t.hasReplayGainTrack= st.getInt64 (f_hasReplayGainTrack) != 0;
    t.replayGainAlbumDb = (float) st.getDouble (f_replayGainAlbum);
    t.hasReplayGainAlbum= st.getInt64 (f_hasReplayGainAlbum) != 0;
    t.peakLinear        = (float) st.getDouble (f_peakLinear);
    t.artworkId         = st.getString (f_artworkId);
    t.dateAddedIso      = st.getString (f_dateAddedIso);
    t.playCount         = st.getInt (f_playCount);
    t.lastPlayedMs      = st.getInt64 (f_lastPlayedMs);
    return t;
}

std::vector<Track> loadAllTracks (LibraryDatabase& db)
{
    const juce::ScopedLock sl (db.lock());
    std::vector<Track> tracks;

    if (! db.isOpen())
        return tracks;

    auto st = db.db().prepare ("SELECT " + trackColumns() + " FROM tracks "
                               "ORDER BY artist COLLATE NOCASE, album COLLATE NOCASE, "
                               "disc_number, track_number, path COLLATE NOCASE");
    if (st == nullptr)
        return tracks;

    while (st->step())
        tracks.push_back (trackFromStatement (*st));

    return tracks;
}

std::optional<Track> findTrackByPath (LibraryDatabase& db, const juce::String& absolutePath)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return {};

    auto st = db.db().prepare ("SELECT " + trackColumns() + " FROM tracks WHERE path = ?");
    if (st == nullptr)
        return {};

    st->bind (1, absolutePath);
    if (st->step())
        return trackFromStatement (*st);
    return {};
}

std::optional<Track> findTrackById (LibraryDatabase& db, TrackId id)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return {};

    auto st = db.db().prepare ("SELECT " + trackColumns() + " FROM tracks WHERE id = ?");
    if (st == nullptr)
        return {};

    st->bind (1, id);
    if (st->step())
        return trackFromStatement (*st);
    return {};
}

TrackId insertTrack (LibraryDatabase& db, const Track& track, juce::String* errorOut)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
    {
        setError (errorOut, "Library database is not open.");
        return 0;
    }

    auto st = db.db().prepare ("INSERT INTO tracks (" + joinFieldNames (kTrackFieldNames, kNumTrackFields, 0)
                               + ") VALUES (" + questionMarks (kNumTrackFields) + ")");
    if (st == nullptr)
    {
        setError (errorOut, db.db().lastError());
        return 0;
    }

    const auto now = juce::Time::currentTimeMillis();
    const auto dateAdded = track.dateAddedIso.isNotEmpty() ? track.dateAddedIso
                                                           : juce::Time::getCurrentTime().toISO8601 (true);

    int i = 1;
    st->bind (i++, track.path);
    st->bind (i++, track.folder);
    st->bind (i++, track.title);
    st->bind (i++, track.artist);
    st->bind (i++, track.albumArtist);
    st->bind (i++, track.album);
    st->bind (i++, track.genre);
    st->bind (i++, track.composer);
    st->bind (i++, track.comment);
    st->bind (i++, track.year);
    st->bind (i++, track.trackNumber);
    st->bind (i++, track.discNumber);
    st->bind (i++, track.rating);
    st->bind (i++, track.durationMs);
    st->bind (i++, track.sampleRate);
    st->bind (i++, track.bitDepth);
    st->bind (i++, track.numChannels);
    st->bind (i++, track.bitrateKbps);
    st->bind (i++, track.formatName);
    st->bind (i++, (int) track.family);
    st->bind (i++, track.isDsd);
    st->bind (i++, track.dsdRateMultiple);
    st->bind (i++, (double) track.dsdGainDb);
    st->bind (i++, track.fileSize);
    st->bind (i++, track.fileModifiedMs);
    st->bind (i++, track.contentHash);
    st->bind (i++, track.hashComputed);
    st->bind (i++, track.missing);
    st->bind (i++, (double) track.replayGainTrackDb);
    st->bind (i++, track.hasReplayGainTrack);
    st->bind (i++, (double) track.replayGainAlbumDb);
    st->bind (i++, track.hasReplayGainAlbum);
    st->bind (i++, (double) track.peakLinear);
    st->bind (i++, track.artworkId);
    st->bind (i++, dateAdded);
    st->bind (i++, now);
    st->bind (i++, track.playCount);
    st->bind (i++, track.lastPlayedMs);

    if (! st->exec())
    {
        setError (errorOut, st->getError().isNotEmpty() ? st->getError() : db.db().lastError());
        return 0;
    }

    return db.db().lastInsertRowid();
}

bool updateTrackFromScan (LibraryDatabase& db, const Track& t, juce::String* errorOut)
{
    if (t.id == kInvalidTrackId)
    {
        setError (errorOut, "Cannot update a track with an invalid id.");
        return false;
    }

    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
    {
        setError (errorOut, "Library database is not open.");
        return false;
    }

    auto st = db.db().prepare (
        "UPDATE tracks SET folder=?, title=?, artist=?, album_artist=?, album=?, genre=?, composer=?,"
        " comment=?, year=?, track_number=?, disc_number=?, rating=?, duration_ms=?, sample_rate=?,"
        " bit_depth=?, num_channels=?, bitrate_kbps=?, format_name=?, family=?, is_dsd=?,"
        " dsd_rate_multiple=?, dsd_gain_db=?, file_size=?, file_modified_ms=?, content_hash=?,"
        " hash_computed=?, missing=?, replaygain_track_db=?, has_replaygain_track=?,"
        " replaygain_album_db=?, has_replaygain_album=?, peak_linear=?, artwork_id=?"
        " WHERE id=?");
    if (st == nullptr)
    {
        setError (errorOut, db.db().lastError());
        return false;
    }

    int i = 1;
    st->bind (i++, t.folder);
    st->bind (i++, t.title);
    st->bind (i++, t.artist);
    st->bind (i++, t.albumArtist);
    st->bind (i++, t.album);
    st->bind (i++, t.genre);
    st->bind (i++, t.composer);
    st->bind (i++, t.comment);
    st->bind (i++, t.year);
    st->bind (i++, t.trackNumber);
    st->bind (i++, t.discNumber);
    st->bind (i++, t.rating);
    st->bind (i++, t.durationMs);
    st->bind (i++, t.sampleRate);
    st->bind (i++, t.bitDepth);
    st->bind (i++, t.numChannels);
    st->bind (i++, t.bitrateKbps);
    st->bind (i++, t.formatName);
    st->bind (i++, (int) t.family);
    st->bind (i++, t.isDsd);
    st->bind (i++, t.dsdRateMultiple);
    st->bind (i++, (double) t.dsdGainDb);
    st->bind (i++, t.fileSize);
    st->bind (i++, t.fileModifiedMs);
    st->bind (i++, t.contentHash);
    st->bind (i++, t.hashComputed);
    st->bind (i++, t.missing);
    st->bind (i++, (double) t.replayGainTrackDb);
    st->bind (i++, t.hasReplayGainTrack);
    st->bind (i++, (double) t.replayGainAlbumDb);
    st->bind (i++, t.hasReplayGainAlbum);
    st->bind (i++, (double) t.peakLinear);
    st->bind (i++, t.artworkId);
    st->bind (i++, t.id);

    if (! st->exec())
    {
        setError (errorOut, st->getError().isNotEmpty() ? st->getError() : db.db().lastError());
        return false;
    }
    return true;
}

bool updateTrackTagFields (LibraryDatabase& db, const Track& t, juce::String* errorOut)
{
    if (t.id == kInvalidTrackId)
    {
        setError (errorOut, "Cannot update a track with an invalid id.");
        return false;
    }

    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
    {
        setError (errorOut, "Library database is not open.");
        return false;
    }

    auto st = db.db().prepare (
        "UPDATE tracks SET title=?, artist=?, album_artist=?, album=?, genre=?, composer=?, comment=?,"
        " year=?, track_number=?, disc_number=?, rating=? WHERE id=?");
    if (st == nullptr)
    {
        setError (errorOut, db.db().lastError());
        return false;
    }

    st->bind (1, t.title);
    st->bind (2, t.artist);
    st->bind (3, t.albumArtist);
    st->bind (4, t.album);
    st->bind (5, t.genre);
    st->bind (6, t.composer);
    st->bind (7, t.comment);
    st->bind (8, t.year);
    st->bind (9, t.trackNumber);
    st->bind (10, t.discNumber);
    st->bind (11, t.rating);
    st->bind (12, t.id);

    if (! st->exec())
    {
        setError (errorOut, st->getError().isNotEmpty() ? st->getError() : db.db().lastError());
        return false;
    }
    return true;
}

bool updateRating (LibraryDatabase& db, TrackId id, int rating)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return false;

    auto st = db.db().prepare ("UPDATE tracks SET rating=? WHERE id=?");
    if (st == nullptr)
        return false;
    st->bind (1, rating);
    st->bind (2, id);
    return st->exec();
}

bool updatePlayStatistics (LibraryDatabase& db, TrackId id, int playCount, juce::int64 lastPlayedMs)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return false;

    auto st = db.db().prepare ("UPDATE tracks SET play_count=?, last_played_ms=? WHERE id=?");
    if (st == nullptr)
        return false;
    st->bind (1, playCount);
    st->bind (2, lastPlayedMs);
    st->bind (3, id);
    return st->exec();
}

bool setMissing (LibraryDatabase& db, TrackId id, bool missing)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return false;

    auto st = db.db().prepare ("UPDATE tracks SET missing=? WHERE id=?");
    if (st == nullptr)
        return false;
    st->bind (1, missing);
    st->bind (2, id);
    return st->exec();
}

bool setContentHash (LibraryDatabase& db, TrackId id, const juce::String& hash)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return false;

    auto st = db.db().prepare ("UPDATE tracks SET content_hash=?, hash_computed=1 WHERE id=?");
    if (st == nullptr)
        return false;
    st->bind (1, hash);
    st->bind (2, id);
    return st->exec();
}

int deleteTracks (LibraryDatabase& db, const std::vector<TrackId>& ids)
{
    if (ids.empty())
        return 0;

    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return 0;

    auto st = db.db().prepare ("DELETE FROM tracks WHERE id IN (" + questionMarks ((int) ids.size()) + ")");
    if (st == nullptr)
        return 0;

    for (int i = 0; i < (int) ids.size(); ++i)
        st->bind (i + 1, ids[(size_t) i]);

    if (! st->exec())
        return 0;
    return db.db().lastChanges();
}

int deleteTracksUnderFolder (LibraryDatabase& db, const juce::String& folderPath)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return 0;

    auto st = db.db().prepare ("DELETE FROM tracks WHERE path LIKE ? ESCAPE '\\'");
    if (st == nullptr)
        return 0;
    st->bind (1, likePrefixForFolder (folderPath));
    if (! st->exec())
        return 0;
    return db.db().lastChanges();
}

int setMissingUnderFolder (LibraryDatabase& db, const juce::String& folderPath, bool missing)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return 0;

    auto st = db.db().prepare ("UPDATE tracks SET missing=? WHERE path LIKE ? ESCAPE '\\'");
    if (st == nullptr)
        return 0;
    st->bind (1, missing);
    st->bind (2, likePrefixForFolder (folderPath));
    if (! st->exec())
        return 0;
    return db.db().lastChanges();
}

int resetContentHashes (LibraryDatabase& db)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return 0;

    auto st = db.db().prepare ("UPDATE tracks SET content_hash='', hash_computed=0 WHERE hash_computed<>0");
    if (st == nullptr || ! st->exec())
        return 0;
    return db.db().lastChanges();
}

int countTracks (LibraryDatabase& db)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return 0;

    auto st = db.db().prepare ("SELECT COUNT(*) FROM tracks");
    if (st == nullptr || ! st->step())
        return 0;
    return st->getInt (0);
}

//==============================================================================
std::vector<LibraryLocation> loadAllLocations (LibraryDatabase& db)
{
    const juce::ScopedLock sl (db.lock());
    std::vector<LibraryLocation> locations;

    if (! db.isOpen())
        return locations;

    auto st = db.db().prepare ("SELECT id, path, recursive, watch, online, last_scan_ms FROM locations "
                               "ORDER BY path COLLATE NOCASE");
    if (st == nullptr)
        return locations;

    while (st->step())
    {
        LibraryLocation l;
        l.id = st->getInt64 (0);
        l.path = st->getString (1);
        l.recursive = st->getInt64 (2) != 0;
        l.watch = st->getInt64 (3) != 0;
        l.online = st->getInt64 (4) != 0;
        l.lastScanMs = st->getInt64 (5);
        l.trackCount = 0;   // filled in by the snapshot builder from the tracks table
        locations.push_back (l);
    }
    return locations;
}

LocationId ensureLocation (LibraryDatabase& db, const juce::String& path, bool recursive, bool watch,
                           juce::String* errorOut)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
    {
        setError (errorOut, "Library database is not open.");
        return 0;
    }

    {
        auto find = db.db().prepare ("SELECT id FROM locations WHERE path = ?");
        if (find == nullptr)
        {
            setError (errorOut, db.db().lastError());
            return 0;
        }
        find->bind (1, path);
        if (find->step())
        {
            const auto existing = find->getInt64 (0);
            // Remember the latest watch preference for an existing location.
            auto upd = db.db().prepare ("UPDATE locations SET watch=? WHERE id=?");
            if (upd != nullptr)
            {
                upd->bind (1, watch);
                upd->bind (2, existing);
                upd->exec();
            }
            return existing;
        }
    }

    auto st = db.db().prepare ("INSERT INTO locations(path, recursive, watch, online, last_scan_ms, added_ms) "
                               "VALUES(?, ?, ?, 1, 0, ?)");
    if (st == nullptr)
    {
        setError (errorOut, db.db().lastError());
        return 0;
    }
    st->bind (1, path);
    st->bind (2, recursive);
    st->bind (3, watch);
    st->bind (4, juce::Time::currentTimeMillis());
    if (! st->exec())
    {
        setError (errorOut, st->getError().isNotEmpty() ? st->getError() : db.db().lastError());
        return 0;
    }
    return db.db().lastInsertRowid();
}

bool updateLocationWatch (LibraryDatabase& db, LocationId id, bool watch)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return false;

    auto st = db.db().prepare ("UPDATE locations SET watch=? WHERE id=?");
    if (st == nullptr)
        return false;
    st->bind (1, watch);
    st->bind (2, id);
    return st->exec();
}

bool updateLocationAfterScan (LibraryDatabase& db, LocationId id, bool online, juce::int64 lastScanMs)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return false;

    auto st = db.db().prepare ("UPDATE locations SET online=?, last_scan_ms=? WHERE id=?");
    if (st == nullptr)
        return false;
    st->bind (1, online);
    st->bind (2, lastScanMs);
    st->bind (3, id);
    return st->exec();
}

bool deleteLocation (LibraryDatabase& db, LocationId id)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return false;

    auto st = db.db().prepare ("DELETE FROM locations WHERE id=?");
    if (st == nullptr)
        return false;
    st->bind (1, id);
    return st->exec();
}

//==============================================================================
bool saveAnalysisJson (LibraryDatabase& db, TrackId id, const juce::String& json, int version,
                       juce::int64 analyzedAtMs)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return false;

    auto st = db.db().prepare ("INSERT INTO analysis(track_id, json, version, analyzed_at) VALUES(?, ?, ?, ?) "
                               "ON CONFLICT(track_id) DO UPDATE SET json=excluded.json, "
                               "version=excluded.version, analyzed_at=excluded.analyzed_at");
    if (st == nullptr)
        return false;
    st->bind (1, id);
    st->bind (2, json);
    st->bind (3, version);
    st->bind (4, analyzedAtMs);
    return st->exec();
}

bool loadAnalysisJson (LibraryDatabase& db, TrackId id, juce::String& jsonOut, int& versionOut,
                       juce::int64& analyzedAtOut)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return false;

    auto st = db.db().prepare ("SELECT json, version, analyzed_at FROM analysis WHERE track_id=?");
    if (st == nullptr)
        return false;
    st->bind (1, id);
    if (! st->step())
        return false;

    jsonOut = st->getString (0);
    versionOut = st->getInt (1);
    analyzedAtOut = st->getInt64 (2);
    return jsonOut.isNotEmpty();
}

//==============================================================================
juce::String getMeta (LibraryDatabase& db, const juce::String& key, const juce::String& fallback)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return fallback;

    auto st = db.db().prepare ("SELECT value FROM meta WHERE key=?");
    if (st == nullptr)
        return fallback;
    st->bind (1, key);
    if (st->step())
        return st->getString (0);
    return fallback;
}

bool setMeta (LibraryDatabase& db, const juce::String& key, const juce::String& value)
{
    const juce::ScopedLock sl (db.lock());
    if (! db.isOpen())
        return false;

    auto st = db.db().prepare ("INSERT INTO meta(key, value) VALUES(?, ?) "
                               "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    if (st == nullptr)
        return false;
    st->bind (1, key);
    st->bind (2, value);
    return st->exec();
}

} // namespace sp::librarystore
