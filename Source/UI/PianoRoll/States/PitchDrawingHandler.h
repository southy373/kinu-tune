#pragma once

#include "InteractionHandler.h"
#include "../../../Utils/TransformParams.h"

#include <vector>

class PitchDrawingHandler final : public InteractionHandler
{
public:
  explicit PitchDrawingHandler(PianoRollComponent& owner);

  bool mouseDown(const juce::MouseEvent& e, float worldX,
                 float worldY) override;
  bool mouseDrag(const juce::MouseEvent& e, float worldX,
                 float worldY) override;
  bool mouseUp(const juce::MouseEvent& e, float worldX,
               float worldY) override;
  void mouseMove(const juce::MouseEvent& e, float worldX,
                 float worldY) override;
  void mouseDoubleClick(const juce::MouseEvent& e, float worldX,
                        float worldY) override;
  void draw(juce::Graphics& g) override;
  bool isActive() const override;
  void cancel() override;

  // Batch 2 (U6): the anchor-based interaction was replaced by freehand
  // strokes - the pointer's own trajectory becomes the pitch curve. The
  // method names below predate that change and are kept because the piano
  // roll calls them: "anchors" now simply means "a drawing session is
  // live" (batch 3 commits each stroke on release, so a session rarely
  // outlives its gesture).
  bool hasAnchors() const { return editedStartFrame >= 0; }
  bool isHoveringAnchor() const { return false; }
  bool isPointerOverPitchRegion() const { return pointerOverPitchRegion; }
  bool isPitchRegionAtWorldX(float worldX) const;
  bool removeAnchorAt(float worldX, float worldY);
  const std::vector<float>& getOriginalMidiCurve() const { return originalMidiCurve; }
  const std::vector<float>& getPreviewMidiCurve() const { return previewMidiCurve; }
  void clearHover();
  bool apply();

  // Batch 3: fired after a stroke (or an erase) has been committed by
  // apply(), so the owner can run its post-commit follow-up (cache
  // invalidation and the committed-edit notifications).
  std::function<void()> onDrawingCommitted;

private:
  struct OriginalNoteState
  {
    Note* note = nullptr;
    TransformParams params;
    std::vector<float> bakedDeltaPitch;
  };

  int frameFromWorldX(float worldX) const;
  float midiFromWorldY(float worldY) const;
  // The note a frame can be drawn on: an editable segment (not a rest,
  // not a deleted segment - deleted segments are outside editing and a
  // stroke must break across them exactly as it breaks across a gap).
  // nullptr when the frame belongs to no such note.
  const Note* pitchNoteAtFrame(int frame) const;
  bool frameBelongsToPitchNote(int frame) const;
  void captureOriginalCurveIfNeeded();
  void writeStrokePoint(int frame, float midi);
  void smoothStrokeSpan();
  void updateAffectedNotePositions();
  void applyPreviewToProject();
  void restoreOriginalNoteStates(bool requestRender);
  const OriginalNoteState* originalStateFor(const Note& note) const;
  void clearState(bool restoreNoteStates);
  void notifyStateChanged();
  // Batch 3: commit the live session as an undoable action right away.
  void commitDrawing();
  // Batch 3: erase with no live session - removes the already-committed
  // drawing around the click as its own undoable action.
  bool eraseCommittedAt(float worldX, float worldY);

  std::vector<OriginalNoteState> originalNoteStates;
  std::vector<float> originalMidiCurve;
  std::vector<float> previewMidiCurve;
  // Span of frames the strokes have actually drawn on (-1 = no drawing).
  int editedStartFrame = -1;
  int editedEndFrame = -1;
  bool strokeActive = false;
  bool dragMoved = false;
  // Last plotted point of the current stroke; -1 while the pointer is over
  // a gap, so no line is ever drawn across unowned frames.
  int lastStrokeFrame = -1;
  float lastStrokeMidi = 0.0f;
  bool pointerOverPitchRegion = false;
  int previewDirtyStart = -1;
  int previewDirtyEnd = -1;
};
