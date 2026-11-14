// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

/**
 * SiFi-GAN front-end: the feature extraction the self-trained SiFi-GAN
 * vocoder was trained with, reproduced stage by stage from the training
 * pipeline (SiFi-GAN's extract_features.py + decode.py input assembly):
 *
 *   24kHz mono waveform
 *     -> low-cut FIR (scipy firwin, 255 taps, 70Hz high-pass)
 *     -> F0 (WORLD Harvest, floor 100 / ceil 840, 5ms frame period)
 *     -> spectral envelope (WORLD CheapTrick, FFT 1024)
 *     -> mel-cepstrum (pysptk.sp2mc equivalent: order 39, alpha 0.466)
 *     -> aperiodicity (WORLD D4C, FFT 1024 -> CodeAperiodicity, 3 bands)
 *     -> continuous F0 (edge fill + linear interpolation + 20Hz low-pass)
 *     -> c = StandardScaler-normalised [mcep(40) | bap(3)]  (43 dims)
 *
 * and the generator's remaining inputs, which the model does NOT compute
 * itself (they were built on the decode side during training):
 *
 *     -> excitation signal (SignalGenerator: sine from cf0, phase by
 *        float64 cumulative sum, plus amplitude-shaped Gaussian noise)
 *     -> 4 dilated-factor arrays from cf0 (dense factors 0.5/1/4/8,
 *        repeated by the cumulative upsamples 5/20/60/120)
 *
 * WORLD itself is the vendored original (third_party/world); the parts
 * reimplemented here are the scipy/pysptk glue around it, and each one
 * is a separate function so a test can compare it against the Python
 * original stage by stage. This file is deliberately JUCE-free so the
 * standalone verification harness can link it without the plugin.
 */
namespace sifigan
{

constexpr int kSampleRate = 24000;
constexpr int kHopSize = 120;
constexpr int kFftSize = 1024;
constexpr int kMcepOrder = 39;
constexpr int kMcepDim = kMcepOrder + 1; // 40, including the 0th coefficient
constexpr int kBapDim = 3;
constexpr int kFeatureDim = kMcepDim + kBapDim; // 43
constexpr double kMcepAlpha = 0.466;            // pysptk table value for 24kHz
constexpr double kF0Floor = 100.0;
constexpr double kF0Ceil = 840.0;
constexpr double kFramePeriodMs = 5.0;

/** Feature normalisation statistics (sklearn StandardScaler per type). */
struct Stats
{
  std::vector<double> mcepMean, mcepScale, bapMean, bapScale;

  bool isValid() const
  {
    if (mcepMean.size() != kMcepDim || mcepScale.size() != kMcepDim ||
        bapMean.size() != kBapDim || bapScale.size() != kBapDim)
      return false;
    // A hand-placed sidecar with a zero / non-finite scale would turn
    // the normalisation into inf / NaN features and a NaN render; the
    // module's rule for unusable input is to fail instead. (Scaler
    // output from sklearn never looks like this: it stores 1.0 for a
    // zero scale.)
    auto usable = [](const std::vector<double> &mean,
                     const std::vector<double> &scale) {
      for (double v : mean)
        if (!std::isfinite(v))
          return false;
      for (double v : scale)
        if (!std::isfinite(v) || v <= 0.0)
          return false;
      return true;
    };
    return usable(mcepMean, mcepScale) && usable(bapMean, bapScale);
  }
};

/**
 * Parse the stats sidecar JSON (the format stage 1 produced from the
 * training joblib: {"features": {"mcep": {"mean": [...], "scale": [...]},
 * "bap": {...}, ...}}). Only mcep and bap are read - they are the only
 * scalers the decode path applies. Returns false with a reason in
 * @p error when the text does not carry usable mcep/bap arrays.
 */
bool loadStatsFromJsonText(const std::string &text, Stats &out,
                           std::string *error = nullptr);

/**
 * scipy.signal.firwin with its defaults (Hamming window, scale=True):
 * @p cutoffNorm is the cutoff in units of Nyquist (0..1), @p passZero
 * false makes a high-pass (spectral inversion of the low-pass prototype).
 * The scale makes the response exactly 1 at DC for a low-pass, and at
 * the centre of the pass band for a high-pass, as scipy does.
 */
std::vector<double> firwin(int numTaps, double cutoffNorm, bool passZero);

/** scipy.signal.lfilter with a = 1 and zero initial state (plain FIR). */
std::vector<double> applyFir(const std::vector<double> &taps,
                             const std::vector<double> &x);

/**
 * pysptk.sp2mc for one frame: the CheapTrick envelope (fftSize/2+1
 * amplitude bins) to a mel-cepstrum of kMcepDim coefficients.
 * log -> real cepstrum (irfft, c[0] halved) -> SPTK freqt (all-pass
 * frequency transformation with @p alpha). The input is used exactly
 * as pysptk receives it; no power/amplitude conversion is applied,
 * matching the training pipeline.
 */
std::vector<double> spectrumToMelCepstrum(const double *envelope,
                                          int numBins, double alpha);

/**
 * The continuous-F0 conversion (extract_features.convert_continuos_f0
 * plus its low-pass stage): unvoiced edges take the nearest voiced
 * value, interior gaps are linearly interpolated, then a firwin
 * 255-tap low-pass at 20Hz (frame rate 200Hz) with edge padding and
 * group-delay compensation is applied; if any value goes negative the
 * filter is retried at 70, 140, ... Hz as the original does. An
 * all-unvoiced input is returned unchanged (all zeros).
 */
std::vector<float> makeContinuousF0(const std::vector<float> &f0);

/** One analysed span: raw (unnormalised) features on the 24k/120 grid. */
struct Analysis
{
  int numFrames = 0;
  std::vector<float> f0;  // the F0 track CheapTrick/D4C consumed (Hz, 0 = unvoiced)
  std::vector<float> cf0; // continuous F0 derived from f0
  std::vector<std::vector<float>> mcep; // [T][40]
  std::vector<std::vector<float>> bap;  // [T][3]
};

/**
 * The full training-identical analysis of a 24kHz mono waveform.
 * When @p f0Track is empty the F0 is estimated with Harvest; when it is
 * given it is used as-is (resampled by the caller onto this grid;
 * truncated or zero-padded to the frame count) - the plugin path,
 * whose F0 comes from its own analysis. The frame count follows
 * WORLD's GetSamplesForHarvest for the waveform length, exactly as
 * pyworld reports it, and every feature is truncated to the shortest,
 * as extract_features.py does.
 */
Analysis analyse(const std::vector<double> &waveform24k,
                 const std::vector<float> &f0Track = {});

/** c for the generator: [mcep | bap] per frame, StandardScaler-normalised. */
std::vector<std::vector<float>> normalise(const Analysis &analysis,
                                          const Stats &stats);

/**
 * The SignalGenerator excitation (signal_types = ["sine"]):
 * vuv and cf0 are upsampled to the sample rate by nearest neighbour
 * (torch F.interpolate's default), the phase is the float64 cumulative
 * sum of cf0/sampleRate, sine = vuv * sin(2*pi*phase) * 0.1, and
 * Gaussian noise is added at 0.003 in voiced samples and 0.001 in
 * unvoiced ones. The whole span is generated in one pass, so chunked
 * inference can slice it and the phase stays continuous across chunks
 * by construction. @p seed fixes the noise draw (std::mt19937_64);
 * the draw cannot match PyTorch's randn sequence, which is fine -
 * training-time decode draws fresh noise the same way.
 */
std::vector<float> buildExcitation(const std::vector<float> &cf0,
                                   std::uint64_t seed);

/**
 * The four dilated-factor inputs: for dense factors {0.5, 1, 4, 8},
 * d = (sampleRate/df) / cf0 with cf0 == 0 replaced by sampleRate/df
 * (so d = 1 there), each repeated by the cumulative upsample factors
 * {5, 20, 60, 120}.
 */
std::array<std::vector<float>, 4> buildDilatedFactors(
    const std::vector<float> &cf0);

} // namespace sifigan
