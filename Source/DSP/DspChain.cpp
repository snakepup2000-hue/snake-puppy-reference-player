/*
    Snake Puppy Reference Player
    The 64-bit real-time DSP chain.

    Design notes
    ------------
    * The audio thread never touches juce::String, the heap, locks or files.
      Parameters are published from the control thread as a plain-data snapshot
      and picked up by the audio thread with a non-blocking try-lock (if the
      control thread happens to be writing, the previous snapshot is used for
      one more block).
    * Every processor implements its own smoothing/fading; the chain owns the
      global (Reference Mode) bypass crossfade, ReplayGain, preamp and the
      automatic-headroom estimate.
    * Convolution uses juce::dsp::Convolution. Preparing and loading happen on
      the message thread; JUCE's convolution process() path is used unchanged
      and it is documented in the final report that this section is float (JUCE
      limitation) while the rest of the chain is double precision.
*/
#include "DSP/DspChain.h"

#include "DSP/Biquad.h"
#include "DSP/Dynamics.h"
#include "DSP/Equalizers.h"
#include "DSP/StereoProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace sp
{

namespace
{
constexpr int kMaxProcessChannels = 8;
constexpr int kInternalBlock = 128;
constexpr int kNumSlots = (int) DspSlot::numSlots;

//==============================================================================
// Plain-data real-time parameter snapshot (no juce::String, no heap).
struct RtBand
{
    bool enabled = false;
    bool bypassed = false;
    int type = 0;
    double freqHz = 1000.0;
    double gainDb = 0.0;
    double q = 0.707;
};

struct RtGraphic
{
    bool enabled = true;
    int bandSet = 0;
    float gains15[GraphicEqParams::kNumBands15] {};
    float gains31[GraphicEqParams::kNumBands31] {};
};

struct RtParametric
{
    bool enabled = true;
    RtBand bands[ParametricEqParams::kNumBands];
};

struct RtBass
{
    bool enabled = true;
    float gainDb = 0.0f;
    double shelfFreqHz = 80.0;
    double shelfQ = 0.707;
    double shelfS = 1.0;
    bool subEnabled = false;
    float subGainDb = 0.0f;
    double subFreqHz = 35.0;
    double subQ = 0.9;
    bool midBassEnabled = false;
    float midBassGainDb = 0.0f;
    double midBassFreqHz = 100.0;
    double midBassQ = 1.0;
    bool punchEnabled = false;
    float punchGainDb = 0.0f;
    double punchFreqHz = 140.0;
    double punchQ = 0.9;
};

struct RtFilters
{
    bool highPassEnabled = false;
    double highPassHz = 20.0;
    int highPassSlope = 12;
    bool lowPassEnabled = false;
    double lowPassHz = 20000.0;
    int lowPassSlope = 12;
};

struct RtStereo
{
    float balance = 0.0f;
    float widthPercent = 100.0f;
    bool monoBassEnabled = false;
    double monoBassHz = 120.0;
    bool protectMono = true;
    bool crossfeedEnabled = false;
    float crossfeedAmountDb = 6.0f;
    double crossfeedCutoffHz = 700.0;
    float crossfeedDelayUs = 250.0f;
};

struct RtConvolution
{
    bool enabled = false;
    float mixPercent = 100.0f;
};

struct RtReplayGain
{
    bool enabled = true;
    bool albumMode = false;
    bool preventClipping = true;
    float preampDb = 0.0f;
    float fallbackGainDb = 0.0f;
};

struct RtPreamp
{
    bool autoHeadroom = true;
    float manualPreampDb = 0.0f;
    bool limiterEnabled = true;
    float ceilingDb = -0.3f;
    float releaseMs = 150.0f;
};

struct RtParams
{
    bool bypassed = false;
    bool eqBypass = false;

    RtGraphic graphic;
    RtParametric parametric;
    RtBass bass;
    RtFilters filters;
    RtStereo stereo;
    RtConvolution convolution;
    RtReplayGain replayGain;
    RtPreamp preamp;

    int chainOrder[kNumSlots] {};
    float autoHeadroomDb = 0.0f;
    int rampSamples = 0;
};

//==============================================================================
/** Combined EQ + bass + HP/LP response (dB) of a DspParams set at one frequency.
    This is the reference used both by getMagnitudeResponseDb() and by the
    automatic headroom estimate, so the displayed curve and the headroom always
    agree with what the filter designs actually do. */
double combinedResponseDb (const DspParams& p, double sampleRate, double freqHz)
{
    if (p.bypassed || ! (sampleRate > 0.0))
        return 0.0;

    double db = 0.0;

    if (! p.eqBypass)
    {
        if (p.graphic.enabled)
        {
            const bool use31 = p.graphic.bandSet == GraphicEqParams::BandSet::bands31;
            const double* centres = use31 ? GraphicEqParams::centres31().data()
                                           : GraphicEqParams::centres15().data();
            const int count = use31 ? GraphicEqParams::kNumBands31 : GraphicEqParams::kNumBands15;
            const double q = use31 ? 4.32 : 2.145;

            for (int i = 0; i < count; ++i)
            {
                const double g = std::clamp ((double) (use31 ? p.graphic.gains31[(size_t) i]
                                                             : p.graphic.gains15[(size_t) i]), -12.0, 12.0);
                db += BiquadCoeffs::bell (sampleRate, centres[i], g, q).magnitudeDbAt (freqHz, sampleRate);
            }
        }

        if (p.parametric.enabled)
        {
            for (const auto& band : p.parametric.bands)
            {
                if (! band.isActive())
                    continue;
                db += BiquadCoeffs::design ((int) band.type, sampleRate, band.freqHz, band.gainDb, band.q)
                          .magnitudeDbAt (freqHz, sampleRate);
            }
        }

        if (p.bass.enabled)
        {
            double s = p.bass.shelfS;
            if (std::abs (p.bass.shelfS - 1.0) < 1.0e-6 && std::abs (p.bass.shelfQ - 0.7071067811865476) > 1.0e-4)
                s = BiquadCoeffs::shelfSFromQ (p.bass.shelfQ, p.bass.gainDb);
            s = std::clamp (s, 0.1, 4.0);

            db += BiquadCoeffs::lowShelf (sampleRate, p.bass.shelfFreqHz,
                                          std::clamp ((double) p.bass.gainDb, -24.0, 24.0), s)
                      .magnitudeDbAt (freqHz, sampleRate);

            if (p.bass.subEnabled)
                db += BiquadCoeffs::bell (sampleRate, p.bass.subFreqHz,
                                          std::clamp ((double) p.bass.subGainDb, -24.0, 24.0), p.bass.subQ)
                          .magnitudeDbAt (freqHz, sampleRate);
            if (p.bass.midBassEnabled)
                db += BiquadCoeffs::bell (sampleRate, p.bass.midBassFreqHz,
                                          std::clamp ((double) p.bass.midBassGainDb, -24.0, 24.0), p.bass.midBassQ)
                          .magnitudeDbAt (freqHz, sampleRate);
            if (p.bass.punchEnabled)
                db += BiquadCoeffs::bell (sampleRate, p.bass.punchFreqHz,
                                          std::clamp ((double) p.bass.punchGainDb, -24.0, 24.0), p.bass.punchQ)
                          .magnitudeDbAt (freqHz, sampleRate);
        }
    }

    auto addFilterBank = [&] (bool highPass, bool enabled, double cutoff, int slope)
    {
        if (! enabled)
            return;
        std::array<double, 4> qs {};
        const int stages = Butterworth::designQs (slope, qs);
        const double f = BiquadCoeffs::clampFrequency (cutoff, sampleRate);
        for (int s = 0; s < stages; ++s)
        {
            const auto c = highPass ? BiquadCoeffs::highPass (sampleRate, f, qs[(size_t) s])
                                    : BiquadCoeffs::lowPass (sampleRate, f, qs[(size_t) s]);
            db += c.magnitudeDbAt (freqHz, sampleRate);
        }
    };

    addFilterBank (true, p.filters.highPassEnabled, p.filters.highPassHz, p.filters.highPassSlopeDbPerOct);
    addFilterBank (false, p.filters.lowPassEnabled, p.filters.lowPassHz, p.filters.lowPassSlopeDbPerOct);

    return db;
}

/** Peak positive gain of the combined EQ/bass/filter response, sampled on a
    dense log grid plus DC. Used for automatic headroom. */
double computeMaxPositiveBoostDb (const DspParams& p, double sampleRate)
{
    if (p.bypassed || ! (sampleRate > 0.0))
        return 0.0;

    double maxDb = combinedResponseDb (p, sampleRate, 10.0);

    const double top = std::min (sampleRate * 0.49, 24000.0);
    constexpr int numPoints = 512;
    for (int i = 0; i < numPoints; ++i)
    {
        const double t = (double) i / (double) (numPoints - 1);
        const double f = 10.0 * std::pow (top / 10.0, t);
        maxDb = std::max (maxDb, combinedResponseDb (p, sampleRate, f));
    }

    return std::max (0.0, maxDb);
}

//==============================================================================
RtParams buildRtParams (const DspParams& p, float autoHeadroomDb, int rampSamples)
{
    RtParams r;

    r.bypassed = p.bypassed;
    r.eqBypass = p.eqBypass;
    r.autoHeadroomDb = autoHeadroomDb;
    r.rampSamples = rampSamples;

    r.graphic.enabled = p.graphic.enabled;
    r.graphic.bandSet = (int) p.graphic.bandSet;
    for (int i = 0; i < GraphicEqParams::kNumBands15; ++i)
        r.graphic.gains15[(size_t) i] = p.graphic.gains15[(size_t) i];
    for (int i = 0; i < GraphicEqParams::kNumBands31; ++i)
        r.graphic.gains31[(size_t) i] = p.graphic.gains31[(size_t) i];

    r.parametric.enabled = p.parametric.enabled;
    for (int i = 0; i < ParametricEqParams::kNumBands; ++i)
    {
        const auto& b = p.parametric.bands[(size_t) i];
        auto& rb = r.parametric.bands[(size_t) i];
        rb.enabled = b.enabled;
        rb.bypassed = b.bypassed;
        rb.type = (int) b.type;
        rb.freqHz = b.freqHz;
        rb.gainDb = b.gainDb;
        rb.q = b.q;
    }

    r.bass.enabled = p.bass.enabled;
    r.bass.gainDb = p.bass.gainDb;
    r.bass.shelfFreqHz = p.bass.shelfFreqHz;
    r.bass.shelfQ = p.bass.shelfQ;
    r.bass.shelfS = p.bass.shelfS;
    r.bass.subEnabled = p.bass.subEnabled;
    r.bass.subGainDb = p.bass.subGainDb;
    r.bass.subFreqHz = p.bass.subFreqHz;
    r.bass.subQ = p.bass.subQ;
    r.bass.midBassEnabled = p.bass.midBassEnabled;
    r.bass.midBassGainDb = p.bass.midBassGainDb;
    r.bass.midBassFreqHz = p.bass.midBassFreqHz;
    r.bass.midBassQ = p.bass.midBassQ;
    r.bass.punchEnabled = p.bass.punchEnabled;
    r.bass.punchGainDb = p.bass.punchGainDb;
    r.bass.punchFreqHz = p.bass.punchFreqHz;
    r.bass.punchQ = p.bass.punchQ;

    r.filters.highPassEnabled = p.filters.highPassEnabled;
    r.filters.highPassHz = p.filters.highPassHz;
    r.filters.highPassSlope = p.filters.highPassSlopeDbPerOct;
    r.filters.lowPassEnabled = p.filters.lowPassEnabled;
    r.filters.lowPassHz = p.filters.lowPassHz;
    r.filters.lowPassSlope = p.filters.lowPassSlopeDbPerOct;

    r.stereo.balance = p.stereo.balance;
    r.stereo.widthPercent = p.stereo.widthPercent;
    r.stereo.monoBassEnabled = p.stereo.monoBassEnabled;
    r.stereo.monoBassHz = p.stereo.monoBassHz;
    r.stereo.protectMono = p.stereo.protectMono;
    r.stereo.crossfeedEnabled = p.stereo.crossfeedEnabled;
    r.stereo.crossfeedAmountDb = p.stereo.crossfeedAmountDb;
    r.stereo.crossfeedCutoffHz = p.stereo.crossfeedCutoffHz;
    r.stereo.crossfeedDelayUs = p.stereo.crossfeedDelayUs;

    r.convolution.enabled = p.convolution.enabled;
    r.convolution.mixPercent = p.convolution.mixPercent;

    r.replayGain.enabled = p.replayGain.enabled;
    r.replayGain.albumMode = p.replayGain.albumMode;
    r.replayGain.preventClipping = p.replayGain.preventClipping;
    r.replayGain.preampDb = p.replayGain.preampDb;
    r.replayGain.fallbackGainDb = p.replayGain.fallbackGainDb;

    r.preamp.autoHeadroom = p.preamp.autoHeadroom;
    r.preamp.manualPreampDb = p.preamp.manualPreampDb;
    r.preamp.limiterEnabled = p.preamp.limiterEnabled;
    r.preamp.ceilingDb = p.preamp.outputCeilingDb;
    r.preamp.releaseMs = p.preamp.limiterReleaseMs;

    for (int i = 0; i < kNumSlots; ++i)
        r.chainOrder[i] = (int) p.chainOrder[(size_t) i];

    return r;
}

GraphicEqParams toGraphicParams (const RtGraphic& g)
{
    GraphicEqParams p;
    p.enabled = g.enabled;
    p.bandSet = (GraphicEqParams::BandSet) g.bandSet;
    for (int i = 0; i < GraphicEqParams::kNumBands15; ++i)
        p.gains15[(size_t) i] = g.gains15[(size_t) i];
    for (int i = 0; i < GraphicEqParams::kNumBands31; ++i)
        p.gains31[(size_t) i] = g.gains31[(size_t) i];
    return p;
}

ParametricEqParams toParametricParams (const RtParametric& in)
{
    ParametricEqParams p;
    p.enabled = in.enabled;
    for (int i = 0; i < ParametricEqParams::kNumBands; ++i)
    {
        EqBand b;
        b.enabled = in.bands[(size_t) i].enabled;
        b.bypassed = in.bands[(size_t) i].bypassed;
        b.type = (BandType) in.bands[(size_t) i].type;
        b.freqHz = in.bands[(size_t) i].freqHz;
        b.gainDb = in.bands[(size_t) i].gainDb;
        b.q = in.bands[(size_t) i].q;
        p.bands[(size_t) i] = b;
    }
    return p;
}

BassParams toBassParams (const RtBass& in)
{
    BassParams p;
    p.enabled = in.enabled;
    p.gainDb = in.gainDb;
    p.shelfFreqHz = in.shelfFreqHz;
    p.shelfQ = in.shelfQ;
    p.shelfS = in.shelfS;
    p.subEnabled = in.subEnabled;
    p.subGainDb = in.subGainDb;
    p.subFreqHz = in.subFreqHz;
    p.subQ = in.subQ;
    p.midBassEnabled = in.midBassEnabled;
    p.midBassGainDb = in.midBassGainDb;
    p.midBassFreqHz = in.midBassFreqHz;
    p.midBassQ = in.midBassQ;
    p.punchEnabled = in.punchEnabled;
    p.punchGainDb = in.punchGainDb;
    p.punchFreqHz = in.punchFreqHz;
    p.punchQ = in.punchQ;
    return p;
}

FilterParams toFilterParams (const RtFilters& in)
{
    FilterParams p;
    p.highPassEnabled = in.highPassEnabled;
    p.highPassHz = in.highPassHz;
    p.highPassSlopeDbPerOct = in.highPassSlope;
    p.lowPassEnabled = in.lowPassEnabled;
    p.lowPassHz = in.lowPassHz;
    p.lowPassSlopeDbPerOct = in.lowPassSlope;
    return p;
}

StereoParams toStereoParams (const RtStereo& in)
{
    StereoParams p;
    p.balance = in.balance;
    p.widthPercent = in.widthPercent;
    p.monoBassEnabled = in.monoBassEnabled;
    p.monoBassHz = in.monoBassHz;
    p.protectMono = in.protectMono;
    p.crossfeedEnabled = in.crossfeedEnabled;
    p.crossfeedAmountDb = in.crossfeedAmountDb;
    p.crossfeedCutoffHz = in.crossfeedCutoffHz;
    p.crossfeedDelayUs = in.crossfeedDelayUs;
    return p;
}

} // namespace

//==============================================================================
struct DspChain::Impl
{
    //== Configuration =======================================================
    double sampleRate = 48000.0;
    int maxBlock = 512;
    int numChannels = 2;
    bool prepared = false;

    //== Control-thread state ================================================
    mutable juce::SpinLock controlLock;
    DspParams controlParams;

    //== Real-time parameter snapshot handoff ================================
    juce::SpinLock rtLock;
    RtParams rtPending;
    mutable RtParams rtLocal;
    std::atomic<bool> rtDirty { false };
    bool rtLocalValid = false;

    //== Processors ==========================================================
    GraphicEqProcessor graphic;
    ParametricEqProcessor parametric;
    BassProcessor bass;
    FilterProcessor filters;
    StereoProcessor stereo;
    Limiter limiter;
    juce::dsp::Convolution convolution;
    bool convolutionPrepared = false;

    //== Top-level gain ramps ================================================
    SampleRamp replayGainRamp { 1.0 };
    SampleRamp preampRamp { 1.0 };
    SampleRamp globalBypassMix { 1.0 };
    SampleRamp convolutionMix { 0.0 };

    bool lastBypassed = false;
    bool lastLimiterEnabled = false;
    bool lastConvolutionEnabled = false;

    //== ReplayGain track state (published from the decode thread) ===========
    std::atomic<float> trackGainDb { 0.0f };
    std::atomic<float> trackPeakLinear { 0.0f };
    std::atomic<bool> trackHasGain { false };
    std::atomic<int> trackRgGeneration { 0 };
    int seenRgGeneration = -1;
    double currentReplayGainDb = 0.0;

    //== Convolution state ===================================================
    std::atomic<bool> convolutionLoaded { false };
    juce::String convolutionError;
    juce::String convolutionName;

    //== Published status values =============================================
    std::atomic<float> lastOutputPeakDb { -144.0f };
    std::atomic<float> limiterReductionDb { 0.0f };
    std::atomic<bool> limiterEngaged { false };
    std::atomic<float> maxBoostDb { 0.0f };
    std::atomic<float> autoHeadroomDb { 0.0f };
    std::atomic<float> effectivePreampDb { 0.0f };
    std::atomic<std::int64_t> clipCount { 0 };
    std::atomic<bool> clipResetRequested { false };
    std::int64_t lastLimiterClips = 0;

    //== Scratch buffers =====================================================
    juce::AudioBuffer<double> dryScratch;
    juce::AudioBuffer<float> convolutionScratch;

    //== Helpers =============================================================
    double computeReplayGainDb (const RtParams& rt) const;
    void updateReplayGainTarget (const RtParams& rt);
    void applyParamsToProcessors (const RtParams& rt);
    void resetProcessorStates();
    void processChain (double* const* pointers, int numCh, int numFrames) noexcept;
    void processConvolution (double* const* pointers, int numCh, int numFrames) noexcept;
    float measureBlockPeak (const juce::AudioBuffer<double>& buffer, int numCh, int start, int numFrames) const;

    DspChainStatus buildStatus() const;
};

//==============================================================================
double DspChain::Impl::computeReplayGainDb (const RtParams& rt) const
{
    if (! rt.replayGain.enabled)
        return 0.0;

    const bool hasGain = trackHasGain.load (std::memory_order_acquire);
    double gain = hasGain ? (double) trackGainDb.load (std::memory_order_relaxed)
                          : (double) rt.replayGain.fallbackGainDb;

    gain += (double) rt.replayGain.preampDb;

    if (rt.replayGain.preventClipping && gain > 0.0)
    {
        const double peak = (double) trackPeakLinear.load (std::memory_order_relaxed);
        if (peak > 0.0)
            gain = std::min (gain, -20.0 * std::log10 (peak));
    }

    return gain;
}

void DspChain::Impl::updateReplayGainTarget (const RtParams& rt)
{
    currentReplayGainDb = computeReplayGainDb (rt);
    replayGainRamp.setTarget (dbToGain (currentReplayGainDb), rt.rampSamples);
}

void DspChain::Impl::resetProcessorStates()
{
    graphic.reset();
    parametric.reset();
    bass.reset();
    filters.reset();
    stereo.reset();
    limiter.reset();
    if (convolutionPrepared)
        convolution.reset();
    convolutionMix.reset (lastConvolutionEnabled ? convolutionMix.target : 0.0);
    replayGainRamp.reset (replayGainRamp.target);
    preampRamp.reset (preampRamp.target);
    lastLimiterClips = 0;
    clipCount.store (0);
}

void DspChain::Impl::applyParamsToProcessors (const RtParams& rt)
{
    const int ramp = rt.bypassed ? 0 : rt.rampSamples;

    graphic.setParams (toGraphicParams (rt.graphic), rt.eqBypass, ramp);
    parametric.setParams (toParametricParams (rt.parametric), rt.eqBypass, ramp);

    // eqBypass disables all three EQ sections (graphic, parametric and bass),
    // matching DspParams::hasActiveProcessing and the status reporting.
    auto bassParams = toBassParams (rt.bass);
    if (rt.eqBypass)
        bassParams.enabled = false;
    bass.setParams (bassParams, ramp);
    filters.setParams (toFilterParams (rt.filters), ramp);
    stereo.setParams (toStereoParams (rt.stereo), ramp);

    // Preamp (automatic headroom + manual trim).
    const double effectivePreampDb = (double) rt.preamp.manualPreampDb + (double) rt.autoHeadroomDb;
    preampRamp.setTarget (dbToGain (effectivePreampDb), ramp);

    // ReplayGain.
    updateReplayGainTarget (rt);

    // Limiter.
    if (rt.preamp.limiterEnabled && ! lastLimiterEnabled)
        limiter.reset();
    lastLimiterEnabled = rt.preamp.limiterEnabled;
    limiter.setCeilingDb (rt.preamp.ceilingDb, ramp);
    limiter.setReleaseMs (rt.preamp.releaseMs);

    // Convolution wet/dry ramp.
    if (rt.convolution.enabled && ! lastConvolutionEnabled)
    {
        if (convolutionPrepared)
            convolution.reset();
        convolutionMix.reset (0.0);
    }
    lastConvolutionEnabled = rt.convolution.enabled;
    convolutionMix.setTarget (rt.convolution.enabled ? std::clamp (rt.convolution.mixPercent / 100.0f, 0.0f, 1.0f) : 0.0f,
                              ramp);

    // Global bypass crossfade.
    if (lastBypassed && ! rt.bypassed)
        resetProcessorStates();
    lastBypassed = rt.bypassed;
    globalBypassMix.setTarget (rt.bypassed ? 0.0 : 1.0, rt.rampSamples);
}

void DspChain::Impl::processConvolution (double* const* pointers, int numCh, int numFrames) noexcept
{
    if (! convolutionPrepared || ! rtLocal.convolution.enabled
        || ! convolutionLoaded.load (std::memory_order_acquire))
        return;

    if (convolutionMix.current <= 0.0 && ! convolutionMix.isRamping())
        return;

    auto* floatData = convolutionScratch.getArrayOfWritePointers();
    const int processedChannels = std::min (numCh, 2);

    for (int ch = 0; ch < 2; ++ch)
    {
        const int source = std::min (ch, processedChannels - 1);
        for (int n = 0; n < numFrames; ++n)
            floatData[ch][n] = (float) pointers[source][n];
    }

    juce::dsp::AudioBlock<float> block (floatData, (size_t) 2, (size_t) numFrames);
    juce::dsp::ProcessContextReplacing<float> context (block);
    convolution.process (context);

    const int outChannels = std::min (numCh, 2);
    for (int n = 0; n < numFrames; ++n)
    {
        const double mix = convolutionMix.next();
        for (int ch = 0; ch < outChannels; ++ch)
        {
            const double dry = pointers[ch][n];
            const double wet = (double) floatData[ch][n];
            pointers[ch][n] = dry + mix * (wet - dry);
        }
    }
}

void DspChain::Impl::processChain (double* const* pointers, int numCh, int numFrames) noexcept
{
    for (int i = 0; i < kNumSlots; ++i)
    {
        const auto slot = (DspSlot) rtLocal.chainOrder[i];
        switch (slot)
        {
            case DspSlot::replayGain:
                if (! replayGainRamp.isAt (1.0))
                {
                    for (int n = 0; n < numFrames; ++n)
                    {
                        const double g = replayGainRamp.next();
                        for (int ch = 0; ch < numCh; ++ch)
                            pointers[ch][n] *= g;
                    }
                }
                break;

            case DspSlot::graphicEq:
                graphic.process (pointers, numCh, numFrames);
                break;

            case DspSlot::parametricEq:
                parametric.process (pointers, numCh, numFrames);
                break;

            case DspSlot::bass:
                bass.process (pointers, numCh, numFrames);
                break;

            case DspSlot::filters:
                filters.process (pointers, numCh, numFrames);
                break;

            case DspSlot::convolution:
                processConvolution (pointers, numCh, numFrames);
                break;

            case DspSlot::crossfeed:
                stereo.processCrossfeed (pointers, numCh, numFrames);
                break;

            case DspSlot::stereoWidth:
                stereo.processWidth (pointers, numCh, numFrames);
                break;

            case DspSlot::balance:
                stereo.processBalance (pointers, numCh, numFrames);
                break;

            case DspSlot::preamp:
                if (! preampRamp.isAt (1.0))
                {
                    for (int n = 0; n < numFrames; ++n)
                    {
                        const double g = preampRamp.next();
                        for (int ch = 0; ch < numCh; ++ch)
                            pointers[ch][n] *= g;
                    }
                }
                break;

            case DspSlot::limiter:
                if (rtLocal.preamp.limiterEnabled)
                {
                    if (clipResetRequested.exchange (false, std::memory_order_acq_rel))
                    {
                        limiter.resetClipCount();
                        lastLimiterClips = 0;
                        clipCount.store (0, std::memory_order_relaxed);
                    }

                    limiter.process (pointers, numCh, numFrames);

                    const auto clips = limiter.clipCount();
                    if (clips != lastLimiterClips)
                    {
                        clipCount.fetch_add (clips - lastLimiterClips, std::memory_order_relaxed);
                        lastLimiterClips = clips;
                    }

                    limiterReductionDb.store ((float) limiter.currentGainReductionDb(), std::memory_order_relaxed);
                    limiterEngaged.store (limiter.isEngaged(), std::memory_order_relaxed);
                }
                break;

            case DspSlot::numSlots:
            default:
                break;
        }
    }
}

float DspChain::Impl::measureBlockPeak (const juce::AudioBuffer<double>& buffer, int numCh, int start, int numFrames) const
{
    float peak = 0.0f;
    for (int ch = 0; ch < numCh; ++ch)
    {
        const auto* data = buffer.getReadPointer (ch, start);
        for (int n = 0; n < numFrames; ++n)
            peak = std::max (peak, (float) std::abs (data[n]));
    }
    return peak > 1.0e-9f ? 20.0f * std::log10 (peak) : -144.0f;
}

//==============================================================================
DspChainStatus DspChain::Impl::buildStatus() const
{
    DspChainStatus status;
    status.sampleRate = sampleRate;
    status.blockSize = maxBlock;
    status.channels = numChannels;

    DspParams p;
    {
        const juce::SpinLock::ScopedLockType lock (controlLock);
        p = controlParams;
    }

    const bool convLoaded = convolutionLoaded.load (std::memory_order_acquire);
    const double rgDb = [&]
    {
        if (! p.replayGain.enabled)
            return 0.0;
        const bool hasGain = trackHasGain.load (std::memory_order_acquire);
        double gain = hasGain ? (double) trackGainDb.load (std::memory_order_relaxed)
                              : (double) p.replayGain.fallbackGainDb;
        gain += (double) p.replayGain.preampDb;
        if (p.replayGain.preventClipping && gain > 0.0)
        {
            const double peak = (double) trackPeakLinear.load (std::memory_order_relaxed);
            if (peak > 0.0)
                gain = std::min (gain, -20.0 * std::log10 (peak));
        }
        return gain;
    }();

    status.maxPositiveBoostDb = (double) maxBoostDb.load();
    status.autoHeadroomDb = autoHeadroomDb.load();
    status.effectivePreampDb = effectivePreampDb.load();
    status.limiterReductionDb = limiterReductionDb.load();
    status.limiterEngaged = limiterEngaged.load();
    status.convolutionLoaded = convLoaded;
    {
        const juce::SpinLock::ScopedLockType lock (controlLock);
        status.convolutionError = convolutionError;
    }

    if (p.bypassed)
    {
        status.anyProcessing = false;
        return status;
    }

    for (int i = 0; i < kNumSlots; ++i)
    {
        const auto slot = p.chainOrder[(size_t) i];
        bool active = false;

        switch (slot)
        {
            case DspSlot::replayGain:
                active = p.replayGain.enabled && std::abs (rgDb) > 1.0e-6;
                break;

            case DspSlot::graphicEq:
                if (! p.eqBypass && p.graphic.enabled)
                {
                    const bool use31 = p.graphic.bandSet == GraphicEqParams::BandSet::bands31;
                    const int count = use31 ? GraphicEqParams::kNumBands31 : GraphicEqParams::kNumBands15;
                    for (int b = 0; b < count; ++b)
                    {
                        const double g = use31 ? p.graphic.gains31[(size_t) b] : p.graphic.gains15[(size_t) b];
                        if (std::abs (g) > 1.0e-4)
                            active = true;
                    }
                }
                break;

            case DspSlot::parametricEq:
                active = ! p.eqBypass && p.parametric.enabled && p.parametric.hasActiveBands();
                break;

            case DspSlot::bass:
                active = ! p.eqBypass && p.bass.isActive();
                break;

            case DspSlot::filters:
                active = p.filters.isActive();
                break;

            case DspSlot::convolution:
                active = p.convolution.enabled && convLoaded && p.convolution.mixPercent > 0.0f;
                break;

            case DspSlot::crossfeed:
                active = p.stereo.crossfeedEnabled;
                break;

            case DspSlot::stereoWidth:
                active = std::abs (p.stereo.widthPercent - 100.0f) > 0.01f || p.stereo.monoBassEnabled;
                break;

            case DspSlot::balance:
                active = std::abs (p.stereo.balance) > 1.0e-4f;
                break;

            case DspSlot::preamp:
                active = std::abs (p.preamp.manualPreampDb) > 1.0e-4f
                         || (p.preamp.autoHeadroom && maxBoostDb.load() > 0.0001f);
                break;

            case DspSlot::limiter:
                active = p.preamp.limiterEnabled;
                break;

            case DspSlot::numSlots:
            default:
                break;
        }

        if (active)
            status.activeSteps.push_back (dspSlotName (slot));
    }

    status.anyProcessing = ! status.activeSteps.empty();
    return status;
}

//==============================================================================
DspChain::DspChain() : impl (std::make_unique<Impl>())
{
    impl->controlParams = DspParams {};
    impl->rtLocal = buildRtParams (impl->controlParams, 0.0f, 0);
    impl->rtLocalValid = true;
    impl->rtDirty.store (false);
    impl->lastBypassed = impl->controlParams.bypassed;
    impl->lastLimiterEnabled = impl->controlParams.preamp.limiterEnabled;
}

DspChain::~DspChain() = default;

void DspChain::prepare (double sampleRate, int maxBlockSize, int numChannels)
{
    auto& d = *impl;

    d.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    d.maxBlock = std::max (1, maxBlockSize);
    d.numChannels = std::clamp (numChannels, 1, kMaxProcessChannels);

    d.graphic.prepare (d.sampleRate, d.maxBlock, d.numChannels);
    d.parametric.prepare (d.sampleRate, d.maxBlock, d.numChannels);
    d.bass.prepare (d.sampleRate, d.maxBlock, d.numChannels);
    d.filters.prepare (d.sampleRate, d.maxBlock, d.numChannels);
    d.stereo.prepare (d.sampleRate, d.maxBlock, d.numChannels);
    d.limiter.prepare (d.sampleRate, d.maxBlock, d.numChannels);

    d.dryScratch.setSize (kMaxProcessChannels, d.maxBlock);
    d.dryScratch.clear();
    d.convolutionScratch.setSize (2, d.maxBlock);
    d.convolutionScratch.clear();

    if (! d.convolutionPrepared)
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = d.sampleRate;
        spec.maximumBlockSize = (juce::uint32) d.maxBlock;
        spec.numChannels = 2;
        d.convolution.prepare (spec);
        d.convolutionPrepared = true;
    }

    d.prepared = true;

    // Re-apply the current params immediately so the chain is correct for the
    // new rate/block size (prepare is called while the audio device is stopped).
    DspParams p;
    {
        const juce::SpinLock::ScopedLockType lock (d.controlLock);
        p = d.controlParams;
    }

    const double maxBoost = computeMaxPositiveBoostDb (p, d.sampleRate);
    const float autoDb = (p.preamp.autoHeadroom && ! p.bypassed) ? (float) (-maxBoost) : 0.0f;
    d.maxBoostDb.store ((float) maxBoost);
    d.autoHeadroomDb.store (autoDb);
    d.effectivePreampDb.store (p.preamp.manualPreampDb + autoDb);

    d.rtLocal = buildRtParams (p, autoDb, 0);
    d.rtLocalValid = true;
    d.lastBypassed = p.bypassed;
    d.lastLimiterEnabled = false;   // force a limiter reset on first application
    d.lastConvolutionEnabled = p.convolution.enabled;
    d.applyParamsToProcessors (d.rtLocal);
    d.resetProcessorStates();
}

void DspChain::reset()
{
    auto& d = *impl;
    if (! d.prepared)
        return;

    d.resetProcessorStates();
    d.globalBypassMix.reset (d.rtLocalValid && d.rtLocal.bypassed ? 0.0 : 1.0);
    d.seenRgGeneration = -1;
    d.lastOutputPeakDb.store (-144.0f);
    d.limiterReductionDb.store (0.0f);
    d.limiterEngaged.store (false);
}

void DspChain::setParams (const DspParams& newParams, int rampMs)
{
    auto& d = *impl;

    const int rampSamples = (d.sampleRate > 0.0 && rampMs > 0)
                                ? (int) std::lround ((double) rampMs * 0.001 * d.sampleRate)
                                : 0;

    const double maxBoost = computeMaxPositiveBoostDb (newParams, d.sampleRate);
    const float autoDb = (newParams.preamp.autoHeadroom && ! newParams.bypassed)
                             ? (float) (-maxBoost) : 0.0f;

    {
        const juce::SpinLock::ScopedLockType lock (d.controlLock);
        d.controlParams = newParams;
        d.maxBoostDb.store ((float) maxBoost);
        d.autoHeadroomDb.store (autoDb);
        d.effectivePreampDb.store (newParams.preamp.manualPreampDb + autoDb);
    }

    RtParams rt = buildRtParams (newParams, autoDb, rampSamples);

    {
        const juce::SpinLock::ScopedLockType lock (d.rtLock);
        d.rtPending = rt;
        d.rtDirty.store (true, std::memory_order_release);
    }
}

void DspChain::setParamsImmediate (const DspParams& params)
{
    setParams (params, 0);
}

DspParams DspChain::getParams() const
{
    const juce::SpinLock::ScopedLockType lock (impl->controlLock);
    return impl->controlParams;
}

void DspChain::setTrackReplayGain (float gainDb, float peakLinear, bool hasGain)
{
    auto& d = *impl;
    d.trackGainDb.store (gainDb, std::memory_order_relaxed);
    d.trackPeakLinear.store (peakLinear, std::memory_order_relaxed);
    d.trackHasGain.store (hasGain, std::memory_order_release);
    d.trackRgGeneration.fetch_add (1, std::memory_order_release);
}

void DspChain::process (juce::AudioBuffer<double>& buffer) noexcept
{
    auto& d = *impl;

    if (! d.prepared || ! d.rtLocalValid)
        return;

    const int totalFrames = buffer.getNumSamples();
    const int bufferChannels = buffer.getNumChannels();
    if (totalFrames <= 0 || bufferChannels <= 0)
        return;

    const int numCh = std::min (bufferChannels, kMaxProcessChannels);

    // Pick up a pending parameter snapshot without ever blocking the audio thread.
    if (d.rtDirty.load (std::memory_order_acquire))
    {
        RtParams pending;
        bool got = false;
        {
            const juce::SpinLock::ScopedTryLockType lock (d.rtLock);
            if (lock.isLocked() && d.rtDirty.load (std::memory_order_acquire))
            {
                pending = d.rtPending;
                d.rtDirty.store (false, std::memory_order_release);
                got = true;
            }
        }
        if (got)
        {
            d.rtLocal = pending;
            d.rtLocalValid = true;
            d.applyParamsToProcessors (d.rtLocal);
            d.seenRgGeneration = d.trackRgGeneration.load (std::memory_order_acquire);
        }
    }
    else
    {
        // ReplayGain can change independently of DspParams (track change).
        const int gen = d.trackRgGeneration.load (std::memory_order_acquire);
        if (gen != d.seenRgGeneration)
        {
            d.seenRgGeneration = gen;
            d.updateReplayGainTarget (d.rtLocal);
        }
    }

    if (d.rtLocal.bypassed && d.globalBypassMix.isAt (0.0) && ! d.globalBypassMix.isRamping())
        return;   // true bypass: the buffer is left bit-identical

    auto* channelPointers = buffer.getArrayOfWritePointers();
    const int chunkSize = std::max (1, std::min (kInternalBlock, d.maxBlock));

    for (int start = 0; start < totalFrames; start += chunkSize)
    {
        const int numFrames = std::min (chunkSize, totalFrames - start);

        const bool needsBlend = ! d.globalBypassMix.isAt (1.0) || d.globalBypassMix.isRamping();

        if (needsBlend)
        {
            for (int ch = 0; ch < numCh; ++ch)
            {
                const auto* in = buffer.getReadPointer (ch, start);
                auto* dry = d.dryScratch.getWritePointer (ch);
                for (int n = 0; n < numFrames; ++n)
                    dry[n] = in[n];
            }
        }

        double* chunkPointers[kMaxProcessChannels] {};
        for (int ch = 0; ch < numCh; ++ch)
            chunkPointers[ch] = channelPointers[ch] + start;

        d.processChain (chunkPointers, numCh, numFrames);

        if (needsBlend)
        {
            for (int n = 0; n < numFrames; ++n)
            {
                const double mix = d.globalBypassMix.next();
                for (int ch = 0; ch < numCh; ++ch)
                {
                    const double dry = d.dryScratch.getReadPointer (ch)[n];
                    chunkPointers[ch][n] = dry + mix * (chunkPointers[ch][n] - dry);
                }
            }
        }

        const float peakDb = d.measureBlockPeak (buffer, numCh, start, numFrames);
        if (peakDb > d.lastOutputPeakDb.load (std::memory_order_relaxed))
            d.lastOutputPeakDb.store (peakDb, std::memory_order_relaxed);
        else
            d.lastOutputPeakDb.store (std::max (peakDb, d.lastOutputPeakDb.load (std::memory_order_relaxed) - 0.1f),
                                      std::memory_order_relaxed);
    }
}

std::vector<float> DspChain::getMagnitudeResponseDb (const std::vector<double>& freqsHz) const
{
    DspParams p;
    {
        const juce::SpinLock::ScopedLockType lock (impl->controlLock);
        p = impl->controlParams;
    }

    std::vector<float> result;
    result.reserve (freqsHz.size());
    for (double f : freqsHz)
        result.push_back ((float) combinedResponseDb (p, impl->sampleRate, f));
    return result;
}

DspChainStatus DspChain::getStatus() const
{
    return impl->buildStatus();
}

//==============================================================================
void DspChain::loadImpulseResponse (const juce::File& file, juce::String& errorOut)
{
    auto& d = *impl;

    errorOut.clear();

    if (! file.existsAsFile())
    {
        errorOut = "Impulse response file not found: " + file.getFullPathName();
        const juce::SpinLock::ScopedLockType lock (d.controlLock);
        d.convolutionError = errorOut;
        d.convolutionLoaded.store (false);
        return;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
    {
        errorOut = "Unsupported or empty impulse response file: " + file.getFileName();
        const juce::SpinLock::ScopedLockType lock (d.controlLock);
        d.convolutionError = errorOut;
        d.convolutionLoaded.store (false);
        return;
    }

    const int channels = std::clamp ((int) reader->numChannels, 1, 2);
    const auto maxLength = (juce::int64) (d.sampleRate * 60.0);   // 60 s ceiling
    const int length = (int) std::min (reader->lengthInSamples, maxLength);
    if (length <= 0)
    {
        errorOut = "Impulse response is too short";
        const juce::SpinLock::ScopedLockType lock (d.controlLock);
        d.convolutionError = errorOut;
        d.convolutionLoaded.store (false);
        return;
    }

    juce::AudioBuffer<float> ir (channels, length);
    ir.clear();
    if (! reader->read (&ir, 0, length, 0, true, channels > 1))
    {
        errorOut = "Failed to read impulse response audio data: " + file.getFileName();
        const juce::SpinLock::ScopedLockType lock (d.controlLock);
        d.convolutionError = errorOut;
        d.convolutionLoaded.store (false);
        return;
    }

    const double irSampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : d.sampleRate;
    reader.reset();

    bool trueStereo = false;
    juce::String irName;
    {
        const juce::SpinLock::ScopedLockType lock (d.controlLock);
        trueStereo = d.controlParams.convolution.trueStereo;
        irName = file.getFileNameWithoutExtension();
    }

    d.convolution.loadImpulseResponse (std::move (ir), irSampleRate,
                                       trueStereo ? juce::dsp::Convolution::Stereo::yes
                                                  : juce::dsp::Convolution::Stereo::no,
                                       juce::dsp::Convolution::Trim::yes,
                                       juce::dsp::Convolution::Normalise::yes);

    {
        const juce::SpinLock::ScopedLockType lock (d.controlLock);
        d.convolutionError.clear();
        d.convolutionName = irName;
        d.controlParams.convolution.irPath = file.getFullPathName();
        d.controlParams.convolution.irName = irName;
    }
    d.convolutionLoaded.store (true, std::memory_order_release);
}

void DspChain::clearImpulseResponse()
{
    auto& d = *impl;
    d.convolutionLoaded.store (false, std::memory_order_release);
    d.convolutionMix.setTarget (0.0, d.rtLocal.rampSamples);
    const juce::SpinLock::ScopedLockType lock (d.controlLock);
    d.convolutionError = "No impulse response loaded";
    d.convolutionName.clear();
    d.controlParams.convolution.irPath.clear();
    d.controlParams.convolution.irName.clear();
}

bool DspChain::isImpulseResponseLoaded() const
{
    return impl->convolutionLoaded.load (std::memory_order_acquire);
}

float DspChain::getLastOutputPeakDb() const
{
    return impl->lastOutputPeakDb.load (std::memory_order_relaxed);
}

float DspChain::getLimiterGainReductionDb() const
{
    return impl->limiterReductionDb.load (std::memory_order_relaxed);
}

bool DspChain::getLimiterEngaged() const
{
    return impl->limiterEngaged.load (std::memory_order_relaxed);
}

int DspChain::getLatencySamples() const
{
    auto& d = *impl;
    if (! d.prepared)
        return 0;

    DspParams p;
    {
        const juce::SpinLock::ScopedLockType lock (d.controlLock);
        p = d.controlParams;
    }

    if (p.bypassed || ! p.preamp.limiterEnabled)
        return 0;

    return d.limiter.latencySamples();
}

std::int64_t DspChain::getClippedSampleCount() const
{
    return impl->clipCount.load (std::memory_order_relaxed);
}

void DspChain::resetClippingCount()
{
    impl->clipResetRequested.store (true, std::memory_order_release);
}

} // namespace sp
