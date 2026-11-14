/*
    Snake Puppy Reference Player
    Application-level keyboard shortcut monitor.

    JUCE only delivers key events to KeyListeners attached to the component
    that currently has focus. A focused table/list/text editor therefore swallows
    Space, arrows and other transport keys before the command manager sees them.
    This monitor intercepts key-down events at the AppKit level, feeds them to
    the ApplicationCommandManager's KeyPressMappingSet, and swallows them only
    when a mapped command actually matches.

    Text editors are exempt so typing in search fields keeps working.
*/
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace sp
{

class GlobalShortcutMonitor
{
public:
    /** Installs a process-wide key-down monitor. Safe to call once. */
    static void install (juce::ApplicationCommandManager& manager, juce::Component& mainComponent);
    static void uninstall();

private:
    GlobalShortcutMonitor() = delete;
};

} // namespace sp
