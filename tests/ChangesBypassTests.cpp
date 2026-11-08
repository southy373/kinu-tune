// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#include "../Source/Audio/Synthesis/IncrementalSynthesizer.h"
#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"
#include "TestAssert.h"
#include <cmath>
#include <iostream>

// VariAudio Changes Bypass: while the flag is set, a render request must
// not resynthesize anything - the composite becomes the pristine original,
// edited notes go back to "not rendered" (dirty, so lifting the bypass
// re-renders them), and the edit data itself is left alone.
int main()
{
    Project project;
    auto& audio = project.getAudioData();
    audio.sampleRate = 44100;

    constexpr int numSamples = 4096;
    audio.originalWaveform.setSize(1, numSamples);
    for (int i = 0; i < numSamples; ++i)
        audio.originalWaveform.setSample(0, i, 0.5f * std::sin(i * 0.05f));
    // The composite starts out as "rendered" audio that differs everywhere.
    audio.waveform.setSize(1, numSamples);
    for (int i = 0; i < numSamples; ++i)
        audio.waveform.setSample(0, i, -0.25f);

    Note edited(0, 100, 60.0f);
    edited.setSrcStartFrame(0);
    edited.setSrcEndFrame(100);
    edited.setOriginalMidiNote(60.0f);
    edited.setFormantShift(2.0f);   // an edit that reaches the audio
    edited.setRenderedEdit(true);   // ...and is currently rendered
    project.addNote(edited);

    Note neutral(100, 200, 62.0f);
    neutral.setSrcStartFrame(100);
    neutral.setSrcEndFrame(200);
    neutral.setOriginalMidiNote(62.0f);
    neutral.setRenderedEdit(true); // inside a rendered cluster, but unedited
    project.addNote(neutral);

    // Not bypassed: the flag defaults off and is not part of a saved project.
    CHECK(!project.isChangesBypassed());
    project.setChangesBypassed(true);
    Project restored;
    CHECK(ProjectSerializer::fromJson(restored,
                                      ProjectSerializer::toJson(project)));
    CHECK(!restored.isChangesBypassed());

    IncrementalSynthesizer synth;
    synth.setProject(&project);
    bool completed = false;
    bool succeeded = false;
    synth.synthesizeRegion(nullptr, [&](bool success)
                           {
                               completed = true;
                               succeeded = success;
                           });
    CHECK(completed);
    CHECK(succeeded);

    // Playback audio is the original again, sample for sample.
    CHECK(audio.waveform.getNumSamples() == numSamples);
    for (int i = 0; i < numSamples; ++i)
        CHECK(audio.waveform.getSample(0, i) ==
              audio.originalWaveform.getSample(0, i));

    auto& notes = project.getNotes();
    // The edited note is un-rendered and dirty: lifting the bypass must
    // render it again. Its edit parameters are untouched.
    CHECK(!notes[0].hasRenderedEdit());
    CHECK(notes[0].isSynthDirty());
    CHECK(notes[0].getFormantShift() == 2.0f);
    // The neutral note needs no re-render.
    CHECK(!notes[1].hasRenderedEdit());
    CHECK(!notes[1].isSynthDirty());
    CHECK(project.hasDirtyNotes());

    // Without an original waveform there is nothing to bypass to.
    Project empty;
    empty.setChangesBypassed(true);
    IncrementalSynthesizer emptySynth;
    emptySynth.setProject(&empty);
    bool emptyCompleted = false;
    bool emptySucceeded = true;
    emptySynth.synthesizeRegion(nullptr, [&](bool success)
                                 {
                                     emptyCompleted = true;
                                     emptySucceeded = success;
                                 });
    CHECK(emptyCompleted);
    CHECK(!emptySucceeded);

    std::cout << "Changes bypass tests passed\n";
    return 0;
}
