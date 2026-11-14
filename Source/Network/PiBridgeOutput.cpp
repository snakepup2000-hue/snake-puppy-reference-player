/*
    Snake Puppy Reference Player
    Direct PiBridge output implementation (SSH + hqdac raw PCM).
*/
#include "Network/PiBridgeOutput.h"

#include "Audio/DecodeSource.h"
#include "DSP/DspChain.h"
#include "DSP/Resampler.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sp
{

namespace
{
juce::String shellQuote (const juce::String& text)
{
    auto escaped = text.replace ("'", "'\\''");
    return "'" + escaped + "'";
}

constexpr int kBlockFrames = 4096;
constexpr double kMaxReceiverRate = 384000.0;
} // namespace

//==============================================================================
struct PiBridgeOutput::Impl : private juce::Thread
{
    Impl() : juce::Thread ("SP PiBridge Output") {}

    ~Impl() override { stop(); }

    //== public-ish state =====================================================
    PiBridgeOutput::State state = PiBridgeOutput::State::idle;
    mutable juce::CriticalSection stateLock;
    juce::String status = "Pi K11 output idle";
    juce::String lastError;
    std::atomic<int> outputRate { 0 };
    std::atomic<std::int64_t> framesSent { 0 };
    std::atomic<std::int64_t> framesTotal { 0 };
    std::int64_t startOffsetMs = 0;

    //== worker internals ====================================================
    PiBridgeConfig config;
    std::unique_ptr<DecodeSource> source;
    std::unique_ptr<DspChain> chain;
    std::unique_ptr<Resampler> resampler;
    DspParams dspParams;
    std::atomic<double> targetGain { 1.0 };
    double appliedGain = 1.0;
    PiBridgeOutput::MonitorCallback monitor;
    std::vector<float> monitorL, monitorR;

    int writeFd = -1;
    int errFd = -1;
    pid_t childPid = 0;
    juce::String errText;

    bool spawnChild (const juce::String& command, juce::String& errorOut)
    {
        int inPipe[2] = { -1, -1 };
        int errPipe[2] = { -1, -1 };
        if (::pipe (inPipe) != 0 || ::pipe (errPipe) != 0)
        {
            errorOut = "Could not create the ssh pipes: " + juce::String (std::strerror (errno));
            return false;
        }

        childPid = ::fork();
        if (childPid < 0)
        {
            ::close (inPipe[0]); ::close (inPipe[1]);
            ::close (errPipe[0]); ::close (errPipe[1]);
            childPid = 0;
            errorOut = "Could not fork ssh: " + juce::String (std::strerror (errno));
            return false;
        }

        if (childPid == 0)
        {
            ::dup2 (inPipe[0], STDIN_FILENO);
            ::dup2 (errPipe[1], STDERR_FILENO);
            ::close (inPipe[0]); ::close (inPipe[1]);
            ::close (errPipe[0]); ::close (errPipe[1]);
            ::execl ("/bin/sh", "sh", "-c", command.toRawUTF8(), (char*) nullptr);
            ::_exit (127);
        }

        ::close (inPipe[0]);
        ::close (errPipe[1]);
        writeFd = inPipe[1];
        errFd = errPipe[0];

        ::fcntl (errFd, F_SETFL, ::fcntl (errFd, F_GETFL, 0) | O_NONBLOCK);
#ifdef F_SETNOSIGPIPE
        ::fcntl (writeFd, F_SETNOSIGPIPE, 1);
#endif
        return true;
    }

    void drainStderr()
    {
        if (errFd < 0)
            return;
        char buffer[1024];
        for (;;)
        {
            const ssize_t n = ::read (errFd, buffer, sizeof (buffer));
            if (n <= 0)
                break;
            errText << juce::String::fromUTF8 (buffer, (int) n);
            if (errText.length() > 4000)
                errText = errText.substring (errText.length() - 4000);
        }
    }

    bool writePcm (const double* left, const double* right, int frames)
    {
        if (writeFd < 0 || frames <= 0)
            return writeFd >= 0;

        std::vector<std::uint8_t> bytes;
        bytes.resize ((size_t) frames * 8);
        auto* p = bytes.data();
        for (int i = 0; i < frames; ++i)
        {
            const double l = juce::jlimit (-1.0, 1.0, left[i]);
            const double r = juce::jlimit (-1.0, 1.0, right[i]);
            const auto li = (std::int32_t) std::llround (l * 2147483647.0);
            const auto ri = (std::int32_t) std::llround (r * 2147483647.0);
            const std::uint32_t ul = (std::uint32_t) li;
            const std::uint32_t ur = (std::uint32_t) ri;
            p[0] = (std::uint8_t) (ul & 0xff);
            p[1] = (std::uint8_t) ((ul >> 8) & 0xff);
            p[2] = (std::uint8_t) ((ul >> 16) & 0xff);
            p[3] = (std::uint8_t) ((ul >> 24) & 0xff);
            p[4] = (std::uint8_t) (ur & 0xff);
            p[5] = (std::uint8_t) ((ur >> 8) & 0xff);
            p[6] = (std::uint8_t) ((ur >> 16) & 0xff);
            p[7] = (std::uint8_t) ((ur >> 24) & 0xff);
            p += 8;
        }

        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            const ssize_t n = ::write (writeFd, bytes.data() + offset, bytes.size() - offset);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (n == 0)
                return false;
            offset += (std::size_t) n;
        }
        framesSent.fetch_add (frames, std::memory_order_relaxed);
        return true;
    }

    void processAndWrite (juce::AudioBuffer<double>& buffer, int frames)
    {
        juce::AudioBuffer<double> view (buffer.getArrayOfWritePointers(), 2, frames);
        chain->process (view);

        auto* l = buffer.getWritePointer (0);
        auto* r = buffer.getWritePointer (1);
        const double target = targetGain.load (std::memory_order_relaxed);
        if (appliedGain != target)
        {
            // Ramp over this block so live volume moves cannot click.
            const double step = (target - appliedGain) / juce::jmax (1, frames);
            auto gain = appliedGain;
            for (int i = 0; i < frames; ++i)
            {
                gain += step;
                l[i] *= gain;
                r[i] *= gain;
            }
            appliedGain = target;
        }
        else if (target != 1.0)
        {
            for (int i = 0; i < frames; ++i)
            {
                l[i] *= target;
                r[i] *= target;
            }
        }

        if (monitor != nullptr)
        {
            monitorL.resize ((size_t) frames);
            monitorR.resize ((size_t) frames);
            for (int i = 0; i < frames; ++i)
            {
                monitorL[(size_t) i] = (float) l[i];
                monitorR[(size_t) i] = (float) r[i];
            }
            monitor (monitorL.data(), monitorR.data(), frames);
        }
    }

    void run() override
    {
        const auto info = source->getInfo();
        const int srcRate = info.sampleRate;

        if (startOffsetMs > 0)
            source->seekToMs (startOffsetMs);
        const int dstRate = outputRate.load();
        const bool needsResample = dstRate > 0 && srcRate > 0 && srcRate != dstRate;

        std::vector<double> interleaved ((size_t) kBlockFrames * 2, 0.0);
        juce::AudioBuffer<double> renderBuffer (2, kBlockFrames);
        renderBuffer.clear();
        juce::AudioBuffer<double> resampledBuffer;
        if (needsResample)
        {
            const int maxOut = Resampler::maxOutputFrames ((double) srcRate, (double) dstRate, kBlockFrames);
            resampledBuffer.setSize (2, juce::jmax (kBlockFrames, maxOut), false, true, false);
            resampledBuffer.clear();
        }

        std::int64_t totalSent = 0;
        bool writeFailed = false;

        for (;;)
        {
            if (threadShouldExit())
                break;

            const int got = source->readFrames (interleaved.data(), kBlockFrames);
            if (got <= 0)
                break;

            auto* inL = renderBuffer.getWritePointer (0);
            auto* inR = renderBuffer.getWritePointer (1);
            for (int i = 0; i < got; ++i)
            {
                inL[i] = interleaved[(size_t) i * 2];
                inR[i] = interleaved[(size_t) i * 2 + 1];
            }

            if (needsResample && resampler != nullptr)
            {
                const double* inCh[2] = { inL, inR };
                double* outCh[2] = { resampledBuffer.getWritePointer (0), resampledBuffer.getWritePointer (1) };
                const int produced = resampler->process (inCh, got, outCh, resampledBuffer.getNumSamples());
                if (produced > 0)
                {
                    processAndWrite (resampledBuffer, produced);
                    if (! writePcm (resampledBuffer.getReadPointer (0),
                                    resampledBuffer.getReadPointer (1), produced))
                    {
                        writeFailed = true;
                        break;
                    }
                    totalSent += produced;
                }
            }
            else
            {
                processAndWrite (renderBuffer, got);
                if (! writePcm (renderBuffer.getReadPointer (0), renderBuffer.getReadPointer (1), got))
                {
                    writeFailed = true;
                    break;
                }
                totalSent += got;
            }

        }

        // Flush the resampler tail (best effort) before closing the pipe.
        if (! writeFailed && needsResample && resampler != nullptr && ! threadShouldExit())
        {
            juce::AudioBuffer<double> flushIn (2, 1);
            flushIn.clear();
            const double* inCh[2] = { flushIn.getReadPointer (0), flushIn.getReadPointer (1) };
            double* outCh[2] = { resampledBuffer.getWritePointer (0), resampledBuffer.getWritePointer (1) };
            const int produced = resampler->process (inCh, 0, outCh, resampledBuffer.getNumSamples(), true);
            if (produced > 0)
            {
                processAndWrite (resampledBuffer, produced);
                writePcm (resampledBuffer.getReadPointer (0),
                          resampledBuffer.getReadPointer (1), produced);
            }
        }

        if (writeFd >= 0)
        {
            ::close (writeFd);
            writeFd = -1;
        }

        int exitStatus = -1;
        if (childPid > 0)
        {
            const auto deadline = juce::Time::getMillisecondCounter() + 8000;
            for (;;)
            {
                const pid_t r = ::waitpid (childPid, &exitStatus, WNOHANG);
                if (r == childPid)
                    break;
                if (r < 0)
                {
                    exitStatus = -1;
                    break;
                }
                drainStderr();
                if (juce::Time::getMillisecondCounter() > deadline || threadShouldExit())
                {
                    ::kill (childPid, SIGTERM);
                    ::waitpid (childPid, &exitStatus, 0);
                    break;
                }
                juce::Thread::sleep (30);
            }
            childPid = 0;
        }
        drainStderr();
        if (errFd >= 0)
        {
            ::close (errFd);
            errFd = -1;
        }

        const bool stoppedByUser = threadShouldExit();
        const bool ok = ! writeFailed && ! stoppedByUser
                        && (WIFEXITED (exitStatus) ? WEXITSTATUS (exitStatus) == 0 : true);

        {
            const juce::ScopedLock sl (stateLock);
            if (stoppedByUser)
            {
                state = PiBridgeOutput::State::idle;
                status = "Pi K11 output stopped.";
            }
            else if (writeFailed || ! ok)
            {
                state = PiBridgeOutput::State::failed;
                lastError = "Pi K11 stream failed";
                if (errText.trim().isNotEmpty())
                    lastError << ": " << errText.trim().substring (juce::jmax (0, errText.trim().length() - 700));
                else
                    lastError << " (receiver exited with status " << exitStatus << ")";
                if (lastError.containsIgnoreCase ("busy"))
                    lastError << " The K11 is single-tenant: quit HQPlayer/NAA (or any other player on the Pi) "
                                 "so ~/hqdac can take the DAC, then press PLAY ON PI K11 again.";
                status = lastError;
            }
            else
            {
                state = PiBridgeOutput::State::finished;
                status = "Pi K11 stream finished (" + juce::String ((int) (totalSent / juce::jmax (1, dstRate))) + " s sent).";
                if (errText.trim().isNotEmpty())
                    status << " Receiver: " << errText.trim().substring (juce::jmax (0, errText.trim().length() - 300));
            }
        }
    }

    void beginStream() { startThread (juce::Thread::Priority::normal); }
    bool isRunningThread() const { return isThreadRunning(); }

    void stop()
    {
        if (isThreadRunning())
        {
            signalThreadShouldExit();
            if (childPid > 0)
                ::kill (childPid, SIGTERM);
            stopThread (4000);
            if (childPid > 0)
            {
                ::kill (childPid, SIGKILL);
                int st = 0;
                ::waitpid (childPid, &st, 0);
                childPid = 0;
            }
            if (writeFd >= 0)
            {
                ::close (writeFd);
                writeFd = -1;
            }
            if (errFd >= 0)
            {
                ::close (errFd);
                errFd = -1;
            }
        }
    }

    void setStatus (PiBridgeOutput::State s, const juce::String& text)
    {
        const juce::ScopedLock sl (stateLock);
        state = s;
        status = text;
    }
};

//==============================================================================
PiBridgeOutput::PiBridgeOutput() : impl (std::make_unique<Impl>()) {}

PiBridgeOutput::~PiBridgeOutput()
{
    stop();
}

juce::String PiBridgeOutput::buildCommand (const PiBridgeConfig& config, int sampleRate)
{
    const juce::String remote =
        config.receiver + " -D " + config.alsaDevice + " -t raw -f S32_LE -r "
        + juce::String (sampleRate) + " -c 2 --buffer-time=" + juce::String (config.bufferUs)
        + " --period-time=" + juce::String (config.periodUs)
        + " --target-time=" + juce::String (config.targetUs) + " -";

    if (config.commandOverride.isNotEmpty())
        return config.commandOverride;

    return "ssh -o BatchMode=yes -o ConnectTimeout=5 -o ServerAliveInterval=30 "
           + shellQuote (config.sshTarget) + " " + shellQuote (remote);
}

int PiBridgeOutput::chooseReceiverRate (int sourceSampleRate)
{
    if (sourceSampleRate <= 0)
        return 384000;

    auto exact = [] (double target, int source)
    {
        const double k = target / (double) source;
        return std::fabs (k - std::round (k)) < 1e-9;
    };

    if (exact (384000.0, sourceSampleRate))
        return 384000;
    if (exact (352800.0, sourceSampleRate))
        return 352800;
    return (sourceSampleRate % 48000) == 0 ? 384000 : 352800;
}

bool PiBridgeOutput::start (const juce::File& file, const DspParams& params, float volumeDb,
                            bool muted, const PiBridgeConfig& config, juce::String& errorOut,
                            std::int64_t startMs)
{
    stop();
    errorOut.clear();

    if (! file.existsAsFile())
    {
        errorOut = "File not found: " + file.getFullPathName();
        return false;
    }

    {
        const juce::ScopedLock sl (impl->stateLock);
        impl->state = PiBridgeOutput::State::preparing;
        impl->status = "Preparing Pi K11 stream...";
        impl->lastError.clear();
        impl->errText.clear();
    }

    impl->config = config;
    impl->dspParams = params;
    impl->targetGain.store (muted ? 0.0 : juce::Decibels::decibelsToGain (juce::jlimit (-144.0f, 24.0f, volumeDb)));
    impl->appliedGain = impl->targetGain.load();
    impl->startOffsetMs = juce::jmax ((std::int64_t) 0, startMs);

    juce::String decodeError;
    impl->source = DecodeSource::create (file, decodeError);
    if (impl->source == nullptr)
    {
        impl->setStatus (PiBridgeOutput::State::failed, "Could not decode " + file.getFileName() + ": " + decodeError);
        errorOut = impl->status;
        return false;
    }

    const auto info = impl->source->getInfo();
    const int srcRate = info.sampleRate;
    if (srcRate <= 0)
    {
        impl->setStatus (PiBridgeOutput::State::failed, "Unsupported source sample rate.");
        errorOut = impl->status;
        return false;
    }

    const int dstRate = chooseReceiverRate (srcRate);
    impl->outputRate.store (dstRate);
    impl->framesTotal.store ((std::int64_t) std::llround ((double) info.totalFrames * dstRate / srcRate));

    int maxChainBlock = kBlockFrames;
    if (dstRate != srcRate)
        maxChainBlock = juce::jmax (kBlockFrames,
                                    Resampler::maxOutputFrames ((double) srcRate, (double) dstRate,
                                                                kBlockFrames));

    impl->chain = std::make_unique<DspChain>();
    impl->chain->prepare ((double) dstRate, maxChainBlock, 2);
    impl->chain->setParamsImmediate (params);

    if (dstRate != srcRate)
    {
        impl->resampler = std::make_unique<Resampler>();
        impl->resampler->prepare ((double) srcRate, (double) dstRate, 2, Resampler::Quality::ultra);
    }

    PiBridgeConfig effective = config;
    effective.targetUs = juce::jmin (effective.targetUs, juce::jmax (4000, effective.bufferUs / 4));

    const auto command = buildCommand (effective, dstRate);
    juce::String spawnError;
    if (! impl->spawnChild (command, spawnError))
    {
        impl->setStatus (PiBridgeOutput::State::failed, spawnError);
        errorOut = spawnError;
        return false;
    }

    impl->framesSent.store (0);
    {
        const juce::ScopedLock sl (impl->stateLock);
        impl->state = PiBridgeOutput::State::streaming;
        impl->status = "Streaming " + file.getFileName() + " to Pi K11 via ssh "
                       + (config.commandOverride.isNotEmpty() ? juce::String ("(custom receiver)")
                                                              : config.sshTarget)
                       + " at " + juce::String (dstRate) + " Hz / S32_LE"
                       + (dstRate != srcRate ? " (upsampled from " + juce::String (srcRate) + " Hz)."
                                             : juce::String (". "));
    }
    impl->beginStream();
    return true;
}

void PiBridgeOutput::setVolume (float volumeDb, bool muted)
{
    impl->targetGain.store (muted ? 0.0 : juce::Decibels::decibelsToGain (juce::jlimit (-144.0f, 24.0f, volumeDb)));
}

void PiBridgeOutput::setDspParams (const DspParams& params, int rampMs)
{
    if (impl->chain != nullptr)
        impl->chain->setParams (params, rampMs);
}

void PiBridgeOutput::setMonitorCallback (MonitorCallback callback)
{
    impl->monitor = std::move (callback);
}

void PiBridgeOutput::stop()
{
    impl->stop();
    impl->setStatus (PiBridgeOutput::State::idle, "Pi K11 output stopped.");
}

bool PiBridgeOutput::isRunning() const { return impl->isRunningThread(); }

PiBridgeOutput::State PiBridgeOutput::getState() const
{
    const juce::ScopedLock sl (impl->stateLock);
    return impl->state;
}

juce::String PiBridgeOutput::getStatusText() const
{
    const juce::ScopedLock sl (impl->stateLock);
    return impl->status;
}

juce::String PiBridgeOutput::getLastError() const
{
    const juce::ScopedLock sl (impl->stateLock);
    return impl->lastError;
}

int PiBridgeOutput::getOutputSampleRate() const { return impl->outputRate.load(); }
std::int64_t PiBridgeOutput::getFramesSent() const { return impl->framesSent.load(); }
std::int64_t PiBridgeOutput::getFramesTotal() const { return impl->framesTotal.load(); }

std::int64_t PiBridgeOutput::getPositionMs() const
{
    const int rate = impl->outputRate.load();
    const auto sent = impl->framesSent.load();
    if (rate <= 0)
        return impl->startOffsetMs;
    return impl->startOffsetMs + (sent * 1000) / rate;
}

} // namespace sp
