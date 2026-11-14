#include "DSP/Equalizers.h"

#include <algorithm>
#include <cmath>

namespace sp
{

namespace
{
constexpr double kGraphicQ15 = 2.145;   // 2/3 octave constant-Q
constexpr double kGraphicQ31 = 4.32;    // 1/3 octave constant-Q

inline double log2Clamped (double hz, double lo = 1.0, double hi = 40000.0)
{
    return std::log2 (std::clamp (hz, lo, hi));
}

inline bool isGainType (int type)
{
    return type == (int) BandType::bell || type == (int) BandType::lowShelf || type == (int) BandType::highShelf;
}

} // namespace

//==============================================================================
// GraphicEqProcessor
//==============================================================================
GraphicEqProcessor::GraphicEqProcessor()
{
    for (int i = 0; i < GraphicEqParams::kNumBands15; ++i)
        bank15.gainSmooth[(size_t) i].reset (0.0);
    for (int i = 0; i < GraphicEqParams::kNumBands31; ++i)
        bank31.gainSmooth[(size_t) i].reset (0.0);
}

void GraphicEqProcessor::prepare (double sr, int maxBlockSize, int numChannels)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;
    maxBlock = std::max (1, maxBlockSize);
    numChannelsPrepared = std::clamp (numChannels, 1, kMaxChannels);

    bank15.count = GraphicEqParams::kNumBands15;
    bank31.count = GraphicEqParams::kNumBands31;

    const auto& c15 = GraphicEqParams::centres15();
    const auto& c31 = GraphicEqParams::centres31();

    for (int i = 0; i < bank15.count; ++i)
    {
        bank15.centreHz[(size_t) i] = c15[(size_t) i];
        bank15.q[(size_t) i] = kGraphicQ15;
        bank15.coeffValid[(size_t) i] = false;
    }

    for (int i = 0; i < bank31.count; ++i)
    {
        bank31.centreHz[(size_t) i] = c31[(size_t) i];
        bank31.q[(size_t) i] = kGraphicQ31;
        bank31.coeffValid[(size_t) i] = false;
    }

    for (int i = 0; i < bank15.count; ++i)
        bank15.gainSmooth[(size_t) i].reset (0.0);
    for (int i = 0; i < bank31.count; ++i)
        bank31.gainSmooth[(size_t) i].reset (0.0);

    reset();
}

void GraphicEqProcessor::reset()
{
    resetBankStates (bank15);
    resetBankStates (bank31);
    for (auto& v : bank15.coeffValid) v = false;
    for (auto& v : bank31.coeffValid) v = false;
    sectionMix.reset (wantActive ? 1.0 : 0.0);
}

void GraphicEqProcessor::resetBankStates (BandBank& bank) noexcept
{
    for (auto& perChannel : bank.states)
        for (auto& st : perChannel)
            st.reset();
}

void GraphicEqProcessor::setParams (const GraphicEqParams& p, bool eqBypass, int ramp)
{
    rampSamples = std::max (0, ramp);

    const int wantBank = (p.bandSet == GraphicEqParams::BandSet::bands31) ? 1 : 0;

    if (wantBank != targetBank)
    {
        targetBank = wantBank;
        if (sectionMix.isAt (1.0) || sectionMix.isRamping() || wantActive)
            pendingBankSwitch = true;
        else
        {
            activeBank = targetBank;
            resetBankStates ((activeBank == 0) ? bank15 : bank31);
            if (activeBank == 0)
            {
                for (auto& v : bank15.coeffValid) v = false;
            }
            else
            {
                for (auto& v : bank31.coeffValid) v = false;
            }
            pendingBankSwitch = false;
        }
    }

    for (int i = 0; i < GraphicEqParams::kNumBands15; ++i)
        bank15.gainSmooth[(size_t) i].setTarget (std::clamp ((double) p.gains15[(size_t) i], -12.0, 12.0), rampSamples);

    for (int i = 0; i < GraphicEqParams::kNumBands31; ++i)
        bank31.gainSmooth[(size_t) i].setTarget (std::clamp ((double) p.gains31[(size_t) i], -12.0, 12.0), rampSamples);

    bool targetAny = false;
    if (targetBank == 0)
    {
        for (int i = 0; i < GraphicEqParams::kNumBands15; ++i)
            if (std::abs (bank15.gainSmooth[(size_t) i].target) > 1.0e-6)
                targetAny = true;
    }
    else
    {
        for (int i = 0; i < GraphicEqParams::kNumBands31; ++i)
            if (std::abs (bank31.gainSmooth[(size_t) i].target) > 1.0e-6)
                targetAny = true;
    }

    const bool wasActive = wantActive;
    wantActive = p.enabled && ! eqBypass && targetAny;

    // If the section is completely silent we can switch banks without a fade.
    if (! wantActive && sectionMix.isAt (0.0))
    {
        if (activeBank != targetBank)
        {
            activeBank = targetBank;
            resetBankStates ((activeBank == 0) ? bank15 : bank31);
        }
        pendingBankSwitch = false;
    }

    if (! wantActive)
    {
        sectionMix.setTarget (0.0, rampSamples);
        return;
    }

    if (pendingBankSwitch)
    {
        sectionMix.setTarget (0.0, rampSamples);
        return;
    }

    if (! wasActive && sectionMix.isAt (0.0))
    {
        resetBankStates ((activeBank == 0) ? bank15 : bank31);
        if (activeBank == 0)
        {
            for (auto& v : bank15.coeffValid) v = false;
        }
        else
        {
            for (auto& v : bank31.coeffValid) v = false;
        }
    }

    sectionMix.setTarget (1.0, rampSamples);
}

void GraphicEqProcessor::updateBankCoefficients (BandBank& bank, int numFrames) noexcept
{
    for (int b = 0; b < bank.count; ++b)
    {
        const double g = bank.gainSmooth[(size_t) b].advance (numFrames);
        if (! bank.coeffValid[(size_t) b] || g != bank.designedGain[(size_t) b])
        {
            bank.coeffs[(size_t) b] = BiquadCoeffs::bell (sampleRate, bank.centreHz[(size_t) b], g, bank.q[(size_t) b]);
            bank.designedGain[(size_t) b] = g;
            bank.coeffValid[(size_t) b] = true;
        }
    }
}

void GraphicEqProcessor::processBank (BandBank& bank, double* const* channels, int numChannels,
                                      int numFrames, bool blend, SampleRamp& mix) noexcept
{
    const int nch = std::min (numChannels, kMaxChannels);
    const int nb = bank.count;

    if (! blend)
    {
        for (int ch = 0; ch < nch; ++ch)
        {
            auto& st = bank.states[(size_t) ch];
            double* data = channels[ch];

            for (int n = 0; n < numFrames; ++n)
            {
                double x = data[n];
                for (int b = 0; b < nb; ++b)
                    x = st[(size_t) b].process (bank.coeffs[(size_t) b], x);
                data[n] = x;
            }
        }
        return;
    }

    for (int n = 0; n < numFrames; ++n)
    {
        const double m = mix.next();
        for (int ch = 0; ch < nch; ++ch)
        {
            auto& st = bank.states[(size_t) ch];
            const double dry = channels[ch][n];
            double wet = dry;
            for (int b = 0; b < nb; ++b)
                wet = st[(size_t) b].process (bank.coeffs[(size_t) b], wet);
            channels[ch][n] = dry + m * (wet - dry);
        }
    }
}

void GraphicEqProcessor::process (double* const* channels, int numChannels, int numFrames) noexcept
{
    if (numFrames <= 0 || channels == nullptr)
        return;

    // Keep coefficients current even while faded out so the next fade-in is clean.
    updateBankCoefficients (bank15, numFrames);
    updateBankCoefficients (bank31, numFrames);

    if (pendingBankSwitch && sectionMix.isAt (0.0))
    {
        activeBank = targetBank;
        resetBankStates ((activeBank == 0) ? bank15 : bank31);
        if (activeBank == 0)
        {
            for (auto& v : bank15.coeffValid) v = false;
        }
        else
        {
            for (auto& v : bank31.coeffValid) v = false;
        }
        pendingBankSwitch = false;
        if (wantActive)
            sectionMix.setTarget (1.0, rampSamples);
    }

    if (sectionMix.isAt (0.0) && ! sectionMix.isRamping())
        return;

    BandBank& bank = (activeBank == 0) ? bank15 : bank31;
    const bool blend = ! sectionMix.isAt (1.0) || sectionMix.isRamping();

    processBank (bank, channels, numChannels, numFrames, blend, sectionMix);
}

double GraphicEqProcessor::magnitudeDbAt (double freqHz) const
{
    const BandBank& bank = (targetBank == 0) ? bank15 : bank31;
    double db = 0.0;
    for (int b = 0; b < bank.count; ++b)
    {
        const BiquadCoeffs c = BiquadCoeffs::bell (sampleRate, bank.centreHz[(size_t) b],
                                                   bank.gainSmooth[(size_t) b].target, bank.q[(size_t) b]);
        db += c.magnitudeDbAt (freqHz, sampleRate);
    }
    return db;
}

//==============================================================================
// ParametricEqProcessor
//==============================================================================
ParametricEqProcessor::ParametricEqProcessor()
{
    reset();
}

void ParametricEqProcessor::prepare (double sr, int maxBlockSize, int numChannels)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;
    maxBlock = std::max (1, maxBlockSize);
    numChannelsPrepared = std::clamp (numChannels, 1, kMaxChannels);

    for (auto& b : bands)
    {
        b.gainDb.reset (0.0);
        b.log2Freq.reset (std::log2 (1000.0));
        b.log2Q.reset (std::log2 (0.7071067811865476));
        b.type = (int) BandType::bell;
        b.targetType = b.type;
        b.wantActive = false;
        b.mix.reset (0.0);
        b.coeffValid = false;
        b.designedType = -1;
        b.coeffs = BiquadCoeffs::makeIdentity();
        for (auto& st : b.states)
            st.reset();
    }
}

void ParametricEqProcessor::reset()
{
    for (auto& b : bands)
    {
        for (auto& st : b.states)
            st.reset();
        b.coeffValid = false;
        b.designedType = -1;
        b.mix.reset (b.wantActive ? 1.0 : 0.0);
        b.gainDb.reset (isGainType (b.type) ? b.gainDb.target : 0.0);
        b.log2Freq.reset (b.log2Freq.target);
        b.log2Q.reset (b.log2Q.target);
    }
}

void ParametricEqProcessor::activateBand (Band& band)
{
    for (auto& st : band.states)
        st.reset();
    band.coeffValid = false;
    band.designedType = -1;
}

void ParametricEqProcessor::setParams (const ParametricEqParams& p, bool eqBypass, int ramp)
{
    rampSamples = std::max (0, ramp);

    for (int i = 0; i < kNumBands; ++i)
    {
        Band& b = bands[(size_t) i];
        const EqBand& pb = p.bands[(size_t) i];

        const bool want = p.enabled && ! eqBypass && pb.isActive();
        const int newType = (int) pb.type;

        if (newType != b.targetType)
        {
            b.targetType = newType;
            if (b.wantActive)
            {
                b.mix.setTarget (0.0, rampSamples);   // fade out, swap type when silent
            }
            else
            {
                // Band is not running: change the topology immediately.
                b.type = newType;
                activateBand (b);
            }
        }

        const double targetFreqLog2 = log2Clamped (pb.freqHz, 10.0, 40000.0);
        const double targetQLog2 = std::log2 (std::clamp (pb.q, 0.1, 24.0));
        const double targetGain = isGainType (newType) ? std::clamp (pb.gainDb, -24.0, 24.0) : 0.0;

        if (want && ! b.wantActive)
        {
            activateBand (b);
            b.gainDb.reset (0.0);
            b.log2Freq.reset (targetFreqLog2);
            b.log2Q.reset (targetQLog2);
            b.gainDb.setTarget (targetGain, rampSamples);
            b.mix.setTarget (1.0, rampSamples);
        }
        else if (want)
        {
            b.gainDb.setTarget (targetGain, rampSamples);
            b.log2Freq.setTarget (targetFreqLog2, rampSamples);
            b.log2Q.setTarget (targetQLog2, rampSamples);
            if (b.type == b.targetType)
                b.mix.setTarget (1.0, rampSamples);
        }
        else if (b.wantActive)
        {
            b.gainDb.setTarget (0.0, rampSamples);
            b.mix.setTarget (0.0, rampSamples);
        }

        b.wantActive = want;
    }
}

void ParametricEqProcessor::updateBandCoefficients (Band& band, int numFrames) noexcept
{
    const double gain = band.gainDb.advance (numFrames);
    const double freq = std::pow (2.0, band.log2Freq.advance (numFrames));
    const double q = std::pow (2.0, band.log2Q.advance (numFrames));

    const bool changed = ! band.coeffValid
                         || band.designedType != band.type
                         || gain != band.designedGain
                         || freq != band.designedFreq
                         || q != band.designedQ;

    if (changed)
    {
        band.coeffs = BiquadCoeffs::design (band.type, sampleRate, freq, gain, q);
        band.designedGain = gain;
        band.designedFreq = freq;
        band.designedQ = q;
        band.designedType = band.type;
        band.coeffValid = true;
    }
}

void ParametricEqProcessor::process (double* const* channels, int numChannels, int numFrames) noexcept
{
    if (numFrames <= 0 || channels == nullptr)
        return;

    const int nch = std::min (numChannels, kMaxChannels);

    // Finish deferred type changes once the band has faded out.
    for (auto& b : bands)
    {
        if (b.type != b.targetType && b.mix.isAt (0.0))
        {
            b.type = b.targetType;
            activateBand (b);
            b.mix.setTarget (b.wantActive ? 1.0 : 0.0, rampSamples);
        }
    }

    int indices[kNumBands];
    int count = 0;
    bool anySolo = true;

    for (int i = 0; i < kNumBands; ++i)
    {
        Band& b = bands[(size_t) i];
        if (b.mix.current > 0.0 || b.mix.isRamping())
        {
            indices[count++] = i;
            if (! b.mix.isAt (1.0))
                anySolo = false;
        }
    }

    if (count == 0)
        return;

    for (int k = 0; k < count; ++k)
        updateBandCoefficients (bands[(size_t) indices[k]], numFrames);

    if (anySolo)
    {
        // Every audible band is fully in: straight cascade, channel at a time.
        for (int ch = 0; ch < nch; ++ch)
        {
            double* data = channels[ch];
            for (int n = 0; n < numFrames; ++n)
            {
                double x = data[n];
                for (int k = 0; k < count; ++k)
                {
                    Band& b = bands[(size_t) indices[k]];
                    x = b.states[(size_t) ch].process (b.coeffs, x);
                }
                data[n] = x;
            }
        }
        return;
    }

    double mixValues[kNumBands];
    for (int n = 0; n < numFrames; ++n)
    {
        for (int k = 0; k < count; ++k)
            mixValues[k] = bands[(size_t) indices[k]].mix.next();

        for (int ch = 0; ch < nch; ++ch)
        {
            double x = channels[ch][n];
            for (int k = 0; k < count; ++k)
            {
                Band& b = bands[(size_t) indices[k]];
                const double wet = b.states[(size_t) ch].process (b.coeffs, x);
                x = x + mixValues[k] * (wet - x);
            }
            channels[ch][n] = x;
        }
    }
}

double ParametricEqProcessor::magnitudeDbAt (double freqHz) const
{
    double db = 0.0;
    for (const auto& b : bands)
    {
        if (! b.wantActive || b.targetType != b.type)
            continue;
        const double gain = b.gainDb.target;
        const double freq = std::pow (2.0, b.log2Freq.target);
        const double q = std::pow (2.0, b.log2Q.target);
        db += BiquadCoeffs::design (b.type, sampleRate, freq, gain, q).magnitudeDbAt (freqHz, sampleRate);
    }
    return db;
}

//==============================================================================
// BassProcessor
//==============================================================================
BassProcessor::BassProcessor()
{
    for (int i = 0; i < kNumBands; ++i)
    {
        gainDb[(size_t) i].reset (0.0);
        log2Freq[(size_t) i].reset (std::log2 (i == main ? 80.0 : 100.0));
        log2Q[(size_t) i].reset (std::log2 (0.7071));
        coeffValid[(size_t) i] = false;
    }
}

void BassProcessor::prepare (double sr, int maxBlockSize, int numChannels)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;
    maxBlock = std::max (1, maxBlockSize);
    numChannelsPrepared = std::clamp (numChannels, 1, kMaxChannels);

    for (int i = 0; i < kNumBands; ++i)
    {
        gainDb[(size_t) i].reset (0.0);
        log2Freq[(size_t) i].reset (std::log2 (i == main ? 80.0 : 100.0));
        log2Q[(size_t) i].reset (std::log2 (0.7071));
        coeffValid[(size_t) i] = false;
        for (auto& perChannel : states)
            perChannel[(size_t) i].reset();
    }
    wasSkipping = true;
}

void BassProcessor::reset()
{
    for (auto& perChannel : states)
        for (auto& st : perChannel)
            st.reset();
    for (auto& v : coeffValid) v = false;
    for (int i = 0; i < kNumBands; ++i)
    {
        gainDb[(size_t) i].reset (gainDb[(size_t) i].target);
        log2Freq[(size_t) i].reset (log2Freq[(size_t) i].target);
        log2Q[(size_t) i].reset (log2Q[(size_t) i].target);
    }
    wasSkipping = true;
}

void BassProcessor::setParams (const BassParams& p, int ramp)
{
    rampSamples = std::max (0, ramp);

    const bool on = p.enabled;
    const double mainDb = on ? std::clamp ((double) p.gainDb, -24.0, 24.0) : 0.0;
    const double subDb = (on && p.subEnabled) ? std::clamp ((double) p.subGainDb, -24.0, 24.0) : 0.0;
    const double midDb = (on && p.midBassEnabled) ? std::clamp ((double) p.midBassGainDb, -24.0, 24.0) : 0.0;
    const double punchDb = (on && p.punchEnabled) ? std::clamp ((double) p.punchGainDb, -24.0, 24.0) : 0.0;

    double s = p.shelfS;
    if (std::abs (p.shelfS - 1.0) < 1.0e-6 && std::abs (p.shelfQ - 0.7071067811865476) > 1.0e-4)
        s = BiquadCoeffs::shelfSFromQ (p.shelfQ, mainDb);
    targetShelfS = std::clamp (s, 0.1, 4.0);

    gainDb[main].setTarget (mainDb, rampSamples);
    gainDb[sub].setTarget (subDb, rampSamples);
    gainDb[midBass].setTarget (midDb, rampSamples);
    gainDb[punch].setTarget (punchDb, rampSamples);

    log2Freq[main].setTarget (log2Clamped (p.shelfFreqHz, 10.0, 500.0), rampSamples);
    log2Freq[sub].setTarget (log2Clamped (p.subFreqHz, 15.0, 120.0), rampSamples);
    log2Freq[midBass].setTarget (log2Clamped (p.midBassFreqHz, 40.0, 200.0), rampSamples);
    log2Freq[punch].setTarget (log2Clamped (p.punchFreqHz, 50.0, 300.0), rampSamples);

    log2Q[sub].setTarget (std::log2 (std::clamp (p.subQ, 0.1, 12.0)), rampSamples);
    log2Q[midBass].setTarget (std::log2 (std::clamp (p.midBassQ, 0.1, 12.0)), rampSamples);
    log2Q[punch].setTarget (std::log2 (std::clamp (p.punchQ, 0.1, 12.0)), rampSamples);
    log2Q[main].setTarget (std::log2 (0.7071067811865476), rampSamples);
}

void BassProcessor::updateCoefficients (int numFrames) noexcept
{
    for (int i = 0; i < kNumBands; ++i)
    {
        const double g = gainDb[(size_t) i].advance (numFrames);
        const double freq = std::pow (2.0, log2Freq[(size_t) i].advance (numFrames));
        const double q = std::pow (2.0, log2Q[(size_t) i].advance (numFrames));

        const bool changed = ! coeffValid[(size_t) i]
                             || g != designedGain[(size_t) i]
                             || freq != designedFreq[(size_t) i]
                             || q != designedQ[(size_t) i];

        if (! changed)
            continue;

        if (i == main)
            coeffs[(size_t) i] = BiquadCoeffs::lowShelf (sampleRate, freq, g, targetShelfS);
        else
            coeffs[(size_t) i] = BiquadCoeffs::bell (sampleRate, freq, g, q);

        designedGain[(size_t) i] = g;
        designedFreq[(size_t) i] = freq;
        designedQ[(size_t) i] = q;
        coeffValid[(size_t) i] = true;
    }
}

void BassProcessor::process (double* const* channels, int numChannels, int numFrames) noexcept
{
    if (numFrames <= 0 || channels == nullptr)
        return;

    updateCoefficients (numFrames);

    bool allZero = true;
    for (int i = 0; i < kNumBands; ++i)
        if (std::abs (gainDb[(size_t) i].current) > 1.0e-9 || gainDb[(size_t) i].isRamping())
            allZero = false;

    if (allZero)
    {
        wasSkipping = true;
        return;
    }

    if (wasSkipping)
    {
        for (auto& perChannel : states)
            for (auto& st : perChannel)
                st.reset();
        wasSkipping = false;
    }

    const int nch = std::min (numChannels, kMaxChannels);

    for (int ch = 0; ch < nch; ++ch)
    {
        auto& st = states[(size_t) ch];
        double* data = channels[ch];

        for (int n = 0; n < numFrames; ++n)
        {
            double x = data[n];
            for (int i = 0; i < kNumBands; ++i)
                x = st[(size_t) i].process (coeffs[(size_t) i], x);
            data[n] = x;
        }
    }
}

double BassProcessor::magnitudeDbAt (double freqHz) const
{
    double db = 0.0;
    for (int i = 0; i < kNumBands; ++i)
    {
        const double g = gainDb[(size_t) i].target;
        const double freq = std::pow (2.0, log2Freq[(size_t) i].target);
        const double q = std::pow (2.0, log2Q[(size_t) i].target);
        if (i == main)
            db += BiquadCoeffs::lowShelf (sampleRate, freq, g, targetShelfS).magnitudeDbAt (freqHz, sampleRate);
        else
            db += BiquadCoeffs::bell (sampleRate, freq, g, q).magnitudeDbAt (freqHz, sampleRate);
    }
    return db;
}

//==============================================================================
// FilterProcessor
//==============================================================================
FilterProcessor::FilterProcessor()
{
    hp.highPass = true;
    lp.highPass = false;
    hp.log2Cutoff.reset (std::log2 (20.0));
    lp.log2Cutoff.reset (std::log2 (20000.0));
    for (int s = 0; s < kMaxStages; ++s)
    {
        hp.stages[(size_t) s].log2Q.reset (std::log2 (0.7071));
        lp.stages[(size_t) s].log2Q.reset (std::log2 (0.7071));
        hp.stages[(size_t) s].mix.reset (0.0);
        lp.stages[(size_t) s].mix.reset (0.0);
    }
}

void FilterProcessor::prepare (double sr, int maxBlockSize, int numChannels)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;
    maxBlock = std::max (1, maxBlockSize);
    numChannelsPrepared = std::clamp (numChannels, 1, kMaxChannels);

    for (Bank* bank : { &hp, &lp })
    {
        bank->wantActive = false;
        bank->numStagesActive = 0;
        for (auto& stage : bank->stages)
        {
            stage.coeffValid = false;
            stage.log2Q.reset (std::log2 (0.7071));
            stage.mix.reset (0.0);
            for (auto& st : stage.states)
                st.reset();
        }
    }
    hp.log2Cutoff.reset (std::log2 (20.0));
    lp.log2Cutoff.reset (std::log2 (20000.0));
}

void FilterProcessor::reset()
{
    resetBankStates (hp);
    resetBankStates (lp);
    hp.log2Cutoff.reset (hp.log2Cutoff.target);
    lp.log2Cutoff.reset (lp.log2Cutoff.target);
    for (Bank* bank : { &hp, &lp })
    {
        for (auto& stage : bank->stages)
        {
            stage.mix.reset (stage.mix.target);
            stage.log2Q.reset (stage.log2Q.target);
            stage.coeffValid = false;
        }
    }
}

void FilterProcessor::resetBankStates (Bank& bank) noexcept
{
    for (auto& stage : bank.stages)
    {
        for (auto& st : stage.states)
            st.reset();
        stage.coeffValid = false;
    }
}

void FilterProcessor::setParams (const FilterParams& p, int ramp)
{
    rampSamples = std::max (0, ramp);

    struct BankSetup { Bank* bank; bool enabled; double cutoffHz; int slope; };
    const BankSetup setups[2] = {
        { &hp, p.highPassEnabled, p.highPassHz, p.highPassSlopeDbPerOct },
        { &lp, p.lowPassEnabled, p.lowPassHz, p.lowPassSlopeDbPerOct }
    };

    for (const auto& setup : setups)
    {
        Bank& bank = *setup.bank;
        bank.wantActive = setup.enabled;

        const double cutoff = BiquadCoeffs::clampFrequency (setup.cutoffHz, sampleRate);
        bank.log2Cutoff.setTarget (std::log2 (std::max (1.0, cutoff)), rampSamples);

        std::array<double, 4> qs {};
        const int numStages = setup.enabled ? Butterworth::designQs (setup.slope, qs) : 0;
        bank.numStagesActive = numStages;

        for (int s = 0; s < kMaxStages; ++s)
        {
            Stage& stage = bank.stages[(size_t) s];
            if (numStages > 0)
                stage.log2Q.setTarget (std::log2 (qs[(size_t) s]), rampSamples);

            const bool wantStage = setup.enabled && s < numStages;
            if (wantStage)
            {
                if (stage.mix.isAt (0.0))
                {
                    for (auto& st : stage.states)
                        st.reset();
                    stage.coeffValid = false;
                }
                stage.mix.setTarget (1.0, rampSamples);
            }
            else
            {
                stage.mix.setTarget (0.0, rampSamples);
            }
        }
    }
}

void FilterProcessor::updateBankCoefficients (Bank& bank, int numFrames) noexcept
{
    const double cutoff = std::pow (2.0, bank.log2Cutoff.advance (numFrames));

    for (auto& stage : bank.stages)
    {
        const double q = std::pow (2.0, stage.log2Q.advance (numFrames));

        const bool changed = ! stage.coeffValid || q != stage.designedQ;
        if (! changed)
            continue;

        stage.coeffs = bank.highPass ? BiquadCoeffs::highPass (sampleRate, cutoff, q)
                                     : BiquadCoeffs::lowPass (sampleRate, cutoff, q);
        stage.designedQ = q;
        stage.coeffValid = true;
    }
}

bool FilterProcessor::bankIsAudible (const Bank& bank) const noexcept
{
    for (const auto& stage : bank.stages)
        if (stage.mix.current > 0.0 || stage.mix.isRamping())
            return true;
    return false;
}

void FilterProcessor::processBank (Bank& bank, double* const* channels, int numChannels, int numFrames) noexcept
{
    if (! bankIsAudible (bank))
        return;

    const int nch = std::min (numChannels, kMaxChannels);

    int indices[kMaxStages];
    int count = 0;
    bool anySolo = true;

    for (int s = 0; s < kMaxStages; ++s)
    {
        Stage& stage = bank.stages[(size_t) s];
        if (stage.mix.current > 0.0 || stage.mix.isRamping())
        {
            indices[count++] = s;
            if (! stage.mix.isAt (1.0))
                anySolo = false;
        }
    }

    if (count == 0)
        return;

    if (anySolo)
    {
        for (int ch = 0; ch < nch; ++ch)
        {
            double* data = channels[ch];
            for (int n = 0; n < numFrames; ++n)
            {
                double x = data[n];
                for (int k = 0; k < count; ++k)
                {
                    Stage& stage = bank.stages[(size_t) indices[k]];
                    x = stage.states[(size_t) ch].process (stage.coeffs, x);
                }
                data[n] = x;
            }
        }
        return;
    }

    double mixValues[kMaxStages];
    for (int n = 0; n < numFrames; ++n)
    {
        for (int k = 0; k < count; ++k)
            mixValues[k] = bank.stages[(size_t) indices[k]].mix.next();

        for (int ch = 0; ch < nch; ++ch)
        {
            double x = channels[ch][n];
            for (int k = 0; k < count; ++k)
            {
                Stage& stage = bank.stages[(size_t) indices[k]];
                const double wet = stage.states[(size_t) ch].process (stage.coeffs, x);
                x = x + mixValues[k] * (wet - x);
            }
            channels[ch][n] = x;
        }
    }
}

void FilterProcessor::process (double* const* channels, int numChannels, int numFrames) noexcept
{
    if (numFrames <= 0 || channels == nullptr)
        return;

    updateBankCoefficients (hp, numFrames);
    updateBankCoefficients (lp, numFrames);

    processBank (hp, channels, numChannels, numFrames);
    processBank (lp, channels, numChannels, numFrames);
}

double FilterProcessor::magnitudeDbAt (double freqHz) const
{
    double db = 0.0;

    for (const Bank* bank : { &hp, &lp })
    {
        const double cutoff = std::pow (2.0, bank->log2Cutoff.target);
        for (int s = 0; s < bank->numStagesActive; ++s)
        {
            const double q = std::pow (2.0, bank->stages[(size_t) s].log2Q.target);
            const auto c = bank->highPass ? BiquadCoeffs::highPass (sampleRate, cutoff, q)
                                          : BiquadCoeffs::lowPass (sampleRate, cutoff, q);
            db += c.magnitudeDbAt (freqHz, sampleRate);
        }
    }

    return db;
}

} // namespace sp
