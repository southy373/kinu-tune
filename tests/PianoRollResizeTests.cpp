// Batch 3.1 regression tests for the piano roll's view state.
//
// The batch 3 trial reported segments (and the waveform) vanishing when
// the left panel was closed and when the window was resized. Both go
// through resized() -> updateScrollBars(), and the vertical view state
// was the one piece not re-validated there: the scrollbar's JUCE-side
// clamping (which can move the range start on its own) fed back into
// scrollY unclamped through scrollBarMoved(). These tests pin the
// invariant that the view range stays valid - and the fitted segments
// stay on screen - across resizes, including the zoomed-out regime
// where the window is taller than the whole pitch space. They also pin
// the initial fit's pitch margin (batch 3.1 item: the first-open view
// was too large).
#include "../Source/UI/MainComponent.h"
#include "../Source/UI/PianoRollComponent.h"
#include "TestAssert.h"
#include <iostream>
#include <limits>

// The fit-margin helper is private to MainComponent; reach it through
// the same friend hook AuditionHandoffTests uses.
struct MainComponentTestAccess
{
  static float fitMarginSemitones()
  {
    return MainComponent::kFitPitchMarginSemitones;
  }
  static std::pair<float, float> pitchRangeWithFitMargin(float minMidi,
                                                         float maxMidi)
  {
    return MainComponent::pitchRangeWithFitMargin(minMidi, maxMidi);
  }
};

namespace {

void drainMessages()
{
  juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
}

double maxScrollYFor(const PianoRollComponent &roll)
{
  const double totalHeight =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * roll.getPixelsPerSemitone();
  return std::max(0.0, totalHeight - roll.getVisibleContentHeight());
}

bool scrollYInRange(const PianoRollComponent &roll)
{
  return roll.getScrollY() >= 0.0 &&
         roll.getScrollY() <= maxScrollYFor(roll) + 0.5;
}

// Vertical span of a note in view coordinates (0 = top of the content
// area), using the same mapping the component draws with.
void noteSpan(const PianoRollComponent &roll, float midi, double &top,
              double &bottom)
{
  const double pps = roll.getPixelsPerSemitone();
  top = (MAX_MIDI_NOTE - (midi + 1.0)) * pps - roll.getScrollY();
  bottom = (MAX_MIDI_NOTE - midi) * pps - roll.getScrollY();
}

bool noteOnScreen(const PianoRollComponent &roll, float midi)
{
  double top, bottom;
  noteSpan(roll, midi, top, bottom);
  return bottom > 0.0 && top < roll.getVisibleContentHeight();
}

Project makeProject()
{
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = SAMPLE_RATE;
  const int frames = 200;
  audio.f0.assign(frames, 220.0f);
  audio.basePitch.assign(frames, 57.0f);
  audio.deltaPitch.assign(frames, 0.0f);
  audio.waveform.setSize(1, frames * HOP_SIZE);
  // Segments spanning midi 58..67.
  project.addNote(Note(10, 60, 58));
  project.addNote(Note(70, 120, 62));
  project.addNote(Note(130, 190, 67));
  return project;
}

} // namespace

int main()
{
  juce::ScopedJuceInitialiser_GUI gui;

  // The fit margin widens the analyzed range symmetrically and clamps
  // to the representable pitch space.
  CHECK(MainComponentTestAccess::fitMarginSemitones() == 3.0f);
  auto range = MainComponentTestAccess::pitchRangeWithFitMargin(60.0f, 72.0f);
  CHECK(range.first == 57.0f && range.second == 75.0f);
  range = MainComponentTestAccess::pitchRangeWithFitMargin(
      static_cast<float>(MIN_MIDI_NOTE) + 1.0f,
      static_cast<float>(MAX_MIDI_NOTE) - 1.0f);
  CHECK(range.first == static_cast<float>(MIN_MIDI_NOTE));
  CHECK(range.second == static_cast<float>(MAX_MIDI_NOTE));

  Project project = makeProject();

  PianoRollComponent roll;
  // Mirror what zoom-to-full requests in the app
  // (MainComponent::zoomToFull): horizontal fit to the whole timeline,
  // vertical fit to the analyzed range widened by the fit margin.
  roll.onZoomFullRequested = [&roll, &project] {
    roll.setPixelsPerSecond(0.0f, false);
    float minMidi = std::numeric_limits<float>::max();
    float maxMidi = std::numeric_limits<float>::lowest();
    for (const auto &note : project.getNotes())
    {
      if (note.isRest())
        continue;
      minMidi = std::min(minMidi, static_cast<float>(note.getMidiNote()));
      maxMidi = std::max(maxMidi, static_cast<float>(note.getMidiNote()));
    }
    const auto fit =
        MainComponentTestAccess::pitchRangeWithFitMargin(minMidi, maxMidi);
    roll.fitPitchRangeToView(fit.first, fit.second);
  };

  roll.setSize(900, 500);
  roll.setProject(&project);
  roll.setSize(901, 500); // a resized() with the project bound
  // Since 0.4.2 opening no longer fits automatically; invoke the fit
  // request (the app's zoom-to-full) directly.
  roll.onZoomFullRequested();
  drainMessages();

  // After the fit every segment is on screen - strictly inside
  // the view, not flush against its edges, because of the margin.
  for (const auto &note : project.getNotes())
    CHECK(noteOnScreen(roll, static_cast<float>(note.getMidiNote())));
  CHECK(scrollYInRange(roll));
  {
    double top, bottom;
    noteSpan(roll, 67.0f, top, bottom);
    CHECK(top > 0.0);
    noteSpan(roll, 58.0f, top, bottom);
    CHECK(bottom < roll.getVisibleContentHeight());
  }
  // The margin makes the fit looser than the bare note range (58..67,
  // 10 semitones) would: pps must be below visibleHeight / 10.
  CHECK(roll.getPixelsPerSemitone() <
        roll.getVisibleContentHeight() / 10.0f);

  // Growing the window (the left panel sliding shut widens the roll the
  // same way) must keep every segment on screen.
  roll.setSize(1280, 640);
  drainMessages();
  for (const auto &note : project.getNotes())
    CHECK(noteOnScreen(roll, static_cast<float>(note.getMidiNote())));
  CHECK(scrollYInRange(roll));

  // Shrinking it again likewise.
  roll.setSize(760, 420);
  drainMessages();
  for (const auto &note : project.getNotes())
    CHECK(noteOnScreen(roll, static_cast<float>(note.getMidiNote())));
  CHECK(scrollYInRange(roll));

  // The extreme regime: zoomed fully out (66 semitones * 8 px = 528 px
  // of pitch space) in a window much taller than that. The view must
  // clamp to the top of the pitch space instead of scrolling into the
  // void below it - the state the unclamped scrollbar feedback used to
  // leave behind on resize.
  roll.setPixelsPerSemitone(MIN_PIXELS_PER_SEMITONE);
  roll.setSize(1000, 1100);
  drainMessages();
  CHECK(roll.getVisibleContentHeight() >
        (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * MIN_PIXELS_PER_SEMITONE);
  CHECK(roll.getScrollY() == 0.0);
  CHECK(scrollYInRange(roll));
  for (const auto &note : project.getNotes())
    CHECK(noteOnScreen(roll, static_cast<float>(note.getMidiNote())));

  // And back to a normal size: still consistent, segments visible.
  roll.setSize(900, 500);
  drainMessages();
  CHECK(scrollYInRange(roll));
  for (const auto &note : project.getNotes())
    CHECK(noteOnScreen(roll, static_cast<float>(note.getMidiNote())));

  std::cout << "PianoRollResizeTests passed" << std::endl;
  return 0;
}
