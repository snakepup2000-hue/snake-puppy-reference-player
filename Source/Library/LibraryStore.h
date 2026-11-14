/*
    Snake Puppy Reference Player
    Database row helpers shared by the library service and its scanner.

    All functions here take the library database lock internally (a
    juce::CriticalSection, which is re-entrant, so callers may hold it for a
    multi-statement transaction). They never scan the disk while holding it.

    This file is owned by the library engineer.
*/
#pragma once

#include "Core/Types.h"
#include "Library/LibraryDatabase.h"

#include <optional>
#include <vector>

namespace sp::librarystore
{

//== Tracks ===================================================================
/** Comma-separated SELECT list; column 0 is the row id, then all Track fields. */
juce::String trackColumns();

/** Builds a Track from a row that was selected with trackColumns(). */
Track trackFromStatement (const SqliteStatement& st);

/** All tracks ordered for display: artist, album, disc, track, path. */
std::vector<Track> loadAllTracks (LibraryDatabase& db);

std::optional<Track> findTrackByPath (LibraryDatabase& db, const juce::String& absolutePath);
std::optional<Track> findTrackById (LibraryDatabase& db, TrackId id);

/** Inserts a new row. Returns the new row id, or 0 on failure (errorOut if non-null). */
TrackId insertTrack (LibraryDatabase& db, const Track& track, juce::String* errorOut = nullptr);

/** Updates the scanner-owned fields (everything except the user-owned id, path,
    date added and play statistics). */
bool updateTrackFromScan (LibraryDatabase& db, const Track& track, juce::String* errorOut = nullptr);

/** Updates only the editable tag fields of an existing row. */
bool updateTrackTagFields (LibraryDatabase& db, const Track& track, juce::String* errorOut = nullptr);

bool updateRating (LibraryDatabase& db, TrackId id, int rating);
bool updatePlayStatistics (LibraryDatabase& db, TrackId id, int playCount, juce::int64 lastPlayedMs);
bool setMissing (LibraryDatabase& db, TrackId id, bool missing);
bool setContentHash (LibraryDatabase& db, TrackId id, const juce::String& hash);

/** Clears every cached content hash so it is recomputed with the current
    algorithm. Returns the number of rows that had a hash. */
int resetContentHashes (LibraryDatabase& db);

int deleteTracks (LibraryDatabase& db, const std::vector<TrackId>& ids);
int deleteTracksUnderFolder (LibraryDatabase& db, const juce::String& folderPath);
int setMissingUnderFolder (LibraryDatabase& db, const juce::String& folderPath, bool missing);

int countTracks (LibraryDatabase& db);

//== Locations ================================================================
std::vector<LibraryLocation> loadAllLocations (LibraryDatabase& db);

/** Returns the existing location id for the same path, or inserts a new row.
    Returns 0 on failure. */
LocationId ensureLocation (LibraryDatabase& db, const juce::String& path, bool recursive, bool watch,
                           juce::String* errorOut = nullptr);

bool updateLocationWatch (LibraryDatabase& db, LocationId id, bool watch);
bool updateLocationAfterScan (LibraryDatabase& db, LocationId id, bool online, juce::int64 lastScanMs);
bool deleteLocation (LibraryDatabase& db, LocationId id);

//== Analysis cache ===========================================================
bool saveAnalysisJson (LibraryDatabase& db, TrackId id, const juce::String& json, int version,
                       juce::int64 analyzedAtMs);
bool loadAnalysisJson (LibraryDatabase& db, TrackId id, juce::String& jsonOut, int& versionOut,
                       juce::int64& analyzedAtOut);

//== Meta =====================================================================
juce::String getMeta (LibraryDatabase& db, const juce::String& key, const juce::String& fallback = {});
bool setMeta (LibraryDatabase& db, const juce::String& key, const juce::String& value);

} // namespace sp::librarystore
