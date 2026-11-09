#pragma once

#include "InteractionHandler.h"
#include "../../../Undo/F0FrameEdit.h"
#include "../../../Undo/SegmentBoundaryAction.h"

#include <limits>
#include <vector>

class Note;

/**
 * Handles selection, note dragging (single + multi), box selection,
 * delta scale/offset drags, pitch tool handle interactions, and double-click snap.
 * The most complex handler — covers the Select edit mode.
 */
class SelectHandler : public InteractionHandler {
public:
  explicit SelectHandler(PianoRollComponent &owner);

  bool mouseDown(const juce::MouseEvent &e, float worldX,
                 float worldY) override;
  bool mouseDrag(const juce::MouseEvent &e, float worldX,
                 float worldY) override;
  bool mouseUp(const juce::MouseEvent &e, float worldX,
               float worldY) override;
  void mouseMove(const juce::MouseEvent &e, float worldX,
                 float worldY) override;
  void mouseDoubleClick(const juce::MouseEvent &e, float worldX,
                        float worldY) override;
  bool isActive() const override;
  void cancel() override;

  // ── Rendering-state getters (used by PianoRollComponent draw code) ──
  bool getIsDeltaScaleDragging() const { return isDeltaScaleDragging; }
  const std::vector<Note *> &getDeltaScaleTargetNotes() const {
    return deltaScaleTargetNotes;
  }
  float getDeltaScaleFactor() const { return deltaScaleFactor; }

  bool getIsDeltaOffsetDragging() const { return isDeltaOffsetDragging; }
  const std::vector<Note *> &getDeltaOffsetTargetNotes() const {
    return deltaOffsetTargetNotes;
  }
  float getDeltaOffsetSemitones() const { return deltaOffsetSemitones; }

  bool isSingleNoteDragging() const { return isDragging; }
  Note *getDraggedNote() const { return draggedNote; }

private:
  /** Rebuild base pitch, fire edited/finished callbacks, and repaint. */
  void rebuildAndNotify();

  /** Shared init logic for deltaScale / deltaOffset mouseDown. */
  bool initDeltaDrag(float worldY,
                     bool &isDraggingOut,
                     float &dragStartYOut,
                     std::vector<Note *> &targetNotesOut,
                     std::vector<F0FrameEdit> &editsOut,
                     int &minFrameOut,
                     int &maxFrameOut);

  void prepareDragBasePreview();
  void applyDragBasePreview(float pitchOffsetSemitones);
  void restoreDragBasePreview();

  // Segment boundary drag (batch 2, B4)
  void beginBoundaryDrag(Note &note, bool startEdge, float worldX);
  void applyBoundaryPreview();

  // Single note drag state
  bool isDragging = false;
  // Pitch drags start only after the pointer moves a few pixels.
  float mouseDownWorldY = 0.0f;
  bool dragThresholdPassed = false;
  Note *draggedNote = nullptr;
  float dragStartY = 0.0f;
  float originalPitchOffset = 0.0f;
  float originalMidiNote = 60.0f;
  float boundaryF0Start = 0.0f;
  float boundaryF0End = 0.0f;
  std::vector<float> originalF0Values;
  float lastDragPitchOffset = 0.0f;
  int dragPreviewStartFrame = -1;
  int dragPreviewEndFrame = -1;
  std::vector<float> dragPreviewWeights;
  std::vector<float> dragBasePitchSnapshot;
  std::vector<float> dragF0Snapshot;

  // Delta pitch scale drag state
  bool isDeltaScaleDragging = false;
  float deltaScaleDragStartY = 0.0f;
  float deltaScaleFactor = 1.0f;
  int deltaScaleMinFrame = std::numeric_limits<int>::max();
  int deltaScaleMaxFrame = std::numeric_limits<int>::min();
  std::vector<Note *> deltaScaleTargetNotes;
  std::vector<F0FrameEdit> deltaScaleEdits;

  // Segment boundary drag state (batch 2, B4): Alt+drag on a segment edge
  // moves the boundary between this segment and its neighbour (or the
  // unowned gap next to it), reassigning output and source frames alike.
  bool isBoundaryDragging = false;
  Note *boundaryNote = nullptr;
  Note *boundaryNeighbour = nullptr; // contiguous neighbour sharing the edge
  bool boundaryIsStart = true;
  float boundaryDragStartWorldX = 0.0f;
  int boundaryMinDelta = 0;
  int boundaryMaxDelta = 0;
  int boundaryCurrentDelta = 0;
  std::vector<NoteBoundaryState> boundaryBeforeStates;

  // Delta pitch offset drag state
  bool isDeltaOffsetDragging = false;
  float deltaOffsetDragStartY = 0.0f;
  float deltaOffsetSemitones = 0.0f;
  int deltaOffsetMinFrame = std::numeric_limits<int>::max();
  int deltaOffsetMaxFrame = std::numeric_limits<int>::min();
  std::vector<Note *> deltaOffsetTargetNotes;
  std::vector<F0FrameEdit> deltaOffsetEdits;
};
