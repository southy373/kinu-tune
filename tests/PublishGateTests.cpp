// Batch 5.1 regression tests for the ARA publish gate
// (projectHasPublishableEdits in Project.h, behind
// PitchNetAudioProcessor::projectHasRegionEdits).
//
// The gate decides whether a region's rendered audio is published to
// the host or cleared back to the untouched source. It predates the
// Batch 2 global percents and never counted them: a region whose only
// edit was the global pitch % rendered correctly, failed the gate, and
// was cleared - so the % "worked" only on regions that also had a
// note edit, and stopped working when the last note edit was undone.
// These tests pin the percent cases alongside the pre-existing ones.
#include "../Source/Models/Project.h"
#include "../Source/Utils/Constants.h"
#include "TestAssert.h"
#include <iostream>

namespace {

// One analysed, completely neutral note: the shape of a region the
// user opened but never edited.
Project makeNeutralProject()
{
    Project project;
    auto &audio = project.getAudioData();
    audio.sampleRate = SAMPLE_RATE;
    audio.f0.assign(200, 220.0f);
    audio.basePitch.assign(200, 57.0f);
    audio.deltaPitch.assign(200, 0.0f);

    Note note(0, 100, 60.0f);
    note.setSrcStartFrame(0);
    note.setSrcEndFrame(100);
    note.setOriginalMidiNote(60.0f);
    project.addNote(note);
    return project;
}

void testUneditedProjectDoesNotPublish()
{
    auto project = makeNeutralProject();
    CHECK(project.getNotes()[0].isNeutralForOriginalWaveform());
    CHECK(!projectHasPublishableEdits(project));
    std::cout << "unedited project does not publish: ok\n";
}

void testPitchPercentAlonePublishes()
{
    auto project = makeNeutralProject();
    project.setPitchPercent(120.0f);
    CHECK(projectHasPublishableEdits(project));

    // Back at exactly 100 there is no edit left, so the gate closes
    // again - that part was always correct; what was broken is that
    // it stayed closed at 120.
    project.setPitchPercent(100.0f);
    CHECK(!projectHasPublishableEdits(project));
    std::cout << "pitch percent alone publishes: ok\n";
}

void testLengthPercentAlonePublishes()
{
    auto project = makeNeutralProject();
    // The plain setter, deliberately without scaleNotesForLengthPercent:
    // a value restored without its frames (deserialisation, undo
    // bookkeeping) must still count as an edit.
    project.setLengthPercent(80.0f);
    CHECK(projectHasPublishableEdits(project));
    std::cout << "length percent alone publishes: ok\n";
}

void testGlobalOffsetsStillPublish()
{
    auto formant = makeNeutralProject();
    formant.setFormantShift(2.0f);
    CHECK(projectHasPublishableEdits(formant));

    auto offset = makeNeutralProject();
    offset.setGlobalPitchOffset(1.0f);
    CHECK(projectHasPublishableEdits(offset));
    std::cout << "global formant / pitch offset still publish: ok\n";
}

void testNoteEditStillPublishes()
{
    auto project = makeNeutralProject();
    project.getNotes()[0].setFormantShift(2.0f);
    CHECK(projectHasPublishableEdits(project));
    std::cout << "a note edit still publishes: ok\n";
}

void testRestNoteDoesNotPublish()
{
    Project project;
    auto &audio = project.getAudioData();
    audio.sampleRate = SAMPLE_RATE;
    audio.f0.assign(200, 220.0f);
    audio.basePitch.assign(200, 57.0f);
    audio.deltaPitch.assign(200, 0.0f);

    Note rest(0, 100, 60.0f);
    rest.setSrcStartFrame(0);
    rest.setSrcEndFrame(100);
    rest.setOriginalMidiNote(60.0f);
    rest.setRest(true);
    rest.setFormantShift(2.0f); // an edit on a rest reaches no audio
    project.addNote(rest);
    CHECK(!projectHasPublishableEdits(project));
    std::cout << "an edited rest does not publish: ok\n";
}

} // namespace

int main()
{
    testUneditedProjectDoesNotPublish();
    testPitchPercentAlonePublishes();
    testLengthPercentAlonePublishes();
    testGlobalOffsetsStillPublish();
    testNoteEditStillPublishes();
    testRestNoteDoesNotPublish();
    std::cout << "PublishGateTests passed\n";
    return 0;
}
