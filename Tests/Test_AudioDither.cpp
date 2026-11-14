/*
    Snake Puppy Reference Player
    TPDF dither tests: noise magnitude in LSB units, no dither when not reducing
    depth, determinism and spectral flatness (no DC).
*/
#include "TestFramework.h"
#include "Audio/Dither.h"
#include <algorithm>
#include <cmath>

using namespace sp;

SP_TEST (AudioDither, TpdfNoiseHasExpectedLsbMagnitudeFor24To16)
{
    constexpr int frames = 200000;
    std::vector<double> left ((size_t) frames), right ((size_t) frames);

    const double dc = 0.25;
    for (auto& v : left)
        v = dc;
    for (auto& v : right)
        v = -dc;

    sp::TpdfDither dither;
    dither.prepare (0x12345678u);

    SP_REQUIRE (sp::TpdfDither::shouldApply (16, 24));
    SP_REQUIRE (! sp::TpdfDither::shouldApply (24, 24));
    SP_REQUIRE (! sp::TpdfDither::shouldApply (24, 16));

    const bool applied = dither.process (left.data(), right.data(), frames, 16, 24);
    SP_REQUIRE (applied);

    const double lsb = sp::TpdfDither::lsbForBits (16);   // 1 / 32768
    SP_REQUIRE_NEAR (lsb, 1.0 / 32768.0, 1.0e-12);

    double sumL = 0.0, sumSqL = 0.0, maxL = 0.0;
    double sumR = 0.0, sumSqR = 0.0, maxR = 0.0;

    for (int i = 0; i < frames; ++i)
    {
        const double nl = (left[(size_t) i] - dc) / lsb;
        const double nr = (right[(size_t) i] + dc) / lsb;
        sumL += nl;
        sumSqL += nl * nl;
        maxL = std::max (maxL, std::abs (nl));
        sumR += nr;
        sumSqR += nr * nr;
        maxR = std::max (maxR, std::abs (nr));
    }

    const double rmsL = std::sqrt (sumSqL / frames);
    const double rmsR = std::sqrt (sumSqR / frames);
    const double meanL = sumL / frames;
    const double meanR = sumR / frames;

    std::cout << "        24->16: rmsL=" << juce::String (rmsL, 4) << " LSB, rmsR="
              << juce::String (rmsR, 4) << " LSB, meanL=" << juce::String (meanL, 4)
              << " LSB, meanR=" << juce::String (meanR, 4) << " LSB, max="
              << juce::String (maxL, 4) << "/" << juce::String (maxR, 4) << " LSB\n";

    // TPDF from two independent uniforms has RMS = sqrt(2/12) = 0.4082 LSB.
    SP_REQUIRE_MSG (std::abs (rmsL - 0.4082) < 0.02, "left RMS " + juce::String (rmsL, 4));
    SP_REQUIRE_MSG (std::abs (rmsR - 0.4082) < 0.02, "right RMS " + juce::String (rmsR, 4));
    SP_REQUIRE_MSG (std::abs (meanL) < 0.02, "left mean " + juce::String (meanL, 4));
    SP_REQUIRE_MSG (std::abs (meanR) < 0.02, "right mean " + juce::String (meanR, 4));
    SP_REQUIRE_MSG (maxL < 1.05 && maxR < 1.05, "TPDF peak must stay within 1 LSB");
}

SP_TEST (AudioDither, NoDitherWhenDepthIsNotReduced)
{
    constexpr int frames = 4096;
    std::vector<double> left ((size_t) frames, 0.5), right ((size_t) frames, 0.5);

    sp::TpdfDither dither;
    dither.prepare (42);
    SP_REQUIRE (! dither.process (left.data(), right.data(), frames, 24, 24));
    for (int i = 0; i < frames; ++i)
    {
        SP_REQUIRE_NEAR (left[(size_t) i], 0.5, 0.0);
        SP_REQUIRE_NEAR (right[(size_t) i], 0.5, 0.0);
    }

    // 16-bit device with a 16-bit source: still no dither.
    SP_REQUIRE (! dither.process (left.data(), right.data(), frames, 16, 16));
}

SP_TEST (AudioDither, DeterministicForSameSeed)
{
    constexpr int frames = 8192;
    std::vector<double> a ((size_t) frames, 0.0), b ((size_t) frames, 0.0);

    sp::TpdfDither d1, d2;
    d1.prepare (0xdeadbeefu);
    d2.prepare (0xdeadbeefu);
    d1.process (a.data(), nullptr, frames, 16, 24);
    d2.process (b.data(), nullptr, frames, 16, 24);

    for (int i = 0; i < frames; ++i)
        SP_REQUIRE_NEAR (a[(size_t) i], b[(size_t) i], 1.0e-15);

    sp::TpdfDither d3;
    d3.prepare (0xfeedfaceu);
    std::vector<double> c ((size_t) frames, 0.0);
    d3.process (c.data(), nullptr, frames, 16, 24);
    bool differs = false;
    for (int i = 0; i < frames; ++i)
        if (std::abs (a[(size_t) i] - c[(size_t) i]) > 0.0)
            differs = true;
    SP_REQUIRE (differs);
}

SP_TEST (AudioDither, LsbScalesWithTargetDepth)
{
    std::cout << "        24-bit LSB=" << sp::TpdfDither::lsbForBits (24)
              << ", 20-bit LSB=" << sp::TpdfDither::lsbForBits (20)
              << ", 16-bit LSB=" << sp::TpdfDither::lsbForBits (16) << "\n";
    SP_REQUIRE_NEAR (sp::TpdfDither::lsbForBits (24), 1.0 / 8388608.0, 1.0e-16);
    SP_REQUIRE_NEAR (sp::TpdfDither::lsbForBits (20), 1.0 / 524288.0, 1.0e-16);
    SP_REQUIRE_NEAR (sp::TpdfDither::lsbForBits (16), 1.0 / 32768.0, 1.0e-12);
    SP_REQUIRE (sp::TpdfDither::lsbForBits (0) == 0.0);
}
