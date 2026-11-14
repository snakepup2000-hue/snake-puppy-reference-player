/*
    Snake Puppy Reference Player
    Music library: SQLite index, folder scanning, metadata + artwork indexing,
    folder watching, duplicate detection, missing-file tracking, analysis cache
    and a lock-free immutable snapshot for the UI.

    Threading:
      - All public mutators are safe to call from the message thread. Scanning
        happens on an internal background thread.
      - The UI reads `getSnapshot()` (an immutable shared_ptr copy). A new
        snapshot is published after each scan / metadata change, so the message
        thread never blocks on database writes.
      - Listeners are notified on the message thread.
*/
#pragma once

#include "Core/ApplicationPaths.h"
#include "Core/Types.h"
#include "Library/LibraryDatabase.h"
#include <juce_data_structures/juce_data_structures.h>

namespace sp
{

class LibrarySnapshot
{
public:
    std::uint64_t version = 0;
    std::vector<Track> tracks;
    std::vector<LibraryLocation> locations;
    juce::int64 builtMs = 0;

    const Track* findTrack (TrackId id) const;
    int indexOfTrack (TrackId id) const;
};

struct ScanProgress
{
    juce::String phase;           // "Scanning", "Reading metadata", "Artwork", "Hashing"
    juce::String currentFile;
    int filesFound = 0;
    int filesProcessed = 0;
    int filesTotal = 0;
};

class LibraryService
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void libraryChanged() {}
        virtual void libraryScanProgress (const ScanProgress&) {}
        virtual void libraryScanFinished (int added, int updated, int removed, int missing) {}
        virtual void libraryLocationsChanged() {}
        virtual void libraryError (const juce::String&) {}
        virtual void libraryMetadataWriteFinished (TrackId, bool success, const juce::String& error) {}
    };

    LibraryService (const ApplicationPaths& paths, LibraryDatabase& database);
    ~LibraryService();

    juce::String initialise();      // opens locations from DB + builds snapshot
    void shutdown();

    //== Snapshot =============================================================
    std::shared_ptr<const LibrarySnapshot> getSnapshot() const;

    //== Locations ============================================================
    std::vector<LibraryLocation> getLocations() const;
    /** Adds a folder location. When done, `callback` runs on the message thread
        with the new location id (0 on failure). */
    void addLocation (const juce::File& folder, bool watchForChanges,
                      std::function<void (LocationId)> callback = {});
    void removeLocation (LocationId, bool deleteTracksFromLocation);
    bool setLocationWatch (LocationId, bool watch);
    void setWatchEnabled (bool);

    //== Import ===============================================================
    void importFiles (const juce::StringArray& files);
    void importFolder (const juce::File& folder, bool addAsLocation);
    void rescanAll (bool refreshMetadata);
    void rescanFolder (const juce::File& folder);
    bool isScanning() const;

    //== Track mutation =========================================================
    /** Writes metadata tags to the file (audio samples untouched) and updates
        the library. Completion is reported asynchronously. */
    void updateMetadata (const Track& updated);
    void updateRating (TrackId, int rating);
    void incrementPlayCount (TrackId);
    void removeTracks (const std::vector<TrackId>&);
    void refreshMissingState();
    Track getTrack (TrackId) const;

    //== Queries / health =====================================================
    HealthReport runHealthCheck();
    std::vector<std::vector<TrackId>> findDuplicates (bool useContentHash);
    /** Compute and store the content hash for tracks that don't have one yet.
        Returns the number of tracks hashed. */
    int computeMissingHashes (int maxTracks = 0);

    //== Artwork ==============================================================
    /** Returns the cached artwork file for an artwork id, or a null File. */
    juce::File artworkFileFor (const juce::String& artworkId) const;
    /** Extracts embedded artwork now (message thread, fast) for a track. */
    juce::File artworkForTrack (const Track&) const;

    //== Analysis cache =======================================================
    bool getAnalysis (TrackId, TrackAnalysis& out) const;
    void saveAnalysis (TrackId, const TrackAnalysis&);

    //== Listener =============================================================
    void addListener (Listener*);
    void removeListener (Listener*);

    const LibraryDatabase& database() const { return db; }
    LibraryDatabase& database() { return db; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    LibraryDatabase& db;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryService)
};

} // namespace sp
