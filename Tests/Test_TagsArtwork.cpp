/*
    Snake Puppy Reference Player
    Tests for embedded artwork (APIC / FLAC PICTURE / MP4 covr / OGG
    METADATA_BLOCK_PICTURE) and ReplayGain read/write across containers.

    Every write test runs on a temp copy; fixtures are never modified.
*/

#include "TestFramework.h"
#include "Library/Tags.h"

#include <cstring>
#include <iostream>

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

juce::MemoryBlock loadBlock (const juce::File& file)
{
    juce::MemoryBlock block;
    SP_REQUIRE_MSG (file.loadFileAsData (block), file.getFullPathName().toStdString());
    return block;
}

bool blocksEqual (const juce::MemoryBlock& a, const juce::MemoryBlock& b)
{
    return a.getSize() == b.getSize()
           && (a.getSize() == 0 || std::memcmp (a.getData(), b.getData(), a.getSize()) == 0);
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

juce::MemoryBlock expectedCover (const char* name)
{
    return loadBlock (fixtureRoot().getChildFile ("Artwork").getChildFile (name));
}

} // namespace

//==============================================================================
SP_TEST (TagsArtwork, FlacArtworkExtraction)
{
    juce::String error;
    const auto artwork = sp::tags::readArtwork (fixture ("Library/Album A/01 - Track One.flac"), error);
    SP_REQUIRE_MSG (error.isEmpty(), error.toStdString());
    SP_REQUIRE_MSG (artwork.size() == 1, juce::String ((int) artwork.size()));

    const auto& cover = artwork[0];
    SP_REQUIRE (cover.isFrontCover());
    // ffmpeg stores the fixture cover with picture type 0 ("Other"); both 0 and
    // 3 are treated as front cover by EmbeddedArtwork::isFrontCover().
    SP_REQUIRE_MSG (cover.pictureType == 3 || cover.pictureType == 0, juce::String (cover.pictureType));
    SP_REQUIRE_MSG (cover.mimeType == "image/jpeg", cover.mimeType.toStdString());
    SP_REQUIRE_MSG (cover.width == 512, juce::String (cover.width));
    SP_REQUIRE_MSG (cover.height == 512, juce::String (cover.height));
    SP_REQUIRE_MSG (cover.data.getSize() == 1758, juce::String ((int) cover.data.getSize()));

    const auto* bytes = static_cast<const std::uint8_t*> (cover.data.getData());
    SP_REQUIRE (bytes != nullptr);
    SP_REQUIRE_MSG (bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff, "JPEG SOI marker");

    std::cout << "    [artwork] FLAC cover: " << (int) cover.data.getSize() << " bytes, " << cover.mimeType
              << ", " << cover.width << "x" << cover.height << "\n";
}

SP_TEST (TagsArtwork, FlacArtworkWriteRoundTrip)
{
    TempDir temp;
    const auto file = copyToTemp (fixture ("Library/Album A/01 - Track One.flac"), temp, "artwork.flac");

    const auto hashBefore = hashOf (file);
    const auto newCover = expectedCover ("cover2.jpg");
    SP_REQUIRE (newCover.getSize() == 1759);

    juce::String error;
    SP_REQUIRE_MSG (sp::tags::writeArtwork (file, newCover, "image/jpeg", error), error.toStdString());

    const auto hashAfter = hashOf (file);
    printHashPair ("FLAC artwork write", hashBefore, hashAfter);
    SP_REQUIRE_MSG (hashBefore == hashAfter, "FLAC audio payload changed by artwork write");

    const auto artwork = sp::tags::readArtwork (file, error);
    SP_REQUIRE_MSG (error.isEmpty(), error.toStdString());
    SP_REQUIRE_MSG (artwork.size() == 1, juce::String ((int) artwork.size()));
    SP_REQUIRE_MSG (artwork[0].isFrontCover(), "front cover first");
    SP_REQUIRE_MSG (blocksEqual (artwork[0].data, newCover), "written FLAC cover bytes round-trip");
    SP_REQUIRE_MSG (artwork[0].mimeType == "image/jpeg", artwork[0].mimeType.toStdString());
    SP_REQUIRE_MSG (artwork[0].width == 512, juce::String (artwork[0].width));

    // Replacing the cover a second time should not accumulate picture blocks.
    const auto originalCover = expectedCover ("cover.jpg");
    SP_REQUIRE_MSG (sp::tags::writeArtwork (file, originalCover, "", error), error.toStdString());
    const auto artwork2 = sp::tags::readArtwork (file, error);
    SP_REQUIRE_MSG (artwork2.size() == 1, juce::String ((int) artwork2.size()));
    SP_REQUIRE_MSG (blocksEqual (artwork2[0].data, originalCover), "replaced FLAC cover bytes");
    SP_REQUIRE_MSG (hashOf (file) == hashBefore, "FLAC audio payload changed by second artwork write");
}

SP_TEST (TagsArtwork, Mp3ArtworkWriteRead)
{
    TempDir temp;
    const auto file = copyToTemp (fixture ("Library/Album A/03 - Track Three.mp3"), temp, "artwork.mp3");

    juce::String error;
    SP_REQUIRE (sp::tags::readArtwork (file, error).empty());
    SP_REQUIRE_MSG (error.isEmpty(), error.toStdString());

    const auto hashBefore = hashOf (file);
    const auto cover = expectedCover ("cover.jpg");

    SP_REQUIRE_MSG (sp::tags::writeArtwork (file, cover, "image/jpeg", error), error.toStdString());

    const auto hashAfter = hashOf (file);
    printHashPair ("MP3 artwork write", hashBefore, hashAfter);
    SP_REQUIRE_MSG (hashBefore == hashAfter, "MP3 audio payload changed by artwork write");

    const auto artwork = sp::tags::readArtwork (file, error);
    SP_REQUIRE_MSG (error.isEmpty(), error.toStdString());
    SP_REQUIRE_MSG (artwork.size() == 1, juce::String ((int) artwork.size()));
    SP_REQUIRE (artwork[0].isFrontCover());
    SP_REQUIRE (blocksEqual (artwork[0].data, cover));
    SP_REQUIRE_MSG (artwork[0].width == 512 && artwork[0].height == 512,
                    juce::String (artwork[0].width) + "x" + juce::String (artwork[0].height));

    // Existing metadata must survive an artwork-only write.
    sp::tags::TagData tags;
    SP_REQUIRE_MSG (sp::tags::readTags (file, tags, error), error.toStdString());
    SP_REQUIRE_MSG (tags.title == "Track Three", tags.title.toStdString());
}

SP_TEST (TagsArtwork, OggArtworkWriteRead)
{
    TempDir temp;
    const auto file = copyToTemp (fixture ("Library/Album B/04 - Pocket Groove.ogg"), temp, "artwork.ogg");

    const auto hashBefore = hashOf (file);
    const auto cover = expectedCover ("cover.jpg");

    juce::String error;
    SP_REQUIRE_MSG (sp::tags::writeArtwork (file, cover, "image/jpeg", error), error.toStdString());
    SP_REQUIRE_MSG (hashOf (file) == hashBefore, "OGG audio packets changed by artwork write");

    const auto artwork = sp::tags::readArtwork (file, error);
    SP_REQUIRE_MSG (error.isEmpty(), error.toStdString());
    SP_REQUIRE_MSG (artwork.size() == 1, juce::String ((int) artwork.size()));
    SP_REQUIRE (blocksEqual (artwork[0].data, cover));
    SP_REQUIRE_MSG (artwork[0].mimeType == "image/jpeg", artwork[0].mimeType.toStdString());

    std::cout << "    [artwork] OGG cover written: " << (int) artwork[0].data.getSize()
              << " bytes, " << artwork[0].width << "x" << artwork[0].height << "\n";
}

SP_TEST (TagsArtwork, M4aArtworkWriteRead)
{
    TempDir temp;
    const auto file = copyToTemp (fixture ("Library/Album B/03 - Slow Burn.m4a"), temp, "artwork.m4a");

    const auto hashBefore = hashOf (file);
    const auto cover = expectedCover ("cover.jpg");

    juce::String error;
    SP_REQUIRE_MSG (sp::tags::writeArtwork (file, cover, "", error), error.toStdString());
    SP_REQUIRE_MSG (hashOf (file) == hashBefore, "M4A mdat changed by artwork write");

    const auto artwork = sp::tags::readArtwork (file, error);
    SP_REQUIRE_MSG (error.isEmpty(), error.toStdString());
    SP_REQUIRE_MSG (artwork.size() == 1, juce::String ((int) artwork.size()));
    SP_REQUIRE (blocksEqual (artwork[0].data, cover));
    SP_REQUIRE_MSG (artwork[0].mimeType == "image/jpeg", artwork[0].mimeType.toStdString());
}

SP_TEST (TagsArtwork, WavAndAiffArtworkWriteRead)
{
    const struct
    {
        const char* fixturePath;
        const char* tempName;
    } cases[] = {
        { "Library/Album B/01 - Rhythm Section.wav", "artwork.wav" },
        { "Library/Album B/02 - Big Room.aiff", "artwork.aiff" },
    };

    for (const auto& item : cases)
    {
        TempDir temp;
        const auto file = copyToTemp (fixture (item.fixturePath), temp, item.tempName);
        const auto hashBefore = hashOf (file);
        const auto cover = expectedCover ("cover2.jpg");

        juce::String error;
        SP_REQUIRE_MSG (sp::tags::writeArtwork (file, cover, "image/jpeg", error),
                        juce::String (item.fixturePath) + ": " + error.toStdString());
        SP_REQUIRE_MSG (hashOf (file) == hashBefore,
                        juce::String (item.fixturePath) + ": audio payload changed by artwork write");

        const auto artwork = sp::tags::readArtwork (file, error);
        SP_REQUIRE_MSG (error.isEmpty(), error.toStdString());
        SP_REQUIRE_MSG (artwork.size() == 1, juce::String ((int) artwork.size()));
        SP_REQUIRE_MSG (blocksEqual (artwork[0].data, cover),
                        juce::String (item.fixturePath) + ": artwork bytes round-trip");
    }
}

//==============================================================================
SP_TEST (TagsArtwork, ReplayGainRoundTrip)
{
    struct Case
    {
        const char* fixturePath;
        const char* tempName;
        float trackGain;
        float albumGain;
        float peak;
    };

    const Case cases[] = {
        { "Library/Album A/01 - Track One.flac", "rg.flac", -7.25f, -6.10f, 0.891234f },
        { "Library/Album A/03 - Track Three.mp3", "rg.mp3", -3.50f, -2.00f, 0.750000f },
        { "Library/Album B/03 - Slow Burn.m4a", "rg.m4a", -5.75f, -4.25f, 0.987654f },
        { "Library/Album B/01 - Rhythm Section.wav", "rg.wav", -1.50f, -1.25f, 0.500000f },
        { "Library/Album B/04 - Pocket Groove.ogg", "rg.ogg", -9.00f, -8.50f, 0.123456f },
    };

    for (const auto& item : cases)
    {
        TempDir temp;
        const auto file = copyToTemp (fixture (item.fixturePath), temp, item.tempName);
        const auto hashBefore = hashOf (file);

        juce::String error;
        SP_REQUIRE_MSG (sp::tags::writeReplayGain (file, item.trackGain, item.albumGain, item.peak, error),
                        juce::String (item.fixturePath) + ": " + error.toStdString());

        const auto hashAfter = hashOf (file);
        printHashPair (juce::String ("ReplayGain ") + item.tempName, hashBefore, hashAfter);
        SP_REQUIRE_MSG (hashBefore == hashAfter,
                        juce::String (item.fixturePath) + ": audio payload changed by ReplayGain write");

        float track = 0.0f, album = 0.0f, peak = 0.0f;
        bool hasTrack = false, hasAlbum = false, hasPeak = false;
        SP_REQUIRE_MSG (sp::tags::readReplayGain (file, track, album, peak, hasTrack, hasAlbum, hasPeak, error),
                        juce::String (item.fixturePath) + ": " + error.toStdString());

        SP_REQUIRE_MSG (hasTrack && hasAlbum && hasPeak, item.fixturePath);
        SP_REQUIRE_NEAR (track, item.trackGain, 0.01);
        SP_REQUIRE_NEAR (album, item.albumGain, 0.01);
        SP_REQUIRE_NEAR (peak, item.peak, 0.00001);

        // The same values must also surface through readTags for the library.
        sp::tags::TagData tags;
        SP_REQUIRE_MSG (sp::tags::readTags (file, tags, error), error.toStdString());
        SP_REQUIRE (tags.hasReplayGainTrack && tags.hasReplayGainAlbum && tags.hasPeak);
        SP_REQUIRE_NEAR (tags.replayGainTrackDb, item.trackGain, 0.01);

        std::cout << "    [ReplayGain] " << item.tempName << ": track=" << track << " dB album=" << album
                  << " dB peak=" << peak << "\n";

        // Rewriting tags keeps ReplayGain intact (it is merged, not dropped).
        sp::tags::TagData titleOnly;
        titleOnly.title = "RG Kept";
        SP_REQUIRE_MSG (sp::tags::writeTags (file, titleOnly, error), error.toStdString());
        SP_REQUIRE_MSG (sp::tags::readReplayGain (file, track, album, peak, hasTrack, hasAlbum, hasPeak, error),
                        error.toStdString());
        SP_REQUIRE_NEAR (track, item.trackGain, 0.01);
    }
}

SP_TEST (TagsArtwork, ReplayGainAbsentOnUntaggedFixture)
{
    sp::tags::TagData tags;
    juce::String error;
    SP_REQUIRE_MSG (sp::tags::readTags (fixture ("Library/Album A/01 - Track One.flac"), tags, error),
                    error.toStdString());
    SP_REQUIRE (! tags.hasReplayGainTrack);
    SP_REQUIRE (! tags.hasReplayGainAlbum);
    SP_REQUIRE (! tags.hasPeak);

    float track = 0.0f, album = 0.0f, peak = 0.0f;
    bool hasTrack = true, hasAlbum = true, hasPeak = true;
    SP_REQUIRE_MSG (sp::tags::readReplayGain (fixture ("Library/Album A/01 - Track One.flac"), track, album,
                                              peak, hasTrack, hasAlbum, hasPeak, error),
                    error.toStdString());
    SP_REQUIRE (! hasTrack && ! hasAlbum && ! hasPeak);
}

SP_TEST (TagsArtwork, RejectsUnsupportedImageData)
{
    TempDir temp;
    const auto file = copyToTemp (fixture ("Library/Album B/03 - Slow Burn.m4a"), temp, "bad-art.m4a");
    const auto hashBefore = hashOf (file);

    const std::uint8_t garbage[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    juce::MemoryBlock block (garbage, sizeof (garbage));

    juce::String error;
    SP_REQUIRE (! sp::tags::writeArtwork (file, block, "image/gif", error));
    SP_REQUIRE_MSG (error.isNotEmpty(), "expected an error for unsupported artwork format");
    SP_REQUIRE_MSG (hashOf (file) == hashBefore, "rejected artwork write must not modify the file");

    juce::MemoryBlock empty;
    SP_REQUIRE (! sp::tags::writeArtwork (file, empty, "image/jpeg", error));
    SP_REQUIRE_MSG (error.isNotEmpty(), "expected an error for empty artwork data");
}
