/*
    Snake Puppy Reference Player
    64-bit biquad filter library (RBJ audio-EQ-cookbook) plus magnitude helpers.

    Everything here is header-only, allocation free and noexcept so it can be
    used directly from the audio callback. All coefficient design is done in
    double precision; the state is double as well.

    Coefficient conventions:
      * Transfer function H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
        (a0 is normalised away).
      * Design functions take frequency in Hz and the sample rate in Hz. Frequencies
        are clamped to a safe range below Nyquist and Q/gain are clamped so that
        extreme (UI/)profile values cannot produce unstable filters at any supported
        sample rate (44.1 kHz .. 384 kHz).
      * "S" is the RBJ shelf slope parameter (0.5 .. 1.5, 1.0 = steepest without
        overshoot). A helper converts between Q and S for the shelf designs.

    Threading: pure value types, safe on any thread.
*/
#pragma once

#include "DSP/DspSmoothing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>

namespace sp
{

//==============================================================================
/** Normalised biquad coefficients (a0 == 1). */
struct BiquadCoeffs
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0;

    bool isIdentity() const noexcept
    {
        return b0 == 1.0 && b1 == 0.0 && b2 == 0.0 && a1 == 0.0 && a2 == 0.0;
    }

    static BiquadCoeffs makeIdentity() noexcept { return {}; }

    static BiquadCoeffs normalise (double B0, double B1, double B2,
                                   double A0, double A1, double A2) noexcept
    {
        BiquadCoeffs c;
        if (std::abs (A0) < 1.0e-30)
            return c;
        const double inv = 1.0 / A0;
        c.b0 = B0 * inv;
        c.b1 = B1 * inv;
        c.b2 = B2 * inv;
        c.a1 = A1 * inv;
        c.a2 = A2 * inv;
        return c;
    }

    //== Design helpers =======================================================
    /** Clamp a requested centre/corner frequency to something designable at fs. */
    static double clampFrequency (double freqHz, double sampleRate) noexcept
    {
        if (! (sampleRate > 0.0))
            return 1000.0;
        const double lowest = 1.0e-3;
        const double highest = sampleRate * 0.497;
        return std::clamp (freqHz, lowest, highest);
    }

    static double clampQ (double q) noexcept { return std::clamp (q, 0.05, 60.0); }

    static double clampGainDb (double db) noexcept { return std::clamp (db, -36.0, 36.0); }

    /** RBJ shelf slope S -> equivalent Q. */
    static double shelfQFromS (double s, double gainDb) noexcept
    {
        s = std::clamp (s, 0.1, 4.0);
        const double a = std::pow (10.0, clampGainDb (gainDb) / 40.0);
        const double denom = (a + 1.0 / a) * (1.0 / s - 1.0);
        const double qSquared = 1.0 / std::max (1.0e-12, denom + 2.0);
        double q = std::sqrt (qSquared);
        if (! std::isfinite (q) || q <= 0.0)
            q = 0.7071067811865476;
        return clampQ (q);
    }

    /** Q -> RBJ shelf slope S (inverse of shelfQFromS, clamped to a valid slope). */
    static double shelfSFromQ (double q, double gainDb) noexcept
    {
        q = clampQ (q);
        const double a = std::pow (10.0, clampGainDb (gainDb) / 40.0);
        const double invQ2 = 1.0 / (q * q);
        const double s = 1.0 / (1.0 + (invQ2 - 2.0) / (a + 1.0 / a));
        return std::clamp (s, 0.1, 4.0);
    }

    //== RBJ cookbook designs =================================================
    static BiquadCoeffs bell (double sampleRate, double freqHz, double gainDb, double q) noexcept
    {
        const double w0 = kTwoPi * clampFrequency (freqHz, sampleRate) / sampleRate;
        const double cw = std::cos (w0);
        const double sw = std::sin (w0);
        const double alpha = sw / (2.0 * clampQ (q));
        const double a = std::pow (10.0, clampGainDb (gainDb) / 40.0);

        return normalise (1.0 + alpha * a, -2.0 * cw, 1.0 - alpha * a,
                          1.0 + alpha / a, -2.0 * cw, 1.0 - alpha / a);
    }

    static BiquadCoeffs lowShelf (double sampleRate, double freqHz, double gainDb, double slopeS) noexcept
    {
        const double w0 = kTwoPi * clampFrequency (freqHz, sampleRate) / sampleRate;
        const double cw = std::cos (w0);
        const double sw = std::sin (w0);
        const double a = std::pow (10.0, clampGainDb (gainDb) / 40.0);
        const double s = std::clamp (slopeS, 0.1, 4.0);
        const double alpha = sw * 0.5 * std::sqrt ((a + 1.0 / a) * (1.0 / s - 1.0) + 2.0);
        const double twoSqrtAAlpha = 2.0 * std::sqrt (a) * alpha;

        return normalise (a * ((a + 1.0) - (a - 1.0) * cw + twoSqrtAAlpha),
                          2.0 * a * ((a - 1.0) - (a + 1.0) * cw),
                          a * ((a + 1.0) - (a - 1.0) * cw - twoSqrtAAlpha),
                          (a + 1.0) + (a - 1.0) * cw + twoSqrtAAlpha,
                          -2.0 * ((a - 1.0) + (a + 1.0) * cw),
                          (a + 1.0) + (a - 1.0) * cw - twoSqrtAAlpha);
    }

    static BiquadCoeffs highShelf (double sampleRate, double freqHz, double gainDb, double slopeS) noexcept
    {
        const double w0 = kTwoPi * clampFrequency (freqHz, sampleRate) / sampleRate;
        const double cw = std::cos (w0);
        const double sw = std::sin (w0);
        const double a = std::pow (10.0, clampGainDb (gainDb) / 40.0);
        const double s = std::clamp (slopeS, 0.1, 4.0);
        const double alpha = sw * 0.5 * std::sqrt ((a + 1.0 / a) * (1.0 / s - 1.0) + 2.0);
        const double twoSqrtAAlpha = 2.0 * std::sqrt (a) * alpha;

        return normalise (a * ((a + 1.0) + (a - 1.0) * cw + twoSqrtAAlpha),
                          -2.0 * a * ((a - 1.0) + (a + 1.0) * cw),
                          a * ((a + 1.0) + (a - 1.0) * cw - twoSqrtAAlpha),
                          (a + 1.0) - (a - 1.0) * cw + twoSqrtAAlpha,
                          2.0 * ((a - 1.0) - (a + 1.0) * cw),
                          (a + 1.0) - (a - 1.0) * cw - twoSqrtAAlpha);
    }

    /** Low shelf designed from a Q value (converted to RBJ slope S). */
    static BiquadCoeffs lowShelfQ (double sampleRate, double freqHz, double gainDb, double q) noexcept
    {
        return lowShelf (sampleRate, freqHz, gainDb, shelfSFromQ (q, gainDb));
    }

    static BiquadCoeffs highShelfQ (double sampleRate, double freqHz, double gainDb, double q) noexcept
    {
        return highShelf (sampleRate, freqHz, gainDb, shelfSFromQ (q, gainDb));
    }

    static BiquadCoeffs highPass (double sampleRate, double freqHz, double q) noexcept
    {
        const double w0 = kTwoPi * clampFrequency (freqHz, sampleRate) / sampleRate;
        const double cw = std::cos (w0);
        const double sw = std::sin (w0);
        const double alpha = sw / (2.0 * clampQ (q));
        const double onePlusCos = 1.0 + cw;

        return normalise (0.5 * onePlusCos, -onePlusCos, 0.5 * onePlusCos,
                          1.0 + alpha, -2.0 * cw, 1.0 - alpha);
    }

    static BiquadCoeffs lowPass (double sampleRate, double freqHz, double q) noexcept
    {
        const double w0 = kTwoPi * clampFrequency (freqHz, sampleRate) / sampleRate;
        const double cw = std::cos (w0);
        const double sw = std::sin (w0);
        const double alpha = sw / (2.0 * clampQ (q));
        const double oneMinusCos = 1.0 - cw;

        return normalise (0.5 * oneMinusCos, oneMinusCos, 0.5 * oneMinusCos,
                          1.0 + alpha, -2.0 * cw, 1.0 - alpha);
    }

    static BiquadCoeffs notch (double sampleRate, double freqHz, double q) noexcept
    {
        const double w0 = kTwoPi * clampFrequency (freqHz, sampleRate) / sampleRate;
        const double cw = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * clampQ (q));

        return normalise (1.0, -2.0 * cw, 1.0,
                          1.0 + alpha, -2.0 * cw, 1.0 - alpha);
    }

    /** Constant 0 dB peak-gain band pass. */
    static BiquadCoeffs bandPass (double sampleRate, double freqHz, double q) noexcept
    {
        const double w0 = kTwoPi * clampFrequency (freqHz, sampleRate) / sampleRate;
        const double cw = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * clampQ (q));

        return normalise (alpha, 0.0, -alpha,
                          1.0 + alpha, -2.0 * cw, 1.0 - alpha);
    }

    static BiquadCoeffs allPass (double sampleRate, double freqHz, double q) noexcept
    {
        const double w0 = kTwoPi * clampFrequency (freqHz, sampleRate) / sampleRate;
        const double cw = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * clampQ (q));

        return normalise (1.0 - alpha, -2.0 * cw, 1.0 + alpha,
                          1.0 + alpha, -2.0 * cw, 1.0 - alpha);
    }

    /** Design by BandType-like integer (kept dependency-free; see Equalizers). */
    static BiquadCoeffs design (int type, double sampleRate, double freqHz, double gainDb, double q) noexcept
    {
        switch (type)
        {
            case 1: return lowShelfQ (sampleRate, freqHz, gainDb, q);
            case 2: return highShelfQ (sampleRate, freqHz, gainDb, q);
            case 3: return highPass (sampleRate, freqHz, q);
            case 4: return lowPass (sampleRate, freqHz, q);
            case 5: return notch (sampleRate, freqHz, q);
            case 6: return bandPass (sampleRate, freqHz, q);
            case 7: return allPass (sampleRate, freqHz, q);
            case 0:
            default: return bell (sampleRate, freqHz, gainDb, q);
        }
    }

    //== Analysis =============================================================
    /** Complex frequency response at freqHz. */
    std::complex<double> response (double freqHz, double sampleRate) const noexcept
    {
        const double w = kTwoPi * freqHz / sampleRate;
        const std::complex<double> z1 (std::cos (w), -std::sin (w));
        const std::complex<double> z2 = z1 * z1;
        const std::complex<double> num = b0 + b1 * z1 + b2 * z2;
        const std::complex<double> den = 1.0 + a1 * z1 + a2 * z2;
        const double denMag = std::abs (den);
        if (denMag < 1.0e-30)
            return {};
        return num / den;
    }

    double magnitudeAt (double freqHz, double sampleRate) const noexcept
    {
        return std::abs (response (freqHz, sampleRate));
    }

    double magnitudeDbAt (double freqHz, double sampleRate) const noexcept
    {
        const double m = magnitudeAt (freqHz, sampleRate);
        return m > 1.0e-12 ? 20.0 * std::log10 (m) : -240.0;
    }

    /** True when the coefficients describe a stable biquad. */
    bool isStable() const noexcept
    {
        if (! std::isfinite (b0) || ! std::isfinite (b1) || ! std::isfinite (b2)
            || ! std::isfinite (a1) || ! std::isfinite (a2))
            return false;
        // Second order stability triangle: |a2| < 1 and |a1| < 1 + a2.
        return std::abs (a2) < 1.0 && std::abs (a1) < 1.0 + a2;
    }
};

//==============================================================================
/** Direct-form II transposed biquad state, double precision. */
struct BiquadState
{
    double z1 = 0.0, z2 = 0.0;

    void reset() noexcept { z1 = z2 = 0.0; }

    inline double process (const BiquadCoeffs& c, double x) noexcept
    {
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
};

//==============================================================================
/** Butterworth cascade helpers for 12 / 24 / 48 dB-per-octave filters. */
struct Butterworth
{
    /** Number of biquad stages for a slope (12 -> 1, 24 -> 2, 48 -> 4). */
    static int stagesForSlope (int slopeDbPerOct) noexcept
    {
        const int halve = slopeDbPerOct / 12;
        return std::clamp (halve, 1, 4);
    }

    /** Q of stage `stage` (0-based) for a Butterworth filter of `numStages` biquads. */
    static double qForStage (int stage, int numStages) noexcept
    {
        const int order = 2 * numStages;
        const double theta = kPi * (2.0 * stage + 1.0) / (2.0 * order);
        const double q = 1.0 / (2.0 * std::sin (theta));
        return BiquadCoeffs::clampQ (q);
    }

    /** Fill `qs` with the stage Q values, returns the stage count. */
    static int designQs (int slopeDbPerOct, std::array<double, 4>& qs) noexcept
    {
        const int n = stagesForSlope (slopeDbPerOct);
        for (int i = 0; i < 4; ++i)
            qs[(size_t) i] = i < n ? qForStage (i, n) : 0.7071067811865476;
        return n;
    }
};

} // namespace sp
