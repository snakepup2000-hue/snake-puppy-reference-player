/*
    Snake Puppy Reference Player
    Tag reading/writing and embedded artwork extraction.

    Pure byte-level file parsing: no JUCE audio format readers and no external
    tools are involved. All functions perform blocking disk I/O and must be
    called from a worker/scanner thread, never from the audio callback. They are
    re-entrant (the module keeps no shared mutable state).

    Supported containers
    --------------------
      Read:  FLAC (Vorbis comments + PICTURE), OGG Vorbis (comment header +
             METADATA_BLOCK_PICTURE), MP3/AAC-ADTS (ID3v2.2/2.3/2.4 at the start,
             including unsynchronisation, extended headers, zlib-compressed
             frames and footer), M4A/MP4 (moov/udta/meta/ilst atoms incl.
             free-form ReplayGain), WAV (LIST INFO + embedded id3 chunk),
             AIFF/AIFC (NAME/AUTH/ANNO + embedded ID3 chunk), DSF (ID3v2 tag at
             the metadata pointer) and DFF/DSDIFF (ID3 chunk).
             CAF is detected but has no tag implementation yet.
      Write: FLAC, OGG Vorbis, MP3, ADTS AAC, M4A/MP4, WAV, AIFF/AIFC, DSF, DFF.

    Integrity guarantees
    --------------------
    - Writers rebuild only the tag/metadata region and copy the audio payload
      bytes verbatim, then install the result with an atomic temp-file rename.
      A failed write leaves the original file untouched.
    - MP4 writers re-point stco/co64 chunk offsets when a moov size change
      shifts the media data (both moov-before-mdat and moov-after-mdat layouts
      are handled); mdat bytes are never modified. Fragmented MP4 (moof) is
      refused with a clear error.
    - Malformed/truncated files return `false` plus a human-readable error
      instead of crashing.
*/

#pragma once

#include <juce_core/juce_core.h>

namespace sp::tags
{

struct TagData
{
    bool readOk = false;
    juce::String title, artist, albumArtist, album, genre, composer, comment;
    int year = 0;
    int trackNumber = 0;
    int discNumber = 0;
    int rating = 0;                 // 0..5; read/written through ID3 POPM only
                                    // (MP3/WAV/AIFF/DSF/DFF). FLAC/OGG/MP4 have
                                    // no standard star-rating field, so it stays 0.

    float replayGainTrackDb = 0.0f;
    bool hasReplayGainTrack = false;
    float replayGainAlbumDb = 0.0f;
    bool hasReplayGainAlbum = false;
    float peakLinear = 0.0f;
    bool hasPeak = false;

    juce::String formatName;        // filled in by readTags/detectFormatName
};

struct EmbeddedArtwork
{
    juce::MemoryBlock data;
    juce::String mimeType;          // image/jpeg, image/png
    int width = 0;
    int height = 0;
    int pictureType = 3;            // ID3 APIC type, 3 = front cover
    bool isFrontCover() const { return pictureType == 3 || pictureType == 0; }
};

/** Detects the container from the file extension plus magic bytes. A matching
    magic signature wins over the extension; when the magic is not recognised
    the extension mapping is used, so a corrupt ".flac" still reports "FLAC"
    (and readTags then explains what is wrong). Returns "FLAC", "MP3", "M4A",
    "AAC", "OGG", "WAV", "AIFF", "DSF", "DFF", "CAF" or "" when unknown. */
juce::String detectFormatName (const juce::File& file);

/** Reads metadata. Returns true on success (out.readOk is set too). A valid
    file with no tags is a success with empty fields. Malformed files return
    false with a descriptive errorOut. */
bool readTags (const juce::File& file, TagData& out, juce::String& errorOut);

/** Writes metadata without touching audio samples.

    Empty / zero-valued fields are skipped: the existing value is preserved.
    (There is currently no API to clear a single field.) The existing
    container tag is merged with `tags`, so unrelated frames/atoms/chunks,
    embedded artwork and ReplayGain survive a metadata edit.

    Returns false and leaves the source file untouched on any failure. */
bool writeTags (const juce::File& file, const TagData& tags, juce::String& errorOut);

/** All embedded artwork blocks, front cover first. Empty if none. On malformed
    input errorOut is set and an empty vector is returned. */
std::vector<EmbeddedArtwork> readArtwork (const juce::File& file, juce::String& errorOut);

/** Inserts or replaces the front-cover artwork. JPEG and PNG data only
    (the MIME type is sniffed from the bytes; `mimeType` may be empty). Other
    artwork blocks are preserved. Atomic, like writeTags. */
bool writeArtwork (const juce::File& file, const juce::MemoryBlock& imageData,
                   const juce::String& mimeType, juce::String& errorOut);

/** Reads ReplayGain values (REPLAYGAIN_* tags, falling back to R128_* when no
    REPLAYGAIN_* gain is present). `has*` flags report presence; the numeric
    outputs are zero when absent. Returns false only when the file cannot be
    read at all. */
bool readReplayGain (const juce::File& file,
                     float& trackGainDb, float& albumGainDb, float& peakLinear,
                     bool& hasTrack, bool& hasAlbum, bool& hasPeak,
                     juce::String& errorOut);

/** Writes ReplayGain tags (used by the offline analysis pass). All three values
    are stored; container-specific storage is ID3 TXXX (MP3/WAV/AIFF/DSF/DFF),
    Vorbis comments (FLAC/OGG) or iTunes free-form atoms (M4A/MP4). */
bool writeReplayGain (const juce::File& file, float trackGainDb, float albumGainDb,
                      float peakLinear, juce::String& errorOut);

/** Returns the raw audio payload region(s) of a file for verification tests:
    FLAC frames, MP3/ADTS MPEG frames (tags excluded), MP4 mdat, WAV data chunk,
    AIFF SSND sample bytes, OGG audio packets, DSF data chunk, DFF DSD chunk.
    Returns false when the payload cannot be identified. */
bool extractAudioPayload (const juce::File& file, juce::MemoryBlock& out, juce::String& errorOut);

/** MD5 (lower-case hex) of extractAudioPayload(); empty string on failure.
    Decode-independent proof that a tag write did not alter audio bytes. */
juce::String audioPayloadHash (const juce::File& file);

} // namespace sp::tags
