#include "Core/AppSettings.h"
#include "Core/Serialization.h"

namespace sp
{

namespace
{
const char* kDeviceProfilesKey = "deviceProfiles";
const char* kShortcutPrefix = "shortcut.";

juce::String dspToJson (const DspParams& p) { return juce::JSON::toString (serialize::toVar (p)); }

DspParams dspFromJson (const juce::String& text, const DspParams& fallback)
{
    if (text.isEmpty())
        return fallback;
    juce::var v;
    auto r = juce::JSON::parse (text, v);
    if (r.failed())
        return fallback;
    return serialize::dspFromVar (v, fallback);
}

struct DefaultShortcut { AppAction action; const char* text; };

const DefaultShortcut kDefaults[] = {
    { AppAction::playPause,           "space" },
    { AppAction::stop,                "command+period" },
    { AppAction::nextTrack,           "command+right" },
    { AppAction::previousTrack,       "command+left" },
    { AppAction::seekForward5,        "right" },
    { AppAction::seekBack5,           "left" },
    { AppAction::seekForward30,       "command+shift+right" },
    { AppAction::seekBack30,          "command+shift+left" },
    { AppAction::volumeUp,            "up" },
    { AppAction::volumeDown,          "down" },
    { AppAction::mute,                "shift+m" },
    { AppAction::toggleEqBypass,      "e" },
    { AppAction::toggleReferenceMode, "shift+r" },
    { AppAction::cycleEqMode,         "shift+e" },
    { AppAction::bassUp,              "]" },
    { AppAction::bassDown,            "[" },
    { AppAction::showLibrary,         "command+1" },
    { AppAction::showPlaylists,       "command+2" },
    { AppAction::showQueue,           "command+3" },
    { AppAction::showHealth,          "command+4" },
    { AppAction::toggleDspStudio,     "command+d" },
    { AppAction::toggleFullPlayer,    "command+shift+f" },
    { AppAction::showSettings,        "command+comma" },
    { AppAction::focusSearch,         "command+f" },
    { AppAction::addToQueue,          "command+shift+return" },
    { AppAction::clearQueue,          "command+shift+backspace" },
    { AppAction::rateOneStar,         "option+1" },
    { AppAction::rateTwoStars,        "option+2" },
    { AppAction::rateThreeStars,      "option+3" },
    { AppAction::rateFourStars,       "option+4" },
    { AppAction::rateFiveStars,       "option+5" },
};
} // namespace

AppSettings::AppSettings (const ApplicationPaths& p) : paths (p)
{
    load();
}

AppSettings::~AppSettings()
{
    saveIfDirty();
}

void AppSettings::load()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "Snake Puppy Reference Player";
    options.filenameSuffix = "settings";
    options.folderName = paths.appDataDir.getFullPathName();
    options.osxLibrarySubFolder = "Application Support";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    options.commonToAllUsers = false;
    options.ignoreCaseOfKeyNames = false;

    properties = std::make_unique<juce::PropertiesFile> (paths.settingsFile, options);
    dirty = false;
}

void AppSettings::save()
{
    if (properties != nullptr)
        properties->saveIfNeeded();
    dirty = false;
}

bool AppSettings::saveIfDirty()
{
    if (dirty)
    {
        save();
        return true;
    }
    return false;
}

juce::var AppSettings::get (const juce::String& key, const juce::var& defaultValue) const
{
    if (properties == nullptr || ! properties->containsKey (key))
        return defaultValue;
    return properties->getValue (key);
}

void AppSettings::set (const juce::String& key, const juce::var& value)
{
    if (properties == nullptr) return;
    properties->setValue (key, value);
    dirty = true;
}

juce::String AppSettings::getString (const juce::String& key, const juce::String& def) const
{
    if (properties == nullptr || ! properties->containsKey (key)) return def;
    return properties->getValue (key, def);
}

int AppSettings::getInt (const juce::String& key, int def) const
{
    if (properties == nullptr || ! properties->containsKey (key)) return def;
    return properties->getIntValue (key, def);
}

double AppSettings::getDouble (const juce::String& key, double def) const
{
    if (properties == nullptr || ! properties->containsKey (key)) return def;
    return properties->getDoubleValue (key, def);
}

bool AppSettings::getBool (const juce::String& key, bool def) const
{
    if (properties == nullptr || ! properties->containsKey (key)) return def;
    return properties->getBoolValue (key, def);
}

//==============================================================================
EngineOptions AppSettings::loadEngineOptions() const
{
    EngineOptions o;
    o.gaplessEnabled = getBool ("engine.gapless", o.gaplessEnabled);
    o.nativeSampleRateSwitching = getBool ("engine.nativeRateSwitching", o.nativeSampleRateSwitching);
    o.memoryPlayback = getBool ("engine.memoryPlayback", o.memoryPlayback);
    o.ditherEnabled = getBool ("engine.dither", o.ditherEnabled);
    o.hogModeEnabled = getBool ("engine.hogMode", o.hogModeEnabled);
    o.bufferFrames = juce::jlimit (32, 4096, getInt ("engine.bufferFrames", o.bufferFrames));
    o.preferredSampleRate = getDouble ("engine.preferredSampleRate", o.preferredSampleRate);
    o.resamplerQuality = juce::jlimit (0, 2, getInt ("engine.resamplerQuality", o.resamplerQuality));
    return o;
}

void AppSettings::saveEngineOptions (const EngineOptions& o)
{
    set ("engine.gapless", o.gaplessEnabled);
    set ("engine.nativeRateSwitching", o.nativeSampleRateSwitching);
    set ("engine.memoryPlayback", o.memoryPlayback);
    set ("engine.dither", o.ditherEnabled);
    set ("engine.hogMode", o.hogModeEnabled);
    set ("engine.bufferFrames", o.bufferFrames);
    set ("engine.preferredSampleRate", o.preferredSampleRate);
    set ("engine.resamplerQuality", o.resamplerQuality);
}

//==============================================================================
DspParams AppSettings::loadManualDsp() const
{
    DspParams flat;
    flat.reset();
    return dspFromJson (getString ("dsp.manual"), flat);
}

void AppSettings::saveManualDsp (const DspParams& p) { set ("dsp.manual", dspToJson (p)); }

DspParams AppSettings::loadDefaultDsp() const
{
    DspParams flat;
    flat.reset();
    return dspFromJson (getString ("dsp.default"), flat);
}

void AppSettings::saveDefaultDsp (const DspParams& p) { set ("dsp.default", dspToJson (p)); }

EqMode AppSettings::loadEqMode() const
{
    return (EqMode) juce::jlimit (0, 3, getInt ("dsp.mode", (int) EqMode::manual));
}

void AppSettings::saveEqMode (EqMode m) { set ("dsp.mode", (int) m); }

LearnScope AppSettings::loadLearnScope() const
{
    return (LearnScope) juce::jlimit (0, 3, getInt ("dsp.learnScope", (int) LearnScope::song));
}

void AppSettings::saveLearnScope (LearnScope s) { set ("dsp.learnScope", (int) s); }

juce::String AppSettings::loadLastEqProfileId() const { return getString ("dsp.lastProfileId"); }

void AppSettings::saveLastEqProfileId (const juce::String& id) { set ("dsp.lastProfileId", id); }

//==============================================================================
std::vector<DeviceDspProfile> AppSettings::loadDeviceProfiles() const
{
    std::vector<DeviceDspProfile> result;
    auto text = getString (kDeviceProfilesKey);
    if (text.isEmpty()) return result;
    juce::var v;
    if (juce::JSON::parse (text, v).failed()) return result;
    if (auto* arr = v.getArray())
        for (auto& item : *arr)
            result.push_back (serialize::deviceProfileFromVar (item));
    return result;
}

void AppSettings::saveDeviceProfiles (const std::vector<DeviceDspProfile>& profiles)
{
    juce::Array<juce::var> arr;
    for (auto& p : profiles) arr.add (serialize::toVar (p));
    set (kDeviceProfilesKey, juce::JSON::toString (juce::var (arr)));
}

DeviceDspProfile AppSettings::findDeviceProfile (const DeviceProfileKey& key) const
{
    for (auto& p : loadDeviceProfiles())
        if (p.key.asKey() == key.asKey())
            return p;
    return {};
}

void AppSettings::upsertDeviceProfile (const DeviceDspProfile& p)
{
    auto all = loadDeviceProfiles();
    bool replaced = false;
    for (auto& existing : all)
    {
        if (existing.key.asKey() == p.key.asKey() || (p.id.isNotEmpty() && existing.id == p.id))
        {
            existing = p;
            replaced = true;
            break;
        }
    }
    if (! replaced) all.push_back (p);
    saveDeviceProfiles (all);
}

//==============================================================================
juce::String AppSettings::defaultShortcut (AppAction a)
{
    for (auto& d : kDefaults)
        if (d.action == a)
            return d.text;
    return {};
}

juce::String AppSettings::getShortcut (AppAction a) const
{
    auto key = juce::String (kShortcutPrefix) + appActionName (a);
    if (properties != nullptr && properties->containsKey (key))
        return properties->getValue (key);
    return defaultShortcut (a);
}

void AppSettings::setShortcut (AppAction a, const juce::String& keyPressText)
{
    set (juce::String (kShortcutPrefix) + appActionName (a), keyPressText);
}

std::map<AppAction, juce::String> AppSettings::getAllShortcuts() const
{
    std::map<AppAction, juce::String> m;
    for (int i = (int) AppAction::playPause; i < (int) AppAction::numActions; ++i)
    {
        auto a = (AppAction) i;
        m[a] = getShortcut (a);
    }
    return m;
}

void AppSettings::resetShortcutsToDefaults()
{
    for (int i = (int) AppAction::playPause; i < (int) AppAction::numActions; ++i)
    {
        auto a = (AppAction) i;
        set (juce::String (kShortcutPrefix) + appActionName (a), defaultShortcut (a));
    }
}

//==============================================================================
juce::String AppSettings::getWindowState (const juce::String& windowId) const
{
    return getString ("window." + windowId + ".state");
}

void AppSettings::setWindowState (const juce::String& windowId, const juce::String& state)
{
    set ("window." + windowId + ".state", state);
}

void AppSettings::saveWindowBounds (const juce::String& windowId, juce::Rectangle<int> bounds, bool maximised)
{
    set ("window." + windowId + ".x", bounds.getX());
    set ("window." + windowId + ".y", bounds.getY());
    set ("window." + windowId + ".w", bounds.getWidth());
    set ("window." + windowId + ".h", bounds.getHeight());
    set ("window." + windowId + ".max", maximised);
}

juce::Rectangle<int> AppSettings::loadWindowBounds (const juce::String& windowId, juce::Rectangle<int> def, bool& maximised) const
{
    maximised = getBool ("window." + windowId + ".max", false);
    if (! properties->containsKey ("window." + windowId + ".w"))
        return def;
    return { getInt ("window." + windowId + ".x", def.getX()),
             getInt ("window." + windowId + ".y", def.getY()),
             juce::jmax (320, getInt ("window." + windowId + ".w", def.getWidth())),
             juce::jmax (240, getInt ("window." + windowId + ".h", def.getHeight())) };
}

//==============================================================================
juce::StringArray AppSettings::getRecentFiles() const
{
    juce::StringArray a;
    a.addTokens (getString ("recent.files"), "\n", "");
    a.removeEmptyStrings();
    return a;
}

void AppSettings::addRecentFile (const juce::String& path)
{
    auto a = getRecentFiles();
    a.removeString (path);
    a.insert (0, path);
    while (a.size() > 20) a.remove (a.size() - 1);
    set ("recent.files", a.joinIntoString ("\n"));
}

juce::StringArray AppSettings::getRecentPlaylists() const
{
    juce::StringArray a;
    a.addTokens (getString ("recent.playlists"), "\n", "");
    a.removeEmptyStrings();
    return a;
}

void AppSettings::addRecentPlaylist (const juce::String& path)
{
    auto a = getRecentPlaylists();
    a.removeString (path);
    a.insert (0, path);
    while (a.size() > 10) a.remove (a.size() - 1);
    set ("recent.playlists", a.joinIntoString ("\n"));
}

juce::var AppSettings::loadNetworkConfig() const
{
    auto text = getString ("network.config");
    if (text.isEmpty()) return {};
    juce::var v;
    if (juce::JSON::parse (text, v).failed()) return {};
    return v;
}

void AppSettings::saveNetworkConfig (const juce::var& v) { set ("network.config", juce::JSON::toString (v)); }

bool AppSettings::getDiagnosticsEnabled() const { return getBool ("diagnostics.enabled", true); }
void AppSettings::setDiagnosticsEnabled (bool b) { set ("diagnostics.enabled", b); }

juce::PropertiesFile& AppSettings::getProperties()
{
    jassert (properties != nullptr);
    return *properties;
}

} // namespace sp
