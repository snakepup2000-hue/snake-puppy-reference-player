/*
    Snake Puppy Reference Player
    Tests for Source/Library/Tags.cpp: format detection, tag reading for every
    fixture container, safe tag writing (audio payload byte-identical, proven
    by MD5), and malformed-file handling.

    All write tests operate on copies inside a temporary directory; the
    fixtures under Tests/Fixtures are never modified.
*/

#include "TestFramework.h"
#include "Library/Tags.h"

#include <cstring>
#include <functional>
#include <iostream>
#include <vector>

namespace
{

juce::File fixtureRoot()
{
    return juce::File (SP_SOURCE_DIR).getChildFile ("Tests/Fixtures");
}

juce::File fixture (const juce::String& relativePath)
{
    return fixtureRoot().getChildFile (relativePath);
}


struct TempDir
{
    juce::File dir;

    TempDir()
    {
        juce::String environmentTemp = juce::SystemStats::getEnvironmentVariable ("TMPDIR", {});

        if (environmentTemp.isEmpty())
            environmentTemp = juce::SystemStats::getEnvironmentVariable ("TEMP", {});

        juce::File root = environmentTemp.isNotEmpty() ? juce::File (environmentTemp)
                                                       : juce::File::getSpecialLocation (juce::File::tempDirectory);

        for (int attempt = 0; attempt < 2 && ! dir.isDirectory(); ++attempt)
        {
            juce::File base = attempt == 0 ? root
                                           : juce::File (SP_SOURCE_DIR).getChildFile ("build")
                                                 .getChildFile ("sp-tags-test-data");

            if (! base.isDirectory())
                base.createDirectory();

            if (! base.isDirectory())
                continue;

            auto candidate = base.getChildFile ("sp-tags-test-"
                                                + juce::String (juce::Time::currentTimeMillis()) + "-"
                                                + juce::String (juce::Random::getSystemRandom().nextInt (1000000)));

            if (candidate.createDirectory().wasOk())
                dir = candidate;
        }
    }
    ~TempDir()
    {
        if (dir.isDirectory())
            dir.deleteRecursively();
    }
};

juce::File copyToTemp (const juce::File& source, const TempDir& temp, const juce::String& name)
{
    SP_REQUIRE_MSG (source.existsAsFile(), source.getFullPathName().toStdString());

    auto destination = temp.dir.getChildFile (name);
    SP_REQUIRE_MSG (source.copyFileTo (destination), source.getFullPathName().toStdString());
    return destination;
}

juce::File writeTempBytes (const TempDir& temp, const juce::String& name,
                           const void* data, std::size_t size)
{
    auto file = temp.dir.getChildFile (name);
    juce::FileOutputStream out (file);
    SP_REQUIRE_MSG (out.openedOk(), file.getFullPathName().toStdString());
    SP_REQUIRE_MSG (size == 0 || out.write (data, (std::size_t) size), file.getFullPathName().toStdString());
    out.flush();
    return file;
}

juce::String hashOf (const juce::File& file)
{
    const auto hash = sp::tags::audioPayloadHash (file);
    SP_REQUIRE_MSG (hash.isNotEmpty(), file.getFullPathName().toStdString());
    return hash;
}

void printHashPair (const juce::String& label, const juce::String& before, const juce::String& after)
{
    std::cout << "    [audio-payload MD5] " << label << ": before=" << before.toStdString()
              << " after=" << after.toStdString() << "\n";
}

void requireTagEquals (const juce::String& actual, const juce::String& expected, const char* what)
{
    SP_REQUIRE_MSG (actual == expected, juce::String (what) + " was '" + actual + "', expected '" + expected + "'");
}

} // namespace

//==============================================================================
SP_TEST (Tags, DetectFormatNameForEveryFixture)
{
    struct Expectation
    {
        const char* path;
        const char* format;
    };

    const Expectation expectations[] = {
        { "Library/Album A/01 - Track One.flac", "FLAC" },
        { "Library/Album A/02 - Track Two.flac", "FLAC" },
        { "Library/Album A/03 - Track Three.mp3", "MP3" },
        { "Library/Album B/01 - Rhythm Section.wav", "WAV" },
        { "Library/Album B/02 - Big Room.aiff", "AIFF" },
        { "Library/Album B/03 - Slow Burn.m4a", "M4A" },
        { "Library/Album B/Disc 2/01 - Late Night.m4a", "M4A" },
        { "Library/Album B/04 - Pocket Groove.ogg", "OGG" },
        { "Library/DSD/01 - DSD Test Tone.dsf", "DSF" },
        { "Library/DSD/02 - DSD Test Tone.dff", "DFF" },
        { "Broken/broken.flac", "FLAC" },   // extension fallback; readTags reports the bad magic
        { "test-1k-44100.wav", "WAV" },
    };

    for (const auto& expectation : expectations)
    {
        const auto file = fixture (expectation.path);
        SP_REQUIRE_MSG (file.existsAsFile(), file.getFullPathName().toStdString());

        const auto detected = sp::tags::detectFormatName (file);
        SP_REQUIRE_MSG (detected == expectation.format,
                        juce::String (expectation.path) + " detected as '" + detected + "'");
    }
}

SP_TEST (Tags, DetectFormatNameByMagicBytes)
{
    TempDir temp;
    SP_REQUIRE_MSG (temp.dir.isDirectory(), "temp directory");

    // Copy real files to names with a misleading/unknown extension; the magic
    // bytes must win.
    const struct
    {
        const char* fixturePath;
        const char* tempName;
        const char* expected;
    } cases[] = {
        { "Library/Album A/01 - Track One.flac", "magic-flac.dat", "FLAC" },
        { "Library/Album A/03 - Track Three.mp3", "magic-mp3.dat", "MP3" },
        { "Library/Album B/01 - Rhythm Section.wav", "magic-wav.dat", "WAV" },
        { "Library/Album B/02 - Big Room.aiff", "magic-aiff.dat", "AIFF" },
        { "Library/Album B/03 - Slow Burn.m4a", "magic-m4a.dat", "M4A" },
        { "Library/Album B/04 - Pocket Groove.ogg", "magic-ogg.dat", "OGG" },
        { "Library/DSD/01 - DSD Test Tone.dsf", "magic-dsf.dat", "DSF" },
        { "Library/DSD/02 - DSD Test Tone.dff", "magic-dff.dat", "DFF" },
    };

    for (const auto& item : cases)
    {
        const auto copy = copyToTemp (fixture (item.fixturePath), temp, item.tempName);
        const auto detected = sp::tags::detectFormatName (copy);
        SP_REQUIRE_MSG (detected == item.expected,
                        juce::String (item.fixturePath) + " -> '" + detected + "'");
    }

    // A non-existent file with an unknown extension has no format.
    SP_REQUIRE (sp::tags::detectFormatName (temp.dir.getChildFile ("nope.xyz")).isEmpty());
}

//==============================================================================
SP_TEST (Tags, ReadFlacTags)
{
    for (const auto& expected : { std::pair<juce::String, juce::String> { "Library/Album A/01 - Track One.flac", "Track One" },
                                  std::pair<juce::String, juce::String> { "Library/Album A/02 - Track Two.flac", "Track Two" } })
    {
        sp::tags::TagData tags;
        juce::String error;
        const auto file = fixture (expected.first);

        SP_REQUIRE_MSG (sp::tags::readTags (file, tags, error), error.toStdString());
        SP_REQUIRE (tags.readOk);
        requireTagEquals (tags.formatName, "FLAC", "formatName");
        requireTagEquals (tags.title, expected.second, "title");
        requireTagEquals (tags.artist, "Test Artist", "artist");
        requireTagEquals (tags.albumArtist, "Test Artist", "albumArtist");
        requireTagEquals (tags.album, "Album A", "album");
        requireTagEquals (tags.genre, "Jazz", "genre");
        SP_REQUIRE_MSG (tags.year == 1975, juce::String (tags.year));
        SP_REQUIRE_MSG (tags.trackNumber == (expected.second == "Track One" ? 1 : 2), juce::String (tags.trackNumber));

        // Only the 24/96 fixture carries a DISCNUMBER comment.
        SP_REQUIRE_MSG (tags.discNumber == (expected.second == "Track One" ? 1 : 0),
                        juce::String (tags.discNumber));
        SP_REQUIRE (tags.composer.isEmpty());
    }
}

SP_TEST (Tags, ReadMp3Tags)
{
    sp::tags::TagData tags;
    juce::String error;

    SP_REQUIRE_MSG (sp::tags::readTags (fixture ("Library/Album A/03 - Track Three.mp3"), tags, error),
                    error.toStdString());

    requireTagEquals (tags.formatName, "MP3", "formatName");
    requireTagEquals (tags.title, "Track Three", "title");
    requireTagEquals (tags.artist, "Test Artist", "artist");
    requireTagEquals (tags.albumArtist, "Test Artist", "albumArtist");
    requireTagEquals (tags.album, "Album A", "album");
    requireTagEquals (tags.genre, "Soul", "genre");
    SP_REQUIRE_MSG (tags.year == 1975, juce::String (tags.year));
    SP_REQUIRE_MSG (tags.trackNumber == 3, juce::String (tags.trackNumber));
}

SP_TEST (Tags, ReadM4aTags)
{
    {
        sp::tags::TagData tags;
        juce::String error;

        SP_REQUIRE_MSG (sp::tags::readTags (fixture ("Library/Album B/03 - Slow Burn.m4a"), tags, error),
                        error.toStdString());

        requireTagEquals (tags.formatName, "M4A", "formatName");
        requireTagEquals (tags.title, "Slow Burn", "title");
        requireTagEquals (tags.artist, "Second Artist", "artist");
        requireTagEquals (tags.albumArtist, "Second Artist", "albumArtist");
        requireTagEquals (tags.album, "Album B", "album");
        requireTagEquals (tags.genre, "Classic R&B", "genre");
        SP_REQUIRE_MSG (tags.year == 1981, juce::String (tags.year));
        SP_REQUIRE_MSG (tags.trackNumber == 3, juce::String (tags.trackNumber));
    }

    {
        sp::tags::TagData tags;
        juce::String error;

        SP_REQUIRE_MSG (sp::tags::readTags (fixture ("Library/Album B/Disc 2/01 - Late Night.m4a"), tags, error),
                        error.toStdString());

        requireTagEquals (tags.title, "Late Night", "title");
        requireTagEquals (tags.album, "Album B/Disc 2", "album");
        requireTagEquals (tags.genre, "Modern R&B", "genre");
        SP_REQUIRE_MSG (tags.year == 2020, juce::String (tags.year));
        SP_REQUIRE_MSG (tags.trackNumber == 1, juce::String (tags.trackNumber));
    }
}

SP_TEST (Tags, ReadWavTags)
{
    sp::tags::TagData tags;
    juce::String error;

    SP_REQUIRE_MSG (sp::tags::readTags (fixture ("Library/Album B/01 - Rhythm Section.wav"), tags, error),
                    error.toStdString());

    requireTagEquals (tags.formatName, "WAV", "formatName");
    requireTagEquals (tags.title, "Rhythm Section", "title");
    requireTagEquals (tags.artist, "Second Artist", "artist");
    requireTagEquals (tags.album, "Album B", "album");
    requireTagEquals (tags.genre, "Funk", "genre");
    SP_REQUIRE_MSG (tags.year == 1981, juce::String (tags.year));
    SP_REQUIRE_MSG (tags.trackNumber == 1, juce::String (tags.trackNumber));
}

SP_TEST (Tags, ReadAiffTags)
{
    sp::tags::TagData tags;
    juce::String error;

    SP_REQUIRE_MSG (sp::tags::readTags (fixture ("Library/Album B/02 - Big Room.aiff"), tags, error),
                    error.toStdString());

    requireTagEquals (tags.formatName, "AIFF", "formatName");
    requireTagEquals (tags.title, "Big Room", "title");

    // The AIFF fixture only carries a NAME chunk (no AUTH/ANNO/ID3), so the
    // remaining fields must be empty rather than invented.
    SP_REQUIRE (tags.artist.isEmpty());
    SP_REQUIRE (tags.album.isEmpty());
}

SP_TEST (Tags, ReadOggTags)
{
    sp::tags::TagData tags;
    juce::String error;

    SP_REQUIRE_MSG (sp::tags::readTags (fixture ("Library/Album B/04 - Pocket Groove.ogg"), tags, error),
                    error.toStdString());

    requireTagEquals (tags.formatName, "OGG", "formatName");
    requireTagEquals (tags.title, "Pocket Groove", "title");
    requireTagEquals (tags.artist, "Third Artist", "artist");
    requireTagEquals (tags.albumArtist, "Third Artist", "albumArtist");
    requireTagEquals (tags.album, "Album B", "album");
    requireTagEquals (tags.genre, "Electronic", "genre");
    SP_REQUIRE_MSG (tags.year == 2005, juce::String (tags.year));
    SP_REQUIRE_MSG (tags.trackNumber == 4, juce::String (tags.trackNumber));
}

SP_TEST (Tags, ReadDsdTagsWithoutMetadata)
{
    for (const auto& path : { "Library/DSD/01 - DSD Test Tone.dsf", "Library/DSD/02 - DSD Test Tone.dff" })
    {
        sp::tags::TagData tags;
        juce::String error;

        SP_REQUIRE_MSG (sp::tags::readTags (fixture (path), tags, error), juce::String (path) + ": " + error);

        SP_REQUIRE (tags.readOk);
        SP_REQUIRE_MSG (tags.formatName == (juce::String (path).endsWithIgnoreCase (".dsf") ? "DSF" : "DFF"),
                        tags.formatName.toStdString());
        SP_REQUIRE (tags.title.isEmpty());
        SP_REQUIRE (tags.artist.isEmpty());
    }
}

//==============================================================================
SP_TEST (Tags, WriteFlacPreservesAudioPayload)
{
    TempDir temp;
    const auto file = copyToTemp (fixture ("Library/Album A/01 - Track One.flac"), temp, "write.flac");

    const auto hashBefore = hashOf (file);

    juce::String artError;
    const auto artworkBefore = sp::tags::readArtwork (file, artError);
    SP_REQUIRE_MSG (artworkBefore.size() == 1, artError.toStdString());

    sp::tags::TagData tags;
    tags.title = "Rewritten Title";
    tags.artist = "Rewritten Artist";
    tags.albumArtist = "Rewritten Album Artist";
    tags.album = "Rewritten Album";
    tags.genre = "Rock";
    tags.composer = "Rewritten Composer";
    tags.comment = "Rewritten Comment";
    tags.year = 2001;
    tags.trackNumber = 7;
    tags.discNumber = 2;
    tags.rating = 4;   // not representable in Vorbis comments; ignored on write

    juce::String error;
    SP_REQUIRE_MSG (sp::tags::writeTags (file, tags, error), error.toStdString());

    const auto hashAfter = hashOf (file);
    printHashPair ("FLAC write", hashBefore, hashAfter);
    SP_REQUIRE_MSG (hashBefore == hashAfter, "FLAC audio payload changed by tag write");

    sp::tags::TagData read;
    SP_REQUIRE_MSG (sp::tags::readTags (file, read, error), error.toStdString());
    requireTagEquals (read.title, "Rewritten Title", "title");
    requireTagEquals (read.artist, "Rewritten Artist", "artist");
    requireTagEquals (read.albumArtist, "Rewritten Album Artist", "albumArtist");
    requireTagEquals (read.album, "Rewritten Album", "album");
    requireTagEquals (read.genre, "Rock", "genre");
    requireTagEquals (read.composer, "Rewritten Composer", "composer");
    requireTagEquals (read.comment, "Rewritten Comment", "comment");
    SP_REQUIRE_MSG (read.year == 2001, juce::String (read.year));
    SP_REQUIRE_MSG (read.trackNumber == 7, juce::String (read.trackNumber));
    SP_REQUIRE_MSG (read.discNumber == 2, juce::String (read.discNumber));

    // FLAC/Vorbis has no standard star-rating field, so rating stays 0 there
    // (POPM ratings are honoured for ID3-based containers only).
    SP_REQUIRE_MSG (read.rating == 0, juce::String (read.rating));

    // The embedded front cover must survive a metadata rewrite.
    const auto artworkAfter = sp::tags::readArtwork (file, artError);
    SP_REQUIRE_MSG (artworkAfter.size() == 1, artError.toStdString());
    SP_REQUIRE (artworkAfter[0].data.getSize() == 1758);
    SP_REQUIRE (artworkAfter[0].isFrontCover());

    // A second rewrite must stay audio-neutral too.
    tags.title = "Second Write";
    SP_REQUIRE_MSG (sp::tags::writeTags (file, tags, error), error.toStdString());
    const auto hashSecond = hashOf (file);
    printHashPair ("FLAC second write", hashBefore, hashSecond);
    SP_REQUIRE_MSG (hashSecond == hashBefore, "FLAC audio payload changed by second tag write");

    SP_REQUIRE_MSG (sp::tags::readTags (file, read, error), error.toStdString());
    requireTagEquals (read.title, "Second Write", "title");
}

SP_TEST (Tags, WriteMp3PreservesAudioPayload)
{
    TempDir temp;
    const auto file = copyToTemp (fixture ("Library/Album A/03 - Track Three.mp3"), temp, "write.mp3");

    const auto hashBefore = hashOf (file);

    sp::tags::TagData tags;
    tags.title = "MP3 Rewritten";
    tags.artist = "New Artist";
    tags.albumArtist = "New Album Artist";
    tags.album = "New Album";
    tags.genre = "Electronic";
    tags.composer = "New Composer";
    tags.comment = "New Comment";
    tags.year = 1999;
    tags.trackNumber = 11;
    tags.discNumber = 3;
    tags.rating = 5;

    juce::String error;
    SP_REQUIRE_MSG (sp::tags::writeTags (file, tags, error), error.toStdString());

    const auto hashAfter = hashOf (file);
    printHashPair ("MP3 write", hashBefore, hashAfter);
    SP_REQUIRE_MSG (hashBefore == hashAfter, "MP3 audio payload changed by tag write");

    sp::tags::TagData read;
    SP_REQUIRE_MSG (sp::tags::readTags (file, read, error), error.toStdString());
    requireTagEquals (read.title, "MP3 Rewritten", "title");
    requireTagEquals (read.artist, "New Artist", "artist");
    requireTagEquals (read.albumArtist, "New Album Artist", "albumArtist");
    requireTagEquals (read.album, "New Album", "album");
    requireTagEquals (read.genre, "Electronic", "genre");
    requireTagEquals (read.composer, "New Composer", "composer");
    requireTagEquals (read.comment, "New Comment", "comment");
    SP_REQUIRE_MSG (read.year == 1999, juce::String (read.year));
    SP_REQUIRE_MSG (read.trackNumber == 11, juce::String (read.trackNumber));
    SP_REQUIRE_MSG (read.discNumber == 3, juce::String (read.discNumber));
    SP_REQUIRE_MSG (read.rating == 5, juce::String (read.rating));

    // Rewrite a second time, then confirm the payload is still identical.
    tags.title = "MP3 Second";
    SP_REQUIRE_MSG (sp::tags::writeTags (file, tags, error), error.toStdString());
    const auto hashSecond = hashOf (file);
    printHashPair ("MP3 second write", hashBefore, hashSecond);
    SP_REQUIRE_MSG (hashSecond == hashBefore, "MP3 audio payload changed by second tag write");
}

SP_TEST (Tags, WriteM4aPreservesMediaData)
{
    TempDir temp;
    const auto file = copyToTemp (fixture ("Library/Album B/03 - Slow Burn.m4a"), temp, "write.m4a");

    const auto hashBefore = hashOf (file);

    sp::tags::TagData tags;
    tags.title = "M4A Rewritten";
    tags.artist = "M4A Artist";
    tags.albumArtist = "M4A Album Artist";
    tags.album = "M4A Album";
    tags.genre = "Jazz";
    tags.composer = "M4A Composer";
    tags.comment = "M4A Comment";
    tags.year = 1987;
    tags.trackNumber = 9;
    tags.discNumber = 2;

    juce::String error;
    SP_REQUIRE_MSG (sp::tags::writeTags (file, tags, error), error.toStdString());

    const auto hashAfter = hashOf (file);
    printHashPair ("M4A write", hashBefore, hashAfter);
    SP_REQUIRE_MSG (hashBefore == hashAfter, "M4A mdat payload changed by tag write");

    sp::tags::TagData read;
    SP_REQUIRE_MSG (sp::tags::readTags (file, read, error), error.toStdString());
    requireTagEquals (read.title, "M4A Rewritten", "title");
    requireTagEquals (read.artist, "M4A Artist", "artist");
    requireTagEquals (read.albumArtist, "M4A Album Artist", "albumArtist");
    requireTagEquals (read.album, "M4A Album", "album");
    requireTagEquals (read.genre, "Jazz", "genre");
    requireTagEquals (read.composer, "M4A Composer", "composer");
    requireTagEquals (read.comment, "M4A Comment", "comment");
    SP_REQUIRE_MSG (read.year == 1987, juce::String (read.year));
    SP_REQUIRE_MSG (read.trackNumber == 9, juce::String (read.trackNumber));
    SP_REQUIRE_MSG (read.discNumber == 2, juce::String (read.discNumber));

    // Force a large moov growth (long title) to exercise the atom-size and
    // chunk-offset rewrite path, then confirm the mdat is still untouched.
    tags.title = juce::String::repeatedString ("Long Title ", 40);
    SP_REQUIRE_MSG (sp::tags::writeTags (file, tags, error), error.toStdString());
    const auto hashGrown = hashOf (file);
    printHashPair ("M4A grown-title write", hashBefore, hashGrown);
    SP_REQUIRE_MSG (hashGrown == hashBefore, "M4A mdat payload changed by grown tag write");

    SP_REQUIRE_MSG (sp::tags::readTags (file, read, error), error.toStdString());
    SP_REQUIRE_MSG (read.title.length() > 200, juce::String (read.title.length()));
}

SP_TEST (Tags, WriteWavPreservesDataChunk)
{
    TempDir temp;
    const auto file = copyToTemp (fixture ("Library/Album B/01 - Rhythm Section.wav"), temp, "write.wav");

    const auto hashBefore = hashOf (file);

    sp::tags::TagData tags;
    tags.title = "WAV Rewritten";
    tags.artist = "WAV Artist";
    tags.albumArtist = "WAV Album Artist";
    tags.album = "WAV Album";
    tags.genre = "Blues";
    tags.composer = "WAV Composer";
    tags.comment = "WAV Comment";
    tags.year = 1995;
    tags.trackNumber = 4;
    tags.discNumber = 1;
    tags.rating = 3;

    juce::String error;
    SP_REQUIRE_MSG (sp::tags::writeTags (file, tags, error), error.toStdString());

    const auto hashAfter = hashOf (file);
    printHashPair ("WAV write", hashBefore, hashAfter);
    SP_REQUIRE_MSG (hashBefore == hashAfter, "WAV data chunk changed by tag write");

    sp::tags::TagData read;
    SP_REQUIRE_MSG (sp::tags::readTags (file, read, error), error.toStdString());
    requireTagEquals (read.title, "WAV Rewritten", "title");
    requireTagEquals (read.artist, "WAV Artist", "artist");
    requireTagEquals (read.album, "WAV Album", "album");
    requireTagEquals (read.genre, "Blues", "genre");
    requireTagEquals (read.composer, "WAV Composer", "composer");
    requireTagEquals (read.comment, "WAV Comment", "comment");
    SP_REQUIRE_MSG (read.year == 1995, juce::String (read.year));
    SP_REQUIRE_MSG (read.trackNumber == 4, juce::String (read.trackNumber));
    SP_REQUIRE_MSG (read.rating == 3, juce::String (read.rating));
}

SP_TEST (Tags, WriteAiffPreservesSoundData)
{
    TempDir temp;
    const auto file = copyToTemp (fixture ("Library/Album B/02 - Big Room.aiff"), temp, "write.aiff");

    const auto hashBefore = hashOf (file);

    sp::tags::TagData tags;
    tags.title = "AIFF Rewritten";
    tags.artist = "AIFF Artist";
    tags.albumArtist = "AIFF Album Artist";
    tags.album = "AIFF Album";
    tags.genre = "Classical";
    tags.composer = "AIFF Composer";
    tags.comment = "AIFF Comment";
    tags.year = 1970;
    tags.trackNumber = 2;
    tags.discNumber = 2;

    juce::String error;
    SP_REQUIRE_MSG (sp::tags::writeTags (file, tags, error), error.toStdString());

    const auto hashAfter = hashOf (file);
    printHashPair ("AIFF write", hashBefore, hashAfter);
    SP_REQUIRE_MSG (hashBefore == hashAfter, "AIFF SSND payload changed by tag write");

    sp::tags::TagData read;
    SP_REQUIRE_MSG (sp::tags::readTags (file, read, error), error.toStdString());
    requireTagEquals (read.title, "AIFF Rewritten", "title");
    requireTagEquals (read.artist, "AIFF Artist", "artist");
    requireTagEquals (read.albumArtist, "AIFF Album Artist", "albumArtist");
    requireTagEquals (read.album, "AIFF Album", "album");
    requireTagEquals (read.genre, "Classical", "genre");
    requireTagEquals (read.composer, "AIFF Composer", "composer");
    requireTagEquals (read.comment, "AIFF Comment", "comment");
    SP_REQUIRE_MSG (read.year == 1970, juce::String (read.year));
    SP_REQUIRE_MSG (read.trackNumber == 2, juce::String (read.trackNumber));
}

SP_TEST (Tags, WriteOggPreservesAudioPackets)
{
    TempDir temp;
    const auto file = copyToTemp (fixture ("Library/Album B/04 - Pocket Groove.ogg"), temp, "write.ogg");

    const auto hashBefore = hashOf (file);

    sp::tags::TagData tags;
    tags.title = "OGG Rewritten";
    tags.artist = "OGG Artist";
    tags.albumArtist = "OGG Album Artist";
    tags.album = "OGG Album";
    tags.genre = "Ambient";
    tags.comment = "OGG Comment";
    tags.year = 2011;
    tags.trackNumber = 6;
    tags.discNumber = 1;

    juce::String error;
    SP_REQUIRE_MSG (sp::tags::writeTags (file, tags, error), error.toStdString());

    const auto hashAfter = hashOf (file);
    printHashPair ("OGG write", hashBefore, hashAfter);
    SP_REQUIRE_MSG (hashBefore == hashAfter, "OGG audio packets changed by tag write");

    sp::tags::TagData read;
    SP_REQUIRE_MSG (sp::tags::readTags (file, read, error), error.toStdString());
    requireTagEquals (read.title, "OGG Rewritten", "title");
    requireTagEquals (read.artist, "OGG Artist", "artist");
    requireTagEquals (read.albumArtist, "OGG Album Artist", "albumArtist");
    requireTagEquals (read.album, "OGG Album", "album");
    requireTagEquals (read.genre, "Ambient", "genre");
    requireTagEquals (read.comment, "OGG Comment", "comment");
    SP_REQUIRE_MSG (read.year == 2011, juce::String (read.year));
    SP_REQUIRE_MSG (read.trackNumber == 6, juce::String (read.trackNumber));
}

//==============================================================================
SP_TEST (Tags, WriteSkipsEmptyFields)
{
    TempDir temp;
    const auto file = copyToTemp (fixture ("Library/Album A/01 - Track One.flac"), temp, "empty-fields.flac");

    const auto hashBefore = hashOf (file);

    sp::tags::TagData onlyTitle;
    onlyTitle.title = "Only The Title";

    juce::String error;
    SP_REQUIRE_MSG (sp::tags::writeTags (file, onlyTitle, error), error.toStdString());

    const auto hashAfter = hashOf (file);
    printHashPair ("FLAC empty-field write", hashBefore, hashAfter);
    SP_REQUIRE_MSG (hashBefore == hashAfter, "audio payload changed by empty-field write");

    sp::tags::TagData read;
    SP_REQUIRE_MSG (sp::tags::readTags (file, read, error), error.toStdString());
    requireTagEquals (read.title, "Only The Title", "title");
    requireTagEquals (read.artist, "Test Artist", "artist");   // preserved, not cleared
    requireTagEquals (read.album, "Album A", "album");
    SP_REQUIRE_MSG (read.trackNumber == 1, juce::String (read.trackNumber));

    // A completely empty TagData must be a no-op that still succeeds.
    sp::tags::TagData empty;
    SP_REQUIRE_MSG (sp::tags::writeTags (file, empty, error), error.toStdString());
    SP_REQUIRE_MSG (hashOf (file) == hashBefore, "audio payload changed by empty write");
    SP_REQUIRE_MSG (sp::tags::readTags (file, read, error), error.toStdString());
    requireTagEquals (read.title, "Only The Title", "title");
}

//==============================================================================
SP_TEST (Tags, BrokenFilesReturnErrorsWithoutCrashing)
{
    juce::String error;
    sp::tags::TagData tags;

    // Completely invalid content with an audio extension.
    SP_REQUIRE (! sp::tags::readTags (fixture ("Broken/broken.flac"), tags, error));
    SP_REQUIRE_MSG (error.isNotEmpty(), "expected an error message for broken.flac");

    // Truncated FLAC: a valid header followed by nothing.
    {
        TempDir temp;
        auto source = fixture ("Library/Album A/01 - Track One.flac");
        juce::MemoryBlock head;
        SP_REQUIRE (source.loadFileAsData (head));
        const auto truncated = writeTempBytes (temp, "truncated.flac", head.getData(), 100);

        error.clear();
        SP_REQUIRE (! sp::tags::readTags (truncated, tags, error));
        SP_REQUIRE_MSG (error.isNotEmpty(), "expected an error message for truncated FLAC");
        std::cout << "    [broken] truncated FLAC error: " << error.toStdString() << "\n";

        juce::MemoryBlock payload;
        SP_REQUIRE (! sp::tags::extractAudioPayload (truncated, payload, error));
    }

    // ID3v2 header declaring a gigantic tag inside a tiny file.
    {
        TempDir temp;
        const std::uint8_t fakeMp3[] = { 'I', 'D', '3', 4, 0, 0, 0x7f, 0x7f, 0x7f, 0x7f,
                                         0xff, 0xfb, 0x90, 0x00 };
        const auto bad = writeTempBytes (temp, "bad.mp3", fakeMp3, sizeof (fakeMp3));

        error.clear();
        SP_REQUIRE (! sp::tags::readTags (bad, tags, error));
        SP_REQUIRE_MSG (error.isNotEmpty(), "expected an error message for truncated ID3 tag");
        std::cout << "    [broken] truncated ID3 error: " << error.toStdString() << "\n";

        // Writing must refuse rather than corrupt a file it cannot parse.
        sp::tags::TagData newTags;
        newTags.title = "Nope";
        error.clear();
        SP_REQUIRE (! sp::tags::writeTags (bad, newTags, error));
        SP_REQUIRE_MSG (error.isNotEmpty(), "expected an error when writing to a broken MP3");
    }

    // Artwork extraction on broken input reports an error and returns nothing.
    error.clear();
    const auto artwork = sp::tags::readArtwork (fixture ("Broken/broken.flac"), error);
    SP_REQUIRE (artwork.empty());
    SP_REQUIRE_MSG (error.isNotEmpty(), "expected an error message for broken artwork read");
}

//==============================================================================
// Hand-crafted ID3v2.2 / 2.3 / 2.4 tags (unsynchronisation, compression,
// footer) and DSD container writes.
//==============================================================================
namespace
{

using ByteVec = std::vector<std::uint8_t>;

void appendBE16 (ByteVec& b, std::uint16_t v)
{
    b.push_back ((std::uint8_t) (v >> 8));
    b.push_back ((std::uint8_t) (v & 0xff));
}

void appendBE32 (ByteVec& b, std::uint32_t v)
{
    for (int i = 3; i >= 0; --i)
        b.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xff));
}

ByteVec syncSafeBytes (std::uint32_t v)
{
    return { (std::uint8_t) ((v >> 21) & 0x7f), (std::uint8_t) ((v >> 14) & 0x7f),
             (std::uint8_t) ((v >> 7) & 0x7f), (std::uint8_t) (v & 0x7f) };
}

void appendSyncSafe (ByteVec& b, std::uint32_t v)
{
    const auto bytes = syncSafeBytes (v);
    b.insert (b.end(), bytes.begin(), bytes.end());
}

ByteVec latin1TextPayload (const char* text)
{
    ByteVec payload;
    payload.push_back (0);   // ISO-8859-1

    for (const char* p = text; *p != 0; ++p)
        payload.push_back ((std::uint8_t) *p);

    payload.push_back (0);
    return payload;
}

ByteVec utf16TextPayload (const juce::String& text)
{
    ByteVec payload;
    payload.push_back (1);   // UTF-16 with BOM
    payload.push_back (0xff);
    payload.push_back (0xfe);

    for (auto it = text.getCharPointer(); ! it.isEmpty(); ++it)
    {
        const auto codePoint = (std::uint32_t) *it;

        if (codePoint <= 0xffff)
        {
            // little-endian UTF-16 code unit
            payload.push_back ((std::uint8_t) (codePoint & 0xff));
            payload.push_back ((std::uint8_t) ((codePoint >> 8) & 0xff));
        }
        else
        {
            const std::uint32_t v = codePoint - 0x10000;
            const std::uint16_t high = (std::uint16_t) (0xd800 + (v >> 10));
            const std::uint16_t low = (std::uint16_t) (0xdc00 + (v & 0x3ff));
            payload.push_back ((std::uint8_t) (high & 0xff));
            payload.push_back ((std::uint8_t) ((high >> 8) & 0xff));
            payload.push_back ((std::uint8_t) (low & 0xff));
            payload.push_back ((std::uint8_t) ((low >> 8) & 0xff));
        }
    }

    payload.push_back (0);
    payload.push_back (0);
    return payload;
}

ByteVec makeId3FrameBytes (int major, const char* id, std::uint16_t flags, const ByteVec& payload)
{
    ByteVec frame;

    if (major == 2)
    {
        frame.push_back ((std::uint8_t) id[0]);
        frame.push_back ((std::uint8_t) id[1]);
        frame.push_back ((std::uint8_t) id[2]);
        frame.push_back ((std::uint8_t) ((payload.size() >> 16) & 0xff));
        frame.push_back ((std::uint8_t) ((payload.size() >> 8) & 0xff));
        frame.push_back ((std::uint8_t) (payload.size() & 0xff));
    }
    else
    {
        for (int i = 0; i < 4; ++i)
            frame.push_back ((std::uint8_t) id[i]);

        if (major == 4)
            appendSyncSafe (frame, (std::uint32_t) payload.size());
        else
            appendBE32 (frame, (std::uint32_t) payload.size());

        appendBE16 (frame, flags);
    }

    frame.insert (frame.end(), payload.begin(), payload.end());
    return frame;
}

ByteVec makeId3TagBytes (int major, std::uint8_t headerFlags, const ByteVec& body, bool footer = false)
{
    ByteVec tag;
    tag.push_back ('I');
    tag.push_back ('D');
    tag.push_back ('3');
    tag.push_back ((std::uint8_t) major);
    tag.push_back (0);
    tag.push_back (headerFlags);
    appendSyncSafe (tag, (std::uint32_t) body.size());
    tag.insert (tag.end(), body.begin(), body.end());

    if (footer)
    {
        tag.push_back ('3');
        tag.push_back ('D');
        tag.push_back ('I');
        tag.push_back ((std::uint8_t) major);
        tag.push_back (0);
        tag.push_back (headerFlags);
        appendSyncSafe (tag, (std::uint32_t) body.size());
    }

    return tag;
}

ByteVec unsynchroniseBytes (const ByteVec& input)
{
    ByteVec output;
    output.reserve (input.size());

    for (const auto byte : input)
    {
        output.push_back (byte);

        if (byte == 0xff)
            output.push_back (0x00);
    }

    return output;
}

ByteVec audioPayloadOf (const juce::File& file)
{
    juce::MemoryBlock block;
    juce::String error;
    SP_REQUIRE_MSG (sp::tags::extractAudioPayload (file, block, error), error.toStdString());

    ByteVec bytes ((std::size_t) block.getSize());
    std::memcpy (bytes.data(), block.getData(), block.getSize());
    return bytes;
}

ByteVec concatBytes (const ByteVec& a, const ByteVec& b)
{
    ByteVec out = a;
    out.insert (out.end(), b.begin(), b.end());
    return out;
}

juce::File writeBytesFile (const TempDir& temp, const juce::String& name, const ByteVec& bytes)
{
    return writeTempBytes (temp, name, bytes.data(), bytes.size());
}

} // namespace

SP_TEST (Tags, ReadAndWriteCraftedId3v23)
{
    TempDir temp;
    const auto audio = audioPayloadOf (fixture ("Library/Album A/03 - Track Three.mp3"));

    ByteVec body;
    const auto title = utf16TextPayload ("V23 Title");
    const auto artist = utf16TextPayload ("V23 Artist");
    const auto album = utf16TextPayload ("V23 Album");
    const auto add = [&body] (int major, const char* id, const ByteVec& payload)
    {
        const auto frame = makeId3FrameBytes (major, id, 0, payload);
        body.insert (body.end(), frame.begin(), frame.end());
    };

    add (3, "TIT2", title);
    add (3, "TPE1", artist);
    add (3, "TALB", album);
    add (3, "TYER", latin1TextPayload ("1969"));
    add (3, "TRCK", latin1TextPayload ("5"));

    const auto file = writeBytesFile (temp, "crafted-v23.mp3",
                                      concatBytes (makeId3TagBytes (3, 0, body), audio));

    juce::String error;
    sp::tags::TagData tags;
    SP_REQUIRE_MSG (sp::tags::readTags (file, tags, error), error.toStdString());
    requireTagEquals (tags.title, "V23 Title", "title");
    requireTagEquals (tags.artist, "V23 Artist", "artist");
    requireTagEquals (tags.album, "V23 Album", "album");
    SP_REQUIRE_MSG (tags.year == 1969, juce::String (tags.year));
    SP_REQUIRE_MSG (tags.trackNumber == 5, juce::String (tags.trackNumber));

    const auto hashBefore = hashOf (file);
    sp::tags::TagData update;
    update.title = "V23 Rewritten";
    SP_REQUIRE_MSG (sp::tags::writeTags (file, update, error), error.toStdString());
    SP_REQUIRE_MSG (hashOf (file) == hashBefore, "ID3v2.3 rewrite changed the audio payload");

    SP_REQUIRE_MSG (sp::tags::readTags (file, tags, error), error.toStdString());
    requireTagEquals (tags.title, "V23 Rewritten", "title");
    requireTagEquals (tags.artist, "V23 Artist", "artist");

    // A v2.3 source tag must be rewritten as v2.3, not silently upgraded.
    juce::MemoryBlock head;
    SP_REQUIRE (file.loadFileAsData (head));
    const auto* bytes = static_cast<const std::uint8_t*> (head.getData());
    SP_REQUIRE_MSG (bytes[3] == 3, juce::String ((int) bytes[3]));
}

SP_TEST (Tags, ReadAndWriteCraftedId3v22)
{
    TempDir temp;
    const auto audio = audioPayloadOf (fixture ("Library/Album A/03 - Track Three.mp3"));

    ByteVec body;
    const auto add = [&body] (const char* id, const ByteVec& payload)
    {
        const auto frame = makeId3FrameBytes (2, id, 0, payload);
        body.insert (body.end(), frame.begin(), frame.end());
    };

    add ("TT2", latin1TextPayload ("V22 Title"));
    add ("TP1", latin1TextPayload ("V22 Artist"));
    add ("TAL", latin1TextPayload ("V22 Album"));
    add ("TRK", latin1TextPayload ("3"));

    const auto file = writeBytesFile (temp, "crafted-v22.mp3",
                                      concatBytes (makeId3TagBytes (2, 0, body), audio));

    juce::String error;
    sp::tags::TagData tags;
    SP_REQUIRE_MSG (sp::tags::readTags (file, tags, error), error.toStdString());
    requireTagEquals (tags.title, "V22 Title", "title");
    requireTagEquals (tags.artist, "V22 Artist", "artist");
    requireTagEquals (tags.album, "V22 Album", "album");
    SP_REQUIRE_MSG (tags.trackNumber == 3, juce::String (tags.trackNumber));

    const auto hashBefore = hashOf (file);
    sp::tags::TagData update;
    update.title = "V22 Rewritten";
    SP_REQUIRE_MSG (sp::tags::writeTags (file, update, error), error.toStdString());
    SP_REQUIRE_MSG (hashOf (file) == hashBefore, "ID3v2.2 rewrite changed the audio payload");
    SP_REQUIRE_MSG (sp::tags::readTags (file, tags, error), error.toStdString());
    requireTagEquals (tags.title, "V22 Rewritten", "title");
    requireTagEquals (tags.artist, "V22 Artist", "artist");
}

SP_TEST (Tags, ReadUnsynchronisedId3v23)
{
    TempDir temp;
    const auto audio = audioPayloadOf (fixture ("Library/Album A/03 - Track Three.mp3"));

    const juce::String expectedTitle = juce::String::charToString ((juce::juce_wchar) 0xff) + " title";
    const auto frame = makeId3FrameBytes (3, "TIT2", 0, utf16TextPayload (expectedTitle));

    // Unsynchronise the whole tag body: every 0xFF is followed by 0x00.
    const auto unsyncedBody = unsynchroniseBytes (frame);
    const auto file = writeBytesFile (temp, "crafted-unsync.mp3",
                                      concatBytes (makeId3TagBytes (3, 0x80, unsyncedBody), audio));

    juce::String error;
    sp::tags::TagData tags;
    SP_REQUIRE_MSG (sp::tags::readTags (file, tags, error), error.toStdString());
    SP_REQUIRE_MSG (tags.title == expectedTitle,
                    juce::String ("unsynchronised title was '") + tags.title + "'");

    // And it must survive a rewrite with the same audio payload.
    const auto hashBefore = hashOf (file);
    sp::tags::TagData update;
    update.artist = "Unsync Artist";
    SP_REQUIRE_MSG (sp::tags::writeTags (file, update, error), error.toStdString());
    SP_REQUIRE_MSG (hashOf (file) == hashBefore, "unsynchronised tag rewrite changed the audio payload");
    SP_REQUIRE_MSG (sp::tags::readTags (file, tags, error), error.toStdString());
    requireTagEquals (tags.title, expectedTitle, "title");
    requireTagEquals (tags.artist, "Unsync Artist", "artist");
}

SP_TEST (Tags, ReadCompressedId3Frames)
{
    TempDir temp;
    const auto audio = audioPayloadOf (fixture ("Library/Album A/03 - Track Three.mp3"));

    // zlib.compress(b"\x00Compressed Title\x00") and
    // zlib.compress(b"\x00V23 Compressed\x00") generated offline.
    const ByteVec zlibV24 = { 0x78, 0x9c, 0x63, 0x70, 0xce, 0xcf, 0x2d, 0x28, 0x4a, 0x2d, 0x2e,
                              0x4e, 0x4d, 0x51, 0x08, 0xc9, 0x2c, 0xc9, 0x49, 0x65, 0x00, 0x00,
                              0x3b, 0x5d, 0x06, 0x38 };
    const ByteVec zlibV23 = { 0x78, 0x9c, 0x63, 0x08, 0x33, 0x32, 0x56, 0x70, 0xce, 0xcf, 0x2d,
                              0x28, 0x4a, 0x2d, 0x2e, 0x4e, 0x4d, 0x61, 0x00, 0x00, 0x25, 0xf7,
                              0x04, 0xf1 };

    // --- ID3v2.4: DLI + compression flags, with a footer -------------------
    {
        ByteVec framePayload;
        appendSyncSafe (framePayload, 18);   // decompressed size (data length indicator)
        framePayload.insert (framePayload.end(), zlibV24.begin(), zlibV24.end());

        const auto frame = makeId3FrameBytes (4, "TIT2", 0x0009, framePayload);
        const auto tagBytes = makeId3TagBytes (4, 0x10, frame, true);
        const auto file = writeBytesFile (temp, "crafted-compressed-v24.mp3",
                                          concatBytes (tagBytes, audio));

        juce::String error;
        sp::tags::TagData tags;
        SP_REQUIRE_MSG (sp::tags::readTags (file, tags, error), error.toStdString());
        requireTagEquals (tags.title, "Compressed Title", "title");

        const auto hashBefore = hashOf (file);
        sp::tags::TagData update;
        update.artist = "Compressed Artist";
        SP_REQUIRE_MSG (sp::tags::writeTags (file, update, error), error.toStdString());
        SP_REQUIRE_MSG (hashOf (file) == hashBefore, "compressed v2.4 rewrite changed the audio payload");
        SP_REQUIRE_MSG (sp::tags::readTags (file, tags, error), error.toStdString());
        requireTagEquals (tags.title, "Compressed Title", "title");
        requireTagEquals (tags.artist, "Compressed Artist", "artist");
    }

    // --- ID3v2.3: compressed frame -----------------------------------------
    {
        ByteVec framePayload;
        appendBE32 (framePayload, 16);   // decompressed size
        framePayload.insert (framePayload.end(), zlibV23.begin(), zlibV23.end());

        const auto frame = makeId3FrameBytes (3, "TIT2", 0x0080, framePayload);
        const auto file = writeBytesFile (temp, "crafted-compressed-v23.mp3",
                                          concatBytes (makeId3TagBytes (3, 0, frame), audio));

        juce::String error;
        sp::tags::TagData tags;
        SP_REQUIRE_MSG (sp::tags::readTags (file, tags, error), error.toStdString());
        requireTagEquals (tags.title, "V23 Compressed", "title");
    }
}

SP_TEST (Tags, WriteDsfAndDffPreserveAudioData)
{
    struct Case
    {
        const char* fixturePath;
        const char* tempName;
    };

    const Case cases[] = {
        { "Library/DSD/01 - DSD Test Tone.dsf", "write.dsf" },
        { "Library/DSD/02 - DSD Test Tone.dff", "write.dff" },
    };

    for (const auto& item : cases)
    {
        TempDir temp;
        const auto file = copyToTemp (fixture (item.fixturePath), temp, item.tempName);
        const auto hashBefore = hashOf (file);

        sp::tags::TagData tags;
        tags.title = "DSD Title";
        tags.artist = "DSD Artist";
        tags.album = "DSD Album";
        tags.genre = "Test";
        tags.year = 2024;
        tags.trackNumber = 1;

        juce::String error;
        SP_REQUIRE_MSG (sp::tags::writeTags (file, tags, error),
                        juce::String (item.fixturePath) + ": " + error.toStdString());

        const auto hashAfter = hashOf (file);
        printHashPair (juce::String ("DSD write ") + item.tempName, hashBefore, hashAfter);
        SP_REQUIRE_MSG (hashBefore == hashAfter,
                        juce::String (item.fixturePath) + ": DSD audio payload changed by tag write");

        sp::tags::TagData read;
        SP_REQUIRE_MSG (sp::tags::readTags (file, read, error),
                        juce::String (item.fixturePath) + ": " + error.toStdString());
        requireTagEquals (read.title, "DSD Title", "title");
        requireTagEquals (read.artist, "DSD Artist", "artist");
        requireTagEquals (read.album, "DSD Album", "album");
        SP_REQUIRE_MSG (read.year == 2024, juce::String (read.year));

        // A second write re-uses the tag region and must stay audio-neutral.
        tags.title = "DSD Title 2";
        SP_REQUIRE_MSG (sp::tags::writeTags (file, tags, error), error.toStdString());
        SP_REQUIRE_MSG (hashOf (file) == hashBefore,
                        juce::String (item.fixturePath) + ": second DSD write changed the audio payload");
        SP_REQUIRE_MSG (sp::tags::readTags (file, read, error), error.toStdString());
        requireTagEquals (read.title, "DSD Title 2", "title");
    }
}

SP_TEST (Tags, Mp4MoovBeforeMdatPatchesChunkOffsets)
{
    TempDir temp;

    // Build a minimal moov-before-mdat MP4 by hand: ftyp + moov(stco) + mdat.
    // The stco entry points at the mdat payload, so writing a bigger moov
    // forces the writer to re-point it.
    const auto makeAtom = [] (const char* type, const ByteVec& content) -> ByteVec
    {
        ByteVec atom;
        appendBE32 (atom, (std::uint32_t) (8 + content.size()));

        for (int i = 0; i < 4; ++i)
            atom.push_back ((std::uint8_t) type[i]);

        atom.insert (atom.end(), content.begin(), content.end());
        return atom;
    };

    ByteVec ftypContent;
    ftypContent.insert (ftypContent.end(), { 'M', '4', 'A', ' ' });
    appendBE32 (ftypContent, 0x200);
    ftypContent.insert (ftypContent.end(), { 'M', '4', 'A', ' ', 'i', 's', 'o', 'm', 'i', 's', 'o', '2' });
    const auto ftyp = makeAtom ("ftyp", ftypContent);

    ByteVec mdatPayload;
    for (int i = 0; i < 64; ++i)
        mdatPayload.push_back ((std::uint8_t) (i * 3 + 1));

    const auto buildMoov = [&] (std::uint32_t chunkOffset) -> ByteVec
    {
        ByteVec stcoContent;
        appendBE32 (stcoContent, 0);   // version/flags
        appendBE32 (stcoContent, 1);   // entry count
        appendBE32 (stcoContent, chunkOffset);
        const auto stco = makeAtom ("stco", stcoContent);
        const auto stbl = makeAtom ("stbl", stco);
        const auto minf = makeAtom ("minf", stbl);
        const auto mdia = makeAtom ("mdia", minf);
        const auto trak = makeAtom ("trak", mdia);
        return makeAtom ("moov", trak);
    };

    const auto firstMoov = buildMoov (0);
    const std::uint32_t expectedOldOffset = (std::uint32_t) (ftyp.size() + firstMoov.size() + 8);
    const auto moov = buildMoov (expectedOldOffset);
    SP_REQUIRE_MSG (moov.size() == firstMoov.size(), "moov size is stable across the stco patch");

    auto fileBytes = ftyp;
    fileBytes.insert (fileBytes.end(), moov.begin(), moov.end());
    // NOTE: build the mdat atom once; taking begin()/end() from two separate
    // temporaries was undefined behaviour and intermittently truncated the file.
    const auto mdat = makeAtom ("mdat", mdatPayload);
    fileBytes.insert (fileBytes.end(), mdat.begin(), mdat.end());

    const auto file = writeBytesFile (temp, "moov-first.m4a", fileBytes);

    juce::String error;
    sp::tags::TagData tags;
    SP_REQUIRE_MSG (sp::tags::readTags (file, tags, error), error.toStdString());

    const auto hashBefore = hashOf (file);

    sp::tags::TagData update;
    update.title = "Moov First Title";
    SP_REQUIRE_MSG (sp::tags::writeTags (file, update, error), error.toStdString());
    SP_REQUIRE_MSG (hashOf (file) == hashBefore, "moov-first tag write changed the mdat payload");

    SP_REQUIRE_MSG (sp::tags::readTags (file, tags, error), error.toStdString());
    requireTagEquals (tags.title, "Moov First Title", "title");

    // Walk the rewritten file: mdat must have shifted and stco must follow it.
    juce::MemoryBlock written;
    SP_REQUIRE (file.loadFileAsData (written));
    const auto* bytes = static_cast<const std::uint8_t*> (written.getData());
    const std::size_t total = (std::size_t) written.getSize();

    const auto readBE32 = [bytes] (std::size_t offset)
    {
        return ((std::uint32_t) bytes[offset] << 24) | ((std::uint32_t) bytes[offset + 1] << 16)
               | ((std::uint32_t) bytes[offset + 2] << 8) | (std::uint32_t) bytes[offset + 3];
    };

    std::size_t pos = 0;
    std::size_t mdatContentStart = 0;
    std::size_t stcoEntry = 0;
    bool foundStco = false;

    while (pos + 8 <= total)
    {
        const std::uint32_t atomSize = readBE32 (pos);
        const std::string type (reinterpret_cast<const char*> (bytes + pos + 4), 4);

        if (type == "mdat")
            mdatContentStart = pos + 8;

        if (type == "moov")
        {
            // Walk moov -> trak -> mdia -> minf -> stbl -> stco recursively.
            std::function<bool (std::size_t, std::size_t)> findStco =
                [&] (std::size_t start, std::size_t end) -> bool
            {
                std::size_t child = start;

                while (child + 8 <= end)
                {
                    const std::uint32_t childSize = readBE32 (child);
                    const std::string childType (reinterpret_cast<const char*> (bytes + child + 4), 4);

                    if (childType == "stco")
                    {
                        stcoEntry = readBE32 (child + 16);
                        return true;
                    }

                    if (childType == "trak" || childType == "mdia" || childType == "minf"
                        || childType == "stbl")
                    {
                        if (findStco (child + 8, child + childSize))
                            return true;
                    }

                    if (childSize < 8)
                        break;

                    child += childSize;
                }

                return false;
            };

            foundStco = findStco (pos + 8, pos + atomSize);
        }

        if (atomSize < 8)
            break;

        pos += atomSize;
    }

    SP_REQUIRE (foundStco);
    SP_REQUIRE_MSG (mdatContentStart > 0, "mdat atom not found");
    SP_REQUIRE_MSG (stcoEntry == (std::uint32_t) mdatContentStart,
                    juce::String ("stco=") + juce::String ((int) stcoEntry) + " mdat="
                        + juce::String ((int) mdatContentStart));
    SP_REQUIRE_MSG (stcoEntry != expectedOldOffset, "expected the moov growth to shift the mdat");

    std::cout << "    [MP4 moov-first] stco moved " << (int) expectedOldOffset << " -> " << (int) stcoEntry
              << ", mdat payload hash " << hashBefore.toStdString() << "\n";
}
