/*
    Snake Puppy Reference Player - signal path panel.
    Message thread only.
*/
#include "UI/SignalPathPanel.h"

#include <cmath>

namespace sp
{

using ui::drawEmptyState;

SignalPathPanel::SignalPathPanel (AppContext& context) : ctx (context)
{
    setOpaque (true);
    status = ctx.engine().getStatus();
    setSize (820, 640);
    startTimerHz (8);
}

SignalPathPanel::~SignalPathPanel()
{
    stopTimer();
}

void SignalPathPanel::refreshNow()
{
    status = ctx.engine().getStatus();
    repaint();
}

void SignalPathPanel::timerCallback()
{
    const auto newStatus = ctx.engine().getStatus();
    const auto& p = newStatus.signalPath;

    juce::String fp;
    fp << p.sourceFile << p.sourceFormat << p.sourceSampleRate << p.sourceBitDepth
       << p.decoderName << (int) p.resampling << p.resampleFrom << p.resampleTo << p.resamplerName
       << p.outputDeviceName << p.outputSampleRate << p.outputBitDepth << p.bufferMs
       << (int) p.hogModeActive << (int) p.ditherApplied << p.volumeDb << (int) p.quality
       << newStatus.volumeDb << (int) newStatus.muted << (int) newStatus.deviceOpen
       << newStatus.errorMessage << newStatus.cpuPercent << newStatus.underrunCount;

    for (auto& s : p.activeDspSteps)
        fp << s << ",";

    if (fp != lastFingerprint)
    {
        lastFingerprint = fp;
        status = newStatus;
        repaint();
    }
}

//==============================================================================
void SignalPathPanel::resized()
{
    auto r = getLocalBounds().reduced (14, 10);
    headerArea = r.removeFromTop (24);
    r.removeFromTop (8);
    qualityArea = r.removeFromTop (58);
    r.removeFromTop (8);
    engineArea = r.removeFromTop (18);
    r.removeFromTop (8);

    auto columns = r;
    const int leftWidth = juce::jmax (300, (int) ((float) columns.getWidth() * 0.56f));
    auto left = columns.removeFromLeft (leftWidth);
    columns.removeFromLeft (10);
    auto right = columns;

    auto sectionHeight = [] (int rowCount) { return 22 + rowCount * 19 + 12; };
    sourceArea = left.removeFromTop (sectionHeight (5));
    left.removeFromTop (10);
    dspArea = left.removeFromTop (sectionHeight (6));

    outputArea = right.removeFromTop (sectionHeight (7));
    right.removeFromTop (10);
    qualityDetailArea = right.removeFromTop (juce::jmax (sectionHeight (5), right.getHeight()));
}

//==============================================================================
void SignalPathPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    paintHeader (g, headerArea);

    const auto& p = status.signalPath;
    const bool engineOffline = (! status.deviceOpen) && (! p.outputDeviceName.isNotEmpty());

    //== Quality banner ======================================================
    {
        const auto colour = ui::qualityColour (p.quality);
        theme::fillSunken (g, qualityArea);

        auto text = qualityArea.reduced (12, 6);
        g.setColour (colour);
        g.setFont (theme::uiFont (20.0f, true, 0.04f));
        g.drawText (p.qualityLabel().toUpperCase(), text.removeFromTop (26),
                    juce::Justification::centredLeft, false);

        g.setColour (theme::textDim);
        g.setFont (theme::uiFont (11.0f));
        g.drawFittedText (p.summary(), text, juce::Justification::centredLeft, 2);

        // Colour rail on the left edge of the banner.
        g.setColour (colour);
        g.fillRect (qualityArea.getX(), qualityArea.getY(), 3, qualityArea.getHeight());
    }

    //== Engine line =========================================================
    {
        juce::String engine;
        engine << "ENGINE  " << (status.deviceOpen ? "ONLINE" : "OFFLINE")
               << "   |   CPU " << juce::String (status.cpuPercent, 1) << " %"
               << "   |   UNDERRUNS " << (long long) status.underrunCount
               << "   |   QUEUE " << status.queueSize;
        if (status.errorMessage.isNotEmpty())
            engine << "   |   " << status.errorMessage;

        g.setFont (theme::monoFont (9.5f));
        g.setColour (status.errorMessage.isNotEmpty() ? theme::warning : theme::textFaint);
        g.drawText (engine, engineArea, juce::Justification::centredLeft, true);
    }

    if (engineOffline)
    {
        auto area = getLocalBounds().reduced (14, 10);
        area.removeFromTop (24 + 8 + 58 + 8 + 18 + 8);
        drawEmptyState (g, area, "AUDIO ENGINE OFFLINE",
                        status.errorMessage.isNotEmpty() ? status.errorMessage
                                                        : "No output device is open, so there is no signal path to report.",
                        theme::warning);
        return;
    }

    //== SOURCE ==============================================================
    {
        std::vector<std::pair<juce::String, juce::String>> rows;
        rows.push_back ({ "File", p.sourceFile.isNotEmpty() ? juce::File (p.sourceFile).getFileName()
                                                            : juce::String::fromUTF8 ("\xe2\x80\x94") });
        rows.push_back ({ "Format", p.sourceFormat.isNotEmpty() ? p.sourceFormat : juce::String::fromUTF8 ("\xe2\x80\x94") });
        rows.push_back ({ "Sample rate", p.sourceSampleRate > 0 ? formatSampleRate (p.sourceSampleRate)
                                                                 : juce::String::fromUTF8 ("\xe2\x80\x94") });
        rows.push_back ({ "Bit depth", p.sourceBitDepth > 0 ? (p.sourceIsDsd
                                                                  ? "DSD" + juce::String (p.dsdMultiple)
                                                                  : formatBitDepth (p.sourceBitDepth))
                                                            : juce::String::fromUTF8 ("\xe2\x80\x94") });
        rows.push_back ({ "Decoder", p.decoderName.isNotEmpty() ? p.decoderName
                                                                 : juce::String::fromUTF8 ("\xe2\x80\x94") });
        paintSection (g, sourceArea, "SOURCE", rows);
    }

    //== DSP =================================================================
    {
        std::vector<std::pair<juce::String, juce::String>> rows;
        if (p.resampling)
            rows.push_back ({ "Resampling", formatSampleRate (juce::roundToInt (p.resampleFrom))
                                                 + " \xe2\x86\x92 " + formatSampleRate (juce::roundToInt (p.resampleTo))
                                                 + (p.resamplerName.isNotEmpty() ? "  (" + p.resamplerName + ")" : "") });
        else
            rows.push_back ({ "Resampling", "None (native rate)" });

        juce::String steps;
        if (p.dspBypassed)
            steps = "Bypassed";
        else if (p.activeDspSteps.empty())
            steps = "None";
        else
            for (size_t i = 0; i < p.activeDspSteps.size(); ++i)
                steps << (i > 0 ? " + " : "") << p.activeDspSteps[i];

        rows.push_back ({ "Active DSP", steps });
        rows.push_back ({ "Volume", p.volumeIsUnity ? "0.0 dB (unity)" : formatDb (p.volumeDb) });
        rows.push_back ({ "ReplayGain", p.replayGainApplied ? "Applied" : "Off" });
        rows.push_back ({ "Dither", p.ditherApplied ? (p.ditherName.isNotEmpty() ? p.ditherName : "TPDF")
                                                     : "Not needed" });
        rows.push_back ({ "Auto headroom", status.dspStatus.autoHeadroomDb < -0.01f
                                                ? formatDb (status.dspStatus.autoHeadroomDb) : "0.0 dB" });
        paintSection (g, dspArea, "DSP", rows);
    }

    //== OUTPUT ==============================================================
    {
        std::vector<std::pair<juce::String, juce::String>> rows;
        rows.push_back ({ "Device", p.outputDeviceName.isNotEmpty() ? p.outputDeviceName
                                                                     : juce::String::fromUTF8 ("\xe2\x80\x94") });
        rows.push_back ({ "Type", p.outputDeviceType.isNotEmpty() ? p.outputDeviceType
                                                                   : juce::String::fromUTF8 ("\xe2\x80\x94") });
        rows.push_back ({ "Rate", p.outputSampleRate > 0 ? formatSampleRate (p.outputSampleRate)
                                                          : juce::String::fromUTF8 ("\xe2\x80\x94") });
        rows.push_back ({ "Bit depth", p.outputBitDepth > 0 ? formatBitDepth (p.outputBitDepth)
                                                             : juce::String::fromUTF8 ("\xe2\x80\x94") });
        rows.push_back ({ "Channels", p.outputChannels > 0 ? formatChannels (p.outputChannels)
                                                            : juce::String::fromUTF8 ("\xe2\x80\x94") });
        rows.push_back ({ "Buffer", p.bufferMs > 0.0 ? juce::String (p.bufferMs, 1) + " ms" : "-" });
        rows.push_back ({ "Hog mode", p.hogModeActive ? "Active (exclusive)" : "Off" });
        paintSection (g, outputArea, "OUTPUT", rows);
    }

    //== QUALITY DETAIL =====================================================
    {
        std::vector<std::pair<juce::String, juce::String>> rows;
        const auto& d = status.dspStatus;
        rows.push_back ({ "Max boost", formatDb ((float) d.maxPositiveBoostDb) });
        rows.push_back ({ "Preamp", formatDb (d.effectivePreampDb) });
        rows.push_back ({ "Limiter", d.limiterEngaged ? "Engaged" : "Idle" });
        rows.push_back ({ "Limiter GR", formatDb (d.limiterReductionDb) });
        rows.push_back ({ "Convolution", d.convolutionLoaded ? "IR loaded" : "Off" });
        paintSection (g, qualityDetailArea, "PROCESSING DETAIL", rows);
    }
}

void SignalPathPanel::paintHeader (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (area.isEmpty())
        return;
    theme::drawSectionHeader (g, area, "SIGNAL PATH", "WHAT THE ENGINE IS ACTUALLY DOING");

    const auto& p = status.signalPath;
    if (status.referenceMode)
    {
        g.setFont (theme::capsFont (9.0f, 0.12f));
        g.setColour (theme::gold);
        g.drawText ("REFERENCE MODE", area, juce::Justification::centredRight, false);
    }
    else if (p.quality == SignalPathInfo::Quality::bitPerfect)
    {
        g.setFont (theme::capsFont (9.0f, 0.12f));
        g.setColour (theme::gold);
        g.drawText ("BIT-PERFECT PATH", area, juce::Justification::centredRight, false);
    }
}

void SignalPathPanel::paintSection (juce::Graphics& g, juce::Rectangle<int> area,
                                    const juce::String& title,
                                    const std::vector<std::pair<juce::String, juce::String>>& rows)
{
    if (area.isEmpty())
        return;

    theme::fillPanel (g, area);
    auto inner = area.reduced (10, 7);
    auto titleArea = inner.removeFromTop (14);
    theme::drawSectionHeader (g, titleArea, title, {});
    theme::drawHairline (g, inner.getX(), inner.getY(), inner.getRight(), inner.getY(), theme::lineFaint);
    inner.removeFromTop (4);

    const int labelWidth = juce::jmin (116, inner.getWidth() / 3);
    for (auto& row : rows)
    {
        if (inner.getHeight() < 14)
            break;

        auto line = inner.removeFromTop (juce::jmin (19, inner.getHeight()));
        g.setFont (theme::capsFont (8.5f, 0.10f));
        g.setColour (theme::textDim);
        g.drawText (row.first.toUpperCase(), line.removeFromLeft (labelWidth),
                    juce::Justification::centredLeft, false);

        g.setFont (theme::monoFont (10.0f));
        g.setColour (row.second == juce::String::fromUTF8 ("\xe2\x80\x94") ? theme::textFaint
                                                                          : theme::text);
        g.drawFittedText (row.second, line, juce::Justification::centredLeft, 1);
    }
}

} // namespace sp
