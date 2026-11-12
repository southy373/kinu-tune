// Batch 3.2 tests for the piano roll's view-state integrity.
//
// The batch 3.1 resize tests only ever read the public viewState
// getters, so they could not see the failure the 3.1 trial reported:
// the renderers draw from the CoordinateMapper, and the two stores can
// disagree (or hold non-finite values) while the getters look fine.
// These tests assert on BOTH stores, on the notes actually falling
// inside the visible range, and - in the paint scenario - on pixels
// really being drawn where a note should be.
#include "../Source/Audio/EditorController.h"
#include "../Source/UI/Main/SettingsManager.h"
#include "../Source/UI/MainComponent.h"
#include "../Source/UI/PianoRollComponent.h"
#include "../Source/Undo/PitchUndoManager.h"
#include "TestAssert.h"
#include <cmath>
#include <iostream>
#include <limits>

// Reaches the CoordinateMapper and the private helpers through the
// friend hook declared in PianoRollComponent.h.
struct PianoRollComponentTestAccess
{
  static CoordinateMapper &mapper(PianoRollComponent &roll)
  {
    return *roll.coordMapper;
  }
  static int notesInVisibleRange(PianoRollComponent &roll)
  {
    return roll.countNotesInVisibleRange();
  }
  static bool valuesFinite(PianoRollComponent &roll)
  {
    return roll.viewStateValuesAreFinite();
  }
  // Writes the viewState store directly, bypassing the guarded
  // setters - the corruption the guards exist to heal.
  static void corruptViewStateScrollY(PianoRollComponent &roll, double y)
  {
    roll.viewState.scrollY = y;
  }
  // fitViewToContent() is private (the recovery path); the recovery
  // fit scenario drives it through this hook.
  static void fitViewToContent(PianoRollComponent &roll)
  {
    roll.fitViewToContent();
  }
};

struct MainComponentTestAccess
{
  static PianoRollComponent &roll(MainComponent &mc) { return mc.pianoRoll; }

  // Binds the controller's project the way the plug-in does, so
  // MainComponent::getProject() (and therefore zoomToFull) sees the
  // same project as the piano roll.
  static void wireController(MainComponent &mc, EditorController &controller,
                             PitchUndoManager &history)
  {
    mc.editorController = &controller;
    mc.undoManager = &history;
    mc.pianoRoll.setProject(controller.getProject());
    mc.pianoRoll.setUndoManager(&history);
  }
};

namespace {

void drainMessages()
{
  juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
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
  project.addNote(Note(10, 60, 58));
  project.addNote(Note(70, 120, 62));
  project.addNote(Note(130, 190, 67));
  return project;
}

bool storesAgree(PianoRollComponent &roll)
{
  auto &mapper = PianoRollComponentTestAccess::mapper(roll);
  return std::abs(roll.getScrollX() - mapper.getScrollX()) < 0.01 &&
         std::abs(roll.getScrollY() - mapper.getScrollY()) < 0.01 &&
         std::abs(static_cast<double>(roll.getPixelsPerSecond()) -
                  static_cast<double>(mapper.getPixelsPerSecond())) < 0.01 &&
         std::abs(static_cast<double>(roll.getPixelsPerSemitone()) -
                  static_cast<double>(mapper.getPixelsPerSemitone())) < 0.01;
}

int visibleNotes(PianoRollComponent &roll)
{
  return PianoRollComponentTestAccess::notesInVisibleRange(roll);
}

// A standalone roll fitted the way the app's zoom-to-full fits it
// (MainComponent::zoomToFull equivalent, as in PianoRollResizeTests).
// Since 0.4.2 opening a project no longer fits automatically, the
// harness invokes the fit request itself after binding.
void fitRoll(PianoRollComponent &roll, Project &project)
{
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
    roll.fitPitchRangeToView(minMidi - 3.0f, maxMidi + 3.0f);
  };
  roll.setSize(900, 500);
  roll.setProject(&project);
  roll.setSize(901, 500); // a resized() with the project bound
  roll.onZoomFullRequested();
  drainMessages();
}

} // namespace

int main()
{
  juce::ScopedJuceInitialiser_GUI gui;
  const double nanD = std::numeric_limits<double>::quiet_NaN();
  const float nanF = std::numeric_limits<float>::quiet_NaN();

  Project project = makeProject();

  // --- A: non-finite values offered through the public setters are
  // refused, and the fitted view survives untouched.
  {
    PianoRollComponent roll;
    fitRoll(roll, project);
    CHECK(visibleNotes(roll) == 3);
    CHECK(storesAgree(roll));

    roll.setScrollX(nanD);
    roll.setScrollY(nanD);
    roll.setPixelsPerSecond(nanF);
    roll.setPixelsPerSemitone(nanF);
    drainMessages();

    CHECK(PianoRollComponentTestAccess::valuesFinite(roll));
    CHECK(storesAgree(roll));
    CHECK(visibleNotes(roll) == 3);
  }

  // --- B: corruption written straight into the viewState store (the
  // setters bypassed) is healed by the next resize, back to a fitted,
  // drawable state - not merely clamped.
  {
    PianoRollComponent roll;
    fitRoll(roll, project);
    PianoRollComponentTestAccess::corruptViewStateScrollY(roll, nanD);
    CHECK(!PianoRollComponentTestAccess::valuesFinite(roll));

    roll.setSize(1000, 560); // resized() -> updateScrollBars()
    drainMessages();

    CHECK(PianoRollComponentTestAccess::valuesFinite(roll));
    CHECK(storesAgree(roll));
    CHECK(visibleNotes(roll) == 3);
  }

  // --- C: corruption in the CoordinateMapper store only (what paint()
  // reads) is likewise healed by a resize.
  {
    PianoRollComponent roll;
    fitRoll(roll, project);
    // CoordinateMapper's zoom setter jlimits, and jlimit passes NaN
    // straight through - the injection the field failure implies.
    PianoRollComponentTestAccess::mapper(roll).setPixelsPerSecond(nanF);
    CHECK(!PianoRollComponentTestAccess::valuesFinite(roll));

    roll.setSize(1024, 560);
    drainMessages();

    CHECK(PianoRollComponentTestAccess::valuesFinite(roll));
    CHECK(storesAgree(roll));
    CHECK(visibleNotes(roll) == 3);
  }

  // --- D: a finite zoom divergence between the stores (the view on
  // screen no longer matches what viewport capture would save) is
  // reconciled by updateScrollBars() on the next resize.
  {
    PianoRollComponent roll;
    fitRoll(roll, project);
    PianoRollComponentTestAccess::mapper(roll).setPixelsPerSecond(321.0f);
    CHECK(!storesAgree(roll));

    roll.setSize(1025, 560);
    drainMessages();

    CHECK(storesAgree(roll));
    CHECK(visibleNotes(roll) == 3);
  }

  // --- E: the editor close/open path through MainComponent, following
  // the field reproduction (visible -> close -> reopen -> blank).
  {
    EditorController controller(false);
    PitchUndoManager history(100);
    MainComponent mc(false);
    {
      auto &bound = *controller.getProject();
      auto &audio = bound.getAudioData();
      audio.sampleRate = SAMPLE_RATE;
      audio.f0.assign(200, 220.0f);
      audio.basePitch.assign(200, 57.0f);
      audio.deltaPitch.assign(200, 0.0f);
      audio.waveform.setSize(1, 200 * HOP_SIZE);
      bound.addNote(Note(10, 60, 58));
      bound.addNote(Note(70, 120, 62));
      bound.addNote(Note(130, 190, 67));
    }
    MainComponentTestAccess::wireController(mc, controller, history);
    auto &roll = MainComponentTestAccess::roll(mc);
    mc.setSize(1100, 650);
    drainMessages();
    if (roll.getWidth() <= 0 || roll.getHeight() <= 0)
      roll.setSize(1000, 560); // headless layout may not size the roll
    drainMessages();
    CHECK(roll.getWidth() > 0 && roll.getHeight() > 0);
    CHECK(visibleNotes(roll) == 3);

    // Capture of a healthy view round-trips into the same view.
    auto captured = mc.getViewportState();
    CHECK(captured.valid);
    CHECK(std::isfinite(captured.scrollX) && std::isfinite(captured.scrollY));
    mc.restoreViewportState(captured);
    drainMessages();
    CHECK(visibleNotes(roll) == 3);
    CHECK(storesAgree(roll));

    // A stored state with a non-finite zoom (what the pre-3.2 capture
    // could persist) must not blank the roll on restore: the invalid
    // fields are rejected, the rest is applied, and the captured state
    // afterwards is clean and marked by the actual values.
    MainViewViewportState corrupt;
    corrupt.valid = true;
    corrupt.scrollX = nanD;
    corrupt.scrollY = 1.0e9;
    corrupt.pixelsPerSecond = nanF;
    corrupt.pixelsPerSemitone = -5.0f;
    mc.restoreViewportState(corrupt);
    drainMessages();
    CHECK(PianoRollComponentTestAccess::valuesFinite(roll));
    CHECK(storesAgree(roll));
    CHECK(visibleNotes(roll) == 3);
    auto afterCorrupt = mc.getViewportState();
    CHECK(std::isfinite(afterCorrupt.scrollX) &&
          std::isfinite(afterCorrupt.scrollY) &&
          std::isfinite(afterCorrupt.pixelsPerSecond) &&
          std::isfinite(afterCorrupt.pixelsPerSemitone));
    CHECK(afterCorrupt.valid);

    // A finite-but-degenerate stored state (extreme scroll at maximum
    // vertical zoom parks the view far below the pitch content) is
    // detected as content-free at restore time and refitted.
    MainViewViewportState degenerate;
    degenerate.valid = true;
    degenerate.scrollX = 0.0;
    degenerate.scrollY = 1.0e9;
    degenerate.pixelsPerSecond = 100.0f;
    degenerate.pixelsPerSemitone = MAX_PIXELS_PER_SEMITONE;
    mc.restoreViewportState(degenerate);
    drainMessages();
    CHECK(PianoRollComponentTestAccess::valuesFinite(roll));
    CHECK(storesAgree(roll));
    CHECK(visibleNotes(roll) == 3);

    // The raised zoom maximum is inside the capture/restore validity
    // range (the guards read the same constants), while a zoom beyond
    // it is rejected as degenerate and never captured back.
    MainViewViewportState maxZoom;
    maxZoom.valid = true;
    maxZoom.scrollX = 0.0;
    maxZoom.scrollY = roll.getScrollY();
    maxZoom.pixelsPerSecond = MAX_PIXELS_PER_SECOND;
    maxZoom.pixelsPerSemitone = roll.getPixelsPerSemitone();
    mc.restoreViewportState(maxZoom);
    drainMessages();
    auto maxCaptured = mc.getViewportState();
    CHECK(maxCaptured.valid);
    CHECK(maxCaptured.pixelsPerSecond == MAX_PIXELS_PER_SECOND);

    MainViewViewportState beyondMax;
    beyondMax.valid = true;
    beyondMax.scrollX = 0.0;
    beyondMax.scrollY = roll.getScrollY();
    beyondMax.pixelsPerSecond = MAX_PIXELS_PER_SECOND * 4.0f;
    beyondMax.pixelsPerSemitone = roll.getPixelsPerSemitone();
    mc.restoreViewportState(beyondMax);
    drainMessages();
    auto beyondCaptured = mc.getViewportState();
    CHECK(beyondCaptured.pixelsPerSecond <= MAX_PIXELS_PER_SECOND);
    CHECK(PianoRollComponentTestAccess::valuesFinite(roll));

    roll.setProject(nullptr);
  }

  // --- G: the raised maximum zoom (batch 3.2: 2000 px/s, 480
  // px/semitone) is reachable through the setters, clamps beyond it,
  // keeps the two stores in agreement, and the visible-range math
  // still works at the limit.
  {
    PianoRollComponent roll;
    fitRoll(roll, project);

    roll.setPixelsPerSecond(MAX_PIXELS_PER_SECOND, false);
    CHECK(roll.getPixelsPerSecond() == MAX_PIXELS_PER_SECOND);
    CHECK(storesAgree(roll));
    // Scrolled to the start at maximum horizontal zoom, the first
    // note still intersects the visible range.
    CHECK(visibleNotes(roll) >= 1);

    roll.setPixelsPerSecond(MAX_PIXELS_PER_SECOND * 4.0f, false);
    CHECK(roll.getPixelsPerSecond() == MAX_PIXELS_PER_SECOND);
    CHECK(PianoRollComponentTestAccess::valuesFinite(roll));

    const float anchorCenter =
        static_cast<float>(roll.getVisibleContentHeight()) * 0.5f;
    roll.setPixelsPerSemitone(MAX_PIXELS_PER_SEMITONE, anchorCenter);
    CHECK(roll.getPixelsPerSemitone() == MAX_PIXELS_PER_SEMITONE);
    CHECK(storesAgree(roll));
    // At maximum zoom on both axes the horizontal window (parked at
    // the content start) and the vertical anchor (the fitted range's
    // centre pitch) do not coincide on any single note, so scroll to
    // the middle note before expecting it in range. (The count's
    // original note timing - raw frames over the sample rate - made
    // every note pass the horizontal test at any scroll position,
    // which is what hid this in the first version of this block.)
    roll.setScrollX(0.9 * MAX_PIXELS_PER_SECOND);
    CHECK(visibleNotes(roll) >= 1);

    roll.setPixelsPerSemitone(MAX_PIXELS_PER_SEMITONE * 4.0f, anchorCenter);
    CHECK(roll.getPixelsPerSemitone() == MAX_PIXELS_PER_SEMITONE);
    CHECK(PianoRollComponentTestAccess::valuesFinite(roll));
    CHECK(storesAgree(roll));
  }

  // --- F: paint() really draws the notes after a corruption + resize
  // sequence. Compared against the same roll with no project bound,
  // so the assertion does not depend on theme colours: inside a
  // note's rectangle, the with-project render must differ from the
  // empty render on far more pixels than grid lines alone explain.
  {
    PianoRollComponent roll;
    fitRoll(roll, project);
    PianoRollComponentTestAccess::corruptViewStateScrollY(roll, nanD);
    roll.setSize(1100, 600);
    roll.setSize(760, 420);
    roll.setSize(1000, 560);
    drainMessages();
    CHECK(visibleNotes(roll) == 3);

    juce::Image withNotes(juce::Image::RGB, roll.getWidth(),
                          roll.getHeight(), true);
    {
      juce::Graphics g(withNotes);
      roll.paint(g);
    }

    roll.setProject(nullptr);
    juce::Image empty(juce::Image::RGB, roll.getWidth(), roll.getHeight(),
                      true);
    {
      juce::Graphics g(empty);
      roll.paint(g);
    }

    // The middle note (midi 62, frames 70..120) in component
    // coordinates, from the CoordinateMapper the renderers use.
    auto &mapper = PianoRollComponentTestAccess::mapper(roll);
    const double sampleRate = project.getAudioData().sampleRate;
    const int x0 = PianoRollComponent::pianoKeysWidth + 2 +
                   static_cast<int>((70.0 * HOP_SIZE / sampleRate) *
                                    mapper.getPixelsPerSecond() -
                                    mapper.getScrollX());
    const int x1 = PianoRollComponent::pianoKeysWidth - 2 +
                   static_cast<int>((120.0 * HOP_SIZE / sampleRate) *
                                    mapper.getPixelsPerSecond() -
                                    mapper.getScrollX());
    const int yMid = PianoRollComponent::headerHeight +
                     static_cast<int>((MAX_MIDI_NOTE - 62.0 - 0.5) *
                                          mapper.getPixelsPerSemitone() -
                                      mapper.getScrollY());
    CHECK(x1 > x0 && yMid > 0 && yMid < roll.getHeight());

    int differing = 0, sampled = 0;
    for (int x = x0; x < x1; ++x)
    {
      ++sampled;
      if (withNotes.getPixelAt(x, yMid) != empty.getPixelAt(x, yMid))
        ++differing;
    }
    // The note body fills most of its row; grid/waveform differences
    // alone would stay well under half the row.
    CHECK(sampled > 20);
    CHECK(differing > sampled / 2);
  }

  // --- H: the visible-range count uses the renderer's project-time
  // math. The renderer culls in project time only (Track mode's
  // offset moves the ruler, not the drawing), with note times from
  // framesToSeconds(visual frames). The first version of the count
  // subtracted getTimelineStart() and timed notes as raw frames /
  // sampleRate, so it reported 0 in both cases below even though
  // the notes were on screen.
  {
    PianoRollComponent roll;
    fitRoll(roll, project);

    // Track mode with a host-timeline offset: the scrollable canvas
    // now starts at the host's zero, -30 s in project time, but the
    // fitted view still shows all three notes.
    roll.setTimelineDisplayOffset(30.0);
    roll.setTrackViewMode(true);
    drainMessages();
    CHECK(roll.getTimelineStart() == -30.0);
    CHECK(visibleNotes(roll) == 3);

    // Clip mode again, zoomed in and scrolled so the visible window
    // starts at 1.45 s: only the last note (frames 130..190, about
    // 1.51..2.21 s) intersects it.
    roll.setTrackViewMode(false);
    roll.setTimelineDisplayOffset(0.0);
    roll.setPixelsPerSecond(1000.0f, false);
    roll.setScrollX(1450.0);
    drainMessages();
    CHECK(visibleNotes(roll) == 1);
  }

  // --- I (batch 4): the view-mode default is Clip. The editor is
  // fixed to Clip mode (Track mode's initial fit caused the
  // disappearing-segments bug); a stale Track default here would
  // re-arm the dormant path's assumptions. This constructs a
  // SettingsManager, whose constructor loads any saved config, so it
  // assumes a machine without a saved trackViewMode (true for CI and
  // fresh installs).
  {
    SettingsManager settings;
    CHECK(!settings.getTrackViewMode());
  }

  // --- J (batch 4, diagnosis candidate E): reopening with a stored
  // viewport must not be overwritten by anything automatic. Restoring
  // a distinctive viewport and then resizing must leave the restored
  // values alone. (Batch 4's suppression made this true against the
  // one-time initial fit; since 0.4.2 there is no automatic fit at
  // all, and this scenario guards the restore path staying quiet.)
  {
    auto makeWideProject = [](Project &proj) {
      auto &audio = proj.getAudioData();
      audio.sampleRate = SAMPLE_RATE;
      const int frames = 600;
      audio.f0.assign(frames, 220.0f);
      audio.basePitch.assign(frames, 57.0f);
      audio.deltaPitch.assign(frames, 0.0f);
      audio.waveform.setSize(1, frames * HOP_SIZE);
      proj.addNote(Note(100, 160, 60));
      proj.addNote(Note(300, 360, 64));
    };

    EditorController controllerB(false);
    makeWideProject(*controllerB.getProject());
    PitchUndoManager historyB(100);
    MainComponent mcB(false);
    MainComponentTestAccess::wireController(mcB, controllerB, historyB);
    auto &rollB = MainComponentTestAccess::roll(mcB);
    mcB.setSize(1100, 650);
    drainMessages();
    if (rollB.getWidth() <= 0 || rollB.getHeight() <= 0)
      rollB.setSize(1000, 560);
    drainMessages();

    // A distinctive restored viewport: 300 px/s (the fit of ~7 s of
    // content into this width would land near 150 px/s) scrolled so
    // the second note (frames 300..360, about 3.5..4.2 s, i.e.
    // 1044..1254 px at 300 px/s) is in view. 800 px of scroll stays
    // inside the scroll clamp for any plausible roll width here.
    MainViewViewportState restored;
    restored.valid = true;
    restored.pixelsPerSecond = 300.0f;
    restored.pixelsPerSemitone = rollB.getPixelsPerSemitone();
    restored.scrollX = 800.0;
    restored.scrollY = rollB.getScrollY();
    mcB.restoreViewportState(restored);
    drainMessages();
    CHECK(std::abs(rollB.getPixelsPerSecond() - 300.0f) < 0.5f);
    CHECK(std::abs(rollB.getScrollX() - 800.0) < 1.0);
    CHECK(visibleNotes(rollB) >= 1);

    // The resize that follows a reopen must not run the fit over the
    // restored viewport.
    rollB.setSize(rollB.getWidth() + 37, rollB.getHeight());
    drainMessages();
    CHECK(std::abs(rollB.getPixelsPerSecond() - 300.0f) < 0.5f);
    CHECK(std::abs(rollB.getScrollX() - 800.0) < 1.0);
    CHECK(storesAgree(rollB));
    rollB.setProject(nullptr);

    // Control: the same rebind WITHOUT a restore gets no fit either
    // (0.4.2) - the view stays at the default zoom, under which the
    // whole project is on screen at these sizes.
    EditorController controllerC(false);
    makeWideProject(*controllerC.getProject());
    PitchUndoManager historyC(100);
    MainComponent mcC(false);
    MainComponentTestAccess::wireController(mcC, controllerC, historyC);
    auto &rollC = MainComponentTestAccess::roll(mcC);
    mcC.setSize(1100, 650);
    drainMessages();
    if (rollC.getWidth() <= 0 || rollC.getHeight() <= 0)
      rollC.setSize(1000, 560);
    drainMessages();
    CHECK(visibleNotes(rollC) == 2);
    CHECK(rollC.getPixelsPerSecond() == DEFAULT_PIXELS_PER_SECOND);
    rollC.setProject(nullptr);
  }

  // --- K (batch 4 fix, 0.4.2): a horizontal fit measures the
  // content, not the host extents the playhead has reached. With the
  // host timeline extended to 300 s (the playhead parked far down
  // the song), the scroll ruler is still 300 s, but the fit ruler
  // stays at the ~2.3 s project, and the recovery fit does not
  // compress the content against the 300 s.
  {
    PianoRollComponent roll;
    fitRoll(roll, project);
    roll.extendTimelineTo(300.0);
    CHECK(roll.getTimelineLength() > 250.0);
    CHECK(roll.getFitTimelineLength() > 0.0);
    CHECK(roll.getFitTimelineLength() < 10.0);

    PianoRollComponentTestAccess::fitViewToContent(roll);
    drainMessages();
    CHECK(roll.getPixelsPerSecond() > 100.0f);
    CHECK(visibleNotes(roll) == 3);
    CHECK(storesAgree(roll));

    // Track mode keeps the legacy ruler (the dormant path is
    // untouched by the fix).
    roll.setTrackViewMode(true);
    CHECK(roll.getFitTimelineLength() > 250.0);
    roll.setTrackViewMode(false);
  }

  // --- K2 (batch 4 fix, 0.4.2): the same guarantee through
  // MainComponent::zoomToFull, the path the zoom-full command and
  // Alt+double-click take.
  {
    EditorController controllerD(false);
    {
      auto &bound = *controllerD.getProject();
      auto &audio = bound.getAudioData();
      audio.sampleRate = SAMPLE_RATE;
      audio.f0.assign(200, 220.0f);
      audio.basePitch.assign(200, 57.0f);
      audio.deltaPitch.assign(200, 0.0f);
      audio.waveform.setSize(1, 200 * HOP_SIZE);
      bound.addNote(Note(10, 60, 58));
      bound.addNote(Note(70, 120, 62));
      bound.addNote(Note(130, 190, 67));
    }
    PitchUndoManager historyD(100);
    MainComponent mcD(false);
    MainComponentTestAccess::wireController(mcD, controllerD, historyD);
    auto &rollD = MainComponentTestAccess::roll(mcD);
    mcD.setSize(1100, 650);
    drainMessages();
    if (rollD.getWidth() <= 0 || rollD.getHeight() <= 0)
      rollD.setSize(1000, 560); // headless layout may not size the roll
    drainMessages();
    CHECK(visibleNotes(rollD) == 3);

    rollD.extendTimelineTo(300.0);
    CHECK(rollD.getTimelineLength() > 250.0);
    rollD.onZoomFullRequested(); // == MainComponent::zoomToFull()
    drainMessages();
    CHECK(rollD.getPixelsPerSecond() > 100.0f);
    CHECK(visibleNotes(rollD) == 3);
    CHECK(storesAgree(rollD));
    rollD.setProject(nullptr);
  }

  // --- L (batch 4 fix, 0.4.2): there is no automatic initial fit.
  // Binding a project and laying out - at open, on the track panel's
  // slide, on a window resize - must never rewrite zoom or scroll on
  // its own, whether the segments are there from the start or arrive
  // late (analysis finishing). The fit request fires only when it is
  // invoked (zoom-to-full command, Alt+double-click, recovery).
  {
    Project lateProject;
    {
      auto &audio = lateProject.getAudioData();
      audio.sampleRate = SAMPLE_RATE;
      audio.f0.assign(200, 220.0f);
      audio.basePitch.assign(200, 57.0f);
      audio.deltaPitch.assign(200, 0.0f);
      audio.waveform.setSize(1, 200 * HOP_SIZE);
    }

    PianoRollComponent roll;
    int fitCount = 0;
    roll.onZoomFullRequested = [&fitCount] { ++fitCount; };
    roll.setProject(&lateProject);
    roll.setSize(900, 500); // opening layout settles, still no segments
    drainMessages();
    CHECK(fitCount == 0);

    // Analysis delivers the segments into the same project object.
    lateProject.addNote(Note(10, 60, 58));
    lateProject.addNote(Note(70, 120, 62));
    lateProject.addNote(Note(130, 190, 67));
    roll.setSize(920, 500); // an unrelated resize: the panel slide, say
    drainMessages();
    CHECK(fitCount == 0);
    CHECK(roll.getPixelsPerSecond() == DEFAULT_PIXELS_PER_SECOND);

    // Segments present from the start: still no automatic fit - the
    // view stays exactly at the defaults through layout and resize.
    PianoRollComponent roll2;
    int fitCount2 = 0;
    roll2.onZoomFullRequested = [&fitCount2] { ++fitCount2; };
    roll2.setProject(&project);
    roll2.setSize(900, 500);
    roll2.setSize(1000, 560);
    drainMessages();
    CHECK(fitCount2 == 0);
    CHECK(roll2.getPixelsPerSecond() == DEFAULT_PIXELS_PER_SECOND);
    CHECK(roll2.getPixelsPerSemitone() == DEFAULT_PIXELS_PER_SEMITONE);
  }

  // --- M (batch 5.2): Clip mode's timeline extends left by the
  // region's host lead-in. The project, the analysis window and the
  // archive format are untouched; only the ruler and the scroll
  // range move. With a display offset of 3630 s (a region an hour
  // into the song) the timeline starts at -3630 s in project time,
  // negative scroll clamps against that start with both stores in
  // agreement, a fit still measures the ~2.3 s content and lands on
  // the content start (project time zero) - never on the lead-in -
  // and an offset change that removes the lead-in re-clamps a stale
  // parked scroll instead of leaving it stranded an hour away.
  {
    PianoRollComponent roll;
    fitRoll(roll, project);

    const double fitLengthBefore = roll.getFitTimelineLength();
    roll.setTimelineDisplayOffset(3630.0);
    drainMessages();
    CHECK(roll.getTimelineStart() == -3630.0);
    CHECK(roll.getFitTimelineStart() == 0.0);
    // The fit ruler does not grow with the lead-in.
    CHECK(roll.getFitTimelineLength() == fitLengthBefore);
    CHECK(roll.getFitTimelineLength() < 10.0);

    // Negative scroll reaches the new start and clamps there, the
    // viewState and CoordinateMapper stores agreeing (the mapper's
    // view start tracks the timeline start in Clip mode too, via
    // the batch 5.2 sync in setTimelineDisplayOffset).
    roll.setScrollX(-1.0e9);
    drainMessages();
    CHECK(std::abs(roll.getScrollX() - roll.getTimelineStart() *
                                           roll.getPixelsPerSecond()) <
          0.01);
    CHECK(storesAgree(roll));

    // The recovery fit lands on the content start and shows every
    // note, exactly as it does without the lead-in.
    PianoRollComponentTestAccess::fitViewToContent(roll);
    drainMessages();
    CHECK(std::abs(roll.getScrollX()) < 0.01);
    CHECK(visibleNotes(roll) == 3);
    CHECK(storesAgree(roll));

    // Park at the far left again, then remove the lead-in: the
    // offset change re-clamps the now out-of-range scroll back to
    // the content start.
    roll.setScrollX(-1.0e9);
    drainMessages();
    CHECK(roll.getScrollX() < -1000.0);
    roll.setTimelineDisplayOffset(0.0);
    drainMessages();
    CHECK(roll.getTimelineStart() == 0.0);
    CHECK(std::abs(roll.getScrollX()) < 0.01);
    CHECK(storesAgree(roll));
    CHECK(visibleNotes(roll) == 3);
  }

  // --- M2 (batch 5.2): the same fit guarantee through
  // MainComponent::zoomToFull with a lead-in present: the fit
  // measures the content and lands on the content start, so all
  // notes stay visible.
  {
    EditorController controllerE(false);
    {
      auto &bound = *controllerE.getProject();
      auto &audio = bound.getAudioData();
      audio.sampleRate = SAMPLE_RATE;
      audio.f0.assign(200, 220.0f);
      audio.basePitch.assign(200, 57.0f);
      audio.deltaPitch.assign(200, 0.0f);
      audio.waveform.setSize(1, 200 * HOP_SIZE);
      bound.addNote(Note(10, 60, 58));
      bound.addNote(Note(70, 120, 62));
      bound.addNote(Note(130, 190, 67));
    }
    PitchUndoManager historyE(100);
    MainComponent mcE(false);
    MainComponentTestAccess::wireController(mcE, controllerE, historyE);
    auto &rollE = MainComponentTestAccess::roll(mcE);
    mcE.setSize(1100, 650);
    drainMessages();
    if (rollE.getWidth() <= 0 || rollE.getHeight() <= 0)
      rollE.setSize(1000, 560); // headless layout may not size the roll
    drainMessages();

    rollE.setTimelineDisplayOffset(3630.0);
    drainMessages();
    CHECK(rollE.getTimelineStart() == -3630.0);
    rollE.onZoomFullRequested(); // == MainComponent::zoomToFull()
    drainMessages();
    CHECK(std::abs(rollE.getScrollX()) < 0.01);
    CHECK(rollE.getPixelsPerSecond() > 100.0f);
    CHECK(visibleNotes(rollE) == 3);
    CHECK(storesAgree(rollE));
    rollE.setProject(nullptr);
  }

  std::cout << "ViewStateRecoveryTests passed" << std::endl;
  return 0;
}
