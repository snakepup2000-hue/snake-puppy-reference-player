/*
    Snake Puppy Reference Player
    Genre-aware profile engine: built-in genre profiles, metadata decisions,
    conservative analysis-assisted refinement, overrides, preference learning
    and SQLite persistence.

    See ProfileEngine.h for the storage schema, precedence and merge contract.
*/
#include "Profile/ProfileEngine.h"
#include "Core/Serialization.h"
#include "Profile/TrackAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

namespace sp
{

//==============================================================================
// Small formatting / normalisation helpers (file-local)
//==============================================================================
namespace
{
constexpr int kProfileSchemaVersion = 1;
constexpr double kLowShareTarget = 0.25;   // 25 % of 20 Hz-16 kHz energy below 120 Hz
constexpr double kLowShareThin = 0.08;     // below this the master sounds light in the bass

juce::String formatDb (double db, int decimals = 1)
{
    if (! std::isfinite (db))
        return "-inf dB";
    juce::String s;
    if (db > 0.049)
        s << "+";
    s << juce::String (db, decimals) << " dB";
    return s;
}

juce::String formatHz (double hz)
{
    if (hz >= 1000.0)
        return juce::String (hz / 1000.0, 1) + " kHz";
    return juce::String ((int) std::lround (hz)) + " Hz";
}

juce::String formatDbMagnitude (double db, int decimals = 1)
{
    if (! std::isfinite (db))
        return "-inf dB";
    return juce::String (std::abs (db), decimals) + " dB";
}

juce::String formatPercent (double ratio)
{
    return juce::String ((int) std::lround (ratio * 100.0)) + "%";
}

bool approxEqual (double a, double b, double tol = 1.0e-4)
{
    return std::abs (a - b) <= tol;
}

/** Lower-case, punctuation-free, single-spaced form used for genre matching.
    '&' becomes " and " so "R&B" and "rhythm & blues" keep their meaning. */
juce::String normaliseText (const juce::String& rawText)
{
    const juce::String text = rawText.replace ("&", " and ");
    juce::String out;
    out.preallocateBytes ((size_t) text.length() * 2 + 8);
    bool lastWasSpace = true;
    for (int i = 0; i < text.length(); ++i)
    {
        const juce::juce_wchar ch = juce::CharacterFunctions::toLowerCase (text[i]);
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
        {
            out << ch;
            lastWasSpace = false;
        }
        else if (! lastWasSpace)
        {
            out << ' ';
            lastWasSpace = true;
        }
    }
    return out.trim();
}

bool containsAny (const juce::String& haystack, std::initializer_list<const char*> needles)
{
    for (auto* n : needles)
        if (haystack.contains (juce::String (n)))
            return true;
    return false;
}

bool isRnbText (const juce::String& n)
{
    // "r&b" normalises to "r and b"; accept the bare "r b" spelling too.
    return containsAny (n, { "r and b", "rnb", "randb", "rhythm and blues" })
        || n == "r b" || n.startsWith ("r b ") || n.contains (" r b ");
}

/** Canonical genre family -> built-in profile id. Empty input maps to flat. */
juce::String profileIdForCanonicalGenre (const juce::String& canonical)
{
    if (canonical.isEmpty() || canonical == "reference")
        return "builtin:flat";
    return "builtin:" + canonical;
}

//== param sanitising ==========================================================
void sanitiseParams (DspParams& p)
{
    auto clamp = [] (double v, double lo, double hi, double fallback) {
        if (! std::isfinite (v))
            return fallback;
        return juce::jlimit (lo, hi, v);
    };

    // Graphic: range may only be 6 or 12 dB; gains must fit the range.
    p.graphic.rangeDb = (std::abs (p.graphic.rangeDb - 12.0f) < 0.5f) ? 12.0f : 6.0f;
    for (auto& g : p.graphic.gains15)
        g = (float) clamp (g, -p.graphic.rangeDb, p.graphic.rangeDb, 0.0);
    for (auto& g : p.graphic.gains31)
        g = (float) clamp (g, -p.graphic.rangeDb, p.graphic.rangeDb, 0.0);

    // Parametric: 20 Hz .. 20 kHz, +/-24 dB, Q 0.1 .. 10.
    for (auto& b : p.parametric.bands)
    {
        b.freqHz = clamp (b.freqHz, 20.0, 20000.0, 1000.0);
        b.gainDb = clamp (b.gainDb, -24.0, 24.0, 0.0);
        b.q = clamp (b.q, 0.1, 10.0, 0.707);
    }

    // Bass management.
    p.bass.rangeDb = (std::abs (p.bass.rangeDb - 12.0f) < 0.5f) ? 12.0f : 6.0f;
    p.bass.gainDb = (float) clamp (p.bass.gainDb, -p.bass.rangeDb, p.bass.rangeDb, 0.0);
    p.bass.shelfFreqHz = clamp (p.bass.shelfFreqHz, 20.0, 500.0, 80.0);
    p.bass.shelfQ = clamp (p.bass.shelfQ, 0.1, 2.0, 0.707);
    p.bass.shelfS = clamp (p.bass.shelfS, 0.5, 1.5, 1.0);
    p.bass.subGainDb = (float) clamp (p.bass.subGainDb, -12.0, 12.0, 0.0);
    p.bass.subFreqHz = clamp (p.bass.subFreqHz, 20.0, 200.0, 35.0);
    p.bass.subQ = clamp (p.bass.subQ, 0.1, 4.0, 0.9);
    p.bass.midBassGainDb = (float) clamp (p.bass.midBassGainDb, -12.0, 12.0, 0.0);
    p.bass.midBassFreqHz = clamp (p.bass.midBassFreqHz, 40.0, 400.0, 100.0);
    p.bass.midBassQ = clamp (p.bass.midBassQ, 0.1, 4.0, 1.0);
    p.bass.punchGainDb = (float) clamp (p.bass.punchGainDb, -12.0, 12.0, 0.0);
    p.bass.punchFreqHz = clamp (p.bass.punchFreqHz, 60.0, 400.0, 140.0);
    p.bass.punchQ = clamp (p.bass.punchQ, 0.1, 4.0, 0.9);

    // Filters.
    p.filters.highPassHz = clamp (p.filters.highPassHz, 10.0, 20000.0, 20.0);
    p.filters.lowPassHz = clamp (p.filters.lowPassHz, 20.0, 22050.0, 20000.0);
    if (p.filters.lowPassHz <= p.filters.highPassHz)
        p.filters.lowPassHz = juce::jmin (22050.0, p.filters.highPassHz * 1.5);
    auto slope = [] (int s) { return (s == 24 || s == 48) ? s : 12; };
    p.filters.highPassSlopeDbPerOct = slope (p.filters.highPassSlopeDbPerOct);
    p.filters.lowPassSlopeDbPerOct = slope (p.filters.lowPassSlopeDbPerOct);

    // Stereo.
    p.stereo.balance = (float) clamp (p.stereo.balance, -1.0, 1.0, 0.0);
    p.stereo.widthPercent = (float) clamp (p.stereo.widthPercent, 0.0, 200.0, 100.0);
    p.stereo.monoBassHz = clamp (p.stereo.monoBassHz, 20.0, 500.0, 120.0);
    p.stereo.crossfeedAmountDb = (float) clamp (p.stereo.crossfeedAmountDb, 0.0, 18.0, 6.0);
    p.stereo.crossfeedCutoffHz = clamp (p.stereo.crossfeedCutoffHz, 200.0, 2000.0, 700.0);
    p.stereo.crossfeedDelayUs = (float) clamp (p.stereo.crossfeedDelayUs, 0.0, 1000.0, 250.0);

    // ReplayGain / preamp / limiter.
    p.replayGain.preampDb = (float) clamp (p.replayGain.preampDb, -24.0, 12.0, 0.0);
    p.replayGain.fallbackGainDb = (float) clamp (p.replayGain.fallbackGainDb, -24.0, 12.0, 0.0);
    p.convolution.mixPercent = (float) clamp (p.convolution.mixPercent, 0.0, 100.0, 100.0);
    p.preamp.manualPreampDb = (float) clamp (p.preamp.manualPreampDb, -24.0, 12.0, 0.0);
    p.preamp.outputCeilingDb = (float) clamp (p.preamp.outputCeilingDb, -6.0, 0.0, -0.3);
    p.preamp.limiterReleaseMs = (float) clamp (p.preamp.limiterReleaseMs, 1.0, 1000.0, 150.0);

    // Chain order must be a permutation of the known slots; repair duplicates.
    std::array<bool, (size_t) DspSlot::numSlots> seen {};
    std::array<DspSlot, (size_t) DspSlot::numSlots> repaired {};
    int writeIndex = 0;
    for (auto slot : p.chainOrder)
    {
        const auto index = (size_t) slot;
        if (index < seen.size() && ! seen[index])
        {
            seen[index] = true;
            repaired[(size_t) writeIndex++] = slot;
        }
    }
    for (size_t i = 0; i < seen.size(); ++i)
        if (! seen[i])
            repaired[(size_t) writeIndex++] = (DspSlot) i;
    p.chainOrder = repaired;
}

//== profile construction helpers =============================================
void setBand (ParametricEqParams& eq, int index, BandType type, double freqHz, double gainDb, double q)
{
    if (index < 0 || index >= ParametricEqParams::kNumBands)
        return;
    auto& b = eq.bands[(size_t) index];
    b.enabled = true;
    b.bypassed = false;
    b.type = type;
    b.freqHz = freqHz;
    b.gainDb = gainDb;
    b.q = q;
}

EqProfile makeProfile (const juce::String& id,
                       const juce::String& name,
                       const juce::String& genre,
                       const juce::String& description,
                       const std::function<void (DspParams&)>& configure)
{
    EqProfile p;
    p.id = id;
    p.name = name;
    p.genre = genre;
    p.description = description;
    p.builtIn = true;
    p.params = ProfileEngine::flatReferenceParams();
    if (configure)
        configure (p.params);
    sanitiseParams (p.params);
    return p;
}

juce::String bandLabel (const EqBand& b)
{
    return juce::String (bandTypeShortName (b.type)) + " @ " + formatHz (b.freqHz);
}

juce::String paramsToJson (const DspParams& params)
{
    return juce::JSON::toString (serialize::toVar (params), true);
}

bool paramsFromJson (const juce::String& text, DspParams& out)
{
    if (text.trim().isEmpty())
        return false;
    juce::var v;
    if (juce::JSON::parse (text, v).failed() || ! v.isObject())
        return false;
    out = serialize::dspFromVar (v, DspParams {});
    sanitiseParams (out);
    return true;
}

const char* learnedKindToScope (LearnedPreference::Kind k)
{
    switch (k)
    {
        case LearnedPreference::Kind::genre: return "genre";
        case LearnedPreference::Kind::album: return "album";
        case LearnedPreference::Kind::song:  return "song";
    }
    return "genre";
}

bool scopeToLearnedKind (const juce::String& scope, LearnedPreference::Kind& out)
{
    if (scope == "genre") { out = LearnedPreference::Kind::genre; return true; }
    if (scope == "album") { out = LearnedPreference::Kind::album; return true; }
    if (scope == "song")  { out = LearnedPreference::Kind::song;  return true; }
    return false;
}

juce::int64 nowMs()
{
    return juce::Time::currentTimeMillis();
}

//==============================================================================
// Base DspParams for a profile: neutral tone but with the *playback* processors
// left in a neutral, non-processing state. Profiles only ever describe tone.
//==============================================================================
} // namespace

//==============================================================================
DspParams ProfileEngine::flatReferenceParams()
{
    DspParams p;
    p.reset();

    p.bypassed = false;
    p.eqBypass = false;

    // Flat graphic EQ (enabled but with every gain at zero, so it is truly flat).
    p.graphic.enabled = true;
    p.graphic.bandSet = GraphicEqParams::BandSet::bands15;
    p.graphic.rangeDb = 6.0f;
    p.graphic.gains15.fill (0.0f);
    p.graphic.gains31.fill (0.0f);

    // No active parametric bands.
    p.parametric.enabled = true;
    p.parametric.reset();

    // Bass control at unity, no sub/mid-bass/punch bands.
    p.bass.reset();
    p.bass.enabled = true;
    p.bass.gainDb = 0.0f;

    // No filters, no stereo changes.
    p.filters = FilterParams {};
    p.stereo.reset();

    // No convolution, no ReplayGain.
    p.convolution = ConvolutionParams {};
    p.replayGain.enabled = false;

    // Limiter and automatic headroom off so the profile is bit-transparent.
    p.preamp.limiterEnabled = false;
    p.preamp.autoHeadroom = false;
    p.preamp.manualPreampDb = 0.0f;

    return p;
}

//==============================================================================
std::vector<EqProfile> ProfileEngine::createBuiltInProfiles()
{
    using BT = BandType;
    std::vector<EqProfile> profiles;
    profiles.reserve (11);

    profiles.push_back (makeProfile (
        "builtin:modern-hiphop", "Modern Hip-Hop", "Hip-Hop",
        "Relatively neutral sub-bass: no bass boost, so 808-heavy modern masters do not get thicker. "
        "A gentle 3.5 kHz upper-mid lift helps vocals and snares cut through, with no added air.",
        [] (DspParams& p) {
            setBand (p.parametric, 0, BT::bell, 3500.0, 0.7, 0.8);
            setBand (p.parametric, 1, BT::bell, 9000.0, -0.5, 0.7);
        }));

    profiles.push_back (makeProfile (
        "builtin:old-school-hiphop", "Old-School Hip-Hop", "Hip-Hop (Old-School)",
        "Slight low-mid warmth around 150 Hz for sample and kick body, a small 3 kHz reduction "
        "for hard snares and a gentle roll-off above 9 kHz to tame brittle 80s/90s digital highs.",
        [] (DspParams& p) {
            setBand (p.parametric, 0, BT::bell, 150.0, 1.0, 0.9);
            setBand (p.parametric, 1, BT::bell, 3000.0, -0.5, 1.0);
            setBand (p.parametric, 2, BT::highShelf, 9000.0, -1.0, 0.707);
        }));

    profiles.push_back (makeProfile (
        "builtin:classic-rnb", "Classic R&B", "R&B (Classic)",
        "Gentle low-shelf warmth (about +2 dB around 100 Hz) for 60s-80s R&B, a small presence "
        "lift near 3 kHz for vocals and a soft top. Conservative so horns and voices stay natural.",
        [] (DspParams& p) {
            p.bass.gainDb = 2.0f;
            p.bass.shelfFreqHz = 100.0;
            setBand (p.parametric, 0, BT::bell, 3000.0, 0.8, 0.9);
            setBand (p.parametric, 1, BT::highShelf, 10000.0, -0.5, 0.707);
        }));

    profiles.push_back (makeProfile (
        "builtin:modern-rnb", "Modern R&B", "R&B (Modern)",
        "Mild sub reinforcement near 35 Hz, a small cut near 160 Hz to avoid boom, a smooth high "
        "shelf and a little presence for contemporary R&B vocals.",
        [] (DspParams& p) {
            p.bass.gainDb = 0.5f;
            p.bass.shelfFreqHz = 80.0;
            p.bass.subEnabled = true;
            p.bass.subGainDb = 1.5f;
            p.bass.subFreqHz = 35.0;
            setBand (p.parametric, 0, BT::bell, 160.0, -0.8, 1.2);
            setBand (p.parametric, 1, BT::bell, 3000.0, 0.6, 0.9);
            setBand (p.parametric, 2, BT::highShelf, 10000.0, -0.6, 0.707);
        }));

    profiles.push_back (makeProfile (
        "builtin:soul", "Soul", "Soul",
        "Warm low-mids around 250 Hz and a smooth, slightly rolled-off top; keeps horns and "
        "vocals rich without adding sub-bass weight.",
        [] (DspParams& p) {
            p.bass.gainDb = 1.0f;
            p.bass.shelfFreqHz = 90.0;
            setBand (p.parametric, 0, BT::bell, 250.0, 0.8, 0.8);
            setBand (p.parametric, 1, BT::bell, 2000.0, 0.3, 0.9);
            setBand (p.parametric, 2, BT::highShelf, 8000.0, -1.0, 0.707);
        }));

    profiles.push_back (makeProfile (
        "builtin:classic-funk", "Classic Funk", "Funk",
        "Tight, controlled low end with a small low-mid cut, a 3 kHz snap for slap bass and "
        "horns, and a gentle top roll-off.",
        [] (DspParams& p) {
            p.bass.gainDb = -0.5f;
            p.bass.shelfFreqHz = 90.0;
            setBand (p.parametric, 0, BT::bell, 200.0, -1.0, 1.0);
            setBand (p.parametric, 1, BT::bell, 3000.0, 1.5, 1.0);
            setBand (p.parametric, 2, BT::highShelf, 10000.0, -0.5, 0.707);
        }));

    profiles.push_back (makeProfile (
        "builtin:jazz", "Jazz", "Jazz",
        "Very close to flat: a touch of warmth near 100 Hz, a subtle 250 Hz lift and a little air "
        "above 12 kHz so acoustic instruments breathe without colouring tone.",
        [] (DspParams& p) {
            p.bass.gainDb = 0.5f;
            p.bass.shelfFreqHz = 100.0;
            setBand (p.parametric, 0, BT::bell, 250.0, 0.4, 0.8);
            setBand (p.parametric, 1, BT::highShelf, 12000.0, 0.8, 0.707);
        }));

    profiles.push_back (makeProfile (
        "builtin:rock", "Rock", "Rock",
        "Slight low-mid presence around 1.8 kHz, controlled sub-bass and a 3.5 kHz cut to tame "
        "harsh guitars and cymbals while keeping the kick tight.",
        [] (DspParams& p) {
            p.bass.gainDb = -0.5f;
            p.bass.shelfFreqHz = 80.0;
            setBand (p.parametric, 0, BT::bell, 1800.0, 0.8, 0.8);
            setBand (p.parametric, 1, BT::bell, 3500.0, -1.2, 1.2);
            setBand (p.parametric, 2, BT::highShelf, 10000.0, -0.4, 0.707);
        }));

    profiles.push_back (makeProfile (
        "builtin:pop", "Pop", "Pop",
        "Mild smile: about +1 dB low shelf, +1 dB air above 12 kHz and a small 3 kHz vocal "
        "presence lift; modern but not scooped.",
        [] (DspParams& p) {
            p.bass.gainDb = 1.0f;
            p.bass.shelfFreqHz = 85.0;
            setBand (p.parametric, 0, BT::bell, 3000.0, 0.8, 0.9);
            setBand (p.parametric, 1, BT::highShelf, 12000.0, 1.0, 0.707);
        }));

    profiles.push_back (makeProfile (
        "builtin:electronic", "Electronic", "Electronic",
        "Controlled sub with a small low shelf, a 200 Hz cut to reduce mud, and gentle air above "
        "13 kHz for synths and hats without boom.",
        [] (DspParams& p) {
            p.bass.gainDb = 0.8f;
            p.bass.shelfFreqHz = 75.0;
            setBand (p.parametric, 0, BT::bell, 200.0, -0.7, 1.0);
            setBand (p.parametric, 1, BT::highShelf, 13000.0, 1.2, 0.707);
        }));

    profiles.push_back (makeProfile (
        "builtin:flat", "Flat/Reference", "Reference",
        "Truly flat: no EQ, bass, filter or stereo changes, with limiter and ReplayGain disabled. "
        "Use this for unaltered reference listening and A/B comparisons.",
        [] (DspParams&) {}));

    return profiles;
}

//==============================================================================
// Impl
//==============================================================================
struct ProfileEngine::Impl
{
    LibraryDatabase& db;
    LibraryService& library;

    mutable juce::CriticalSection mutex;

    EqMode mode = EqMode::manual;
    LearnScope learnScope = LearnScope::song;

    std::vector<EqProfile> builtIns;
    std::map<juce::String, EqProfile> builtInEdits;
    std::vector<EqProfile> userProfiles;

    std::map<TrackId, DspParams> songOverrides;
    std::map<juce::String, DspParams> albumOverrides;
    std::map<int, std::map<juce::String, LearnedPreference>> learned;   // kind -> key -> pref

    bool persistenceAvailable = false;
    bool initialised = false;
    juce::String lastError;

    Impl (LibraryDatabase& d, LibraryService& l) : db (d), library (l) {}

    //== schema / persistence =================================================
    bool execute (const juce::String& sql, juce::String& errorOut)
    {
        const juce::ScopedLock dbLock (db.lock());
        if (! db.db().execute (sql, &errorOut))
            return false;
        return true;
    }

    bool writeRow (const juce::String& sql,
                   const std::function<void (SqliteStatement&)>& binder,
                   juce::String& errorOut)
    {
        const juce::ScopedLock dbLock (db.lock());
        auto statement = db.db().prepare (sql);
        if (statement == nullptr || ! statement->isValid())
        {
            errorOut = "Could not prepare profile statement: "
                     + (statement != nullptr ? statement->getError() : juce::String ("no statement"));
            return false;
        }
        binder (*statement);
        if (! statement->exec())
        {
            errorOut = "Profile database write failed: " + statement->getError();
            return false;
        }
        return true;
    }

    bool ensureSchema (juce::String& errorOut)
    {
        static const char* statements[] = {
            "CREATE TABLE IF NOT EXISTS eq_profiles ("
            " id TEXT PRIMARY KEY,"
            " name TEXT NOT NULL DEFAULT '',"
            " genre TEXT NOT NULL DEFAULT '',"
            " description TEXT NOT NULL DEFAULT '',"
            " builtin INTEGER NOT NULL DEFAULT 0,"
            " device_key TEXT NOT NULL DEFAULT '',"
            " params_json TEXT NOT NULL DEFAULT '{}',"
            " updated_ms INTEGER NOT NULL DEFAULT 0)",

            "CREATE TABLE IF NOT EXISTS song_overrides ("
            " track_id INTEGER PRIMARY KEY,"
            " params_json TEXT NOT NULL DEFAULT '{}',"
            " updated_ms INTEGER NOT NULL DEFAULT 0)",

            "CREATE TABLE IF NOT EXISTS album_overrides ("
            " album_key TEXT PRIMARY KEY,"
            " params_json TEXT NOT NULL DEFAULT '{}',"
            " updated_ms INTEGER NOT NULL DEFAULT 0)",

            "CREATE TABLE IF NOT EXISTS learned_prefs ("
            " scope TEXT NOT NULL,"
            " key TEXT NOT NULL,"
            " params_json TEXT NOT NULL DEFAULT '{}',"
            " learned_ms INTEGER NOT NULL DEFAULT 0,"
            " PRIMARY KEY (scope, key))",

            "CREATE TABLE IF NOT EXISTS profile_schema_version ("
            " id INTEGER PRIMARY KEY CHECK (id = 1),"
            " version INTEGER NOT NULL)",

            nullptr
        };

        const juce::ScopedLock dbLock (db.lock());
        for (int i = 0; statements[i] != nullptr; ++i)
            if (! db.db().execute (statements[i], &errorOut))
                return false;

        // Record our schema version (do not overwrite a newer version written by
        // a future build; report that instead of corrupting it).
        {
            int storedVersion = 0;
            auto select = db.db().prepare ("SELECT version FROM profile_schema_version WHERE id = 1");
            if (select != nullptr && select->isValid() && select->step())
                storedVersion = select->getInt (0);
            if (storedVersion > kProfileSchemaVersion)
            {
                errorOut = "Profile storage was written by a newer build (schema v"
                         + juce::String (storedVersion) + ", this build supports v"
                         + juce::String (kProfileSchemaVersion) + ").";
                return false;
            }
            auto insert = db.db().prepare ("INSERT OR REPLACE INTO profile_schema_version (id, version) VALUES (1, ?)");
            if (insert == nullptr || ! insert->isValid())
            {
                errorOut = "Could not prepare profile schema version statement.";
                return false;
            }
            insert->bind (1, (std::int64_t) kProfileSchemaVersion);
            if (! insert->exec())
            {
                errorOut = "Could not write profile schema version: " + insert->getError();
                return false;
            }
        }
        return true;
    }

    bool loadProfiles (juce::String& errorOut)
    {
        const juce::ScopedLock dbLock (db.lock());
        auto statement = db.db().prepare (
            "SELECT id, name, genre, description, builtin, device_key, params_json "
            "FROM eq_profiles ORDER BY builtin DESC, updated_ms ASC, name ASC");
        if (statement == nullptr || ! statement->isValid())
        {
            errorOut = "Could not read eq_profiles: "
                     + (statement != nullptr ? statement->getError() : juce::String ("no statement"));
            return false;
        }

        while (statement->step())
        {
            EqProfile p;
            p.id = statement->getString (0);
            p.name = statement->getString (1);
            p.genre = statement->getString (2);
            p.description = statement->getString (3);
            p.builtIn = statement->getInt (4) != 0;
            p.deviceKey = statement->getString (5);
            DspParams params;
            if (paramsFromJson (statement->getString (6), params))
                p.params = params;
            else
                p.params = ProfileEngine::flatReferenceParams();

            if (p.id.isEmpty())
                continue;

            if (p.builtIn)
            {
                p.builtIn = true;
                builtInEdits[p.id] = p;
            }
            else
            {
                p.builtIn = false;
                userProfiles.push_back (p);
            }
        }
        return true;
    }

    bool loadOverrides (juce::String& errorOut)
    {
        {
            const juce::ScopedLock dbLock (db.lock());
            auto statement = db.db().prepare ("SELECT track_id, params_json FROM song_overrides");
            if (statement == nullptr || ! statement->isValid())
            {
                errorOut = "Could not read song_overrides: "
                         + (statement != nullptr ? statement->getError() : juce::String ("no statement"));
                return false;
            }
            while (statement->step())
            {
                DspParams params;
                if (paramsFromJson (statement->getString (1), params))
                    songOverrides[statement->getInt64 (0)] = params;
            }
        }
        {
            const juce::ScopedLock dbLock (db.lock());
            auto statement = db.db().prepare ("SELECT album_key, params_json FROM album_overrides");
            if (statement == nullptr || ! statement->isValid())
            {
                errorOut = "Could not read album_overrides: "
                         + (statement != nullptr ? statement->getError() : juce::String ("no statement"));
                return false;
            }
            while (statement->step())
            {
                DspParams params;
                if (paramsFromJson (statement->getString (1), params))
                    albumOverrides[statement->getString (0)] = params;
            }
        }
        return true;
    }

    bool loadLearned (juce::String& errorOut)
    {
        const juce::ScopedLock dbLock (db.lock());
        auto statement = db.db().prepare ("SELECT scope, key, params_json, learned_ms FROM learned_prefs");
        if (statement == nullptr || ! statement->isValid())
        {
            errorOut = "Could not read learned_prefs: "
                     + (statement != nullptr ? statement->getError() : juce::String ("no statement"));
            return false;
        }
        while (statement->step())
        {
            LearnedPreference::Kind kind;
            if (! scopeToLearnedKind (statement->getString (0), kind))
                continue;
            LearnedPreference pref;
            pref.kind = kind;
            pref.key = statement->getString (1);
            pref.learnedMs = statement->getInt64 (3);
            DspParams params;
            if (paramsFromJson (statement->getString (2), params))
                pref.params = params;
            else
                continue;
            learned[(int) kind][pref.key] = pref;
        }
        return true;
    }

    bool persistProfileRow (const EqProfile& p, bool builtinOverride)
    {
        juce::String err;
        const bool ok = writeRow (
            "INSERT OR REPLACE INTO eq_profiles (id, name, genre, description, builtin, device_key, params_json, updated_ms) "
            "VALUES (?,?,?,?,?,?,?,?)",
            [&] (SqliteStatement& s) {
                s.bind (1, p.id);
                s.bind (2, p.name);
                s.bind (3, p.genre);
                s.bind (4, p.description);
                s.bind (5, (std::int64_t) (builtinOverride ? 1 : 0));
                s.bind (6, p.deviceKey);
                s.bind (7, paramsToJson (p.params));
                s.bind (8, nowMs());
            },
            err);
        if (! ok)
            lastError = err;
        return ok;
    }

    bool deleteProfileRow (const juce::String& id, bool builtinOverride)
    {
        juce::String err;
        const bool ok = writeRow ("DELETE FROM eq_profiles WHERE id = ? AND builtin = ?",
                                  [&] (SqliteStatement& s) {
                                      s.bind (1, id);
                                      s.bind (2, (std::int64_t) (builtinOverride ? 1 : 0));
                                  },
                                  err);
        if (! ok)
            lastError = err;
        return ok;
    }

    bool persistSongOverride (TrackId id, const DspParams& params)
    {
        juce::String err;
        const bool ok = writeRow (
            "INSERT OR REPLACE INTO song_overrides (track_id, params_json, updated_ms) VALUES (?,?,?)",
            [&] (SqliteStatement& s) {
                s.bind (1, (std::int64_t) id);
                s.bind (2, paramsToJson (params));
                s.bind (3, nowMs());
            },
            err);
        if (! ok)
            lastError = err;
        return ok;
    }

    bool deleteSongOverrideRow (TrackId id)
    {
        juce::String err;
        const bool ok = writeRow ("DELETE FROM song_overrides WHERE track_id = ?",
                                  [&] (SqliteStatement& s) { s.bind (1, (std::int64_t) id); },
                                  err);
        if (! ok)
            lastError = err;
        return ok;
    }

    bool persistAlbumOverride (const juce::String& key, const DspParams& params)
    {
        juce::String err;
        const bool ok = writeRow (
            "INSERT OR REPLACE INTO album_overrides (album_key, params_json, updated_ms) VALUES (?,?,?)",
            [&] (SqliteStatement& s) {
                s.bind (1, key);
                s.bind (2, paramsToJson (params));
                s.bind (3, nowMs());
            },
            err);
        if (! ok)
            lastError = err;
        return ok;
    }

    bool deleteAlbumOverrideRow (const juce::String& key)
    {
        juce::String err;
        const bool ok = writeRow ("DELETE FROM album_overrides WHERE album_key = ?",
                                  [&] (SqliteStatement& s) { s.bind (1, key); },
                                  err);
        if (! ok)
            lastError = err;
        return ok;
    }

    bool persistLearned (const LearnedPreference& pref)
    {
        juce::String err;
        const bool ok = writeRow (
            "INSERT OR REPLACE INTO learned_prefs (scope, key, params_json, learned_ms) VALUES (?,?,?,?)",
            [&] (SqliteStatement& s) {
                s.bind (1, juce::String (learnedKindToScope (pref.kind)));
                s.bind (2, pref.key);
                s.bind (3, paramsToJson (pref.params));
                s.bind (4, pref.learnedMs);
            },
            err);
        if (! ok)
            lastError = err;
        return ok;
    }

    bool deleteLearnedRow (LearnedPreference::Kind kind, const juce::String& key)
    {
        juce::String err;
        const bool ok = writeRow ("DELETE FROM learned_prefs WHERE scope = ? AND key = ?",
                                  [&] (SqliteStatement& s) {
                                      s.bind (1, juce::String (learnedKindToScope (kind)));
                                      s.bind (2, key);
                                  },
                                  err);
        if (! ok)
            lastError = err;
        return ok;
    }

    //== profile lookup =======================================================
    EqProfile factoryProfile (const juce::String& id) const
    {
        for (auto& p : builtIns)
            if (p.id == id)
                return p;
        return {};
    }

    EqProfile effectiveProfile (const juce::String& id) const
    {
        auto edit = builtInEdits.find (id);
        if (edit != builtInEdits.end())
        {
            auto p = edit->second;
            p.builtIn = true;
            p.id = id;
            return p;
        }
        return factoryProfile (id);
    }

    EqProfile findProfileNoLock (const juce::String& id) const
    {
        if (auto p = effectiveProfile (id); p.id.isNotEmpty())
            return p;
        for (auto& p : userProfiles)
            if (p.id == id)
                return p;
        return {};
    }

    EqProfile profileForGenreNoLock (const juce::String& genre) const
    {
        const auto canonical = ProfileEngine::normaliseGenre (genre);
        return effectiveProfile (profileIdForCanonicalGenre (canonical));
    }

    //== learning / override lookup ===========================================
    static juce::String learnedGenreKey (const juce::String& genre)
    {
        auto canonical = ProfileEngine::normaliseGenre (genre);
        if (canonical.isNotEmpty())
            return canonical;
        return normaliseText (genre);   // keep an unknown tag stable
    }

    static juce::String canonicalLearnedKey (LearnedPreference::Kind kind, const juce::String& key)
    {
        if (kind == LearnedPreference::Kind::genre)
        {
            auto canonical = ProfileEngine::normaliseGenre (key);
            return canonical.isNotEmpty() ? canonical : normaliseText (key);
        }
        return key.trim();
    }

    const LearnedPreference* findLearned (LearnedPreference::Kind kind, const juce::String& key) const
    {
        const auto it = learned.find ((int) kind);
        if (it == learned.end())
            return nullptr;
        const auto found = it->second.find (key);
        return found != it->second.end() ? &found->second : nullptr;
    }

    //== decision =============================================================
    static void applyIntelligentRefinements (const TrackAnalysis& analysis,
                                             DspParams& params,
                                             std::vector<juce::String>& reasons)
    {
        // --- Bass: compare the measured low-band share with the 25 % target.
        const double lowShare = analysis.lowEnergyRatio;
        if (lowShare > kLowShareTarget + 0.01)
        {
            const double excess = lowShare - kLowShareTarget;
            const double wanted = juce::jlimit (0.5, 2.5, excess * 20.0);
            const double applied = trimPositiveBass (params.bass, wanted);
            if (applied > 0.05)
            {
                reasons.push_back ("Measured low-band share " + formatPercent (lowShare)
                                   + " (target 25% or less), bass shelf reduced by "
                                   + formatDbMagnitude (applied) + ".");
            }
            else
            {
                reasons.push_back ("Measured low-band share " + formatPercent (lowShare)
                                   + " (target 25% or less); the profile already applies no bass boost, so nothing was added.");
            }
        }
        else if (lowShare > 0.0 && lowShare < kLowShareThin)
        {
            const double lift = juce::jlimit (0.0, 1.0, (kLowShareThin - lowShare) * 8.0);
            if (lift > 0.05)
            {
                params.bass.enabled = true;
                params.bass.gainDb = (float) juce::jlimit ((double) -params.bass.rangeDb,
                                                           (double) params.bass.rangeDb,
                                                           (double) params.bass.gainDb + lift);
                reasons.push_back ("Measured low-band share " + formatPercent (lowShare)
                                   + " (below the 8% target), bass shelf raised by "
                                   + formatDbMagnitude (lift) + ".");
            }
        }

        // --- High end: bright masters get a small shelf cut, dark ones a little air.
        const double centroid = analysis.spectralCentroidHz;
        if (centroid > 3200.0 || analysis.highEnergyRatio > 0.12)
        {
            const double wanted = juce::jlimit (0.5, 1.5, 0.5 + (centroid - 3200.0) / 1600.0);
            const double applied = reduceHighShelf (params.parametric, wanted);
            const juce::String measured = centroid > 0.0
                ? "Measured spectral centroid " + formatHz (centroid)
                : "Measured high-band share " + formatPercent (analysis.highEnergyRatio);
            if (applied > 0.05)
            {
                reasons.push_back (measured + " (bright; spectral centroid threshold 3200 Hz), high shelf reduced by "
                                   + formatDbMagnitude (applied) + ".");
            }
            else
            {
                reasons.push_back (measured + " is bright (above the 3200 Hz threshold); "
                                   "the profile already cuts the top, so it was left alone.");
            }
        }
        else if (centroid > 0.0 && centroid < 900.0)
        {
            const double lift = 0.5;
            auto& highShelf = params.parametric.bands[8];
            highShelf.enabled = true;
            highShelf.bypassed = false;
            highShelf.type = BandType::highShelf;
            highShelf.freqHz = 12000.0;
            highShelf.q = 0.707;
            highShelf.gainDb = juce::jlimit (-3.0, 3.0, highShelf.gainDb + lift);
            reasons.push_back ("Measured spectral centroid " + formatHz (centroid)
                               + " suggests a dark master; high shelf raised by "
                               + formatDbMagnitude (lift) + ".");
        }

        // --- Loudness / dynamics: report, never add compression.
        if (analysis.integratedLufs > -9.0 && analysis.integratedLufs < 0.0)
            reasons.push_back ("Measured integrated loudness " + juce::String (analysis.integratedLufs, 1)
                               + " LUFS is hot; no extra boost is applied, the output limiter handles headroom.");

        if (analysis.crestFactorDb > 0.0)
        {
            if (analysis.crestFactorDb < 9.0)
                reasons.push_back ("Crest factor " + formatDb (analysis.crestFactorDb)
                                   + " suggests dense mastering; no additional compression or boost is applied.");
            else if (analysis.crestFactorDb > 16.0)
                reasons.push_back ("Crest factor " + formatDb (analysis.crestFactorDb)
                                   + " suggests wide dynamics; conservative profile gains are kept.");
        }

        if (analysis.truePeakDb > -0.5 && analysis.truePeakDb < 24.0)
            reasons.push_back ("Measured true peak " + formatDb (analysis.truePeakDb)
                               + " dBTP leaves little headroom; automatic headroom covers any boost.");

        sanitiseParams (params);
    }

    /** Reduces positive bass gains, main shelf first, and returns the dB removed. */
    static double trimPositiveBass (BassParams& bass, double wanted)
    {
        if (! bass.enabled || wanted <= 0.0)
            return 0.0;

        double applied = 0.0;
        auto trim = [&] (float& gain) {
            if (applied >= wanted)
                return;
            const double take = juce::jmin (wanted - applied, (double) juce::jmax (0.0f, gain));
            gain = (float) ((double) gain - take);
            applied += take;
        };
        trim (bass.gainDb);
        if (bass.subEnabled)     trim (bass.subGainDb);
        if (bass.midBassEnabled) trim (bass.midBassGainDb);
        if (bass.punchEnabled)   trim (bass.punchGainDb);
        return applied;
    }

    /** Cuts the high shelf (band 8), tracking how much was removed. */
    static double reduceHighShelf (ParametricEqParams& eq, double wanted)
    {
        if (wanted <= 0.0)
            return 0.0;
        auto& highShelf = eq.bands[8];
        highShelf.type = BandType::highShelf;
        highShelf.freqHz = 10000.0;
        highShelf.q = 0.707;
        const double before = highShelf.enabled ? highShelf.gainDb : 0.0;
        const double target = juce::jmax (-3.0, before - wanted);
        highShelf.enabled = true;
        highShelf.bypassed = false;
        highShelf.gainDb = target;
        return before - target;
    }

    ProfileDecision decideInternal (const Track& track,
                                    const TrackAnalysis* analysis,
                                    DspParams* paramsOut) const
    {
        ProfileDecision d;
        d.mode = mode;

        if (mode == EqMode::manual)
        {
            d.profileName = "Manual";
            d.reasons.push_back ("Manual EQ mode: only your own parameters are applied; no genre profile is selected.");
            if (paramsOut != nullptr)
                *paramsOut = DspParams {};
            return d;
        }

        if (mode == EqMode::reference)
        {
            d.profileName = "Reference";
            d.profileId = "builtin:flat";
            d.reasons.push_back ("Reference mode is active: all DSP is bypassed so the signal path stays unaltered.");
            if (paramsOut != nullptr)
                *paramsOut = ProfileEngine::flatReferenceParams();
            return d;
        }

        // --- Metadata-only suggestion.
        auto profile = profileForGenreNoLock (track.genre);
        d.profileId = profile.id;
        d.profileName = profile.name;
        DspParams params = profile.params;

        const auto genreTag = track.genre.trim();
        if (genreTag.isEmpty())
        {
            d.matchedGenre.clear();
            d.reasons.push_back ("No genre tag is present, so the Flat/Reference profile is used (neutral, no correction).");
        }
        else
        {
            d.matchedGenre = genreTag;
            const auto canonical = ProfileEngine::normaliseGenre (genreTag);
            const auto profileCanonical = ProfileEngine::normaliseGenre (profile.genre);
            if (canonical.isEmpty())
            {
                d.profileId = "builtin:flat";
                d.profileName = "Flat/Reference";
                params = ProfileEngine::flatReferenceParams();
                d.reasons.push_back ("Genre tag \"" + genreTag
                                     + "\" has no matching profile; the Flat/Reference profile is used as a neutral fallback.");
            }
            else if (profileCanonical == canonical)
            {
                d.reasons.push_back ("Genre tag \"" + genreTag + "\" matched the " + profile.name + " profile.");
            }
            else
            {
                d.reasons.push_back ("Genre tag \"" + genreTag + "\" was mapped to \"" + canonical
                                     + "\" and matched the " + profile.name + " profile.");
            }
        }

        // --- Intelligent refinement (only when real measurements exist).
        if (mode == EqMode::intelligent)
        {
            if (analysis != nullptr && analysis->valid)
            {
                d.usedAudioAnalysis = true;
                applyIntelligentRefinements (*analysis, params, d.reasons);
            }
            else
            {
                d.reasons.push_back ("No measured audio analysis is available for this track yet, "
                                     "so the metadata-only genre suggestion is used unchanged.");
            }
        }

        if (paramsOut != nullptr)
            *paramsOut = params;
        return d;
    }

    /** Replaces tone blocks from `tone` while preserving the user's playback
        safety settings (ReplayGain, preamp/limiter, convolution, chain order). */
    static DspParams mergeForPlayback (const DspParams& base, const DspParams& tone)
    {
        DspParams out = base;
        out.bypassed = base.bypassed;
        out.eqBypass = base.eqBypass;
        out.graphic = tone.graphic;
        out.parametric = tone.parametric;
        out.bass = tone.bass;
        out.filters = tone.filters;
        out.stereo = tone.stereo;
        // ReplayGain, preamp/limiter, convolution and chain order stay from base.
        return out;
    }
};

//==============================================================================
// Lifetime / setup
//==============================================================================
ProfileEngine::ProfileEngine (LibraryDatabase& database, LibraryService& library)
    : impl (std::make_unique<Impl> (database, library))
{
}

ProfileEngine::~ProfileEngine()
{
    shutdown();
}

juce::String ProfileEngine::initialise()
{
    const juce::ScopedLock sl (impl->mutex);

    impl->builtIns = createBuiltInProfiles();
    impl->builtInEdits.clear();
    impl->userProfiles.clear();
    impl->songOverrides.clear();
    impl->albumOverrides.clear();
    impl->learned.clear();
    impl->lastError.clear();

    if (! impl->db.isOpen())
    {
        impl->persistenceAvailable = false;
        impl->initialised = true;
        impl->lastError = "Profile database is not open; running in memory only.";
        return {};
    }

    juce::String error;
    if (! impl->ensureSchema (error))
    {
        impl->persistenceAvailable = false;
        impl->initialised = true;
        impl->lastError = error;
        return "Profile storage unavailable: " + error;
    }

    if (! impl->loadProfiles (error) || ! impl->loadOverrides (error) || ! impl->loadLearned (error))
    {
        impl->persistenceAvailable = false;
        impl->initialised = true;
        impl->lastError = error;
        return "Could not load profile data: " + error;
    }

    impl->persistenceAvailable = true;
    impl->initialised = true;
    return {};
}

void ProfileEngine::shutdown()
{
    const juce::ScopedLock sl (impl->mutex);
    impl->initialised = false;
}

bool ProfileEngine::isPersistenceAvailable() const
{
    const juce::ScopedLock sl (impl->mutex);
    return impl->persistenceAvailable;
}

juce::String ProfileEngine::getLastError() const
{
    const juce::ScopedLock sl (impl->mutex);
    return impl->lastError;
}

//==============================================================================
// Profiles
//==============================================================================
std::vector<EqProfile> ProfileEngine::getBuiltInProfiles() const
{
    const juce::ScopedLock sl (impl->mutex);
    return impl->builtIns;
}

std::vector<EqProfile> ProfileEngine::getAllProfiles() const
{
    const juce::ScopedLock sl (impl->mutex);

    std::vector<EqProfile> result;
    result.reserve (impl->builtIns.size() + impl->userProfiles.size());
    for (auto& factory : impl->builtIns)
        result.push_back (impl->effectiveProfile (factory.id));
    for (auto& user : impl->userProfiles)
        result.push_back (user);
    return result;
}

std::vector<EqProfile> ProfileEngine::getUserProfiles() const
{
    const juce::ScopedLock sl (impl->mutex);
    return impl->userProfiles;
}

EqProfile ProfileEngine::getProfile (const juce::String& id) const
{
    const juce::ScopedLock sl (impl->mutex);
    return impl->findProfileNoLock (id);
}

EqProfile ProfileEngine::getProfileForGenre (const juce::String& genre) const
{
    const juce::ScopedLock sl (impl->mutex);
    auto profile = impl->profileForGenreNoLock (genre);
    if (profile.id.isEmpty())
        profile = impl->effectiveProfile ("builtin:flat");
    return profile;
}

juce::String ProfileEngine::findProfileIdForGenre (const juce::String& genre) const
{
    return profileIdForCanonicalGenre (normaliseGenre (genre));
}

bool ProfileEngine::saveUserProfile (const EqProfile& input)
{
    EqProfile profile = input;
    bool changed = false;

    {
        const juce::ScopedLock sl (impl->mutex);

        if (profile.id.isEmpty())
            profile.id = "user:" + juce::Uuid().toString();
        else if (! profile.id.startsWith ("builtin:") && ! profile.id.startsWith ("user:"))
            profile.id = "user:" + profile.id;

        sanitiseParams (profile.params);

        const bool isBuiltInId = profile.id.startsWith ("builtin:")
                              && impl->factoryProfile (profile.id).id.isNotEmpty();

        if (isBuiltInId)
        {
            // Editing a built-in stores an override row; the id and builtIn flag
            // are preserved so references to the built-in remain stable.
            auto factory = impl->factoryProfile (profile.id);
            profile.builtIn = true;
            if (profile.name.isEmpty()) profile.name = factory.name;
            if (profile.genre.isEmpty()) profile.genre = factory.genre;
            if (profile.description.isEmpty()) profile.description = factory.description;

            if (impl->persistenceAvailable && ! impl->persistProfileRow (profile, true))
                return false;

            impl->builtInEdits[profile.id] = profile;
        }
        else
        {
            profile.builtIn = false;
            if (impl->persistenceAvailable && ! impl->persistProfileRow (profile, false))
                return false;

            bool replaced = false;
            for (auto& existing : impl->userProfiles)
            {
                if (existing.id == profile.id)
                {
                    existing = profile;
                    replaced = true;
                    break;
                }
            }
            if (! replaced)
                impl->userProfiles.push_back (profile);
        }

        impl->lastError.clear();
        changed = true;
    }

    if (changed)
        sendChangeMessage();
    return changed;
}

bool ProfileEngine::deleteUserProfile (const juce::String& id)
{
    bool removed = false;
    {
        const juce::ScopedLock sl (impl->mutex);

        if (impl->factoryProfile (id).id.isNotEmpty())
        {
            // Deleting a built-in id means "reset this built-in".
            const auto it = impl->builtInEdits.find (id);
            if (it == impl->builtInEdits.end())
                return false;
            if (impl->persistenceAvailable && ! impl->deleteProfileRow (id, true))
                return false;
            impl->builtInEdits.erase (it);
            impl->lastError.clear();
            removed = true;
        }
        else
        {
            auto it = std::find_if (impl->userProfiles.begin(), impl->userProfiles.end(),
                                    [&] (const EqProfile& p) { return p.id == id; });
            if (it == impl->userProfiles.end())
                return false;
            if (impl->persistenceAvailable && ! impl->deleteProfileRow (id, false))
                return false;
            impl->userProfiles.erase (it);
            impl->lastError.clear();
            removed = true;
        }
    }

    if (removed)
        sendChangeMessage();
    return removed;
}

bool ProfileEngine::resetBuiltInProfile (const juce::String& id)
{
    const juce::ScopedLock sl (impl->mutex);
    if (impl->factoryProfile (id).id.isEmpty())
        return false;
    const auto it = impl->builtInEdits.find (id);
    if (it == impl->builtInEdits.end())
        return false;
    if (impl->persistenceAvailable && ! impl->deleteProfileRow (id, true))
        return false;
    impl->builtInEdits.erase (it);
    impl->lastError.clear();
    return true;
}

bool ProfileEngine::hasBuiltInEdit (const juce::String& id) const
{
    const juce::ScopedLock sl (impl->mutex);
    return impl->builtInEdits.find (id) != impl->builtInEdits.end();
}

//==============================================================================
// Mode
//==============================================================================
EqMode ProfileEngine::getMode() const
{
    const juce::ScopedLock sl (impl->mutex);
    return impl->mode;
}

void ProfileEngine::setMode (EqMode mode)
{
    const juce::ScopedLock sl (impl->mutex);
    impl->mode = mode;
}

//==============================================================================
// Decisions
//==============================================================================
ProfileDecision ProfileEngine::decide (const Track& track, const TrackAnalysis* analysis) const
{
    const juce::ScopedLock sl (impl->mutex);
    return impl->decideInternal (track, analysis, nullptr);
}

DspParams ProfileEngine::resolveParams (const Track& track,
                                        const TrackAnalysis* analysis,
                                        const DspParams& manualParams,
                                        ProfileDecision& decisionOut) const
{
    const juce::ScopedLock sl (impl->mutex);

    // Manual and reference modes short-circuit before any override/learned data.
    auto decision = impl->decideInternal (track, analysis, nullptr);
    if (decision.mode == EqMode::manual)
    {
        decisionOut = decision;
        return manualParams;
    }
    if (decision.mode == EqMode::reference)
    {
        decisionOut = decision;
        return flatReferenceParams();
    }

    DspParams profileParams;
    decision = impl->decideInternal (track, analysis, &profileParams);

    // Precedence: song override > album override > learned song > learned album
    //             > learned genre > profile decision > manual (handled above).
    if (track.id != kInvalidTrackId)
    {
        const auto song = impl->songOverrides.find (track.id);
        if (song != impl->songOverrides.end())
        {
            decision.profileName = "Song override";
            decision.profileId.clear();
            decision.matchedGenre.clear();
            decision.overrideKind = "song";
            decision.usedAudioAnalysis = false;
            decision.learnedPreferenceApplied = false;
            decision.reasons.clear();
            decision.reasons.push_back ("A saved song override is applied for this track; "
                                        "the genre profile and analysis are not used.");
            decisionOut = decision;
            return Impl::mergeForPlayback (manualParams, song->second);
        }
    }

    const auto albumK = albumKey (track);
    if (albumK.isNotEmpty())
    {
        const auto album = impl->albumOverrides.find (albumK);
        if (album != impl->albumOverrides.end())
        {
            decision.profileName = "Album override";
            decision.profileId.clear();
            decision.overrideKind = "album";
            decision.usedAudioAnalysis = false;
            decision.learnedPreferenceApplied = false;
            decision.reasons.clear();
            decision.reasons.push_back ("A saved album override is applied for album \""
                                        + track.displayAlbum() + "\"; the genre profile and analysis are not used.");
            decisionOut = decision;
            return Impl::mergeForPlayback (manualParams, album->second);
        }
    }

    // Learned preferences (song > album > genre).
    if (const auto* learnedSong = impl->findLearned (LearnedPreference::Kind::song, songKey (track)))
    {
        decision.profileName = "Learned song preference";
        decision.overrideKind.clear();
        decision.learnedPreferenceApplied = true;
        decision.usedAudioAnalysis = false;
        decision.reasons.clear();
        decision.reasons.push_back ("Your learned preference for this track is applied instead of the profile.");
        decisionOut = decision;
        return Impl::mergeForPlayback (manualParams, learnedSong->params);
    }

    if (albumK.isNotEmpty())
    {
        if (const auto* learnedAlbum = impl->findLearned (LearnedPreference::Kind::album, albumK))
        {
            decision.profileName = "Learned album preference";
            decision.overrideKind.clear();
            decision.learnedPreferenceApplied = true;
            decision.usedAudioAnalysis = false;
            decision.reasons.clear();
            decision.reasons.push_back ("Your learned preference for album \"" + track.displayAlbum()
                                        + "\" is applied instead of the genre profile.");
            decisionOut = decision;
            return Impl::mergeForPlayback (manualParams, learnedAlbum->params);
        }
    }

    const auto genreKeyForLearning = Impl::learnedGenreKey (track.genre);
    if (genreKeyForLearning.isNotEmpty())
    {
        if (const auto* learnedGenre = impl->findLearned (LearnedPreference::Kind::genre, genreKeyForLearning))
        {
            decision.profileName = "Learned genre preference";
            decision.overrideKind.clear();
            decision.learnedPreferenceApplied = true;
            decision.usedAudioAnalysis = false;
            decision.reasons.clear();
            decision.reasons.push_back ("Your learned preference for genre \"" + track.genre
                                        + "\" is applied instead of the built-in profile.");
            decisionOut = decision;
            return Impl::mergeForPlayback (manualParams, learnedGenre->params);
        }
    }

    decisionOut = decision;
    return Impl::mergeForPlayback (manualParams, profileParams);
}

bool ProfileEngine::wantsAnalysis (const Track& track) const
{
    const juce::ScopedLock sl (impl->mutex);
    if (impl->mode != EqMode::intelligent || ! impl->initialised)
        return false;

    if (track.id != kInvalidTrackId && impl->songOverrides.count (track.id) > 0)
        return false;

    const auto albumK = albumKey (track);
    if (albumK.isNotEmpty() && impl->albumOverrides.count (albumK) > 0)
        return false;

    if (impl->findLearned (LearnedPreference::Kind::song, songKey (track)) != nullptr)
        return false;
    if (albumK.isNotEmpty() && impl->findLearned (LearnedPreference::Kind::album, albumK) != nullptr)
        return false;
    if (impl->findLearned (LearnedPreference::Kind::genre, Impl::learnedGenreKey (track.genre)) != nullptr)
        return false;

    return true;
}

//==============================================================================
// Overrides
//==============================================================================
bool ProfileEngine::hasSongOverride (TrackId id) const
{
    const juce::ScopedLock sl (impl->mutex);
    return id != kInvalidTrackId && impl->songOverrides.count (id) > 0;
}

bool ProfileEngine::hasAlbumOverride (const Track& track) const
{
    const juce::ScopedLock sl (impl->mutex);
    const auto key = albumKey (track);
    return key.isNotEmpty() && impl->albumOverrides.count (key) > 0;
}

DspParams ProfileEngine::getSongOverride (TrackId id) const
{
    const juce::ScopedLock sl (impl->mutex);
    const auto it = impl->songOverrides.find (id);
    return it != impl->songOverrides.end() ? it->second : flatReferenceParams();
}

DspParams ProfileEngine::getAlbumOverride (const Track& track) const
{
    const juce::ScopedLock sl (impl->mutex);
    const auto key = albumKey (track);
    const auto it = impl->albumOverrides.find (key);
    return it != impl->albumOverrides.end() ? it->second : flatReferenceParams();
}

bool ProfileEngine::setSongOverride (const Track& track, const DspParams& params)
{
    if (track.id == kInvalidTrackId)
    {
        const juce::ScopedLock sl (impl->mutex);
        impl->lastError = "A song override needs a library track id; this track has none.";
        return false;
    }

    DspParams sanitised = params;
    sanitiseParams (sanitised);

    {
        const juce::ScopedLock sl (impl->mutex);
        if (impl->persistenceAvailable && ! impl->persistSongOverride (track.id, sanitised))
            return false;
        impl->songOverrides[track.id] = sanitised;
        impl->lastError.clear();
    }
    sendChangeMessage();
    return true;
}

bool ProfileEngine::setAlbumOverride (const Track& track, const DspParams& params)
{
    const auto key = albumKey (track);
    if (key.isEmpty())
    {
        const juce::ScopedLock sl (impl->mutex);
        impl->lastError = "An album override needs album or album-artist metadata; this track has neither.";
        return false;
    }

    DspParams sanitised = params;
    sanitiseParams (sanitised);

    {
        const juce::ScopedLock sl (impl->mutex);
        if (impl->persistenceAvailable && ! impl->persistAlbumOverride (key, sanitised))
            return false;
        impl->albumOverrides[key] = sanitised;
        impl->lastError.clear();
    }
    sendChangeMessage();
    return true;
}

void ProfileEngine::clearSongOverride (TrackId id)
{
    bool removed = false;
    {
        const juce::ScopedLock sl (impl->mutex);
        if (! impl->songOverrides.count (id))
            return;
        if (impl->persistenceAvailable && ! impl->deleteSongOverrideRow (id))
            return;
        impl->songOverrides.erase (id);
        impl->lastError.clear();
        removed = true;
    }
    if (removed)
        sendChangeMessage();
}

void ProfileEngine::clearAlbumOverride (const Track& track)
{
    const auto key = albumKey (track);
    if (key.isEmpty())
        return;

    bool removed = false;
    {
        const juce::ScopedLock sl (impl->mutex);
        if (! impl->albumOverrides.count (key))
            return;
        if (impl->persistenceAvailable && ! impl->deleteAlbumOverrideRow (key))
            return;
        impl->albumOverrides.erase (key);
        impl->lastError.clear();
        removed = true;
    }
    if (removed)
        sendChangeMessage();
}

std::vector<ProfileOverride> ProfileEngine::getAllOverrides() const
{
    const juce::ScopedLock sl (impl->mutex);

    std::vector<ProfileOverride> result;
    result.reserve (impl->songOverrides.size() + impl->albumOverrides.size());
    for (auto& [trackId, params] : impl->songOverrides)
    {
        ProfileOverride o;
        o.kind = ProfileOverride::Kind::song;
        o.key = juce::String (trackId);
        o.params = params;
        result.push_back (o);
    }
    for (auto& [key, params] : impl->albumOverrides)
    {
        ProfileOverride o;
        o.kind = ProfileOverride::Kind::album;
        o.key = key;
        o.params = params;
        result.push_back (o);
    }
    return result;
}

//==============================================================================
// Learning
//==============================================================================
LearnScope ProfileEngine::getLearnScope() const
{
    const juce::ScopedLock sl (impl->mutex);
    return impl->learnScope;
}

void ProfileEngine::setLearnScope (LearnScope scope)
{
    const juce::ScopedLock sl (impl->mutex);
    impl->learnScope = scope;
}

bool ProfileEngine::learnFromUserEdit (const Track& track, const DspParams& editedParams, LearnScope scope)
{
    if (scope == LearnScope::off)
    {
        const juce::ScopedLock sl (impl->mutex);
        impl->lastError = "Learning is off.";
        return false;
    }

    LearnedPreference pref;
    pref.learnedMs = nowMs();
    pref.params = editedParams;
    sanitiseParams (pref.params);

    switch (scope)
    {
        case LearnScope::genre:
        {
            pref.kind = LearnedPreference::Kind::genre;
            pref.key = Impl::learnedGenreKey (track.genre);
            if (pref.key.isEmpty())
            {
                const juce::ScopedLock sl (impl->mutex);
                impl->lastError = "This track has no genre tag, so a genre preference cannot be learned.";
                return false;
            }
            break;
        }
        case LearnScope::album:
        {
            pref.kind = LearnedPreference::Kind::album;
            pref.key = albumKey (track);
            if (pref.key.isEmpty())
            {
                const juce::ScopedLock sl (impl->mutex);
                impl->lastError = "This track has no album metadata, so an album preference cannot be learned.";
                return false;
            }
            break;
        }
        case LearnScope::song:
        {
            pref.kind = LearnedPreference::Kind::song;
            pref.key = songKey (track);
            if (pref.key.isEmpty())
            {
                const juce::ScopedLock sl (impl->mutex);
                impl->lastError = "This track has no file path, so a song preference cannot be learned.";
                return false;
            }
            break;
        }
        case LearnScope::off:
            return false;
    }

    {
        const juce::ScopedLock sl (impl->mutex);
        if (impl->persistenceAvailable && ! impl->persistLearned (pref))
            return false;
        impl->learned[(int) pref.kind][pref.key] = pref;
        impl->lastError.clear();
    }
    sendChangeMessage();
    return true;
}

std::vector<LearnedPreference> ProfileEngine::getAllLearned() const
{
    const juce::ScopedLock sl (impl->mutex);

    std::vector<LearnedPreference> result;
    for (auto& [kind, entries] : impl->learned)
        for (auto& [key, pref] : entries)
            result.push_back (pref);
    return result;
}

bool ProfileEngine::clearLearned (LearnedPreference::Kind kind, const juce::String& key)
{
    const auto canonical = Impl::canonicalLearnedKey (kind, key);
    bool removed = false;

    {
        const juce::ScopedLock sl (impl->mutex);
        const auto kindIt = impl->learned.find ((int) kind);
        if (kindIt != impl->learned.end() && kindIt->second.count (canonical))
        {
            if (impl->persistenceAvailable && ! impl->deleteLearnedRow (kind, canonical))
                return false;
            kindIt->second.erase (canonical);
            impl->lastError.clear();
            removed = true;
        }
    }

    if (removed)
        sendChangeMessage();
    return removed;
}

void ProfileEngine::clearAllLearned()
{
    {
        const juce::ScopedLock sl (impl->mutex);
        if (impl->persistenceAvailable)
        {
            juce::String err;
            const juce::ScopedLock dbLock (impl->db.lock());
            if (! impl->db.db().execute ("DELETE FROM learned_prefs", &err))
            {
                impl->lastError = "Could not clear learned preferences: " + err;
                return;
            }
        }
        impl->learned.clear();
        impl->lastError.clear();
    }
    sendChangeMessage();
}

//==============================================================================
// Analysis
//==============================================================================
bool ProfileEngine::getOrComputeAnalysis (const Track& track, TrackAnalysis& out, juce::String& errorOut)
{
    errorOut.clear();
    out = {};

    if (track.id != kInvalidTrackId)
    {
        TrackAnalysis cached;
        if (impl->library.getAnalysis (track.id, cached) && cached.valid)
        {
            out = cached;
            return true;
        }
    }

    if (track.path.isEmpty())
    {
        errorOut = "This track has no file path, so it cannot be analysed.";
        return false;
    }

    const juce::File file (track.path);
    if (! file.existsAsFile())
    {
        errorOut = "File not found: " + track.path;
        return false;
    }

    if (! TrackAnalyzer::analyzeFile (file, out, 120, errorOut))
    {
        out = {};
        return false;
    }

    if (track.id != kInvalidTrackId)
        impl->library.saveAnalysis (track.id, out);

    return out.valid;
}

//==============================================================================
// Utilities: keys, genre matching, comparison
//==============================================================================
juce::String ProfileEngine::normaliseGenre (const juce::String& genre)
{
    const auto n = normaliseText (genre);
    if (n.isEmpty())
        return {};

    // Pass-through for explicit reference/flat tags.
    if (n == "reference" || n == "flat" || n == "none" || n == "no genre")
        return "reference";

    // --- Hip-hop family (old-school detected first).
    const bool hipHop = containsAny (n, { "hip hop", "hiphop", "rap", "trap", "drill", "grime" });
    if (hipHop)
    {
        const bool oldSchool = containsAny (n, { "old school", "oldschool", "golden era", "boom bap",
                                                 "90s", "1990s", "80s", "classic" });
        return oldSchool ? "old-school-hiphop" : "modern-hiphop";
    }

    // --- R&B family.
    if (isRnbText (n))
    {
        const bool classic = containsAny (n, { "classic", "motown", "60s", "70s", "doo wop" });
        return classic ? "classic-rnb" : "modern-rnb";
    }
    if (containsAny (n, { "motown", "doo wop" }))
        return "classic-rnb";

    // --- Funk before soul so "funk and soul" keeps its snap.
    if (containsAny (n, { "funk", "boogie", "p funk" }))
        return "classic-funk";
    if (containsAny (n, { "soul", "neo soul" }))
        return "soul";
    if (containsAny (n, { "jazz", "bebop", "bossa", "swing", "fusion" }))
        return "jazz";
    if (containsAny (n, { "rock", "metal", "punk", "grunge", "indie", "alternative" }))
        return "rock";
    if (containsAny (n, { "pop", "top 40", "chart" }))
        return "pop";
    if (containsAny (n, { "electronic", "electronica", "edm", "house", "techno", "trance", "dubstep",
                          "drum and bass", "dnb", "idm", "ambient", "dance", "garage", "breakbeat" }))
        return "electronic";

    return {};
}

juce::String ProfileEngine::genreKey (const juce::String& genre)
{
    return normaliseGenre (genre);
}

juce::String ProfileEngine::albumKey (const Track& track)
{
    if (track.albumArtist.trim().isEmpty() && track.album.trim().isEmpty())
        return {};
    return track.albumArtist.trim() + "|" + track.album.trim();
}

juce::String ProfileEngine::songKey (const Track& track)
{
    return track.path.trim();
}

std::vector<ProfileEngine::ParameterDiff> ProfileEngine::compare (const DspParams& a, const DspParams& b)
{
    std::vector<ParameterDiff> diffs;

    auto addText = [&] (const juce::String& label, const juce::String& aText, const juce::String& bText) {
        ParameterDiff d;
        d.label = label;
        d.a = aText;
        d.b = bText;
        diffs.push_back (d);
    };

    auto addGain = [&] (const juce::String& label, double aGain, double bGain, bool aOn = true, bool bOn = true) {
        if (! aOn && ! bOn)
            return;
        if (aOn == bOn && approxEqual (aGain, bGain, 0.02))
            return;
        ParameterDiff d;
        d.label = label;
        d.a = aOn ? formatDb (aGain) : "off";
        d.b = bOn ? formatDb (bGain) : "off";
        d.deltaDb = (float) (bGain - aGain);
        diffs.push_back (d);
    };

    if (a.bypassed != b.bypassed)
        addText ("Processing", a.bypassed ? "Bypassed" : "Active", b.bypassed ? "Bypassed" : "Active");
    if (a.eqBypass != b.eqBypass)
        addText ("EQ bypass", a.eqBypass ? "On" : "Off", b.eqBypass ? "On" : "Off");

    // Graphic EQ (only bands that differ).
    for (int i = 0; i < GraphicEqParams::kNumBands15; ++i)
    {
        const auto aGain = a.graphic.gains15[(size_t) i];
        const auto bGain = b.graphic.gains15[(size_t) i];
        if (approxEqual (aGain, bGain, 0.02) && a.graphic.enabled == b.graphic.enabled)
            continue;
        addGain ("Graphic EQ " + formatHz (GraphicEqParams::centres15()[(size_t) i]),
                 aGain, bGain, a.graphic.enabled, b.graphic.enabled);
    }
    for (int i = 0; i < GraphicEqParams::kNumBands31; ++i)
    {
        const auto aGain = a.graphic.gains31[(size_t) i];
        const auto bGain = b.graphic.gains31[(size_t) i];
        if (approxEqual (aGain, bGain, 0.02) && a.graphic.enabled == b.graphic.enabled)
            continue;
        // Skip 31-band entries that duplicate a 15-band centre already listed.
        bool duplicate = false;
        for (int j = 0; j < GraphicEqParams::kNumBands15; ++j)
            if (approxEqual (GraphicEqParams::centres15()[(size_t) j],
                             GraphicEqParams::centres31()[(size_t) i], 0.1)
                && ! approxEqual (a.graphic.gains15[(size_t) j], b.graphic.gains15[(size_t) j], 0.02))
                duplicate = true;
        if (duplicate)
            continue;
        addGain ("Graphic EQ 31 " + formatHz (GraphicEqParams::centres31()[(size_t) i]),
                 aGain, bGain, a.graphic.enabled, b.graphic.enabled);
    }

    // Parametric bands.
    for (int i = 0; i < ParametricEqParams::kNumBands; ++i)
    {
        const auto& ab = a.parametric.bands[(size_t) i];
        const auto& bb = b.parametric.bands[(size_t) i];
        const bool aOn = ab.enabled && ! ab.bypassed;
        const bool bOn = bb.enabled && ! bb.bypassed;
        const bool structural = ab.type != bb.type
                             || ! approxEqual (ab.freqHz, bb.freqHz, 0.1)
                             || ! approxEqual (ab.q, bb.q, 0.001);
        if (! structural && (! approxEqual (ab.gainDb, bb.gainDb, 0.02)) && (aOn == bOn))
        {
            addGain (bandLabel (bb), ab.gainDb, bb.gainDb, aOn, bOn);
            continue;
        }
        if (! structural && aOn == bOn)
            continue;

        ParameterDiff d;
        d.label = bandLabel (bb);
        d.a = aOn ? (juce::String (bandTypeShortName (ab.type)) + " @ " + formatHz (ab.freqHz)
                    + ", " + formatDb (ab.gainDb))
                  : juce::String ("off");
        d.b = bOn ? (juce::String (bandTypeShortName (bb.type)) + " @ " + formatHz (bb.freqHz)
                    + ", " + formatDb (bb.gainDb))
                  : juce::String ("off");
        d.deltaDb = (float) (bb.gainDb - ab.gainDb);
        if (aOn || bOn)
            diffs.push_back (d);
    }

    // Bass management.
    const auto& ab = a.bass;
    const auto& bb = b.bass;
    const bool aBassOn = ab.enabled && std::abs (ab.gainDb) > 0.02f;
    const bool bBassOn = bb.enabled && std::abs (bb.gainDb) > 0.02f;
    if (! approxEqual (ab.gainDb, bb.gainDb, 0.02)
        || (aBassOn != bBassOn)
        || ! approxEqual (ab.shelfFreqHz, bb.shelfFreqHz, 0.1))
    {
        ParameterDiff d;
        d.label = "Bass @" + formatHz (bBassOn ? bb.shelfFreqHz : ab.shelfFreqHz);
        d.a = ab.enabled ? formatDb (ab.gainDb) : "off";
        d.b = bb.enabled ? formatDb (bb.gainDb) : "off";
        d.deltaDb = bb.gainDb - ab.gainDb;
        if (! (aBassOn == false && bBassOn == false))
            diffs.push_back (d);
    }
    auto addBassBand = [&] (const juce::String& label, bool aEnabled, float aGain, bool bEnabled,
                            float bGain, double freqForLabel) {
        if (aEnabled == bEnabled && approxEqual (aGain, bGain, 0.02))
            return;
        ParameterDiff d;
        d.label = label + " @" + formatHz (freqForLabel);
        d.a = aEnabled ? formatDb (aGain) : "off";
        d.b = bEnabled ? formatDb (bGain) : "off";
        d.deltaDb = bGain - aGain;
        diffs.push_back (d);
    };
    addBassBand ("Sub", ab.subEnabled && std::abs (ab.subGainDb) > 0.02f, ab.subGainDb,
                 bb.subEnabled && std::abs (bb.subGainDb) > 0.02f, bb.subGainDb,
                 bb.subEnabled ? bb.subFreqHz : ab.subFreqHz);
    addBassBand ("Mid-bass", ab.midBassEnabled && std::abs (ab.midBassGainDb) > 0.02f, ab.midBassGainDb,
                 bb.midBassEnabled && std::abs (bb.midBassGainDb) > 0.02f, bb.midBassGainDb,
                 bb.midBassEnabled ? bb.midBassFreqHz : ab.midBassFreqHz);
    addBassBand ("Punch", ab.punchEnabled && std::abs (ab.punchGainDb) > 0.02f, ab.punchGainDb,
                 bb.punchEnabled && std::abs (bb.punchGainDb) > 0.02f, bb.punchGainDb,
                 bb.punchEnabled ? bb.punchFreqHz : ab.punchFreqHz);

    // Filters.
    if (a.filters.highPassEnabled != b.filters.highPassEnabled
        || ! approxEqual (a.filters.highPassHz, b.filters.highPassHz, 0.1))
    {
        ParameterDiff d;
        d.label = "High-pass @ " + formatHz (b.filters.highPassEnabled ? b.filters.highPassHz
                                                                       : a.filters.highPassHz);
        d.a = a.filters.highPassEnabled ? juce::String (a.filters.highPassHz, 0) + " Hz" : "off";
        d.b = b.filters.highPassEnabled ? juce::String (b.filters.highPassHz, 0) + " Hz" : "off";
        diffs.push_back (d);
    }
    if (a.filters.lowPassEnabled != b.filters.lowPassEnabled
        || ! approxEqual (a.filters.lowPassHz, b.filters.lowPassHz, 0.1))
    {
        ParameterDiff d;
        d.label = "Low-pass @ " + formatHz (b.filters.lowPassEnabled ? b.filters.lowPassHz
                                                                     : a.filters.lowPassHz);
        d.a = a.filters.lowPassEnabled ? juce::String (a.filters.lowPassHz, 0) + " Hz" : "off";
        d.b = b.filters.lowPassEnabled ? juce::String (b.filters.lowPassHz, 0) + " Hz" : "off";
        diffs.push_back (d);
    }

    // Stereo.
    if (! approxEqual (a.stereo.widthPercent, b.stereo.widthPercent, 0.05))
        addText ("Stereo width", juce::String (a.stereo.widthPercent, 0) + " %",
                 juce::String (b.stereo.widthPercent, 0) + " %");
    if (! approxEqual (a.stereo.balance, b.stereo.balance, 0.001))
        addText ("Balance", juce::String (a.stereo.balance, 2), juce::String (b.stereo.balance, 2));
    if (a.stereo.monoBassEnabled != b.stereo.monoBassEnabled
        || ! approxEqual (a.stereo.monoBassHz, b.stereo.monoBassHz, 0.1))
        addText ("Mono bass @ " + formatHz (b.stereo.monoBassHz),
                 a.stereo.monoBassEnabled ? "On" : "Off", b.stereo.monoBassEnabled ? "On" : "Off");
    if (a.stereo.crossfeedEnabled != b.stereo.crossfeedEnabled)
        addText ("Crossfeed", a.stereo.crossfeedEnabled ? "On" : "Off", b.stereo.crossfeedEnabled ? "On" : "Off");

    // ReplayGain / preamp / limiter (playback settings).
    if (a.replayGain.enabled != b.replayGain.enabled)
        addText ("ReplayGain", a.replayGain.enabled ? "On" : "Off", b.replayGain.enabled ? "On" : "Off");
    if (a.preamp.limiterEnabled != b.preamp.limiterEnabled)
        addText ("Limiter", a.preamp.limiterEnabled ? "On" : "Off", b.preamp.limiterEnabled ? "On" : "Off");
    if (! approxEqual (a.preamp.manualPreampDb, b.preamp.manualPreampDb, 0.05))
        addGain ("Preamp", a.preamp.manualPreampDb, b.preamp.manualPreampDb);

    return diffs;
}

} // namespace sp
