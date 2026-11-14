/*
    Snake Puppy Reference Player
    Persistent application settings (PropertiesFile XML):
      - engine / output options
      - window layout and view preferences
      - keyboard shortcuts
      - device DSP profiles
      - genre learning scope and other user preferences

    Library content, playlists, EQ presets and per-track/album overrides live in
    SQLite through LibraryService / PlaylistManager / ProfileEngine.
*/
#pragma once

#include "Audio/AudioEngine.h"
#include "Core/ApplicationPaths.h"
#include "Core/DspParams.h"
#include "Core/Types.h"
#include <juce_data_structures/juce_data_structures.h>
#include <map>

namespace sp
{

class AppSettings
{
public:
    explicit AppSettings (const ApplicationPaths& paths);
    ~AppSettings();

    void load();
    void save();
    bool saveIfDirty();

    //== Generic ==============================================================
    juce::var get (const juce::String& key, const juce::var& defaultValue) const;
    void set (const juce::String& key, const juce::var& value);

    juce::String getString (const juce::String& key, const juce::String& def = {}) const;
    int getInt (const juce::String& key, int def = 0) const;
    double getDouble (const juce::String& key, double def = 0.0) const;
    bool getBool (const juce::String& key, bool def = false) const;

    //== Engine options =======================================================
    EngineOptions loadEngineOptions() const;
    void saveEngineOptions (const EngineOptions&);

    //== DSP ==================================================================
    DspParams loadManualDsp() const;
    void saveManualDsp (const DspParams&);
    DspParams loadDefaultDsp() const;
    void saveDefaultDsp (const DspParams&);
    EqMode loadEqMode() const;
    void saveEqMode (EqMode);
    LearnScope loadLearnScope() const;
    void saveLearnScope (LearnScope);
    juce::String loadLastEqProfileId() const;
    void saveLastEqProfileId (const juce::String&);

    //== Device profiles ======================================================
    std::vector<DeviceDspProfile> loadDeviceProfiles() const;
    void saveDeviceProfiles (const std::vector<DeviceDspProfile>&);
    DeviceDspProfile findDeviceProfile (const DeviceProfileKey&) const;
    void upsertDeviceProfile (const DeviceDspProfile&);

    //== Keyboard shortcuts ===================================================
    juce::String getShortcut (AppAction) const;
    void setShortcut (AppAction, const juce::String& keyPressText);
    std::map<AppAction, juce::String> getAllShortcuts() const;
    void resetShortcutsToDefaults();
    static juce::String defaultShortcut (AppAction);

    //== Window / layout ======================================================
    juce::String getWindowState (const juce::String& windowId) const;
    void setWindowState (const juce::String& windowId, const juce::String& state);
    void saveWindowBounds (const juce::String& windowId, juce::Rectangle<int> bounds, bool maximised);
    juce::Rectangle<int> loadWindowBounds (const juce::String& windowId, juce::Rectangle<int> defaultValue, bool& maximised) const;

    //== Recently used ========================================================
    juce::StringArray getRecentFiles() const;
    void addRecentFile (const juce::String& path);
    juce::StringArray getRecentPlaylists() const;
    void addRecentPlaylist (const juce::String& path);

    //== Network output =======================================================
    juce::var loadNetworkConfig() const;
    void saveNetworkConfig (const juce::var&);

    //== Diagnostics ==========================================================
    bool getDiagnosticsEnabled() const;
    void setDiagnosticsEnabled (bool);

    juce::PropertiesFile& getProperties();

private:
    ApplicationPaths paths;
    std::unique_ptr<juce::PropertiesFile> properties;
    bool dirty = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppSettings)
};

} // namespace sp
