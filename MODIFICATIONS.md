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

### Batch 3.2 (vanishing-segments view-state fix, models folder move, zoom limits)

The batch 3.1 trial showed the resize fix did **not** cure the
"all segments (and the waveform) disappear and never come back"
report. Follow-up reports from the same machine narrowed it down:
the edit data survives the disappearance (pitch edits are intact
afterwards), closing the editor while everything is visible and
reopening it brings the blank state back, and Nuendo's Add/Remove
Event - which rebinds the editor without the captured state -
revives the display. That pattern points at the captured view state,
not at lost project data. The piano roll keeps its view state in two
stores - `PianoRollComponent`'s `viewState` (which viewport capture
reads) and the `CoordinateMapper` (which every renderer draws from) -
and before this batch the two were only partially synchronised:
`updateScrollBars()` pushed the scroll offsets but never the zoom
factors, several setters early-returned on "no significant change"
without checking the other store, and JUCE's `jlimit` passes NaN
straight through, so a single non-finite scroll or zoom value could
enter the state and could never be clamped back out of it.
**Which exact mechanism fired on the user's machine is unverified**
until the next trial: the fixes below are layered so that each
candidate mechanism (non-finite values, store divergence, degenerate
restored scroll, empty-project write-back) is covered independently,
and the new diagnostics log records enough to identify the mechanism
from that trial's log file.

- **Finite-value guards and complete two-store sync
  (PianoRollComponent).** The view-state setters
  (`setPixelsPerSecond`, `setPixelsPerSemitone`, `setScrollX`,
  `setScrollY`) and `scrollBarMoved()` now reject non-finite input,
  and refuse to compute from an already-corrupt state.
  `updateScrollBars()` - which runs on every resize and at the end of
  most view operations - now checks all eight values (both stores x
  scrollX/scrollY/pixelsPerSecond/pixelsPerSemitone) for finiteness,
  pushes the zoom factors to the CoordinateMapper as well as the
  scroll offsets, and, on detecting a non-finite value, recovers to a
  fit-equivalent sane state (whole timeline fitted horizontally, the
  project's pitch range fitted vertically) instead of leaving the
  corrupt value in place. The setters' "no significant change" early
  returns no longer skip the sync when the other store has drifted.
  Every recovery is logged with the corrupt values it replaced.
  All of these finiteness checks inspect the exponent bits directly
  instead of using `std::isfinite`: Release builds of pitchnet_core /
  pitchnet_ui are compiled with `-ffast-math`, whose
  `-ffinite-math-only` folds `std::isfinite` / `std::isnan` to
  constant results, which would compile the guards away in exactly
  the builds that ship. (The same applies to the pre-existing
  `std::isfinite` guard in `fitPitchRangeToView`, which was inert in
  Release builds for the same reason and now uses the bit-level
  check too.)
- **Capture / restore guards (MainComponent).**
  `getViewportState()` no longer captures corrupt values: a
  non-finite scroll is replaced by 0, and a non-finite or
  out-of-range zoom marks the capture invalid (so the next open uses
  the default fitted view). `restoreViewportState()` validates each
  field before applying it and, afterwards, runs a sanity check: if
  the project has segments but the restored view contains none of
  them (a finite-but-degenerate scroll, e.g. parked far past the
  content), the view is refitted to the content. That check runs only
  at restore time, never during normal scrolling.
  (Review follow-up: the visible-range count behind that check,
  `countNotesInVisibleRange()`, now uses exactly the note renderer's
  culling math - project-time window with no timeline-start term,
  `framesToSeconds()` on the visual frames, and the segment-style
  ghost range - instead of its first version, which subtracted
  `getTimelineStart()` and timed notes as raw frames over the sample
  rate and so drifted from the drawn view in Track mode and at any
  scrolled position.)
- **Maximum zoom-in raised 4x (Constants).** `MAX_PIXELS_PER_SECOND`
  goes 500 -> **2000** and `MAX_PIXELS_PER_SEMITONE` 120 -> **480**,
  for fine narration pitch editing: at 2000 px/s one analysis frame
  (512 samples) spans ~23 px, and at 480 px/semitone one cent spans
  ~4.8 px. Every limit consumer reads these constants - the piano
  roll setters and wheel zoom, the CoordinateMapper, the scroll
  clamps, the toolbar and workspace zoom sliders, the overview, and
  the capture/restore validation above - so the new limits are
  consistent everywhere with no other code changes; the batch 3.2
  guards were written against the constants, not literal values.
  Rendering cost at high zoom stays bounded by the existing culling:
  the renderers draw only the visible range, and the waveform
  background cache is a viewport-sized strip (capped at 8192 px
  wide), not a whole-content image.
- **ARA write-back guard (PluginProcessor, Project).**
  `updateProjectStateFromEditor()` now skips a write-back whose
  incoming project is empty of analysis (no notes, no F0, no
  waveform) when the stored snapshot or active region project holds
  analysis data, and the editor-teardown slot assignment in
  `setMainComponent()` keeps an analysed stored region project over
  an unanalysed outgoing one. Analysed-to-analysed write-backs -
  normal editing - are unaffected; a waveform-only project (audio
  loaded, analysis not yet produced) is deliberately not blocked.
  The shared decision lives in `projectHasAnalysisData` /
  `projectIsEmptyOfAnalysis` / `shouldPreserveStoredProject`
  (Project.h); "has analysis" requires notes or F0 but not a
  waveform, because state-archive-restored projects carry notes + F0
  and no waveform.
- **Diagnostics (viewstate log).** A new always-on log,
  `viewstate_<session>.txt` in the plugin's logs folder (the regular
  debug log is compiled out of release builds), records: editor open
  (which store the project was bound from, note/F0/waveform counts
  for the bound project, the snapshot and the stored region slot),
  editor close (the same summaries for the outgoing project and both
  stores), viewport capture and restore lines with **both** stores'
  four values side by side plus how many notes fall inside the
  visible range and the kind of the last synchronisation, recovery
  events with the corrupt values replaced, and thinned `resized()`
  lines (at most one per second, only when the view values changed).
  This is what the next trial's log will be read against to confirm
  the mechanism.
- **Tests.** New `ViewStateRecoveryTests` (fault injection through
  the public setters, direct store corruption, and
  `restoreViewportState`; two-store agreement assertions; a `paint()`
  pixel check after a corruption + resize sequence; the raised zoom
  limits), `AraProjectSyncGuardTests` (the write-back guard decision
  and a simulated close/reopen-while-analysis-pending lifecycle) and
  `PlatformPathsTests` (new/legacy root resolution order). A pre-fix
  reproduction attempt against the batch 3.1 tree is recorded
  honestly: under this toolchain's `-ffast-math` Release flags the
  injected NaN values were folded by the clamps into boundary values
  instead of persisting as NaN, so a deterministic "NaN forever"
  reproduction was not obtained on Linux/GCC - the shipped Windows
  build is MSVC, whose fast-float behaviour differs, and the
  finite-but-degenerate captured-state route needs no NaN at all.
  The guards are bit-level and flag-independent for that reason.
- **Models folder: the vendor level is dropped from the shared-data
  path.** Models now live at `%PROGRAMDATA%\FlexaTune\models`
  (previously `%PROGRAMDATA%\Keinoaza\FlexaTune\models`); the
  installer places them there and its uninstall prompt points there.
  The plug-in prefers the new roots (per-user, then common) and falls
  back to the legacy Keinoaza roots when the new ones have no models
  folder, and additionally probes the legacy roots per file, so batch
  3 / 3.1 installs keep finding their models; the old folder is never
  moved or deleted. The same root also carries the bundled `lang/`
  and `fonts/` resources, which move with it. Unchanged, as
  specified: the bundle ID (`com.keinoaza.flexatune`), the
  manufacturer code (`Kein`), `%APPDATA%\FlexaTune` (settings and
  logs), and the Program Files install destination
  (`{commonpf64}\Keinoaza\FlexaTune` in the installer script - the
  vendor level was only dropped from the shared-data path).

### Batch 3.1 (DirectML session ladder, resize fixes)

Follow-up to the batch 3 trial on the user's machine (2026-11-10): the
BigVGAN log showed DirectML loading the model but every inference
failing on a ConvTranspose node (`node_convolution`, empty status
message), so batch 3's fallback rendered everything on CPU at roughly
9-10x slower than realtime. This batch attacks the GPU path and fixes
three display bugs from the same trial. **Which ladder rung works on a
real GPU is unverified**: no environment available during development
has a DirectML GPU, so the fix is verified by construction, CPU
behaviour and tests, and on the user's machine by the bigvgan log at
the next trial (every rung's outcome and the adopted rung are logged).

- **Opset 17 model variant.** `tools/convert_bigvgan.py` now also
  produces `bigvgan_v2_44khz_128band_opset17.onnx` alongside the main
  export (opset 20 / IR 10, unchanged). Neither exporter can target
  opset 17 for this graph (the legacy TorchScript exporter fails on
  the alias-free activation's Pad -> Conv; the Dynamo exporter's
  automatic downgrade has no version adapter for Pad), so the variant
  is derived by re-stamping the exported graph's opset declaration
  from 20 to 17 (the IR version stays 10). The graph uses no post-17
  operator features; the derivation is fail-closed (an operator
  inventory allowlist, no Pad "axes" input, the full ONNX checker
  under the opset 17 schema, an ONNX Runtime smoke test, and a
  numerical cross-check against the main model with tolerance 1e-4).
  Measured when this was adopted: the variant's CPU output is
  bit-identical to the main model's (max abs error 0.0), and both sit
  1.23e-05 (max abs) from the PyTorch reference - no quality change.
  The release zip - and therefore the installer, which copies the
  whole models folder - now bundles both files (~490 MB each, so the
  download grows by roughly one more model). The main model alone
  still gates AI Resynthesis availability; the variant is optional
  everywhere it is consumed.
- **Session ladder (BigVGANVocoder).** Session establishment now walks
  a ladder and adopts the first rung whose **probe inference** (a
  fixed 16-frame mel, which runs every node of the fully convolutional
  graph) succeeds: (1) DirectML + main model + full graph
  optimization, (2) DirectML + main model + basic graph optimization,
  (3) DirectML + opset 17 model + full graph optimization,
  (4) DirectML + opset 17 model + basic graph optimization, (5) CPU +
  main model (batch 3's fallback configuration). The DirectML rungs
  exist only in DirectML builds, and when the provider cannot be
  appended at all the remaining DirectML rungs are skipped. Every
  rung's success or failure and its reason, the adopted rung, and the
  probe duration are written to the bigvgan log; a probe failure never
  surfaces as a render failure. The ladder runs once per model load,
  not per render. Batch 3's inference-time behaviour is unchanged: a
  real render that still fails on the GPU recreates the session on CPU
  (from the model file the live session was built from) and retries
  once. The CPU thread cap (~70%) and the generic DirectML device-0
  selection are unchanged; there is no GPU-model-specific branching.
- **Stale failure reasons cleared (batch 3 review, minor 1).**
  `BigVGANVocoder::lastError` is cleared on a successful load and on a
  successful inference, and `IncrementalSynthesizer` clears its
  last-synthesis-error at the start of every render, so a failure can
  no longer be labelled with a previous render's reason.
- **Segments vanishing on panel close / window resize.** The piano
  roll's vertical view state was the one piece not re-validated on
  resize: `updateScrollBars()` re-clamped scrollX against the new size
  but not scrollY, and `scrollBarMoved()` applied the vertical
  scrollbar's range start unclamped - a value JUCE itself moves when
  it clamps the visible range into the total range (for example when
  the window is taller than the whole pitch space), which then fed
  back into the view. Both places now clamp scrollY exactly the way
  `setScrollY()` does, so a resize or a panel slide re-validates the
  view against the new size. Covered by the new PianoRollResizeTests:
  the initial fit survives grow and shrink resizes with every segment
  on screen, and the zoomed-out tall-window regime clamps to scrollY 0
  instead of parking the view outside the pitch space.
- **Initial vertical fit margin.** The fit that frames the segments
  (the initial fit when a project opens, Shift+F, the vertical
  auto-zoom) previously widened only the top of the analyzed range by
  1 semitone. It now adds **3 semitones on both sides** (clamped to
  the representable pitch range, MIN_MIDI_NOTE..MAX_MIDI_NOTE), so a
  project opens more zoomed out vertically, as the trial asked.

### Batch 3 (FlexaTune)

Trial feedback on the batch-2 build, implemented as one batch.

- **Rebrand and folder separation.** The product was renamed twice
  within this batch: first from Kinu Tune to Pure Tune, then - by the
  user's decision on 2026-11-10, before any build was publicly
  shipped under either new name - to **FlexaTune**, which is the
  shipping name. Earlier references to Pure Tune in this section
  describe the same work. The product is FlexaTune
  everywhere user-visible: the toolbar logo, splash and application
  strings, all six localisations, error messages. `PlatformPaths`'
  product folder is "FlexaTune", so settings and logs live in
  `%APPDATA%\FlexaTune` (macOS `~/Library/FlexaTune/` etc.) and
  release builds load models from `%PROGRAMDATA%\Keinoaza\FlexaTune\models`
  - fully separate from both the shared Kinu Tune folders and the
  short-lived Pure Tune folders, which are left untouched (no
  migration: the plugin had never been used in a real project, so
  the internal identity changed with the name - bundle id
  `com.keinoaza.flexatune`, plugin code `FlxT`, and a new installer
  AppId). The Windows installer script is renamed
  `packaging/windows/FlexaTune.iss`, installs the models into the new
  folder, and at uninstall time offers (default: No) to also delete
  FlexaTune's own models and settings folders; the old
  `packaging/windows/PitchNet.iss`, which only held the upstream
  update-check URL, is deleted. Internal identifiers are deliberately
  NOT renamed (the `VariAudioInspector` class and file names, test
  names, provenance comments): they are not user-visible and renaming
  them would churn the whole tree. Caveat: an elevated uninstall
  resolves `{userappdata}` to the elevating user's profile.
- **Dead code.** `checkForUpdatesOnLaunch` and everything it dragged
  in (version helpers, the update dialog, the skipped-version
  setting, the `update.*` strings, the sessionloops.com URL) are
  removed; no reference remains.
- **AI resynthesis failure handling.** BigVGAN inference now falls
  back at the inference level, not just at session creation: if a
  chunk fails or throws on a DirectML session, the session is
  recreated on CPU and the whole synthesis retried once. The BigVGAN
  log file is written in all builds, and a failed render's reason is
  carried to the Rendering card's status line (full text as a
  tooltip) through IncrementalSynthesizer's last-error slot. The
  AI engine's availability check includes ONNX Runtime availability.
- **UI moves.** The project-wide pitch % / formant / length % faders
  moved from the Rendering/Time cards to a new Global card at the
  top of the left inspector column. Changes Bypass is now a simple
  power-symbol toggle in the toolbar, next to the live-audition
  toggle (the labelled Rendering-card switch is gone). The Language
  card sits at the bottom of the right panel. Both side panels start
  open - read as: the parameter panel starts shown and the left
  slide panel starts expanded (the inspector column itself is
  always visible). The timeline display defaults to Time (it is not
  serialised, so this is the default in the Project macros, the
  panel and the piano-roll fallback alike). Opening a project fits
  all segments once (horizontal fit + pitch-range fit, via the
  existing zoom-to-full). The wheel zoom step doubled (0.3 to 0.6
  per notch, one shared constant).
- **Length % rework.** Scaling is anchored at the earliest segment
  start, which never moves; only the tail stretches or shrinks.
  `Project::getOutputFrameCount()` defines the output grid (analysis
  span scaled around the anchor, extended to cover every note end)
  and everything renders against it: the F0 dirty range, the
  adjusted-F0 reads past the dense grid (composed from the covering
  note), the synthesizer's range clamping, blend-mask voiced-ness
  past the grid (note coverage), the NSF mel range, and the
  composite waveform, which grows for a stretched tail and is
  trimmed back when a render reaches a shrunken output end. This
  fixes both the drift and the "200% = silence" failure. Projects
  saved by the pre-anchor build re-derive their base table with a
  small one-time error bounded by the first segment's offset. Limit:
  what the host plays past the original clip/event end is still the
  host's business (DOP only appends a fixed tail), so a stretched
  render can exist in the project beyond what an event plays.
- **Pitch-line gaps.** Speech segmentation bridges F0 drop-outs up
  to 6 frames (~70 ms, was 3). The timing source map is now bridged
  across the gaps between retimed notes (linear between the
  neighbouring notes' map values) and the NSF mel path bridges the
  same way, so joins under time scaling no longer jump between
  unrelated source frames. Honest limits: this removes the map
  discontinuity; it cannot invent pitch where the analysis found
  none, and gaps inside a note's own source span are unchanged.
- **Operations.** Final modifier map on the piano roll:
  - Alt+click on a segment: split at the pointer. Alt on a segment
    edge: boundary drag. Alt on the tilt handles: rotate. (All
    unchanged Select-mode behaviour; Alt-double-click on empty
    canvas still fits the view.)
  - Ctrl+Alt held: freehand drawing tool (borrowed for the gesture).
  - Ctrl+Alt+Shift held: stretch (timing) tool, likewise borrowed.
  - Ctrl+click: toggle the segment in the selection. Ctrl held
    during a segment pitch drag: fine (snap-off) drag. Shift+Ctrl:
    seek / scrub. Middle-drag: pan. Wheel: zoom; Shift+Ctrl+wheel:
    vertical zoom.
  - Removed: the batch-2 Alt zoom rectangle and Alt-click zoom-out,
    and the Ctrl-hold zoom drag (with their cursors). An Alt+drag on
    a segment is a split press followed by no drag, as before; the
    relative-snap Alt drag variant is gone with the rectangle.
  - Redo is Ctrl+Shift+Z; Ctrl+Y remains as an alias (Mac already
    used Cmd+Shift+Z). No key-assignment settings screen.
  - The pitch-correction fader is bipolar (-100..100): negative
    values push the pitch away from the target by that share of the
    sung deviation, as a fine adjust. `correctionShare` covers
    [-1, 1] with dust snaps at the landmarks; a pitch sitting
    further from its target than sung now reads as that negative
    value instead of 0 (the reference-carrying rule for other edits
    is unchanged). The on-canvas Correct Pitch handle keeps its
    previous behaviour.
  - Freehand drawing applies immediately: stroke release commits a
    normal undoable action; the confirmation panel is deleted.
    Erasing (right-click / double-click) a committed drawing resets
    the +-10-frame window to the note's analysed contour as its own
    undoable action, since after a commit the drawing lives in the
    note's centre and regenerated base curve.
- **Insert trial removed.** The batch-2 streaming insert prototype
  (StreamingPitchShifter, its APVTS parameters, InsertEffectView,
  the processBlock early return, its latency reporting and tests)
  is deleted; non-ARA use is the upstream capture/import editing
  workflow again, with latency 0 in both modes.

### Identity
- Product name Kinu Tune, vendor Keinoaza, plugin codes `Kein` / `Kntu`
  (`Kntd` for development builds), bundle id `com.keinoaza.kinutune`.
- Settings folder `~/Library/Kinu Tune/` (macOS), logs `~/Library/Logs/Kinu Tune/`, models
  `~/Library/Application Support/Keinoaza/Kinu Tune/models/`.
- New icon and logo images.
- Version number set to 0.1.0, then 0.1.1, 0.1.2, 0.1.3 and 0.1.4.

### Privacy
- The online update check of PitchNet is disabled. (Batch 3: the dead
  update-check code, its orphaned helpers, dialog and strings are
  removed entirely; the shipped build never contained a caller.)
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
  `getAiResynthesisSupport` reports support, a saved AI choice is
  kept, and the engine can be selected; the default stays PSOLA and the NSF
  vocoder code is untouched. The model (~490 MB) is never committed:
  `tools/convert_bigvgan.py` generates it from the official checkpoint
  (Dynamo exporter, weights inlined into a single file) and the Windows CI
  and release workflows run it before configuring; the release zip ships it
  in its models folder. New code: `Source/Audio/BigVGANVocoder.{h,cpp}`
  (ONNX wrapper, DirectML with CPU fallback; chunked inference with
  trimmed context frames),
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
- BigVGAN speed work (it rendered far slower than the NSF vocoder): in
  DirectML builds `BigVGANVocoder` appends the DirectML execution
  provider exactly like the NSF vocoder (memory pattern off, sequential
  execution, single ORT thread), retries session creation with CPU
  options when that fails, logs the provider it ended up on, and exposes
  it through `isUsingGpu()` / `activeProviderName()`.
  `getAiResynthesisSupport` reports Accelerated in that case (DirectML
  provider present plus a hardware adapter) and CPU-only otherwise. On
  the CPU the intra-op thread budget is about 70% of the logical cores
  rather than half, leaving the host DAW headroom, and inference chunks
  grow from 200 to 512 frames so the trimmed 32-frame context costs
  proportionally less. The Windows CI and release workflows now
  configure with `-DUSE_DIRECTML=ON -DUSE_BUNDLED_DIRECTML_RUNTIME=ON`
  (they previously forced DirectML off), so the redistributable
  DirectML.dll ships next to the plug-in instead of depending on the
  inbox Windows version.

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

### Batch 2: remaining VariAudio operations, freehand drawing, global percent controls, insert prototype (2026-11-09)

Operations (gap-list U1/U4, B2/B4-B8/B12):

- U1: middle-drag pans the view (it scrubbed the playhead before);
  scrubbing moved to Shift+Ctrl+drag, extending the batch-1
  Shift+Ctrl+click seek. U4: a plain drag starting on empty canvas is
  box selection, Nuendo-style (it panned the view before); panning
  remains on middle-drag and the scrollbars.
- B2: Alt+drag rectangle zooms in on the rectangle; Alt+click (a press
  that moves less than 6 px) zooms out; Alt+double-click fit-to-view
  already existed. Alt on a pitch-tool handle still reaches the tilt
  tool, which is checked first.
- B8: a Split smart control at the segment's bottom-centre edge. This
  supersedes the 2026-10-19 layout note that the default handle set
  has no split control.
- B12: Absolute snap lands on semitones only (drag and arrow-key
  nudge); scale-note snapping survives only in Relative mode.
- B5/B6: tilt gains a movable anchor (`Note::tiltAnchor`, default the
  centre, serialised only when moved), shown as a diamond on the top
  edge; Alt+tilt rotates the tilt line about the anchor instead of
  tilting around the centre.
- B7: straighten gains a range (`Note::straightenRangeStart/End`,
  serialised): vibrato scaling applies fully inside the range and
  feathers out over up to 5% of the note on either side
  (`PitchToolOperations::scaleVibratoInRange`).
- B4: Alt+press within 6 px of a segment edge drags that boundary:
  the output and source edges move together, a touching neighbour
  follows, and gaps (frames owned by no note - the speech build makes
  no rest notes) and other segments clamp the drag on both timelines.
  Committed as `SegmentBoundaryAction` (undoable). Trade-off: an Alt
  press exactly on an edge no longer starts an Alt split drag.
- Ctrl fine adjust: holding Ctrl during a segment pitch drag disables
  snapping for as long as it is held (resolved per mouse event, like
  the existing Shift = Off). On Windows Ctrl arrives as the command
  modifier, which previously selected Absolute during drags; Ctrl+click
  (selection toggle) and Shift+Ctrl+click / Shift+Ctrl+wheel are
  unaffected because a Ctrl+click never starts a pitch drag.
- U6: pitch drawing is freehand now. The drag stroke itself is the
  curve: it writes the preview curve directly for frames owned by the
  note under the pointer (frames owned by no note are skipped, so a
  gap breaks a stroke; unvoiced frames inside a note are written).
  On release the stroke is smoothed (5-point moving average, ends
  feathered) and the pre-existing confirmation panel, baked-delta
  machinery and `PitchDrawingAction` undo apply unchanged.
  Right-click / double-click erases the drawn curve over +/-10 frames
  back to the original.

Global percent controls (U7):

- Pitch % (50-200) and length % (50-200), default 100, live in
  `MacroParameters` and are serialised with the project (JSON and the
  binary archive). Pitch % scales the synthesis F0 after all per-note
  edits (preserved-timing spans are restored from the source F0
  afterwards and stay unscaled); 100 restores the render exactly.
- Length % scales every note's output timing against a runtime base
  table (never serialised): the base is the current timing whenever
  100 is in effect, and timing/boundary edits re-derive it, so manual
  edits and the percent compose. Tail semantics: the composite buffer
  length stays at the analysis frame count - above 100% stretches the
  notes within it (content past the end is truncated), below 100%
  compresses them and the vacated tail follows the existing
  warp/vacated-region behaviour. Returning to 100 restores the
  original timings, except that manual timing edits made while a
  percent was in effect can leave a rounding drift of at most one
  frame.
- The ARA parameter panel has three global faders: Global Pitch %,
  Global Formant and Global Length %. Global Formant is the
  pre-existing `Project::formantShift` setting made panel-editable
  (the host's formantShift parameter writes the same value; a host
  change is reflected in the panel on the next project bind). Pitch %
  and formant commits mark the full F0 range dirty and re-render like
  a DAW automation change; length % commits are one
  `LengthPercentAction` undo step. These faders exist in ARA mode
  only: the non-ARA mode is the insert prototype below, which has no
  length control (a plug-in cannot declare a scaled output length to
  the host - DOP offers only a user-specified tail).
- Every fader added in this batch (panel and insert) is a
  `GlobalFaderSlider`: double-click opens direct numeric entry, and
  Alt+click resets to neutral (100% for the percents, 0 for formant).

Insert prototype (non-ARA mode; spec change replaced the earlier
"percent-only simple view" plan):

- Non-ARA is a realtime streaming insert effect now, self-contained in
  `Source/Audio/StreamingPitchShifter` so it can be dropped as a unit:
  TD-PSOLA stretch-then-resample pitch shift by percent, with
  similarity-aligned grains (each grain's analysis centre is
  cross-correlated against the intermediate tail - textbook spacing
  alone tears the waveform at 1.5x, verified numerically), NSDF pitch
  estimation (1024-sample window, 256 hop, 55-420 Hz) and formant
  correction by PSOLA grain length. No AI.
- Latency is fixed at 2048 samples (46.4 ms at 44.1 kHz, 42.7 ms at
  48 kHz), reported via `setLatencySamples` whenever no ARA document
  is bound (ARA reports 0) and verified by
  `StreamingPitchShifterTests`: an impulse's energy centroid must
  land within +/-300 samples of the reported 2048 (grains smear an
  impulse, so the centroid - not an exact sample position - is the
  measure), and a 220 Hz sine must measure within a few percent of
  330 Hz at 150% and of 110 Hz at 50%. The grain chain and the reader
  are both seeded on the first `process()` call from the ratio then
  in effect, so a percent change between `prepare()` and the first
  block - the normal host state-restore order - does not shift the
  delay; the tests cover that order and a re-prepare. All buffers
  are allocated in `prepare()`; `process()` allocates nothing. At
  pitch 100% and neutral formant the output is the input bit-exact
  (the pipeline keeps running internally so engaging a shift is
  seamless, but the dry signal is what leaves). Note the host still
  compensates the reported latency in that state, so a neutral insert
  can sit 2048 samples early in hosts that apply compensation to a
  plug-in whose wet path is bypassed internally.
- Routing: `processBlock` sends the block to the insert effect
  whenever no ARA document is bound (`araDocumentController` is the
  audio-thread-safe signal), before the playhead tail, so no
  transport is needed. The editor then shows exactly two faders -
  the APVTS parameters `insertPitchPercent` (50-200) and
  `insertFormant` (-12..+12 st) - and no capture UI. The legacy
  capture classes and their tests stay compiled and untouched; the
  capture path still serves ARA instances whose renderer lets a block
  fall through.
- Limitations: the resampler is linear interpolation (some aliasing
  at large ratios), the formant shift is the grain-length
  approximation rather than a spectral-envelope shift, dry/wet is not
  applied in insert mode (the output stage is gain only), and more
  than two channels are not processed (the shifter is prepared for at
  most two).

CI:

- `.github/workflows/ci.yml` roles swapped: `windows-build` is the
  release gate - it builds the VST3, builds the full unit-test suite
  and runs every test with ctest, and keeps the check-vst3.ps1 host
  check. `macos-tests` is compile-only (tests not configured, ctest
  not run) while still proving the product builds for macOS.

Post-review fixes (independent review of this batch):

- Project-wide edits - the global pitch offset, the global formant
  shift and the pitch percent - now go through one predicate,
  `Project::hasProjectWideEdit()`. The Changes Bypass pass decided
  "edited" per note from `Note::isNeutralForOriginalWaveform()` alone,
  so a project whose only edit was a global percent left the bypass
  with nothing dirty and was never re-rendered (the render request
  gates on dirty notes); `Project::markEditedNotesDirty()` likewise
  missed the pitch percent, dropping those projects on engine
  switches. Both paths now treat every editable note as edited while
  a project-wide adjustment is in effect; deleted segments stay on
  the pristine original. Covered by ChangesBypassTests (the
  pitch-%-only bypass round trip) and SynthesisEngineShareTests.
- Freehand pitch drawing no longer touches deleted segments: the
  handler's frame-ownership, original-state capture, position-update
  and apply loops used `Note::isRest()` and now use
  `Note::isRestOrDeleted()`, so a stroke breaks across a deleted
  segment exactly as it breaks across an unowned gap
  (VariAudioInspectorTests).
- The freehand smoothing kernel no longer averages across a note
  boundary: every kernel frame must belong to the same note as the
  frame being smoothed, so two directly adjacent segments no longer
  bend each other's joins.
- StreamingPitchShifter latency: the grain chain and the reader are
  now both seeded on the first `process()` call from the ratio then
  in effect, instead of seeding the chain in `reset()` with whatever
  percent happened to be set when `prepare()` ran. A percent change
  between prepare and the first block (the normal host state-restore
  order) shifted the two scales apart and moved the effective delay
  away from the reported 2048 samples (about 380 samples in the
  worst ordering the tests exercise). StreamingPitchShifterTests
  covers the prepare/percent-change and re-prepare orders, and the
  latency paragraph above now states the test tolerance as it is
  (+/-300 samples on the impulse centroid).

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
  user's home; `packaging/windows/FlexaTune.iss` is the Windows installer, built
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
- `.github/workflows/ci.yml` builds the Windows VST3 and runs the full unit
  tests there on every push (Windows is the release gate since batch 2); the
  macOS job is a compile-only check. `PITCHNET_REQUIRE_MODELS=OFF` lets it
  configure without the macOS Core ML vocoder.
- dev-rules.md lists the design rules to keep when changing the code.
- cmake/StripBuildRpaths.cmake removes absolute rpaths (the build machine's
  ONNX Runtime folder) from the macOS bundles after linking and fails the build
  if one is left.
- tests/ModelInferenceTests.cpp loads the shipped models and checks analysis
  and synthesis with numbers (FCPE reports a tone's pitch, GAME finds the note,
  the vocoder and PSOLA shift it by a minor third, and AI Resynthesis can be
  chosen on macOS and Windows). Since batch 2 the CI Windows job runs it
  together with the whole unit-test suite.
- Batch 2 tests: GlobalPercentTests (percent model, serializer
  round-trip, length scaling, undo) and StreamingPitchShifterTests
  (bit-exact bypass, measured pitch ratios, measured latency);
  VariAudioInspectorTests and PitchToolOperationsTests grew blocks for
  the batch-2 operations and freehand drawing.
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
  `packaging/macos/installer/*`, `packaging/windows/FlexaTune.iss`,
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
- Batch 2: `Source/Audio/StreamingPitchShifter.h/.cpp`,
  `Source/Plugin/InsertEffectView.h`,
  `Source/Undo/LengthPercentAction.h`,
  `Source/Undo/SegmentBoundaryAction.h`,
  `tests/GlobalPercentTests.cpp`, `tests/StreamingPitchShifterTests.cpp`,
  `tests/PitchToolOperationsTests.cpp` (and `GlobalFaderSlider` added
  to the existing `Source/UI/Sliders.h`)

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
