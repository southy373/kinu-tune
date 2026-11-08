#pragma once

#include "../JuceHeader.h"
#include "MelSpectrogram.h"
#include <vector>

/**
 * Mel spectrogram in the exact form BigVGAN v2 (44kHz / 128 band) expects.
 *
 * This is NOT interchangeable with the analysis mel in
 * AudioData::melSpectrogram (fmin 40 / fmax 16000, log floor 1e-10). BigVGAN
 * was trained on the librosa-style mel its own meldataset.py computes:
 *
 *   sample rate 44100, n_fft 2048, hop 512, periodic Hann window,
 *   128 mel bands spanning 0..22050 Hz (Slaney scale, Slaney normalisation),
 *   magnitude sqrt(re^2 + im^2 + 1e-9), natural log of clamp(x, min=1e-5),
 *   and framing with NO centring: the signal is first padded by
 *   (n_fft - hop) / 2 = 768 samples of reflection on both sides, then frame
 *   t covers padded samples [t * hop, t * hop + n_fft).
 *
 * The filterbank itself is shared with MelSpectrogram: at fmin 0 / fmax
 * 22050 its Slaney bank matches librosa to float precision (verified during
 * the BigVGAN preparation work), and its log floor is now a constructor
 * parameter, so only the framing is reimplemented here.
 */
class BigVGANMel
{
public:
    static constexpr int kSampleRate = 44100;
    static constexpr int kNumMels = 128;
    static constexpr int kHopSize = 512;
    static constexpr int kFftSize = 2048;

    /**
     * Compute the BigVGAN mel for a mono 44.1kHz waveform.
     * @return Log mel [T, 128], T = floor((numSamples - hop) / hop) + 1 for
     *         inputs of at least one hop; empty for empty input.
     */
    std::vector<std::vector<float>> compute(const float *audio, int numSamples);

private:
    MelSpectrogram mel{kSampleRate, kFftSize, kHopSize, kNumMels,
                       0.0f, 22050.0f, 1e-5f};
};
