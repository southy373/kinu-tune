#pragma once

#include <cmath>

// Audio constants
constexpr int SAMPLE_RATE = 44100;
constexpr int HOP_SIZE = 512;
constexpr int WIN_SIZE = 2048;
constexpr int N_FFT = 2048;
constexpr int NUM_MELS = 128;
constexpr float FMIN = 40.0f;
constexpr float FMAX = 16000.0f;

// MIDI constants
constexpr int MIN_MIDI_NOTE = 24; // C1
constexpr int MAX_MIDI_NOTE = 89; // F6
constexpr int MIDI_A4 = 69;
constexpr float FREQ_A4 = 440.0f;

// UI constants
constexpr float DEFAULT_PIXELS_PER_SECOND = 100.0f;
constexpr float DEFAULT_PIXELS_PER_SEMITONE = 45.0f;
// Widest horizontal zoom. Low enough that "Zoom Full" (Shift+F) fits a
// song-length take (about 9 minutes in 1100 px); the piano roll's own
// limit is the larger of this and "whole timeline in view".
constexpr float MIN_PIXELS_PER_SECOND = 2.0f;
// Batch 3.2: the maximum zoom-in was raised 4x in both axes for fine
// narration pitch work. At 2000 px/s one analysis frame (512 samples)
// spans ~23 px, so per-frame pitch and waveform detail is editable;
// at 480 px/semitone one cent spans ~4.8 px, so few-cent movements
// are visible and grabbable. Every consumer (setters, scroll clamps,
// sliders, overview, the batch 3.2 capture/restore validation) reads
// these constants, so the limits stay consistent everywhere.
constexpr float MAX_PIXELS_PER_SECOND = 2000.0f;
constexpr float MIN_PIXELS_PER_SEMITONE = 8.0f;
constexpr float MAX_PIXELS_PER_SEMITONE = 480.0f;
constexpr float DEFAULT_EMPTY_PROJECT_DURATION_SECONDS = 30.0f;

// UI colors moved to Source/Utils/UI/Theme.h

// Utility functions
inline float midiToFreq(float midi) {
  return FREQ_A4 * std::pow(2.0f, (midi - MIDI_A4) / 12.0f);
}

inline float freqToMidi(float freq) {
  if (freq <= 0.0f)
    return 0.0f;
  return 12.0f * std::log2(freq / FREQ_A4) + MIDI_A4;
}

inline int secondsToFrames(float seconds) {
  return static_cast<int>(seconds * SAMPLE_RATE / HOP_SIZE);
}

inline float framesToSeconds(int frames) {
  return static_cast<float>(frames) * HOP_SIZE / SAMPLE_RATE;
}

inline float framesToSeconds(float frames) {
  return frames * HOP_SIZE / SAMPLE_RATE;
}
