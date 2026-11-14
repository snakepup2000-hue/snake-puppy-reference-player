/*
    Snake Puppy Reference Player
    Small Objective-C++ guard so an AppKit exception raised inside a UI action
    is reported instead of aborting the process.
*/
#pragma once

#include <juce_core/juce_core.h>
#include <functional>

namespace sp
{
/** Runs `action`, catching NSException. Returns false and fills errorOut when
    an Objective-C exception was raised. */
bool runGuardingObjCExceptions (const std::function<void()>& action, juce::String& errorOut);
} // namespace sp
