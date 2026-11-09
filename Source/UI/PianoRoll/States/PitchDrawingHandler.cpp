#include "PitchDrawingHandler.h"
#include "../../../Undo/PitchDrawingAction.h"
#include "../../../Utils/Constants.h"
#include "../../../Utils/PitchCurveProcessor.h"
#include "../../../Utils/NotePitch.h"
#include "../../../Utils/TransformParams.h"
#include "../../../Utils/UI/Theme.h"

#include <algorithm>
#include <cmath>
#include <limits>

// Batch 2 (U6): freehand pitch drawing. The previous interaction placed
// draggable anchors and interpolated between them with a smootherstep,
// which users found fiddly ("かなりクセがあって使いづらかった"). Now the
// pointer's trajectory itself is written into the preview curve while
// dragging; the downstream model is unchanged - on Apply the drawn curve
// is baked into each note's bakedDeltaPitch through the same
// PitchDrawingAction as before, so undo and rendering behave exactly as
// they did for anchor drawings.

PitchDrawingHandler::PitchDrawingHandler(PianoRollComponent& owner)
    : InteractionHandler(owner)
{
}

bool PitchDrawingHandler::mouseDown(const juce::MouseEvent& e, float worldX,
                              float worldY)
{
  if (e.mods.isPopupMenu())
    return removeAnchorAt(worldX, worldY);

  if (!e.mods.isLeftButtonDown() || !owner_.project)
    return false;

  if (!isPitchRegionAtWorldX(worldX))
    return false;

  captureOriginalCurveIfNeeded();
  strokeActive = true;
  dragMoved = false;
  lastStrokeFrame = -1;
  writeStrokePoint(frameFromWorldX(worldX), midiFromWorldY(worldY));
  updateAffectedNotePositions();
  applyPreviewToProject();
  notifyStateChanged();
  return true;
}

bool PitchDrawingHandler::mouseDrag(const juce::MouseEvent&, float worldX,
                              float worldY)
{
  if (!strokeActive)
    return false;

  writeStrokePoint(frameFromWorldX(worldX), midiFromWorldY(worldY));
  updateAffectedNotePositions();
  dragMoved = true;
  owner_.repaint();
  return true;
}

bool PitchDrawingHandler::mouseUp(const juce::MouseEvent&, float, float)
{
  if (!strokeActive)
    return false;
  strokeActive = false;
  lastStrokeFrame = -1;
  if (dragMoved)
  {
    // Light smoothing once the stroke lands, feathered at the span ends
    // so the joins with the untouched curve stay click-free.
    smoothStrokeSpan();
    updateAffectedNotePositions();
    applyPreviewToProject();
  }
  dragMoved = false;
  owner_.repaint();
  return true;
}

void PitchDrawingHandler::mouseMove(const juce::MouseEvent&, float worldX,
                              float)
{
  const bool newPointerOverPitchRegion = isPitchRegionAtWorldX(worldX);
  if (newPointerOverPitchRegion == pointerOverPitchRegion)
    return;
  pointerOverPitchRegion = newPointerOverPitchRegion;
  owner_.setMouseCursor(pointerOverPitchRegion
                            ? juce::MouseCursor::CrosshairCursor
                            : juce::MouseCursor::NormalCursor);
  owner_.repaint();
}

void PitchDrawingHandler::mouseDoubleClick(const juce::MouseEvent&, float worldX,
                                     float worldY)
{
  removeAnchorAt(worldX, worldY);
}

bool PitchDrawingHandler::removeAnchorAt(float worldX, float worldY)
{
  // Freehand equivalent of removing an anchor: erase the drawn curve
  // around the click back to the original curve.
  if (previewMidiCurve.empty() || editedStartFrame < 0)
    return false;

  const int frame = frameFromWorldX(worldX);
  constexpr int eraseRadius = 10;
  const int from = std::max(editedStartFrame, frame - eraseRadius);
  const int to = std::min(editedEndFrame, frame + eraseRadius);
  bool erased = false;
  for (int f = from; f <= to; ++f)
  {
    if (previewMidiCurve[static_cast<size_t>(f)] !=
        originalMidiCurve[static_cast<size_t>(f)])
    {
      previewMidiCurve[static_cast<size_t>(f)] =
          originalMidiCurve[static_cast<size_t>(f)];
      erased = true;
    }
  }
  if (!erased)
    return false;

  // Shrink the edited span to what is still drawn; erasing everything is
  // the same as discarding the drawing.
  int newStart = -1;
  int newEnd = -1;
  for (int f = editedStartFrame; f <= editedEndFrame; ++f)
  {
    if (previewMidiCurve[static_cast<size_t>(f)] !=
        originalMidiCurve[static_cast<size_t>(f)])
    {
      if (newStart < 0)
        newStart = f;
      newEnd = f;
    }
  }
  if (newStart < 0)
  {
    clearState(true);
    return true;
  }
  editedStartFrame = newStart;
  editedEndFrame = newEnd;

  updateAffectedNotePositions();
  applyPreviewToProject();
  pointerOverPitchRegion = isPitchRegionAtWorldX(worldX);
  owner_.setMouseCursor(pointerOverPitchRegion
                            ? juce::MouseCursor::CrosshairCursor
                            : juce::MouseCursor::NormalCursor);
  notifyStateChanged();
  return true;
}

void PitchDrawingHandler::clearHover()
{
  if (pointerOverPitchRegion)
  {
    pointerOverPitchRegion = false;
    owner_.repaint();
  }
}

void PitchDrawingHandler::draw(juce::Graphics&)
{
  // Nothing to draw: the piano roll renders the preview curve itself
  // through its midiCurveOverride, and freehand strokes have no anchors.
}

bool PitchDrawingHandler::isActive() const
{
  return strokeActive || editedStartFrame >= 0;
}

void PitchDrawingHandler::cancel()
{
  clearState(true);
}

void PitchDrawingHandler::clearState(bool restoreNoteStates)
{
  if (restoreNoteStates)
  {
    restoreOriginalNoteStates(true);
  }
  originalNoteStates.clear();
  originalMidiCurve.clear();
  previewMidiCurve.clear();
  editedStartFrame = -1;
  editedEndFrame = -1;
  strokeActive = false;
  dragMoved = false;
  lastStrokeFrame = -1;
  pointerOverPitchRegion = false;
  previewDirtyStart = -1;
  previewDirtyEnd = -1;
  notifyStateChanged();
}

bool PitchDrawingHandler::apply()
{
  if (!owner_.project || editedStartFrame < 0 || previewMidiCurve.empty())
    return false;

  const int firstFrame = editedStartFrame;
  const int lastFrame = editedEndFrame;
  auto& project = *owner_.project;
  std::vector<PitchDrawingNoteState> before;
  std::vector<PitchDrawingNoteState> after;

  for (auto& note : project.getNotes())
  {
    if (note.isRestOrDeleted() || note.getEndFrame() <= firstFrame ||
        note.getStartFrame() > lastFrame)
      continue;

    const auto* original = originalStateFor(note);
    if (!original)
      continue;

    PitchDrawingNoteState oldState;
    oldState.note = &note;
    oldState.params = original->params;
    oldState.bakedDeltaPitch = original->bakedDeltaPitch;
    before.push_back(oldState);

    PitchDrawingNoteState newState;
    newState.note = &note;
    newState.params = TransformParams::fromNote(note);
    newState.bakedDeltaPitch = note.getBakedDeltaPitch();
    after.push_back(std::move(newState));
  }

  if (before.empty())
    return false;

  auto action = std::make_unique<PitchDrawingAction>(
      &project, std::move(before), std::move(after));
  if (owner_.undoManager)
    owner_.undoManager->addAction(std::move(action));

  project.setModified(true);
  clearState(false);
  return true;
}

int PitchDrawingHandler::frameFromWorldX(float worldX) const
{
  if (!owner_.project || owner_.project->getAudioData().f0.empty())
    return 0;
  const int lastFrame =
      static_cast<int>(owner_.project->getAudioData().f0.size()) - 1;
  const int frame = static_cast<int>(std::round(
      secondsToFrames(static_cast<float>(owner_.xToTime(worldX)))));
  return juce::jlimit(0, lastFrame, frame);
}

float PitchDrawingHandler::midiFromWorldY(float worldY) const
{
  float midi = owner_.yToMidi(worldY - owner_.pixelsPerSemitone * 0.5f);
  if (owner_.project)
    midi -= owner_.project->getGlobalPitchOffset();
  return juce::jlimit(static_cast<float>(MIN_MIDI_NOTE),
                      static_cast<float>(MAX_MIDI_NOTE), midi);
}

const Note* PitchDrawingHandler::pitchNoteAtFrame(int frame) const
{
  if (!owner_.project)
    return nullptr;
  for (const auto& note : owner_.project->getNotes())
  {
    if (!note.isRestOrDeleted() && note.getStartFrame() <= frame &&
        note.getEndFrame() > frame)
      return &note;
  }
  return nullptr;
}

bool PitchDrawingHandler::frameBelongsToPitchNote(int frame) const
{
  return pitchNoteAtFrame(frame) != nullptr;
}

bool PitchDrawingHandler::isPitchRegionAtWorldX(float worldX) const
{
  if (!owner_.project)
    return false;

  const auto& f0 = owner_.project->getAudioData().f0;
  const int frame = static_cast<int>(std::round(
      secondsToFrames(static_cast<float>(owner_.xToTime(worldX)))));
  return frame >= 0 && frame < static_cast<int>(f0.size()) &&
         frameBelongsToPitchNote(frame);
}

void PitchDrawingHandler::captureOriginalCurveIfNeeded()
{
  if (!originalMidiCurve.empty() || !owner_.project)
    return;

  const auto& audioData = owner_.project->getAudioData();
  const size_t size = audioData.f0.size();
  originalMidiCurve.assign(size, 0.0f);
  for (size_t i = 0; i < size; ++i)
  {
    const float base = i < audioData.basePitch.size() ? audioData.basePitch[i] : 0.0f;
    const float delta = i < audioData.deltaPitch.size() ? audioData.deltaPitch[i] : 0.0f;
    originalMidiCurve[i] = base + delta;
  }
  previewMidiCurve = originalMidiCurve;

  originalNoteStates.clear();
  originalNoteStates.reserve(owner_.project->getNotes().size());
  for (auto& note : owner_.project->getNotes())
    if (!note.isRestOrDeleted())
      originalNoteStates.push_back(
          {&note, TransformParams::fromNote(note), note.getBakedDeltaPitch()});
}

void PitchDrawingHandler::writeStrokePoint(int frame, float midi)
{
  if (previewMidiCurve.empty() ||
      frame < 0 || frame >= static_cast<int>(previewMidiCurve.size()))
    return;

  const auto plot = [this](int f, float value)
  {
    previewMidiCurve[static_cast<size_t>(f)] = value;
    editedStartFrame = editedStartFrame < 0 ? f : std::min(editedStartFrame, f);
    editedEndFrame = std::max(editedEndFrame, f);
  };

  // Interpolate between the previous plotted point and this one so a fast
  // stroke has no holes. Frames that belong to no note (the gaps between
  // segments) are skipped: the stroke breaks there instead of drawing a
  // line across silence, and the next in-note point starts a new chain.
  if (lastStrokeFrame >= 0 && frame != lastStrokeFrame)
  {
    const int step = frame > lastStrokeFrame ? 1 : -1;
    for (int f = lastStrokeFrame + step;; f += step)
    {
      if (frameBelongsToPitchNote(f))
      {
        const float t = static_cast<float>(f - lastStrokeFrame) /
                        static_cast<float>(frame - lastStrokeFrame);
        plot(f, juce::jmap(t, lastStrokeMidi, midi));
      }
      if (f == frame)
        break;
    }
  }
  else if (frameBelongsToPitchNote(frame))
  {
    plot(frame, midi);
  }

  if (frameBelongsToPitchNote(frame))
  {
    lastStrokeFrame = frame;
    lastStrokeMidi = midi;
  }
}

void PitchDrawingHandler::smoothStrokeSpan()
{
  if (previewMidiCurve.empty() || editedStartFrame < 0)
    return;

  const auto drawn = previewMidiCurve;
  constexpr int radius = 2;
  for (int f = editedStartFrame; f <= editedEndFrame; ++f)
  {
    if (!frameBelongsToPitchNote(f))
      continue;
    double sum = 0.0;
    int count = 0;
    for (int k = -radius; k <= radius; ++k)
    {
      const int idx = f + k;
      if (idx < editedStartFrame || idx > editedEndFrame ||
          !frameBelongsToPitchNote(idx))
        continue;
      sum += drawn[static_cast<size_t>(idx)];
      ++count;
    }
    if (count == 0)
      continue;
    const float smoothed = static_cast<float>(sum / static_cast<double>(count));
    // Full smoothing in the interior; the first and last few frames blend
    // back to the drawn values so the stroke's ends are preserved exactly.
    const int edgeDistance = std::min(f - editedStartFrame,
                                      editedEndFrame - f);
    const float blend = juce::jlimit(
        0.0f, 1.0f,
        static_cast<float>(edgeDistance) / static_cast<float>(radius * 2));
    previewMidiCurve[static_cast<size_t>(f)] =
        juce::jmap(blend, drawn[static_cast<size_t>(f)], smoothed);
  }
}

void PitchDrawingHandler::updateAffectedNotePositions()
{
  if (!owner_.project || editedStartFrame < 0 || previewMidiCurve.empty())
    return;

  // Start from the exact pre-drawing positions on every preview update, so
  // extending or erasing a stroke cannot accumulate movement.
  restoreOriginalNoteStates(false);

  const int firstFrame = editedStartFrame;
  const int lastFrame = editedEndFrame;
  const auto& audioData = owner_.project->getAudioData();

  for (auto& note : owner_.project->getNotes())
  {
    if (note.isRestOrDeleted() || note.getEndFrame() <= firstFrame ||
        note.getStartFrame() > lastFrame)
      continue;

    const int start = std::max(0, note.getStartFrame());
    const int end = std::min(note.getEndFrame(),
                             static_cast<int>(previewMidiCurve.size()));
    double sum = 0.0;
    int count = 0;
    for (int frame = start; frame < end; ++frame)
    {
      const bool voiced =
          audioData.voicedMask.empty() ||
          (frame < static_cast<int>(audioData.voicedMask.size()) &&
           audioData.voicedMask[static_cast<size_t>(frame)]);
      if (!voiced || !std::isfinite(previewMidiCurve[static_cast<size_t>(frame)]))
        continue;
      sum += previewMidiCurve[static_cast<size_t>(frame)];
      ++count;
    }

    if (count > 0)
      NotePitch::setSungPitch(note, static_cast<float>(sum / static_cast<double>(count)));
  }

  if (owner_.onPitchEdited)
    owner_.onPitchEdited();
}

void PitchDrawingHandler::applyPreviewToProject()
{
  if (!owner_.project || editedStartFrame < 0 || previewMidiCurve.empty())
    return;

  const int firstFrame = editedStartFrame;
  const int lastFrame = editedEndFrame;
  auto& project = *owner_.project;
  auto& audioData = project.getAudioData();
  std::vector<Note*> affected;

  for (auto& note : project.getNotes())
  {
    if (note.isRestOrDeleted() || note.getEndFrame() <= firstFrame ||
        note.getStartFrame() > lastFrame)
      continue;

    // Keep the live note center calculated by updateAffectedNotePositions(),
    // but make the per-note transforms neutral before deriving the base curve
    // for the baked preview contour.
    note.setTiltLeft(0.0f);
    note.setTiltRight(0.0f);
    note.setVibrato(1.0f);
    note.setPitchDrift(1.0f);
    note.setSmoothLeftFrames(0);
    note.setSmoothRightFrames(0);
    note.setDeltaScale(1.0f);
    note.setDeltaOffset(0.0f);
    affected.push_back(&note);
  }

  if (affected.empty())
    return;

  PitchCurveProcessor::rebuildBaseFromNotes(project);
  int dirtyStart = std::numeric_limits<int>::max();
  int dirtyEnd = std::numeric_limits<int>::min();
  for (auto* note : affected)
  {
    const int duration = std::max(0, note->getDurationFrames());
    std::vector<float> bakedDelta(static_cast<size_t>(duration), 0.0f);
    for (int i = 0; i < duration; ++i)
    {
      const int frame = note->getStartFrame() + i;
      if (frame >= 0 && frame < static_cast<int>(previewMidiCurve.size()) &&
          frame < static_cast<int>(audioData.basePitch.size()))
        bakedDelta[static_cast<size_t>(i)] =
            previewMidiCurve[static_cast<size_t>(frame)] -
            audioData.basePitch[static_cast<size_t>(frame)];
    }
    note->setBakedDeltaPitch(std::move(bakedDelta));
    note->markDirty();
    note->markSynthDirty();
    dirtyStart = std::min(dirtyStart, note->getStartFrame());
    dirtyEnd = std::max(dirtyEnd, note->getEndFrame());
  }

  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(dirtyStart, dirtyEnd);
  previewDirtyStart = previewDirtyStart < 0
                          ? dirtyStart
                          : std::min(previewDirtyStart, dirtyStart);
  previewDirtyEnd = previewDirtyEnd < 0
                        ? dirtyEnd
                        : std::max(previewDirtyEnd, dirtyEnd);
  owner_.invalidateBasePitchCache();
  if (owner_.onPitchEdited)
    owner_.onPitchEdited();
  if (owner_.onPitchPreviewRenderRequested)
    owner_.onPitchPreviewRenderRequested();
}

void PitchDrawingHandler::restoreOriginalNoteStates(bool requestRender)
{
  if (!owner_.project || originalNoteStates.empty())
    return;

  for (const auto& state : originalNoteStates)
  {
    if (!state.note)
      continue;
    state.params.applyToNote(*state.note);
    state.note->setBakedDeltaPitch(state.bakedDeltaPitch);
    if (requestRender && previewDirtyStart >= 0 &&
        state.note->getStartFrame() < previewDirtyEnd &&
        state.note->getEndFrame() > previewDirtyStart)
    {
      state.note->markDirty();
      state.note->markSynthDirty();
    }
  }

  if (!requestRender)
    return;

  PitchCurveProcessor::rebuildBaseFromNotes(*owner_.project);
  if (previewDirtyStart >= 0 && previewDirtyEnd > previewDirtyStart)
    owner_.project->markNoteEditDirtyRange(previewDirtyStart, previewDirtyEnd);
  owner_.invalidateBasePitchCache();
  if (owner_.onPitchEdited)
    owner_.onPitchEdited();
  if (previewDirtyStart >= 0 && owner_.onPitchPreviewRenderRequested)
    owner_.onPitchPreviewRenderRequested();
}

const PitchDrawingHandler::OriginalNoteState*
PitchDrawingHandler::originalStateFor(const Note& note) const
{
  const auto found = std::find_if(
      originalNoteStates.begin(), originalNoteStates.end(),
      [&note](const OriginalNoteState& state) { return state.note == &note; });
  return found != originalNoteStates.end() ? &*found : nullptr;
}

void PitchDrawingHandler::notifyStateChanged()
{
  owner_.updatePitchDrawingConfirmation();
  owner_.repaint();
}
