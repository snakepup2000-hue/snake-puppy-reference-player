/*
    Snake Puppy Reference Player
    Genre-aware EQ / profile engine - the "intelligent" layer.

    Three modes:
      manual       : user params only.
      autoGenre    : choose a built-in/user genre profile from file metadata.
      intelligent  : metadata suggestion refined by measured audio analysis
                     (only when a cached TrackAnalysis exists or analysis is
                     requested explicitly).
      reference    : all DSP bypassed (handled by AppContext/AudioEngine).

    Precedence for a track:
      reference mode > song override > album override > learned preference
      (song > album > genre) > profile decision (auto/intelligent) > manual.

    Every decision carries human-readable reasons so the UI can always explain
    why a track sounds the way it does. Nothing is applied silently.

    ---------------------------------------------------------------------------
    Built-in profiles and edits

    The eleven built-in profiles have stable ids ("builtin:...") and are never
    deleted. Editing a built-in through saveUserProfile() stores an *override*
    row keyed by the same id in `eq_profiles` (column `builtin` = 1); the stable
    id and the builtIn flag are preserved and the edited copy is merged into
    getAllProfiles()/getProfile(). resetBuiltInProfile(id) (or
    deleteUserProfile(id)) removes the override so the factory defaults return.
    User-created profiles use an id beginning "user:" and builtIn = false.

    A downloaded/edited profile only shapes tone: graphic EQ, parametric EQ,
    bass management, filters and stereo. ReplayGain, preamp/limiter,
    convolution (IR choice) and chain order are global playback settings and are
    always taken from the caller's base `manualParams` when resolutions are
    merged. The Flat/Reference profile is genuinely neutral: no EQ/bass/stereo
    changes and limiter + ReplayGain disabled.

    ---------------------------------------------------------------------------
    Persistence (SQLite, this module owns its tables via CREATE TABLE IF NOT
    EXISTS in initialise()):

      eq_profiles       id, name, genre, description, builtin, device_key,
                        params_json, updated_ms
      song_overrides    track_id, params_json, updated_ms
      album_overrides   album_key (albumArtist|album), params_json, updated_ms
      learned_prefs     scope ('genre'|'album'|'song'), key, params_json,
                        learned_ms, PRIMARY KEY (scope, key)
      profile_schema_version  id, version      (single row, currently 1)

    Threading: all methods are message-thread / background-analysis safe. State
    is guarded by an internal critical section; DB statements are serialised
    through LibraryDatabase::lock() plus the same mutex. decide()/resolveParams()
    never touch SQLite (they only read the in-memory cache loaded by initialise()
    and updated by the mutators).
*/
#pragma once

#include "Core/DspParams.h"
#include "Core/Types.h"
#include "Audio/DecodeSource.h"
#include "Library/LibraryDatabase.h"
#include "Library/LibraryService.h"
#include <juce_data_structures/juce_data_structures.h>

namespace sp
{

struct LearnedPreference
{
    enum class Kind { genre = 0, album, song };
    Kind kind = Kind::genre;
    juce::String key;               // normalised genre key / album key / song key
    DspParams params;               // full learned params for this scope
    juce::int64 learnedMs = 0;
};

struct ProfileOverride
{
    enum class Kind { song = 0, album };
    Kind kind = Kind::song;
    juce::String key;
    DspParams params;
};

class ProfileEngine : public juce::ChangeBroadcaster
{
public:
    ProfileEngine (LibraryDatabase& database, LibraryService& library);
    ~ProfileEngine() override;

    /** Creates the profile tables, merges built-ins with saved edits and loads
        user profiles, overrides and learned preferences. Empty on success. */
    juce::String initialise();
    void shutdown();

    /** True when initialise() found an open database and could create its
        tables. When false the engine still works in memory (useful for tests
        and for a read-only profile browser). */
    bool isPersistenceAvailable() const;

    /** Last persistence error (empty when the last write succeeded). */
    juce::String getLastError() const;

    //== Profiles =============================================================
    /** The eleven factory profiles, always in a stable order. */
    static std::vector<EqProfile> createBuiltInProfiles();
    /** Built-ins (factory state, ignoring user edits). */
    std::vector<EqProfile> getBuiltInProfiles() const;
    /** Built-ins with any saved edits applied, followed by user profiles. */
    std::vector<EqProfile> getAllProfiles() const;
    std::vector<EqProfile> getUserProfiles() const;
    EqProfile getProfile (const juce::String& id) const;
    /** Returns the genre profile for `genre`, or the Flat/Reference profile
        when nothing matches. */
    EqProfile getProfileForGenre (const juce::String& genre) const;
    /** Returns the stable profile id for `genre`, or "builtin:flat". */
    juce::String findProfileIdForGenre (const juce::String& genre) const;

    /** Saves a user profile. If `profile.id` is a built-in id this stores an
        edit override for that built-in instead (id/builtIn are preserved).
        Profiles with no id get a generated "user:..." id. */
    bool saveUserProfile (const EqProfile&);
    /** Deletes a user profile, or removes an edit of a built-in id (reset). */
    bool deleteUserProfile (const juce::String& id);
    /** Removes a saved edit of a built-in profile, restoring factory defaults.
        Returns false when the built-in is unknown or has no saved edit. */
    bool resetBuiltInProfile (const juce::String& id);
    /** True when `id` names a built-in that currently has a saved edit. */
    bool hasBuiltInEdit (const juce::String& id) const;

    //== Mode =================================================================
    EqMode getMode() const;
    void setMode (EqMode);

    //== Decision =============================================================
    /** Decides the profile for a track. `analysis` may be nullptr (metadata
        only). In manual mode the decision simply reports "Manual"; in
        reference mode it reports "Reference". */
    ProfileDecision decide (const Track&, const TrackAnalysis* analysis) const;

    /** Resolves the full DSP params for a track. `manualParams` is the current
        manual setup, used in manual mode and as the base for playback safety
        settings (ReplayGain / preamp / limiter / convolution / chain order).
        `usedAnalysis` receives the analysis actually used. */
    DspParams resolveParams (const Track&,
                             const TrackAnalysis* analysis,
                             const DspParams& manualParams,
                             ProfileDecision& decisionOut) const;

    /** True when the intelligent mode would benefit from a cached analysis for
        this track (no override/learned preference applies). */
    bool wantsAnalysis (const Track&) const;

    //== Overrides ============================================================
    // Song overrides are keyed by track id, album overrides by albumKey().
    bool hasSongOverride (TrackId) const;
    bool hasAlbumOverride (const Track&) const;
    DspParams getSongOverride (TrackId) const;
    DspParams getAlbumOverride (const Track&) const;
    bool setSongOverride (const Track&, const DspParams&);
    bool setAlbumOverride (const Track&, const DspParams&);
    void clearSongOverride (TrackId);
    void clearAlbumOverride (const Track&);
    std::vector<ProfileOverride> getAllOverrides() const;

    //== Learning =============================================================
    LearnScope getLearnScope() const;
    void setLearnScope (LearnScope);
    /** Stores the user's edited params (full EQ + bass state) as a learned
        preference for `scope` (off = nothing is learned). Genre scope keys on
        normaliseGenre(), album scope on albumKey() and song scope on songKey(). */
    bool learnFromUserEdit (const Track&, const DspParams& editedParams, LearnScope scope);
    std::vector<LearnedPreference> getAllLearned() const;
    bool clearLearned (LearnedPreference::Kind, const juce::String& key);
    void clearAllLearned();

    //== Analysis =============================================================
    /** Returns a cached analysis or computes and caches one. Blocking call:
        run it from a background thread (AppContext does this). */
    bool getOrComputeAnalysis (const Track&, TrackAnalysis& out, juce::String& error);

    //== Utilities ============================================================
    /** Truly neutral params: flat EQ, no bass/filter/stereo change, limiter and
        ReplayGain off. hasActiveProcessing(false) is false for this state. */
    static DspParams flatReferenceParams();
    /** Canonical genre family key used for matching and learning, e.g.
        "modern-hiphop", "classic-rnb", "jazz"; empty when no family matches. */
    static juce::String normaliseGenre (const juce::String& genre);
    /** Alias of normaliseGenre() (documented lookup key). */
    static juce::String genreKey (const juce::String& genre);
    /** Album identity used for album overrides / learned album preferences:
        albumArtist + "|" + album. Empty when both are missing. */
    static juce::String albumKey (const Track&);
    /** Track identity used for learned song preferences: the absolute path. */
    static juce::String songKey (const Track&);

    struct ParameterDiff
    {
        juce::String label;
        juce::String a;
        juce::String b;
        float deltaDb = 0.0f;
    };
    /** Differences between two param sets, for the A/B UI. Only parameters that
        actually differ are listed; dB deltas are formatted to one decimal. */
    static std::vector<ParameterDiff> compare (const DspParams& a, const DspParams& b);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProfileEngine)
};

} // namespace sp
