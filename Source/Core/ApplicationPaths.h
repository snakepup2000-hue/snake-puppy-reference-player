/*
    Snake Puppy Reference Player
    Application data locations. The player keeps its own container so that it
    never collides with other software:
      ~/Library/Application Support/Snake Puppy Reference Player/
*/
#pragma once

#include <juce_core/juce_core.h>

namespace sp
{

struct ApplicationPaths
{
    juce::File appDataDir;
    juce::File libraryDb;
    juce::File settingsFile;
    juce::File logFile;
    juce::File artworkDir;
    juce::File presetsDir;
    juce::File impulseResponsesDir;
    juce::File playlistsDir;
    juce::File analysisDir;

    static ApplicationPaths resolve();
    bool ensureDirectoriesExist (juce::String& errorOut) const;

    /** For tests: build a sandboxed set of paths rooted at an arbitrary dir. */
    static ApplicationPaths forRoot (const juce::File& root);
};

} // namespace sp
