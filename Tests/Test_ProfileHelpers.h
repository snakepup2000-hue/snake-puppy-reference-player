/*
    Shared helpers for the profile / genre-intelligence tests.
    Tests get their own temporary application root and SQLite database so they
    never touch the user's real library.
*/
#pragma once

#include "Core/ApplicationPaths.h"
#include "Library/LibraryDatabase.h"
#include "Library/LibraryService.h"
#include "Profile/ProfileEngine.h"

#include <memory>

namespace sptest_profile
{

inline juce::File fixtureFile (const juce::String& relativePath)
{
    return juce::File (juce::String (SP_SOURCE_DIR))
        .getChildFile ("Tests/Fixtures")
        .getChildFile (relativePath);
}

struct TempEnvironment
{
    juce::File root;
    sp::ApplicationPaths paths;
    sp::LibraryDatabase db;
    std::unique_ptr<sp::LibraryService> library;

    TempEnvironment()
    {
        // Tests must run inside the writable workspace (the user temp directory
        // may be sandboxed), so the sandbox lives under build/profile-tests.
        auto base = juce::File (juce::String (SP_SOURCE_DIR))
                        .getChildFile ("build").getChildFile ("profile-tests");
        base.createDirectory();
        root = base.getChildFile ("run-"
                                  + juce::String (juce::Time::currentTimeMillis())
                                  + "-" + juce::String (juce::Random::getSystemRandom().nextInt (1000000)));
        if (root.createDirectory().failed())
            root = juce::File::getCurrentWorkingDirectory().getChildFile ("profile-tests-tmp");
        root.createDirectory();
        paths = sp::ApplicationPaths::forRoot (root);

        juce::String error;
        paths.ensureDirectoriesExist (error);
        db.open (paths.libraryDb);
        db.initialiseSchema();
        library = std::make_unique<sp::LibraryService> (paths, db);
    }

    ~TempEnvironment()
    {
        library.reset();
        db.close();
        root.deleteRecursively();
    }

    std::unique_ptr<sp::ProfileEngine> makeEngine()
    {
        return std::make_unique<sp::ProfileEngine> (db, *library);
    }

    bool reopenDatabase()
    {
        db.close();
        if (db.open (paths.libraryDb).isNotEmpty())
            return false;
        return db.initialiseSchema().isEmpty();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TempEnvironment)
};

inline sp::Track makeTrack (const juce::String& genre,
                            const juce::String& albumArtist = "Test Artist",
                            const juce::String& album = "Test Album",
                            sp::TrackId id = 1,
                            const juce::String& path = "/tmp/sp-profile-test.flac")
{
    sp::Track t;
    t.id = id;
    t.genre = genre;
    t.albumArtist = albumArtist;
    t.album = album;
    t.path = path;
    t.title = "Test Track";
    return t;
}

inline sp::DspParams toneParams (float bassGainDb)
{
    auto p = sp::ProfileEngine::flatReferenceParams();
    p.bass.enabled = true;
    p.bass.gainDb = bassGainDb;
    return p;
}

} // namespace sptest_profile
