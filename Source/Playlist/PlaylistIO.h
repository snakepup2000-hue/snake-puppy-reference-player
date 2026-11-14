/*
    Snake Puppy Reference Player
    M3U / M3U8 / PLS playlist file parsing and writing. Pure file-format code:
    it never talks to the library or the database, which makes the parser easy
    to test and reuse (e.g. for drag-and-drop previews).

    Robustness:
      - UTF-8 (with or without BOM) and UTF-16 LE/BE (with BOM) input.
      - CRLF, CR and LF line endings.
      - M3U: #EXTM3U, #EXTINF:<seconds>[,<title>], #PLAYLIST:<name>, # comments,
        blank lines. Relative paths are preserved verbatim (the resolver
        resolves them against the playlist file's directory).
      - PLS: [playlist] section, case-insensitive keys, FileN / TitleN /
        LengthN, keys processed in ascending N order, files without a
        [playlist] section are accepted with a warning.
      - Writers always emit UTF-8 with LF line endings and strip embedded CR/LF
        from individual fields (a path containing a newline cannot be
        represented in these formats).

    The import path also accepts `file://` URIs and `~`-relative entries; see
    PlaylistResolver.h.
*/
#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace sp::playlistio
{

enum class Format
{
    unknown = 0,
    m3u,
    m3u8,
    pls
};

struct Entry
{
    juce::String path;              // exactly as stored in the file (absolute or relative)
    juce::String title;             // #EXTINF title or PLS TitleN (may be empty)
    double lengthSeconds = -1.0;    // #EXTINF duration or PLS LengthN; -1 = unknown
};

struct Document
{
    Format format = Format::unknown;
    juce::String title;             // #PLAYLIST:<name> where present
    std::vector<Entry> entries;
    juce::StringArray warnings;     // non-fatal parser observations
};

/** Human-readable format name: "M3U", "M3U8", "PLS", "unknown". */
const char* formatName (Format);

/** Detects from the file extension first, then from the content when the
    extension is unfamiliar (presence of [playlist] or #EXTM3U). */
Format detectFormat (const juce::File&, const juce::String& content = {});

/** Reads a playlist file. Returns false only on a hard failure (file missing,
    unreadable, or not a recognised playlist); a syntactically valid file with
    zero entries returns true with an empty entry list. */
bool parse (const juce::File&, Document&, juce::String& errorOut);

/** Serialisers. Each appends exactly one trailing newline; output is UTF-8,
    LF-only. Empty entry lists produce just the header. */
juce::String serialiseM3U8 (const std::vector<Entry>&);
juce::String serialiseM3UBasic (const std::vector<Entry>&);
juce::String serialisePLS (const std::vector<Entry>&);

/** Convenience: dispatch on the target file's extension; returns false and sets
    errorOut for unsupported extensions. */
bool write (const juce::File& target, const std::vector<Entry>&, juce::String& errorOut);

/** Reads the raw file bytes and decodes UTF-8/UTF-16 with BOM detection. */
bool readFileText (const juce::File&, juce::String& textOut, juce::String& errorOut);

} // namespace sp::playlistio
