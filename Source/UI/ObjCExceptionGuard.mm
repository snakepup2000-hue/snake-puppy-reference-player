#include "UI/ObjCExceptionGuard.h"

#import <Foundation/Foundation.h>

namespace sp
{

bool runGuardingObjCExceptions (const std::function<void()>& action, juce::String& errorOut)
{
    @try
    {
        action();
        return true;
    }
    @catch (NSException* exception)
    {
        juce::String name, reason;
        if (exception != nil)
        {
            if (exception.name != nil)
                name = juce::String ([exception.name UTF8String]);
            if (exception.reason != nil)
                reason = juce::String ([exception.reason UTF8String]);
        }
        errorOut = "Objective-C exception";
        if (name.isNotEmpty())
            errorOut << " (" << name << ")";
        if (reason.isNotEmpty())
            errorOut << ": " << reason;
        juce::Logger::writeToLog (errorOut);
        return false;
    }
}

} // namespace sp
