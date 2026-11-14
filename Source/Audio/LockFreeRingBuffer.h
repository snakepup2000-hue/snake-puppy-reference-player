/*
    Snake Puppy Reference Player
    Lock-free single-producer / single-consumer ring buffer for stereo
    interleaved 64-bit PCM frames.

    Threading contract
      - The producer is the audio engine's decode thread.
      - The consumer is the Core Audio real-time callback.
      - write(), getFreeFrames(), getBufferedFrames(), requestFlush(),
        waitForFlushAck() may only be called from the producer thread.
      - read() and serviceFlushRequest() may only be called from the consumer
        (audio) thread.
      - consumerHardReset()/prepare() are non-real-time only and must not run
        concurrently with the consumer. The engine guards them with its
        device-start lock.

    Flush protocol (used by seek/load/reset while the device is running):
      1. producer: requestFlush() -> the buffer stops accepting writes.
      2. consumer: on its next read/service call it sets readPos = writePos
         (discarding everything) and sets flushAcked.
      3. producer: waitForFlushAck() sees the ack, clears the request and may
         write new audio.
    If the device is not running there is no consumer; the engine then calls
    consumerHardReset() under its non-real-time device lock instead.

    No allocation, no locks and no syscalls happen in write()/read().
*/
#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sp
{

class LockFreeStereoRingBuffer
{
public:
    LockFreeStereoRingBuffer() = default;

    /** Non-real-time: allocate storage for at least capacityFrames stereo
        frames. Capacity is rounded up to a power of two. */
    void prepare (std::int64_t capacityFrames)
    {
        std::int64_t cap = 1;
        while (cap < capacityFrames)
            cap <<= 1;

        capacity = cap;
        mask = cap - 1;
        storage.assign ((size_t) (cap * kChannels), 0.0);
        resetPositions();
    }

    /** Non-real-time, consumer side: discard everything and restart counters.
        Only safe when the audio callback is not running. */
    void consumerHardReset() noexcept
    {
        resetPositions();
        flushRequested.store (false, std::memory_order_relaxed);
        flushAcked.store (false, std::memory_order_relaxed);
    }

    int getCapacityFrames() const noexcept { return (int) capacity; }
    int getNumChannels() const noexcept { return kChannels; }

    //== Producer side ========================================================
    /** Number of frames the producer can write right now. */
    int getFreeFrames() const noexcept
    {
        if (flushRequested.load (std::memory_order_acquire))
            return 0;

        const auto wr = writePos.load (std::memory_order_relaxed);
        const auto rd = readPos.load (std::memory_order_acquire);
        const auto used = wr - rd;
        return (int) juce::jlimit<std::int64_t> (0, capacity - used, capacity);
    }

    /** Frames currently waiting for the consumer. Safe to call from either
        thread (used for status/metring). */
    int getBufferedFrames() const noexcept
    {
        const auto wr = writePos.load (std::memory_order_acquire);
        const auto rd = readPos.load (std::memory_order_acquire);
        const auto used = wr - rd;
        return (int) (used < 0 ? 0 : (used > capacity ? capacity : used));
    }

    /** Producer: ask the consumer to discard the buffer contents. Returns false
        if a flush is already in progress. */
    bool requestFlush() noexcept
    {
        if (flushRequested.load (std::memory_order_acquire))
            return false;
        flushAcked.store (false, std::memory_order_relaxed);
        flushRequested.store (true, std::memory_order_release);
        return true;
    }

    bool isFlushPending() const noexcept
    {
        return flushRequested.load (std::memory_order_acquire);
    }

    /** Producer: wait until the consumer acknowledged the pending flush.
        Returns false on timeout. Only blocks the decode thread. */
    bool waitForFlushAck (int timeoutMs) noexcept
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax (0, timeoutMs);

        while (juce::Time::getMillisecondCounter() < deadline)
        {
            if (flushAcked.load (std::memory_order_acquire))
            {
                flushRequested.store (false, std::memory_order_release);
                return true;
            }
            juce::Thread::sleep (1);
        }

        return flushAcked.load (std::memory_order_acquire);
    }

    /** Producer: abandon a flush request (e.g. device went away). */
    void cancelFlush() noexcept
    {
        flushRequested.store (false, std::memory_order_release);
        flushAcked.store (false, std::memory_order_relaxed);
    }

    /** Producer: append interleaved stereo frames. Returns frames written. */
    int write (const double* interleavedStereo, int frames) noexcept
    {
        if (frames <= 0 || interleavedStereo == nullptr)
            return 0;
        if (flushRequested.load (std::memory_order_acquire))
            return 0;

        const auto wr = writePos.load (std::memory_order_relaxed);
        const auto rd = readPos.load (std::memory_order_acquire);
        const auto room = capacity - (wr - rd);

        const int n = (int) juce::jmin<std::int64_t> ((std::int64_t) frames, juce::jmax<std::int64_t> (0, room));
        if (n <= 0)
            return 0;

        copyIn (wr, interleavedStereo, n);
        writePos.store (wr + n, std::memory_order_release);
        return n;
    }

    std::int64_t framesWritten() const noexcept { return writePos.load (std::memory_order_relaxed); }
    std::int64_t framesRead() const noexcept { return readPos.load (std::memory_order_relaxed); }

    //== Consumer side ========================================================
    /** Consumer: honours a pending flush request. Returns true if it flushed. */
    bool serviceFlushRequest() noexcept
    {
        if (! flushRequested.load (std::memory_order_acquire))
            return false;

        readPos.store (writePos.load (std::memory_order_acquire), std::memory_order_release);
        flushAcked.store (true, std::memory_order_release);
        return true;
    }

    /** Consumer: read up to `frames` stereo frames. Returns frames read and
        zero-fills nothing; the caller handles missing data. */
    int read (double* interleavedStereo, int frames) noexcept
    {
        if (frames <= 0 || interleavedStereo == nullptr)
            return 0;
        if (flushRequested.load (std::memory_order_acquire))
            return 0;

        const auto rd = readPos.load (std::memory_order_relaxed);
        const auto wr = writePos.load (std::memory_order_acquire);
        const auto avail = wr - rd;

        const int n = (int) juce::jmin<std::int64_t> ((std::int64_t) frames, juce::jmax<std::int64_t> (0, avail));
        if (n <= 0)
            return 0;

        copyOut (rd, interleavedStereo, n);
        readPos.store (rd + n, std::memory_order_release);
        return n;
    }

private:
    static constexpr int kChannels = 2;

    void resetPositions() noexcept
    {
        writePos.store (0, std::memory_order_relaxed);
        readPos.store (0, std::memory_order_relaxed);
    }

    void copyIn (std::int64_t startFrame, const double* src, int frames) noexcept
    {
        std::int64_t frame = startFrame;
        int done = 0;

        while (done < frames)
        {
            const auto idx = (int) (frame & mask);
            const int contiguous = juce::jmin (frames - done, (int) (capacity - idx));
            std::memcpy (storage.data() + (size_t) idx * kChannels,
                         src + (size_t) done * kChannels,
                         sizeof (double) * (size_t) contiguous * kChannels);
            done += contiguous;
            frame += contiguous;
        }
    }

    void copyOut (std::int64_t startFrame, double* dst, int frames) noexcept
    {
        std::int64_t frame = startFrame;
        int done = 0;

        while (done < frames)
        {
            const auto idx = (int) (frame & mask);
            const int contiguous = juce::jmin (frames - done, (int) (capacity - idx));
            std::memcpy (dst + (size_t) done * kChannels,
                         storage.data() + (size_t) idx * kChannels,
                         sizeof (double) * (size_t) contiguous * kChannels);
            done += contiguous;
            frame += contiguous;
        }
    }

    std::vector<double> storage;
    std::int64_t capacity = 0;
    std::int64_t mask = 0;
    std::atomic<std::int64_t> writePos { 0 };
    std::atomic<std::int64_t> readPos { 0 };
    std::atomic<bool> flushRequested { false };
    std::atomic<bool> flushAcked { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LockFreeStereoRingBuffer)
};

} // namespace sp
