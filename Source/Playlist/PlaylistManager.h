/*
    Snake Puppy Reference Player
    Playlists: internal (SQLite) playlists, smart playlists, M3U/M3U8/PLS
    import/export, and manual ordering. Uses the library snapshot for smart
    playlist evaluation; never triggers a library rescan.

    Storage: this manager owns the `playlists` and `playlist_items` tables in the
    shared library database and creates them itself in initialise(). All
    multi-statement writes happen inside a single transaction while holding
    LibraryDatabase::lock(), so the library scanner and the playlist editor can
    share one SQLite connection safely.

    Threading:
      - Construction, initialise(), shutdown(), every mutator, import/export and
        saveAll() must be called from the message thread. Mutations are written
        through to SQLite immediately (so a crash or an AppContext shutdown that
        does not call saveAll() cannot lose edits); saveAll() rewrites the whole
        snapshot as a consistency flush.
      - One PlaylistManager should mutate a given database at a time (the
        message thread serialises it); getSnapshot() is safe from any thread.
      - getSnapshot() may be called from any thread; it returns an immutable
        shared_ptr that is replaced (never mutated) when playlists change.
      - resolveTracks()/resolveTrackIds() read the lock-free library snapshot and
        the current playlist snapshot; safe to call from any thread, but callers
        normally use the message thread.
      - A ChangeBroadcaster notification is sent (message thread, asynchronously)
        after every successful mutation.
*/
#pragma once

#include "Core/Types.h"
#include "Library/LibraryDatabase.h"
#include "Library/LibraryService.h"
#include <juce_data_structures/juce_data_structures.h>
#include <memory>
#include <vector>

namespace sp
{

struct PlaylistEntry
{
    TrackId trackId = kInvalidTrackId;
    int position = 0;
};

class PlaylistSnapshot
{
public:
    std::uint64_t version = 0;
    std::vector<Playlist> playlists;     // ordered by name (case-insensitive), then id

    const Playlist* find (PlaylistId id) const;
    int indexOf (PlaylistId id) const;
};

/** Detailed result of a playlist file import (for UI reporting). */
struct PlaylistImportResult
{
    PlaylistId playlistId = kInvalidPlaylistId;
    juce::String playlistName;
    juce::String format;                 // "M3U", "M3U8", "PLS"
    int entriesRead = 0;                 // entries found in the file
    int resolved = 0;                    // entries matched to library tracks
    int unresolved = 0;                  // entries that could not be matched
    juce::StringArray unresolvedEntries; // raw entries that were not resolved
    juce::StringArray unresolvedReasons; // parallel reason for each unresolved entry
    juce::StringArray warnings;          // parser warnings

    /** One-line human readable summary, e.g.
        "Imported \"Mix\" (M3U8): 8 of 10 entries matched the library (2 not found)." */
    juce::String message() const;
};

class PlaylistManager : public juce::ChangeBroadcaster
{
public:
    PlaylistManager (LibraryDatabase& database, LibraryService& library);
    ~PlaylistManager() override;

    /** Creates the playlist tables if needed and loads every playlist into a
        fresh snapshot. Returns an empty string on success. */
    juce::String initialise();
    void shutdown();

    bool isInitialised() const;
    /** Human-readable description of the last database/operation failure. */
    juce::String getLastError() const;

    std::shared_ptr<const PlaylistSnapshot> getSnapshot() const;

    //== Management ===========================================================
    /** Creates a manual or smart playlist. Empty/whitespace names become
        "New Playlist"; names are made unique case-insensitively by appending
        " (2)", " (3)", ... Returns kInvalidPlaylistId on failure. */
    PlaylistId createPlaylist (const juce::String& name, bool smart = false);
    /** Renames a playlist. The requested name is trimmed and made unique
        (excluding the playlist itself). Returns false for an invalid id or an
        empty name. */
    bool renamePlaylist (PlaylistId, const juce::String& newName);
    /** Deletes a playlist and all of its items (transactional cascade). */
    bool deletePlaylist (PlaylistId);
    /** Copies items and smart definition; the copy is named "<name> copy". */
    PlaylistId duplicatePlaylist (PlaylistId);

    //== Editing (manual playlists) ===========================================
    /** Appends or inserts tracks at `insertIndex` (-1 or out of range = append).
        Duplicate track ids are allowed (a playlist may contain a track more than
        once, e.g. for repeated cues); ids <= 0 are ignored. Ids are stored even
        if the track is not currently in the library - resolveTracks() skips such
        entries instead of failing. */
    bool addTracks (PlaylistId, const std::vector<TrackId>&, int insertIndex = -1);
    bool removeTrackAt (PlaylistId, int index);
    /** Moves the item at fromIndex so that it ends up at toIndex (clamped) in the
        resulting list. */
    bool moveTrack (PlaylistId, int fromIndex, int toIndex);
    bool clearPlaylist (PlaylistId);

    //== Smart playlists ======================================================
    /** Replaces the smart definition of a smart playlist (manual playlists must
        be converted by recreating them). */
    bool setSmartDefinition (PlaylistId, const SmartPlaylistDefinition&);
    /** For a smart playlist: evaluates the rules against the current library
        snapshot, sorts and applies the limit. For a manual playlist: returns the
        stored ids in playlist order (including ids not currently in the library;
        resolveTracks() filters those out). */
    std::vector<TrackId> resolveTrackIds (PlaylistId) const;
    /** Resolves to playable tracks. For manual playlists, ids whose track is not
        in the library are skipped (never crashes, never rescans). */
    std::vector<Track> resolveTracks (PlaylistId) const;
    static bool trackMatchesRules (const Track&, const SmartPlaylistDefinition&);
    /** Applies rules + sort + limit to an explicit track list. Pure; useful for
        UI previews and tests. */
    static std::vector<Track> applySmartDefinition (const std::vector<Track>& libraryTracks,
                                                    const SmartPlaylistDefinition&);
    static const char* smartFieldName (SmartField);
    static const char* smartOpName (SmartOp);

    //== Import / export ======================================================
    /** Detects .m3u/.m3u8/.pls; resolves entries against the library by
        absolute path, normalised path, unique filename and finally
        filename + duration. A new playlist is created named after the file (or
        the embedded #PLAYLIST title). Returns false only on a hard failure
        (missing/unreadable/unrecognised/empty file); unresolved entries are
        reported through `resultOut`, never silently dropped. */
    bool importPlaylistFile (const juce::File& file, juce::String& errorOut,
                             PlaylistId* newPlaylistId = nullptr,
                             PlaylistImportResult* resultOut = nullptr);

    /** Writes .m3u (path list), .m3u8 (#EXTM3U/#EXTINF, UTF-8) or .pls
        (NumberOfEntries/FileN/TitleN/LengthN) based on the target extension.
        Paths are written relative to the playlist's directory when every track
        is inside that directory tree, otherwise absolute - consistently for the
        whole file. Library tracks whose file is currently missing are still
        written (the path is valid metadata), but ids that no longer exist in the
        library are skipped. Returns false when there is nothing to export or the
        write fails. */
    bool exportPlaylist (PlaylistId, const juce::File& file, juce::String& errorOut);
    juce::StringArray importedExtensions() const;

    //== Persistence ==========================================================
    /** Rewrites every playlist and item from the current snapshot to the
        database in one transaction (mutations already persist immediately; this
        flushes/verifies the complete state). */
    bool saveAll();

    //== Notifications ========================================================
    void markChanged();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaylistManager)
};

} // namespace sp
