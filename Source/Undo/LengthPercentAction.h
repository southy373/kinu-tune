#pragma once

#include "TimingAction.h"
#include "UndoableAction.h"
#include "../Models/Project.h"

#include <utility>
#include <vector>

/**
 * Batch 2 (U7): one undo step for a project length-% change.
 *
 * The frame movement itself is captured as ordinary note timing states
 * (before/after), exactly like a timing drag, plus the percent value on
 * each side. Undo/redo replays the states through applyNoteTimingStates
 * (which also invalidates the runtime length base, so a later percent
 * change re-derives it from the restored frames) and restores the
 * stored percent.
 */
class LengthPercentAction final : public UndoableAction
{
public:
    LengthPercentAction(Project &project,
                        std::vector<NoteTimingState> before,
                        std::vector<NoteTimingState> after,
                        float oldPercent, float newPercent)
        : project(project), before(std::move(before)),
          after(std::move(after)), oldPercent(oldPercent),
          newPercent(newPercent) {}

    void undo() override
    {
        applyNoteTimingStates(project, before);
        project.setLengthPercent(oldPercent);
    }

    void redo() override
    {
        applyNoteTimingStates(project, after);
        project.setLengthPercent(newPercent);
    }

    juce::String getName() const override { return "Change Length %"; }

private:
    Project &project;
    std::vector<NoteTimingState> before;
    std::vector<NoteTimingState> after;
    float oldPercent;
    float newPercent;
};
