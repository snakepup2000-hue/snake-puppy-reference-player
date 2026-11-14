#include "UI/GlobalShortcutMonitor.h"

#import <AppKit/AppKit.h>

namespace sp
{
namespace
{
    id monitorToken = nil;
    juce::ApplicationCommandManager* commandManager = nullptr;
    juce::Component* mainComponent = nullptr;

    static bool isTextInputFocused (juce::Component* focused)
    {
        for (auto* c = focused; c != nullptr; c = c->getParentComponent())
            if (dynamic_cast<juce::TextEditor*> (c) != nullptr)
                return true;
        return false;
    }

    static juce::ModifierKeys modifiersFromEvent (NSEvent* event)
    {
        const auto flags = event.modifierFlags;
        int bits = 0;
        if ((flags & NSEventModifierFlagCommand) != 0) bits |= juce::ModifierKeys::commandModifier;
        if ((flags & NSEventModifierFlagShift)   != 0) bits |= juce::ModifierKeys::shiftModifier;
        if ((flags & NSEventModifierFlagOption)  != 0) bits |= juce::ModifierKeys::altModifier;
        if ((flags & NSEventModifierFlagControl) != 0) bits |= juce::ModifierKeys::ctrlModifier;
        return juce::ModifierKeys (bits);
    }

    static bool toKeyPress (NSEvent* event, juce::KeyPress& result)
    {
        const auto modifiers = modifiersFromEvent (event);
        const int keyCode = (int) event.keyCode;

        switch (keyCode)
        {
            case 49:  result = juce::KeyPress (juce::KeyPress::spaceKey, modifiers, ' '); return true;
            case 123: result = juce::KeyPress (juce::KeyPress::leftKey, modifiers, 0); return true;
            case 124: result = juce::KeyPress (juce::KeyPress::rightKey, modifiers, 0); return true;
            case 125: result = juce::KeyPress (juce::KeyPress::downKey, modifiers, 0); return true;
            case 126: result = juce::KeyPress (juce::KeyPress::upKey, modifiers, 0); return true;
            case 36:  result = juce::KeyPress (juce::KeyPress::returnKey, modifiers, '\r'); return true;
            case 48:  result = juce::KeyPress (juce::KeyPress::tabKey, modifiers, '\t'); return true;
            case 53:  result = juce::KeyPress (juce::KeyPress::escapeKey, modifiers, 0); return true;
            case 51:  result = juce::KeyPress (juce::KeyPress::backspaceKey, modifiers, 0); return true;
            case 117: result = juce::KeyPress (juce::KeyPress::deleteKey, modifiers, 0); return true;
            default: break;
        }

        auto* chars = event.charactersIgnoringModifiers;
        if (chars == nil || [chars length] == 0)
            return false;

        const auto ch = (juce::juce_wchar) [chars characterAtIndex: 0];
        const auto upper = juce::CharacterFunctions::toUpperCase (ch);
        result = juce::KeyPress ((int) upper, modifiers, ch);
        return true;
    }
}

void GlobalShortcutMonitor::install (juce::ApplicationCommandManager& manager,
                                     juce::Component& mainComponentIn)
{
    uninstall();
    commandManager = &manager;
    mainComponent = &mainComponentIn;

    monitorToken = [NSEvent addLocalMonitorForEventsMatchingMask: NSEventMaskKeyDown
                                                         handler: ^NSEvent* (NSEvent* event)
    {
        if (commandManager == nullptr)
            return event;

        // Only the main window owns global transport shortcuts; settings
        // windows and modal dialogs keep their own key handling.
        if (mainComponent == nullptr
            || juce::TopLevelWindow::getActiveTopLevelWindow() != mainComponent->getTopLevelComponent())
            return event;

        auto* focused = juce::Component::getCurrentlyFocusedComponent();
        if (focused != nullptr && isTextInputFocused (focused))
            return event;

        juce::KeyPress key;
        if (! toKeyPress (event, key))
            return event;

        if (commandManager->getKeyMappings() != nullptr
            && commandManager->getKeyMappings()->keyPressed (key, focused))
            return nil;

        return event;
    }];
}

void GlobalShortcutMonitor::uninstall()
{
    if (monitorToken != nil)
    {
        [NSEvent removeMonitor: monitorToken];
        monitorToken = nil;
    }
    commandManager = nullptr;
    mainComponent = nullptr;
}

} // namespace sp
