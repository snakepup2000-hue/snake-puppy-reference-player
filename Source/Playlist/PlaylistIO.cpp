#include "Playlist/PlaylistIO.h"

#include <cmath>
#include <map>

namespace sp::playlistio
{

namespace
{
constexpr juce::int64 kMaxPlaylistFileBytes = 16 * 1024 * 1024;
constexpr int kMaxWarnings = 20;

juce::String stripEmbeddedLineBreaks (const juce::String& text)
{
    auto cleaned = text.replace ("\r", " ").replace ("\n", " ");
    return cleaned.trim();
}

void addWarning (Document& doc, const juce::String& warning)
{
    if (doc.warnings.size() < kMaxWarnings)
        doc.warnings.add (warning);
}

bool parseM3U (const juce::StringArray& lines, Document& doc)
{
    bool havePendingInfo = false;
    juce::String pendingTitle;
    double pendingLength = -1.0;

    for (const auto& rawLine : lines)
    {
        const auto line = rawLine.trim();
        if (line.isEmpty())
            continue;

        if (line.startsWithChar ('#'))
        {
            if (line.startsWithIgnoreCase ("#EXTINF:"))
            {
                const auto rest = line.substring (8);
                const int comma = rest.indexOfChar (',');
                const auto lengthText = (comma >= 0 ? rest.substring (0, comma) : rest).trim();
                const double parsedLength = lengthText.getDoubleValue();
                pendingLength = parsedLength > 0.0 ? parsedLength : -1.0;
                pendingTitle = comma >= 0 ? rest.substring (comma + 1).trim() : juce::String();
                havePendingInfo = true;
            }
            else if (line.startsWithIgnoreCase ("#PLAYLIST:"))
            {
                doc.title = line.substring (10).trim();   // after "#PLAYLIST:"
            }

            // All other directives (#EXTM3U, #EXT-X-*, comments) are ignored.
            continue;
        }

        Entry entry;
        entry.path = line;
        if (havePendingInfo)
        {
            entry.title = pendingTitle;
            entry.lengthSeconds = pendingLength;
        }
        doc.entries.push_back (entry);

        havePendingInfo = false;
        pendingTitle = {};
        pendingLength = -1.0;
    }

    return true;
}

bool parsePLS (const juce::StringArray& lines, Document& doc)
{
    std::map<int, Entry> numberedEntries;
    bool sawPlaylistSection = false;
    int declaredCount = -1;

    for (const auto& rawLine : lines)
    {
        const auto line = rawLine.trim();
        if (line.isEmpty())
            continue;

        if (line.startsWithChar (';') || line.startsWithChar ('#'))
            continue;

        if (line.startsWithIgnoreCase ("[playlist]"))
        {
            sawPlaylistSection = true;
            continue;
        }

        const int equals = line.indexOfChar ('=');
        if (equals <= 0)
        {
            addWarning (doc, "Ignored unrecognised PLS line: " + line.substring (0, 120));
            continue;
        }

        const auto key = line.substring (0, equals).trim().toLowerCase();
        auto value = line.substring (equals + 1).trim();
        if (value.length() >= 2 && value.startsWithChar ('"') && value.endsWithChar ('"'))
            value = value.substring (1, value.length() - 1).trim();

        if (key == "numberofentries")
        {
            declaredCount = value.getIntValue();
            continue;
        }

        if (key == "version")
            continue;

        if (key == "name" || key == "playlistname")
        {
            if (doc.title.isEmpty())
                doc.title = value;
            continue;
        }

        if (key.startsWith ("file"))
        {
            const int n = key.substring (4).getIntValue();
            if (n > 0 && value.isNotEmpty())
                numberedEntries[n].path = value;
            continue;
        }

        if (key.startsWith ("title"))
        {
            const int n = key.substring (5).getIntValue();
            if (n > 0)
                numberedEntries[n].title = value;
            continue;
        }

        if (key.startsWith ("length"))
        {
            const int n = key.substring (6).getIntValue();
            if (n > 0)
            {
                const double length = value.getDoubleValue();
                numberedEntries[n].lengthSeconds = length > 0.0 ? length : -1.0;
            }
            continue;
        }

        addWarning (doc, "Ignored unknown PLS key: " + key);
    }

    if (! sawPlaylistSection)
        addWarning (doc, "PLS file has no [playlist] section; entries were still read.");

    for (auto& entry : numberedEntries)
        if (entry.second.path.isNotEmpty())
            doc.entries.push_back (entry.second);

    if (declaredCount >= 0 && (size_t) declaredCount != doc.entries.size())
        addWarning (doc, "PLS NumberOfEntries=" + juce::String (declaredCount) + " but "
                             + juce::String ((int) doc.entries.size()) + " file entries were found.");

    return true;
}

juce::String lengthText (double lengthSeconds)
{
    if (! (lengthSeconds > 0.0))
        return "-1";
    return juce::String ((juce::int64) std::llround (lengthSeconds));
}
} // namespace

//==============================================================================
const char* formatName (Format format)
{
    switch (format)
    {
        case Format::m3u:   return "M3U";
        case Format::m3u8:  return "M3U8";
        case Format::pls:   return "PLS";
        case Format::unknown: break;
    }
    return "unknown";
}

//==============================================================================
bool readFileText (const juce::File& file, juce::String& textOut, juce::String& errorOut)
{
    textOut.clear();
    errorOut.clear();

    if (! file.existsAsFile())
    {
        errorOut = "Playlist file does not exist: " + file.getFullPathName();
        return false;
    }

    const auto size = file.getSize();
    if (size <= 0)
    {
        errorOut = "Playlist file is empty: " + file.getFullPathName();
        return false;
    }
    if (size > kMaxPlaylistFileBytes)
    {
        errorOut = "Playlist file is too large (limit 16 MB): " + file.getFullPathName();
        return false;
    }

    std::unique_ptr<juce::FileInputStream> stream (file.createInputStream());
    if (stream == nullptr || ! stream->openedOk())
    {
        errorOut = "Could not read playlist file: " + file.getFullPathName();
        return false;
    }

    juce::MemoryBlock bytes ((size_t) size);
    if (stream->read (bytes.getData(), (int) size) != (int) size)
    {
        errorOut = "Could not read playlist file: " + file.getFullPathName();
        return false;
    }

    const auto* data = static_cast<const juce::uint8*> (bytes.getData());
    const auto numBytes = (size_t) bytes.getSize();

    if (numBytes >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
    {
        textOut = juce::String::fromUTF8 (reinterpret_cast<const char*> (data + 3), (int) numBytes - 3);
        return true;
    }

    if (numBytes >= 2 && data[0] == 0xFF && data[1] == 0xFE)
    {
        const auto numChars = (numBytes - 2) / 2;
        if (numChars == 0)
        {
            textOut.clear();
            return true;
        }

        std::vector<juce::int16> utf16 ((size_t) numChars);
        for (size_t i = 0; i < numChars; ++i)
            utf16[i] = (juce::int16) (juce::uint16) (data[2 + i * 2] | (data[3 + i * 2] << 8));
        textOut = juce::String (juce::CharPointer_UTF16 (utf16.data()), (size_t) numChars);
        return true;
    }

    if (numBytes >= 2 && data[0] == 0xFE && data[1] == 0xFF)
    {
        const auto numChars = (numBytes - 2) / 2;
        if (numChars == 0)
        {
            textOut.clear();
            return true;
        }

        std::vector<juce::int16> utf16 ((size_t) numChars);
        for (size_t i = 0; i < numChars; ++i)
            utf16[i] = (juce::int16) (juce::uint16) ((data[2 + i * 2] << 8) | data[3 + i * 2]);
        textOut = juce::String (juce::CharPointer_UTF16 (utf16.data()), (size_t) numChars);
        return true;
    }

    textOut = juce::String::fromUTF8 (reinterpret_cast<const char*> (data), (int) numBytes);
    return true;
}

//==============================================================================
Format detectFormat (const juce::File& file, const juce::String& content)
{
    const auto extension = file.getFileExtension().toLowerCase();
    Format byExtension = Format::unknown;
    if (extension == ".m3u")       byExtension = Format::m3u;
    else if (extension == ".m3u8") byExtension = Format::m3u8;
    else if (extension == ".pls")  byExtension = Format::pls;

    if (content.isNotEmpty())
    {
        const auto head = content.substring (0, 4096);
        if (head.containsIgnoreCase ("[playlist]"))
            return Format::pls;
        if (head.containsIgnoreCase ("#EXTM3U"))
            return byExtension == Format::m3u8 ? Format::m3u8 : Format::m3u;
    }

    return byExtension;
}

bool parse (const juce::File& file, Document& doc, juce::String& errorOut)
{
    doc = {};
    errorOut.clear();

    juce::String text;
    if (! readFileText (file, text, errorOut))
        return false;

    const auto format = detectFormat (file, text);
    if (format == Format::unknown)
    {
        errorOut = "Unrecognised playlist format (expected .m3u, .m3u8 or .pls): "
                   + file.getFullPathName();
        return false;
    }

    doc.format = format;

    const auto lines = juce::StringArray::fromLines (text);
    if (format == Format::pls)
        parsePLS (lines, doc);
    else
        parseM3U (lines, doc);

    return true;
}

//==============================================================================
juce::String serialiseM3U8 (const std::vector<Entry>& entries)
{
    juce::String out;
    out << "#EXTM3U\n";

    for (const auto& entry : entries)
    {
        out << "#EXTINF:" << lengthText (entry.lengthSeconds) << ","
            << stripEmbeddedLineBreaks (entry.title) << "\n";
        out << stripEmbeddedLineBreaks (entry.path) << "\n";
    }

    return out;
}

juce::String serialiseM3UBasic (const std::vector<Entry>& entries)
{
    juce::String out;
    out << "#EXTM3U\n";

    for (const auto& entry : entries)
        out << stripEmbeddedLineBreaks (entry.path) << "\n";

    return out;
}

juce::String serialisePLS (const std::vector<Entry>& entries)
{
    juce::String out;
    out << "[playlist]\n";
    out << "NumberOfEntries=" << juce::String ((int) entries.size()) << "\n";

    for (size_t i = 0; i < entries.size(); ++i)
    {
        const auto n = juce::String ((int) i + 1);
        const auto& entry = entries[i];
        out << "File" << n << "=" << stripEmbeddedLineBreaks (entry.path) << "\n";

        const auto title = stripEmbeddedLineBreaks (entry.title);
        if (title.isNotEmpty())
            out << "Title" << n << "=" << title << "\n";

        out << "Length" << n << "=" << lengthText (entry.lengthSeconds) << "\n";
    }

    out << "Version=2\n";
    return out;
}

bool write (const juce::File& target, const std::vector<Entry>& entries, juce::String& errorOut)
{
    errorOut.clear();

    const auto format = detectFormat (target);
    if (format == Format::unknown)
    {
        errorOut = "Unsupported playlist file extension \"" + target.getFileExtension()
                   + "\"; use .m3u, .m3u8 or .pls.";
        return false;
    }

    juce::String text;
    switch (format)
    {
        case Format::m3u8: text = serialiseM3U8 (entries); break;
        case Format::m3u:  text = serialiseM3UBasic (entries); break;
        case Format::pls:  text = serialisePLS (entries); break;
        case Format::unknown: break;
    }

    auto parentResult = target.getParentDirectory().createDirectory();
    if (parentResult.failed())
    {
        errorOut = "Could not create playlist directory: " + target.getParentDirectory().getFullPathName()
                   + " (" + parentResult.getErrorMessage() + ")";
        return false;
    }

    if (! target.replaceWithText (text))
    {
        errorOut = "Could not write playlist file: " + target.getFullPathName();
        return false;
    }

    return true;
}

} // namespace sp::playlistio
