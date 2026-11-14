/*
    Snake Puppy Reference Player
    Smart-playlist rule evaluation and ordering. This is deliberately a pure,
    dependency-free (database-free) module so it can be unit tested with
    synthetic Track values and reused by the UI for live previews.

    Rule semantics
    --------------
    - String comparisons are case-insensitive and ignore leading/trailing
      whitespace on both sides (metadata frequently has stray spaces).
    - Numeric fields: year, rating, sampleRate (Hz), bitDepth, playCount,
      duration (seconds, from durationMs) and dateAdded (milliseconds since the
      Unix epoch, parsed from the ISO-8601 tag). `dateAdded` keeps its ISO text
      for the string operators.
    - Missing metadata: an unknown year (<= 0), sampleRate (<= 0),
      bitDepth (<= 0), duration (<= 0) or empty dateAdded never satisfies a
      numeric comparison. rating 0 is treated as the real value "unrated/0" and
      playCount 0 is a real play count, so both compare literally.
    - `between` is inclusive and tolerates number/number2 being supplied in
      either order.
    - `inList` is case-insensitive for text fields; for numeric fields each list
      entry is parsed as a number.
    - String operators on numeric fields compare against the formatted value
      (e.g. "48000"); numeric operators on string fields parse the field text
      (e.g. a year written into a comment field) and are false when it is not
      numeric.

    Ordering (SmartPlaylistDefinition::sortColumn is a field name such as
    "artist", "albumArtist", "year", "sampleRate", "dateAdded"):
    - Unknown sort columns fall back to "artist".
    - Unknown/empty values sort last in both directions.
    - Ties are broken by title, then path, then track id so the order is stable
      and deterministic; `sortDescending` reverses the comparison.
    - `limit` is applied after sorting; 0 or negative means unlimited.
*/
#pragma once

#include "Core/Types.h"
#include <vector>

namespace sp::smartplay
{

/** Canonical lowerCamelCase name of a field ("albumArtist", "sampleRate"). */
const char* fieldName (SmartField);

/** Parses a field name case-insensitively, accepting snake_case and hyphens
    ("album_artist", "sample-rate"). Sets ok=false and returns SmartField::artist
    when the name is not recognised. */
SmartField fieldFromName (const juce::String& name, bool* ok = nullptr);

/** True when the field's canonical ordering is numeric (year, rating,
    sampleRate, bitDepth, duration, playCount, dateAdded is text-but-sortable). */
bool fieldIsNumeric (SmartField);

/** The field as text (trimmed by callers where needed). */
juce::String fieldString (const Track&, SmartField);

/** The field as a number. Returns false when the field is missing or when the
    underlying text is not numeric (see the semantics above). */
bool fieldNumber (const Track&, SmartField, double& out);

/** True when the track satisfies a single rule. */
bool matchesRule (const Track&, const SmartRule&);

/** True when the track satisfies the whole definition (matchAll/matchAny). An
    empty rule list matches everything when matchAll is true, and nothing when
    matchAll is false (vacuous truth / vacuous falsehood respectively). */
bool matches (const Track&, const SmartPlaylistDefinition&);

/** Selects matching track pointers (into libraryTracks), sorted and limited. */
void select (const std::vector<Track>& libraryTracks, const SmartPlaylistDefinition&,
             std::vector<const Track*>& out);

/** Applies rules + ordering + limit to a library track list. */
std::vector<Track> apply (const std::vector<Track>& libraryTracks, const SmartPlaylistDefinition&);

/** Sorts matching pointers in place according to the definition. */
void sortTracks (std::vector<const Track*>& tracks, const SmartPlaylistDefinition&);

} // namespace sp::smartplay
