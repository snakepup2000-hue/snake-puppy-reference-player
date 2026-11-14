/*
    Profile engine persistence tests — learned preferences, overrides, user
    profiles and built-in edits survive new engine instances and database
    reopens. Also verifies parameter sanitising on load/save.
*/
#include "TestFramework.h"
#include "Test_ProfileHelpers.h"

#include <iostream>

SP_TEST (ProfilePersistence, LearnedGenrePersistsAcrossEngineInstances)
{
    sptest_profile::TempEnvironment env;
    const auto track = sptest_profile::makeTrack ("Jazz", "Artist", "Album", 11, "/tmp/learned.flac");

    {
        auto engine = env.makeEngine();
        SP_REQUIRE (engine->initialise().isEmpty());
        SP_REQUIRE (engine->isPersistenceAvailable());
        SP_REQUIRE (engine->learnFromUserEdit (track, sptest_profile::toneParams (2.5f), sp::LearnScope::genre));
        SP_REQUIRE (engine->getAllLearned().size() == 1);
    }

    {
        auto engine = env.makeEngine();
        SP_REQUIRE (engine->initialise().isEmpty());
        const auto learned = engine->getAllLearned();
        SP_REQUIRE_MSG (learned.size() == 1, (int) learned.size());
        SP_REQUIRE (learned[0].kind == sp::LearnedPreference::Kind::genre);
        SP_REQUIRE (learned[0].key == "jazz");
        SP_REQUIRE_NEAR ((double) learned[0].params.bass.gainDb, 2.5, 1.0e-4);

        // Applying it must be explained and must win over the built-in profile.
        engine->setMode (sp::EqMode::autoGenre);
        sp::ProfileDecision decision;
        const auto params = engine->resolveParams (track, nullptr, sp::DspParams {}, decision);
        SP_REQUIRE_NEAR ((double) params.bass.gainDb, 2.5, 1.0e-4);
        SP_REQUIRE (decision.learnedPreferenceApplied);
        SP_REQUIRE (decision.reasons.size() > 0);
        std::cout << "  learned genre applied: " << decision.explanation().toStdString() << "\n";
    }

    {
        auto engine = env.makeEngine();
        SP_REQUIRE (engine->initialise().isEmpty());
        SP_REQUIRE (engine->clearLearned (sp::LearnedPreference::Kind::genre, "jazz"));
        SP_REQUIRE (engine->getAllLearned().empty());
    }

    {
        auto engine = env.makeEngine();
        SP_REQUIRE (engine->initialise().isEmpty());
        SP_REQUIRE_MSG (engine->getAllLearned().empty(), "clearLearned must persist");
    }
}

SP_TEST (ProfilePersistence, OverridesPersistAcrossDatabaseReopen)
{
    sptest_profile::TempEnvironment env;
    const auto track = sptest_profile::makeTrack ("Soul", "Override Artist", "Override Album", 7, "/tmp/override.flac");

    {
        auto engine = env.makeEngine();
        SP_REQUIRE (engine->initialise().isEmpty());
        SP_REQUIRE (engine->setSongOverride (track, sptest_profile::toneParams (4.0f)));
        SP_REQUIRE (engine->setAlbumOverride (track, sptest_profile::toneParams (2.0f)));
        SP_REQUIRE (engine->hasSongOverride (7));
        SP_REQUIRE (engine->hasAlbumOverride (track));
        SP_REQUIRE (engine->getAllOverrides().size() == 2);
    }

    SP_REQUIRE (env.reopenDatabase());

    {
        auto engine = env.makeEngine();
        SP_REQUIRE (engine->initialise().isEmpty());
        SP_REQUIRE_MSG (engine->hasSongOverride (7), "song override must survive a database reopen");
        SP_REQUIRE_MSG (engine->hasAlbumOverride (track), "album override must survive a database reopen");
        SP_REQUIRE_NEAR ((double) engine->getSongOverride (7).bass.gainDb, 4.0, 1.0e-4);
        SP_REQUIRE_NEAR ((double) engine->getAlbumOverride (track).bass.gainDb, 2.0, 1.0e-4);

        engine->clearSongOverride (7);
        engine->clearAlbumOverride (track);
        SP_REQUIRE (! engine->hasSongOverride (7));
        SP_REQUIRE (! engine->hasAlbumOverride (track));
        SP_REQUIRE (engine->getAllOverrides().empty());
    }

    SP_REQUIRE (env.reopenDatabase());

    {
        auto engine = env.makeEngine();
        SP_REQUIRE (engine->initialise().isEmpty());
        SP_REQUIRE_MSG (! engine->hasSongOverride (7), "cleared song override must stay cleared");
        SP_REQUIRE_MSG (! engine->hasAlbumOverride (track), "cleared album override must stay cleared");
    }
}

SP_TEST (ProfilePersistence, UserProfilesAndBuiltInEdits)
{
    sptest_profile::TempEnvironment env;

    {
        auto engine = env.makeEngine();
        SP_REQUIRE (engine->initialise().isEmpty());
        SP_REQUIRE (engine->getUserProfiles().empty());
        SP_REQUIRE (engine->getAllProfiles().size() == 11);

        // 1. User profile round-trip.
        sp::EqProfile user;
        user.id = "user:test-curve";
        user.name = "My Curve";
        user.genre = "jazz";
        user.description = "Test curve";
        user.params = sp::ProfileEngine::flatReferenceParams();
        user.params.bass.gainDb = 1.5f;
        SP_REQUIRE (engine->saveUserProfile (user));
        SP_REQUIRE (engine->getUserProfiles().size() == 1);
        SP_REQUIRE (engine->getAllProfiles().size() == 12);
        SP_REQUIRE_NEAR ((double) engine->getProfile ("user:test-curve").params.bass.gainDb, 1.5, 1.0e-4);

        // 2. Editing a built-in stores an override, not a new profile.
        auto jazz = engine->getProfile ("builtin:jazz");
        SP_REQUIRE (jazz.id == "builtin:jazz");
        jazz.name = "Jazz (my edit)";
        jazz.params.bass.gainDb = 2.0f;
        SP_REQUIRE (engine->saveUserProfile (jazz));
        SP_REQUIRE (engine->hasBuiltInEdit ("builtin:jazz"));
        SP_REQUIRE (engine->getAllProfiles().size() == 12);   // 11 built-ins (one edited) + 1 user
        SP_REQUIRE (engine->getProfile ("builtin:jazz").name == "Jazz (my edit)");
        SP_REQUIRE_NEAR ((double) engine->getProfile ("builtin:jazz").params.bass.gainDb, 2.0, 1.0e-4);
        // The factory profile for genre matching now reflects the edit.
        SP_REQUIRE_NEAR ((double) engine->getProfileForGenre ("Jazz").params.bass.gainDb, 2.0, 1.0e-4);
    }

    SP_REQUIRE (env.reopenDatabase());

    {
        auto engine = env.makeEngine();
        SP_REQUIRE (engine->initialise().isEmpty());
        SP_REQUIRE_MSG (engine->hasBuiltInEdit ("builtin:jazz"), "built-in edit must persist");
        SP_REQUIRE (engine->getProfile ("builtin:jazz").name == "Jazz (my edit)");
        SP_REQUIRE (engine->getUserProfiles().size() == 1);

        // Reset the built-in edit -> factory defaults return.
        SP_REQUIRE (engine->resetBuiltInProfile ("builtin:jazz"));
        SP_REQUIRE (! engine->hasBuiltInEdit ("builtin:jazz"));
        SP_REQUIRE (engine->getProfile ("builtin:jazz").name == "Jazz");
        SP_REQUIRE_NEAR ((double) engine->getProfile ("builtin:jazz").params.bass.gainDb, 0.5, 1.0e-4);
        SP_REQUIRE (! engine->resetBuiltInProfile ("builtin:jazz"));    // nothing left to reset

        // Delete the user profile.
        SP_REQUIRE (engine->deleteUserProfile ("user:test-curve"));
        SP_REQUIRE (engine->getUserProfiles().empty());
        SP_REQUIRE (engine->getAllProfiles().size() == 11);
    }

    SP_REQUIRE (env.reopenDatabase());

    {
        auto engine = env.makeEngine();
        SP_REQUIRE (engine->initialise().isEmpty());
        SP_REQUIRE_MSG (! engine->hasBuiltInEdit ("builtin:jazz"), "reset built-in must stay reset");
        SP_REQUIRE_MSG (engine->getUserProfiles().empty(), "deleted user profile must stay deleted");
        SP_REQUIRE (engine->getProfile ("builtin:jazz").name == "Jazz");
    }
}

SP_TEST (ProfilePersistence, SavedParamsAreClamped)
{
    sptest_profile::TempEnvironment env;
    auto engine = env.makeEngine();
    SP_REQUIRE (engine->initialise().isEmpty());

    sp::EqProfile wild;
    wild.id = "user:wild";
    wild.name = "Wild";
    wild.params = sp::ProfileEngine::flatReferenceParams();
    wild.params.bass.gainDb = 99.0f;
    wild.params.bass.shelfFreqHz = 5000.0;
    wild.params.parametric.bands[0].enabled = true;
    wild.params.parametric.bands[0].gainDb = 80.0;
    wild.params.parametric.bands[0].freqHz = -10.0;
    wild.params.parametric.bands[0].q = 0.0;
    wild.params.stereo.widthPercent = 900.0f;
    wild.params.stereo.balance = 5.0f;

    SP_REQUIRE (engine->saveUserProfile (wild));

    const auto saved = engine->getProfile ("user:wild");
    SP_REQUIRE (saved.id == "user:wild");
    SP_REQUIRE_LE ((double) saved.params.bass.gainDb, 6.0);
    SP_REQUIRE_GE ((double) saved.params.bass.gainDb, -6.0);
    SP_REQUIRE_LE (saved.params.bass.shelfFreqHz, 500.0);
    SP_REQUIRE (std::abs (saved.params.parametric.bands[0].gainDb) <= 24.0);
    SP_REQUIRE_GE (saved.params.parametric.bands[0].freqHz, 20.0);
    SP_REQUIRE_GE (saved.params.parametric.bands[0].q, 0.1);
    SP_REQUIRE_LE ((double) saved.params.stereo.widthPercent, 200.0);
    SP_REQUIRE_LE (std::abs ((double) saved.params.stereo.balance), 1.0);

    std::cout << "  clamped: bass=" << saved.params.bass.gainDb
              << " dB shelf=" << saved.params.bass.shelfFreqHz
              << " bandGain=" << saved.params.parametric.bands[0].gainDb
              << " bandFreq=" << saved.params.parametric.bands[0].freqHz
              << " width=" << saved.params.stereo.widthPercent << " %\n";

    // And after a reload from JSON as well.
    SP_REQUIRE (env.reopenDatabase());
    auto engine2 = env.makeEngine();
    SP_REQUIRE (engine2->initialise().isEmpty());
    const auto reloaded = engine2->getProfile ("user:wild");
    SP_REQUIRE (reloaded.id == "user:wild");
    SP_REQUIRE_LE (std::abs ((double) reloaded.params.stereo.balance), 1.0);
}
