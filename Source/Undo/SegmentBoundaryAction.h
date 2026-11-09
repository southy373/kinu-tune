#pragma once

#include "UndoableAction.h"
#include "../Models/Project.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/TimingRegionUtils.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

// Batch 2 (B4): moving a segment boundary in the Select tool. A boundary
// move reassigns frames between the two sides - it never stretches - so
// the output frames and the source frames of the grabbed edge move by the
// same amount on both notes. The state therefore records all four values.
struct NoteBoundaryState
{
    Note* note = nullptr;
    int startFrame = 0;
    int endFrame = 0;
    int srcStartFrame = 0;
    int srcEndFrame = 0;

    static NoteBoundaryState capture(Note& note)
    {
        return {&note, note.getStartFrame(), note.getEndFrame(),
                note.getSrcStartFrame(), note.getSrcEndFrame()};
    }
};

inline void applyNoteBoundaryStates(
    Project& project, const std::vector<NoteBoundaryState>& states)
{
    int dirtyStart = std::numeric_limits<int>::max();
    int dirtyEnd = std::numeric_limits<int>::min();

    for (const auto& state : states)
    {
        if (!state.note)
            continue;

        Note& note = *state.note;
        const bool leftEdgeChanged =
            note.getStartFrame() != state.startFrame ||
            note.getSrcStartFrame() != state.srcStartFrame;
        const bool rightEdgeChanged =
            note.getEndFrame() != state.endFrame ||
            note.getSrcEndFrame() != state.srcEndFrame;
        if (!leftEdgeChanged && !rightEdgeChanged)
            continue;

        // Same region-edge expansion as timing edits: a region's first or
        // last note also drags the unpitched lead/tail of its source
        // region, which must be re-rendered too.
        if (leftEdgeChanged && timingRegions::isFirstNote(project, note))
        {
            const auto region = timingRegions::getSourceRegion(project, note);
            const int currentRegionStart =
                region.start + note.getStartFrame() - note.getSrcStartFrame();
            const int destinationRegionStart =
                region.start + state.startFrame - state.srcStartFrame;
            dirtyStart = std::min({dirtyStart, region.start,
                                   currentRegionStart,
                                   destinationRegionStart});
        }

        if (rightEdgeChanged && timingRegions::isLastNote(project, note))
        {
            const auto region = timingRegions::getSourceRegion(project, note);
            const int currentRegionEnd =
                region.end + note.getEndFrame() - note.getSrcEndFrame();
            const int destinationRegionEnd =
                region.end + state.endFrame - state.srcEndFrame;
            dirtyEnd = std::max({dirtyEnd, region.end, currentRegionEnd,
                                 destinationRegionEnd});
        }

        dirtyStart = std::min({dirtyStart, note.getStartFrame(),
                               state.startFrame});
        dirtyEnd = std::max({dirtyEnd, note.getEndFrame(), state.endFrame});
        note.setStartFrame(state.startFrame);
        note.setEndFrame(state.endFrame);
        note.setSrcStartFrame(state.srcStartFrame);
        note.setSrcEndFrame(state.srcEndFrame);
        note.markDirty();
        note.markSynthDirty();
    }

    // A boundary move outside the length-% path re-bases the length scale.
    project.invalidateLengthBase();
    PitchCurveProcessor::rebuildBaseFromNotes(project);
    if (dirtyStart <= dirtyEnd)
    {
        const int frameCount = project.getAudioData().getNumFrames();
        project.setF0DirtyRange(std::max(0, dirtyStart - 24),
                                std::min(frameCount, dirtyEnd + 24));
    }
    project.setModified(true);
}

class SegmentBoundaryAction final : public UndoableAction
{
public:
    SegmentBoundaryAction(Project& project,
                          std::vector<NoteBoundaryState> before,
                          std::vector<NoteBoundaryState> after)
        : project(project), before(std::move(before)),
          after(std::move(after))
    {
    }

    void undo() override { applyNoteBoundaryStates(project, before); }
    void redo() override { applyNoteBoundaryStates(project, after); }
    juce::String getName() const override { return "Move Segment Boundary"; }

private:
    Project& project;
    std::vector<NoteBoundaryState> before;
    std::vector<NoteBoundaryState> after;
};
