#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"
#include "../Source/Undo/LengthPercentAction.h"
#include "../Source/Undo/TimingAction.h"
#include "TestAssert.h"

#include <cmath>
#include <iostream>
#include <vector>

namespace
{

bool nearF(float a, float b, float tolerance = 1.0e-3f)
{
    return std::abs(a - b) <= tolerance;
}

// A self-consistent small project: three segments (0-100, 100-200,
// 250-300) over a 600-frame analysis, like a real analysed project.
void fillProject(Project &project)
{
    auto &audio = project.getAudioData();
    audio.sampleRate = 44100;
    audio.melSpectrogram.assign(600, std::vector<float>(1, 0.0f));
    audio.f0.assign(600, 261.6f);
    audio.basePitch.assign(600, 60.0f);
    audio.deltaPitch.assign(600, 0.0f);
    audio.voicedMask.assign(600, true);

    const int spans[3][2] = {{0, 100}, {100, 200}, {250, 300}};
    const float midis[3] = {60.3f, 61.8f, 64.0f};
    for (int i = 0; i < 3; ++i)
    {
        Note note(spans[i][0], spans[i][1], midis[i]);
        note.setSrcStartFrame(spans[i][0]);
        note.setSrcEndFrame(spans[i][1]);
        note.setOriginalMidiNote(midis[i]);
        note.setDeltaPitch(
            std::vector<float>(static_cast<size_t>(spans[i][1] - spans[i][0]),
                               0.0f));
        note.setOriginalDeltaPitch(note.getDeltaPitch());
        project.addNote(note);
    }
}

std::vector<NoteTimingState> captureStates(Project &project)
{
    std::vector<NoteTimingState> states;
    for (auto &note : project.getNotes())
        states.push_back(NoteTimingState::capture(note));
    return states;
}

} // namespace

int main()
{
    // Defaults and clamping.
    {
        Project project;
        fillProject(project);
        CHECK(nearF(project.getPitchPercent(), 100.0f));
        CHECK(nearF(project.getLengthPercent(), 100.0f));
        CHECK(nearF(project.getFormantShift(), 0.0f));

        project.setPitchPercent(500.0f);
        CHECK(nearF(project.getPitchPercent(), 200.0f));
        project.setPitchPercent(10.0f);
        CHECK(nearF(project.getPitchPercent(), 50.0f));
        project.setLengthPercent(0.0f);
        CHECK(nearF(project.getLengthPercent(), 50.0f));
    }

    // Persistence: pitch %, length % and the global formant all survive
    // JSON and binary-archive round trips.
    {
        Project project;
        fillProject(project);
        project.setPitchPercent(125.0f);
        project.setLengthPercent(80.0f);
        project.setFormantShift(3.5f);

        Project jsonRestored;
        CHECK(ProjectSerializer::fromJson(
            jsonRestored, ProjectSerializer::toJson(project)));
        CHECK(nearF(jsonRestored.getPitchPercent(), 125.0f));
        CHECK(nearF(jsonRestored.getLengthPercent(), 80.0f));
        CHECK(nearF(jsonRestored.getFormantShift(), 3.5f));

        for (auto mode :
             {ProjectSerializer::BinaryArchiveMode::selfContained,
              ProjectSerializer::BinaryArchiveMode::hostBackedARA})
        {
            juce::MemoryBlock archive;
            CHECK(ProjectSerializer::toBinaryArchive(project, archive, mode));
            Project restored;
            CHECK(ProjectSerializer::fromBinaryArchive(
                restored, archive.getData(), archive.getSize()));
            CHECK(nearF(restored.getPitchPercent(), 125.0f));
            CHECK(nearF(restored.getLengthPercent(), 80.0f));
            CHECK(nearF(restored.getFormantShift(), 3.5f));
        }
    }

    // Length % scales output frames from a stable base: 150% then back
    // to 100% restores the original frames exactly.
    {
        Project project;
        fillProject(project);
        project.scaleNotesForLengthPercent(150.0f);
        CHECK(nearF(project.getLengthPercent(), 150.0f));
        CHECK(project.getNotes()[0].getStartFrame() == 0);
        CHECK(project.getNotes()[0].getEndFrame() == 150);
        CHECK(project.getNotes()[1].getStartFrame() == 150);
        CHECK(project.getNotes()[1].getEndFrame() == 300);
        CHECK(project.getNotes()[2].getStartFrame() == 375);
        CHECK(project.getNotes()[2].getEndFrame() == 450);
        // Source frames are not part of the scale (time-stretch semantics).
        CHECK(project.getNotes()[1].getSrcStartFrame() == 100);
        CHECK(project.getNotes()[1].getSrcEndFrame() == 200);

        project.scaleNotesForLengthPercent(100.0f);
        CHECK(project.getNotes()[0].getEndFrame() == 100);
        CHECK(project.getNotes()[1].getStartFrame() == 100);
        CHECK(project.getNotes()[1].getEndFrame() == 200);
        CHECK(project.getNotes()[2].getStartFrame() == 250);
        CHECK(project.getNotes()[2].getEndFrame() == 300);

        project.scaleNotesForLengthPercent(50.0f);
        CHECK(project.getNotes()[1].getStartFrame() == 50);
        CHECK(project.getNotes()[1].getEndFrame() == 100);
        CHECK(project.getNotes()[2].getStartFrame() == 125);
        project.scaleNotesForLengthPercent(100.0f);
        CHECK(project.getNotes()[2].getStartFrame() == 250);
    }

    // The undo action restores frames and percent together.
    {
        Project project;
        fillProject(project);
        const auto before = captureStates(project);
        project.scaleNotesForLengthPercent(150.0f);
        const auto after = captureStates(project);
        LengthPercentAction action(project, before, after, 100.0f, 150.0f);
        action.undo();
        CHECK(nearF(project.getLengthPercent(), 100.0f));
        CHECK(project.getNotes()[1].getStartFrame() == 100);
        CHECK(project.getNotes()[2].getEndFrame() == 300);
        action.redo();
        CHECK(nearF(project.getLengthPercent(), 150.0f));
        CHECK(project.getNotes()[1].getStartFrame() == 150);
        CHECK(project.getNotes()[2].getEndFrame() == 450);
    }

    // A timing edit at a non-100 percent invalidates the base; the next
    // scale re-derives it from the current frames at the current percent
    // (the documented sub-frame drift case): note 1 was moved to start
    // at 160 while at 150%, so its 100% base becomes round(160 / 1.5).
    {
        Project project;
        fillProject(project);
        project.scaleNotesForLengthPercent(150.0f);
        auto moved = captureStates(project);
        moved[1].startFrame = 160;
        applyNoteTimingStates(project, moved);
        project.scaleNotesForLengthPercent(100.0f);
        CHECK(nearF(project.getLengthPercent(), 100.0f));
        CHECK(project.getNotes()[1].getStartFrame() == 107);
        CHECK(project.getNotes()[1].getEndFrame() == 200);
        CHECK(project.getNotes()[2].getStartFrame() == 250);
    }

    // Pitch % is a render-time ratio only: it never moves notes.
    {
        Project project;
        fillProject(project);
        project.setPitchPercent(150.0f);
        CHECK(project.getNotes()[1].getStartFrame() == 100);
        CHECK(project.getNotes()[1].getEndFrame() == 200);
    }

    std::cout << "Global percent tests passed\n";
    return 0;
}
