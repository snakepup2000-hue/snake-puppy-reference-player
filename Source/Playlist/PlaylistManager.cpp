#include "Playlist/PlaylistManager.h"

#include "Core/Serialization.h"
#include "Playlist/PlaylistIO.h"
#include "Playlist/PlaylistResolver.h"
#include "Playlist/SmartPlaylist.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <utility>

namespace sp
{

namespace
{
constexpr int kMaxPlaylistNameLength = 200;
constexpr int kMaxImportedEntries = 100000;
constexpr int kMaxReportedUnresolvedEntries = 200;

juce::String playlistFriendlyError (const juce::String& context, const juce::String& detail)
{
    if (detail.isEmpty())
        return context;

    return context + ": " + detail;
}
} // namespace

//==============================================================================
const Playlist* PlaylistSnapshot::find (PlaylistId id) const
{
    for (const auto& playlist : playlists)
        if (playlist.id == id)
            return &playlist;

    return nullptr;
}

int PlaylistSnapshot::indexOf (PlaylistId id) const
{
    for (size_t i = 0; i < playlists.size(); ++i)
        if (playlists[i].id == id)
            return (int) i;

    return -1;
}

//==============================================================================
juce::String PlaylistImportResult::message() const
{
    juce::String text;
    text << "Imported \"" << (playlistName.isNotEmpty() ? playlistName : juce::String ("playlist"))
         << "\" (" << format << "): " << resolved << " of " << entriesRead
         << " entries matched the library";

    if (unresolved > 0)
        text << " (" << unresolved << " not found)";

    return text + ".";
}

//==============================================================================
struct PlaylistManager::Impl
{
    Impl (PlaylistManager& ownerToUse, LibraryDatabase& databaseToUse, LibraryService& libraryToUse)
        : owner (ownerToUse), database (databaseToUse), library (libraryToUse)
    {
        snapshot = std::make_shared<PlaylistSnapshot>();
    }

    PlaylistManager& owner;
    LibraryDatabase& database;
    LibraryService& library;

    std::shared_ptr<const PlaylistSnapshot> snapshot;
    mutable juce::SpinLock snapshotLock;
    std::uint64_t nextVersion = 1;
    bool initialised = false;

    juce::String lastError;
    mutable juce::SpinLock errorLock;

    //== Errors / snapshot ====================================================
    void setError (const juce::String& error)
    {
        {
            const juce::SpinLock::ScopedLockType sl (errorLock);
            lastError = error;
        }

        juce::Logger::writeToLog ("Playlist: " + error);
    }

    juce::String getError() const
    {
        const juce::SpinLock::ScopedLockType sl (errorLock);
        return lastError;
    }

    std::shared_ptr<const PlaylistSnapshot> current() const
    {
        const juce::SpinLock::ScopedLockType sl (snapshotLock);
        return snapshot;
    }

    void publish (std::vector<Playlist>&& playlists, bool notify)
    {
        auto next = std::make_shared<PlaylistSnapshot>();
        next->playlists = std::move (playlists);

        {
            const juce::SpinLock::ScopedLockType sl (snapshotLock);
            next->version = nextVersion++;
            snapshot = next;
        }

        if (notify)
            owner.sendChangeMessage();
    }

    //== Transactions =========================================================
    /** Runs `work` under the shared database lock inside one transaction. */
    bool runInTransaction (const std::function<bool (juce::String&)>& work, juce::String& errorOut)
    {
        const juce::ScopedLock sl (database.lock());

        if (! database.isOpen())
        {
            errorOut = "Library database is not open";
            return false;
        }

        if (! database.beginTransaction())
        {
            errorOut = playlistFriendlyError ("Could not begin a playlist transaction", database.db().lastError());
            return false;
        }

        juce::String workError;
        if (! work (workError))
        {
            database.rollbackTransaction();
            errorOut = workError.isNotEmpty() ? workError : juce::String ("Playlist transaction failed");
            return false;
        }

        if (! database.commitTransaction())
        {
            const auto commitError = database.db().lastError();
            database.rollbackTransaction();
            errorOut = playlistFriendlyError ("Could not commit playlist changes", commitError);
            return false;
        }

        return true;
    }

    bool reloadAndPublish()
    {
        std::vector<Playlist> playlists;
        juce::String error;

        {
            const juce::ScopedLock sl (database.lock());

            if (! database.isOpen())
            {
                setError ("Library database is not open");
                return false;
            }

            if (! loadAll (playlists, error))
            {
                setError (error);
                return false;
            }
        }

        publish (std::move (playlists), true);
        return true;
    }

    //== Schema / loading =====================================================
    bool ensureSchema (juce::String& errorOut)
    {
        static const char* statements[]
        {
            "CREATE TABLE IF NOT EXISTS playlists ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "name TEXT NOT NULL,"
            "smart INTEGER NOT NULL DEFAULT 0,"
            "match_all INTEGER NOT NULL DEFAULT 1,"
            "sort_column TEXT NOT NULL DEFAULT 'artist',"
            "sort_desc INTEGER NOT NULL DEFAULT 0,"
            "limit_count INTEGER NOT NULL DEFAULT 0,"
            "smart_json TEXT,"
            "created INTEGER NOT NULL DEFAULT 0,"
            "modified INTEGER NOT NULL DEFAULT 0)",

            "CREATE UNIQUE INDEX IF NOT EXISTS idx_playlists_name"
            " ON playlists(name COLLATE NOCASE)",

            "CREATE TABLE IF NOT EXISTS playlist_items ("
            "playlist_id INTEGER NOT NULL,"
            "position INTEGER NOT NULL,"
            "track_id INTEGER NOT NULL,"
            "PRIMARY KEY (playlist_id, position),"
            "FOREIGN KEY (playlist_id) REFERENCES playlists(id) ON DELETE CASCADE)",

            "CREATE INDEX IF NOT EXISTS idx_playlist_items_playlist"
            " ON playlist_items(playlist_id, position)",

            "CREATE INDEX IF NOT EXISTS idx_playlist_items_track"
            " ON playlist_items(track_id)"
        };

        for (const auto* sql : statements)
        {
            juce::String error;
            if (! database.db().execute (sql, &error))
            {
                errorOut = playlistFriendlyError ("Could not create playlist tables", error);
                return false;
            }
        }

        return true;
    }

    bool loadAll (std::vector<Playlist>& out, juce::String& errorOut)
    {
        out.clear();

        auto statement = database.db().prepare (
            "SELECT id, name, smart, match_all, sort_column, sort_desc, limit_count, smart_json, created, modified "
            "FROM playlists ORDER BY name COLLATE NOCASE, id");

        if (statement == nullptr || ! statement->isValid())
        {
            errorOut = playlistFriendlyError ("Could not read playlists", database.db().lastError());
            return false;
        }

        std::unordered_map<PlaylistId, size_t> indexById;

        while (statement->step())
        {
            Playlist playlist;
            playlist.id = statement->getInt64 (0);
            playlist.name = statement->getString (1);
            playlist.smart = statement->getInt (2) != 0;

            SmartPlaylistDefinition fallback;
            fallback.matchAll = statement->getInt (3) != 0;
            fallback.sortColumn = statement->getString (4);
            if (fallback.sortColumn.isEmpty())
                fallback.sortColumn = "artist";
            fallback.sortDescending = statement->getInt (5) != 0;
            fallback.limit = statement->getInt (6);

            playlist.smartDef = fallback;
            const auto smartJson = statement->getString (7);
            if (playlist.smart && smartJson.isNotEmpty())
            {
                const auto parsed = juce::JSON::parse (smartJson);
                if (parsed.isObject())
                    playlist.smartDef = serialize::smartDefFromVar (parsed, fallback);
            }

            playlist.createdMs = statement->getInt64 (8);
            playlist.modifiedMs = statement->getInt64 (9);

            indexById[playlist.id] = out.size();
            out.push_back (std::move (playlist));
        }

        if (statement->getError().isNotEmpty())
        {
            // step() only populates the error string for real SQLite failures;
            // a normal SQLITE_DONE end of iteration leaves it empty.
            errorOut = playlistFriendlyError ("Could not read playlists", statement->getError());
            return false;
        }

        auto items = database.db().prepare (
            "SELECT playlist_id, track_id FROM playlist_items ORDER BY playlist_id, position");

        if (items == nullptr || ! items->isValid())
        {
            errorOut = playlistFriendlyError ("Could not read playlist items", database.db().lastError());
            return false;
        }

        while (items->step())
        {
            const auto playlistId = items->getInt64 (0);
            const auto found = indexById.find (playlistId);
            if (found == indexById.end())
            {
                juce::Logger::writeToLog ("Playlist: ignoring playlist_items row for missing playlist "
                                          + juce::String (playlistId));
                continue;
            }

            out[found->second].trackIds.push_back (items->getInt64 (1));
        }

        return true;
    }

    //== Writing ==============================================================
    juce::String uniqueName (const juce::String& desired, PlaylistId excludingId) const
    {
        auto base = desired.trim().substring (0, kMaxPlaylistNameLength).trim();
        if (base.isEmpty())
            base = "New Playlist";

        const auto snapshotNow = current();

        auto isTaken = [&snapshotNow, excludingId] (const juce::String& candidate)
        {
            for (const auto& playlist : snapshotNow->playlists)
                if (playlist.id != excludingId && playlist.name.compareIgnoreCase (candidate) == 0)
                    return true;
            return false;
        };

        if (! isTaken (base))
            return base;

        for (int suffix = 2; suffix < 10000; ++suffix)
        {
            const auto candidate = base + " (" + juce::String (suffix) + ")";
            if (! isTaken (candidate))
                return candidate;
        }

        return base + " (" + juce::String ((juce::int64) juce::Time::currentTimeMillis()) + ")";
    }

    static juce::String smartDefinitionJson (const SmartPlaylistDefinition& definition)
    {
        return juce::JSON::toString (serialize::toVar (definition), true);
    }

    bool insertPlaylist (Playlist& playlist, juce::String& errorOut)
    {
        auto statement = database.db().prepare (
            "INSERT INTO playlists (name, smart, match_all, sort_column, sort_desc, limit_count, "
            "smart_json, created, modified) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");

        if (statement == nullptr || ! statement->isValid())
        {
            errorOut = playlistFriendlyError ("Could not create a playlist", database.db().lastError());
            return false;
        }

        statement->bind (1, playlist.name)
                 .bind (2, playlist.smart)
                 .bind (3, playlist.smartDef.matchAll)
                 .bind (4, playlist.smartDef.sortColumn)
                 .bind (5, playlist.smartDef.sortDescending)
                 .bind (6, playlist.smartDef.limit)
                 .bind (7, playlist.smart ? smartDefinitionJson (playlist.smartDef) : juce::String())
                 .bind (8, playlist.createdMs)
                 .bind (9, playlist.modifiedMs);

        if (! statement->exec())
        {
            errorOut = playlistFriendlyError ("Could not create playlist \"" + playlist.name + "\"",
                                              statement->getError());
            return false;
        }

        playlist.id = database.db().lastInsertRowid();
        return playlist.id != kInvalidPlaylistId;
    }

    bool updatePlaylist (const Playlist& playlist, juce::String& errorOut)
    {
        auto statement = database.db().prepare (
            "UPDATE playlists SET name = ?, smart = ?, match_all = ?, sort_column = ?, sort_desc = ?, "
            "limit_count = ?, smart_json = ?, created = ?, modified = ? WHERE id = ?");

        if (statement == nullptr || ! statement->isValid())
        {
            errorOut = playlistFriendlyError ("Could not update the playlist", database.db().lastError());
            return false;
        }

        statement->bind (1, playlist.name)
                 .bind (2, playlist.smart)
                 .bind (3, playlist.smartDef.matchAll)
                 .bind (4, playlist.smartDef.sortColumn)
                 .bind (5, playlist.smartDef.sortDescending)
                 .bind (6, playlist.smartDef.limit)
                 .bind (7, playlist.smart ? smartDefinitionJson (playlist.smartDef) : juce::String())
                 .bind (8, playlist.createdMs)
                 .bind (9, playlist.modifiedMs)
                 .bind (10, playlist.id);

        if (! statement->exec())
        {
            errorOut = playlistFriendlyError ("Could not update playlist \"" + playlist.name + "\"",
                                              statement->getError());
            return false;
        }

        if (database.db().lastChanges() == 0)
        {
            errorOut = "Playlist no longer exists in the database";
            return false;
        }

        return true;
    }

    bool touchPlaylist (PlaylistId id, std::int64_t modifiedMs, juce::String& errorOut)
    {
        auto statement = database.db().prepare ("UPDATE playlists SET modified = ? WHERE id = ?");
        if (statement == nullptr || ! statement->isValid())
        {
            errorOut = playlistFriendlyError ("Could not update the playlist timestamp",
                                              database.db().lastError());
            return false;
        }

        statement->bind (1, modifiedMs).bind (2, id);
        if (! statement->exec())
        {
            errorOut = playlistFriendlyError ("Could not update the playlist timestamp",
                                              statement->getError());
            return false;
        }

        if (database.db().lastChanges() == 0)
        {
            errorOut = "Playlist no longer exists in the database";
            return false;
        }

        return true;
    }

    bool writeItems (PlaylistId playlistId, const std::vector<TrackId>& trackIds, juce::String& errorOut)
    {
        auto remove = database.db().prepare ("DELETE FROM playlist_items WHERE playlist_id = ?");
        if (remove == nullptr || ! remove->isValid())
        {
            errorOut = playlistFriendlyError ("Could not replace playlist items", database.db().lastError());
            return false;
        }

        remove->bind (1, playlistId);
        if (! remove->exec())
        {
            errorOut = playlistFriendlyError ("Could not clear playlist items", remove->getError());
            return false;
        }

        if (trackIds.empty())
            return true;

        auto insert = database.db().prepare (
            "INSERT INTO playlist_items (playlist_id, position, track_id) VALUES (?, ?, ?)");
        if (insert == nullptr || ! insert->isValid())
        {
            errorOut = playlistFriendlyError ("Could not write playlist items", database.db().lastError());
            return false;
        }

        for (size_t i = 0; i < trackIds.size(); ++i)
        {
            insert->reset();
            insert->bind (1, playlistId)
                    .bind (2, (std::int64_t) i)
                    .bind (3, trackIds[i]);

            if (! insert->exec())
            {
                errorOut = playlistFriendlyError ("Could not write playlist item "
                                                      + juce::String ((int) i),
                                                  insert->getError());
                return false;
            }
        }

        return true;
    }

    bool deletePlaylistRows (PlaylistId id, juce::String& errorOut)
    {
        auto removeItems = database.db().prepare ("DELETE FROM playlist_items WHERE playlist_id = ?");
        if (removeItems == nullptr || ! removeItems->isValid())
        {
            errorOut = playlistFriendlyError ("Could not delete playlist items", database.db().lastError());
            return false;
        }

        removeItems->bind (1, id);
        if (! removeItems->exec())
        {
            errorOut = playlistFriendlyError ("Could not delete playlist items", removeItems->getError());
            return false;
        }

        auto removePlaylist = database.db().prepare ("DELETE FROM playlists WHERE id = ?");
        if (removePlaylist == nullptr || ! removePlaylist->isValid())
        {
            errorOut = playlistFriendlyError ("Could not delete the playlist", database.db().lastError());
            return false;
        }

        removePlaylist->bind (1, id);
        if (! removePlaylist->exec())
        {
            errorOut = playlistFriendlyError ("Could not delete the playlist", removePlaylist->getError());
            return false;
        }

        if (database.db().lastChanges() == 0)
        {
            errorOut = "Playlist no longer exists in the database";
            return false;
        }

        return true;
    }
};

//==============================================================================
PlaylistManager::PlaylistManager (LibraryDatabase& database, LibraryService& library)
    : impl (std::make_unique<Impl> (*this, database, library))
{
}

PlaylistManager::~PlaylistManager() = default;

juce::String PlaylistManager::initialise()
{
    if (! impl->database.isOpen())
    {
        const auto error = "Library database is not open";
        impl->setError (error);
        return error;
    }

    std::vector<Playlist> playlists;
    juce::String error;

    {
        const juce::ScopedLock sl (impl->database.lock());

        if (! impl->database.beginTransaction())
        {
            error = playlistFriendlyError ("Could not begin playlist schema transaction",
                                           impl->database.db().lastError());
        }
        else
        {
            if (! impl->ensureSchema (error))
            {
                impl->database.rollbackTransaction();
            }
            else if (! impl->loadAll (playlists, error))
            {
                impl->database.rollbackTransaction();
            }
            else if (! impl->database.commitTransaction())
            {
                error = playlistFriendlyError ("Could not commit playlist schema",
                                               impl->database.db().lastError());
                impl->database.rollbackTransaction();
            }
        }
    }

    if (error.isNotEmpty())
    {
        impl->setError (error);
        return error;
    }

    impl->initialised = true;
    impl->publish (std::move (playlists), false);
    return {};
}

void PlaylistManager::shutdown()
{
    impl->initialised = false;
}

bool PlaylistManager::isInitialised() const { return impl->initialised; }

juce::String PlaylistManager::getLastError() const { return impl->getError(); }

std::shared_ptr<const PlaylistSnapshot> PlaylistManager::getSnapshot() const { return impl->current(); }

//==============================================================================
PlaylistId PlaylistManager::createPlaylist (const juce::String& name, bool smart)
{
    if (! impl->initialised)
    {
        impl->setError ("Playlist manager is not initialised");
        return kInvalidPlaylistId;
    }

    PlaylistId created = kInvalidPlaylistId;
    juce::String error;

    const bool ok = impl->runInTransaction ([this, &name, smart, &created] (juce::String& workError) -> bool
    {
        Playlist playlist;
        playlist.name = impl->uniqueName (name, kInvalidPlaylistId);
        playlist.smart = smart;
        playlist.createdMs = playlist.modifiedMs = juce::Time::currentTimeMillis();
        if (smart)
            playlist.smartDef = SmartPlaylistDefinition();

        if (! impl->insertPlaylist (playlist, workError))
            return false;

        created = playlist.id;
        return true;
    }, error);

    if (! ok)
    {
        impl->setError (error);
        return kInvalidPlaylistId;
    }

    if (! impl->reloadAndPublish())
        return kInvalidPlaylistId;

    return created;
}

bool PlaylistManager::renamePlaylist (PlaylistId id, const juce::String& newName)
{
    if (! impl->initialised)
    {
        impl->setError ("Playlist manager is not initialised");
        return false;
    }

    const auto playlistPtr = getSnapshot()->find (id);
    if (playlistPtr == nullptr)
    {
        impl->setError ("Playlist not found (it may have been deleted)");
        return false;
    }

    const auto trimmed = newName.trim();
    if (trimmed.isEmpty())
    {
        impl->setError ("Playlist name cannot be empty");
        return false;
    }

    const Playlist playlist = *playlistPtr;
    const auto unique = impl->uniqueName (trimmed, id);
    if (unique == playlist.name)
        return true;

    juce::String error;
    const bool ok = impl->runInTransaction ([this, &playlist, &unique] (juce::String& workError) -> bool
    {
        Playlist updated = playlist;
        updated.name = unique;
        updated.modifiedMs = juce::Time::currentTimeMillis();
        return impl->updatePlaylist (updated, workError);
    }, error);

    if (! ok)
    {
        impl->setError (error);
        return false;
    }

    return impl->reloadAndPublish();
}

bool PlaylistManager::deletePlaylist (PlaylistId id)
{
    if (! impl->initialised)
    {
        impl->setError ("Playlist manager is not initialised");
        return false;
    }

    if (getSnapshot()->find (id) == nullptr)
    {
        impl->setError ("Playlist not found (it may already have been deleted)");
        return false;
    }

    juce::String error;
    const bool ok = impl->runInTransaction ([this, id] (juce::String& workError) -> bool
    {
        return impl->deletePlaylistRows (id, workError);
    }, error);

    if (! ok)
    {
        impl->setError (error);
        return false;
    }

    return impl->reloadAndPublish();
}

PlaylistId PlaylistManager::duplicatePlaylist (PlaylistId id)
{
    if (! impl->initialised)
    {
        impl->setError ("Playlist manager is not initialised");
        return kInvalidPlaylistId;
    }

    const auto sourcePtr = getSnapshot()->find (id);
    if (sourcePtr == nullptr)
    {
        impl->setError ("Playlist not found (it may have been deleted)");
        return kInvalidPlaylistId;
    }

    const Playlist source = *sourcePtr;
    PlaylistId created = kInvalidPlaylistId;
    juce::String error;

    const bool ok = impl->runInTransaction ([this, &source, &created] (juce::String& workError) -> bool
    {
        Playlist copy = source;
        copy.id = kInvalidPlaylistId;
        copy.name = impl->uniqueName (source.name + " copy", kInvalidPlaylistId);
        copy.createdMs = copy.modifiedMs = juce::Time::currentTimeMillis();

        if (! impl->insertPlaylist (copy, workError))
            return false;

        if (! source.smart && ! impl->writeItems (copy.id, source.trackIds, workError))
            return false;

        created = copy.id;
        return true;
    }, error);

    if (! ok)
    {
        impl->setError (error);
        return kInvalidPlaylistId;
    }

    if (! impl->reloadAndPublish())
        return kInvalidPlaylistId;

    return created;
}

//==============================================================================
bool PlaylistManager::addTracks (PlaylistId id, const std::vector<TrackId>& trackIds, int insertIndex)
{
    if (! impl->initialised)
    {
        impl->setError ("Playlist manager is not initialised");
        return false;
    }

    const auto playlistPtr = getSnapshot()->find (id);
    if (playlistPtr == nullptr)
    {
        impl->setError ("Playlist not found (it may have been deleted)");
        return false;
    }

    if (playlistPtr->smart)
    {
        impl->setError ("Cannot add tracks to a smart playlist; edit its rules instead");
        return false;
    }

    std::vector<TrackId> newIds;
    newIds.reserve (trackIds.size());
    for (const auto trackId : trackIds)
        if (trackId > kInvalidTrackId)
            newIds.push_back (trackId);

    if (newIds.empty())
        return true;    // nothing to do; not an error

    auto updated = playlistPtr->trackIds;
    if (insertIndex < 0 || insertIndex >= (int) updated.size())
    {
        updated.insert (updated.end(), newIds.begin(), newIds.end());
    }
    else
    {
        updated.insert (updated.begin() + insertIndex, newIds.begin(), newIds.end());
    }

    juce::String error;
    const bool ok = impl->runInTransaction ([this, id, &updated] (juce::String& workError) -> bool
    {
        if (! impl->writeItems (id, updated, workError))
            return false;
        return impl->touchPlaylist (id, juce::Time::currentTimeMillis(), workError);
    }, error);

    if (! ok)
    {
        impl->setError (error);
        return false;
    }

    return impl->reloadAndPublish();
}

bool PlaylistManager::removeTrackAt (PlaylistId id, int index)
{
    if (! impl->initialised)
    {
        impl->setError ("Playlist manager is not initialised");
        return false;
    }

    const auto playlistPtr = getSnapshot()->find (id);
    if (playlistPtr == nullptr)
    {
        impl->setError ("Playlist not found (it may have been deleted)");
        return false;
    }

    if (playlistPtr->smart)
    {
        impl->setError ("Cannot remove tracks from a smart playlist; edit its rules instead");
        return false;
    }

    const auto& currentIds = playlistPtr->trackIds;
    if (index < 0 || index >= (int) currentIds.size())
    {
        impl->setError ("Track index " + juce::String (index) + " is out of range ("
                        + juce::String ((int) currentIds.size()) + " track(s))");
        return false;
    }

    auto updated = currentIds;
    updated.erase (updated.begin() + index);

    juce::String error;
    const bool ok = impl->runInTransaction ([this, id, &updated] (juce::String& workError) -> bool
    {
        if (! impl->writeItems (id, updated, workError))
            return false;
        return impl->touchPlaylist (id, juce::Time::currentTimeMillis(), workError);
    }, error);

    if (! ok)
    {
        impl->setError (error);
        return false;
    }

    return impl->reloadAndPublish();
}

bool PlaylistManager::moveTrack (PlaylistId id, int fromIndex, int toIndex)
{
    if (! impl->initialised)
    {
        impl->setError ("Playlist manager is not initialised");
        return false;
    }

    const auto playlistPtr = getSnapshot()->find (id);
    if (playlistPtr == nullptr)
    {
        impl->setError ("Playlist not found (it may have been deleted)");
        return false;
    }

    if (playlistPtr->smart)
    {
        impl->setError ("Smart playlists are ordered by their rules and cannot be reordered");
        return false;
    }

    const auto& currentIds = playlistPtr->trackIds;
    const int count = (int) currentIds.size();

    if (fromIndex < 0 || fromIndex >= count)
    {
        impl->setError ("Source index " + juce::String (fromIndex) + " is out of range ("
                        + juce::String (count) + " track(s))");
        return false;
    }

    if (toIndex < 0 || toIndex >= count)
    {
        impl->setError ("Destination index " + juce::String (toIndex) + " is out of range ("
                        + juce::String (count) + " track(s))");
        return false;
    }

    if (fromIndex == toIndex)
        return true;

    auto updated = currentIds;
    const auto moved = updated[(size_t) fromIndex];
    updated.erase (updated.begin() + fromIndex);

    const int clamped = juce::jlimit (0, (int) updated.size(), toIndex);
    updated.insert (updated.begin() + clamped, moved);

    juce::String error;
    const bool ok = impl->runInTransaction ([this, id, &updated] (juce::String& workError) -> bool
    {
        if (! impl->writeItems (id, updated, workError))
            return false;
        return impl->touchPlaylist (id, juce::Time::currentTimeMillis(), workError);
    }, error);

    if (! ok)
    {
        impl->setError (error);
        return false;
    }

    return impl->reloadAndPublish();
}

bool PlaylistManager::clearPlaylist (PlaylistId id)
{
    if (! impl->initialised)
    {
        impl->setError ("Playlist manager is not initialised");
        return false;
    }

    const auto playlistPtr = getSnapshot()->find (id);
    if (playlistPtr == nullptr)
    {
        impl->setError ("Playlist not found (it may have been deleted)");
        return false;
    }

    if (playlistPtr->smart)
    {
        impl->setError ("A smart playlist has no manual tracks; edit its rules instead");
        return false;
    }

    if (playlistPtr->trackIds.empty())
        return true;

    juce::String error;
    const bool ok = impl->runInTransaction ([this, id] (juce::String& workError) -> bool
    {
        if (! impl->writeItems (id, {}, workError))
            return false;
        return impl->touchPlaylist (id, juce::Time::currentTimeMillis(), workError);
    }, error);

    if (! ok)
    {
        impl->setError (error);
        return false;
    }

    return impl->reloadAndPublish();
}

//==============================================================================
bool PlaylistManager::setSmartDefinition (PlaylistId id, const SmartPlaylistDefinition& definition)
{
    if (! impl->initialised)
    {
        impl->setError ("Playlist manager is not initialised");
        return false;
    }

    const auto playlistPtr = getSnapshot()->find (id);
    if (playlistPtr == nullptr)
    {
        impl->setError ("Playlist not found (it may have been deleted)");
        return false;
    }

    if (! playlistPtr->smart)
    {
        impl->setError ("Playlist \"" + playlistPtr->name + "\" is a manual playlist; create a smart playlist to use rules");
        return false;
    }

    SmartPlaylistDefinition sanitised = definition;
    if (sanitised.sortColumn.trim().isEmpty())
        sanitised.sortColumn = "artist";
    if (sanitised.limit < 0)
        sanitised.limit = 0;

    juce::String error;
    const bool ok = impl->runInTransaction ([this, id, &sanitised] (juce::String& workError) -> bool
    {
        Playlist updated;
        {
            const auto snapshot = getSnapshot();
            const auto* current = snapshot->find (id);
            if (current == nullptr)
            {
                workError = "Playlist no longer exists";
                return false;
            }
            updated = *current;
        }

        updated.smartDef = sanitised;
        updated.modifiedMs = juce::Time::currentTimeMillis();

        if (! impl->writeItems (id, {}, workError))
            return false;

        return impl->updatePlaylist (updated, workError);
    }, error);

    if (! ok)
    {
        impl->setError (error);
        return false;
    }

    return impl->reloadAndPublish();
}

std::vector<TrackId> PlaylistManager::resolveTrackIds (PlaylistId id) const
{
    const auto snapshot = getSnapshot();
    const auto* playlist = snapshot->find (id);
    if (playlist == nullptr)
        return {};

    if (! playlist->smart)
        return playlist->trackIds;

    const auto librarySnapshot = impl->library.getSnapshot();
    std::vector<const Track*> selected;
    smartplay::select (librarySnapshot->tracks, playlist->smartDef, selected);

    std::vector<TrackId> ids;
    ids.reserve (selected.size());
    for (const auto* track : selected)
        ids.push_back (track->id);

    return ids;
}

std::vector<Track> PlaylistManager::resolveTracks (PlaylistId id) const
{
    const auto snapshot = getSnapshot();
    const auto* playlist = snapshot->find (id);
    if (playlist == nullptr)
        return {};

    const auto librarySnapshot = impl->library.getSnapshot();

    if (playlist->smart)
        return smartplay::apply (librarySnapshot->tracks, playlist->smartDef);

    std::unordered_map<TrackId, const Track*> tracksById;
    tracksById.reserve (librarySnapshot->tracks.size());
    for (const auto& track : librarySnapshot->tracks)
        tracksById[track.id] = &track;

    std::vector<Track> tracks;
    tracks.reserve (playlist->trackIds.size());
    for (const auto trackId : playlist->trackIds)
    {
        const auto found = tracksById.find (trackId);
        if (found != tracksById.end())
            tracks.push_back (*found->second);
    }

    return tracks;
}

bool PlaylistManager::trackMatchesRules (const Track& track, const SmartPlaylistDefinition& definition)
{
    return smartplay::matches (track, definition);
}

std::vector<Track> PlaylistManager::applySmartDefinition (const std::vector<Track>& libraryTracks,
                                                          const SmartPlaylistDefinition& definition)
{
    return smartplay::apply (libraryTracks, definition);
}

const char* PlaylistManager::smartFieldName (SmartField field)
{
    return smartplay::fieldName (field);
}

const char* PlaylistManager::smartOpName (SmartOp op)
{
    switch (op)
    {
        case SmartOp::is:          return "is";
        case SmartOp::isNot:       return "isNot";
        case SmartOp::contains:    return "contains";
        case SmartOp::notContains: return "notContains";
        case SmartOp::beginsWith:  return "beginsWith";
        case SmartOp::endsWith:    return "endsWith";
        case SmartOp::greaterThan: return "greaterThan";
        case SmartOp::lessThan:    return "lessThan";
        case SmartOp::between:     return "between";
        case SmartOp::inList:      return "inList";
    }

    return "is";
}

//==============================================================================
bool PlaylistManager::importPlaylistFile (const juce::File& file, juce::String& errorOut,
                                          PlaylistId* newPlaylistId, PlaylistImportResult* resultOut)
{
    errorOut.clear();
    if (resultOut != nullptr)
        *resultOut = PlaylistImportResult();

    if (! impl->initialised)
    {
        errorOut = "Playlist manager is not initialised";
        impl->setError (errorOut);
        return false;
    }

    playlistio::Document document;
    juce::String parseError;
    if (! playlistio::parse (file, document, parseError))
    {
        errorOut = parseError;
        impl->setError (errorOut);
        return false;
    }

    if (document.entries.empty())
    {
        errorOut = "Playlist file contains no entries: " + file.getFullPathName();
        impl->setError (errorOut);
        return false;
    }

    if ((int) document.entries.size() > kMaxImportedEntries)
    {
        errorOut = "Playlist file has too many entries (" + juce::String ((int) document.entries.size())
                   + "; limit " + juce::String (kMaxImportedEntries) + ")";
        impl->setError (errorOut);
        return false;
    }

    PlaylistImportResult result;
    result.format = playlistio::formatName (document.format);
    result.entriesRead = (int) document.entries.size();
    result.warnings = document.warnings;

    const auto librarySnapshot = impl->library.getSnapshot();
    const PlaylistPathResolver resolver (*librarySnapshot);
    const auto playlistDirectory = file.getParentDirectory();

    std::vector<TrackId> resolvedIds;
    resolvedIds.reserve (document.entries.size());

    for (const auto& entry : document.entries)
    {
        const auto match = resolver.resolve (entry.path, entry.lengthSeconds, playlistDirectory);
        if (match.resolved)
        {
            resolvedIds.push_back (match.trackId);
            ++result.resolved;
            continue;
        }

        ++result.unresolved;
        if (result.unresolvedEntries.size() < kMaxReportedUnresolvedEntries)
        {
            result.unresolvedEntries.add (entry.path);
            result.unresolvedReasons.add (match.reason);
        }
    }

    auto playlistName = document.title.trim();
    if (playlistName.isEmpty())
        playlistName = file.getFileNameWithoutExtension().trim();
    if (playlistName.isEmpty())
        playlistName = "Imported Playlist";

    const auto newId = createPlaylist (playlistName, false);
    if (newId == kInvalidPlaylistId)
    {
        errorOut = "Could not create a playlist for the import: " + impl->getError();
        return false;
    }

    if (! resolvedIds.empty() && ! addTracks (newId, resolvedIds))
    {
        const auto addError = impl->getError();
        deletePlaylist (newId);
        errorOut = "Could not add the imported tracks to the new playlist: " + addError;
        return false;
    }

    result.playlistId = newId;
    if (const auto* created = getSnapshot()->find (newId))
        result.playlistName = created->name;
    else
        result.playlistName = playlistName;

    if (newPlaylistId != nullptr)
        *newPlaylistId = newId;
    if (resultOut != nullptr)
        *resultOut = result;

    juce::Logger::writeToLog ("Playlist: " + result.message());
    return true;
}

bool PlaylistManager::exportPlaylist (PlaylistId id, const juce::File& file, juce::String& errorOut)
{
    errorOut.clear();

    if (! impl->initialised)
    {
        errorOut = "Playlist manager is not initialised";
        impl->setError (errorOut);
        return false;
    }

    const auto format = playlistio::detectFormat (file);
    if (format == playlistio::Format::unknown)
    {
        errorOut = "Unsupported playlist file extension \"" + file.getFileExtension()
                   + "\"; export as .m3u, .m3u8 or .pls.";
        impl->setError (errorOut);
        return false;
    }

    const auto snapshot = getSnapshot();
    const auto* playlist = snapshot->find (id);
    if (playlist == nullptr)
    {
        errorOut = "Playlist not found (it may have been deleted)";
        impl->setError (errorOut);
        return false;
    }

    const auto tracks = resolveTracks (id);
    if (tracks.empty())
    {
        errorOut = "Playlist \"" + playlist->name + "\" has no tracks that exist in the library; nothing to export.";
        impl->setError (errorOut);
        return false;
    }

    const auto directory = file.getParentDirectory();

    bool allTracksInsideDirectory = true;
    for (const auto& track : tracks)
    {
        if (! juce::File (track.path).isAChildOf (directory))
        {
            allTracksInsideDirectory = false;
            break;
        }
    }

    std::vector<playlistio::Entry> entries;
    entries.reserve (tracks.size());

    int missingFiles = 0;
    for (const auto& track : tracks)
    {
        if (track.path.containsAnyOf ("\r\n"))
        {
            errorOut = "Cannot export a playlist containing a track path with an embedded newline: " + track.path;
            impl->setError (errorOut);
            return false;
        }

        playlistio::Entry entry;
        if (allTracksInsideDirectory)
            entry.path = juce::File (track.path).getRelativePathFrom (directory);
        else
            entry.path = juce::File (track.path).getFullPathName();

        entry.title = track.displayArtist() + " - " + track.displayTitle();
        entry.lengthSeconds = track.durationMs > 0 ? track.durationMs / 1000.0 : -1.0;
        entries.push_back (std::move (entry));

        if (! juce::File (track.path).existsAsFile())
            ++missingFiles;
    }

    if (! playlist->smart)
    {
        const auto skipped = (int) playlist->trackIds.size() - (int) tracks.size();
        if (skipped > 0)
            juce::Logger::writeToLog ("Playlist export: skipped " + juce::String (skipped)
                                      + " entr" + (skipped == 1 ? "y" : "ies")
                                      + " whose track is no longer in the library.");
    }

    if (missingFiles > 0)
        juce::Logger::writeToLog ("Playlist export: " + juce::String (missingFiles)
                                  + " track file(s) are currently missing; their paths were still written.");

    juce::String writeError;
    if (! playlistio::write (file, entries, writeError))
    {
        errorOut = writeError;
        impl->setError (errorOut);
        return false;
    }

    juce::Logger::writeToLog ("Playlist export: wrote " + juce::String ((int) entries.size())
                              + " entry/entries to " + file.getFullPathName() + " ("
                              + playlistio::formatName (format) + ").");
    return true;
}

juce::StringArray PlaylistManager::importedExtensions() const
{
    return { "m3u", "m3u8", "pls" };
}

//==============================================================================
bool PlaylistManager::saveAll()
{
    if (! impl->initialised)
    {
        impl->setError ("Playlist manager is not initialised");
        return false;
    }

    const auto snapshot = getSnapshot();

    juce::String error;
    const bool ok = impl->runInTransaction ([this, &snapshot] (juce::String& workError) -> bool
    {
        // Remove playlists that no longer exist in the snapshot.
        auto existing = impl->database.db().prepare ("SELECT id FROM playlists");
        if (existing == nullptr || ! existing->isValid())
        {
            workError = playlistFriendlyError ("Could not read playlists for saving",
                                               impl->database.db().lastError());
            return false;
        }

        std::vector<PlaylistId> toRemove;
        while (existing->step())
        {
            const auto id = existing->getInt64 (0);
            if (snapshot->find (id) == nullptr)
                toRemove.push_back (id);
        }

        for (const auto id : toRemove)
            if (! impl->deletePlaylistRows (id, workError))
                return false;

        auto upsert = impl->database.db().prepare (
            "INSERT OR REPLACE INTO playlists (id, name, smart, match_all, sort_column, sort_desc, "
            "limit_count, smart_json, created, modified) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

        if (upsert == nullptr || ! upsert->isValid())
        {
            workError = playlistFriendlyError ("Could not write playlists", impl->database.db().lastError());
            return false;
        }

        for (const auto& playlist : snapshot->playlists)
        {
            upsert->reset();
            upsert->bind (1, playlist.id)
                   .bind (2, playlist.name)
                   .bind (3, playlist.smart)
                   .bind (4, playlist.smartDef.matchAll)
                   .bind (5, playlist.smartDef.sortColumn)
                   .bind (6, playlist.smartDef.sortDescending)
                   .bind (7, playlist.smartDef.limit)
                   .bind (8, playlist.smart ? Impl::smartDefinitionJson (playlist.smartDef) : juce::String())
                   .bind (9, playlist.createdMs)
                   .bind (10, playlist.modifiedMs);

            if (! upsert->exec())
            {
                workError = playlistFriendlyError ("Could not save playlist \"" + playlist.name + "\"",
                                                   upsert->getError());
                return false;
            }

            if (! impl->writeItems (playlist.id, playlist.smart ? std::vector<TrackId>() : playlist.trackIds,
                                    workError))
                return false;
        }

        return true;
    }, error);

    if (! ok)
    {
        impl->setError (error);
        return false;
    }

    return impl->reloadAndPublish();
}

void PlaylistManager::markChanged()
{
    sendChangeMessage();
}

} // namespace sp
