/*
    Snake Puppy Reference Player
    Music library service: background scanning, folder watching, immutable
    snapshots, health checks, duplicate detection, artwork cache access and the
    analysis cache.

    Threading model
    ---------------
    - One background scanner thread owns every file read and every scan write.
      Jobs (folder scans, explicit file imports, metadata writes) are queued and
      executed in FIFO order, so database writes never interleave.
    - The message thread may call mutators; they only enqueue work or perform a
      short, locked database statement.
    - Snapshots are immutable shared_ptr<const LibrarySnapshot> values. A new
      snapshot is published after every change; readers copy the shared_ptr.
    - Listener callbacks are always delivered through MessageManager::callAsync,
      never from the scanner thread.
    - The folder watcher is a message-thread Timer that polls directory mtimes
      (JUCE 8 no longer ships FileSystemWatcher); a change queues an incremental
      rescan of that location. Directory mtimes catch added/removed/renamed
      entries; in-place edits of an existing file are caught by rescanAll().
*/
#include "Library/LibraryService.h"

#include "Core/Serialization.h"
#include "Library/LibraryScanner.h"
#include "Library/LibraryStore.h"
#include "Library/Tags.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <vector>

namespace sp
{

namespace
{
constexpr int kProgressIntervalMs = 120;
constexpr int kScanWriteBatch = 64;
constexpr int kWatchPollIntervalMs = 3000;

/** True when `path` is the location folder or an entry below it. */
bool isUnderRoot (const juce::String& path, const juce::String& root)
{
    auto r = root;
    while (r.length() > 1 && r.endsWithChar ('/'))
        r = r.dropLastCharacters (1);

    if (r.isEmpty())
        return false;

    return path.equalsIgnoreCase (r) || path.startsWithIgnoreCase (r + "/");
}

juce::String normalisedRoot (const juce::File& f)
{
    auto p = f.getFullPathName();
    while (p.length() > 1 && p.endsWithChar ('/'))
        p = p.dropLastCharacters (1);
    return p;
}

struct ListenerHub
{
    std::atomic<bool> alive { true };
    juce::CriticalSection lock;
    std::list<LibraryService::Listener*> listeners;
};

struct ScanJob
{
    enum class Type { scanRoots, scanFiles, writeMetadata };

    Type type = Type::scanRoots;
    std::vector<juce::File> roots;      // folders: missing-file detection applies below them
    std::vector<juce::File> files;      // explicit files (imports)
    bool refreshMetadata = false;
    Track metadataTrack;
};

struct ScanStats
{
    int added = 0;
    int updated = 0;
    int missing = 0;
    int probeFailures = 0;
    int tagFailures = 0;
    int tagSuccess = 0;
    juce::String firstProbeError;
    juce::String firstTagError;
    juce::String writeError;
};

struct DuplicateGroup
{
    std::vector<TrackId> ids;
    juce::String reason;
    juce::int64 totalBytes = 0;
};

bool groupLarger (const DuplicateGroup& a, const DuplicateGroup& b)
{
    if (a.ids.size() != b.ids.size())
        return a.ids.size() > b.ids.size();
    return a.totalBytes > b.totalBytes;
}

} // namespace

//==============================================================================
struct LibraryService::Impl : private juce::Thread,
                              private juce::Timer
{
    Impl (const ApplicationPaths& p, LibraryDatabase& database)
        : juce::Thread ("Snake Puppy Library Scanner"),
          paths (p),
          db (database),
          hub (std::make_shared<ListenerHub>())
    {
        auto initial = std::make_shared<LibrarySnapshot>();
        initial->builtMs = juce::Time::currentTimeMillis();
        snapshot = std::move (initial);
    }

    ~Impl() override { shutdown(); }

    //== Infrastructure =======================================================
    void notify (std::function<void (LibraryService::Listener&)> fn) const
    {
        auto h = hub;
        if (h == nullptr || ! h->alive.load())
            return;
        if (juce::MessageManager::getInstanceWithoutCreating() == nullptr)
            return;

        juce::MessageManager::callAsync ([h, fn]() mutable
        {
            if (! h->alive.load())
                return;

            std::vector<LibraryService::Listener*> copy;
            {
                const juce::ScopedLock sl (h->lock);
                copy.assign (h->listeners.begin(), h->listeners.end());
            }
            for (auto* l : copy)
                if (l != nullptr)
                    fn (*l);
        });
    }

    void postCallback (std::function<void()> fn) const
    {
        auto h = hub;
        if (h == nullptr || ! h->alive.load())
            return;
        if (juce::MessageManager::getInstanceWithoutCreating() == nullptr)
            return;

        juce::MessageManager::callAsync ([h, fn]() mutable
        {
            if (h->alive.load())
                fn();
        });
    }

    void postProgress (ScanProgress progress, bool force)
    {
        const auto now = juce::Time::getMillisecondCounter();
        const auto last = lastProgressPostMs.load();
        if (! force && now - last < kProgressIntervalMs)
            return;
        lastProgressPostMs.store (now);

        notify ([progress] (LibraryService::Listener& l) { l.libraryScanProgress (progress); });
    }

    void postLibraryChanged() const { notify ([] (LibraryService::Listener& l) { l.libraryChanged(); }); }
    void postLocationsChanged() const { notify ([] (LibraryService::Listener& l) { l.libraryLocationsChanged(); }); }
    void postError (const juce::String& message) const
    {
        if (message.isNotEmpty())
            notify ([message] (LibraryService::Listener& l) { l.libraryError (message); });
    }
    void postScanFinished (const ScanStats& stats) const
    {
        const int added = stats.added, updated = stats.updated, missing = stats.missing;
        notify ([added, updated, missing] (LibraryService::Listener& l)
        {
            l.libraryScanFinished (added, updated, 0, missing);
        });
    }
    void postMetadataWriteFinished (TrackId id, bool success, const juce::String& error) const
    {
        notify ([id, success, error] (LibraryService::Listener& l)
        {
            l.libraryMetadataWriteFinished (id, success, error);
        });
    }

    //== Snapshots ============================================================
    std::shared_ptr<const LibrarySnapshot> currentSnapshot() const
    {
        const juce::ScopedLock sl (snapshotLock);
        return snapshot;
    }

    void publishSnapshot (std::shared_ptr<LibrarySnapshot> s)
    {
        const juce::ScopedLock sl (snapshotLock);
        s->version = ++versionCounter;
        s->builtMs = juce::Time::currentTimeMillis();
        snapshot = std::move (s);
    }

    static int countTracksUnderLocation (const std::vector<Track>& tracks, const LibraryLocation& location)
    {
        int count = 0;
        for (auto& t : tracks)
            if (isUnderRoot (t.folder, location.path))
                ++count;
        return count;
    }

    std::shared_ptr<LibrarySnapshot> buildSnapshotFromDb()
    {
        auto s = std::make_shared<LibrarySnapshot>();
        s->tracks = librarystore::loadAllTracks (db);
        s->locations = librarystore::loadAllLocations (db);
        for (auto& location : s->locations)
            location.trackCount = countTracksUnderLocation (s->tracks, location);
        s->builtMs = juce::Time::currentTimeMillis();
        return s;
    }

    void rebuildSnapshot (bool notifyLibraryChanged, bool notifyLocationsChanged = false)
    {
        publishSnapshot (buildSnapshotFromDb());
        if (notifyLibraryChanged)
            postLibraryChanged();
        if (notifyLocationsChanged)
            postLocationsChanged();
    }

    /** Applies a small in-place edit to one track and republishes a snapshot. */
    bool updateTrackInSnapshot (TrackId id, const std::function<void (Track&)>& edit)
    {
        bool found = false;
        {
            const juce::ScopedLock sl (snapshotLock);
            if (snapshot != nullptr)
            {
                auto copy = std::make_shared<LibrarySnapshot> (*snapshot);
                for (auto& t : copy->tracks)
                {
                    if (t.id == id)
                    {
                        edit (t);
                        found = true;
                        break;
                    }
                }

                if (found)
                {
                    copy->version = ++versionCounter;
                    copy->builtMs = juce::Time::currentTimeMillis();
                    snapshot = std::move (copy);
                }
            }
        }
        if (found)
            postLibraryChanged();
        return found;
    }

    //== Job queue ============================================================
    void enqueue (ScanJob job)
    {
        if (quitting.load())
            return;

        {
            const juce::ScopedLock sl (jobLock);
            jobs.push_back (std::move (job));
        }
        pendingJobs.fetch_add (1);
        jobReady.signal();
    }

    /** Starts the scanner thread (private-inherited Thread accessor). */
    bool startScanner() { return startThread(); }

    /** Destroys the scanner thread and drops queued jobs. Idempotent. */
    void shutdown()
    {
        if (shutdownDone)
            return;
        shutdownDone = true;

        quitting.store (true);
        jobReady.signal();

        stopTimer();

        if (isThreadRunning())
        {
            if (! stopThread (30000))
            {
                juce::Logger::writeToLog ("Library: scanner thread did not stop in 30 s; waiting for it.");
                stopThread (-1);
            }
        }
        threadIsRunning.store (false);

        {
            const juce::ScopedLock sl (jobLock);
            jobs.clear();
            pendingWatchRoots.clear();
        }
        pendingJobs.store (0);
        scanActive.store (false);

        if (auto h = hub)
            h->alive.store (false);
        hub.reset();
    }

    //== Thread ===============================================================
    void run() override
    {
        threadIsRunning.store (true);

        while (! threadShouldExit())
        {
            ScanJob job;
            bool haveJob = false;
            {
                const juce::ScopedLock sl (jobLock);
                if (! jobs.empty())
                {
                    job = std::move (jobs.front());
                    jobs.pop_front();
                    haveJob = true;
                }
            }

            if (! haveJob)
            {
                jobReady.wait (250);
                continue;
            }

            try
            {
                if (job.type == ScanJob::Type::writeMetadata)
                    runMetadataWrite (job.metadataTrack);
                else
                    runScan (job);
            }
            catch (const std::exception& e)
            {
                postError (juce::String ("Library background job failed: ") + e.what());
            }
            catch (...)
            {
                postError ("Library background job failed with an unknown error.");
            }

            pendingJobs.fetch_sub (1);
        }

        threadIsRunning.store (false);
    }

    //== Scanning =============================================================
    void runScan (const ScanJob& job)
    {
        scanActive.store (true);

        {
            const juce::ScopedLock sl (jobLock);
            for (auto& r : job.roots)
                pendingWatchRoots.erase (normalisedRoot (r).toStdString());
        }

        const auto startedMs = juce::Time::getMillisecondCounterHiRes();
        ScanStats stats;

        ScanProgress progress;
        progress.phase = "Scanning";
        postProgress (progress, true);

        //--- Collect the file list ---------------------------------------------
        std::vector<juce::File> files;
        std::set<std::string> seen;
        auto addFile = [&files, &seen] (const juce::File& f)
        {
            if (! f.existsAsFile() || ! scanner::isSupportedAudioFile (f))
                return;
            const auto path = f.getFullPathName();
            if (seen.insert (path.toStdString()).second)
                files.push_back (f);
        };

        for (auto& root : job.roots)
        {
            if (threadShouldExit())
                break;
            if (! root.isDirectory())
                continue;
            for (auto& f : root.findChildFiles (juce::File::findFiles, true, "*"))
                addFile (f);
        }

        for (auto& f : job.files)
            addFile (f);

        const int filesTotal = (int) files.size();
        progress.filesFound = filesTotal;
        progress.filesTotal = filesTotal;
        postProgress (progress, true);

        juce::Logger::writeToLog ("Library: scan starting (" + juce::String ((int) job.roots.size())
                                  + " folder(s), " + juce::String ((int) job.files.size())
                                  + " explicit file(s), " + juce::String (filesTotal) + " audio file(s))");

        //--- Existing rows (one read, then only stat comparisons) --------------
        std::map<std::string, Track> existingByPath;
        for (auto& t : librarystore::loadAllTracks (db))
            existingByPath[t.path.toStdString()] = t;

        struct PendingWrite { Track track; bool isNew; };
        std::vector<PendingWrite> pendingWrites;
        int processed = 0;
        bool writeFailed = false;
        bool aborted = false;

        auto flushWrites = [this, &pendingWrites, &stats, &writeFailed]
        {
            if (pendingWrites.empty())
                return;

            const juce::ScopedLock sl (db.lock());
            if (! db.beginTransaction())
            {
                stats.writeError = "Could not start a library write transaction: " + db.db().lastError();
                writeFailed = true;
                pendingWrites.clear();
                return;
            }

            bool ok = true;
            for (auto& pending : pendingWrites)
            {
                juce::String error;
                bool good = pending.isNew
                                ? librarystore::insertTrack (db, pending.track, &error) != kInvalidTrackId
                                : librarystore::updateTrackFromScan (db, pending.track, &error);

                if (! good)
                {
                    if (stats.writeError.isEmpty())
                        stats.writeError = error;
                    ok = false;
                    break;
                }

                if (pending.isNew)
                    ++stats.added;
                else
                    ++stats.updated;
            }

            if (ok)
            {
                if (! db.commitTransaction())
                {
                    ok = false;
                    stats.writeError = "Could not commit a library write transaction.";
                    db.rollbackTransaction();
                }
            }
            else
            {
                db.rollbackTransaction();
                writeFailed = true;
            }

            pendingWrites.clear();
        };

        for (int i = 0; i < filesTotal; ++i)
        {
            if (threadShouldExit())
            {
                aborted = true;
                juce::Logger::writeToLog ("Library: scan aborted by shutdown after "
                                          + juce::String (i) + " of " + juce::String (filesTotal) + " file(s)");
                break;
            }

            const auto& file = files[(size_t) i];
            const auto path = file.getFullPathName();
            seen.insert (path.toStdString());

            auto existingIt = existingByPath.find (path.toStdString());
            const Track* existing = existingIt != existingByPath.end() ? &existingIt->second : nullptr;

            const auto fileSize = file.getSize();
            const auto modifiedMs = file.getLastModificationTime().toMilliseconds();

            const bool needsRead = existing == nullptr
                                || job.refreshMetadata
                                || existing->fileSize != fileSize
                                || existing->fileModifiedMs != modifiedMs
                                || existing->sampleRate <= 0
                                || existing->durationMs <= 0;

            if (! needsRead)
            {
                if (existing->missing)
                {
                    if (librarystore::setMissing (db, existing->id, false))
                    {
                        existingIt->second.missing = false;
                        ++stats.updated;
                    }
                }

                progress.currentFile = path;
                progress.filesProcessed = processed;
                postProgress (progress, false);
                continue;
            }

            Track track;
            scanner::TrackFileInfo fileInfo;
            const auto readStartedMs = juce::Time::getMillisecondCounterHiRes();
            const bool readOk = scanner::buildTrackInfo (file, paths.artworkDir, true, track, fileInfo);
            const auto readElapsedMs = juce::Time::getMillisecondCounterHiRes() - readStartedMs;
            if (readElapsedMs > 250.0)
                juce::Logger::writeToLog ("Library: reading " + path + " took "
                                          + juce::String (readElapsedMs, 1) + " ms (slow file or cold start)");

            if (! readOk)
            {
                ++stats.probeFailures;
                if (stats.firstProbeError.isEmpty() && fileInfo.probeError.isNotEmpty())
                    stats.firstProbeError = fileInfo.probeError;
                juce::Logger::writeToLog ("Library: could not read " + path + " (" + fileInfo.probeError + ")");

                progress.currentFile = path;
                progress.filesProcessed = processed;
                postProgress (progress, false);
                continue;
            }

            ++processed;
            if (fileInfo.tagsRead)
            {
                ++stats.tagSuccess;
            }
            else
            {
                if (stats.firstTagError.isEmpty())
                    stats.firstTagError = fileInfo.tagError;
                ++stats.tagFailures;
            }

            if (existing != nullptr)
            {
                track.id = existing->id;
                track.dateAddedIso = existing->dateAddedIso;
                track.playCount = existing->playCount;
                track.lastPlayedMs = existing->lastPlayedMs;
                if (existing->rating > 0)
                    track.rating = existing->rating;

                const bool sameFile = existing->fileSize == track.fileSize
                                   && existing->fileModifiedMs == track.fileModifiedMs;
                if (sameFile)
                {
                    track.contentHash = existing->contentHash;
                    track.hashComputed = existing->hashComputed;
                }
                else
                {
                    // The bytes changed; a previously computed hash may now be stale.
                    track.contentHash.clear();
                    track.hashComputed = false;
                }

                track.missing = false;
                pendingWrites.push_back ({ track, false });
            }
            else
            {
                track.missing = false;
                pendingWrites.push_back ({ track, true });
            }

            if ((int) pendingWrites.size() >= kScanWriteBatch)
                flushWrites();

            if (writeFailed)
            {
                aborted = true;
                break;
            }

            progress.currentFile = path;
            progress.filesProcessed = processed;
            postProgress (progress, false);
        }

        flushWrites();

        //--- Missing-file detection -------------------------------------------
        if (! aborted && ! job.roots.empty())
        {
            for (auto& entry : existingByPath)
            {
                if (seen.count (entry.first) != 0)
                    continue;

                auto& track = entry.second;
                bool underRoot = false;
                for (auto& root : job.roots)
                    if (isUnderRoot (track.path, normalisedRoot (root)))
                    {
                        underRoot = true;
                        break;
                    }

                if (underRoot && ! track.missing)
                    if (librarystore::setMissing (db, track.id, true))
                    {
                        track.missing = true;
                        ++stats.missing;
                    }
            }
        }

        //--- Location online / last-scan state --------------------------------
        updateLocationStates (job.roots, juce::Time::currentTimeMillis());

        //--- Publish ----------------------------------------------------------
        auto newSnapshot = buildSnapshotFromDb();
        publishSnapshot (newSnapshot);

        ScanProgress finalProgress;
        finalProgress.phase = "Finished";
        finalProgress.filesFound = filesTotal;
        finalProgress.filesProcessed = processed;
        finalProgress.filesTotal = filesTotal;
        postProgress (finalProgress, true);

        postLibraryChanged();
        postLocationsChanged();
        postScanFinished (stats);

        if (aborted)
            postError (stats.writeError.isNotEmpty()
                           ? "Library scan aborted: " + stats.writeError
                           : juce::String ("Library scan was interrupted; the library keeps its existing rows."));

        if (stats.probeFailures > 0)
            postError (juce::String (stats.probeFailures) + " file(s) could not be read. First error: "
                       + stats.firstProbeError);

        if (stats.tagFailures > 0 && stats.tagSuccess == 0)
            postError (juce::String (stats.tagFailures)
                       + " file(s) could not be tagged, so titles/albums use filename fallbacks. First error: "
                       + stats.firstTagError);
        else if (stats.tagFailures > 0)
            juce::Logger::writeToLog ("Library: " + juce::String (stats.tagFailures)
                                      + " file(s) had unreadable tags (" + stats.firstTagError + ")");

        juce::Logger::writeToLog ("Library scan finished in "
                                  + juce::String (juce::Time::getMillisecondCounterHiRes() - startedMs, 1)
                                  + " ms: found " + juce::String (filesTotal)
                                  + ", added " + juce::String (stats.added)
                                  + ", updated " + juce::String (stats.updated)
                                  + ", marked missing " + juce::String (stats.missing)
                                  + ", unreadable " + juce::String (stats.probeFailures));

        scanActive.store (false);
    }

    void updateLocationStates (const std::vector<juce::File>& scannedRoots, juce::int64 scanTimeMs)
    {
        auto locations = librarystore::loadAllLocations (db);
        for (auto& location : locations)
        {
            const bool online = juce::File (location.path).isDirectory();
            juce::int64 lastScan = location.lastScanMs;
            for (auto& root : scannedRoots)
                if (location.path.equalsIgnoreCase (normalisedRoot (root)))
                {
                    lastScan = scanTimeMs;
                    break;
                }

            if (online != location.online || lastScan != location.lastScanMs)
                librarystore::updateLocationAfterScan (db, location.id, online, lastScan);
        }
    }

    //== Metadata writes ======================================================
    void runMetadataWrite (const Track& updated)
    {
        auto current = librarystore::findTrackById (db, updated.id);
        if (! current.has_value())
        {
            postMetadataWriteFinished (updated.id, false, "Track not found in the library.");
            return;
        }

        const auto file = juce::File (current->path);
        if (! file.existsAsFile())
        {
            const auto message = "File is missing: " + current->path;
            postMetadataWriteFinished (updated.id, false, message);
            postError (message);
            return;
        }

        tags::TagData tagsToWrite;
        tagsToWrite.title = updated.title;
        tagsToWrite.artist = updated.artist;
        tagsToWrite.albumArtist = updated.albumArtist;
        tagsToWrite.album = updated.album;
        tagsToWrite.genre = updated.genre;
        tagsToWrite.composer = updated.composer;
        tagsToWrite.comment = updated.comment;
        tagsToWrite.year = updated.year;
        tagsToWrite.trackNumber = updated.trackNumber;
        tagsToWrite.discNumber = updated.discNumber;
        tagsToWrite.rating = juce::jlimit (0, 5, updated.rating);

        juce::String writeError;
        if (! tags::writeTags (file, tagsToWrite, writeError))
        {
            const auto message = writeError.isNotEmpty() ? writeError
                                                         : juce::String ("Tag writing is not available.");
            postMetadataWriteFinished (updated.id, false, message);
            postError ("Could not write metadata for " + current->displayTitle() + ": " + message);
            return;
        }

        Track fresh = *current;

        // Re-read what actually landed in the file so the library shows the
        // truth rather than the requested values.
        tags::TagData reRead;
        juce::String reReadError;
        if (tags::readTags (file, reRead, reReadError) && reRead.readOk)
        {
            fresh.title = reRead.title;
            fresh.artist = reRead.artist;
            fresh.albumArtist = reRead.albumArtist;
            fresh.album = reRead.album;
            fresh.genre = reRead.genre;
            fresh.composer = reRead.composer;
            fresh.comment = reRead.comment;
            fresh.year = reRead.year;
            fresh.trackNumber = reRead.trackNumber;
            fresh.discNumber = reRead.discNumber;
            // Rating is library-owned state; keep the requested value unless the
            // tag layer actually round-tripped a rating.
            fresh.rating = reRead.rating > 0 ? juce::jlimit (0, 5, reRead.rating)
                                             : juce::jlimit (0, 5, updated.rating);
            fresh.replayGainTrackDb = reRead.replayGainTrackDb;
            fresh.hasReplayGainTrack = reRead.hasReplayGainTrack;
            fresh.replayGainAlbumDb = reRead.replayGainAlbumDb;
            fresh.hasReplayGainAlbum = reRead.hasReplayGainAlbum;
            fresh.peakLinear = reRead.peakLinear;
        }
        else
        {
            fresh.title = updated.title;
            fresh.artist = updated.artist;
            fresh.albumArtist = updated.albumArtist;
            fresh.album = updated.album;
            fresh.genre = updated.genre;
            fresh.composer = updated.composer;
            fresh.comment = updated.comment;
            fresh.year = updated.year;
            fresh.trackNumber = updated.trackNumber;
            fresh.discNumber = updated.discNumber;
            fresh.rating = juce::jlimit (0, 5, updated.rating);
        }

        fresh.fileSize = file.getSize();
        fresh.fileModifiedMs = file.getLastModificationTime().toMilliseconds();
        fresh.contentHash.clear();
        fresh.hashComputed = false;

        juce::String dbError;
        if (! librarystore::updateTrackFromScan (db, fresh, &dbError))
        {
            const auto message = "Tags were written but the library row could not be updated: " + dbError;
            postMetadataWriteFinished (updated.id, false, message);
            postError (message);
            return;
        }

        updateTrackInSnapshot (updated.id, [&fresh] (Track& t) { t = fresh; });
        postMetadataWriteFinished (updated.id, true, {});
    }

    //== Folder watching ======================================================
    static juce::String signatureForFolder (const juce::File& root)
    {
        std::vector<std::pair<std::string, juce::int64>> entries;
        entries.emplace_back (root.getFullPathName().toStdString(),
                              root.getLastModificationTime().toMilliseconds());

        for (auto& dir : root.findChildFiles (juce::File::findDirectories, true))
            entries.emplace_back (dir.getFullPathName().toStdString(),
                                  dir.getLastModificationTime().toMilliseconds());

        std::sort (entries.begin(), entries.end(),
                   [] (const auto& a, const auto& b) { return a.first < b.first; });

        std::uint64_t hash = 1469598103934665603ULL;
        auto mix = [&hash] (std::uint64_t value)
        {
            hash ^= value;
            hash *= 1099511628211ULL;
        };

        for (auto& entry : entries)
        {
            mix ((std::uint64_t) entry.second);
            mix ((std::uint64_t) juce::String (entry.first).hashCode64());
        }

        return juce::String::toHexString (hash) + "-" + juce::String ((int) entries.size());
    }

    void enqueueWatchScan (const juce::String& path)
    {
        {
            const juce::ScopedLock sl (jobLock);
            if (pendingWatchRoots.count (path.toStdString()) != 0)
                return;
            pendingWatchRoots.insert (path.toStdString());
        }

        ScanJob job;
        job.type = ScanJob::Type::scanRoots;
        job.roots.emplace_back (path);
        enqueue (std::move (job));
    }

    void refreshWatchSignatures()
    {
        if (! watchEnabled || quitting.load())
        {
            stopTimer();
            watchSignatures.clear();
            return;
        }

        auto locations = librarystore::loadAllLocations (db);
        std::map<std::string, juce::String> signatures;
        bool anyWatched = false;

        for (auto& location : locations)
        {
            if (! location.watch)
                continue;
            anyWatched = true;
            const juce::File folder (location.path);
            signatures[location.path.toStdString()] = folder.isDirectory() ? signatureForFolder (folder)
                                                                           : juce::String ("missing");
        }

        watchSignatures = std::move (signatures);

        if (anyWatched)
        {
            if (! isTimerRunning())
                startTimer (kWatchPollIntervalMs);
        }
        else
        {
            stopTimer();
        }
    }

    void timerCallback() override
    {
        if (! watchEnabled || quitting.load())
            return;

        auto locations = librarystore::loadAllLocations (db);
        std::map<std::string, juce::String> next;

        for (auto& location : locations)
        {
            if (! location.watch)
                continue;

            const juce::File folder (location.path);
            const auto signature = folder.isDirectory() ? signatureForFolder (folder)
                                                        : juce::String ("missing");
            const auto key = location.path.toStdString();
            next[key] = signature;

            auto previous = watchSignatures.find (key);
            if (previous == watchSignatures.end() || previous->second != signature)
                enqueueWatchScan (location.path);
        }

        watchSignatures = std::move (next);
    }

    //== Duplicate detection helpers ==========================================
    std::vector<DuplicateGroup> duplicateGroups (bool useContentHash)
    {
        auto tracks = librarystore::loadAllTracks (db);

        std::map<std::string, DuplicateGroup> groups;

        for (auto& t : tracks)
        {
            if (t.missing)
                continue;

            std::string key;
            juce::String reason;

            if (useContentHash)
            {
                if (! t.hashComputed || t.contentHash.isEmpty())
                    continue;
                key = "hash:" + t.contentHash.toStdString();
                reason = "Identical audio content (hash " + t.contentHash.substring (0, 12) + ")";
            }
            else
            {
                if (t.fileSize <= 0 || t.durationMs <= 0)
                    continue;
                key = "size:" + juce::String (t.fileSize).toStdString()
                    + ":" + juce::String (t.durationMs).toStdString()
                    + ":" + juce::String (t.sampleRate).toStdString()
                    + ":" + juce::String (t.numChannels).toStdString()
                    + ":" + t.formatName.toLowerCase().toStdString();
                reason = "Same file size, duration, format and channel count";
            }

            auto& group = groups[key];
            group.ids.push_back (t.id);
            group.totalBytes += t.fileSize;
            if (group.reason.isEmpty())
                group.reason = reason;
        }

        std::vector<DuplicateGroup> result;
        for (auto& entry : groups)
            if (entry.second.ids.size() > 1)
            {
                std::sort (entry.second.ids.begin(), entry.second.ids.end());
                result.push_back (std::move (entry.second));
            }

        std::sort (result.begin(), result.end(), groupLarger);
        return result;
    }

    //== Playlist health ======================================================
    void checkBrokenPlaylists (HealthReport& report)
    {
        const juce::ScopedLock sl (db.lock());
        if (! db.isOpen())
            return;

        std::vector<juce::String> tables;
        {
            auto st = db.db().prepare ("SELECT name FROM sqlite_master WHERE type='table'");
            if (st == nullptr)
                return;
            while (st->step())
                tables.push_back (st->getString (0));
        }

        juce::String playlistsTable;
        for (auto& name : tables)
            if (name.equalsIgnoreCase ("playlists") || name.equalsIgnoreCase ("playlist"))
                playlistsTable = name;

        if (playlistsTable.isEmpty())
            return;     // no playlist data yet: nothing to report, honestly

        auto tableColumns = [this] (const juce::String& table)
        {
            std::vector<juce::String> columns;
            auto st = db.db().prepare ("PRAGMA table_info(" + table + ")");
            if (st == nullptr)
                return columns;
            while (st->step())
                columns.push_back (st->getString (1).toLowerCase());
            return columns;
        };

        const auto playlistColumns = tableColumns (playlistsTable);
        auto hasColumn = [] (const std::vector<juce::String>& columns, const juce::String& name)
        {
            return std::find (columns.begin(), columns.end(), name) != columns.end();
        };

        if (! hasColumn (playlistColumns, "id") || ! hasColumn (playlistColumns, "name"))
        {
            report.issues.push_back ({ "warning",
                "Playlist table '" + playlistsTable + "' has an unexpected schema; the broken-playlist check was skipped." });
            return;
        }

        juce::String entriesTable;
        std::vector<juce::String> entryColumns;

        for (auto& name : tables)
        {
            if (name.equalsIgnoreCase (playlistsTable) || ! name.containsIgnoreCase ("playlist"))
                continue;

            const auto columns = tableColumns (name);
            if (hasColumn (columns, "track_id")
                && (hasColumn (columns, "playlist_id") || hasColumn (columns, "playlist")))
            {
                entriesTable = name;
                entryColumns = columns;
                break;
            }
        }

        if (entriesTable.isEmpty())
            return;     // playlists exist but have no entries table: no broken entries possible

        const auto playlistIdColumn = hasColumn (entryColumns, "playlist_id") ? juce::String ("playlist_id")
                                                                              : juce::String ("playlist");

        auto st = db.db().prepare ("SELECT p.id, p.name, COUNT(*) FROM " + entriesTable + " e "
                                   "JOIN " + playlistsTable + " p ON p.id = e." + playlistIdColumn + " "
                                   "LEFT JOIN tracks t ON t.id = e.track_id "
                                   "WHERE t.id IS NULL OR t.missing <> 0 "
                                   "GROUP BY p.id, p.name ORDER BY p.name COLLATE NOCASE");
        if (st == nullptr)
        {
            report.issues.push_back ({ "warning",
                "The playlist tables could not be queried for broken entries: " + db.db().lastError() });
            return;
        }

        while (st->step())
        {
            HealthReport::BrokenPlaylist broken;
            broken.id = st->getInt64 (0);
            broken.name = st->getString (1);
            broken.missingEntries = st->getInt (2);
            report.brokenPlaylists.push_back (broken);
        }
    }

    //== State ================================================================
    ApplicationPaths paths;
    LibraryDatabase& db;
    std::shared_ptr<ListenerHub> hub;

    mutable juce::CriticalSection snapshotLock;
    std::shared_ptr<const LibrarySnapshot> snapshot;
    std::uint64_t versionCounter = 0;

    juce::CriticalSection jobLock;
    std::deque<ScanJob> jobs;
    juce::WaitableEvent jobReady;
    std::set<std::string> pendingWatchRoots;

    std::atomic<bool> quitting { false };
    std::atomic<bool> scanActive { false };
    std::atomic<int> pendingJobs { 0 };
    std::atomic<bool> threadIsRunning { false };
    std::atomic<juce::int64> lastProgressPostMs { 0 };

    bool initialised = false;
    bool shutdownDone = false;

    // Message-thread only.
    bool watchEnabled = true;
    std::map<std::string, juce::String> watchSignatures;

    mutable juce::CriticalSection artworkLock;
    std::map<std::string, juce::File> artworkCache;
};

//==============================================================================
// LibrarySnapshot
//==============================================================================
const Track* LibrarySnapshot::findTrack (TrackId id) const
{
    for (auto& t : tracks)
        if (t.id == id)
            return &t;
    return nullptr;
}

int LibrarySnapshot::indexOfTrack (TrackId id) const
{
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].id == id)
            return (int) i;
    return -1;
}

//==============================================================================
LibraryService::LibraryService (const ApplicationPaths& paths, LibraryDatabase& database)
    : impl (std::make_unique<Impl> (paths, database)), db (database)
{
}

LibraryService::~LibraryService() = default;

juce::String LibraryService::initialise()
{
    if (impl->initialised)
        return {};

    if (! db.isOpen())
        return "Library database is not open.";

    impl->watchEnabled = librarystore::getMeta (db, "watch_enabled", "1") != "0";

    // Load locations; keep rows for offline drives but mark their tracks missing.
    auto locations = librarystore::loadAllLocations (db);
    std::vector<juce::File> startupRoots;

    for (auto& location : locations)
    {
        const juce::File folder (location.path);
        const bool online = folder.isDirectory();

        if (online != location.online)
            librarystore::updateLocationAfterScan (db, location.id, online, location.lastScanMs);

        if (! online)
        {
            const auto marked = librarystore::setMissingUnderFolder (db, location.path, true);
            if (marked > 0)
                juce::Logger::writeToLog ("Library: location offline (" + location.path + "), "
                                          + juce::String (marked) + " track(s) marked missing.");
        }
        else
        {
            startupRoots.push_back (folder);
        }
    }

    impl->publishSnapshot (impl->buildSnapshotFromDb());
    impl->refreshWatchSignatures();

    if (! impl->startScanner())
        return "Could not start the library scanner thread.";

    // Incremental rescan of available folders in the background; the snapshot
    // above already exposes the persisted library immediately.
    if (! startupRoots.empty())
    {
        ScanJob job;
        job.type = ScanJob::Type::scanRoots;
        job.roots = std::move (startupRoots);
        impl->enqueue (std::move (job));
    }

    impl->initialised = true;
    return {};
}

void LibraryService::shutdown()
{
    impl->shutdown();
}

std::shared_ptr<const LibrarySnapshot> LibraryService::getSnapshot() const
{
    return impl->currentSnapshot();
}

//==============================================================================
// Locations
//==============================================================================
std::vector<LibraryLocation> LibraryService::getLocations() const
{
    auto snapshot = impl->currentSnapshot();
    if (snapshot != nullptr && ! snapshot->locations.empty())
        return snapshot->locations;

    return librarystore::loadAllLocations (const_cast<LibraryDatabase&> (db));
}

void LibraryService::addLocation (const juce::File& folder, bool watchForChanges,
                                  std::function<void (LocationId)> callback)
{
    if (! folder.isDirectory())
    {
        const auto message = "Not a folder: " + folder.getFullPathName();
        impl->postError (message);
        if (callback)
            impl->postCallback ([callback] { callback (0); });
        return;
    }

    const auto path = normalisedRoot (folder);
    const bool watch = watchForChanges && impl->watchEnabled;

    juce::String error;
    const auto id = librarystore::ensureLocation (db, path, true, watch, &error);
    if (id == 0)
    {
        const auto message = "Could not add library location " + path + ": "
                           + (error.isNotEmpty() ? error : juce::String ("database error"));
        impl->postError (message);
        if (callback)
            impl->postCallback ([callback] { callback (0); });
        return;
    }

    // Make sure the location is online in the persisted row.
    for (auto& location : librarystore::loadAllLocations (db))
        if (location.id == id && ! location.online)
            librarystore::updateLocationAfterScan (db, id, true, location.lastScanMs);

    impl->rebuildSnapshot (true, true);
    impl->refreshWatchSignatures();

    ScanJob job;
    job.type = ScanJob::Type::scanRoots;
    job.roots.emplace_back (path);
    impl->enqueue (std::move (job));

    if (callback)
        impl->postCallback ([callback, id] { callback (id); });
}

void LibraryService::removeLocation (LocationId id, bool deleteTracksFromLocation)
{
    juce::String path;
    for (auto& location : librarystore::loadAllLocations (db))
        if (location.id == id)
            path = location.path;

    if (path.isEmpty())
    {
        impl->postError ("Library location not found.");
        return;
    }

    if (deleteTracksFromLocation)
    {
        const auto deleted = librarystore::deleteTracksUnderFolder (db, path);
        juce::Logger::writeToLog ("Library: removed location " + path + " and " + juce::String (deleted)
                                  + " track(s).");
    }

    librarystore::deleteLocation (db, id);
    impl->refreshWatchSignatures();
    impl->rebuildSnapshot (true, true);
}

bool LibraryService::setLocationWatch (LocationId id, bool watch)
{
    if (! librarystore::updateLocationWatch (db, id, watch))
    {
        impl->postError ("Could not change the watch state of that library location.");
        return false;
    }

    impl->refreshWatchSignatures();
    impl->rebuildSnapshot (false, true);
    return true;
}

void LibraryService::setWatchEnabled (bool enabled)
{
    impl->watchEnabled = enabled;
    librarystore::setMeta (db, "watch_enabled", enabled ? "1" : "0");
    impl->refreshWatchSignatures();
}

//==============================================================================
// Import
//==============================================================================
void LibraryService::importFiles (const juce::StringArray& files)
{
    std::vector<juce::File> supported;
    for (auto& path : files)
    {
        const juce::File file (path);
        if (file.existsAsFile() && scanner::isSupportedAudioFile (file))
            supported.push_back (file);
    }

    if (supported.empty())
    {
        impl->postError ("No supported audio files were selected for import.");
        return;
    }

    ScanJob job;
    job.type = ScanJob::Type::scanFiles;
    job.files = std::move (supported);
    impl->enqueue (std::move (job));
}

void LibraryService::importFolder (const juce::File& folder, bool addAsLocation)
{
    if (! folder.isDirectory())
    {
        impl->postError ("Not a folder: " + folder.getFullPathName());
        return;
    }

    if (addAsLocation)
    {
        addLocation (folder, true);
        return;
    }

    juce::StringArray files;
    for (auto& f : folder.findChildFiles (juce::File::findFiles, true, "*"))
        if (scanner::isSupportedAudioFile (f))
            files.add (f.getFullPathName());

    if (files.isEmpty())
    {
        impl->postError ("No supported audio files were found in " + folder.getFullPathName());
        return;
    }

    importFiles (files);
}

void LibraryService::rescanAll (bool refreshMetadata)
{
    auto locations = librarystore::loadAllLocations (db);

    ScanJob job;
    job.type = ScanJob::Type::scanRoots;
    job.refreshMetadata = refreshMetadata;

    for (auto& location : locations)
        job.roots.emplace_back (location.path);

    if (job.roots.empty())
    {
        impl->updateLocationStates ({}, juce::Time::currentTimeMillis());
        impl->rebuildSnapshot (true, true);
        ScanStats stats;
        impl->postScanFinished (stats);
        return;
    }

    impl->enqueue (std::move (job));
}

void LibraryService::rescanFolder (const juce::File& folder)
{
    ScanJob job;
    job.type = ScanJob::Type::scanRoots;
    job.roots.emplace_back (normalisedRoot (folder));
    impl->enqueue (std::move (job));
}

bool LibraryService::isScanning() const
{
    return impl->scanActive.load() || impl->pendingJobs.load() > 0;
}

//==============================================================================
// Track mutation
//==============================================================================
void LibraryService::updateMetadata (const Track& updated)
{
    if (updated.id == kInvalidTrackId)
    {
        impl->postError ("Cannot update metadata: invalid track id.");
        return;
    }

    ScanJob job;
    job.type = ScanJob::Type::writeMetadata;
    job.metadataTrack = updated;
    impl->enqueue (std::move (job));
}

void LibraryService::updateRating (TrackId id, int rating)
{
    const int clamped = juce::jlimit (0, 5, rating);

    if (! librarystore::updateRating (db, id, clamped))
    {
        impl->postError ("Could not save the rating.");
        return;
    }

    impl->updateTrackInSnapshot (id, [clamped] (Track& t) { t.rating = clamped; });
}

void LibraryService::incrementPlayCount (TrackId id)
{
    auto track = librarystore::findTrackById (db, id);
    if (! track.has_value())
        return;

    const int newCount = track->playCount + 1;
    const auto now = juce::Time::currentTimeMillis();

    if (! librarystore::updatePlayStatistics (db, id, newCount, now))
        return;

    impl->updateTrackInSnapshot (id, [newCount, now] (Track& t)
    {
        t.playCount = newCount;
        t.lastPlayedMs = now;
    });
}

void LibraryService::removeTracks (const std::vector<TrackId>& ids)
{
    if (ids.empty())
        return;

    if (librarystore::deleteTracks (db, ids) <= 0)
    {
        impl->postError ("No tracks were removed from the library.");
        return;
    }

    impl->rebuildSnapshot (true, true);
}

void LibraryService::refreshMissingState()
{
    const auto tracks = librarystore::loadAllTracks (db);
    std::vector<TrackId> nowMissing;
    std::vector<TrackId> nowPresent;

    for (auto& t : tracks)
    {
        const bool missing = ! juce::File (t.path).existsAsFile();
        if (missing != t.missing)
            (missing ? nowMissing : nowPresent).push_back (t.id);
    }

    bool locationsChanged = false;
    for (auto& location : librarystore::loadAllLocations (db))
    {
        const bool online = juce::File (location.path).isDirectory();
        if (online != location.online)
        {
            librarystore::updateLocationAfterScan (db, location.id, online, location.lastScanMs);
            locationsChanged = true;
        }
    }

    if (! nowMissing.empty() || ! nowPresent.empty())
    {
        const juce::ScopedLock sl (db.lock());
        if (db.beginTransaction())
        {
            for (auto id : nowMissing)
                librarystore::setMissing (db, id, true);
            for (auto id : nowPresent)
                librarystore::setMissing (db, id, false);

            if (! db.commitTransaction())
            {
                db.rollbackTransaction();
                impl->postError ("Could not save the missing-file state: " + db.db().lastError());
                return;
            }
        }
    }

    if (! nowMissing.empty() || ! nowPresent.empty() || locationsChanged)
        impl->rebuildSnapshot (true, locationsChanged);
}

Track LibraryService::getTrack (TrackId id) const
{
    auto track = librarystore::findTrackById (const_cast<LibraryDatabase&> (db), id);
    return track.has_value() ? *track : Track();
}

//==============================================================================
// Queries / health
//==============================================================================
HealthReport LibraryService::runHealthCheck()
{
    refreshMissingState();

    HealthReport report;
    report.generatedMs = juce::Time::currentTimeMillis();

    const auto tracks = librarystore::loadAllTracks (db);

    int hashedTracks = 0;
    for (auto& t : tracks)
    {
        if (t.missing)
        {
            HealthReport::MissingTrack missing;
            missing.id = t.id;
            missing.path = t.path;
            missing.title = t.displayTitle();
            report.missingTracks.push_back (missing);
        }
        if (t.hashComputed && t.contentHash.isNotEmpty())
            ++hashedTracks;
    }

    auto groups = impl->duplicateGroups (true);
    if (groups.empty() && hashedTracks == 0)
        groups = impl->duplicateGroups (false);

    for (auto& group : groups)
    {
        HealthReport::DuplicateGroup duplicate;
        duplicate.ids = group.ids;
        duplicate.reason = group.reason;
        report.duplicateGroups.push_back (std::move (duplicate));
    }

    impl->checkBrokenPlaylists (report);

    for (auto& location : librarystore::loadAllLocations (db))
        if (! juce::File (location.path).isDirectory())
            report.issues.push_back ({ "warning", "Library location is offline: " + location.path });

    if (hashedTracks < (int) tracks.size())
        report.issues.push_back ({ "info",
            juce::String ((int) tracks.size() - hashedTracks) + " of " + juce::String ((int) tracks.size())
            + " track(s) have no content hash yet; run a hash pass for complete duplicate detection." });

    return report;
}

std::vector<std::vector<TrackId>> LibraryService::findDuplicates (bool useContentHash)
{
    std::vector<std::vector<TrackId>> result;
    for (auto& group : impl->duplicateGroups (useContentHash))
        result.push_back (group.ids);
    return result;
}

int LibraryService::computeMissingHashes (int maxTracks)
{
    auto candidatesFrom = [this, maxTracks]()
    {
        std::vector<Track> candidates;
        for (auto& t : librarystore::loadAllTracks (db))
            if (! t.hashComputed)
                candidates.push_back (t);

        if (maxTracks > 0 && (int) candidates.size() > maxTracks)
            candidates.resize ((size_t) maxTracks);
        return candidates;
    };

    auto candidates = candidatesFrom();
    if (candidates.empty())
        return 0;

    // Probe the tag layer once to find out whether payload hashes are available.
    juce::String algorithm;
    scanner::contentHashForFile (juce::File (candidates.front().path), algorithm);

    if (algorithm.isNotEmpty())
    {
        const auto storedAlgorithm = librarystore::getMeta (db, "content_hash_algorithm");
        if (storedAlgorithm.isNotEmpty() && storedAlgorithm != algorithm)
        {
            const auto reset = librarystore::resetContentHashes (db);
            juce::Logger::writeToLog ("Library: content hash algorithm changed to '" + algorithm + "'; "
                                      + juce::String (reset) + " cached hash(es) invalidated.");
            librarystore::setMeta (db, "content_hash_algorithm", algorithm);
            candidates = candidatesFrom();
        }
        else if (storedAlgorithm.isEmpty())
        {
            librarystore::setMeta (db, "content_hash_algorithm", algorithm);
        }
    }

    const int total = (int) candidates.size();
    ScanProgress progress;
    progress.phase = "Hashing";
    progress.filesFound = total;
    progress.filesTotal = total;
    impl->postProgress (progress, true);

    int hashed = 0;
    int failed = 0;

    for (int i = 0; i < total; ++i)
    {
        if (impl->quitting.load())
            break;

        auto& track = candidates[(size_t) i];
        juce::String method;
        const auto hash = scanner::contentHashForFile (juce::File (track.path), method);

        if (hash.isNotEmpty() && librarystore::setContentHash (db, track.id, hash))
            ++hashed;
        else
            ++failed;

        progress.filesProcessed = i + 1;
        progress.currentFile = track.path;
        impl->postProgress (progress, false);
    }

    progress.filesProcessed = hashed + failed;
    progress.currentFile.clear();
    impl->postProgress (progress, true);

    if (hashed > 0)
        impl->rebuildSnapshot (true);
    if (failed > 0)
        impl->postError (juce::String (failed) + " file(s) could not be hashed for duplicate detection.");

    return hashed;
}

//==============================================================================
// Artwork
//==============================================================================
juce::File LibraryService::artworkFileFor (const juce::String& artworkId) const
{
    if (artworkId.isEmpty())
        return {};

    {
        const juce::ScopedLock sl (impl->artworkLock);
        auto found = impl->artworkCache.find (artworkId.toStdString());
        if (found != impl->artworkCache.end() && found->second.existsAsFile())
            return found->second;
    }

    auto file = scanner::findCachedArtwork (impl->paths.artworkDir, artworkId);
    if (file.existsAsFile())
    {
        const juce::ScopedLock sl (impl->artworkLock);
        impl->artworkCache[artworkId.toStdString()] = file;
    }
    return file;
}

juce::File LibraryService::artworkForTrack (const Track& track) const
{
    auto cached = artworkFileFor (track.artworkId);
    if (cached.existsAsFile())
        return cached;

    const juce::File source (track.path);
    if (! source.existsAsFile())
        return {};

    juce::String error;
    auto artwork = tags::readArtwork (source, error);

    const tags::EmbeddedArtwork* chosen = nullptr;
    for (auto& a : artwork)
        if (a.isFrontCover() && a.data.getSize() > 0) { chosen = &a; break; }
    if (chosen == nullptr)
        for (auto& a : artwork)
            if (a.data.getSize() > 0) { chosen = &a; break; }

    if (chosen == nullptr)
        return {};

    juce::String id;
    juce::String cacheError;
    auto file = scanner::cacheArtwork (impl->paths.artworkDir, chosen->data, chosen->mimeType, id, cacheError);

    if (file.existsAsFile() && id.isNotEmpty())
    {
        const juce::ScopedLock sl (impl->artworkLock);
        impl->artworkCache[id.toStdString()] = file;
    }
    else if (cacheError.isNotEmpty())
    {
        impl->postError ("Artwork: " + cacheError);
    }

    return file;
}

//==============================================================================
// Analysis cache
//==============================================================================
bool LibraryService::getAnalysis (TrackId id, TrackAnalysis& out) const
{
    juce::String json;
    int version = 0;
    juce::int64 analyzedAt = 0;

    if (! librarystore::loadAnalysisJson (const_cast<LibraryDatabase&> (db), id, json, version, analyzedAt))
        return false;

    juce::var value;
    if (juce::JSON::parse (json, value).failed())
        return false;

    out = serialize::analysisFromVar (value, {});
    if (version != TrackAnalysis::kVersion)
        out.valid = false;

    return true;
}

void LibraryService::saveAnalysis (TrackId id, const TrackAnalysis& analysis)
{
    if (id == kInvalidTrackId)
    {
        impl->postError ("Cannot save analysis for an invalid track id.");
        return;
    }

    const auto json = juce::JSON::toString (serialize::toVar (analysis));
    const int version = analysis.version > 0 ? analysis.version : TrackAnalysis::kVersion;

    if (! librarystore::saveAnalysisJson (db, id, json, version, juce::Time::currentTimeMillis()))
        impl->postError ("Could not save the analysis cache.");
}

//==============================================================================
// Listeners
//==============================================================================
void LibraryService::addListener (Listener* listener)
{
    auto h = impl->hub;
    if (h == nullptr || listener == nullptr)
        return;

    const juce::ScopedLock sl (h->lock);
    if (std::find (h->listeners.begin(), h->listeners.end(), listener) == h->listeners.end())
        h->listeners.push_back (listener);
}

void LibraryService::removeListener (Listener* listener)
{
    auto h = impl->hub;
    if (h == nullptr || listener == nullptr)
        return;

    const juce::ScopedLock sl (h->lock);
    h->listeners.remove (listener);
}

} // namespace sp
