// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include <atomic>
#include <juce_core/juce_core.h>

/**
 * Which algorithm performs the pitch-shift stage of a render.
 *
 * This is independent of SynthesisEngineType: that one chooses what
 * rebuilds the shifted audio (the classic path or the BigVGAN AI
 * resynthesis), this one chooses how the pitch itself is moved before
 * that stage. PSOLA is the original engine and stays the default, so a
 * settings file that predates the choice behaves exactly as before.
 */
enum class PitchShiftEngine
{
  Psola,
  RubberBand,
  SignalsmithStretch
};

inline juce::String pitchShiftEngineToString(PitchShiftEngine engine)
{
  switch (engine)
  {
    case PitchShiftEngine::Psola:
      return "psola";
    case PitchShiftEngine::RubberBand:
      return "rubberband";
    case PitchShiftEngine::SignalsmithStretch:
      return "signalsmith";
  }
  return "psola";
}

inline PitchShiftEngine stringToPitchShiftEngine(const juce::String &value)
{
  if (value == "rubberband")
    return PitchShiftEngine::RubberBand;
  if (value == "signalsmith")
    return PitchShiftEngine::SignalsmithStretch;
  return PitchShiftEngine::Psola;
}

/**
 * The pitch-shift engine shared by every rendering controller of one
 * plug-in instance, mirroring SynthesisEnginePreference. Owned by the
 * processor (PluginProcessor::renderPitchShiftEngine), handed to each
 * EditorController as it is created; the editor writes through its
 * bound controller, so a render controller that has no editor still
 * uses the chosen engine.
 */
struct PitchShiftEnginePreference
{
  std::atomic<PitchShiftEngine> value{PitchShiftEngine::Psola};
};
