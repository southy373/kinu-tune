#include "BigVGANMel.h"
#include <algorithm>
#include <cmath>

namespace
{
// Reflection padding the way torch/librosa do it: the edge sample is not
// repeated, the signal mirrors around it (index -1 reads sample 1).
float reflectSample(const float *audio, int numSamples, int index)
{
    if (numSamples <= 1)
        return audio[0];

    const int period = 2 * numSamples - 2;
    int m = index % period;
    if (m < 0)
        m += period;
    if (m >= numSamples)
        m = period - m;
    return audio[m];
}

std::vector<float> reflectPad(const float *audio, int numSamples, int pad)
{
    std::vector<float> padded(static_cast<size_t>(numSamples) +
                              2 * static_cast<size_t>(pad));
    for (size_t i = 0; i < padded.size(); ++i)
        padded[i] = reflectSample(audio, numSamples,
                                  static_cast<int>(i) - pad);
    return padded;
}
} // namespace

std::vector<std::vector<float>> BigVGANMel::compute(const float *audio,
                                                    int numSamples)
{
    if (audio == nullptr || numSamples <= 0)
        return {};

    // BigVGAN pads by (n_fft - hop) / 2 = 768 samples on both sides, then
    // takes uncentred frames of n_fft every hop samples.
    constexpr int kPad = (kFftSize - kHopSize) / 2;
    auto padded = reflectPad(audio, numSamples, kPad);

    int numFrames = (static_cast<int>(padded.size()) - kFftSize) / kHopSize + 1;
    int extraFrames = 0;
    if (numFrames < 1)
    {
        // Shorter than one FFT window even after padding: extend the
        // reflection (by whole hops, so the frame bookkeeping below stays
        // exact) until a single frame fits rather than dropping the render.
        // Phrase renders are seconds long, so this is a guard for
        // degenerate inputs only.
        const int deficit = kFftSize - static_cast<int>(padded.size());
        extraFrames = (deficit + kHopSize - 1) / kHopSize;
        auto extended = reflectPad(padded.data(),
                                   static_cast<int>(padded.size()),
                                   extraFrames * kHopSize);
        padded.swap(extended);
        numFrames = 1;
    }

    // MelSpectrogram centres frame j on sample j * hop of its input, so its
    // frame j covers [j * hop - n_fft/2, j * hop + n_fft/2). BigVGAN frame t
    // covers [t * hop, t * hop + n_fft) of the same padded signal, which is
    // MelSpectrogram frame t + 2 (plus one per whole hop of guard padding).
    // For every t in range that window lies fully inside the padded signal,
    // so MelSpectrogram's own boundary reflection never engages and the
    // frames are the STFT frames BigVGAN expects, with no re-derivation of
    // the filterbank.
    auto centred = mel.compute(padded.data(),
                               static_cast<int>(padded.size()));

    const size_t firstCentredFrame = static_cast<size_t>(2 + extraFrames);
    std::vector<std::vector<float>> result;
    result.reserve(static_cast<size_t>(numFrames));
    for (int t = 0; t < numFrames; ++t)
    {
        const size_t centredIndex =
            firstCentredFrame + static_cast<size_t>(t);
        if (centredIndex < centred.size())
            result.push_back(std::move(centred[centredIndex]));
        else
            result.emplace_back(kNumMels, std::log(1e-5f));
    }
    return result;
}
