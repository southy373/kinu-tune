// Batch 3.2 tests for the ARA project write-back guard.
//
// The decision the processor makes at editor teardown lives in the
// free predicates in Project.h (projectHasAnalysisData /
// projectIsEmptyOfAnalysis / shouldPreserveStoredProject), so it can
// be tested here without a processor instance - no existing test
// constructs one. The lifecycle scenario below replays, at the data
// level, the exact exchange the processor performs: stored region
// slot vs the editor's outgoing project, including the reported case
// "editor closed and reopened while analysis was still pending".
#include "../Source/Models/Project.h"
#include "../Source/Utils/Constants.h"
#include "TestAssert.h"
#include <iostream>

namespace {

Project makeAnalysedProject(int noteCount)
{
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = SAMPLE_RATE;
  const int frames = 200;
  audio.f0.assign(frames, 220.0f);
  audio.basePitch.assign(frames, 57.0f);
  audio.deltaPitch.assign(frames, 0.0f);
  audio.waveform.setSize(1, frames * HOP_SIZE);
  for (int i = 0; i < noteCount; ++i)
    project.addNote(Note(10 + i * 60, 60 + i * 60, 58));
  return project;
}

// Archive-restored projects carry notes + F0 but no waveform (the
// audio stays in the host). They must still count as analysed.
Project makeArchiveStyleProject()
{
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = SAMPLE_RATE;
  audio.f0.assign(200, 220.0f);
  project.addNote(Note(10, 60, 58));
  project.addNote(Note(70, 120, 62));
  return project;
}

Project makeWaveformOnlyProject()
{
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = SAMPLE_RATE;
  audio.waveform.setSize(1, 200 * HOP_SIZE);
  return project;
}

} // namespace

int main()
{
  // --- Predicate matrix.
  const Project analysed = makeAnalysedProject(3);
  CHECK(projectHasAnalysisData(analysed));
  CHECK(!projectIsEmptyOfAnalysis(analysed));

  const Project archiveStyle = makeArchiveStyleProject();
  CHECK(archiveStyle.getAudioData().waveform.getNumSamples() == 0);
  CHECK(projectHasAnalysisData(archiveStyle));
  CHECK(!projectIsEmptyOfAnalysis(archiveStyle));

  const Project waveformOnly = makeWaveformOnlyProject();
  CHECK(!projectHasAnalysisData(waveformOnly));
  // Audio without analysis is not "empty of analysis": the guard must
  // not block a write-back just because analysis has not produced
  // notes yet (e.g. a freshly loaded clip in the same region).
  CHECK(!projectIsEmptyOfAnalysis(waveformOnly));

  const Project empty;
  CHECK(!projectHasAnalysisData(empty));
  CHECK(projectIsEmptyOfAnalysis(empty));

  // --- The guard decision itself.
  // Analysed stored + empty outgoing -> preserve.
  CHECK(shouldPreserveStoredProject(&analysed, &empty));
  // Analysed stored + no outgoing at all -> preserve.
  CHECK(shouldPreserveStoredProject(&analysed, nullptr));
  // Archive-style stored counts as analysed -> preserve.
  CHECK(shouldPreserveStoredProject(&archiveStyle, &empty));
  // Analysed stored + analysed (edited) outgoing -> write back.
  const Project edited = makeAnalysedProject(4);
  CHECK(!shouldPreserveStoredProject(&analysed, &edited));
  // Analysed stored + waveform-only outgoing -> write back (see the
  // predicate note above).
  CHECK(!shouldPreserveStoredProject(&analysed, &waveformOnly));
  // Nothing analysed stored -> nothing to preserve.
  CHECK(!shouldPreserveStoredProject(nullptr, &empty));
  CHECK(!shouldPreserveStoredProject(&empty, &empty));
  CHECK(!shouldPreserveStoredProject(&waveformOnly, &empty));

  // --- Lifecycle: the stored region project survives an editor that
  // is closed and reopened while analysis is pending (its outgoing
  // project is a fresh, empty one), using the same decision and
  // exchange shape as PluginProcessor::setMainComponent's teardown.
  {
    std::unique_ptr<Project> storedSlot =
        std::make_unique<Project>(makeAnalysedProject(3));

    // Editor opens in the analysis-pending state and closes again:
    // its project never got analysed.
    std::unique_ptr<Project> outgoing = std::make_unique<Project>();
    if (!shouldPreserveStoredProject(storedSlot.get(), outgoing.get()))
      storedSlot = std::move(outgoing);
    CHECK(storedSlot != nullptr);
    CHECK(projectHasAnalysisData(*storedSlot));
    CHECK(storedSlot->getNotes().size() == 3);

    // Reopen, edit for real, close: the analysed edit writes back.
    std::unique_ptr<Project> editedOutgoing =
        std::make_unique<Project>(makeAnalysedProject(4));
    if (!shouldPreserveStoredProject(storedSlot.get(),
                                      editedOutgoing.get()))
      storedSlot = std::move(editedOutgoing);
    CHECK(storedSlot != nullptr);
    CHECK(storedSlot->getNotes().size() == 4);
  }

  // --- The updateProjectStateFromEditor-level guard, replayed with
  // the same predicates the processor uses: an empty incoming project
  // is skipped when the snapshot holds analysis, and goes through
  // when it does not.
  {
    std::unique_ptr<Project> snapshot =
        std::make_unique<Project>(makeAnalysedProject(3));
    const Project incomingEmpty;
    const bool skip = projectIsEmptyOfAnalysis(incomingEmpty) &&
                      snapshot != nullptr &&
                      projectHasAnalysisData(*snapshot);
    CHECK(skip);
    if (!skip)
      snapshot = std::make_unique<Project>(incomingEmpty);
    CHECK(snapshot->getNotes().size() == 3);

    std::unique_ptr<Project> noSnapshot;
    const bool skipWithoutSnapshot =
        projectIsEmptyOfAnalysis(incomingEmpty) && noSnapshot != nullptr &&
        projectHasAnalysisData(*noSnapshot);
    CHECK(!skipWithoutSnapshot);
  }

  std::cout << "AraProjectSyncGuardTests passed" << std::endl;
  return 0;
}
