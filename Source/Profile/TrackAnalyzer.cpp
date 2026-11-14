/*
    Snake Puppy Reference Player
    Offline track analysis (see TrackAnalyzer.h for the method summary).

    Implementation notes
    --------------------
    * Loudness follows BS.1770-4 closely but the K-weighting biquads are
      re-derived from their analogue prototypes at the file's sample rate,
      which is the usual practical simplification when analysing mixed sample
      rates. Blocks are 400 ms with a 100 ms hop, gated absolutely at -70 LUFS
      and relatively at -10 LU.
    * The FFT calibration divides each one-sided bin power by N * sum(w^2)
      (Hann). That makes the summed spectrum equal the mean square of the
      analysed signal, so band ratios and absolute band dB are both meaningful
      without an extra arbitrary scale.
    * True peak uses JUCE's 4x half-band FIR oversampler (max quality) and takes
      the maximum absolute value of the oversampled signal in dBTP.
*/
#include "Profile/TrackAnalyzer.h"
#include "Audio/AudioEngine.h"
#include "Audio/DecodeSource.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace sp
{

namespace
{
//==============================================================================
constexpr double kLufsOffset = -0.691;          // BS.1770 constant
constexpr double kGateAbsoluteLufs = -70.0;
constexpr double kGateRelativeLu = -10.0;
constexpr double kFloorDb = -144.0;

constexpr int kFftOrder = 11;
constexpr int kFftSize = 1 << kFftOrder;        // 2048
constexpr int kNumBands = kFftSize / 2 + 1;

constexpr int kTruePeakBlock = 4096;

//==============================================================================
double linearToDb (double v)
{
    return v > 0.0 ? 20.0 * std::log10 (v) : kFloorDb;
}

double powerToDb (double p)
{
    return p > 0.0 ? 10.0 * std::log10 (p) : kFloorDb;
}

//==============================================================================
/** RBJ biquad in transposed direct form II, double precision. */
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    void reset() { z1 = z2 = 0.0; }

    void normalise (double nb0, double nb1, double nb2, double na0, double na1, double na2)
    {
        const double inv = 1.0 / na0;
        b0 = nb0 * inv; b1 = nb1 * inv; b2 = nb2 * inv;
        a1 = na1 * inv; a2 = na2 * inv;
    }

    void setHighShelf (double sampleRate, double freqHz, double q, double gainDb)
    {
        const double a = std::pow (10.0, gainDb / 40.0);
        const double w0 = 2.0 * juce::MathConstants<double>::pi * freqHz / sampleRate;
        const double cosw = std::cos (w0);
        const double sinw = std::sin (w0);
        const double alpha = sinw / (2.0 * q);
        const double sqrtA = std::sqrt (a);
        normalise (a * ((a + 1.0) + (a - 1.0) * cosw + 2.0 * sqrtA * alpha),
                   -2.0 * a * ((a - 1.0) + (a + 1.0) * cosw),
                   a * ((a + 1.0) + (a - 1.0) * cosw - 2.0 * sqrtA * alpha),
                   (a + 1.0) - (a - 1.0) * cosw + 2.0 * sqrtA * alpha,
                   2.0 * ((a - 1.0) - (a + 1.0) * cosw),
                   (a + 1.0) - (a - 1.0) * cosw - 2.0 * sqrtA * alpha);
        reset();
    }

    void setHighPass (double sampleRate, double freqHz, double q)
    {
        const double w0 = 2.0 * juce::MathConstants<double>::pi * freqHz / sampleRate;
        const double cosw = std::cos (w0);
        const double sinw = std::sin (w0);
        const double alpha = sinw / (2.0 * q);
        const double b0n = (1.0 + cosw) * 0.5;
        normalise (b0n, -(1.0 + cosw), b0n, 1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
        reset();
    }

    double process (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

//==============================================================================
/** K-weighting: +4 dB high shelf at ~1682 Hz followed by a ~38 Hz high pass. */
struct KWeighting
{
    Biquad shelf, highPass;

    void prepare (double sampleRate)
    {
        shelf.setHighShelf (sampleRate, 1681.97, 0.7072, 3.999);
        highPass.setHighPass (sampleRate, 38.135, 0.5003);
    }

    double process (double x) { return highPass.process (shelf.process (x)); }
    void reset() { shelf.reset(); highPass.reset(); }
};

//==============================================================================
struct LoudnessResult
{
    double integratedLufs = kFloorDb;
    double loudnessRangeLu = 0.0;
};

double loudnessOfPower (double combinedMeanSquare)
{
    if (combinedMeanSquare <= 0.0)
        return kFloorDb;
    return kLufsOffset + 10.0 * std::log10 (combinedMeanSquare);
}

LoudnessResult measureLoudness (const juce::AudioBuffer<double>& buffer, double sampleRate)
{
    LoudnessResult result;
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jlimit (1, 2, buffer.getNumChannels());
    if (numSamples <= 0 || sampleRate <= 0.0)
        return result;

    // K-weight each channel once; gating blocks are then cheap sliding sums.
    std::vector<std::vector<double>> weighted ((size_t) numChannels);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        weighted[(size_t) ch].resize ((size_t) numSamples);
        KWeighting k;
        k.prepare (sampleRate);
        const auto* src = buffer.getReadPointer (ch);
        auto& dst = weighted[(size_t) ch];
        for (int i = 0; i < numSamples; ++i)
            dst[(size_t) i] = k.process (src[i]);
    }

    auto combinedMeanSquare = [&] (int start, int length) {
        double sum = 0.0;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto& data = weighted[(size_t) ch];
            double channelSum = 0.0;
            for (int i = 0; i < length; ++i)
            {
                const double v = data[(size_t) (start + i)];
                channelSum += v * v;
            }
            sum += channelSum / (double) length;
        }
        return sum;
    };

    // --- Integrated loudness: 400 ms blocks, 100 ms hop, two-stage gating.
    const int blockLength = juce::jmax (1, (int) std::llround (0.4 * sampleRate));
    const int blockHop = juce::jmax (1, (int) std::llround (0.1 * sampleRate));

    std::vector<double> blockPowers;
    if (numSamples < blockLength)
        blockPowers.push_back (combinedMeanSquare (0, numSamples));
    else
        for (int start = 0; start + blockLength <= numSamples; start += blockHop)
            blockPowers.push_back (combinedMeanSquare (start, blockLength));

    if (! blockPowers.empty())
    {
        double absoluteSum = 0.0;
        int absoluteCount = 0;
        for (double p : blockPowers)
        {
            if (loudnessOfPower (p) > kGateAbsoluteLufs)
            {
                absoluteSum += p;
                ++absoluteCount;
            }
        }

        if (absoluteCount > 0)
        {
            const double absoluteMean = absoluteSum / (double) absoluteCount;
            const double relativeGate = loudnessOfPower (absoluteMean) + kGateRelativeLu;

            double gatedSum = 0.0;
            int gatedCount = 0;
            for (double p : blockPowers)
            {
                const double l = loudnessOfPower (p);
                if (l > kGateAbsoluteLufs && l >= relativeGate)
                {
                    gatedSum += p;
                    ++gatedCount;
                }
            }
            result.integratedLufs = gatedCount > 0 ? loudnessOfPower (gatedSum / (double) gatedCount)
                                                   : loudnessOfPower (absoluteMean);
        }
    }

    // --- Loudness range proxy: 3 s short-term windows, 1 s hop, above -70 LUFS.
    const int shortLength = juce::jmax (1, (int) std::llround (3.0 * sampleRate));
    const int shortHop = juce::jmax (1, (int) std::llround (1.0 * sampleRate));
    std::vector<double> shortTerm;
    if (numSamples < shortLength)
    {
        const double l = loudnessOfPower (combinedMeanSquare (0, numSamples));
        if (l > kGateAbsoluteLufs)
            shortTerm.push_back (l);
    }
    else
    {
        for (int start = 0; start + shortLength <= numSamples; start += shortHop)
        {
            const double l = loudnessOfPower (combinedMeanSquare (start, shortLength));
            if (l > kGateAbsoluteLufs)
                shortTerm.push_back (l);
        }
    }

    if (shortTerm.size() >= 2)
    {
        std::sort (shortTerm.begin(), shortTerm.end());
        const auto percentile = [&] (double fraction) {
            const size_t index = (size_t) juce::jlimit<int> (0, (int) shortTerm.size() - 1,
                                                             (int) std::llround (fraction * (double) (shortTerm.size() - 1)));
            return shortTerm[index];
        };
        result.loudnessRangeLu = juce::jmax (0.0, percentile (0.95) - percentile (0.10));
    }

    return result;
}

//==============================================================================
double measureTruePeakDb (const juce::AudioBuffer<double>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
        return kFloorDb;

    const int channels = juce::jmin (numChannels, 2);
    juce::dsp::Oversampling<float> oversampling ((size_t) channels, 2,
                                                 juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
                                                 true, true);
    oversampling.initProcessing ((size_t) kTruePeakBlock);

    juce::AudioBuffer<float> block (channels, kTruePeakBlock);
    double peak = 0.0;

    for (int start = 0; start < numSamples; start += kTruePeakBlock)
    {
        const int length = juce::jmin (kTruePeakBlock, numSamples - start);
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* dst = block.getWritePointer (ch);
            const auto* src = buffer.getReadPointer (ch, start);
            for (int i = 0; i < length; ++i)
                dst[i] = (float) src[i];
            if (length < kTruePeakBlock)
                juce::FloatVectorOperations::clear (dst + length, kTruePeakBlock - length);
        }

        juce::dsp::AudioBlock<float> inputBlock (block.getArrayOfWritePointers(),
                                                 (size_t) channels, (size_t) kTruePeakBlock);
        auto oversampled = oversampling.processSamplesUp (inputBlock);
        for (size_t ch = 0; ch < oversampled.getNumChannels(); ++ch)
        {
            const auto* data = oversampled.getChannelPointer (ch);
            const auto samples = oversampled.getNumSamples();
            for (size_t i = 0; i < samples; ++i)
                peak = juce::jmax (peak, std::abs ((double) data[i]));
        }
    }

    return linearToDb (peak);
}

//==============================================================================
struct SpectralMetrics
{
    double centroidHz = 0.0;
    double subRatio = 0.0;
    double lowRatio = 0.0;
    double midRatio = 0.0;
    double highRatio = 0.0;
    double lowBandDb = kFloorDb;
    double midBandDb = kFloorDb;
    double highBandDb = kFloorDb;
};

SpectralMetrics measureSpectrum (const juce::AudioBuffer<double>& buffer, double sampleRate)
{
    SpectralMetrics result;
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0 || sampleRate <= 0.0)
        return result;

    // Mono sum for the spectrum.
    std::vector<double> mono ((size_t) numSamples);
    for (int i = 0; i < numSamples; ++i)
    {
        double sum = 0.0;
        for (int ch = 0; ch < numChannels; ++ch)
            sum += buffer.getSample (ch, i);
        mono[(size_t) i] = sum / (double) numChannels;
    }

    // Periodic Hann window and its summed square (for the power calibration).
    std::vector<double> window ((size_t) kFftSize);
    double windowPower = 0.0;
    for (int i = 0; i < kFftSize; ++i)
    {
        window[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * (double) i / (double) kFftSize);
        windowPower += window[(size_t) i] * window[(size_t) i];
    }

    juce::dsp::FFT fft (kFftOrder);
    std::vector<float> fftData ((size_t) (2 * kFftSize), 0.0f);
    std::vector<double> accumulatedPower ((size_t) kNumBands, 0.0);
    int frames = 0;

    const int hop = kFftSize / 2;
    for (int start = 0; start < numSamples; start += hop)
    {
        const int length = juce::jmin (kFftSize, numSamples - start);
        for (int i = 0; i < length; ++i)
            fftData[(size_t) i] = (float) (mono[(size_t) (start + i)] * window[(size_t) i]);
        for (int i = length; i < kFftSize; ++i)
            fftData[(size_t) i] = 0.0f;

        fft.performRealOnlyForwardTransform (fftData.data(), true);

        for (int k = 0; k < kNumBands; ++k)
        {
            const double re = (double) fftData[(size_t) (2 * k)];
            const double im = (double) fftData[(size_t) (2 * k + 1)];
            const double singleSided = (k == 0 || k == kFftSize / 2) ? 1.0 : 2.0;
            accumulatedPower[(size_t) k] += (re * re + im * im) * singleSided;
        }
        ++frames;
    }

    if (frames == 0)
        return result;

    // Per-bin mean square, calibrated so the sum equals the signal mean square.
    const double binNormalisation = 1.0 / ((double) frames * (double) kFftSize * windowPower);
    const double binHz = sampleRate / (double) kFftSize;

    auto bandPower = [&] (double lowHz, double highHz) {
        double sum = 0.0;
        for (int k = 1; k < kNumBands; ++k)
        {
            const double hz = (double) k * binHz;
            if (hz >= lowHz && hz < highHz)
                sum += accumulatedPower[(size_t) k] * binNormalisation;
        }
        return sum;
    };

    const double subPower = bandPower (20.0, 60.0);
    const double lowPower = bandPower (20.0, 120.0);          // includes sub
    const double midPower = bandPower (120.0, 2000.0);
    const double highPower = bandPower (6000.0, 16000.0);
    const double totalPower = bandPower (20.0, 16000.0);

    if (totalPower > 0.0)
    {
        result.subRatio = subPower / totalPower;
        result.lowRatio = lowPower / totalPower;
        result.midRatio = midPower / totalPower;
        result.highRatio = highPower / totalPower;

        double weightedFrequency = 0.0;
        double powerSum = 0.0;
        for (int k = 1; k < kNumBands; ++k)
        {
            const double hz = (double) k * binHz;
            if (hz < 20.0 || hz >= 16000.0)
                continue;
            const double p = accumulatedPower[(size_t) k] * binNormalisation;
            weightedFrequency += hz * p;
            powerSum += p;
        }
        result.centroidHz = powerSum > 0.0 ? weightedFrequency / powerSum : 0.0;
    }

    result.lowBandDb = powerToDb (lowPower);
    result.midBandDb = powerToDb (midPower);
    result.highBandDb = powerToDb (highPower);
    return result;
}

//==============================================================================
struct DecodedAudio
{
    juce::AudioBuffer<double> buffer;
    double sampleRate = 0.0;
    juce::String decoderNote;
};

bool probeFile (const juce::File& file, double& sampleRateOut, double& declaredSecondsOut,
                int& sourceChannelsOut)
{
    sampleRateOut = 0.0;
    declaredSecondsOut = 0.0;
    sourceChannelsOut = 0;

    // Prefer the format-agnostic decoder probe (handles DSD and platform formats).
    {
        AudioFileInfo info;
        juce::String error;
        if (DecodeSource::probeInfo (file, info, error) && info.sampleRate > 0)
        {
            sampleRateOut = (double) info.sampleRate;
            sourceChannelsOut = info.numChannels;
            if (info.durationMs > 0)
                declaredSecondsOut = (double) info.durationMs / 1000.0;
            else if (info.totalFrames > 0)
                declaredSecondsOut = (double) info.totalFrames / (double) info.sampleRate;
            return true;
        }
    }

    // Fall back to a JUCE reader probe (WAV/AIFF/FLAC/MP3/OGG).
    juce::AudioFormatManager manager;
    manager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (manager.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0)
        return false;

    sampleRateOut = reader->sampleRate;
    sourceChannelsOut = (int) reader->numChannels;
    if (reader->lengthInSamples > 0)
        declaredSecondsOut = (double) reader->lengthInSamples / reader->sampleRate;
    return true;
}

/** Detects a truncated uncompressed PCM file: the header declares N frames but
    the file cannot possibly contain them. Container decoders happily zero-fill
    the missing region, which would otherwise analyse as a mostly-silent file. */
bool detectPcmTruncation (const juce::File& file, juce::String& errorOut)
{
    juce::AudioFormatManager manager;
    manager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (manager.createReaderFor (file));
    if (reader == nullptr || reader->bitsPerSample <= 0 || reader->lengthInSamples <= 0)
        return false;

    // JUCE format names are human-readable ("WAV file", "AIFF file", ...).
    const auto format = reader->getFormatName().toLowerCase();
    const bool uncompressed = format.contains ("wav") || format.contains ("aiff")
                           || format.contains ("aif") || format.contains ("caf")
                           || format.contains ("w64") || format.contains ("rf64");
    if (! uncompressed)
        return false;

    const std::int64_t bytesPerFrame = ((std::int64_t) ((reader->bitsPerSample + 7) / 8))
                                     * (std::int64_t) reader->numChannels;
    const std::int64_t expectedBytes = (std::int64_t) reader->lengthInSamples * bytesPerFrame;
    if (bytesPerFrame <= 0 || expectedBytes <= 0)
        return false;

    if (file.getSize() < expectedBytes)
    {
        errorOut = "File appears truncated: its header declares "
                 + juce::String (expectedBytes) + " bytes of uncompressed audio but the file is only "
                 + juce::String (file.getSize()) + " bytes.";
        return true;
    }
    return false;
}

bool decodeViaAudioEngine (const juce::File& file, int seconds, DecodedAudio& out, juce::String& errorOut)
{
    juce::AudioBuffer<double> buffer;
    juce::String error;
    if (! AudioEngine::decodeFileToBuffer (file, buffer, seconds, error))
    {
        errorOut = error.isNotEmpty() ? error : "the audio engine decoder failed";
        return false;
    }
    if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0)
    {
        errorOut = "the audio engine returned an empty buffer";
        return false;
    }
    out.buffer = std::move (buffer);
    out.decoderNote = "audio-engine";
    return true;
}

bool decodeViaDecodeSource (const juce::File& file, int seconds, DecodedAudio& out, juce::String& errorOut)
{
    juce::String error;
    auto source = DecodeSource::create (file, error);
    if (source == nullptr)
    {
        errorOut = error.isNotEmpty() ? error : "DecodeSource could not open the file";
        return false;
    }

    const auto& info = source->getInfo();
    if (info.sampleRate <= 0)
    {
        errorOut = "the decoder reported an invalid sample rate";
        return false;
    }

    std::int64_t framesWanted = (std::int64_t) std::llround ((double) seconds * (double) info.sampleRate);
    if (info.totalFrames > 0)
        framesWanted = juce::jmin (framesWanted, info.totalFrames);
    framesWanted = juce::jmin (framesWanted, TrackAnalyzer::kMaxDecodedFrames);
    if (framesWanted <= 0)
    {
        errorOut = "the file contains no audio frames";
        return false;
    }

    source->seekToMs (0);

    juce::AudioBuffer<double> buffer (2, (int) framesWanted);
    buffer.clear();
    constexpr int kChunkFrames = 8192;
    std::vector<double> interleaved ((size_t) kChunkFrames * 2, 0.0);
    std::int64_t framesRead = 0;

    while (framesRead < framesWanted)
    {
        const int request = (int) juce::jmin<std::int64_t> ((std::int64_t) kChunkFrames, framesWanted - framesRead);
        const int got = source->readFrames (interleaved.data(), request);
        if (got <= 0)
            break;
        for (int i = 0; i < got; ++i)
        {
            buffer.setSample (0, (int) (framesRead + i), interleaved[(size_t) i * 2]);
            buffer.setSample (1, (int) (framesRead + i), interleaved[(size_t) i * 2 + 1]);
        }
        framesRead += got;
    }

    if (framesRead <= 0)
    {
        errorOut = "the decoder produced no audio frames (file may be truncated or corrupt)";
        return false;
    }

    buffer.setSize (2, (int) framesRead, true, true, true);
    out.buffer = std::move (buffer);
    out.sampleRate = (double) info.sampleRate;
    out.decoderNote = "decode-source";
    return true;
}

bool decodeViaJuce (const juce::File& file, int seconds, DecodedAudio& out, juce::String& errorOut)
{
    juce::AudioFormatManager manager;
    manager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (manager.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0)
    {
        errorOut = "no decoder could open the file";
        return false;
    }

    const double sampleRate = reader->sampleRate;
    std::int64_t available = reader->lengthInSamples;
    if (available <= 0)
        available = (std::int64_t) std::llround ((double) seconds * sampleRate);

    std::int64_t frames = juce::jmin<std::int64_t> ((std::int64_t) std::llround ((double) seconds * sampleRate),
                                                   available);
    frames = juce::jmin (frames, TrackAnalyzer::kMaxDecodedFrames);
    if (frames <= 0)
    {
        errorOut = "the file contains no audio frames";
        return false;
    }

    const int channels = juce::jmax (1, juce::jmin (2, (int) reader->numChannels));
    juce::AudioBuffer<float> floatBuffer (juce::jmax (2, channels), (int) frames);
    floatBuffer.clear();

    if (! reader->read (&floatBuffer, 0, (int) frames, 0, true, channels > 1))
    {
        errorOut = "the decoder reported a read error (the file may be truncated or corrupt)";
        return false;
    }

    out.buffer.setSize (channels, (int) frames);
    for (int ch = 0; ch < channels; ++ch)
    {
        const auto* src = floatBuffer.getReadPointer (ch);
        auto* dst = out.buffer.getWritePointer (ch);
        for (int i = 0; i < (int) frames; ++i)
            dst[i] = (double) src[i];
    }
    out.sampleRate = sampleRate;
    out.decoderNote = "juce-offline";
    return true;
}

} // namespace

//==============================================================================
bool TrackAnalyzer::analyzeBuffer (const juce::AudioBuffer<double>& buffer,
                                   double sampleRate,
                                   TrackAnalysis& out)
{
    out = {};
    if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0 || sampleRate <= 0.0)
        return false;

    out.valid = true;
    out.version = kAnalyzerVersion;
    out.analyzer = "sp-analyzer/1";
    out.analyzedSeconds = (double) buffer.getNumSamples() / sampleRate;

    // --- Peaks and RMS / crest factor.
    double samplePeak = 0.0;
    double sumSquares = 0.0;
    std::int64_t sampleCount = 0;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const auto* data = buffer.getReadPointer (ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const double v = data[i];
            samplePeak = juce::jmax (samplePeak, std::abs (v));
            sumSquares += v * v;
            ++sampleCount;
        }
    }
    const double meanSquare = sampleCount > 0 ? sumSquares / (double) sampleCount : 0.0;

    out.samplePeakDb = linearToDb (samplePeak);
    out.truePeakDb = measureTruePeakDb (buffer);
    out.crestFactorDb = juce::jmax (0.0, out.samplePeakDb - powerToDb (meanSquare));

    // --- Loudness / dynamics.
    const auto loudness = measureLoudness (buffer, sampleRate);
    out.integratedLufs = loudness.integratedLufs;
    out.loudnessRangeLu = loudness.loudnessRangeLu;

    // --- Spectrum.
    const auto spectrum = measureSpectrum (buffer, sampleRate);
    out.spectralCentroidHz = spectrum.centroidHz;
    out.subEnergyRatio = spectrum.subRatio;
    out.lowEnergyRatio = spectrum.lowRatio;
    out.midEnergyRatio = spectrum.midRatio;
    out.highEnergyRatio = spectrum.highRatio;
    out.lowBandDb = spectrum.lowBandDb;
    out.midBandDb = spectrum.midBandDb;
    out.highBandDb = spectrum.highBandDb;

    return true;
}

//==============================================================================
bool TrackAnalyzer::analyzeFile (const juce::File& file,
                                 TrackAnalysis& out,
                                 int maxSeconds,
                                 juce::String& errorOut)
{
    out = {};
    errorOut.clear();

    if (! file.existsAsFile())
    {
        errorOut = "File not found: " + file.getFullPathName();
        return false;
    }

    const int requestedSeconds = maxSeconds <= 0
                               ? kMaxAnalysisSeconds
                               : juce::jlimit (1, kMaxAnalysisSeconds, maxSeconds);

    if (detectPcmTruncation (file, errorOut))
        return false;

    double probedRate = 0.0;
    double declaredSeconds = 0.0;
    int sourceChannels = 0;
    probeFile (file, probedRate, declaredSeconds, sourceChannels);

    // Very high sample-rate sources are decoded for a shorter wall-clock window
    // so the decoded PCM stays inside a predictable memory budget.
    int decodeSeconds = requestedSeconds;
    if (probedRate > 0.0)
    {
        const int budgetSeconds = (int) std::floor ((double) kMaxDecodedFrames / probedRate);
        decodeSeconds = juce::jlimit (1, requestedSeconds, juce::jmax (2, budgetSeconds));
    }

    DecodedAudio decoded;
    juce::String engineError, sourceError, juceError;
    bool decodedOk = false;

    if (decodeViaAudioEngine (file, decodeSeconds, decoded, engineError))
    {
        decodedOk = true;
        if (decoded.sampleRate <= 0.0)
            decoded.sampleRate = probedRate;
    }
    else if (decodeViaDecodeSource (file, decodeSeconds, decoded, sourceError))
    {
        decodedOk = true;
    }
    else if (decodeViaJuce (file, decodeSeconds, decoded, juceError))
    {
        decodedOk = true;
        if (decoded.sampleRate <= 0.0)
            decoded.sampleRate = probedRate;
    }

    if (! decodedOk)
    {
        errorOut = "Could not decode \"" + file.getFileName() + "\": "
                 + engineError + "; " + sourceError + "; " + juceError;
        return false;
    }

    if (decoded.sampleRate <= 0.0)
    {
        errorOut = "Decoded the file but could not determine its sample rate, so analysis is not possible.";
        return false;
    }

    // DecodeSource delivers stereo by contract and some decoders upmix mono to
    // dual-mono. Measure a mono source as mono so loudness/crest results are
    // identical whichever decode path produced them (EBU R128 measures a mono
    // file on its single channel; dual-mono would read +3 dB louder).
    if (sourceChannels == 1 && decoded.buffer.getNumChannels() > 1)
        decoded.buffer.setSize (1, decoded.buffer.getNumSamples(), true, false, false);

    // Truncated files: if we asked for the whole stream but got materially less
    // audio back, report an error instead of silently analysing an incomplete file.
    if (declaredSeconds > 0.25 && requestedSeconds >= (int) std::floor (declaredSeconds))
    {
        const double gotSeconds = (double) decoded.buffer.getNumSamples() / decoded.sampleRate;
        if (gotSeconds < declaredSeconds * 0.90)
        {
            errorOut = "File appears truncated: decoded " + juce::String (gotSeconds, 2)
                     + " s of a " + juce::String (declaredSeconds, 2) + " s stream.";
            return false;
        }
    }

    if (! analyzeBuffer (decoded.buffer, decoded.sampleRate, out))
    {
        errorOut = "The decoded audio was empty, so it could not be analysed.";
        out = {};
        return false;
    }

    out.analyzer = "sp-analyzer/1 (" + decoded.decoderNote + ")";
    return true;
}

} // namespace sp
