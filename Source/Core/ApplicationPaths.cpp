#include "Core/ApplicationPaths.h"

namespace sp
{

ApplicationPaths ApplicationPaths::resolve()
{
    ApplicationPaths p;
    const auto appSupport = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                             .getChildFile ("Application Support");
    p.appDataDir = appSupport.getChildFile ("Snake Puppy Reference Player");
    p.libraryDb = p.appDataDir.getChildFile ("library.db");
    p.settingsFile = p.appDataDir.getChildFile ("settings.xml");
    p.logFile = p.appDataDir.getChildFile ("snake-puppy.log");
    p.artworkDir = p.appDataDir.getChildFile ("artwork");
    p.presetsDir = p.appDataDir.getChildFile ("presets");
    p.impulseResponsesDir = p.appDataDir.getChildFile ("impulse-responses");
    p.playlistsDir = p.appDataDir.getChildFile ("playlists");
    p.analysisDir = p.appDataDir.getChildFile ("analysis");
    return p;
}

ApplicationPaths ApplicationPaths::forRoot (const juce::File& root)
{
    ApplicationPaths p;
    p.appDataDir = root.getChildFile ("appdata");
    p.libraryDb = p.appDataDir.getChildFile ("library.db");
    p.settingsFile = p.appDataDir.getChildFile ("settings.xml");
    p.logFile = p.appDataDir.getChildFile ("snake-puppy.log");
    p.artworkDir = p.appDataDir.getChildFile ("artwork");
    p.presetsDir = p.appDataDir.getChildFile ("presets");
    p.impulseResponsesDir = p.appDataDir.getChildFile ("impulse-responses");
    p.playlistsDir = p.appDataDir.getChildFile ("playlists");
    p.analysisDir = p.appDataDir.getChildFile ("analysis");
    return p;
}

bool ApplicationPaths::ensureDirectoriesExist (juce::String& errorOut) const
{
    const juce::File dirs[] { appDataDir, artworkDir, presetsDir, impulseResponsesDir, playlistsDir, analysisDir };
    for (auto& d : dirs)
    {
        auto r = d.createDirectory();
        if (r.failed())
        {
            errorOut = "Could not create directory: " + d.getFullPathName() + " (" + r.getErrorMessage() + ")";
            return false;
        }
    }
    return true;
}

} // namespace sp
