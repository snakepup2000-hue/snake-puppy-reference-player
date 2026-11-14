#include "Playlist/SmartPlaylist.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace sp::smartplay
{

namespace
{
constexpr double kEpsilon = 1.0e-9;

bool approximatelyEqual (double a, double b)
{
    return std::abs (a - b) <= kEpsilon * std::max (1.0, std::max (std::abs (a), std::abs (b)));
}

/** Strict numeric parse: the whole trimmed string must be consumed. */
bool parseNumber (const juce::String& text, double& out)
{
    const auto trimmed = text.trim();
    if (trimmed.isEmpty())
        return false;

    const char* begin = trimmed.toRawUTF8();
    char* end = nullptr;
    const double value = std::strtod (begin, &end);
    if (end == begin)
        return false;

    while (*end == ' ' || *end == '\t')
        ++end;
    if (*end != '\0')
        return false;

    out = value;
    return true;
}

juce::String formatNumber (SmartField field, double value)
{
    if (field == SmartField::duration)
    {
        if (std::abs (value - std::round (value)) >= kEpsilon)
            return juce::String (value, 3);
    }

    return juce::String ((juce::int64) std::llround (value));
}

bool stringMatches (const juce::String& rawValue, SmartOp op, const juce::String& rawText)
{
    const auto value = rawValue.trim();
    const auto text = rawText.trim();

    switch (op)
    {
        case SmartOp::is:           return value.compareIgnoreCase (text) == 0;
        case SmartOp::isNot:        return value.compareIgnoreCase (text) != 0;
        case SmartOp::contains:     return value.containsIgnoreCase (text);
        case SmartOp::notContains:  return ! value.containsIgnoreCase (text);
        case SmartOp::beginsWith:   return value.startsWithIgnoreCase (text);
        case SmartOp::endsWith:     return value.endsWithIgnoreCase (text);
        case SmartOp::greaterThan:
        case SmartOp::lessThan:
        case SmartOp::between:
        case SmartOp::inList:
            return false;
    }

    return false;
}

/** Text used for contains/beginsWith/endsWith on a numeric field: the formatted
    number, or an empty string when the numeric value is unknown. */
juce::String textForStringOperator (const Track& track, SmartField field)
{
    if (! fieldIsNumeric (field))
        return fieldString (track, field);

    double value = 0.0;
    if (! fieldNumber (track, field, value))
        return {};

    return formatNumber (field, value);
}

bool numericMatches (const Track& track, const SmartRule& rule)
{
    double value = 0.0;
    if (! fieldNumber (track, rule.field, value))
        return false;

    switch (rule.op)
    {
        case SmartOp::is:          return approximatelyEqual (value, rule.number);
        case SmartOp::isNot:       return ! approximatelyEqual (value, rule.number);
        case SmartOp::greaterThan: return value > rule.number;
        case SmartOp::lessThan:    return value < rule.number;
        case SmartOp::between:
        {
            const double low = std::min (rule.number, rule.number2);
            const double high = std::max (rule.number, rule.number2);
            return value >= low && value <= high;
        }

        case SmartOp::contains:
        case SmartOp::notContains:
        case SmartOp::beginsWith:
        case SmartOp::endsWith:
        case SmartOp::inList:
            return false;
    }

    return false;
}

bool listMatches (const Track& track, const SmartRule& rule)
{
    if (rule.list.isEmpty())
        return false;

    if (fieldIsNumeric (rule.field))
    {
        double value = 0.0;
        if (! fieldNumber (track, rule.field, value))
            return false;

        for (const auto& item : rule.list)
        {
            double candidate = 0.0;
            if (parseNumber (item, candidate) && approximatelyEqual (value, candidate))
                return true;
        }
        return false;
    }

    const auto value = fieldString (track, rule.field).trim();
    for (const auto& item : rule.list)
        if (value.compareIgnoreCase (item.trim()) == 0)
            return true;

    return false;
}

int compareValues (const Track& a, const Track& b, SmartField field, bool descending)
{
    auto flip = [descending] (int result) { return descending ? -result : result; };

    if (fieldIsNumeric (field))
    {
        double va = 0.0, vb = 0.0;
        const bool ka = fieldNumber (a, field, va);
        const bool kb = fieldNumber (b, field, vb);

        // Unknown values always sort last, in both directions.
        if (! ka && ! kb) return 0;
        if (! ka) return 1;
        if (! kb) return -1;

        if (approximatelyEqual (va, vb))
            return 0;
        return flip (va < vb ? -1 : 1);
    }

    const auto sa = fieldString (a, field).trim();
    const auto sb = fieldString (b, field).trim();

    if (sa.isEmpty() && sb.isEmpty()) return 0;
    if (sa.isEmpty()) return 1;      // empty always sorts last
    if (sb.isEmpty()) return -1;

    return flip (sa.compareIgnoreCase (sb));
}

} // namespace

//==============================================================================
const char* fieldName (SmartField field)
{
    switch (field)
    {
        case SmartField::artist:      return "artist";
        case SmartField::album:       return "album";
        case SmartField::albumArtist: return "albumArtist";
        case SmartField::genre:       return "genre";
        case SmartField::year:        return "year";
        case SmartField::rating:      return "rating";
        case SmartField::sampleRate:  return "sampleRate";
        case SmartField::bitDepth:    return "bitDepth";
        case SmartField::format:      return "format";
        case SmartField::title:       return "title";
        case SmartField::duration:    return "duration";
        case SmartField::dateAdded:   return "dateAdded";
        case SmartField::playCount:   return "playCount";
        case SmartField::comment:     return "comment";
        case SmartField::folder:      return "folder";
    }
    return "artist";
}

SmartField fieldFromName (const juce::String& name, bool* ok)
{
    juce::String normalised;
    for (auto c : name)
        if (c != '_' && c != '-' && c != ' ')
            normalised << juce::String::charToString (juce::CharacterFunctions::toLowerCase (c));

    struct Entry { const char* name; SmartField field; };
    static const Entry entries[]
    {
        { "artist", SmartField::artist },
        { "album", SmartField::album },
        { "albumartist", SmartField::albumArtist },
        { "genre", SmartField::genre },
        { "year", SmartField::year },
        { "rating", SmartField::rating },
        { "samplerate", SmartField::sampleRate },
        { "bitdepth", SmartField::bitDepth },
        { "format", SmartField::format },
        { "title", SmartField::title },
        { "duration", SmartField::duration },
        { "length", SmartField::duration },
        { "dateadded", SmartField::dateAdded },
        { "playcount", SmartField::playCount },
        { "comment", SmartField::comment },
        { "folder", SmartField::folder },
        { "path", SmartField::folder },
    };

    for (const auto& e : entries)
    {
        if (normalised == e.name)
        {
            if (ok != nullptr) *ok = true;
            return e.field;
        }
    }

    if (ok != nullptr) *ok = false;
    return SmartField::artist;
}

bool fieldIsNumeric (SmartField field)
{
    switch (field)
    {
        case SmartField::year:
        case SmartField::rating:
        case SmartField::sampleRate:
        case SmartField::bitDepth:
        case SmartField::duration:
        case SmartField::playCount:
            return true;

        case SmartField::dateAdded:
        case SmartField::artist:
        case SmartField::album:
        case SmartField::albumArtist:
        case SmartField::genre:
        case SmartField::format:
        case SmartField::title:
        case SmartField::comment:
        case SmartField::folder:
            return false;
    }

    return false;
}

juce::String fieldString (const Track& track, SmartField field)
{
    switch (field)
    {
        case SmartField::artist:      return track.artist;
        case SmartField::album:       return track.album;
        case SmartField::albumArtist: return track.albumArtist;
        case SmartField::genre:       return track.genre;
        case SmartField::format:      return track.formatName;
        case SmartField::title:       return track.title;
        case SmartField::comment:     return track.comment;
        case SmartField::dateAdded:   return track.dateAddedIso;
        case SmartField::folder:
            if (track.folder.isNotEmpty())
                return track.folder;
            return juce::File (track.path).getParentDirectory().getFullPathName();
        case SmartField::year:        return juce::String (track.year);
        case SmartField::rating:      return juce::String (track.rating);
        case SmartField::sampleRate:  return juce::String (track.sampleRate);
        case SmartField::bitDepth:    return juce::String (track.bitDepth);
        case SmartField::duration:    return formatNumber (SmartField::duration, track.durationMs / 1000.0);
        case SmartField::playCount:   return juce::String (track.playCount);
    }

    return {};
}

bool fieldNumber (const Track& track, SmartField field, double& out)
{
    switch (field)
    {
        case SmartField::year:
            out = (double) track.year;
            return track.year > 0;

        case SmartField::rating:
            out = (double) track.rating;
            return true;                    // 0 is a valid "unrated" value

        case SmartField::sampleRate:
            out = (double) track.sampleRate;
            return track.sampleRate > 0;

        case SmartField::bitDepth:
            out = (double) track.bitDepth;
            return track.bitDepth > 0;

        case SmartField::duration:
            out = track.durationMs / 1000.0;
            return track.durationMs > 0;

        case SmartField::playCount:
            out = (double) track.playCount;
            return true;                    // 0 plays is a real value

        case SmartField::dateAdded:
        {
            if (track.dateAddedIso.trim().isEmpty())
                return false;
            const auto time = juce::Time::fromISO8601 (track.dateAddedIso);
            out = (double) time.toMilliseconds();
            return out > 0.0;
        }

        case SmartField::artist:
        case SmartField::album:
        case SmartField::albumArtist:
        case SmartField::genre:
        case SmartField::format:
        case SmartField::title:
        case SmartField::comment:
        case SmartField::folder:
            return parseNumber (fieldString (track, field), out);
    }

    return false;
}

//==============================================================================
bool matchesRule (const Track& track, const SmartRule& rule)
{
    switch (rule.op)
    {
        case SmartOp::is:
        case SmartOp::isNot:
            if (fieldIsNumeric (rule.field))
                return numericMatches (track, rule);
            return stringMatches (fieldString (track, rule.field), rule.op, rule.text);

        case SmartOp::contains:
        case SmartOp::notContains:
        case SmartOp::beginsWith:
        case SmartOp::endsWith:
            return stringMatches (textForStringOperator (track, rule.field), rule.op, rule.text);

        case SmartOp::greaterThan:
        case SmartOp::lessThan:
        case SmartOp::between:
            return numericMatches (track, rule);

        case SmartOp::inList:
            return listMatches (track, rule);
    }

    return false;
}

bool matches (const Track& track, const SmartPlaylistDefinition& definition)
{
    if (definition.rules.empty())
        return definition.matchAll;

    if (definition.matchAll)
    {
        for (const auto& rule : definition.rules)
            if (! matchesRule (track, rule))
                return false;
        return true;
    }

    for (const auto& rule : definition.rules)
        if (matchesRule (track, rule))
            return true;
    return false;
}

//==============================================================================
void sortTracks (std::vector<const Track*>& tracks, const SmartPlaylistDefinition& definition)
{
    bool sortColumnRecognised = false;
    const auto field = fieldFromName (definition.sortColumn, &sortColumnRecognised);
    const auto ordering = sortColumnRecognised ? field : SmartField::artist;
    const bool descending = definition.sortDescending;

    std::stable_sort (tracks.begin(), tracks.end(),
                      [ordering, descending] (const Track* a, const Track* b)
    {
        if (a == nullptr || b == nullptr)
            return a != nullptr;

        // compareValues already applies the direction while keeping unknown or
        // empty values last in both directions.
        const int primary = compareValues (*a, *b, ordering, descending);

        if (primary != 0)
            return primary < 0;

        // Ties are broken by title, then path, then id so ordering is stable
        // and deterministic in both directions.
        int result = a->displayTitle().compareIgnoreCase (b->displayTitle());
        if (result == 0)
            result = a->path.compareIgnoreCase (b->path);
        if (result == 0)
            result = a->id < b->id ? -1 : (a->id > b->id ? 1 : 0);

        if (descending)
            result = -result;

        return result < 0;
    });
}

void select (const std::vector<Track>& libraryTracks, const SmartPlaylistDefinition& definition,
             std::vector<const Track*>& out)
{
    out.clear();
    out.reserve (libraryTracks.size());

    for (const auto& track : libraryTracks)
        if (matches (track, definition))
            out.push_back (&track);

    sortTracks (out, definition);

    const int limit = definition.limit;
    if (limit > 0 && (int) out.size() > limit)
        out.resize ((size_t) limit);
}

std::vector<Track> apply (const std::vector<Track>& libraryTracks, const SmartPlaylistDefinition& definition)
{
    std::vector<const Track*> matched;
    select (libraryTracks, definition, matched);

    std::vector<Track> result;
    result.reserve (matched.size());
    for (const auto* track : matched)
        result.push_back (*track);

    return result;
}

} // namespace sp::smartplay
