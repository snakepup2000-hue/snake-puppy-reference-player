/*
    Snake Puppy Reference Player
    High quality streaming sample-rate converter (windowed-sinc polyphase).
    Runs on the decode thread, never on the audio callback.
*/
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace sp
{

class Resampler
{
public:
    enum class Quality { fast = 0, high = 1, ultra = 2 };

    Resampler();
    ~Resampler();

    void prepare (double sourceRate, double targetRate, int numChannels, Quality quality = Quality::high);
    void reset();

    /** Streams `inFrames` frames from `inChannels` (non-interleaved, numChannels
        pointers), writing at most `maxOutFrames` to `outChannels`, returns the
        number of frames produced. Set flush=true at end-of-stream so the
        filter tail is emitted. May return 0 when more input is needed. */
    int process (const double* const* inChannels, int inFrames,
                 double* const* outChannels, int maxOutFrames, bool flush = false);

    double sourceRate() const;
    double targetRate() const;
    int numChannels() const;
    int latencySamples() const;
    double ratio() const;

    /** Upper bound of output frames for a given input block. */
    static int maxOutputFrames (double sourceRate, double targetRate, int inFrames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Resampler)
};

} // namespace sp
