/*
    Snake Puppy Reference Player
    Low-level file inspection used by the library scanner: format probing
    (with a JUCE AudioFormatManager fallback and native DSF/DFF header parsing),
    tag/artwork metadata, content hashing for duplicate detection and artwork
    cache storage.

    No database access happens here; this is pure file I/O and parsing. All
    functions may be called from the scanner thread. artworkFileFor / artwork
    cache lookups are cheap and safe from the message thread.

    This file is owned by the library engineer.
*/
#pragma once

#include "Audio/DecodeSource.h"
#include "Core/Types.h"
#include "Library/Tags.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace sp::scanner
{

/** Supported audio extensions, lowercase, without the dot. Mirrors the formats
    the app's decoder can play (see DecodeSource::supportedExtensions()). */
const juce::StringArray& supportedExtensions();
bool isSupportedAudioFile (const juce::File&);

/** Probes format/rate/depth/duration. Tries DecodeSource::probeInfo first and
    falls back to a native DSD header parser or the JUCE AudioFormatManager
    (which covers WAV/AIFF/FLAC/MP3/OGG and, on macOS, M4A/ALAC/AAC via
    CoreAudio). */
bool probeAudioFile (const juce::File& file, AudioFileInfo& infoOut, juce::String& errorOut);

struct TrackFileInfo
{
    bool probed = false;
    bool tagsRead = false;
    bool artworkFound = false;
    juce::String probeError;
    juce::String tagError;
};

/** Fills `trackOut` with everything the scanner derives from the file: path,
    folder, size, mtime, technical info, tags, ReplayGain, artwork id and a
    fallback title. When `extractArtwork` is true, embedded artwork is written
    to the artwork cache (content-hash filenames). Returns true when the file
    could be probed (tag failures are reported through `status` but are not
    fatal, so a file with broken tags can still be played and indexed). */
bool buildTrackInfo (const juce::File& file, const juce::File& artworkDir, bool extractArtwork,
                     Track& trackOut, TrackFileInfo& status);

//== Content hashing ==========================================================
/** MD5 of arbitrary bytes, lowercase hex. */
juce::String md5Bytes (const void* data, size_t numBytes);
/** MD5 of a whole file; empty on read failure. */
juce::String md5File (const juce::File& file);
/** Stable hash used for duplicate detection. Uses Tags::audioPayloadHash when
    the tag layer can provide it (so tag-only edits do not split duplicates),
    otherwise falls back to an MD5 of the complete file. `methodOut` describes
    which method was used ("payload" or "file"). Empty on failure. */
juce::String contentHashForFile (const juce::File& file, juce::String& methodOut);

//== Artwork cache ============================================================
/** Content hash (MD5) of image bytes; this is Track::artworkId. */
juce::String artworkIdForBytes (const juce::MemoryBlock& data);
/** Writes the artwork bytes into `artworkDir/<id>.<ext>` (atomic replace) and
    returns the cached file, or a null File on failure. */
juce::File cacheArtwork (const juce::File& artworkDir, const juce::MemoryBlock& data,
                         const juce::String& mimeType, juce::String& artworkIdOut, juce::String& errorOut);
/** Finds `<artworkDir>/<id>.*`; null File when the cache entry does not exist. */
juce::File findCachedArtwork (const juce::File& artworkDir, const juce::String& artworkId);
/** Derives a file extension from an image mime type or magic bytes. */
juce::String artworkExtension (const juce::MemoryBlock& data, const juce::String& mimeType);

//== Native DSD headers =======================================================
/** Parses a DSF or DFF header without decoding audio. Exposed for tests. */
bool parseDsdHeader (const juce::File& file, AudioFileInfo& infoOut, juce::String& errorOut);
bool isDsdFile (const juce::File& file);

} // namespace sp::scanner
