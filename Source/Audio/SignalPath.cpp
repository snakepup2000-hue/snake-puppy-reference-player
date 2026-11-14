/*
    Snake Puppy Reference Player
    Signal path evaluation. See SignalPath.h for the rules and threading.
*/
#include "Audio/SignalPath.h"

namespace sp
{

namespace
{
bool isUnityVolume (float volumeDb, bool muted)
{
    return ! muted && std::abs (volumeDb) < 1.0e-4f;
}

bool noProcessing (const SignalPathInputs& in)
{
    return in.dspBypassed
        && ! in.dspProcessing
        && in.activeDspSteps.empty()
        && isUnityVolume (in.volumeDb, in.muted)
        && ! in.replayGainApplied
        && ! in.ditherApplied
        && ! in.resampling;
}

bool matchesDeviceRate (const SignalPathInputs& in)
{
    return in.hasTrack
        && in.sourceSampleRate > 0
        && in.outputSampleRate == in.sourceSampleRate;
}

bool exclusiveIntegerPath (const SignalPathInputs& in)
{
    return in.hogModeActive
        && in.integerOutputFormat
        && in.outputBitDepth > 0
        && in.sourceBitDepth > 0
        && in.outputBitDepth >= in.sourceBitDepth;
}
} // namespace

bool isBitPerfect (const SignalPathInputs& in)
{
    return in.hasTrack
        && in.sourceIsIntegerPcm
        && ! in.sourceIsDsd
        && matchesDeviceRate (in)
        && noProcessing (in)
        && exclusiveIntegerPath (in);
}

SignalPathInfo evaluateSignalPath (const SignalPathInputs& in)
{
    SignalPathInfo info;

    info.sourceFile = in.sourceFile;
    info.sourceFormat = in.sourceFormat;
    info.decoderName = in.decoderName;
    info.sourceSampleRate = in.sourceSampleRate;
    info.sourceBitDepth = in.sourceBitDepth;
    info.sourceChannels = in.sourceChannels;
    info.sourceIsDsd = in.sourceIsDsd;
    info.dsdMultiple = in.dsdMultiple;

    info.resampling = in.resampling;
    info.resampleFrom = in.resampleFrom;
    info.resampleTo = in.resampleTo;
    info.resamplerName = in.resamplerName;

    info.activeDspSteps = in.activeDspSteps;
    info.dspBypassed = in.dspBypassed && ! in.dspProcessing && in.activeDspSteps.empty();
    info.volumeIsUnity = isUnityVolume (in.volumeDb, in.muted);
    info.volumeDb = in.volumeDb;
    info.replayGainApplied = in.replayGainApplied;
    info.ditherApplied = in.ditherApplied;
    info.ditherName = in.ditherName;

    info.outputDeviceName = in.outputDeviceName;
    info.outputDeviceType = in.outputDeviceType;
    info.outputSampleRate = in.outputSampleRate;
    info.outputBitDepth = in.outputBitDepth;
    info.outputChannels = in.outputChannels;
    info.hogModeActive = in.hogModeActive;
    info.bufferMs = in.bufferMs;

    if (! in.hasTrack)
    {
        info.quality = SignalPathInfo::Quality::unknown;
        return info;
    }

    if (in.sourceFile.isEmpty() && in.sourceSampleRate <= 0)
    {
        info.quality = SignalPathInfo::Quality::unknown;
        return info;
    }

    if (isBitPerfect (in))
        info.quality = SignalPathInfo::Quality::bitPerfect;
    else if (in.sourceIsDsd)
        info.quality = SignalPathInfo::Quality::processed;   // 1-bit -> PCM conversion
    else if (noProcessing (in) && matchesDeviceRate (in) && in.sourceSampleRate > 0)
        info.quality = SignalPathInfo::Quality::bitTransparent;
    else
        info.quality = SignalPathInfo::Quality::processed;

    return info;
}

} // namespace sp
