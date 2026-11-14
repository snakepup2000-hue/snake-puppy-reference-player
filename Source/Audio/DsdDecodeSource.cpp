/*
    Snake Puppy Reference Player
    Native DSD decoder for DSF (Sony) and DFF (Philips DSDIFF) containers.

    DSD64/128/256 are supported. The 1-bit stream is decoded with a two-stage
    polyphase FIR decimator (8x then 2x, Kaiser-windowed sinc, ~100 dB
    stopband) to PCM at dsdRate/16:
        DSD64  (2.8224 MHz) -> 176.4 kHz
        DSD128 (5.6448 MHz) -> 352.8 kHz
        DSD256 (11.2896 MHz)-> 705.6 kHz
    A -6 dB default gain is applied to the PCM output (dsdGainDb in AudioFileInfo)
    to restore the headroom that DSD's noise-shaping assumes.

    DSF bit layout: 1-bit samples are LSB-first, stored as per-channel blocks of
    blockSizePerChannel bytes interleaved channel by channel.
    DFF bit layout: 1-bit samples are MSB-first, interleaved channel by channel
    per sample frame.

    Threading: single-threaded decode use (decode thread / test thread). Disk
    I/O only; never call from the audio callback.
*/
#include "Audio/DecodeBackends.h"
#include "Audio/DecodeUtils.h"
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace sp
{

namespace
{
constexpr double kPi = 3.14159265358979323846;

double besselI0 (double x)
{
    double sum = 1.0;
    double term = 1.0;
    const double halfX = x * 0.5;

    for (int k = 1; k < 64; ++k)
    {
        term *= (halfX / (double) k) * (halfX / (double) k);
        sum += term;
        if (term < sum * 1.0e-12)
            break;
    }
    return sum;
}

std::vector<double> designLowPass (double passHz, double stopHz, double sampleRate,
                                   double attenuationDb, double& cutoffOut)
{
    passHz = juce::jlimit (1.0, sampleRate * 0.49, passHz);
    stopHz = juce::jlimit (passHz + 1.0, sampleRate * 0.5, stopHz);

    const double cutoff = 0.5 * (passHz + stopHz);
    cutoffOut = cutoff;

    const double deltaF = (stopHz - passHz) / sampleRate;
    const double beta = 0.1102 * (attenuationDb - 8.7);

    int numTaps = (int) std::ceil ((attenuationDb - 8.0) / (2.285 * 2.0 * kPi * juce::jmax (1.0e-6, deltaF)));
    numTaps = juce::jlimit (31, 1023, numTaps);
    if ((numTaps % 2) == 0)
        ++numTaps;

    const int middle = numTaps / 2;
    std::vector<double> taps ((size_t) numTaps, 0.0);
    double sum = 0.0;

    for (int i = 0; i < numTaps; ++i)
    {
        const double n = (double) (i - middle);
        const double sinc = n == 0.0 ? 2.0 * cutoff / sampleRate
                                     : std::sin (2.0 * kPi * cutoff * n / sampleRate) / (kPi * n);
        const double x = (double) i / (double) (numTaps - 1);
        const double window = besselI0 (beta * std::sqrt (juce::jmax (0.0, 1.0 - std::pow (2.0 * x - 1.0, 2.0))))
                            / besselI0 (beta);
        taps[(size_t) i] = sinc * window;
        sum += taps[(size_t) i];
    }

    if (std::abs (sum) > 1.0e-12)
        for (auto& t : taps)
            t /= sum;

    return taps;
}

/** Polyphase FIR decimator. Coefficients are ordered newest-sample-first. */
class FirDecimator
{
public:
    void prepare (int decimationFactor, std::vector<double> coefficients, int channels)
    {
        factor = juce::jmax (1, decimationFactor);
        taps = std::move (coefficients);
        numChannels = juce::jmax (1, channels);
        history.assign ((size_t) numChannels, std::vector<double> (juce::jmax ((size_t) 1, taps.size()), 0.0));
        reset();
    }

    void reset() noexcept
    {
        for (auto& h : history)
            std::fill (h.begin(), h.end(), 0.0);
        histPos = 0;
        decimCount = 0;
    }

    int tapsCount() const noexcept { return (int) taps.size(); }

    /** Consumes all input frames; writes floor((decimCount + in) / factor)
        output frames. Returns output frame count. */
    int process (const double* const* in, int inFrames, double* const* out, int maxOutFrames) noexcept
    {
        if (inFrames <= 0 || maxOutFrames <= 0)
            return 0;

        const int tapCount = (int) taps.size();
        const int histSize = tapCount;
        int produced = 0;

        for (int n = 0; n < inFrames; ++n)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                history[(size_t) ch][(size_t) histPos] = in[ch][n];

            ++histPos;
            if (histPos >= histSize)
                histPos = 0;

            ++decimCount;
            if (decimCount < factor)
                continue;

            decimCount = 0;
            if (produced >= maxOutFrames)
                continue;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto& h = history[(size_t) ch];
                double acc = 0.0;
                int idx = histPos;   // newest sample position after increment/wrap

                for (int k = 0; k < tapCount; ++k)
                {
                    --idx;
                    if (idx < 0)
                        idx = histSize - 1;
                    acc += taps[(size_t) k] * h[(size_t) idx];
                }
                out[ch][produced] = acc;
            }

            ++produced;
        }

        return produced;
    }

private:
    int factor = 1;
    int numChannels = 2;
    std::vector<double> taps;
    std::vector<std::vector<double>> history;
    int histPos = 0;
    int decimCount = 0;
};

//==============================================================================
class DsdDecodeSource final : public DecodeSource
{
public:
    explicit DsdDecodeSource (const juce::File& file, juce::String& errorOut)
    {
        stream = std::make_unique<juce::FileInputStream> (file);
        if (stream == nullptr || stream->failedToOpen())
        {
            errorOut = "Could not open DSD file: " + file.getFullPathName();
            return;
        }

        fileSize = stream->getTotalLength();
        path = file.getFullPathName();

        const bool opened = parseHeader (errorOut);
        if (! opened)
            return;

        prepareFilters();
        prepareBuffers();

        pcmFifo.assign ((size_t) kFifoFrames * 2, 0.0);
        errorOut.clear();
    }

    ~DsdDecodeSource() override { close(); }

    const AudioFileInfo& getInfo() const override { return info; }

    bool seekToMs (std::int64_t ms) override
    {
        if (bitsPerChannel <= 0)
            return false;

        auto targetFrame = (ms * (std::int64_t) juce::jmax (1, info.sampleRate)) / 1000;
        targetFrame = juce::jlimit<std::int64_t> (0, info.totalFrames, targetFrame);

        const auto targetBit = targetFrame * 16;
        const auto warmupBits = (std::int64_t) (stageAFilter.tapsCount() + 8 * stageBFilter.tapsCount());
        const auto startBit = juce::jmax<std::int64_t> (0, targetBit - warmupBits);

        inputBitPos = startBit;
        stageAFilter.reset();
        stageBFilter.reset();
        fifoRead = 0;
        fifoWrite = 0;
        finished = false;
        lastError.clear();
        // Anything before the requested frame is filter warm-up: produce it and
        // throw it away so seeking never starts with a filter transient.
        pendingDiscardFrames = targetFrame - (startBit / 16);
        deliveryPositionFrames = targetFrame;
        return true;
    }

    int readFrames (double* interleavedStereo, int numFramesRequested) override
    {
        if (interleavedStereo == nullptr || numFramesRequested <= 0 || bitsPerChannel <= 0)
            return 0;

        int delivered = 0;

        while (delivered < numFramesRequested)
        {
            if (fifoRead == fifoWrite)
            {
                if (lastError.isNotEmpty())
                    break;
                if (! decodeNextChunk())
                    break;
                if (fifoRead == fifoWrite)
                    break;
            }

            while (fifoRead != fifoWrite && delivered < numFramesRequested)
            {
                if (pendingDiscardFrames > 0)
                {
                    --pendingDiscardFrames;
                    fifoRead = (fifoRead + 1) % kFifoFrames;
                    continue;
                }

                interleavedStereo[(size_t) delivered * 2] = pcmFifo[(size_t) fifoRead * 2];
                interleavedStereo[(size_t) delivered * 2 + 1] = pcmFifo[(size_t) fifoRead * 2 + 1];
                ++delivered;
                ++deliveryPositionFrames;
                fifoRead = (fifoRead + 1) % kFifoFrames;
            }
        }

        if (deliveryPositionFrames >= info.totalFrames)
            finished = true;

        return delivered;
    }

    std::int64_t positionMs() const override
    {
        if (info.sampleRate <= 0)
            return 0;
        return (deliveryPositionFrames * 1000) / info.sampleRate;
    }

    bool isFinished() const override { return finished || deliveryPositionFrames >= info.totalFrames; }
    juce::String getLastError() const override { return lastError; }

    void close() override
    {
        if (stream != nullptr)
            stream.reset();
    }

private:
    static constexpr int kInputChunkBits = 4096;
    static constexpr int kFifoFrames = 4096;

    //== Header parsing =======================================================
    bool readAt (std::int64_t offset, void* dest, int numBytes)
    {
        if (offset < 0 || ! stream->setPosition (offset))
            return false;
        return stream->read (dest, numBytes) == numBytes;
    }

    static std::uint32_t readU32BE (const unsigned char* p)
    {
        return ((std::uint32_t) p[0] << 24) | ((std::uint32_t) p[1] << 16) | ((std::uint32_t) p[2] << 8) | p[3];
    }

    static std::uint32_t readU32LE (const unsigned char* p)
    {
        return ((std::uint32_t) p[3] << 24) | ((std::uint32_t) p[2] << 16) | ((std::uint32_t) p[1] << 8) | p[0];
    }

    static std::uint64_t readU64BE (const unsigned char* p)
    {
        return ((std::uint64_t) readU32BE (p) << 32) | readU32BE (p + 4);
    }

    static std::uint64_t readU64LE (const unsigned char* p)
    {
        return ((std::uint64_t) readU32LE (p + 4) << 32) | readU32LE (p);
    }

    bool parseHeader (juce::String& errorOut)
    {
        unsigned char head[16] {};
        if (! readAt (0, head, 16))
        {
            errorOut = "Could not read the DSD header of " + path;
            return false;
        }

        if (std::memcmp (head, "DSD ", 4) == 0)
            return parseDsf (errorOut);
        if (std::memcmp (head, "FRM8", 4) == 0)
            return parseDff (errorOut);

        errorOut = "Not a DSD file (no DSF or DFF signature): " + path;
        return false;
    }

    bool finishHeader (juce::String& errorOut, bool isDsfContainer)
    {
        if (channels < 1 || channels > 2)
        {
            errorOut = "Unsupported DSD channel count " + juce::String (channels)
                     + " (only mono and stereo DSD are supported)";
            return false;
        }
        if (bitsPerSample != 1)
        {
            errorOut = "Unsupported DSD sample size " + juce::String (bitsPerSample) + " (expected 1 bit)";
            return false;
        }
        if (dsdRate <= 44100 * 32)
        {
            errorOut = "Unsupported DSD rate " + juce::String (dsdRate) + " Hz";
            return false;
        }

        const double multiple = (double) dsdRate / 44100.0;
        const int rounded = juce::roundToInt (multiple);
        if (std::abs (multiple - (double) rounded) > 0.01 || (rounded != 64 && rounded != 128 && rounded != 256))
        {
            errorOut = "Unsupported DSD rate " + juce::String (dsdRate) + " Hz (supported: DSD64/128/256)";
            return false;
        }

        dsdMultiple = rounded;
        const int pcmRate = (int) (dsdRate / 16);

        if (bitsPerChannel <= 0)
        {
            errorOut = "DSD header reports no audio samples";
            return false;
        }

        info.path = path;
        info.sampleRate = pcmRate;
        info.bitDepth = 1;
        info.numChannels = channels;
        info.totalFrames = bitsPerChannel / 16;
        info.durationMs = pcmRate > 0 ? (info.totalFrames * 1000) / pcmRate : 0;
        info.formatName = isDsfContainer ? "DSF" : "DFF";
        info.family = Track::Family::dsd;
        info.isDsd = true;
        info.dsdMultiple = dsdMultiple;
        info.dsdGainDb = -6.0f;
        info.decoderName = juce::String ("Native DSD (") + (isDsfContainer ? "DSF" : "DFF") + ", DSD"
                         + juce::String (dsdMultiple) + ", 16x FIR -> "
                         + juce::String (pcmRate) + " Hz)";

        // DSD usually has no ReplayGain, but some rippers write ID3 tags after
        // the data; the bounded scan is harmless.
        detail::parseReplayGainTags (juce::File (path), info);

        if (info.totalFrames <= 0)
        {
            errorOut = "DSD file contains no complete PCM frames";
            return false;
        }

        return true;
    }

    bool parseDsf (juce::String& errorOut)
    {
        isDsf = true;
        unsigned char header[28] {};
        if (! readAt (0, header, 28))
        {
            errorOut = "Truncated DSF header";
            return false;
        }

        const auto headerSize = readU64LE (header + 4);
        const auto declaredFileSize = readU64LE (header + 12);
        if (headerSize != 28)
        {
            errorOut = "Unsupported DSF header size " + juce::String ((juce::int64) headerSize);
            return false;
        }
        if (declaredFileSize > (std::uint64_t) fileSize + 4096)
        {
            errorOut = "Corrupt DSF header: declared file size " + juce::String ((juce::int64) declaredFileSize)
                     + " exceeds the file size " + juce::String ((juce::int64) fileSize);
            return false;
        }

        std::int64_t pos = 28;
        bool haveFmt = false;
        bool haveData = false;

        while (pos + 12 <= fileSize)
        {
            unsigned char chunk[12] {};
            if (! readAt (pos, chunk, 12))
                break;

            const auto id = std::string ((const char*) chunk, 4);
            const auto chunkSize = (std::int64_t) readU64LE (chunk + 4);

            if (chunkSize < 12 || pos + chunkSize > fileSize + 12)
            {
                errorOut = "Corrupt DSF chunk '" + juce::String (id) + "' at offset " + juce::String (pos);
                return false;
            }

            if (id == "fmt ")
            {
                unsigned char body[40] {};
                if (chunkSize < 52 || ! readAt (pos + 12, body, 40))
                {
                    errorOut = "Truncated DSF fmt chunk";
                    return false;
                }

                const auto formatVersion = readU32LE (body + 0);
                const auto formatId = readU32LE (body + 4);
                channels = (int) readU32LE (body + 12);
                dsdRate = (std::int64_t) readU32LE (body + 16);
                bitsPerSample = (int) readU32LE (body + 20);
                bitsPerChannel = (std::int64_t) readU64LE (body + 24);
                blockSizePerChannel = (int) readU32LE (body + 32);

                if (formatVersion != 1)
                {
                    errorOut = "Unsupported DSF format version " + juce::String (formatVersion);
                    return false;
                }
                if (formatId != 0)
                {
                    errorOut = "Unsupported DSF format id " + juce::String (formatId) + " (only raw DSD)";
                    return false;
                }
                if (blockSizePerChannel <= 0 || blockSizePerChannel > (1 << 20))
                {
                    errorOut = "Invalid DSF block size " + juce::String (blockSizePerChannel);
                    return false;
                }
                haveFmt = true;
            }
            else if (id == "data")
            {
                dataOffset = pos + 12;
                dataBytes = juce::jmin (chunkSize - 12, fileSize - dataOffset);
                haveData = true;
            }

            pos += chunkSize;
        }

        if (! haveFmt)
        {
            errorOut = "DSF file has no fmt chunk";
            return false;
        }
        if (! haveData)
        {
            errorOut = "DSF file has no data chunk";
            return false;
        }

        return finishHeader (errorOut, true);
    }

    /** DSDIFF chunk sizes are 64-bit big-endian. Some writers (including the
        ffmpeg-derived fixtures) declare a fmt chunk size that includes its own
        header, so we locate the fmt fields and the DSD data chunk by scanning
        for their IDs instead of trusting a single size field. */
    bool parseDff (juce::String& errorOut)
    {
        isDsf = false;
        const auto fmtPos = findAscii ("fmt ", 16, 1024 * 1024);
        if (fmtPos < 0)
        {
            errorOut = "DFF file has no fmt chunk";
            return false;
        }

        unsigned char fmt[64] {};
        if (! readAt (fmtPos + 12, fmt, 48))
        {
            errorOut = "Truncated DFF fmt chunk";
            return false;
        }

        const auto formatVersion = readU32BE (fmt + 0);
        const auto formatId = readU32BE (fmt + 4);
        channels = (int) readU32BE (fmt + 12);
        dsdRate = (std::int64_t) readU32BE (fmt + 16);
        bitsPerSample = (int) readU32BE (fmt + 20);
        bitsPerChannel = (std::int64_t) readU64BE (fmt + 24);
        blockSizePerChannel = (int) readU32BE (fmt + 32);

        if (formatVersion != 1)
        {
            errorOut = "Unsupported DFF format version " + juce::String (formatVersion);
            return false;
        }
        if (formatId != 0)
        {
            errorOut = "Unsupported DFF format id " + juce::String (formatId) + " (only raw DSD)";
            return false;
        }

        const auto dstPos = findAscii ("DST ", fmtPos + 8, 4 * 1024 * 1024);
        const auto dsdPos = findAscii ("DSD ", fmtPos + 8, 4 * 1024 * 1024);

        if (dsdPos < 0 || (dstPos >= 0 && dstPos < dsdPos))
        {
            errorOut = "DST-compressed DFF is not supported (decode to DSDIFF DSD first)";
            return false;
        }

        unsigned char chunk[12] {};
        if (! readAt (dsdPos, chunk, 12))
        {
            errorOut = "Truncated DFF DSD chunk header";
            return false;
        }

        const auto dataSize = (std::int64_t) readU64BE (chunk + 4);
        dataOffset = dsdPos + 12;
        if (dataSize <= 0)
        {
            errorOut = "DFF DSD chunk declares no data";
            return false;
        }

        dataBytes = juce::jmin (dataSize, fileSize - dataOffset);
        if (dataBytes < dataSize)
        {
            // Truncated file: allow probing but be honest about it.
            truncated = true;
        }

        return finishHeader (errorOut, false);
    }

    std::int64_t findAscii (const char* needle, std::int64_t start, std::int64_t maxScan)
    {
        const int needleLen = 4;
        const std::int64_t end = juce::jmin (fileSize, start + maxScan);
        std::vector<unsigned char> buffer (64 * 1024);
        std::int64_t pos = start;

        while (pos + needleLen <= end)
        {
            const int toRead = (int) juce::jmin<std::int64_t> ((std::int64_t) buffer.size(), end - pos);
            if (toRead < needleLen)
                break;
            if (! readAt (pos, buffer.data(), toRead))
                break;

            for (int i = 0; i + needleLen <= toRead; ++i)
                if (std::memcmp (buffer.data() + i, needle, (size_t) needleLen) == 0)
                    return pos + i;

            pos += (std::int64_t) toRead - (needleLen - 1);
        }

        return -1;
    }

    //== Bit reading ==========================================================
    /** Fills left/right with up to n 1-bit samples (+1/-1). Returns samples
        written (less than n only at end of stream). */
    int readInputBits (double* left, double* right, int n)
    {
        return isDsf ? readDsfBits (left, right, n) : readDffBits (left, right, n);
    }

    int readDsfBits (double* left, double* right, int n)
    {
        const int bitsPerBlock = blockSizePerChannel * 8;
        int produced = 0;

        while (produced < n && inputBitPos < bitsPerChannel)
        {
            const auto block = inputBitPos / bitsPerBlock;
            const int bitInBlock = (int) (inputBitPos % bitsPerBlock);
            int take = (int) juce::jmin<std::int64_t> ((std::int64_t) (n - produced),
                                                       juce::jmin<std::int64_t> ((std::int64_t) (bitsPerBlock - bitInBlock),
                                                                                 bitsPerChannel - inputBitPos));
            if (take <= 0)
                break;

            const int firstByte = bitInBlock >> 3;
            const int lastByte = (bitInBlock + take - 1) >> 3;
            const int numBytes = lastByte - firstByte + 1;

            for (int ch = 0; ch < 2; ++ch)
            {
                const int srcCh = juce::jmin (ch, channels - 1);
                const auto offset = dataOffset + (block * (std::int64_t) channels + srcCh) * (std::int64_t) blockSizePerChannel
                                  + firstByte;
                const int read = (int) juce::jmin<std::int64_t> ((std::int64_t) numBytes,
                                                                juce::jmax<std::int64_t> (0, fileSize - offset));
                unsigned char* buf = (ch == 0) ? byteBuffer0.data() : byteBuffer1.data();
                if (read <= 0 || ! readAt (offset, buf, read))
                {
                    lastError = "DSF data ended early at byte " + juce::String (offset)
                              + (truncated ? " (truncated file)" : "");
                    return produced;
                }

                for (int i = 0; i < take; ++i)
                {
                    const int g = bitInBlock + i;
                    const int byteIdx = (g >> 3) - firstByte;
                    const int bit = g & 7;
                    const double value = ((buf[byteIdx] >> bit) & 1) ? 1.0 : -1.0;
                    (ch == 0 ? left : right)[produced + i] = value;
                }
            }

            produced += take;
            inputBitPos += take;
        }

        return produced;
    }

    int readDffBits (double* left, double* right, int n)
    {
        int produced = 0;

        while (produced < n && inputBitPos < bitsPerChannel)
        {
            int take = (int) juce::jmin<std::int64_t> ((std::int64_t) (n - produced), bitsPerChannel - inputBitPos);
            take = juce::jmin (take, kInputChunkBits);
            if (take <= 0)
                break;

            const auto firstByte = inputBitPos >> 3;
            const auto lastByte = (inputBitPos + take - 1) >> 3;
            const int numBytes = (int) (lastByte - firstByte + 1);
            const int totalBytes = numBytes * channels;
            const auto offset = dataOffset + firstByte * (std::int64_t) channels;

            if (offset + totalBytes > fileSize)
            {
                lastError = "DFF data ended early at byte " + juce::String ((juce::int64) offset)
                          + (truncated ? " (truncated file)" : "");
                return produced;
            }

            if (! readAt (offset, byteBuffer0.data(), totalBytes))
            {
                lastError = "Could not read DFF data at byte " + juce::String ((juce::int64) offset);
                return produced;
            }

            const unsigned char* buf = byteBuffer0.data();
            for (int i = 0; i < take; ++i)
            {
                const auto globalBit = inputBitPos + i;
                const int byteIdx = (int) ((globalBit >> 3) - firstByte);
                const int bit = (int) (globalBit & 7);
                for (int ch = 0; ch < 2; ++ch)
                {
                    const int srcCh = juce::jmin (ch, channels - 1);
                    const double value = ((buf[byteIdx * channels + srcCh] >> (7 - bit)) & 1) ? 1.0 : -1.0;
                    (ch == 0 ? left : right)[produced + i] = value;
                }
            }

            produced += take;
            inputBitPos += take;
        }

        return produced;
    }

    //== Filtering ============================================================
    void prepareFilters()
    {
        const double pcmRate = (double) info.sampleRate;
        const double audioPass = 0.45 * pcmRate;
        const double stageAInput = (double) dsdRate;
        const double stageAOutput = 2.0 * pcmRate;

        double cutoffA = 0.0;
        auto tapsA = designLowPass (audioPass, stageAOutput - audioPass, stageAInput, 100.0, cutoffA);
        stageAFilter.prepare (8, std::move (tapsA), 2);

        double cutoffB = 0.0;
        auto tapsB = designLowPass (audioPass, pcmRate - audioPass, stageAOutput, 100.0, cutoffB);
        stageBFilter.prepare (2, std::move (tapsB), 2);
    }

    void prepareBuffers()
    {
        const int midCapacity = kInputChunkBits / 8 + 8;
        const int outCapacity = midCapacity / 2 + 8;

        inputL.assign ((size_t) kInputChunkBits, 0.0);
        inputR.assign ((size_t) kInputChunkBits, 0.0);
        midL.assign ((size_t) midCapacity, 0.0);
        midR.assign ((size_t) midCapacity, 0.0);
        outL.assign ((size_t) outCapacity, 0.0);
        outR.assign ((size_t) outCapacity, 0.0);

        inPtrs[0] = inputL.data();
        inPtrs[1] = inputR.data();
        midPtrs[0] = midL.data();
        midPtrs[1] = midR.data();
        outPtrs[0] = outL.data();
        outPtrs[1] = outR.data();

        byteBuffer0.assign (64 * 1024, 0);
        byteBuffer1.assign (64 * 1024, 0);
    }

    bool decodeNextChunk()
    {
        if (inputBitPos >= bitsPerChannel)
        {
            finished = true;
            return false;
        }

        const int bits = (int) juce::jmin<std::int64_t> ((std::int64_t) kInputChunkBits, bitsPerChannel - inputBitPos);
        const int got = readInputBits (inputL.data(), inputR.data(), bits);

        if (got <= 0)
        {
            finished = true;
            return false;
        }

        const int midFrames = stageAFilter.process (inPtrs, got, midPtrs, (int) midL.size());
        const int outFrames = stageBFilter.process (midPtrs, midFrames, outPtrs, (int) outL.size());

        const float gain = std::pow (10.0f, info.dsdGainDb / 20.0f);

        for (int i = 0; i < outFrames; ++i)
        {
            const int next = (fifoWrite + 1) % kFifoFrames;
            if (next == fifoRead)
                break;  // FIFO full; caller will drain before asking again
            pcmFifo[(size_t) fifoWrite * 2] = (double) (outL[(size_t) i] * gain);
            pcmFifo[(size_t) fifoWrite * 2 + 1] = (double) (outR[(size_t) i] * gain);
            fifoWrite = next;
        }

        return true;
    }

    //== State ================================================================
    std::unique_ptr<juce::FileInputStream> stream;
    juce::String path;
    std::int64_t fileSize = 0;
    AudioFileInfo info;
    juce::String lastError;

    bool isDsf = false;
    bool truncated = false;
    int channels = 0;
    std::int64_t dsdRate = 0;
    int dsdMultiple = 0;
    int bitsPerSample = 0;
    std::int64_t bitsPerChannel = 0;
    int blockSizePerChannel = 0;
    std::int64_t dataOffset = 0;
    std::int64_t dataBytes = 0;

    FirDecimator stageAFilter;
    FirDecimator stageBFilter;

    std::vector<double> inputL, inputR, midL, midR, outL, outR;
    const double* inPtrs[2] { nullptr, nullptr };
    double* midPtrs[2] { nullptr, nullptr };
    double* outPtrs[2] { nullptr, nullptr };
    std::vector<unsigned char> byteBuffer0, byteBuffer1;

    std::vector<double> pcmFifo;
    int fifoRead = 0;
    int fifoWrite = 0;

    std::int64_t inputBitPos = 0;
    std::int64_t deliveryPositionFrames = 0;
    std::int64_t pendingDiscardFrames = 0;
    bool finished = false;
};

} // namespace

std::unique_ptr<DecodeSource> detail::createDsdDecodeSource (const juce::File& file, juce::String& errorOut)
{
    auto source = std::make_unique<DsdDecodeSource> (file, errorOut);
    if (errorOut.isNotEmpty())
        return nullptr;
    return source;
}

} // namespace sp
