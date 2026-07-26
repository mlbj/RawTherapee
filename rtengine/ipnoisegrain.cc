/* -*- C++ -*-
 *
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <random>

#include "color.h"
#include "gauss.h"
#include "improcfun.h"
#include "labimage.h"
#include "imagefloat.h"
#include "procparams.h"
#include "rt_math.h"

#ifdef _OPENMP
#include <omp.h>
#endif

namespace rtengine
{

namespace {

constexpr float LAB_L_MAX = 32768.f;
constexpr unsigned int GRAIN_NOISE_BASE_SEED = 43;

// The strongest visible amplitude (in internal L 0-32768 units) at strength=100,
// before the shadow/highlight weighting and resolution (sk) scaling are applied.
constexpr float MAX_NOISE_AMPLITUDE = 5000.f;

// Fixed internal scale for the Poisson lambda (independent of the strength
// slider): controls the "granularity" of the shot-noise sampling, not its
// visible amount. Kept separate from strength so that strength alone controls
// output amplitude, monotonically.
constexpr float POISSON_LAMBDA_BASE = 40.f;

// Signal-dependent weighting: more visible noise in shadows, less in highlights,
// same shape as the one used for Locallab's simple noise (see addGaNoise()).
float shadowHighlightWeight(float L)
{
    float kvar = 1.f;

    if (L < 12000.f) {
        constexpr float ah = -0.5f / 12000.f;
        constexpr float bh = 1.5f;
        kvar = ah * L + bh;
    } else if (L > 20000.f) {
        constexpr float ah = -0.5f / 12768.f;
        constexpr float bh = 1.f - 20000.f * ah;
        kvar = ah * L + bh;
        kvar = kvar < 0.5f ? 0.5f : kvar;
    }

    return kvar;
}

// Approximate gamut protection: clip the chroma of a noise-perturbed a/b pair
// so it can't exceed what's plausible at that lightness/hue, using the same
// lightweight Prophoto approximation Locallab uses to keep chroma tools from
// producing impossible/oversaturated colors.
void clampChromaGamut(float L, float& a, float& b)
{
    const float lum = L / 327.68f;
    const float hue = std::atan2(b, a);
    const float chroma = std::sqrt(SQR(a) + SQR(b)) / 327.68f;

    if (chroma < 1e-6f) {
        return;
    }

    float maxChroma = 0.f;
    Color::pregamutlab(lum, hue, maxChroma);

    if (chroma > maxChroma) {
        const float scale = maxChroma / chroma;
        a *= scale;
        b *= scale;
    }
}

void addGaussianNoise(LabImage* lab, double strength, bool chroma, int sk, bool multiThread)
{
    const float baseStdDev = static_cast<float>(strength) / 100.f * MAX_NOISE_AMPLITUDE / std::sqrt(static_cast<float>(std::max(sk, 1)));
    const int W = lab->W;
    const int H = lab->H;

#ifdef _OPENMP
    #pragma omp parallel if (multiThread)
#endif
    {
#ifdef _OPENMP
        std::mt19937 rng(GRAIN_NOISE_BASE_SEED + omp_get_thread_num());
#else
        std::mt19937 rng(GRAIN_NOISE_BASE_SEED);
#endif
        std::normal_distribution<float> dist(0.f, 1.f);

#ifdef _OPENMP
        #pragma omp for schedule(static)
#endif
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const float varia = baseStdDev * shadowHighlightWeight(lab->L[y][x]);
                lab->L[y][x] = LIM(lab->L[y][x] + varia * dist(rng), 0.f, LAB_L_MAX);

                if (chroma) {
                    float a = lab->a[y][x] + varia * dist(rng);
                    float b = lab->b[y][x] + varia * dist(rng);
                    clampChromaGamut(lab->L[y][x], a, b);
                    lab->a[y][x] = a;
                    lab->b[y][x] = b;
                }
            }
        }
    }
}

void addPoissonNoise(LabImage* lab, double strength, bool chroma, int sk, bool multiThread)
{
    // Shot-noise-like: the raw Poisson deviation (sample - lambda) has a
    // standard deviation of sqrt(lambda), so brighter pixels naturally get
    // more absolute noise. "strength" only scales the overall output gain,
    // so it stays monotonic (higher strength = more visible noise) instead of
    // also shrinking the granularity of the underlying Poisson sampling.
    const float gain = static_cast<float>(strength) / 100.f * MAX_NOISE_AMPLITUDE / std::sqrt(POISSON_LAMBDA_BASE) / std::sqrt(static_cast<float>(std::max(sk, 1)));
    const int W = lab->W;
    const int H = lab->H;

#ifdef _OPENMP
    #pragma omp parallel if (multiThread)
#endif
    {
#ifdef _OPENMP
        std::mt19937 rng(GRAIN_NOISE_BASE_SEED + omp_get_thread_num());
#else
        std::mt19937 rng(GRAIN_NOISE_BASE_SEED);
#endif
        std::poisson_distribution<int> dist;

#ifdef _OPENMP
        #pragma omp for schedule(static)
#endif
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const float normL = rtengine::LIM(lab->L[y][x] / LAB_L_MAX, 0.f, 1.f);
                const float lambda = std::max(0.01f, normL * POISSON_LAMBDA_BASE);
                const int sample = dist(rng, std::poisson_distribution<int>::param_type(lambda));
                const float delta = (sample - lambda) * gain;
                lab->L[y][x] = LIM(lab->L[y][x] + delta, 0.f, LAB_L_MAX);

                if (chroma) {
                    const int sampleA = dist(rng, std::poisson_distribution<int>::param_type(lambda));
                    const int sampleB = dist(rng, std::poisson_distribution<int>::param_type(lambda));
                    float a = lab->a[y][x] + (sampleA - lambda) * gain;
                    float b = lab->b[y][x] + (sampleB - lambda) * gain;
                    clampChromaGamut(lab->L[y][x], a, b);
                    lab->a[y][x] = a;
                    lab->b[y][x] = b;
                }
            }
        }
    }
}

void addFilmGrain(ImProcFunctions* ipf, LabImage* lab, int isogr, int strengr, int scalegr, float divgr)
{
    const int W = lab->W;
    const int H = lab->H;

    // ipgrain.cc's Gamma response is subtle across the GUI's [0.2, 3.0] range
    // (it mostly reshapes how grain varies across tones rather than its
    // overall amount). Stretch it around its 1.5 neutral point so the same
    // slider produces a more perceptible tonal-response change in this tool,
    // without touching the shared engine used by Locallab's Film Grain.
    constexpr float DIVGR_NEUTRAL = 1.5f;
    constexpr float DIVGR_AMPLIFY = 2.5f;
    const float divgrEffective = LIM(DIVGR_NEUTRAL + (divgr - DIVGR_NEUTRAL) * DIVGR_AMPLIFY, 0.05f, 8.f);

    // isogr only feeds the noise's spatial frequency ("zoom" in ipgrain.cc),
    // which only spans about 3.4x across the GUI's [20, 6400] range -- too
    // narrow to read as anything but a reshuffled pattern. Stretch it
    // upward from its floor so grain coarseness changes are actually visible.
    constexpr float ISOGR_FLOOR = 20.f;
    constexpr float ISOGR_AMPLIFY = 5.f;
    const int isogrEffective = static_cast<int>(LIM(ISOGR_FLOOR + (static_cast<float>(isogr) - ISOGR_FLOOR) * ISOGR_AMPLIFY, ISOGR_FLOOR, 40000.f));

    Imagefloat tmpImage(W, H);

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            tmpImage.g(y, x) = lab->L[y][x];
            tmpImage.r(y, x) = lab->a[y][x];
            tmpImage.b(y, x) = lab->b[y][x];
        }
    }

    ipf->filmGrain(&tmpImage, isogrEffective, strengr, scalegr, divgrEffective, W, H, 0, W, H);

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            // filmGrain() adds to L without clamping; a/b pass through unmodified.
            lab->L[y][x] = LIM(tmpImage.g(y, x), 0.f, LAB_L_MAX);
            lab->a[y][x] = tmpImage.r(y, x);
            lab->b[y][x] = tmpImage.b(y, x);
        }
    }
}

// A plain, optional soft-focus blur, independent of the noise/grain methods
// above -- unlike Locallab's "Blur & Noise" section, it can be switched off
// entirely while still adding grain/noise, or used on its own.
void applyBlur(LabImage* lab, double radius, int sk, bool multiThread)
{
    const double sigma = radius / std::max(sk, 1);

    if (sigma < 0.1) {
        return;
    }

#ifdef _OPENMP
    #pragma omp parallel if (multiThread)
#endif
    {
        gaussianBlur(lab->L, lab->L, lab->W, lab->H, sigma);
        gaussianBlur(lab->a, lab->a, lab->W, lab->H, sigma);
        gaussianBlur(lab->b, lab->b, lab->W, lab->H, sigma);
    }
}

} // namespace

void ImProcFunctions::noiseGrain(LabImage* lab, int sk)
{
    const procparams::GrainNoiseParams& grain = params->grainNoise;

    if (!grain.enabled || lab->W < 8 || lab->H < 8) {
        return;
    }

    if (grain.blurEnabled) {
        applyBlur(lab, grain.blurRadius, sk, multiThread);
    }

    if (grain.method == "poisson") {
        addPoissonNoise(lab, grain.strength, grain.chroma, sk, multiThread);
    } else if (grain.method == "film") {
        addFilmGrain(this, lab, grain.isogr, grain.strengr, grain.scalegr, grain.divgr);
    } else {
        addGaussianNoise(lab, grain.strength, grain.chroma, sk, multiThread);
    }
}

} // namespace rtengine
