// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

// Batch 5.1 regression (dry-leak investigation, 2026-11-11): the
// published composite must not carry a perceptible layer of the
// ORIGINAL pitch under a pitch-shifted render. The investigation
// measured the composite waveform directly (synthesizeRegion driven
// to completion, +12 semitones, harmonic-stack voice surrogate) and
// found the original-pitch component at -44 dB or below on every
// engine. This test pins the PSOLA figure as a regression bound -
// original-pitch amplitude over the voiced middle <= -40 dB of the
// middle RMS - so a future change that starts mixing dry signal into
// the composite (or stops replacing it) fails here instead of in a
// user's headphones. It is also the harness the cross-version render
// comparisons build on.
//
// Batch 5.2 regression (global-percent-after-undo investigation,
// 2026-11-12): the same measurement with the note left DIRTY but
// neutral - the state a pitch-edit Undo arrives in. The synthesizer's
// neutral-reset path used to treat only a global formant shift as a
// project-wide edit, so with the global pitch % as the only edit it
// overwrote the correctly shifted render with the original audio and
// the % appeared dead. A protection case pins the other side: with
// no project-wide edit at all, a dirty neutral note must still reset
// to the pristine original, sample for sample.
#include "../Source/Audio/Synthesis/IncrementalSynthesizer.h"
#include "../Source/Models/Project.h"
#include "../Source/Utils/Constants.h"
#include "TestAssert.h"

#include <cmath>
#include <iostream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr int kSampleRate = 44100;
constexpr double kSourceF0 = 110.0;
// MIDI note of 110 Hz: 69 + 12 * log2(110 / 440) = 45.
constexpr float kSourceMidi = 45.0f;
constexpr int kNumFrames = 200;
constexpr int kNumSamples = kNumFrames * HOP_SIZE;

// Harmonic stack (sawtooth-like voice surrogate), same construction
// as PitchShiftEngineTests: fundamental plus 12 harmonics at 1/h,
// overall scale 0.25. A single sine is not usable here - PSOLA grain
// overlap can cancel it and fake a perfect score.
float harmonicSample(long absoluteSample) {
  double sum = 0.0;
  for (int h = 1; h <= 12; ++h)
    sum += std::sin(2.0 * M_PI * kSourceF0 * h *
                    static_cast<double>(absoluteSample) / kSampleRate) /
           h;
  return static_cast<float>(0.25 * sum);
}

// Single-frequency amplitude over the middle half, FormantShiftTests'
// convention: hypot(re, im) / window length, phase referenced to the
// absolute sample index.
double amplitudeAt(const juce::AudioBuffer<float> &audio, double hz) {
  const int n = audio.getNumSamples();
  const int begin = n / 4;
  const int end = 3 * n / 4;
  const float *p = audio.getReadPointer(0);
  double re = 0.0, im = 0.0;
  for (int i = begin; i < end; ++i) {
    const double phase = 2.0 * M_PI * hz * i / kSampleRate;
    re += p[i] * std::cos(phase);
    im += p[i] * std::sin(phase);
  }
  return std::hypot(re, im) / static_cast<double>(end - begin);
}

double middleRms(const juce::AudioBuffer<float> &audio) {
  const int n = audio.getNumSamples();
  const int begin = n / 4;
  const int end = 3 * n / 4;
  const float *p = audio.getReadPointer(0);
  double energy = 0.0;
  for (int i = begin; i < end; ++i)
    energy += static_cast<double>(p[i]) * p[i];
  return std::sqrt(energy / static_cast<double>(end - begin));
}

// The one-note project every case shares: a fully voiced harmonic
// stack at a constant 110 Hz, the composite starting as the pristine
// original, as after analysis. With markNoteDirty the note is left
// carrying the marks a pitch-edit Undo leaves behind
// (PitchDrawingAction::apply writes the old parameters back and marks
// the note dirty + synth-dirty plus the note-edit dirty range) while
// its parameters stay exactly neutral.
void buildProject(Project &project, bool markNoteDirty) {
  auto &audio = project.getAudioData();
  audio.sampleRate = kSampleRate;

  audio.originalWaveform.setSize(1, kNumSamples);
  for (int i = 0; i < kNumSamples; ++i)
    audio.originalWaveform.setSample(0, i, harmonicSample(i));
  // The composite starts as the pristine original, as after analysis.
  audio.waveform.makeCopyOf(audio.originalWaveform);

  audio.f0.assign(kNumFrames, static_cast<float>(kSourceF0));
  audio.basePitch.assign(kNumFrames, kSourceMidi);
  audio.deltaPitch.assign(kNumFrames, 0.0f);
  audio.voicedMask.assign(kNumFrames, true);
  audio.vadMask.assign(kNumFrames, true);
  // synthesizeRegion requires a mel even on the classic path (it only
  // gates on non-emptiness there); the PSOLA render never reads it.
  audio.melSpectrogram.assign(kNumFrames,
                              std::vector<float>(NUM_MELS, 0.0f));

  Note note(0, kNumFrames, kSourceMidi);
  note.setSrcStartFrame(0);
  note.setSrcEndFrame(kNumFrames);
  note.setOriginalMidiNote(kSourceMidi);
  project.addNote(note);

  if (markNoteDirty) {
    auto &stored = project.getNotes().front();
    stored.markDirty();
    stored.markSynthDirty();
    project.markNoteEditDirtyRange(0, kNumFrames);
  }
}

// Drives one PSOLA render to completion; completion is posted back
// through the message loop.
bool renderPsola(Project &project) {
  IncrementalSynthesizer synth;
  synth.setProject(&project);
  synth.setSynthesisEngine(SynthesisEngineType::Psola);
  synth.setPitchShiftEngine(PitchShiftEngine::Psola);

  bool completed = false;
  bool succeeded = false;
  synth.synthesizeRegion(nullptr, [&](bool success) {
    completed = true;
    succeeded = success;
  });
  for (int i = 0; i < 2400 && !completed; ++i)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  CHECK(completed);
  return succeeded;
}

double maxAbsDifferenceFromOriginal(const Project &project) {
  const auto &audio = project.getAudioData();
  double maxDiff = 0.0;
  for (int i = 0; i < audio.waveform.getNumSamples(); ++i)
    maxDiff = std::max(
        maxDiff, static_cast<double>(std::abs(audio.waveform.getSample(0, i) -
                                              audio.originalWaveform.getSample(0, i))));
  return maxDiff;
}

} // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI gui;

  // Case 1: the original dry-leak regression. The note is clean (the
  // state right after analysis) and the only edit is the global
  // pitch % at 200 (+12 semitones).
  {
    Project project;
    buildProject(project, false);

    // The global-% form of the edit, mirroring MainComponent::
    // globalRenderCommitted: pitch % at 200 and the F0 dirty range
    // over every frame.
    project.setPitchPercent(200.0f);
    project.setF0DirtyRange(0, kNumFrames);

    CHECK(renderPsola(project));

    const auto &audio = project.getAudioData();
    const double rms = middleRms(audio.waveform);
    CHECK(rms > 0.05); // the render is real audio, not silence

    // The shifted fundamental must dominate: the render really moved.
    const double shifted = amplitudeAt(audio.waveform, 2.0 * kSourceF0);
    CHECK(shifted > 0.05);

    // The regression bound: the original pitch left in the voiced
    // middle stays at or below -40 dB of the middle RMS (amplitude
    // ratio 0.01). The 0.5.0 measurement was 0.0066 (-43.6 dB).
    const double residual = amplitudeAt(audio.waveform, kSourceF0);
    const double ratio = residual / rms;
    std::cout << "original-pitch residual ratio: " << ratio << "\n";
    CHECK(ratio <= 0.01);
  }

  // Case 2 (batch 5.2 regression): the note is dirty but neutral -
  // the state a full Undo of pitch edits arrives in - and the only
  // edit is still the global pitch %. The % must reach the audio:
  // before the fix, the neutral-reset path restored the original over
  // the shifted render and every check below failed.
  {
    Project project;
    buildProject(project, true);
    CHECK(project.getNotes().front().isDirty());
    CHECK(project.getNotes().front().isNeutralForOriginalWaveform());

    project.setPitchPercent(200.0f);
    project.setF0DirtyRange(0, kNumFrames);
    CHECK(project.hasProjectWideEdit());

    CHECK(renderPsola(project));

    const auto &audio = project.getAudioData();
    // The composite actually moved away from the original...
    CHECK(maxAbsDifferenceFromOriginal(project) > 0.01);
    const double rms = middleRms(audio.waveform);
    CHECK(rms > 0.05);
    // ...to the shifted pitch, with the original pitch no louder
    // than the case-1 bound.
    const double shifted = amplitudeAt(audio.waveform, 2.0 * kSourceF0);
    CHECK(shifted > 0.05);
    const double residual = amplitudeAt(audio.waveform, kSourceF0);
    const double ratio = residual / rms;
    std::cout << "dirty+neutral original-pitch residual ratio: " << ratio
              << "\n";
    CHECK(ratio <= 0.01);
  }

  // Case 3 (protection): the same dirty-but-neutral note with NO
  // project-wide edit keeps the reset behaviour the mechanism exists
  // for - the composite is the pristine original, sample for sample.
  {
    Project project;
    buildProject(project, true);
    project.setF0DirtyRange(0, kNumFrames);
    CHECK(!project.hasProjectWideEdit());

    CHECK(renderPsola(project));

    const auto &audio = project.getAudioData();
    CHECK(audio.waveform.getNumSamples() ==
          audio.originalWaveform.getNumSamples());
    for (int i = 0; i < audio.waveform.getNumSamples(); ++i)
      CHECK(audio.waveform.getSample(0, i) ==
            audio.originalWaveform.getSample(0, i));
  }

  std::cout << "Original pitch residual tests passed\n";
  return 0;
}
