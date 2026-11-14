// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#include "SiFiGANFeatures.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <random>

#include "world/cheaptrick.h"
#include "world/codec.h"
#include "world/d4c.h"
#include "world/harvest.h"

namespace sifigan
{
namespace
{

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// A small radix-2 FFT, only what spectrumToMelCepstrum needs (1024 points).
// ---------------------------------------------------------------------------

void fftInPlace(std::vector<std::complex<double>> &a, bool inverse)
{
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i)
  {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j)
      std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1)
  {
    const double angle =
        (inverse ? 2.0 : -2.0) * kPi / static_cast<double>(len);
    const std::complex<double> step(std::cos(angle), std::sin(angle));
    for (size_t i = 0; i < n; i += len)
    {
      std::complex<double> w(1.0, 0.0);
      for (size_t k = 0; k < len / 2; ++k)
      {
        const auto u = a[i + k];
        const auto v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= step;
      }
    }
  }
  if (inverse)
    for (auto &v : a)
      v /= static_cast<double>(n);
}

// ---------------------------------------------------------------------------
// JSON helpers for the stats sidecar: just enough to find
// "features" -> "<name>" -> "mean"/"scale" -> [numbers]. The format is
// the project's own (stage 1 wrote it), so a full JSON parser is not
// warranted; anything unexpected makes the parse fail, not guess.
// ---------------------------------------------------------------------------

bool findKey(const std::string &text, size_t from, const std::string &key,
             size_t &valuePos)
{
  const std::string needle = "\"" + key + "\"";
  size_t pos = text.find(needle, from);
  if (pos == std::string::npos)
    return false;
  pos = text.find(':', pos + needle.size());
  if (pos == std::string::npos)
    return false;
  ++pos;
  while (pos < text.size() &&
         (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
          text[pos] == '\r'))
    ++pos;
  valuePos = pos;
  return true;
}

bool parseNumberArray(const std::string &text, size_t arrayPos,
                        std::vector<double> &out)
{
  if (arrayPos >= text.size() || text[arrayPos] != '[')
    return false;
  size_t pos = arrayPos + 1;
  out.clear();
  while (pos < text.size())
  {
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
            text[pos] == '\r' || text[pos] == ','))
      ++pos;
    if (pos < text.size() && text[pos] == ']')
      return true;
    try
    {
      size_t used = 0;
      const double value = std::stod(text.substr(pos), &used);
      if (used == 0)
        return false;
      out.push_back(value);
      pos += used;
    }
    catch (...)
    {
      return false;
    }
  }
  return false;
}

bool parseFeatureStats(const std::string &text, size_t featuresPos,
                       const std::string &name, std::vector<double> &mean,
                       std::vector<double> &scale)
{
  size_t sectionPos = 0;
  if (!findKey(text, featuresPos, name, sectionPos))
    return false;
  // The next feature's section must not leak into this one: bound the
  // search for mean/scale to before the following feature key. The
  // sidecar lists features in a fixed order, so the bound is the next
  // '{' of a sibling section - approximated by searching mean/scale
  // only, which appear immediately inside this feature's object.
  size_t meanPos = 0, scalePos = 0;
  if (!findKey(text, sectionPos, "mean", meanPos) ||
      !parseNumberArray(text, meanPos, mean))
    return false;
  if (!findKey(text, sectionPos, "scale", scalePos) ||
      !parseNumberArray(text, scalePos, scale))
    return false;
  return mean.size() == scale.size() && !mean.empty();
}

} // namespace

bool loadStatsFromJsonText(const std::string &text, Stats &out,
                           std::string *error)
{
  const auto fail = [error](const std::string &reason)
  {
    if (error != nullptr)
      *error = reason;
    return false;
  };

  size_t featuresPos = 0;
  if (!findKey(text, 0, "features", featuresPos))
    return fail("stats JSON has no \"features\" object");

  Stats parsed;
  if (!parseFeatureStats(text, featuresPos, "mcep", parsed.mcepMean,
                         parsed.mcepScale))
    return fail("stats JSON has no usable \"mcep\" mean/scale arrays");
  if (!parseFeatureStats(text, featuresPos, "bap", parsed.bapMean,
                         parsed.bapScale))
    return fail("stats JSON has no usable \"bap\" mean/scale arrays");
  if (!parsed.isValid())
    return fail("stats dimensions do not match mcep 40 / bap 3");

  out = std::move(parsed);
  return true;
}

std::vector<double> firwin(int numTaps, double cutoffNorm, bool passZero)
{
  // scipy.signal.firwin(numTaps, cutoffNorm) with the default Hamming
  // window and scaling. cutoffNorm is in units of Nyquist, so the
  // cutoff in cycles/sample is cutoffNorm / 2.
  const int M = numTaps - 1;
  std::vector<double> h(static_cast<size_t>(numTaps), 0.0);
  const double fc = cutoffNorm / 2.0;
  const auto sinc = [](double x)
  {
    if (x == 0.0)
      return 1.0;
    const double px = kPi * x;
    return std::sin(px) / px;
  };

  for (int n = 0; n < numTaps; ++n)
  {
    const double m = static_cast<double>(n) - M / 2.0;
    double ideal = 0.0;
    if (passZero)
    {
      ideal = 2.0 * fc * sinc(2.0 * fc * m);
    }
    else
    {
      // The single pass band is [fc, 0.5] cycles/sample: its ideal
      // response is the full band minus the low band.
      ideal = sinc(m) - 2.0 * fc * sinc(2.0 * fc * m);
    }
    const double window =
        0.54 - 0.46 * std::cos(2.0 * kPi * n / static_cast<double>(M));
    h[static_cast<size_t>(n)] = ideal * window;
  }

  // scipy's scale=True: unit response at DC for a low-pass, at the
  // centre of the pass band ((cutoff + Nyquist) / 2) for a high-pass.
  const double scaleFreq = passZero ? 0.0 : (fc + 0.5) / 2.0;
  std::complex<double> response(0.0, 0.0);
  for (int n = 0; n < numTaps; ++n)
  {
    const double phase = -2.0 * kPi * scaleFreq * n;
    response += h[static_cast<size_t>(n)] *
                std::complex<double>(std::cos(phase), std::sin(phase));
  }
  const double magnitude = std::abs(response);
  if (magnitude > 0.0)
    for (auto &v : h)
      v /= magnitude;
  return h;
}

std::vector<double> applyFir(const std::vector<double> &taps,
                             const std::vector<double> &x)
{
  std::vector<double> y(x.size(), 0.0);
  const size_t numTaps = taps.size();
  for (size_t n = 0; n < x.size(); ++n)
  {
    double acc = 0.0;
    const size_t kmax = std::min(numTaps, n + 1);
    for (size_t k = 0; k < kmax; ++k)
      acc += taps[k] * x[n - k];
    y[n] = acc;
  }
  return y;
}

std::vector<double> spectrumToMelCepstrum(const double *envelope,
                                          int numBins, double alpha)
{
  // pysptk.sp2mc: c = irfft(log(envelope)); c[0] /= 2; freqt(c).
  const int fftSize = (numBins - 1) * 2;
  std::vector<std::complex<double>> spectrum(
      static_cast<size_t>(fftSize), {0.0, 0.0});
  for (int k = 0; k < numBins; ++k)
    spectrum[static_cast<size_t>(k)] = std::log(envelope[k]);
  for (int k = 1; k < numBins - 1; ++k)
    spectrum[static_cast<size_t>(fftSize - k)] =
        spectrum[static_cast<size_t>(k)];

  fftInPlace(spectrum, true); // inverse (includes the 1/N scaling)

  std::vector<double> cepstrum(static_cast<size_t>(fftSize));
  for (int i = 0; i < fftSize; ++i)
    cepstrum[static_cast<size_t>(i)] = spectrum[static_cast<size_t>(i)].real();
  cepstrum[0] /= 2.0;

  // SPTK freqt: the all-pass frequency transformation of the cepstrum,
  // input order fftSize-1 down to output order kMcepOrder.
  const int inputOrder = fftSize - 1;
  std::vector<double> g(kMcepDim, 0.0), d(kMcepDim, 0.0);
  const double beta = 1.0 - alpha * alpha;
  for (int i = inputOrder; i >= 0; --i)
  {
    d[0] = g[0];
    g[0] = cepstrum[static_cast<size_t>(i)] + alpha * d[0];
    if (kMcepDim > 1)
    {
      d[1] = g[1];
      g[1] = beta * d[0] + alpha * d[1];
    }
    for (int m = 2; m < kMcepDim; ++m)
    {
      d[static_cast<size_t>(m)] = g[static_cast<size_t>(m)];
      g[static_cast<size_t>(m)] =
          d[static_cast<size_t>(m - 1)] +
          alpha * (d[static_cast<size_t>(m)] - g[static_cast<size_t>(m - 1)]);
    }
  }
  return g;
}

std::vector<float> makeContinuousF0(const std::vector<float> &f0)
{
  const int T = static_cast<int>(f0.size());
  if (T == 0)
    return {};

  // extract_features.convert_continuos_f0
  bool anyVoiced = false;
  float startF0 = 0.0f, endF0 = 0.0f;
  for (float v : f0)
    if (v != 0.0f)
    {
      if (!anyVoiced)
        startF0 = v;
      endF0 = v;
      anyVoiced = true;
    }
  if (!anyVoiced)
    return f0; // the original leaves an all-unvoiced track untouched

  std::vector<double> cf0(f0.begin(), f0.end());
  int startIdx = 0;
  while (startIdx < T && cf0[static_cast<size_t>(startIdx)] != startF0)
    ++startIdx;
  int endIdx = T - 1;
  while (endIdx >= 0 && cf0[static_cast<size_t>(endIdx)] != endF0)
    --endIdx;
  for (int i = 0; i < startIdx; ++i)
    cf0[static_cast<size_t>(i)] = startF0;
  for (int i = endIdx; i < T; ++i)
    cf0[static_cast<size_t>(i)] = endF0;

  // Linear interpolation over the zero gaps (scipy interp1d over the
  // non-zero frame indices, which after the edge fill span [0, T)).
  {
    std::vector<int> nz;
    for (int i = 0; i < T; ++i)
      if (cf0[static_cast<size_t>(i)] != 0.0)
        nz.push_back(i);
    for (size_t k = 1; k < nz.size(); ++k)
    {
      const int a = nz[k - 1], b = nz[k];
      for (int i = a + 1; i < b; ++i)
      {
        const double t = static_cast<double>(i - a) / (b - a);
        cf0[static_cast<size_t>(i)] =
            cf0[static_cast<size_t>(a)] * (1.0 - t) +
            cf0[static_cast<size_t>(b)] * t;
      }
    }
  }

  // extract_features.low_pass_filter at the 200Hz frame rate: firwin
  // 255 taps, edge padding by the tap count, group delay (127)
  // compensated in the slice. The original slice yields one sample
  // more than the input; the extra tail sample is dropped by the
  // pipeline's min-length truncation, so it is dropped here too.
  const auto lowPass = [](const std::vector<double> &x, double cutoffHz)
  {
    constexpr int numTaps = 255;
    const double frameRate = kSampleRate / kHopSize; // 200
    const auto taps = firwin(numTaps, cutoffHz / (frameRate / 2.0), true);
    std::vector<double> padded(x.size() + 2 * numTaps);
    for (size_t i = 0; i < padded.size(); ++i)
    {
      const int src = static_cast<int>(i) - numTaps;
      padded[i] = x[static_cast<size_t>(
          std::clamp(src, 0, static_cast<int>(x.size()) - 1))];
    }
    const auto filtered = applyFir(taps, padded);
    const int start = numTaps + numTaps / 2;
    std::vector<double> out;
    for (int i = start; i < static_cast<int>(filtered.size()) - numTaps / 2;
         ++i)
      out.push_back(filtered[static_cast<size_t>(i)]);
    return out;
  };

  std::vector<double> smoothed = lowPass(cf0, 20.0);
  double cutoff = 70.0;
  const auto hasNegative = [](const std::vector<double> &v)
  {
    return std::any_of(v.begin(), v.end(), [](double s) { return s < 0.0; });
  };
  while (hasNegative(smoothed))
  {
    smoothed = lowPass(cf0, cutoff);
    cutoff *= 2.0;
  }

  std::vector<float> result(static_cast<size_t>(T));
  for (int i = 0; i < T; ++i)
    result[static_cast<size_t>(i)] =
        static_cast<float>(smoothed[static_cast<size_t>(i)]);
  return result;
}

Analysis analyse(const std::vector<double> &waveform24k,
                 const std::vector<float> &f0Track)
{
  Analysis result;
  const int xLength = static_cast<int>(waveform24k.size());
  if (xLength == 0)
    return result;

  // Low-cut first: extract_features applies it before every WORLD call.
  const auto filtered = applyFir(
      firwin(255, 70.0 / (kSampleRate / 2.0), false), waveform24k);

  const int numFrames =
      GetSamplesForHarvest(kSampleRate, xLength, kFramePeriodMs);
  std::vector<double> temporalPositions(static_cast<size_t>(numFrames));
  for (int i = 0; i < numFrames; ++i)
    temporalPositions[static_cast<size_t>(i)] =
        i * kFramePeriodMs / 1000.0;

  std::vector<double> f0(static_cast<size_t>(numFrames), 0.0);
  if (f0Track.empty())
  {
    HarvestOption option;
    InitializeHarvestOption(&option);
    option.f0_floor = kF0Floor;
    option.f0_ceil = kF0Ceil;
    option.frame_period = kFramePeriodMs;
    Harvest(filtered.data(), xLength, kSampleRate, &option,
            temporalPositions.data(), f0.data());
  }
  else
  {
    for (int i = 0; i < numFrames; ++i)
      f0[static_cast<size_t>(i)] =
          i < static_cast<int>(f0Track.size())
              ? static_cast<double>(f0Track[static_cast<size_t>(i)])
              : 0.0;
  }

  constexpr int numBins = kFftSize / 2 + 1;
  std::vector<std::vector<double>> envelope(
      static_cast<size_t>(numFrames), std::vector<double>(numBins));
  std::vector<std::vector<double>> aperiodicity(
      static_cast<size_t>(numFrames), std::vector<double>(numBins));
  std::vector<double *> envelopePtrs(static_cast<size_t>(numFrames));
  std::vector<double *> apPtrs(static_cast<size_t>(numFrames));
  for (int i = 0; i < numFrames; ++i)
  {
    envelopePtrs[static_cast<size_t>(i)] =
        envelope[static_cast<size_t>(i)].data();
    apPtrs[static_cast<size_t>(i)] =
        aperiodicity[static_cast<size_t>(i)].data();
  }

  CheapTrickOption cheapTrickOption;
  InitializeCheapTrickOption(kSampleRate, &cheapTrickOption);
  cheapTrickOption.fft_size = kFftSize; // pyworld passes it explicitly
  CheapTrick(filtered.data(), xLength, kSampleRate,
             temporalPositions.data(), f0.data(), numFrames,
             &cheapTrickOption, envelopePtrs.data());

  D4COption d4cOption;
  InitializeD4COption(&d4cOption);
  D4C(filtered.data(), xLength, kSampleRate, temporalPositions.data(),
      f0.data(), numFrames, kFftSize, &d4cOption, apPtrs.data());

  std::vector<std::vector<double>> coded(
      static_cast<size_t>(numFrames), std::vector<double>(kBapDim, 0.0));
  std::vector<double *> codedPtrs(static_cast<size_t>(numFrames));
  for (int i = 0; i < numFrames; ++i)
    codedPtrs[static_cast<size_t>(i)] = coded[static_cast<size_t>(i)].data();
  CodeAperiodicity(const_cast<const double * const *>(apPtrs.data()),
                   numFrames, kSampleRate, kFftSize, codedPtrs.data());

  result.numFrames = numFrames;
  result.f0.assign(f0.begin(), f0.end());
  result.cf0 = makeContinuousF0(result.f0);
  result.mcep.resize(static_cast<size_t>(numFrames));
  result.bap.resize(static_cast<size_t>(numFrames));
  for (int i = 0; i < numFrames; ++i)
  {
    const auto mc = spectrumToMelCepstrum(
        envelope[static_cast<size_t>(i)].data(), numBins, kMcepAlpha);
    auto &mcepFrame = result.mcep[static_cast<size_t>(i)];
    mcepFrame.assign(mc.begin(), mc.end());
    auto &bapFrame = result.bap[static_cast<size_t>(i)];
    bapFrame.resize(kBapDim);
    for (int j = 0; j < kBapDim; ++j)
      bapFrame[static_cast<size_t>(j)] =
          static_cast<float>(coded[static_cast<size_t>(i)]
                                  [static_cast<size_t>(j)]);
  }
  return result;
}

std::vector<std::vector<float>> normalise(const Analysis &analysis,
                                          const Stats &stats)
{
  std::vector<std::vector<float>> c(
      static_cast<size_t>(analysis.numFrames),
      std::vector<float>(kFeatureDim, 0.0f));
  for (int t = 0; t < analysis.numFrames; ++t)
  {
    auto &frame = c[static_cast<size_t>(t)];
    const auto &mcep = analysis.mcep[static_cast<size_t>(t)];
    const auto &bap = analysis.bap[static_cast<size_t>(t)];
    for (int j = 0; j < kMcepDim; ++j)
      frame[static_cast<size_t>(j)] = static_cast<float>(
          (mcep[static_cast<size_t>(j)] - stats.mcepMean[static_cast<size_t>(j)]) /
          stats.mcepScale[static_cast<size_t>(j)]);
    for (int j = 0; j < kBapDim; ++j)
      frame[static_cast<size_t>(kMcepDim + j)] = static_cast<float>(
          (bap[static_cast<size_t>(j)] - stats.bapMean[static_cast<size_t>(j)]) /
          stats.bapScale[static_cast<size_t>(j)]);
  }
  return c;
}

std::vector<float> buildExcitation(const std::vector<float> &cf0,
                                   std::uint64_t seed)
{
  const size_t T = cf0.size();
  const size_t S = T * kHopSize;
  std::vector<float> signal(S, 0.0f);
  if (T == 0)
    return signal;

  // SignalGenerator.sinusoid: nearest-neighbour upsampling of vuv and
  // cf0 (F.interpolate's default mode), phase as a float64 cumulative
  // sum, noise shaped by vuv. The noise draw uses a standard Gaussian
  // generator; its sequence differs from PyTorch's, as one fresh
  // decode's draw differs from another's.
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> gaussian(0.0, 1.0);

  double phase = 0.0;
  for (size_t s = 0; s < S; ++s)
  {
    const float f0 = cf0[s / kHopSize];
    const double vuv = f0 > 0.0f ? 1.0 : 0.0;
    phase += static_cast<double>(f0) / kSampleRate;
    double value = vuv * std::sin(2.0 * kPi * phase) * 0.1;
    const double noiseAmp = vuv * 0.003 + (1.0 - vuv) * 0.001;
    value += gaussian(rng) * noiseAmp;
    signal[s] = static_cast<float>(value);
  }
  return signal;
}

std::array<std::vector<float>, 4> buildDilatedFactors(
    const std::vector<float> &cf0)
{
  constexpr double denseFactors[4] = {0.5, 1.0, 4.0, 8.0};
  constexpr int upsamples[4] = {5, 20, 60, 120};
  std::array<std::vector<float>, 4> out;
  for (int k = 0; k < 4; ++k)
  {
    const double base = kSampleRate / denseFactors[k];
    auto &df = out[static_cast<size_t>(k)];
    df.resize(cf0.size() * static_cast<size_t>(upsamples[k]));
    for (size_t t = 0; t < cf0.size(); ++t)
    {
      const double value =
          cf0[t] == 0.0f ? 1.0 : base / static_cast<double>(cf0[t]);
      for (int r = 0; r < upsamples[k]; ++r)
        df[t * static_cast<size_t>(upsamples[k]) +
           static_cast<size_t>(r)] = static_cast<float>(value);
    }
  }
  return out;
}

} // namespace sifigan
