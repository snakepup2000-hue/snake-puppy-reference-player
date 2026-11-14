#include "Playlist/PlaylistResolver.h"

#include <cmath>
#include <cstdlib>
#include <limits.h>

namespace sp
{

namespace
{
constexpr double kDurationToleranceMs = 1000.0;

std::string toKey (const juce::String& text)
{
    return text.toLowerCase().toStdString();
}

bool looksLikeWindowsAbsolutePath (const juce::String& text)
{
    return text.length() >= 3
           && juce::CharacterFunctions::isLetter (text[0])
           && text[1] == ':'
           && (text[2] == '\\' || text[2] == '/');
}

bool looksLikeNetworkUrl (const juce::String& text)
{
    if (text.contains ("://"))
        return true;

    const auto lowered = text.toLowerCase();
    return lowered.startsWith ("http:") || lowered.startsWith ("https:")
           || lowered.startsWith ("ftp:") || lowered.startsWith ("mms:")
           || lowered.startsWith ("rtsp:") || lowered.startsWith ("udp:");
}

/** Resolves symlinks (e.g. macOS /var -> /private/var) when the target exists.
    Returns an empty string when the path does not exist or cannot be resolved.
    JUCE 8 File has no canonical-path helper, so use the platform realpath(). */
juce::String canonicalPathIfExisting (const juce::File& file)
{
   #if JUCE_MAC || JUCE_LINUX || JUCE_BSD
    const auto path = file.getFullPathName();
    if (path.isEmpty())
        return {};

    char resolved[PATH_MAX];
    if (realpath (path.toRawUTF8(), resolved) != nullptr)
        return juce::String::fromUTF8 (resolved);
   #endif

    return {};
}
} // namespace

//==============================================================================
juce::String PlaylistPathResolver::normalisePath (const juce::String& path)
{
    const auto trimmed = path.trim();
    if (trimmed.isEmpty())
        return {};

    juce::File file (trimmed);
    const auto canonical = canonicalPathIfExisting (file);
    if (canonical.isNotEmpty())
        return canonical;

    const auto absolute = file.getFullPathName();
    if (! absolute.startsWithChar ('/'))
        return absolute;        // non-POSIX (cannot happen for resolved entries, but stay honest)

    juce::StringArray parts;
    for (const auto& token : juce::StringArray::fromTokens (absolute, "/", ""))
    {
        if (token.isEmpty() || token == ".")
            continue;

        if (token == "..")
        {
            if (! parts.isEmpty())
                parts.remove (parts.size() - 1);
            continue;
        }

        parts.add (token);
    }

    return "/" + parts.joinIntoString ("/");
}

//==============================================================================
PlaylistPathResolver::PlaylistPathResolver (const LibrarySnapshot& snapshot)
{
    tracks.reserve (snapshot.tracks.size());

    for (const auto& track : snapshot.tracks)
    {
        if (track.path.isEmpty())
            continue;

        const auto index = (int) tracks.size();
        tracks.push_back (&track);

        const auto normalised = normalisePath (track.path);
        if (normalised.isNotEmpty())
            byPath.emplace (toKey (normalised), index);

        const auto fileName = juce::File (track.path).getFileName().toLowerCase();
        if (fileName.isNotEmpty())
            byFilename.emplace (fileName.toStdString(), index);
    }
}

PlaylistPathResolver::Match PlaylistPathResolver::resolve (const juce::String& entryPath,
                                                         double lengthSeconds,
                                                         const juce::File& playlistDirectory) const
{
    Match match;

    auto text = entryPath.trim();
    if (text.isEmpty())
    {
        match.reason = "empty path";
        return match;
    }

    if (text.startsWithIgnoreCase ("file://"))
        text = juce::URL::removeEscapeChars (text.substring (7)).trim();

    if (text.startsWithChar ('~'))
        text = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName()
               + text.substring (1);

    if (looksLikeWindowsAbsolutePath (text))
    {
        match.reason = "Windows path cannot be resolved on this system";
        return match;
    }

    if (looksLikeNetworkUrl (text))
    {
        match.reason = "network/stream URL is not a library file";
        return match;
    }

    const bool isRelative = ! juce::File::isAbsolutePath (text);
    juce::String absolutePath;
    if (isRelative)
    {
        auto relative = text.replaceCharacter ('\\', '/');
        absolutePath = playlistDirectory.getChildFile (relative).getFullPathName();
    }
    else
    {
        absolutePath = text;
    }

    const auto normalised = normalisePath (absolutePath);
    const auto pathMatch = byPath.find (toKey (normalised));
    if (pathMatch != byPath.end())
    {
        match.trackId = tracks[(size_t) pathMatch->second]->id;
        match.resolved = true;
        match.matchedBy = isRelative ? "relative path" : "absolute path";
        return match;
    }

    //== Filename fallback =====================================================
    const auto fileName = juce::File (absolutePath).getFileName().toLowerCase();
    if (fileName.isEmpty())
    {
        match.reason = "entry has no filename";
        return match;
    }

    std::vector<int> candidates;
    const auto range = byFilename.equal_range (fileName.toStdString());
    for (auto it = range.first; it != range.second; ++it)
        candidates.push_back (it->second);

    if (candidates.empty())
    {
        match.reason = juce::File (absolutePath).existsAsFile()
                           ? "file exists but is not in the library"
                           : "not found in the library";
        return match;
    }

    if (candidates.size() == 1)
    {
        match.trackId = tracks[(size_t) candidates.front()]->id;
        match.resolved = true;
        match.matchedBy = "filename";
        return match;
    }

    if (lengthSeconds > 0.0)
    {
        const double wantedMs = lengthSeconds * 1000.0;
        std::vector<int> durationMatches;
        for (const int index : candidates)
        {
            const auto durationMs = (double) tracks[(size_t) index]->durationMs;
            if (durationMs > 0.0 && std::abs (durationMs - wantedMs) < kDurationToleranceMs)
                durationMatches.push_back (index);
        }

        if (durationMatches.size() == 1)
        {
            match.trackId = tracks[(size_t) durationMatches.front()]->id;
            match.resolved = true;
            match.matchedBy = "filename+duration";
            return match;
        }
    }

    match.reason = "ambiguous: " + juce::String ((int) candidates.size())
                   + " library tracks share the filename \"" + fileName + "\"";
    return match;
}

} // namespace sp
