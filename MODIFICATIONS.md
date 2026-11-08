# Modifications

Kinu Tune is a modified version of [PitchNet](https://github.com/SessionLoops/PitchNet)
0.7.2 by Session Loops, which is itself a modified derivative of
[HachiTune](https://github.com/KCKT0112/HachiTune). This file is the notice of
modification required by section 5(a) of the GNU Affero General Public License,
version 3.

- Modified by: Keinoaza
- Date of modification: 2026-10-19 (first release, Kinu Tune 0.1.0); continued through 2026-10-30 (Kinu Tune 0.1.4)
- Based on: SessionLoops/PitchNet commit `8787321` ("Disable file logging in release builds")
- License of the modified work: GNU Affero General Public License, version 3 (see [LICENSE](LICENSE))

The full history of the changes is in the git log of this repository.

## Summary of changes

### Identity
- Product name Kinu Tune, vendor Keinoaza, plugin codes `Kein` / `Kntu`
  (`Kntd` for development builds), bundle id `com.keinoaza.kinutune`.
- Settings folder `~/Library/Kinu Tune/` (macOS), logs `~/Library/Logs/Kinu Tune/`, models
  `~/Library/Application Support/Keinoaza/Kinu Tune/models/`.
- New icon and logo images.
- Version number set to 0.1.0, then 0.1.1, 0.1.2, 0.1.3 and 0.1.4.

### Privacy
- The online update check of PitchNet is disabled.
- ONNX Runtime telemetry is turned off as far as the documented API allows:
  official ONNX Runtime builds for Windows pass usage events to Windows
  TraceLogging by default, so every ONNX Runtime environment (FCPE and RMVPE
  pitch detectors, GAME, vocoder) calls `DisableTelemetryEvents()` right after it
  is created. Two limits of the official ONNX Runtime binary remain: the one
  start-up event that ONNX Runtime writes while creating its first
  environment (the ONNX Runtime version and build type) comes before this
  call, and ONNX Runtime turns its events back on when a Windows
  event-tracing session enables them again later (until Kinu Tune next
  creates an environment). ONNX Runtime has no telemetry on other platforms.

### Windows loading
- The Windows plug-in links the Visual C++ runtime statically, so it no longer
  depends on the msvcp140.dll a host ships (an older one made it crash on
  insert).
- ONNX Runtime is loaded only from the copy bundled next to the plug-in, on
  every platform through `OnnxRuntime::initialise` (`ORT_API_MANUAL_INIT`);
  the older onnxruntime.dll Windows has in System32 is never used. When the
  bundled copy cannot be loaded (on Windows usually because the Microsoft
  Visual C++ Redistributable is missing), analysis and AI resynthesis are
  turned off instead of crashing, and the editor says how to fix it.

### Cubase VariAudio-style editing
- Notes drawn as rectangular segments with the pitch name on hover; the
  original pitch curve of an edited segment stays visible in grey.
- Smart controls on the hovered segment (straighten curve, correct pitch, glue,
  tilt, formant, volume), shown when zoomed in; "Default" or "Show All".
- Pitch Snap Mode Absolute / Relative / Off, with Shift / Cmd / Alt overrides
  while dragging; arrow keys follow the snap mode; Shift+arrows move by cents;
  left/right arrows select the previous/next segment; Shift-double-click selects
  every later segment of the same pitch.
- A click only selects a segment; Option/Alt-click splits it.
- Double-click resets a segment to its analysed state.
- Smart Controls default to "Default" in a fresh session (VariAudio's
  initial view), not "Show All"; the choice is session-only.
- Delete Segments (Backspace): the selected segments leave the editor and
  their audio returns to the pristine original. The notes stay in the list
  under a new `deleted` flag with every edit (pitch, formant, volume,
  timing, shared-boundary companions) reset through the existing Restore
  machinery, so the synthesizer's restore path copies the original
  waveform back over their ranges on both engines. One undo step restores
  the edits and clears the flag; the flag is saved in project files.
- VariAudio Changes Bypass, a toggle in the Rendering card under the
  engine radios: while on, resynthesis is suspended and playback is the
  pristine original (see the audit note below for how the flags are kept
  consistent). Session-only; it is never written to a project file.
- Shift+Ctrl+click moves the playback cursor (dragging scrubs), taking
  precedence over the Ctrl-click selection toggle; plain Ctrl+click keeps
  toggling a segment in and out of the selection.
- Dragging on empty space scrolls the view; Option/Alt + wheel or drag zooms
  vertically.
- Zoom Full: Shift+F, or Option/Alt-double-click where there is no segment.
  The widest horizontal zoom was lowered so that a song-length take fits on
  screen; grid and ruler lines thin out when zoomed far out.
- Live audition while dragging a segment: the whole segment repeats with a
  0.3 s pause until the mouse is released (ARA, non-ARA plugin and standalone).
  In ARA hosts the audition is played by the editor renderer of the edited
  track.

### Inspector (left column)
- "Tools": smart controls, pitch snap mode, and segment sliders (Correct Pitch,
  Flatten Curve, Pitch Drift, Shift Formant, Volume), each drag one undo step.
- "Editor Scale": root and scale menus, "Use Project Key" switch, snap to scale.
  Replaces the toolbar key/scale button, which was removed.
- "Harmony": one-click scale-degree shifts (3rd, 4th, 5th, 6th up/down, octave
  up/down) for the selection or every note; major/minor thirds follow the
  scale.
- The column scrolls when the editor is shorter than its content.
- The toolbar Pitch Center / Pitch Drift popup button was removed (the same
  adjustments are in the inspector).

### Correct Pitch reference
- Correct Pitch measures from the sung pitch of a segment. Every edit that
  moves a segment's pitch (harmony, arrow keys, drags, track-view drags, the
  other smart controls, tilt reset, snap to semitone) now carries that
  reference along (ScaleUtils::carriedCorrectionSource) instead of replacing
  it with the moved pitch, so the slider keeps its value and effect after a
  move; split and merge keep the gap between reference and pitch. Undo and
  redo restore it.
- The Correct Pitch smart control uses the same reference and scale as the
  inspector slider; neither jumps when a gesture starts.
- Drag-end code reads the f0 size after rebuilding the pitch curves.
- A right/Ctrl click on the cycle ruler no longer arms a cycle drag that took
  over the next drag anywhere in the editor.
- Every write of a note's pitch and its Correct Pitch reference goes through
  Source/Utils/NotePitch.h; the undo actions take the references as required
  arguments.

### Audition output
- The dragged segment's feedback, the piano key tone and the region preview
  sound through one owner (Source/UI/Main/AuditionOutput.h), which holds the
  host preview and audition callbacks. A source that starts takes the output
  from the previous one; play, pause and stop silence the dragged segment and
  the piano key. Behaviour changes where two sources used to fight over the
  output: a piano key or a drag now finishes a running region preview, a
  region preview now stops a sounding drag or key loop, and a transport
  command now also stops a held piano key.

### Rendering engine (AI Resynthesis / Classic)
- Switching the engine re-renders every note whose edits reach the audio
  (Project::markEditedNotesDirty, MainComponent::rerenderEditedNotes). Upstream
  only stored the setting; the audio rendered by the previous engine stayed
  until the next edit, and only that edit's range changed.
- In the plug-in, every EditorController the processor creates shares one
  SynthesisEnginePreference (Source/Plugin/RenderControllers.h), initialised
  from the saved preference. Upstream bound the editor to one controller while
  the controllers that render the canvas region and the background
  modifications kept the default engine, so when the plug-in was opened
  through ARA (as a Cubase extension) Classic never rendered.
- A status line under the engine radios names the engine of the running or
  last render ("Last render: Classic"), since release builds write no log.
- The Windows release is built without DirectML, where upstream disables the AI
  Resynthesis radio; yet machines with a discrete GPU still default to AI, so
  picking Classic once could not be undone. getAiResynthesisSupport
  (Source/Audio/SynthesisEngineType.cpp, next to the default engine) now
  reports CPU-only support there: the radio stays clickable and its tooltip
  says renders take longer. Where AI Resynthesis cannot run at all (ONNX
  Runtime not loaded), an AI choice is moved to Classic and saved instead of
  staying selected but disabled. GpuDeviceList::hasGpuInference is removed.

### Speech build (CM / MA use, no non-commercial models)
- The GAME note-segmentation model and the PC-NSF-HiFiGAN vocoder (both
  CC BY-NC-SA 4.0 weights) are no longer shipped, downloaded or loaded.
  `GAMEDetector::loadModels` returns false unless `KINU_ENABLE_GAME` is defined;
  `defaultSynthesisEngineType`, `synthesisEngineTypeForSavedPreference` and
  `getAiResynthesisSupport` return PSOLA / PSOLA / Unavailable unless
  `KINU_ENABLE_AI_RESYNTHESIS` is defined. The analysis no longer needs the
  vocoder model, and the vocoder is loaded only when it is the selected engine.
- New rule-based note segmentation for speech (`segmentSpeechNotes` in
  `Source/Audio/EditorController.cpp`): notes are the voiced stretches of the
  F0 track, with F0 drop-outs of up to 3 frames bridged, cut at energy dips of
  9 dB or more (syllables), and with heads and tails extended into consonants
  and breath using a level threshold relative to the recording. A note needs
  only 3 voiced frames, so short words are kept.
- CMake no longer downloads GAME or requires the vocoder model;
  ModelInferenceTests checks FCPE and PSOLA only; the "not available" tooltip
  of AI Resynthesis says the build does not include it.
- AI Resynthesis returns through BigVGAN v2 (44kHz / 128 band; code and
  weights MIT), gated on the model file instead of a build flag: when
  `bigvgan_v2_44khz_128band.onnx` is present in the models folder,
  `getAiResynthesisSupport` reports CPU-only support, a saved AI choice is
  kept, and the engine can be selected; the default stays PSOLA and the NSF
  vocoder code is untouched. The model (~490 MB) is never committed:
  `tools/convert_bigvgan.py` generates it from the official checkpoint
  (Dynamo exporter, weights inlined into a single file) and the Windows CI
  and release workflows run it before configuring; the release zip ships it
  in its models folder. New code: `Source/Audio/BigVGANVocoder.{h,cpp}`
  (CPU ONNX wrapper; chunked inference with trimmed context frames),
  `Source/Utils/BigVGANMel.{h,cpp}` (BigVGAN's mel: the existing Slaney
  filterbank at fmin 0 / fmax 22050 with a 1e-5 log floor - a new
  `MelSpectrogram` constructor parameter - plus BigVGAN's uncentred
  768-sample reflection framing). BigVGAN has no F0 input, so
  `IncrementalSynthesizer` renders the edited pitch with PSOLA into a
  scratch waveform and BigVGAN regenerates it from that waveform's mel;
  the result rejoins the normal commit path. `EditorController` owns the
  `BigVGANVocoder` and loads the model in preference to the NSF vocoder
  when AI Resynthesis is selected; the unavailable tooltip now names the
  BigVGAN file to place.

### Non-destructive re-editing (audit, 2026-11-09)

Verified in source, because re-editing a segment must always start from
the original recording, never from the previously rendered audio:

- Rendering reads only `AudioData::originalWaveform` (the copy taken at
  analysis, `EditorController::analyzeAudio`); the PSOLA scratch is built
  from it inside `IncrementalSynthesizer`, and the commit stage reads the
  existing composite only to choose splice points. Reset/deleted segments
  and the Changes Bypass restore from the same buffer.
- The pitch model is level + contour: `basePitch` is a smoothed step
  curve of the notes' current pitch levels (`BasePitchCurve::
  generateForNotes`), and the analysed contour lives in each note's
  `originalDeltaPitch`, which only analysis/segmentation
  (`PitchCurveProcessor::rebuildCurvesFromSource`) and the legacy-load
  repair ever write. Rebuilds (`rebuildBaseFromNotes` and friends)
  recompose the global delta from the stored per-note source
  (`Note::getActiveDeltaPitch`) with the note's transform parameters
  applied; the edited curve and the rendered audio are never re-analysed.
  `composeF0InPlace` writes only the derived cache `AudioData::f0`.
- `denseF0`, the denominator of the synthesis pitch ratio
  (`IncrementalSynthesizer::buildPitchRatioCurve`), is written only at
  analysis and in load fallbacks when the saved array is empty.
- A pitch drag anchors on the gesture start (the note's pitch then, and
  the pointer position) and folds to an absolute target on release
  (`NotePitch::applyEdit`); the F0 snapshot it keeps is for undo only.
- Formant, volume and timing are absolute per-note parameters; source
  frames (`srcStartFrame`/`srcEndFrame`) never change after detection,
  and the rendered-frame bounds are bookkeeping only.
- Saving persists the pristine data verbatim (per-note
  `originalDeltaPitch`/`bakedDeltaPitch` and the global `basePitch`,
  `baseF0`, `deltaPitch`, `denseF0` arrays in both the JSON format and
  the binary archive), and loading restores them verbatim; the
  derive-from-F0 fallbacks run only for legacy files that never stored
  them. `tests/ChangesBypassTests.cpp` and the inspector tests pin the
  bypass and delete behaviour above.

### Defaults
- Pitch Snap Mode Absolute, smart controls "Default", scale shown in the piano
  roll, for new projects. Saved projects keep their saved settings.

### Notices
- THIRD_PARTY_NOTICES.txt: Kinu Tune notice added at the top; GAME and
  PC-NSF-HiFiGAN model weights are listed with their CC BY-NC-SA 4.0 license;
  VST 3 SDK notice added; Kinu Tune binary releases do not include rmvpe.onnx.
- licenses/: the vocoder notice from OpenVPI (unchanged), ONNX Runtime license
  and third-party notices, Apache License 2.0, ARA SDK notice, and the licenses
  of the components compiled in through JUCE.
- Resources/fonts/*-OFL.txt: the SIL Open Font License texts of the bundled fonts.
- `.github/workflows/build.yml` (upstream CI) removed; `.github/workflows/windows-release.yml`
  builds the Windows release; packaging/INSTALL-*.txt are the install notes in
  the release zips. From 0.1.1 the macOS release also contains the Audio Unit
  (`Kinu Tune.component`) besides the VST3.
- Installers: the upstream PitchNet.pkgproj and PitchNet.iss are not used.
  `packaging/macos/build-pkg.sh` builds a macOS package that installs into the
  user's home; `packaging/windows/KinuTune.iss` is the Windows installer, built
  and checked by `.github/workflows/windows-installer.yml`. Both are made from
  the files of the release zips.

### Tests and checks
- New unit tests: HarmonyIntervalTests, HarmonyApplyTests,
  VariAudioControlTests, VariAudioInspectorTests, AuditionOutputTests,
  AuditionHandoffTests, SynthesisEngineShareTests, AiResynthesisChoiceTests,
  ChangesBypassTests,
  and RenderSnapshots (renders
  the piano roll and inspector to PNG files). VariAudioInspectorTests also
  covers Delete Segments (flag, full edit reset, undo/redo, serializer
  round-trip) and the Ctrl+click toggle vs Shift+Ctrl+click seek.
- tests/SourceGuard.cmake: fails when code outside the allowed files writes a
  note's pitch directly, calls the engine's audition loop directly, or creates
  a plug-in EditorController outside Source/Plugin/RenderControllers.h.
- `.github/workflows/ci.yml` runs the unit tests on macOS and builds the Windows
  VST3 on every push; `PITCHNET_REQUIRE_MODELS=OFF` lets it configure without
  the macOS Core ML vocoder.
- dev-rules.md lists the design rules to keep when changing the code.
- cmake/StripBuildRpaths.cmake removes absolute rpaths (the build machine's
  ONNX Runtime folder) from the macOS bundles after linking and fails the build
  if one is left.
- tests/ModelInferenceTests.cpp loads the shipped models and checks analysis
  and synthesis with numbers (FCPE reports a tone's pitch, GAME finds the note,
  the vocoder and PSOLA shift it by a minor third, and AI Resynthesis can be
  chosen on macOS and Windows). It is the one test the CI Windows job also runs, so
  Windows is known to analyse and synthesise.
- cmake/DependencyHashes.cmake records the SHA-256 of every archive CMake
  downloads (ONNX Runtime, the DirectML NuGet packages, the GAME models); a
  download that does not match stops the configure step. The GitHub Actions
  workflows pin the actions they use to commit ids instead of tags.
- tools/windows/check-vst3.ps1 (run by both workflows) creates the Windows
  plug-in in a minimal host (tools/windows/vst3-instantiate) on the runner's
  runtime, on an older Visual C++ runtime and in clean Windows containers with
  and without the redistributable, and runs pluginval.
  OnnxRuntimeAvailabilityTests checks that nothing calls ONNX Runtime when it
  could not be loaded.

## Files

New files:

- `MODIFICATIONS.md`, `licenses/*`, `Resources/fonts/*-OFL.txt`
- `.github/workflows/windows-release.yml`, `.github/workflows/ci.yml`,
  `packaging/INSTALL-macOS.txt`, `packaging/INSTALL-Windows.txt`, `dev-rules.md`
- `.github/workflows/windows-installer.yml`, `packaging/macos/build-pkg.sh`,
  `packaging/macos/installer/*`, `packaging/windows/KinuTune.iss`,
  `tools/windows/check-installer.ps1`
- `Source/UI/HarmonyPanel.h`, `Source/UI/HarmonyPanel.cpp`
- `Source/UI/VariAudioInspector.h`, `Source/UI/VariAudioInspector.cpp`
- `Source/Undo/InspectorEditAction.h`
- `Source/Utils/HarmonyUtils.h`, `Source/Utils/NotePitch.h`
- `Source/UI/Main/AuditionOutput.h`
- `cmake/StripBuildRpaths.cmake`
- `tools/windows/check-vst3.ps1`, `tools/windows/vst3-instantiate/*`
- `tests/HarmonyApplyTests.cpp`, `tests/HarmonyIntervalTests.cpp`,
  `tests/RenderSnapshots.cpp`, `tests/VariAudioControlTests.cpp`,
  `tests/VariAudioInspectorTests.cpp`, `tests/AuditionOutputTests.cpp`,
  `tests/AuditionHandoffTests.cpp`, `tests/SourceGuard.cmake`,
  `tests/OnnxRuntimeAvailabilityTests.cpp`

Removed files:

- `Source/UI/ScaleSelectionControl.h`, `Source/UI/ScaleSelectionControl.cpp`
- `.github/workflows/build.yml`

Modified files (changed by Keinoaza on or after 2026-10-19):

- `.gitignore`, `CMakeLists.txt`, `README.md`, `THIRD_PARTY_NOTICES.txt`, `tests/CMakeLists.txt`
- `Resources/images/icon.png`, `Resources/images/logo.png`
- `Resources/lang/en.json`, `es.json`, `ja.json`, `ko.json`, `zh.json`, `zh-TW.json`
- `Source/Main.cpp`
- `Source/Audio/AudioEngine.h/.cpp`, `Source/Audio/ResampledLoopAudition.h/.cpp`
- `Source/Audio/FCPEPitchDetector.cpp`, `Source/Audio/GAMEDetector.cpp`,
  `Source/Audio/RMVPEPitchDetector.cpp`, `Source/Audio/Vocoder.cpp`
- `Source/Models/Note.h`, `Source/Models/Project.h`, `Source/Models/ProjectSerializer.cpp`
- `Source/Plugin/ARADocumentController.h/.cpp`, `Source/Plugin/PluginEditor.cpp`,
  `Source/Plugin/PluginProcessor.h/.cpp`
- `Source/UI/Commands.h`, `Source/UI/IMainView.h`, `Source/UI/MainComponent.h/.cpp`,
  `Source/UI/Main/SettingsManager.cpp`, `Source/UI/ParameterPanel.h/.cpp`,
  `Source/UI/PianoRollComponent.h/.cpp`, `Source/UI/PianoRollWorkspaceView.cpp`,
  `Source/UI/ToolbarComponent.h/.cpp`, `Source/UI/Workspace/WorkspaceComponent.h/.cpp`
- `Source/UI/PianoRoll/GridRenderer.cpp`, `NoteRenderer.h/.cpp`,
  `PianoRollInteractionContext.h/.cpp`, `PitchEditor.h/.cpp`,
  `NoteSplitter.cpp`, `PitchToolController.cpp`, `PitchToolHandles.h/.cpp`,
  `States/LoopDragHandler.cpp`, `States/PitchDrawingHandler.cpp`,
  `States/SelectHandler.h/.cpp`, `TimelineRenderer.cpp`,
  `WaveformBackgroundRenderer.h/.cpp`
- `Source/Plugin/AraDiagnostics.h`, `Source/Utils/AppLogger.h` (comments)
- `Source/Audio/EditorController.cpp`, `Source/UI/GpuDeviceList.cpp`,
  `Source/UI/SettingsComponent.cpp`
- `Source/Utils/OnnxRuntime.h`, `Source/Utils/OnnxRuntimeLoader.h/.cpp`
- `Source/Utils/Constants.h`, `Source/Utils/Localization.h`, `Source/Utils/OnnxRuntime.cpp`,
  `Source/Utils/PitchMacroScope.h`, `Source/Utils/PlatformPaths.h`, `Source/Utils/ScaleUtils.h`,
  `Source/Utils/TransformParams.h`
- `Source/Undo/DragActions.h`, `Source/Undo/NoteActions.h`
