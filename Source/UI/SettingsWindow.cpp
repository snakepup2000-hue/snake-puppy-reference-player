/*
    Snake Puppy Reference Player - Settings window implementation.
    Message thread only.
*/
#include "UI/SettingsWindow.h"

#include "UI/NetworkPanel.h"

#include <cmath>

namespace sp
{

namespace
{
using ui::configureLabel;
using ui::configureSmallLabel;
using ui::drawEmptyState;

void configureCaption (juce::Label& label, const juce::String& text)
{
    configureLabel (label, 8.5f, theme::textDim);
    label.setText (text, juce::dontSendNotification);
    label.setFont (theme::capsFont (8.5f, 0.12f));
}

juce::String promptForName (juce::Component* associated, const juce::String& title,
                            const juce::String& initialText)
{
    juce::AlertWindow window (title, "Enter a name:", juce::MessageBoxIconType::QuestionIcon,
                              associated);
    window.addTextEditor ("name", initialText);
    window.addButton ("SAVE", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window.addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    if (window.runModalLoop() == 1)
        return window.getTextEditor ("name") != nullptr ? window.getTextEditor ("name")->getText().trim()
                                                        : juce::String();
    return {};
}

bool confirmAction (juce::Component* associated, const juce::String& title, const juce::String& message,
                    const juce::String& confirmText = "REMOVE")
{
    juce::AlertWindow window (title, message, juce::MessageBoxIconType::QuestionIcon, associated);
    window.addButton (confirmText, 1);
    window.addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    return window.runModalLoop() == 1;
}

/** Captures one key combination for the shortcut editor. */
class KeyCaptureComponent : public juce::Component
{
public:
    KeyCaptureComponent()
    {
        setWantsKeyboardFocus (true);
        setMouseClickGrabsKeyboardFocus (true);
        setSize (340, 62);
    }

    bool hasCapture = false;
    juce::KeyPress captured;
    std::function<void()> onCaptured;

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::panelSunken);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 3.0f);
        g.setColour (theme::goldDim);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 3.0f, 1.0f);

        g.setColour (hasCapture ? theme::goldBright : theme::textDim);
        g.setFont (theme::monoFont (16.0f, true));
        g.drawFittedText (hasCapture ? captured.getTextDescription().toUpperCase()
                                     : "PRESS A KEY COMBINATION...",
                          getLocalBounds(), juce::Justification::centred, 2);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.getKeyCode() == 0)
            return false;

        // Escape cancels rather than binding itself.
        if (key.getKeyCode() == juce::KeyPress::escapeKey)
        {
            if (onCancelled != nullptr)
                onCancelled();
            return true;
        }

        captured = key;
        hasCapture = true;
        repaint();
        if (onCaptured != nullptr)
            onCaptured();
        return true;
    }

    std::function<void()> onCancelled;

    void focusGained (FocusChangeType) override { repaint(); }
};

//==============================================================================
/** Audio / device / engine options. */
class AudioTab : public juce::Component,
                 private juce::Timer
{
public:
    explicit AudioTab (AppContext& context) : ctx (context)
    {
        setOpaque (true);

        deviceBox.setTextWhenNothingSelected ("No output devices");
        deviceBox.onChange = [this]
        {
            if (updating)
                return;
            rebuildRateList();
        };
        addAndMakeVisible (deviceBox);

        rateBox.setTextWhenNothingSelected ("Auto");
        addAndMakeVisible (rateBox);

        bufferBox.setTextWhenNothingSelected ("512");
        for (int frames : { 64, 128, 256, 512, 1024, 2048 })
            bufferBox.addItem (juce::String (frames) + " frames", frames);
        addAndMakeVisible (bufferBox);

        qualityBox.setTextWhenNothingSelected ("Resampler");
        qualityBox.addItem ("Fast", 1);
        qualityBox.addItem ("High", 2);
        qualityBox.addItem ("Ultra", 3);
        addAndMakeVisible (qualityBox);

        auto addToggle = [this] (juce::ToggleButton& toggle, const juce::String& text,
                                 const juce::String& tooltip)
        {
            toggle.setButtonText (text);
            toggle.setTooltip (tooltip);
            addAndMakeVisible (toggle);
        };

        addToggle (hogToggle, "HOG MODE (EXCLUSIVE)",
                   "Ask Core Audio for exclusive access (kAudioDevicePropertyHogMode)");
        addToggle (nativeRateToggle, "NATIVE RATE SWITCHING",
                   "Follow each file's sample rate when the device allows it");
        addToggle (ditherToggle, "DITHER", "TPDF dither when reducing effective bit depth");
        addToggle (gaplessToggle, "GAPLESS", "Sample-accurate transitions between queue tracks");
        addToggle (memoryToggle, "MEMORY PLAYBACK", "Decode the whole track to RAM when it fits");

        applyButton.onClick = [this] { apply(); };
        addAndMakeVisible (applyButton);

        refreshButton.onClick = [this]
        {
            ctx.refreshOutputDevices();
            rebuildDeviceList();
            ctx.setStatusMessage ("Output devices refreshed.");
        };
        addAndMakeVisible (refreshButton);

        configureLabel (currentLabel, 11.0f, theme::text);
        configureLabel (errorLabel, 10.0f, theme::warning);
        configureSmallLabel (deviceInfoLabel, theme::textDim);
        configureCaption (rateCaption, "SAMPLE RATE");
        configureCaption (bufferCaption, "BUFFER");
        configureCaption (qualityCaption, "RESAMPLER QUALITY");
        addAndMakeVisible (rateCaption);
        addAndMakeVisible (bufferCaption);
        addAndMakeVisible (qualityCaption);
        addAndMakeVisible (deviceInfoLabel);
        configureSmallLabel (hintLabel, theme::textFaint);
        hintLabel.setText ("Engine option changes are saved to the application settings and applied to the "
                           "output device. A device error is shown below rather than silently ignored.",
                           juce::dontSendNotification);
        addAndMakeVisible (currentLabel);
        addAndMakeVisible (errorLabel);
        addAndMakeVisible (hintLabel);

        syncFromOptions();
        rebuildDeviceList();
        setSize (960, 620);
        startTimerHz (2);
    }

    ~AudioTab() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::background);
        auto top = getLocalBounds().reduced (14, 8).removeFromTop (22);
        theme::drawSectionHeader (g, top, "AUDIO / OUTPUT", "DEVICE, RATE, LATENCY AND ENGINE BEHAVIOUR");

        theme::fillPanel (g, outputCard);
        theme::drawSectionHeader (g, outputCard.reduced (12, 6).removeFromTop (16), "OUTPUT DEVICE", "CORE AUDIO");
        theme::fillPanel (g, engineCard);
        theme::drawSectionHeader (g, engineCard.reduced (12, 6).removeFromTop (16), "ENGINE", "PROCESSING AND TRANSPORT");
        theme::fillPanel (g, statusCard);
        theme::drawSectionHeader (g, statusCard.reduced (12, 6).removeFromTop (16), "STATUS", "HONEST STATE");
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 8);
        r.removeFromTop (22);
        r.removeFromTop (8);

        outputCard = r.removeFromTop (144);
        r.removeFromTop (10);
        engineCard = r.removeFromTop (170);
        r.removeFromTop (10);
        statusCard = r;

        auto outputInner = outputCard.reduced (12, 8);
        outputInner.removeFromTop (20);
        {
            auto row = outputInner.removeFromTop (30);
            deviceBox.setBounds (row.removeFromLeft (juce::jmin (380, row.getWidth() * 2 / 3)));
            row.removeFromLeft (10);
            refreshButton.setBounds (row.removeFromLeft (juce::jmin (130, row.getWidth())));
        }
        outputInner.removeFromTop (10);
        {
            auto row = outputInner.removeFromTop (30);
            rateCaption.setBounds (row.removeFromLeft (78));
            rateBox.setBounds (row.removeFromLeft (juce::jmin (190, row.getWidth() / 3)));
            row.removeFromLeft (14);
            bufferCaption.setBounds (row.removeFromLeft (52));
            bufferBox.setBounds (row.removeFromLeft (juce::jmin (170, row.getWidth())));
        }

        auto engineInner = engineCard.reduced (12, 8);
        engineInner.removeFromTop (20);
        auto toggleRow1 = engineInner.removeFromTop (26);
        const int toggleWidth = engineInner.getWidth() / 2 - 10;
        hogToggle.setBounds (toggleRow1.removeFromLeft (toggleWidth));
        toggleRow1.removeFromLeft (20);
        nativeRateToggle.setBounds (toggleRow1.removeFromLeft (toggleWidth));
        auto toggleRow2 = engineInner.removeFromTop (26);
        ditherToggle.setBounds (toggleRow2.removeFromLeft (toggleWidth));
        toggleRow2.removeFromLeft (20);
        gaplessToggle.setBounds (toggleRow2.removeFromLeft (toggleWidth));
        auto toggleRow3 = engineInner.removeFromTop (26);
        memoryToggle.setBounds (toggleRow3.removeFromLeft (toggleWidth));
        toggleRow3.removeFromLeft (20);
        qualityCaption.setBounds (toggleRow3.removeFromLeft (juce::jmin (126, toggleRow3.getWidth() / 2)));
        qualityBox.setBounds (toggleRow3.removeFromLeft (juce::jmin (160, toggleRow3.getWidth())).withHeight (24));

        auto statusInner = statusCard.reduced (12, 8);
        statusInner.removeFromTop (18);
        currentLabel.setBounds (statusInner.removeFromTop (20));
        deviceInfoLabel.setBounds (statusInner.removeFromTop (18));
        errorLabel.setBounds (statusInner.removeFromTop (20));
        statusInner.removeFromTop (6);
        hintLabel.setBounds (statusInner.removeFromTop (34));
        statusInner.removeFromBottom (6);
        auto row = statusInner.removeFromBottom (30);
        applyButton.setBounds (row.removeFromRight (120));
    }

private:
    void timerCallback() override
    {
        const auto status = ctx.engine().getStatus();
        const auto& p = status.signalPath;

        juce::String current;
        current << "CURRENT: " << (p.outputDeviceName.isNotEmpty() ? p.outputDeviceName
                                                                   : juce::String ("no device"))
                << "  |  " << (p.outputSampleRate > 0 ? formatSampleRate (p.outputSampleRate)
                                                              : juce::String ("-"))
                << "  |  " << (p.bufferMs > 0.0 ? juce::String (p.bufferMs, 1) + " ms"
                                                       : juce::String ("-"))
                << "  |  " << (p.hogModeActive ? "hog mode" : "shared mode");
        if (current != currentLabel.getText())
            currentLabel.setText (current, juce::dontSendNotification);

        juce::String error;
        if (status.errorMessage.isNotEmpty())
            error = "ENGINE: " + status.errorMessage;
        if (error != errorLabel.getText())
            errorLabel.setText (error, juce::dontSendNotification);

        updateDeviceInfo();
    }

    void updateDeviceInfo()
    {
        const int index = deviceBox.getSelectedItemIndex();
        juce::String info;
        if (index >= 0 && index < (int) devices.size())
        {
            const auto& device = devices[(size_t) index];
            info << "DEVICE: " << device.typeName
                 << (device.numOutputChannels > 0 ? "   |   " + formatChannels (device.numOutputChannels) : juce::String())
                 << (device.nativeBitDepth > 0 ? "   |   " + formatBitDepth (device.nativeBitDepth) + " native" : juce::String())
                 << (device.availableSampleRates.empty() ? juce::String()
                                                         : "   |   " + juce::String ((int) device.availableSampleRates.size()) + " rates");
        }
        else
        {
            info = "DEVICE: no output device reported by Core Audio.";
        }
        if (deviceInfoLabel.getText() != info)
            deviceInfoLabel.setText (info, juce::dontSendNotification);
    }

    void syncFromOptions()
    {
        updating = true;
        const auto options = ctx.settings().loadEngineOptions();
        hogToggle.setToggleState (options.hogModeEnabled, juce::dontSendNotification);
        nativeRateToggle.setToggleState (options.nativeSampleRateSwitching, juce::dontSendNotification);
        ditherToggle.setToggleState (options.ditherEnabled, juce::dontSendNotification);
        gaplessToggle.setToggleState (options.gaplessEnabled, juce::dontSendNotification);
        memoryToggle.setToggleState (options.memoryPlayback, juce::dontSendNotification);
        qualityBox.setSelectedId (juce::jlimit (1, 3, options.resamplerQuality + 1), juce::dontSendNotification);

        int bufferId = 512;
        for (int frames : { 64, 128, 256, 512, 1024, 2048 })
            if (frames == options.bufferFrames)
                bufferId = frames;
        bufferBox.setSelectedId (bufferId, juce::dontSendNotification);
        updating = false;
    }

    void rebuildDeviceList()
    {
        auto latest = ctx.getOutputDevices();
        devices = std::move (latest);

        updating = true;
        deviceBox.clear (juce::dontSendNotification);
        if (devices.empty())
        {
            deviceBox.addItem ("No output devices available", 1);
            deviceBox.setSelectedId (1, juce::dontSendNotification);
        }
        else
        {
            const auto current = ctx.engine().devices().getCurrentDeviceName();
            int id = 1;
            int selected = 1;
            for (auto& d : devices)
            {
                deviceBox.addItem (d.name.isNotEmpty() ? d.name : d.deviceId, id);
                if (d.name == current)
                    selected = id;
                ++id;
            }
            deviceBox.setSelectedId (selected, juce::dontSendNotification);
        }
        updating = false;

        rebuildRateList();
    }

    void rebuildRateList()
    {
        const int index = deviceBox.getSelectedItemIndex();
        rates.clear();
        updating = true;
        rateBox.clear (juce::dontSendNotification);
        rateBox.addItem ("Auto (follow file)", 1);
        double current = 0.0;

        if (index >= 0 && index < (int) devices.size())
        {
            const auto& device = devices[(size_t) index];
            current = device.currentSampleRate;
            int id = 2;
            for (double rate : device.availableSampleRates)
            {
                rateBox.addItem (juce::String (rate / 1000.0, 1) + " kHz", id++);
                rates.push_back (rate);
            }
        }

        int selected = 1;
        for (int i = 0; i < (int) rates.size(); ++i)
            if (std::abs (rates[(size_t) i] - current) < 1.0)
                selected = i + 2;
        rateBox.setSelectedId (selected, juce::dontSendNotification);
        updating = false;
    }

    void apply()
    {
        EngineOptions options = ctx.engine().getOptions();
        options.hogModeEnabled = hogToggle.getToggleState();
        options.nativeSampleRateSwitching = nativeRateToggle.getToggleState();
        options.ditherEnabled = ditherToggle.getToggleState();
        options.gaplessEnabled = gaplessToggle.getToggleState();
        options.memoryPlayback = memoryToggle.getToggleState();
        options.resamplerQuality = juce::jlimit (0, 2, qualityBox.getSelectedId() - 1);
        options.bufferFrames = bufferBox.getSelectedId() > 0 ? bufferBox.getSelectedId() : 512;

        const int rateIndex = rateBox.getSelectedItemIndex();
        double rate = 0.0;
        if (rateIndex > 0 && rateIndex - 1 < (int) rates.size())
            rate = rates[(size_t) (rateIndex - 1)];
        options.preferredSampleRate = rate;

        ctx.engine().setOptions (options, true);
        ctx.settings().saveEngineOptions (options);
        ctx.settings().saveIfDirty();

        const int deviceIndex = deviceBox.getSelectedItemIndex();
        juce::String error;
        if (deviceIndex >= 0 && deviceIndex < (int) devices.size())
        {
            const auto& device = devices[(size_t) deviceIndex];
            const auto name = device.name.isNotEmpty() ? device.name : device.deviceId;
            if (! ctx.selectOutputDevice (name, rate, error))
                errorLabel.setText ("DEVICE: " + error, juce::dontSendNotification);
            else
                errorLabel.setText ({}, juce::dontSendNotification);
        }

        ctx.setStatusMessage ("Audio settings applied.");
    }

    AppContext& ctx;
    SpComboBox deviceBox, rateBox, bufferBox, qualityBox;
    juce::ToggleButton hogToggle, nativeRateToggle, ditherToggle, gaplessToggle, memoryToggle;
    SpButton applyButton { "APPLY" }, refreshButton { "REFRESH" };
    juce::Label currentLabel, errorLabel, hintLabel, deviceInfoLabel;
    juce::Label rateCaption, bufferCaption, qualityCaption;
    std::vector<OutputDeviceInfo> devices;
    std::vector<double> rates;
    bool updating = false;
    juce::Rectangle<int> outputCard, engineCard, statusCard;
};

//==============================================================================
/** Library locations. */
class LibraryTab : public juce::Component,
                   private juce::ListBoxModel,
                   private juce::Timer
{
public:
    explicit LibraryTab (AppContext& context) : ctx (context)
    {
        setOpaque (true);
        list.setModel (this);
        list.setRowHeight (48);
        addAndMakeVisible (list);

        addButton.onClick = [this] { addFolder(); };
        removeButton.onClick = [this] { removeSelected(); };
        rescanButton.onClick = [this] { ctx.library().rescanAll (true); setStatus ("Scanning all library locations..."); };
        watchButton.setClickingTogglesState (true);
        watchButton.onClick = [this]
        {
            const int index = list.getSelectedRow();
            if (index < 0 || index >= (int) locations.size())
                return;
            ctx.library().setLocationWatch (locations[(size_t) index].id, watchButton.getToggleState());
            setStatus (watchButton.getToggleState() ? "Folder watching enabled." : "Folder watching disabled.");
        };
        clearButton.onClick = [this] { clearLibrary(); };
        addAndMakeVisible (addButton);
        addAndMakeVisible (removeButton);
        addAndMakeVisible (rescanButton);
        addAndMakeVisible (watchButton);
        addAndMakeVisible (clearButton);

        configureLabel (statusLabel, 10.5f, theme::textDim);
        configureSmallLabel (hintLabel, theme::textFaint);
        hintLabel.setText ("Removing a location also removes its indexed tracks from the library "
                           "(the files on disk are never touched). CLEAR LIBRARY removes every "
                           "location and every track from the browser - use it to start over.",
                           juce::dontSendNotification);
        addAndMakeVisible (statusLabel);
        addAndMakeVisible (hintLabel);

        setSize (960, 620);
        refresh();
        startTimerHz (2);
    }

    ~LibraryTab() override
    {
        stopTimer();
        list.setModel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::background);
        auto top = getLocalBounds().reduced (14, 8).removeFromTop (22);
        theme::drawSectionHeader (g, top, "LIBRARY", "FOLDERS, WATCHING AND SCANNING");
        theme::fillPanel (g, listCard);
        theme::drawSectionHeader (g, listCard.reduced (12, 6).removeFromTop (16), "LIBRARY LOCATIONS",
                                  juce::String ((int) locations.size()) + " LOCATIONS");
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 8);
        r.removeFromTop (22);
        r.removeFromTop (8);

        auto buttonRow = r.removeFromTop (30);
        addButton.setBounds (buttonRow.removeFromLeft (120));
        buttonRow.removeFromLeft (8);
        removeButton.setBounds (buttonRow.removeFromLeft (100));
        buttonRow.removeFromLeft (8);
        rescanButton.setBounds (buttonRow.removeFromLeft (120));
        buttonRow.removeFromLeft (8);
        watchButton.setBounds (buttonRow.removeFromLeft (120));
        buttonRow.removeFromLeft (8);
        clearButton.setBounds (buttonRow.removeFromLeft (130));

        r.removeFromTop (8);
        listCard = r;
        list.setBounds (listCard.reduced (1));

        auto bottom = r.removeFromBottom (juce::jmax (34, r.getHeight()));
        juce::ignoreUnused (bottom);
        statusLabel.setBounds (listCard.getX(), listCard.getBottom() + 6, getWidth() - 28, 16);
        hintLabel.setBounds (listCard.getX(), listCard.getBottom() + 24, getWidth() - 28, 30);
    }

private:
    void timerCallback() override { refresh(); }

    void refresh()
    {
        const auto latest = ctx.library().getLocations();
        juce::String signature;
        for (auto& l : latest)
            signature << l.id << "|" << l.path << "|" << (int) l.watch << "|" << l.trackCount << "|" << (int) l.online << ";";

        if (signature != lastSignature)
        {
            lastSignature = signature;
            locations = latest;
            list.updateContent();
            if (list.getSelectedRow() < 0 && ! locations.empty())
                list.selectRow (0);
            repaint();
        }

        const bool scanning = ctx.library().isScanning();
        if (scanning && ! wasScanning)
            setStatus ("Scanning library...");
        else if (! scanning && wasScanning)
            setStatus ("Scan finished.");
        wasScanning = scanning;

        removeButton.setEnabled (list.getSelectedRow() >= 0);
        watchButton.setEnabled (list.getSelectedRow() >= 0);
        clearButton.setEnabled (! locations.empty());
    }

    void setStatus (const juce::String& message) { statusLabel.setText (message, juce::dontSendNotification); }

    void addFolder()
    {
        auto chooser = std::make_shared<juce::FileChooser> (
            "Add library folder", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*");
        juce::Component::SafePointer<LibraryTab> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectDirectories,
                              [safe, chooser] (const juce::FileChooser& fc)
        {
            auto* tab = safe.getComponent();
            if (tab == nullptr)
                return;
            const auto folder = fc.getResult();
            if (folder == juce::File() || ! folder.isDirectory())
                return;

            tab->setStatus ("Adding folder " + folder.getFullPathName() + "...");
            tab->ctx.library().addLocation (folder, true, [safe] (LocationId id)
            {
                if (auto* t = safe.getComponent())
                    t->setStatus (id > 0 ? juce::String ("Folder added to the library.") 
                                         : juce::String ("Could not add the folder."));
            });
        });
    }

    void removeSelected()
    {
        const int index = list.getSelectedRow();
        if (index < 0 || index >= (int) locations.size())
            return;

        const auto& location = locations[(size_t) index];
        if (! confirmAction (this, "REMOVE LIBRARY LOCATION",
                             "Remove \"" + location.path + "\" and its "
                             + juce::String ((int) location.trackCount)
                             + " indexed tracks from the library?\n\nFiles on disk are not deleted."))
            return;

        ctx.library().removeLocation (location.id, true);
        setStatus ("Location and its tracks removed from the library.");
    }

    void clearLibrary()
    {
        if (locations.empty())
            return;

        if (! confirmAction (this, "CLEAR LIBRARY",
                             "Remove all " + juce::String ((int) locations.size())
                             + " library locations and every indexed track from the browser?\n\n"
                               "Files on disk are not deleted. Playlists are kept but will show "
                               "missing entries until you add folders again."))
            return;

        // Remove the locations first so a scan cannot immediately re-add the
        // tracks, then delete every indexed track - including files that were
        // imported directly (drag-drop / Import Files) and therefore have no
        // library location.
        std::vector<TrackId> trackIds;
        if (auto snapshot = ctx.library().getSnapshot())
        {
            trackIds.reserve (snapshot->tracks.size());
            for (auto& track : snapshot->tracks)
                trackIds.push_back (track.id);
        }

        std::vector<LocationId> locationIds;
        locationIds.reserve (locations.size());
        for (auto& location : locations)
            locationIds.push_back (location.id);

        for (auto id : locationIds)
            ctx.library().removeLocation (id, false);

        if (! trackIds.empty())
            ctx.library().removeTracks (trackIds);

        setStatus ("Library cleared. Add a folder to build it again.");
    }

    int getNumRows() override { return (int) locations.size(); }

    void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height,
                           bool rowIsSelected) override
    {
        if (rowNumber < 0 || rowNumber >= (int) locations.size())
            return;

        const auto& location = locations[(size_t) rowNumber];
        juce::Rectangle<int> area (0, 0, width, height);

        if (rowIsSelected)
        {
            g.setColour (theme::selectionFill);
            g.fillRect (area);
            g.setColour (theme::gold);
            g.fillRect (0, area.getY() + 4, 2, area.getHeight() - 8);
        }
        else if (rowNumber % 2 == 1)
        {
            g.setColour (theme::rowAlternate);
            g.fillRect (area);
        }

        auto text = area.reduced (12, 6);
        auto pathRow = text.removeFromTop (18);
        g.setColour (rowIsSelected ? theme::goldBright : theme::text);
        g.setFont (theme::uiFont (12.5f, true));
        g.drawText (location.path, pathRow, juce::Justification::centredLeft, true);

        juce::String detail;
        detail << (int) location.trackCount << " tracks"
               << "  |  " << (location.watch ? "watching" : "not watched")
               << "  |  " << (location.online ? "online" : "offline");

        g.setColour (location.online ? theme::textDim : theme::warning);
        g.setFont (theme::uiFont (9.5f));
        g.drawText (detail, text, juce::Justification::centredLeft, true);
    }

    void selectedRowsChanged (int lastRowSelected) override
    {
        if (lastRowSelected >= 0 && lastRowSelected < (int) locations.size())
            watchButton.setToggleState (locations[(size_t) lastRowSelected].watch, juce::dontSendNotification);
        removeButton.setEnabled (lastRowSelected >= 0);
        watchButton.setEnabled (lastRowSelected >= 0);
    }

    AppContext& ctx;
    juce::ListBox list { "libraryLocations", nullptr };
    SpButton addButton { "ADD FOLDER" }, removeButton { "REMOVE" }, rescanButton { "RESCAN ALL" },
        clearButton { "CLEAR LIBRARY" };
    juce::ToggleButton watchButton { "WATCH FOLDER" };
    juce::Label statusLabel, hintLabel;
    std::vector<LibraryLocation> locations;
    juce::String lastSignature;
    bool wasScanning = false;
    juce::Rectangle<int> listCard;
};

//==============================================================================
/** Keyboard shortcut editor. */
class ShortcutsTab : public juce::Component,
                     private juce::ListBoxModel
{
public:
    explicit ShortcutsTab (AppContext& context) : ctx (context)
    {
        setOpaque (true);
        list.setModel (this);
        list.setRowHeight (30);
        addAndMakeVisible (list);

        setButton.onClick = [this] { captureShortcut(); };
        clearButton.onClick = [this] { clearSelected(); };
        resetButton.onClick = [this]
        {
            ctx.settings().resetShortcutsToDefaults();
            ctx.settings().saveIfDirty();
            reload();
            setStatus ("Shortcuts reset to the application defaults.");
        };
        addAndMakeVisible (setButton);
        addAndMakeVisible (clearButton);
        addAndMakeVisible (resetButton);

        configureSmallLabel (hintLabel, theme::textFaint);
        hintLabel.setText ("Select an action, then press SET SHORTCUT and type the new key combination. "
                           "Esc cancels. Changes are written to the application settings and picked up by the "
                           "command manager immediately.",
                           juce::dontSendNotification);
        configureLabel (statusLabel, 10.5f, theme::textDim);
        addAndMakeVisible (hintLabel);
        addAndMakeVisible (statusLabel);

        setSize (960, 620);
        reload();
    }

    ~ShortcutsTab() override { list.setModel (nullptr); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::background);
        auto top = getLocalBounds().reduced (14, 8).removeFromTop (22);
        theme::drawSectionHeader (g, top, "KEYBOARD SHORTCUTS", "APPLICATION ACTIONS");
        theme::fillPanel (g, card);
        theme::drawSectionHeader (g, card.reduced (12, 6).removeFromTop (16), "ACTIONS",
                                  juce::String ((int) entries.size()) + " BINDINGS");
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 8);
        r.removeFromTop (22);
        r.removeFromTop (8);
        card = r;

        auto buttons = card.removeFromBottom (36);
        setButton.setBounds (buttons.removeFromLeft (160));
        buttons.removeFromLeft (8);
        clearButton.setBounds (buttons.removeFromLeft (110));
        buttons.removeFromLeft (8);
        resetButton.setBounds (buttons.removeFromLeft (150));

        card.removeFromBottom (58);
        list.setBounds (card.reduced (1));
        statusLabel.setBounds (card.getX(), card.getBottom() + 4, getWidth() - 28, 18);
        hintLabel.setBounds (card.getX(), card.getBottom() + 22, getWidth() - 28, 30);
    }

private:
    struct Entry
    {
        AppAction action = AppAction::none;
        juce::String name;
        juce::String shortcut;
    };

    void reload()
    {
        entries.clear();
        for (int i = (int) AppAction::playPause; i < (int) AppAction::numActions; ++i)
        {
            const auto action = (AppAction) i;
            Entry e;
            e.action = action;
            e.name = ctx.getActionDescription (action);
            e.shortcut = ctx.settings().getShortcut (action);
            entries.push_back (e);
        }
        list.updateContent();
        list.repaint();
    }

    void setStatus (const juce::String& text) { statusLabel.setText (text, juce::dontSendNotification); }

    void captureShortcut()
    {
        const int index = list.getSelectedRow();
        if (index < 0 || index >= (int) entries.size())
        {
            setStatus ("Select an action first.");
            return;
        }

        const auto action = entries[(size_t) index].action;
        const auto name = entries[(size_t) index].name;

        juce::AlertWindow window ("SET SHORTCUT", "Press the new key combination for \"" + name + "\".",
                                  juce::MessageBoxIconType::NoIcon, this);
        KeyCaptureComponent capture;
        capture.onCaptured = [&window] { window.exitModalState (1); };
        capture.onCancelled = [&window] { window.exitModalState (0); };
        window.addCustomComponent (&capture);
        window.addButton ("CANCEL", 0);

        juce::Component::SafePointer<KeyCaptureComponent> safeCapture (&capture);
        juce::Timer::callAfterDelay (80, [safeCapture]
        {
            if (auto* component = safeCapture.getComponent())
                component->grabKeyboardFocus();
        });

        const int result = window.runModalLoop();
        if (result != 1 || ! capture.hasCapture)
            return;

        ctx.settings().setShortcut (action, capture.captured.getTextDescription());
        ctx.settings().saveIfDirty();
        reload();
        setStatus ("Shortcut set: " + capture.captured.getTextDescription());
        ctx.setStatusMessage ("Shortcut updated for \"" + name + "\".");
    }

    void clearSelected()
    {
        const int index = list.getSelectedRow();
        if (index < 0 || index >= (int) entries.size())
        {
            setStatus ("Select an action first.");
            return;
        }

        ctx.settings().setShortcut (entries[(size_t) index].action, {});
        ctx.settings().saveIfDirty();
        reload();
        setStatus ("Shortcut cleared for " + entries[(size_t) index].name + ".");
    }

    int getNumRows() override { return (int) entries.size(); }

    void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height,
                           bool rowIsSelected) override
    {
        if (rowNumber < 0 || rowNumber >= (int) entries.size())
            return;

        const auto& entry = entries[(size_t) rowNumber];
        juce::Rectangle<int> area (0, 0, width, height);
        if (rowIsSelected)
        {
            g.setColour (theme::selectionFill);
            g.fillRect (area);
        }
        else if (rowNumber % 2 == 1)
        {
            g.setColour (theme::rowAlternate);
            g.fillRect (area);
        }

        auto text = area.reduced (12, 0);
        auto shortcutArea = text.removeFromRight (juce::jmin (200, width / 3));

        g.setColour (rowIsSelected ? theme::goldBright : theme::text);
        g.setFont (theme::uiFont (11.5f));
        g.drawText (entry.name, text, juce::Justification::centredLeft, true);

        g.setColour (entry.shortcut.isEmpty() ? theme::textFaint : theme::silver);
        g.setFont (theme::monoFont (11.0f));
        g.drawText (entry.shortcut.isEmpty() ? juce::String ("unbound") : entry.shortcut,
                    shortcutArea, juce::Justification::centredRight, false);
    }

    void selectedRowsChanged (int lastRowSelected) override
    {
        setButton.setEnabled (lastRowSelected >= 0);
        clearButton.setEnabled (lastRowSelected >= 0);
    }

    AppContext& ctx;
    juce::ListBox list { "shortcuts", nullptr };
    SpButton setButton { "SET SHORTCUT..." }, clearButton { "CLEAR" }, resetButton { "RESET DEFAULTS" };
    juce::Label hintLabel, statusLabel;
    std::vector<Entry> entries;
    juce::Rectangle<int> card;
};

//==============================================================================
/** Genre / user profile list. */
class ProfilesTab : public juce::Component,
                    private juce::ListBoxModel,
                    private juce::Timer
{
public:
    explicit ProfilesTab (AppContext& context) : ctx (context)
    {
        setOpaque (true);
        list.setModel (this);
        list.setRowHeight (44);
        addAndMakeVisible (list);

        applyButton.onClick = [this]
        {
            const int index = list.getSelectedRow();
            if (index < 0 || index >= (int) profiles.size())
                return;
            ctx.selectProfile (profiles[(size_t) index].id);
            setStatus ("Applied profile: " + profiles[(size_t) index].name);
        };
        deleteButton.onClick = [this]
        {
            const int index = list.getSelectedRow();
            if (index < 0 || index >= (int) profiles.size())
                return;
            const auto& profile = profiles[(size_t) index];
            if (profile.builtIn)
            {
                setStatus ("Built-in profiles are generated and cannot be deleted.");
                return;
            }
            if (! confirmAction (this, "DELETE PROFILE", "Delete the user profile \"" + profile.name + "\"?", "DELETE"))
                return;
            if (ctx.profiles().deleteUserProfile (profile.id))
                setStatus ("Profile deleted.");
            else
                setStatus ("Could not delete the profile.");
            refresh (true);
        };
        saveButton.onClick = [this]
        {
            const auto name = promptForName (this, "SAVE CURRENT SOUND AS PROFILE",
                                             "My Profile " + juce::String (juce::Time::getCurrentTime().getDayOfMonth()));
            if (name.isEmpty())
                return;

            EqProfile profile;
            profile.id = juce::Uuid().toString();
            profile.name = name;
            profile.description = "Saved from Settings";
            profile.builtIn = false;
            profile.params = ctx.getCurrentDspParams();

            const auto track = ctx.engine().getStatus().currentTrack;
            if (track.path.isNotEmpty())
                profile.genre = track.displayGenre();

            if (ctx.profiles().saveUserProfile (profile))
            {
                setStatus ("Profile saved: " + name);
                refresh (true);
            }
            else
            {
                setStatus ("Could not save the profile.");
            }
        };
        resetLearnedButton.onClick = [this]
        {
            if (! confirmAction (this, "RESET LEARNED PREFERENCES",
                                 "Clear all learned genre, album and song preferences? This cannot be undone.",
                                 "RESET"))
                return;
            ctx.profiles().clearAllLearned();
            setStatus ("Learned preferences cleared.");
        };
        addAndMakeVisible (applyButton);
        addAndMakeVisible (deleteButton);
        addAndMakeVisible (saveButton);
        addAndMakeVisible (resetLearnedButton);

        configureSmallLabel (hintLabel, theme::textFaint);
        hintLabel.setText ("Profiles are EQ/bass presets the profile engine selects from in Auto Genre and "
                           "Intelligent modes. To edit one, apply it, adjust the DSP Studio, then save a new "
                           "profile.",
                           juce::dontSendNotification);
        configureLabel (statusLabel, 10.5f, theme::textDim);
        addAndMakeVisible (hintLabel);
        addAndMakeVisible (statusLabel);

        setSize (960, 620);
        refresh (true);
        startTimerHz (2);
    }

    ~ProfilesTab() override
    {
        stopTimer();
        list.setModel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::background);
        auto top = getLocalBounds().reduced (14, 8).removeFromTop (22);
        theme::drawSectionHeader (g, top, "GENRE PROFILES", "BUILT-IN AND USER EQ PROFILES");
        theme::fillPanel (g, card);
        theme::drawSectionHeader (g, card.reduced (12, 6).removeFromTop (16), "PROFILES",
                                  juce::String ((int) profiles.size()) + " PROFILES");

        if (profiles.empty() && ! listArea.isEmpty())
            drawEmptyState (g, listArea.reduced (20), "NO PROFILES PUBLISHED",
                            "The profile engine has not published any built-in or user profiles yet. "
                            "Save the current sound as a profile with SAVE CURRENT AS PROFILE.",
                            theme::textDim);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 8);
        r.removeFromTop (22);
        r.removeFromTop (8);
        card = r;

        auto buttons = card.removeFromBottom (36);
        applyButton.setBounds (buttons.removeFromLeft (100));
        buttons.removeFromLeft (8);
        deleteButton.setBounds (buttons.removeFromLeft (100));
        buttons.removeFromLeft (8);
        saveButton.setBounds (buttons.removeFromLeft (180));
        buttons.removeFromLeft (8);
        resetLearnedButton.setBounds (buttons.removeFromLeft (190));
        card.removeFromBottom (58);
        list.setBounds (card.reduced (1));
        statusLabel.setBounds (card.getX(), card.getBottom() + 4, getWidth() - 28, 18);
        hintLabel.setBounds (card.getX(), card.getBottom() + 22, getWidth() - 28, 30);
    }

private:
    void timerCallback() override { refresh (false); }

    void refresh (bool force)
    {
        auto latest = ctx.profiles().getAllProfiles();
        juce::String signature;
        for (auto& p : latest)
            signature << p.id << "|" << p.name << "|" << p.genre << "|" << (int) p.builtIn << ";";

        if (force || signature != lastSignature)
        {
            lastSignature = signature;
            profiles = std::move (latest);
            list.updateContent();
            repaint();
        }

        applyButton.setEnabled (list.getSelectedRow() >= 0);
        deleteButton.setEnabled (list.getSelectedRow() >= 0);
    }

    void setStatus (const juce::String& text) { statusLabel.setText (text, juce::dontSendNotification); }

    int getNumRows() override { return (int) profiles.size(); }

    void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height,
                           bool rowIsSelected) override
    {
        if (rowNumber < 0 || rowNumber >= (int) profiles.size())
            return;

        const auto& profile = profiles[(size_t) rowNumber];
        juce::Rectangle<int> area (0, 0, width, height);
        if (rowIsSelected)
        {
            g.setColour (theme::selectionFill);
            g.fillRect (area);
        }
        else if (rowNumber % 2 == 1)
        {
            g.setColour (theme::rowAlternate);
            g.fillRect (area);
        }

        auto text = area.reduced (12, 4);
        auto nameArea = text.removeFromTop (18);
        g.setColour (rowIsSelected ? theme::goldBright : theme::text);
        g.setFont (theme::uiFont (12.0f, true));
        g.drawText (profile.name, nameArea, juce::Justification::centredLeft, true);

        juce::String detail;
        if (profile.genre.isNotEmpty())
            detail << "Genre: " << profile.genre << "   ";
        detail << (profile.builtIn ? "built-in" : "user profile");
        if (profile.description.isNotEmpty())
            detail << "  |  " << profile.description;

        g.setColour (theme::textDim);
        g.setFont (theme::uiFont (9.5f));
        g.drawText (detail, text, juce::Justification::centredLeft, true);
    }

    void selectedRowsChanged (int lastRowSelected) override
    {
        applyButton.setEnabled (lastRowSelected >= 0);
        deleteButton.setEnabled (lastRowSelected >= 0);
    }

    AppContext& ctx;
    juce::ListBox list { "profiles", nullptr };
    SpButton applyButton { "APPLY" }, deleteButton { "DELETE" }, saveButton { "SAVE CURRENT AS PROFILE" },
        resetLearnedButton { "CLEAR LEARNED" };
    juce::Label hintLabel, statusLabel;
    std::vector<EqProfile> profiles;
    juce::String lastSignature;
    juce::Rectangle<int> card, listArea;
};

//==============================================================================
/** Diagnostics text. */
class DiagnosticsTab : public juce::Component,
                       private juce::Timer
{
public:
    explicit DiagnosticsTab (AppContext& context) : ctx (context)
    {
        setOpaque (true);

        text.setMultiLine (true);
        text.setReadOnly (true);
        text.setScrollbarsShown (true);
        text.setCaretVisible (true);
        text.setFont (theme::monoFont (11.0f));
        text.setColour (juce::TextEditor::textColourId, theme::text);
        text.setColour (juce::TextEditor::backgroundColourId, theme::panelSunken);
        addAndMakeVisible (text);

        refreshButton.onClick = [this] { refresh (true); };
        addAndMakeVisible (refreshButton);

        copyButton.onClick = [this]
        {
            juce::SystemClipboard::copyTextToClipboard (text.getText());
            statusLabel.setText ("Diagnostics copied to the clipboard.", juce::dontSendNotification);
        };
        addAndMakeVisible (copyButton);

        configureSmallLabel (statusLabel, theme::textDim);
        addAndMakeVisible (statusLabel);

        setSize (960, 620);
        refresh (true);
        startTimerHz (2);
    }

    ~DiagnosticsTab() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::background);
        auto top = getLocalBounds().reduced (14, 8).removeFromTop (22);
        theme::drawSectionHeader (g, top, "DIAGNOSTICS", "ENGINE AND SIGNAL PATH SNAPSHOT");
        theme::fillPanel (g, card);
        theme::drawSectionHeader (g, card.reduced (12, 6).removeFromTop (16), "REPORT", "COPY THIS WHEN REPORTING AN ISSUE");
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 8);
        r.removeFromTop (22);
        r.removeFromTop (8);
        card = r;

        auto buttons = card.removeFromBottom (32);
        refreshButton.setBounds (buttons.removeFromLeft (110));
        buttons.removeFromLeft (8);
        copyButton.setBounds (buttons.removeFromLeft (120));
        buttons.removeFromLeft (10);
        statusLabel.setBounds (buttons);
        card.removeFromBottom (4);
        text.setBounds (card.reduced (1).reduced (6));
    }

private:
    void timerCallback() override { refresh (false); }

    void refresh (bool force)
    {
        auto report = ctx.getDiagnosticsText();
        report << "\nBuild: " << __DATE__ << " " << __TIME__ << "\n";
        report << "JUCE: " << juce::SystemStats::getJUCEVersion() << "\n";
        if (report != text.getText())
            text.setText (report, juce::dontSendNotification);

        if (force)
            statusLabel.setText ("Diagnostics refreshed.", juce::dontSendNotification);
    }

    AppContext& ctx;
    juce::TextEditor text;
    SpButton refreshButton { "REFRESH" }, copyButton { "COPY" };
    juce::Label statusLabel;
    juce::Rectangle<int> card;
};

//==============================================================================
/** About / build information and honest limitations. */
class AboutTab : public juce::Component
{
public:
    explicit AboutTab (AppContext& context) : ctx (context) { setOpaque (true); setSize (960, 620); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::background);
        auto r = getLocalBounds().reduced (24, 16);

        g.setColour (theme::gold);
        g.setFont (theme::uiFont (24.0f, true, 0.10f));
        g.drawText ("SNAKE PUPPY REFERENCE PLAYER", r.removeFromTop (34),
                    juce::Justification::centredLeft, false);

        g.setColour (theme::silverDim);
        g.setFont (theme::uiFont (12.0f));
        g.drawText ("Audiophile macOS player - 64-bit DSP, honest signal path reporting", r.removeFromTop (20),
                    juce::Justification::centredLeft, false);

        r.removeFromTop (12);
        auto build = r.removeFromTop (76);
        theme::fillPanel (g, build);
        auto info = build.reduced (12, 8);
        g.setColour (theme::textDim);
        g.setFont (theme::monoFont (10.5f));

        juce::String lines;
        lines << "Version: 0.1.0\n";
        lines << "Built: " << __DATE__ << " " << __TIME__ << "\n";
        lines << "JUCE: " << juce::SystemStats::getJUCEVersion() << "\n";
        lines << "OS: " << juce::SystemStats::getOperatingSystemName() << "\n";
        lines << "Data: " << ctx.paths().appDataDir.getFullPathName();
        g.drawFittedText (lines, info, juce::Justification::topLeft, 5);

        r.removeFromTop (12);
        theme::fillPanel (g, r);
        auto limitations = r.reduced (12, 8);
        g.setColour (theme::gold);
        g.setFont (theme::capsFont (10.0f, 0.12f));
        g.drawText ("HONEST LIMITATIONS", limitations.removeFromTop (18), juce::Justification::centredLeft, false);

        g.setColour (theme::textDim);
        g.setFont (theme::uiFont (10.5f));
        g.drawFittedText (
            "- Bit-perfect playback is claimed only when the Signal Path panel reports it: chain bypassed, "
            "unity volume, ReplayGain off, no resampling, integer source, exclusive integer device path at the same rate.\n"
            "- Realtime DSP, metering and loudness values are produced by the audio path; when no device or audio "
            "data is available the UI says so instead of inventing values.\n"
            "- Network output implements UPnP/DLNA AVTransport only. AirPlay, Snapcast, Roon and HQPlayer NAA are "
            "not implemented and are listed as unavailable.\n"
            "- DSD files are decoded to PCM before the DSP chain; local DSP does not apply to network playback.\n"
            "- Hog (exclusive) mode and native sample-rate switching depend on Core Audio and the device driver; "
            "refusals are reported rather than hidden.\n"
            "- Convolution requires a compatible impulse response; load failures appear in the chain status.\n"
            "- Device DSP profiles, presets and learned preferences are stored per user account in the application "
            "data folder.",
            limitations, juce::Justification::topLeft, 14);
    }

private:
    AppContext& ctx;
};

} // namespace

//==============================================================================
class SettingsWindow::Content : public juce::Component
{
public:
    explicit Content (AppContext& context)
    {
        setOpaque (true);

        tabs.addTab ("AUDIO", theme::background, new AudioTab (context), true);
        tabs.addTab ("LIBRARY", theme::background, new LibraryTab (context), true);
        tabs.addTab ("SHORTCUTS", theme::background, new ShortcutsTab (context), true);
        tabs.addTab ("PROFILES", theme::background, new ProfilesTab (context), true);
        tabs.addTab ("NETWORK", theme::background, new NetworkPanel (context), true);
        tabs.addTab ("DIAGNOSTICS", theme::background, new DiagnosticsTab (context), true);
        tabs.addTab ("ABOUT", theme::background, new AboutTab (context), true);
        addAndMakeVisible (tabs);

        setSize (980, 700);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::background);
        auto header = getLocalBounds().reduced (14, 0).removeFromTop (46);
        g.setColour (theme::gold);
        g.setFont (theme::uiFont (15.0f, true, 0.14f));
        g.drawText ("SETTINGS", header.removeFromTop (26), juce::Justification::centredLeft, false);

        g.setColour (theme::textFaint);
        g.setFont (theme::capsFont (9.5f, 0.12f));
        g.drawText ("SYSTEM, LIBRARY, INPUT AND NETWORK", header.removeFromTop (14),
                    juce::Justification::centredLeft, false);

        theme::drawHairline (g, 14, 46, getWidth() - 14, 46, theme::line);
    }

    void setTab (int index)
    {
        tabs.setCurrentTabIndex (juce::jlimit (0, juce::jmax (0, tabs.getNumTabs() - 1), index));
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromTop (46);
        tabs.setBounds (r);
    }

private:
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
};

//==============================================================================
namespace
{
SettingsWindow* gSettingsWindow = nullptr;
}

SettingsWindow::SettingsWindow (AppContext& context)
    : DocumentWindow ("Settings", theme::background, DocumentWindow::closeButton),
      ctx (context)
{
    ui::ensureSharedLookAndFeel();

    setUsingNativeTitleBar (true);
    content = new Content (ctx);
    setContentOwned (content, false);
    setResizable (true, true);
    setResizeLimits (900, 620, 2600, 2000);

    bool maximised = false;
    auto bounds = ctx.settings().loadWindowBounds ("settings", { 160, 120, 1000, 720 }, maximised);
    if (maximised)
        bounds = { 120, 80, 1200, 820 };
    setBounds (bounds);
    lastSavedBounds = getBounds();

    startTimer (2000);
}

SettingsWindow::~SettingsWindow()
{
    stopTimer();
}

SettingsWindow* SettingsWindow::show (AppContext& context)
{
    if (gSettingsWindow == nullptr)
        gSettingsWindow = new SettingsWindow (context);

    gSettingsWindow->setVisible (true);
    gSettingsWindow->toFront (true);
    return gSettingsWindow;
}

void SettingsWindow::destroyInstance()
{
    delete gSettingsWindow;
    gSettingsWindow = nullptr;
}

void SettingsWindow::setTab (int index)
{
    if (content != nullptr)
        content->setTab (index);
}

void SettingsWindow::closeButtonPressed()
{
    saveBounds();
    setVisible (false);
}

void SettingsWindow::timerCallback()
{
    saveBounds();
}

void SettingsWindow::saveBounds()
{
    if (! isVisible() || getBounds() == lastSavedBounds)
        return;

    lastSavedBounds = getBounds();
    ctx.settings().saveWindowBounds ("settings", getBounds(), isFullScreen());
}

} // namespace sp
