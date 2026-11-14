/*
    Snake Puppy Reference Player
    Core shared value types. Owned by the Lead: do not change public fields
    without coordinating, because DSP, library, engine, profiles and UI all
    depend on them.
*/
#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace sp
{
using TrackId = std::int64_t;
using PlaylistId = std::int64_t;
using LocationId = std::int64_t;

constexpr TrackId kInvalidTrackId = 0;
constexpr PlaylistId kInvalidPlaylistId = 0;

//==============================================================================
enum class PlaybackState { stopped, paused, playing, loading, error };

const char* playbackStateName (PlaybackState s);

//==============================================================================
/** A single audio file in the library. This is a value type; the UI and the
    audio engine work from copies / snapshots so that the scanner thread can
    update the database without racing readers. */
struct Track
{
    enum class Family { pcmInteger, pcmFloat, lossy, dsd, unknown };

    TrackId id = kInvalidTrackId;
    juce::String path;              // absolute
    juce::String folder;            // absolute parent directory

    juce::String title;
    juce::String artist;
    juce::String albumArtist;
    juce::String album;
    juce::String genre;
    juce::String composer;
    juce::String comment;

    int year = 0;
    int trackNumber = 0;
    int discNumber = 0;
    int rating = 0;                 // 0..5 stars

    std::int64_t durationMs = 0;
    int sampleRate = 0;             // DSD: equivalent PCM rate (e.g. 2822400 for DSD64)
    int bitDepth = 0;               // DSD: 1
    int numChannels = 0;
    int bitrateKbps = 0;

    juce::String formatName;        // "FLAC", "ALAC", "DSF", "MP3", ...
    Family family = Family::unknown;
    bool isDsd = false;
    int dsdRateMultiple = 0;        // 64, 128, 256 ... when isDsd
    float dsdGainDb = -6.0f;        // DSD->PCM conversion gain applied by the decoder

    std::int64_t fileSize = 0;
    juce::int64 fileModifiedMs = 0;

    // Duplicate detection: content hash is computed lazily by the scanner.
    juce::String contentHash;
    bool hashComputed = false;

    bool missing = false;           // file no longer present; kept in library

    float replayGainTrackDb = 0.0f; // 0 means unknown / not present
    bool hasReplayGainTrack = false;
    float replayGainAlbumDb = 0.0f;
    bool hasReplayGainAlbum = false;
    float peakLinear = 0.0f;        // ReplayGain peak, 0 = unknown

    juce::String artworkId;         // hash key in the artwork cache
    juce::String dateAddedIso;      // ISO-8601
    int playCount = 0;
    juce::int64 lastPlayedMs = 0;

    juce::String displayArtist() const { return artist.isNotEmpty() ? artist : albumArtist; }
    juce::String displayTitle() const { return title.isNotEmpty() ? title : juce::File (path).getFileNameWithoutExtension(); }
    juce::String displayAlbum() const { return album.isNotEmpty() ? album : "(No Album)"; }
    juce::String displayGenre() const { return genre.isNotEmpty() ? genre : "(No Genre)"; }
    juce::String key() const { return "track:" + juce::String (id) + ":" + path; }
};

//==============================================================================
struct LibraryLocation
{
    LocationId id = 0;
    juce::String path;
    bool recursive = true;
    bool watch = true;
    bool online = true;
    std::int64_t lastScanMs = 0;
    int trackCount = 0;
};

//==============================================================================
enum class SmartField
{
    artist, album, albumArtist, genre, year, rating, sampleRate, bitDepth,
    format, title, duration, dateAdded, playCount, comment, folder
};

enum class SmartOp
{
    is, isNot, contains, notContains, beginsWith, endsWith,
    greaterThan, lessThan, between, inList
};

struct SmartRule
{
    SmartField field = SmartField::genre;
    SmartOp op = SmartOp::is;
    juce::String text;
    double number = 0.0;
    double number2 = 0.0;      // for between
    juce::StringArray list;    // for inList
};

struct SmartPlaylistDefinition
{
    std::vector<SmartRule> rules;
    bool matchAll = true;
    juce::String sortColumn = "artist";
    bool sortDescending = false;
    int limit = 0;             // 0 = unlimited
};

struct Playlist
{
    PlaylistId id = kInvalidPlaylistId;
    juce::String name;
    bool smart = false;
    SmartPlaylistDefinition smartDef;
    std::vector<TrackId> trackIds;   // manual playlists only
    std::int64_t createdMs = 0;
    std::int64_t modifiedMs = 0;

    const char* key() const { return "playlist"; }
};

//==============================================================================
/** Offline tonal/dynamic analysis of a track, used by the intelligent EQ mode.
    All values are measured from decoded audio, never inferred from metadata. */
struct TrackAnalysis
{
    static constexpr int kVersion = 3;

    bool valid = false;
    int version = 0;
    double analyzedSeconds = 0.0;
    double integratedLufs = 0.0;
    double loudnessRangeLu = 0.0;
    double samplePeakDb = -144.0;
    double truePeakDb = -144.0;
    double crestFactorDb = 0.0;
    double spectralCentroidHz = 0.0;
    double lowEnergyRatio = 0.0;    // 20-120 Hz share of mid/side-summed energy
    double subEnergyRatio = 0.0;    // 20-60 Hz
    double midEnergyRatio = 0.0;    // 120-2000 Hz
    double highEnergyRatio = 0.0;   // 6000-16000 Hz
    double lowBandDb = -144.0;      // absolute band energy (dBFS RMS)
    double midBandDb = -144.0;
    double highBandDb = -144.0;
    juce::String analyzer = "sp-analyzer/1";
};

//==============================================================================
enum class EqMode { manual = 0, autoGenre = 1, intelligent = 2, reference = 3 };

const char* eqModeName (EqMode m);

/** What the profile engine decided for a track, and why. The explanation is
    shown in the UI: the app never applies an unexplained EQ change. */
struct ProfileDecision
{
    EqMode mode = EqMode::manual;
    juce::String profileId;
    juce::String profileName;
    juce::String matchedGenre;
    bool usedAudioAnalysis = false;
    bool learnedPreferenceApplied = false;
    juce::String overrideKind;      // "song", "album", "" (none)
    std::vector<juce::String> reasons;
    juce::String explanation() const;
};

//==============================================================================
struct DeviceProfileKey
{
    juce::String deviceName;        // e.g. "FiiO K11 R2R"
    juce::String outputKind;        // "headphones", "speakers", "dac" or ""

    juce::String asKey() const;
};

//==============================================================================
/** Everything needed to render the Audio Signal Path panel honestly. */
struct SignalPathInfo
{
    enum class Quality
    {
        unknown,
        bitPerfect,        // exclusive, integer device path, rate matched, no processing
        bitTransparent,    // float pipeline, rate matched, no processing (OS may mix)
        processed,         // DSP, volume, RG or resampling active
        error
    };

    juce::String sourceFile;
    juce::String sourceFormat;
    juce::String decoderName;
    int sourceSampleRate = 0;
    int sourceBitDepth = 0;
    int sourceChannels = 0;
    bool sourceIsDsd = false;
    int dsdMultiple = 0;

    bool resampling = false;
    double resampleFrom = 0.0;
    double resampleTo = 0.0;
    juce::String resamplerName;

    std::vector<juce::String> activeDspSteps;
    bool dspBypassed = true;
    bool volumeIsUnity = true;
    float volumeDb = 0.0f;
    bool replayGainApplied = false;
    bool ditherApplied = false;
    juce::String ditherName;

    juce::String outputDeviceName;
    juce::String outputDeviceType;   // "Core Audio", "USB", "Built-in" ...
    int outputSampleRate = 0;
    int outputBitDepth = 0;
    int outputChannels = 0;
    bool hogModeActive = false;
    double bufferMs = 0.0;

    Quality quality = Quality::unknown;
    juce::String qualityLabel() const;
    juce::String summary() const;    // one-line "FLAC 24/96 -> 96 kHz -> DAC (bit-perfect)"
};

//==============================================================================
struct OutputDeviceInfo
{
    juce::String name;
    juce::String typeName;          // Core Audio
    juce::String deviceId;          // JUCE device name / CoreAudio UID
    int numOutputChannels = 0;
    int nativeBitDepth = 0;         // integer bits reported by Core Audio, 0 unknown
    bool isDefault = false;
    std::vector<double> availableSampleRates;
    double currentSampleRate = 0.0;
    int currentBufferFrames = 0;
    juce::String coreAudioUid;
};

//==============================================================================
/** An app action that can be bound to a keyboard shortcut. */
enum class AppAction
{
    none = 0,
    playPause, stop, nextTrack, previousTrack,
    seekForward5, seekBack5, seekForward30, seekBack30,
    volumeUp, volumeDown, mute,
    toggleEqBypass, toggleReferenceMode, cycleEqMode,
    bassUp, bassDown,
    showLibrary, showPlaylists, showQueue, showHealth,
    toggleDspStudio, toggleFullPlayer, showSettings,
    focusSearch, addToQueue, clearQueue,
    rateOneStar, rateTwoStars, rateThreeStars, rateFourStars, rateFiveStars,
    numActions
};

const char* appActionName (AppAction a);
const char* appActionDescription (AppAction a);

//==============================================================================
/** Stable IDs used for ordering/categorising UI lists. */
struct AlbumRef
{
    juce::String name, artist;
    juce::String artworkId;
    int year = 0;
    int trackCount = 0;
    std::int64_t durationMs = 0;
};

struct HealthReport
{
    struct MissingTrack { TrackId id; juce::String path; juce::String title; };
    struct BrokenPlaylist { PlaylistId id; juce::String name; int missingEntries = 0; };
    struct DuplicateGroup { std::vector<TrackId> ids; juce::String reason; };
    struct Issue { juce::String severity; juce::String message; };

    std::vector<MissingTrack> missingTracks;
    std::vector<BrokenPlaylist> brokenPlaylists;
    std::vector<DuplicateGroup> duplicateGroups;
    std::vector<Issue> issues;
    std::int64_t generatedMs = 0;
};

} // namespace sp
