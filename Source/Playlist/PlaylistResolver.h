/*
    Snake Puppy Reference Player
    Resolves playlist-file entries (as parsed by PlaylistIO) to library track
    ids. Pure: it works against an immutable LibrarySnapshot and never scans or
    touches the database, so the resolution rules can be unit tested with a
    synthetic snapshot.

    Resolution order
    ----------------
      1. absolute path, normalised (canonical path when the file exists, which
         also resolves /var -> /private/var symlinks on macOS);
      2. relative path resolved against the playlist file's directory, then
         normalised;
      3. unique filename in the library;
      4. filename + duration (from #EXTINF / PLS LengthN) within 1 second
         (strictly less than 1000 ms, so 1 s vs 2 s files stay unambiguous).
    Entries that stay ambiguous are reported as unresolved with a reason
    instead of silently picking one; nothing is ever dropped silently.
*/
#pragma once

#include "Core/Types.h"
#include "Library/LibraryService.h"
#include <juce_core/juce_core.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace sp
{

class PlaylistPathResolver
{
public:
    struct Match
    {
        TrackId trackId = kInvalidTrackId;
        bool resolved = false;
        juce::String matchedBy;     // "absolute path", "relative path", "filename", "filename+duration"
        juce::String reason;        // set when resolved == false
    };

    explicit PlaylistPathResolver (const LibrarySnapshot& snapshot);

    /** Resolves one entry. `playlistDirectory` is the directory containing the
        playlist file (used for relative entries). */
    Match resolve (const juce::String& entryPath, double lengthSeconds,
                   const juce::File& playlistDirectory) const;

    /** Canonical/lexically normalised absolute path, suitable for comparison. */
    static juce::String normalisePath (const juce::String& path);

    int numLibraryTracks() const { return (int) tracks.size(); }

private:
    std::vector<const Track*> tracks;
    std::unordered_map<std::string, int> byPath;            // lower-case normalised path -> index
    std::unordered_multimap<std::string, int> byFilename;   // lower-case filename -> index
};

} // namespace sp
