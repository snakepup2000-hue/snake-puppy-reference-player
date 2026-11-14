/*
    Profile engine tests — built-in profiles, flat reference state, conservative
    gains, genre normalisation and A/B comparison.
*/
#include "TestFramework.h"
#include "Profile/ProfileEngine.h"

#include <cmath>
#include <iostream>
#include <set>

namespace
{
void printProfiles (const std::vector<sp::EqProfile>& profiles)
{
    for (auto& p : profiles)
    {
        std::cout << "    " << p.id.toStdString() << "  \"" << p.name.toStdString()
                  << "\"  genre=" << p.genre.toStdString()
                  << "  maxBoost=" << p.params.maxPositiveBoostDb() << " dB\n";
    }
}

const sp::EqProfile* findById (const std::vector<sp::EqProfile>& profiles, const juce::String& id)
{
    for (auto& p : profiles)
        if (p.id == id)
            return &p;
    return nullptr;
}
} // namespace

SP_TEST (ProfileCore, BuiltInsPresentAndStable)
{
    const auto profiles = sp::ProfileEngine::createBuiltInProfiles();
    SP_REQUIRE_MSG (profiles.size() == 11, (int) profiles.size());

    const char* expectedIds[] = {
        "builtin:modern-hiphop", "builtin:old-school-hiphop", "builtin:classic-rnb",
        "builtin:modern-rnb", "builtin:soul", "builtin:classic-funk", "builtin:jazz",
        "builtin:rock", "builtin:pop", "builtin:electronic", "builtin:flat"
    };
    const char* expectedNames[] = {
        "Modern Hip-Hop", "Old-School Hip-Hop", "Classic R&B",
        "Modern R&B", "Soul", "Classic Funk", "Jazz",
        "Rock", "Pop", "Electronic", "Flat/Reference"
    };

    std::set<juce::String> uniqueIds;
    std::set<juce::String> uniqueNames;
    for (size_t i = 0; i < profiles.size(); ++i)
    {
        const auto& p = profiles[i];
        SP_REQUIRE_MSG (p.id == expectedIds[i], p.id.toStdString());
        SP_REQUIRE_MSG (p.name == expectedNames[i], p.name.toStdString());
        SP_REQUIRE (p.builtIn);
        SP_REQUIRE_MSG (p.genre.isNotEmpty(), p.id.toStdString());
        SP_REQUIRE_MSG (p.description.isNotEmpty(), p.id.toStdString());
        uniqueIds.insert (p.id);
        uniqueNames.insert (p.name);
    }
    SP_REQUIRE_MSG (uniqueIds.size() == 11, (int) uniqueIds.size());
    SP_REQUIRE_MSG (uniqueNames.size() == 11, (int) uniqueNames.size());

    std::cout << "  built-ins (" << profiles.size() << "):\n";
    printProfiles (profiles);
}

SP_TEST (ProfileCore, FlatReferenceIsTrulyFlat)
{
    const auto flat = sp::ProfileEngine::flatReferenceParams();
    SP_REQUIRE (! flat.bypassed);
    SP_REQUIRE (! flat.eqBypass);
    SP_REQUIRE (flat.graphic.isFlat());
    SP_REQUIRE (! flat.parametric.hasActiveBands());
    SP_REQUIRE (! flat.bass.isActive());
    SP_REQUIRE (! flat.filters.isActive());
    SP_REQUIRE (! flat.stereo.isActive());
    SP_REQUIRE (! flat.replayGain.enabled);
    SP_REQUIRE (! flat.preamp.limiterEnabled);
    SP_REQUIRE (! flat.preamp.autoHeadroom);
    SP_REQUIRE_NEAR (flat.preamp.manualPreampDb, 0.0, 1.0e-6);
    SP_REQUIRE_MSG (! flat.hasActiveProcessing (true),
                    "flat reference must not process anything");

    // The built-in Flat/Reference profile must be the same neutral state.
    const auto profiles = sp::ProfileEngine::createBuiltInProfiles();
    const auto* flatProfile = findById (profiles, "builtin:flat");
    SP_REQUIRE (flatProfile != nullptr);
    if (flatProfile != nullptr)
    {
        SP_REQUIRE (sp::ProfileEngine::compare (flatProfile->params, flat).empty());
        SP_REQUIRE (! flatProfile->params.hasActiveProcessing (true));
    }
}

SP_TEST (ProfileCore, GenreProfilesAreConservative)
{
    const auto profiles = sp::ProfileEngine::createBuiltInProfiles();
    double maxBandGain = 0.0;
    double maxBassGain = 0.0;
    int activeBands = 0;

    for (auto& p : profiles)
    {
        for (auto& band : p.params.parametric.bands)
        {
            if (! band.isActive())
                continue;
            ++activeBands;
            SP_REQUIRE_MSG (std::abs (band.gainDb) <= 3.0,
                            juce::String (p.id + " " + juce::String (band.gainDb)).toStdString());
            maxBandGain = juce::jmax (maxBandGain, std::abs (band.gainDb));
        }
        SP_REQUIRE_MSG (std::abs (p.params.bass.gainDb) <= 3.0f, p.id.toStdString());
        maxBassGain = juce::jmax (maxBassGain, (double) std::abs (p.params.bass.gainDb));

        const float sub = p.params.bass.subEnabled ? std::abs (p.params.bass.subGainDb) : 0.0f;
        SP_REQUIRE (sub <= 3.0f);

        // Profiles only describe tone; playback safety stays with the user.
        SP_REQUIRE (! p.params.replayGain.enabled);
        SP_REQUIRE (! p.params.preamp.limiterEnabled);
    }

    std::cout << "  conservative check: " << activeBands << " active bands, max band gain "
              << maxBandGain << " dB, max bass shelf " << maxBassGain << " dB\n";
    SP_REQUIRE_GE (activeBands, 15);        // enough tone shaping to be musical, not a stub
    SP_REQUIRE_LE (maxBandGain, 3.0);
    SP_REQUIRE_LE (maxBassGain, 3.0);
}

SP_TEST (ProfileCore, GenreNormalisation)
{
    using P = sp::ProfileEngine;
    SP_REQUIRE (P::normaliseGenre ("Jazz") == "jazz");
    SP_REQUIRE (P::normaliseGenre ("  JAZZ  ") == "jazz");
    SP_REQUIRE (P::normaliseGenre ("Soul") == "soul");
    SP_REQUIRE (P::normaliseGenre ("Funk") == "classic-funk");
    SP_REQUIRE (P::normaliseGenre ("Funk/Soul") == "classic-funk");
    SP_REQUIRE (P::normaliseGenre ("R&B") == "modern-rnb");
    SP_REQUIRE (P::normaliseGenre ("RnB") == "modern-rnb");
    SP_REQUIRE (P::normaliseGenre ("r&b") == "modern-rnb");
    SP_REQUIRE (P::normaliseGenre ("Rhythm and Blues") == "modern-rnb");
    SP_REQUIRE (P::normaliseGenre ("Classic R&B") == "classic-rnb");
    SP_REQUIRE (P::normaliseGenre ("Modern R&B") == "modern-rnb");
    SP_REQUIRE (P::normaliseGenre ("Hip Hop") == "modern-hiphop");
    SP_REQUIRE (P::normaliseGenre ("hip-hop") == "modern-hiphop");
    SP_REQUIRE (P::normaliseGenre ("Rap") == "modern-hiphop");
    SP_REQUIRE (P::normaliseGenre ("Old School Hip-Hop") == "old-school-hiphop");
    SP_REQUIRE (P::normaliseGenre ("90s Hip Hop") == "old-school-hiphop");
    SP_REQUIRE (P::normaliseGenre ("Rock") == "rock");
    SP_REQUIRE (P::normaliseGenre ("Pop") == "pop");
    SP_REQUIRE (P::normaliseGenre ("EDM") == "electronic");
    SP_REQUIRE (P::normaliseGenre ("House") == "electronic");
    SP_REQUIRE (P::normaliseGenre ("Techno") == "electronic");
    SP_REQUIRE (P::normaliseGenre ("Electronic") == "electronic");
    SP_REQUIRE (P::normaliseGenre ("Polka").isEmpty());
    SP_REQUIRE (P::normaliseGenre ("").isEmpty());

    // Keys.
    sp::Track t;
    t.albumArtist = "Artist";
    t.album = "Album";
    t.path = "/music/song.flac";
    SP_REQUIRE (P::albumKey (t) == "Artist|Album");
    SP_REQUIRE (P::songKey (t) == "/music/song.flac");
    sp::Track noAlbum;
    SP_REQUIRE (P::albumKey (noAlbum).isEmpty());
}

SP_TEST (ProfileCore, CompareProducesAbDiff)
{
    const auto profiles = sp::ProfileEngine::createBuiltInProfiles();
    const auto* classicRnb = findById (profiles, "builtin:classic-rnb");
    const auto* jazz = findById (profiles, "builtin:jazz");
    SP_REQUIRE (classicRnb != nullptr);
    SP_REQUIRE (jazz != nullptr);
    if (classicRnb == nullptr || jazz == nullptr)
        return;

    const auto flat = sp::ProfileEngine::flatReferenceParams();
    const auto diffs = sp::ProfileEngine::compare (flat, classicRnb->params);
    SP_REQUIRE_MSG (diffs.size() > 0, (int) diffs.size());

    bool foundBass = false;
    for (auto& d : diffs)
    {
        std::cout << "    A/B: " << d.label.toStdString() << " | A=" << d.a.toStdString()
                  << " B=" << d.b.toStdString() << " delta=" << d.deltaDb << " dB\n";
        if (d.label.startsWith ("Bass"))
        {
            foundBass = true;
            SP_REQUIRE_NEAR (d.deltaDb, 2.0, 0.01);
            SP_REQUIRE (d.b.contains ("+"));
        }
        SP_REQUIRE (d.label.isNotEmpty());
        SP_REQUIRE (d.a.isNotEmpty());
        SP_REQUIRE (d.b.isNotEmpty());
    }
    SP_REQUIRE_MSG (foundBass, "compare() should list the bass shelf difference");

    // Identical parameter sets produce no differences.
    SP_REQUIRE (sp::ProfileEngine::compare (flat, flat).empty());

    // Two different profiles differ too.
    SP_REQUIRE (! sp::ProfileEngine::compare (jazz->params, classicRnb->params).empty());
}
