/*
    Profile engine tests — decisions, genre matching, analysis-assisted
    refinement, precedence, manual/reference modes and explainability.
*/
#include "TestFramework.h"
#include "Test_ProfileHelpers.h"

#include <iostream>

namespace
{
juce::String joinReasons (const sp::ProfileDecision& d)
{
    juce::StringArray parts;
    for (auto& r : d.reasons)
        parts.add (r);
    return parts.joinIntoString (" | ");
}

void printDecision (const char* label, const sp::ProfileDecision& d)
{
    std::cout << "    " << label << ": mode=" << sp::eqModeName (d.mode)
              << " profile=\"" << d.profileName.toStdString() << "\""
              << " id=" << d.profileId.toStdString()
              << " analysis=" << (d.usedAudioAnalysis ? "yes" : "no")
              << " learned=" << (d.learnedPreferenceApplied ? "yes" : "no")
              << " override=" << d.overrideKind.toStdString() << "\n"
              << "      " << d.explanation().toStdString() << "\n";
}

sp::TrackAnalysis brightHeavyAnalysis()
{
    sp::TrackAnalysis a;
    a.valid = true;
    a.version = sp::TrackAnalysis::kVersion;
    a.analyzedSeconds = 120.0;
    a.integratedLufs = -8.5;
    a.loudnessRangeLu = 3.2;
    a.samplePeakDb = -0.1;
    a.truePeakDb = -0.05;
    a.crestFactorDb = 8.0;
    a.spectralCentroidHz = 3800.0;
    a.lowEnergyRatio = 0.34;
    a.subEnergyRatio = 0.18;
    a.midEnergyRatio = 0.55;
    a.highEnergyRatio = 0.13;
    a.lowBandDb = -12.0;
    a.midBandDb = -15.0;
    a.highBandDb = -24.0;
    return a;
}
} // namespace

SP_TEST (ProfileDecisions, AutoGenreMatchesMetadata)
{
    sptest_profile::TempEnvironment env;
    auto engine = env.makeEngine();
    SP_REQUIRE (engine->initialise().isEmpty());
    engine->setMode (sp::EqMode::autoGenre);

    struct Case { const char* genre; const char* expectedId; };
    const Case cases[] = {
        { "Jazz", "builtin:jazz" },
        { "Soul", "builtin:soul" },
        { "Funk", "builtin:classic-funk" },
        { "Classic R&B", "builtin:classic-rnb" },
        { "Modern R&B", "builtin:modern-rnb" },
        { "Electronic", "builtin:electronic" },
        { "Hip Hop", "builtin:modern-hiphop" },
        { "Rap", "builtin:modern-hiphop" },
        { "Old-School Hip-Hop", "builtin:old-school-hiphop" },
        { "Rock", "builtin:rock" },
        { "Pop", "builtin:pop" },
    };

    for (auto& c : cases)
    {
        const auto track = sptest_profile::makeTrack (c.genre);
        const auto decision = engine->decide (track, nullptr);
        SP_REQUIRE_MSG (decision.profileId == c.expectedId,
                        (juce::String (c.genre) + " -> " + decision.profileId).toStdString());
        SP_REQUIRE (decision.mode == sp::EqMode::autoGenre);
        SP_REQUIRE_MSG (! decision.reasons.empty(), c.genre);
        SP_REQUIRE (decision.matchedGenre == juce::String (c.genre));
        SP_REQUIRE (! decision.usedAudioAnalysis);
        SP_REQUIRE (decision.explanation().isNotEmpty());
        printDecision (c.genre, decision);
    }
}

SP_TEST (ProfileDecisions, UnknownAndMissingGenreFallBackToFlat)
{
    sptest_profile::TempEnvironment env;
    auto engine = env.makeEngine();
    SP_REQUIRE (engine->initialise().isEmpty());
    engine->setMode (sp::EqMode::autoGenre);

    auto unknown = sptest_profile::makeTrack ("Polka");
    auto d = engine->decide (unknown, nullptr);
    SP_REQUIRE (d.profileId == "builtin:flat");
    SP_REQUIRE (d.reasons.size() > 0);
    SP_REQUIRE_MSG (joinReasons (d).containsIgnoreCase ("no matching profile"),
                    joinReasons (d).toStdString());
    printDecision ("unknown genre", d);

    auto missing = sptest_profile::makeTrack ("");
    auto d2 = engine->decide (missing, nullptr);
    SP_REQUIRE (d2.profileId == "builtin:flat");
    SP_REQUIRE_MSG (joinReasons (d2).containsIgnoreCase ("no genre tag"),
                    joinReasons (d2).toStdString());
    printDecision ("missing genre", d2);

    SP_REQUIRE (engine->findProfileIdForGenre ("Polka") == "builtin:flat");
    SP_REQUIRE (engine->getProfileForGenre ("Polka").id == "builtin:flat");
    SP_REQUIRE (engine->findProfileIdForGenre ("Jazz") == "builtin:jazz");
}

SP_TEST (ProfileDecisions, IntelligentRefinesBassUsingMeasurements)
{
    sptest_profile::TempEnvironment env;
    auto engine = env.makeEngine();
    SP_REQUIRE (engine->initialise().isEmpty());

    const auto track = sptest_profile::makeTrack ("Classic R&B");
    const sp::DspParams manualParams;   // default playback base

    engine->setMode (sp::EqMode::autoGenre);
    sp::ProfileDecision autoDecision;
    const auto autoParams = engine->resolveParams (track, nullptr, manualParams, autoDecision);

    const auto analysis = brightHeavyAnalysis();
    engine->setMode (sp::EqMode::intelligent);
    sp::ProfileDecision intDecision;
    const auto intParams = engine->resolveParams (track, &analysis, manualParams, intDecision);

    SP_REQUIRE (! autoDecision.usedAudioAnalysis);
    SP_REQUIRE (intDecision.usedAudioAnalysis);
    SP_REQUIRE_GT ((double) autoParams.bass.gainDb, (double) intParams.bass.gainDb);

    const auto reasons = joinReasons (intDecision);
    SP_REQUIRE_MSG (reasons.contains ("34%"), reasons.toStdString());
    SP_REQUIRE_MSG (reasons.containsIgnoreCase ("bass"), reasons.toStdString());
    SP_REQUIRE_MSG (reasons.contains ("Crest factor"), reasons.toStdString());

    // A/B differences: bass and high shelf must both move.
    const auto diffs = sp::ProfileEngine::compare (autoParams, intParams);
    SP_REQUIRE_MSG (diffs.size() >= 2, (int) diffs.size());

    printDecision ("auto (metadata only)", autoDecision);
    printDecision ("intelligent (analysis)", intDecision);
    std::cout << "    bass: auto=" << autoParams.bass.gainDb
              << " dB  intelligent=" << intParams.bass.gainDb << " dB\n";
    for (auto& d : diffs)
        std::cout << "    diff: " << d.label.toStdString() << "  " << d.a.toStdString()
                  << " -> " << d.b.toStdString() << "  (" << d.deltaDb << " dB)\n";

    // Without analysis the intelligent mode must fall back and say so.
    sp::ProfileDecision fallbackDecision;
    const auto fallbackParams = engine->resolveParams (track, nullptr, manualParams, fallbackDecision);
    SP_REQUIRE (! fallbackDecision.usedAudioAnalysis);
    SP_REQUIRE_NEAR ((double) fallbackParams.bass.gainDb, (double) autoParams.bass.gainDb, 1.0e-6);
    SP_REQUIRE_MSG (joinReasons (fallbackDecision).containsIgnoreCase ("No measured audio analysis"),
                    joinReasons (fallbackDecision).toStdString());
    printDecision ("intelligent without analysis", fallbackDecision);

    // Invalid analysis is treated as missing data.
    sp::TrackAnalysis invalid;
    invalid.valid = false;
    sp::ProfileDecision invalidDecision;
    engine->resolveParams (track, &invalid, manualParams, invalidDecision);
    SP_REQUIRE (! invalidDecision.usedAudioAnalysis);
}

SP_TEST (ProfileDecisions, PrecedenceSongAlbumLearnedProfile)
{
    sptest_profile::TempEnvironment env;
    auto engine = env.makeEngine();
    SP_REQUIRE (engine->initialise().isEmpty());
    engine->setMode (sp::EqMode::autoGenre);

    const auto track = sptest_profile::makeTrack ("Jazz", "Precedence Artist", "Precedence Album", 42, "/tmp/prec.flac");
    const sp::DspParams manualParams;

    sp::ProfileDecision profileDecision;
    const auto profileParams = engine->resolveParams (track, nullptr, manualParams, profileDecision);
    const double profileBass = profileParams.bass.gainDb;
    printDecision ("profile", profileDecision);

    // Learned genre (Jazz) -> 3.0 dB.
    SP_REQUIRE (engine->learnFromUserEdit (track, sptest_profile::toneParams (3.0f), sp::LearnScope::genre));
    sp::ProfileDecision learnedGenreDecision;
    const auto learnedGenreParams = engine->resolveParams (track, nullptr, manualParams, learnedGenreDecision);
    SP_REQUIRE_NEAR ((double) learnedGenreParams.bass.gainDb, 3.0, 1.0e-4);
    SP_REQUIRE (learnedGenreDecision.learnedPreferenceApplied);
    SP_REQUIRE (learnedGenreDecision.reasons.size() > 0);
    printDecision ("learned genre", learnedGenreDecision);

    // Learned album -> 3.5 dB beats learned genre.
    SP_REQUIRE (engine->learnFromUserEdit (track, sptest_profile::toneParams (3.5f), sp::LearnScope::album));
    sp::ProfileDecision learnedAlbumDecision;
    const auto learnedAlbumParams = engine->resolveParams (track, nullptr, manualParams, learnedAlbumDecision);
    SP_REQUIRE_NEAR ((double) learnedAlbumParams.bass.gainDb, 3.5, 1.0e-4);
    SP_REQUIRE (learnedAlbumDecision.learnedPreferenceApplied);
    printDecision ("learned album", learnedAlbumDecision);

    // Learned song (keyed by path) -> 3.8 dB beats learned album.
    SP_REQUIRE (engine->learnFromUserEdit (track, sptest_profile::toneParams (3.8f), sp::LearnScope::song));
    sp::ProfileDecision learnedSongDecision;
    const auto learnedSongParams = engine->resolveParams (track, nullptr, manualParams, learnedSongDecision);
    SP_REQUIRE_NEAR ((double) learnedSongParams.bass.gainDb, 3.8, 1.0e-4);
    SP_REQUIRE (learnedSongDecision.learnedPreferenceApplied);
    SP_REQUIRE (engine->clearLearned (sp::LearnedPreference::Kind::song, sp::ProfileEngine::songKey (track)));
    sp::ProfileDecision afterSongClear;
    SP_REQUIRE_NEAR ((double) engine->resolveParams (track, nullptr, manualParams, afterSongClear).bass.gainDb,
                     3.5, 1.0e-4);

    // Album override -> 4.0 dB beats learned preferences.
    SP_REQUIRE (engine->setAlbumOverride (track, sptest_profile::toneParams (4.0f)));
    sp::ProfileDecision albumOverrideDecision;
    const auto albumOverrideParams = engine->resolveParams (track, nullptr, manualParams, albumOverrideDecision);
    SP_REQUIRE_NEAR ((double) albumOverrideParams.bass.gainDb, 4.0, 1.0e-4);
    SP_REQUIRE (albumOverrideDecision.overrideKind == "album");
    printDecision ("album override", albumOverrideDecision);

    // Song override -> 5.0 dB beats everything.
    SP_REQUIRE (engine->setSongOverride (track, sptest_profile::toneParams (5.0f)));
    sp::ProfileDecision songOverrideDecision;
    const auto songOverrideParams = engine->resolveParams (track, nullptr, manualParams, songOverrideDecision);
    SP_REQUIRE_NEAR ((double) songOverrideParams.bass.gainDb, 5.0, 1.0e-4);
    SP_REQUIRE (songOverrideDecision.overrideKind == "song");
    printDecision ("song override", songOverrideDecision);

    // Unwind the precedence one step at a time.
    engine->clearSongOverride (42);
    sp::ProfileDecision d1;
    SP_REQUIRE_NEAR ((double) engine->resolveParams (track, nullptr, manualParams, d1).bass.gainDb, 4.0, 1.0e-4);

    engine->clearAlbumOverride (track);
    sp::ProfileDecision d2;
    SP_REQUIRE_NEAR ((double) engine->resolveParams (track, nullptr, manualParams, d2).bass.gainDb, 3.5, 1.0e-4);

    SP_REQUIRE (engine->clearLearned (sp::LearnedPreference::Kind::album, sp::ProfileEngine::albumKey (track)));
    sp::ProfileDecision d3;
    SP_REQUIRE_NEAR ((double) engine->resolveParams (track, nullptr, manualParams, d3).bass.gainDb, 3.0, 1.0e-4);

    engine->clearAllLearned();
    sp::ProfileDecision d4;
    SP_REQUIRE_NEAR ((double) engine->resolveParams (track, nullptr, manualParams, d4).bass.gainDb, profileBass, 1.0e-4);
    SP_REQUIRE (engine->getAllLearned().empty());
}

SP_TEST (ProfileDecisions, ManualModeReturnsManualParams)
{
    sptest_profile::TempEnvironment env;
    auto engine = env.makeEngine();
    SP_REQUIRE (engine->initialise().isEmpty());
    engine->setMode (sp::EqMode::manual);

    const auto track = sptest_profile::makeTrack ("Jazz");
    auto manual = sp::ProfileEngine::flatReferenceParams();
    manual.bass.gainDb = -1.5f;
    manual.graphic.gains15[3] = 2.0f;

    const auto decision = engine->decide (track, nullptr);
    SP_REQUIRE (decision.mode == sp::EqMode::manual);
    SP_REQUIRE (decision.profileName == "Manual");
    SP_REQUIRE (decision.explanation().isNotEmpty());

    sp::ProfileDecision resolvedDecision;
    const auto resolved = engine->resolveParams (track, nullptr, manual, resolvedDecision);
    SP_REQUIRE_MSG (sp::ProfileEngine::compare (resolved, manual).empty(),
                    "manual mode must return the caller's params unchanged");
    SP_REQUIRE (resolvedDecision.mode == sp::EqMode::manual);
    printDecision ("manual", resolvedDecision);
}

SP_TEST (ProfileDecisions, ReferenceModeIsBypassed)
{
    sptest_profile::TempEnvironment env;
    auto engine = env.makeEngine();
    SP_REQUIRE (engine->initialise().isEmpty());
    engine->setMode (sp::EqMode::reference);

    auto processedManual = sp::ProfileEngine::flatReferenceParams();
    processedManual.bass.gainDb = 4.0f;

    const auto track = sptest_profile::makeTrack ("Jazz");
    const auto decision = engine->decide (track, nullptr);
    SP_REQUIRE (decision.mode == sp::EqMode::reference);
    SP_REQUIRE (decision.profileName == "Reference");
    SP_REQUIRE (decision.reasons.size() > 0);
    SP_REQUIRE (decision.explanation().isNotEmpty());
    printDecision ("reference", decision);

    sp::ProfileDecision resolvedDecision;
    const auto resolved = engine->resolveParams (track, nullptr, processedManual, resolvedDecision);
    SP_REQUIRE (! resolved.hasActiveProcessing (true));
}

SP_TEST (ProfileDecisions, EveryNonManualDecisionExplainsItself)
{
    sptest_profile::TempEnvironment env;
    auto engine = env.makeEngine();
    SP_REQUIRE (engine->initialise().isEmpty());

    const auto track = sptest_profile::makeTrack ("Modern R&B");
    const sp::TrackAnalysis analysis = brightHeavyAnalysis();
    const sp::DspParams manualParams;

    for (auto mode : { sp::EqMode::autoGenre, sp::EqMode::intelligent })
    {
        engine->setMode (mode);
        const auto decision = engine->decide (track, mode == sp::EqMode::intelligent ? &analysis : nullptr);
        SP_REQUIRE_MSG (! decision.reasons.empty(), sp::eqModeName (mode));
        const auto explanation = decision.explanation();
        SP_REQUIRE_MSG (explanation.isNotEmpty(), sp::eqModeName (mode));
        // The explanation must mention either the genre metadata or the analysis.
        SP_REQUIRE (explanation.containsIgnoreCase ("genre") || explanation.containsIgnoreCase ("Measured"));
        printDecision (sp::eqModeName (mode), decision);
    }

    // wantsAnalysis() is only true in intelligent mode and only without overrides.
    engine->setMode (sp::EqMode::autoGenre);
    SP_REQUIRE (! engine->wantsAnalysis (track));
    engine->setMode (sp::EqMode::intelligent);
    SP_REQUIRE (engine->wantsAnalysis (track));
    engine->setSongOverride (track, sptest_profile::toneParams (2.0f));
    SP_REQUIRE (! engine->wantsAnalysis (track));
}

SP_TEST (ProfileDecisions, MergePreservesPlaybackSafetySettings)
{
    sptest_profile::TempEnvironment env;
    auto engine = env.makeEngine();
    SP_REQUIRE (engine->initialise().isEmpty());
    engine->setMode (sp::EqMode::autoGenre);

    sp::DspParams base;
    base.replayGain.enabled = true;
    base.replayGain.preampDb = -3.5f;
    base.replayGain.albumMode = true;
    base.preamp.limiterEnabled = true;
    base.preamp.outputCeilingDb = -0.5f;
    base.convolution.enabled = true;
    base.convolution.irPath = "/ir.wav";
    base.eqBypass = true;

    const auto track = sptest_profile::makeTrack ("Classic R&B");
    sp::ProfileDecision decision;
    const auto resolved = engine->resolveParams (track, nullptr, base, decision);

    // Profile tone blocks applied...
    SP_REQUIRE_NEAR ((double) resolved.bass.gainDb, 2.0, 1.0e-4);
    // ...but the user's playback safety settings survive the merge.
    SP_REQUIRE (resolved.replayGain.enabled);
    SP_REQUIRE_NEAR (resolved.replayGain.preampDb, -3.5, 1.0e-4);
    SP_REQUIRE (resolved.replayGain.albumMode);
    SP_REQUIRE (resolved.preamp.limiterEnabled);
    SP_REQUIRE_NEAR (resolved.preamp.outputCeilingDb, -0.5, 1.0e-4);
    SP_REQUIRE (resolved.convolution.enabled);
    SP_REQUIRE (resolved.convolution.irPath == "/ir.wav");
    SP_REQUIRE (resolved.eqBypass);
    printDecision ("merged profile", decision);
}
