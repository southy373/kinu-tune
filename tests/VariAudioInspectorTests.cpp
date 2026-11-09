// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#include "../Source/Audio/ResampledLoopAudition.h"
#include "../Source/Models/ProjectSerializer.h"
#include "../Source/UI/PianoRollComponent.h"
#include "../Source/UI/PianoRoll/PitchDrawingConfirmationPanel.h"
#include "../Source/UI/VariAudioInspector.h"
#include "../Source/Undo/PitchUndoManager.h"
#include "../Source/Utils/PitchMacroScope.h"
#include "../Source/Utils/HarmonyUtils.h"
#include "../Source/Utils/ScaleUtils.h"
#include "TestAssert.h"
#include <cmath>
#include <iostream>

// Inspector sliders, double-click reset, Option zoom, drag-audition buffer,
// selection-aware pitch macro and the editor scale menus (docs/design.md 5.6).
namespace
{
using Param = PianoRollComponent::InspectorParam;

void prepareAudio(Project& project, int frames = 600)
{
    auto& audio = project.getAudioData();
    audio.sampleRate = SAMPLE_RATE;
    audio.f0.assign(static_cast<size_t>(frames), 261.6f);
    audio.basePitch.assign(static_cast<size_t>(frames), 60.0f);
    audio.deltaPitch.assign(static_cast<size_t>(frames), 0.0f);
}

Note makeNote(int start, int end, float midi)
{
    Note note(start, end, midi);
    note.setSrcStartFrame(start);
    note.setSrcEndFrame(end);
    note.setOriginalMidiNote(midi);
    note.setDeltaPitch(std::vector<float>(static_cast<size_t>(end - start), 0.0f));
    return note;
}

bool near(float a, float b, float tolerance = 1.0e-3f)
{
    return std::abs(a - b) <= tolerance;
}

// The inspector's menus are private; find them the way a user sees them.
template <typename T>
std::vector<T*> findChildren(juce::Component& root)
{
    std::vector<T*> found;
    std::function<void(juce::Component&)> walk = [&](juce::Component& c)
    {
        for (auto* child : c.getChildren())
        {
            if (auto* typed = dynamic_cast<T*>(child))
                found.push_back(typed);
            walk(*child);
        }
    };
    walk(root);
    return found;
}

struct Fixture
{
    Project project;
    PianoRollComponent roll;
    PitchUndoManager history;
    Fixture()
    {
        prepareAudio(project);
        project.addNote(makeNote(0, 100, 60.3f));   // 30 cents sharp
        project.addNote(makeNote(100, 200, 61.8f)); // 20 cents flat of D
        project.addNote(makeNote(250, 300, 64.0f));
        roll.setSize(900, 420);
        roll.setProject(&project);
        roll.setUndoManager(&history);
        roll.resized();
    }
    Note& note(size_t i) { return project.getNotes()[i]; }
};
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    // --- Targets: the selection, otherwise every segment -------------------
    {
        Fixture f;
        CHECK(f.roll.getInspectorTargets().size() == 3);
        f.note(1).setSelected(true);
        CHECK(f.roll.getInspectorTargets().size() == 1);
        CHECK(f.roll.getInspectorTargets().front() == &f.note(1));
    }

    // --- Correct Pitch: proportional pull toward the semitone, one undo ----
    {
        Fixture f;
        f.note(0).setSelected(true);
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 0.0f));
        f.roll.beginInspectorEdit(Param::CorrectPitch);
        f.roll.setInspectorEditValue(50.0f);
        CHECK(near(f.note(0).getMidiNote(), 60.15f));
        f.roll.setInspectorEditValue(100.0f);
        CHECK(near(f.note(0).getMidiNote(), 60.0f));
        f.roll.endInspectorEdit();
        CHECK(near(f.note(1).getMidiNote(), 61.8f)); // not selected: untouched
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 100.0f));
        // A later gesture measures from the sung pitch, not the corrected one.
        f.roll.beginInspectorEdit(Param::CorrectPitch);
        f.roll.setInspectorEditValue(0.0f);
        f.roll.endInspectorEdit();
        CHECK(near(f.note(0).getMidiNote(), 60.3f));
        f.history.undo();
        CHECK(near(f.note(0).getMidiNote(), 60.0f));
        f.history.undo();
        CHECK(near(f.note(0).getMidiNote(), 60.3f));
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 60.3f));
        CHECK(!f.history.canUndo());
    }

    // --- Flatten, drift, formant: one value for all targets, undoable ------
    {
        Fixture f;
        f.roll.beginInspectorEdit(Param::FlattenCurve);
        f.roll.setInspectorEditValue(75.0f);
        f.roll.endInspectorEdit();
        for (size_t i = 0; i < 3; ++i)
            CHECK(near(f.note(i).getVibrato(), 0.25f));
        CHECK(near(*f.roll.getInspectorValue(Param::FlattenCurve), 75.0f));

        f.roll.beginInspectorEdit(Param::PitchDrift);
        f.roll.setInspectorEditValue(40.0f);
        f.roll.endInspectorEdit();
        CHECK(near(f.note(2).getPitchDrift(), 0.6f));

        f.roll.beginInspectorEdit(Param::Formant);
        f.roll.setInspectorEditValue(-2.5f);
        f.roll.endInspectorEdit();
        CHECK(near(f.note(1).getFormantShift(), -2.5f));

        f.history.undo();
        f.history.undo();
        f.history.undo();
        for (size_t i = 0; i < 3; ++i)
        {
            CHECK(near(f.note(i).getVibrato(), 1.0f));
            CHECK(near(f.note(i).getPitchDrift(), 1.0f));
            CHECK(near(f.note(i).getFormantShift(), 0.0f));
        }
    }

    // --- Volume goes through the gain-only action --------------------------
    {
        Fixture f;
        f.note(2).setSelected(true);
        f.roll.beginInspectorEdit(Param::Volume);
        f.roll.setInspectorEditValue(-6.0f);
        f.roll.endInspectorEdit();
        CHECK(near(f.note(2).getVolumeDb(), -6.0f));
        CHECK(near(f.note(0).getVolumeDb(), 0.0f));
        f.history.undo();
        CHECK(near(f.note(2).getVolumeDb(), 0.0f));
    }

    // --- A gesture that changes nothing leaves no undo step -----------------
    {
        Fixture f;
        f.roll.beginInspectorEdit(Param::Formant);
        f.roll.setInspectorEditValue(0.0f);
        f.roll.endInspectorEdit();
        CHECK(!f.history.canUndo());
    }

    auto source = juce::Desktop::getInstance().getMainMouseSource();
    const auto event = [&source](juce::Component& c, juce::Point<float> at,
                                 juce::ModifierKeys mods, int clicks)
    {
        return juce::MouseEvent(source, at, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                                juce::Time::getCurrentTime(), at,
                                juce::Time::getCurrentTime(), clicks, false);
    };

    // --- Double-click a segment: back to the analysed state ----------------
    {
        Fixture f;
        f.note(1).setMidiNote(63.0f);
        f.note(1).setVibrato(0.2f);
        f.note(1).setFormantShift(3.0f);
        f.roll.fitPitchRangeToView(58.0f, 66.0f);
        const float x = PianoRollComponent::pianoKeysWidth +
                        static_cast<float>(framesToSeconds(150)) * f.roll.getPixelsPerSecond() -
                        static_cast<float>(f.roll.getScrollX());
        const float y = PianoRollComponent::headerHeight +
                        (MAX_MIDI_NOTE - 63.0f) * f.roll.getPixelsPerSemitone() +
                        f.roll.getPixelsPerSemitone() * 0.5f -
                        static_cast<float>(f.roll.getScrollY());
        f.roll.mouseDoubleClick(event(f.roll, {x, y}, juce::ModifierKeys::leftButtonModifier, 2));
        CHECK(near(f.note(1).getMidiNote(), 61.8f));
        CHECK(near(f.note(1).getVibrato(), 1.0f));
        CHECK(near(f.note(1).getFormantShift(), 0.0f));
        CHECK(f.history.canUndo());
        f.history.undo();
        CHECK(near(f.note(1).getMidiNote(), 63.0f));
    }

    // --- Delete Segments: gone from the editor, edits reset, one undo -----
    {
        Fixture f;
        auto& n = f.note(1);
        n.setMidiNote(63.0f);
        n.setFormantShift(3.0f);
        n.setVolumeDb(-6.0f);
        n.setStartFrame(90); // timing edit
        n.setSelected(true);
        CHECK(f.roll.deleteSelectedSegments());
        CHECK(n.isDeleted());
        CHECK(!n.isSelected());
        CHECK(near(n.getMidiNote(), 61.8f)); // analysed pitch again
        CHECK(near(n.getFormantShift(), 0.0f));
        CHECK(near(n.getVolumeDb(), 0.0f));
        CHECK(n.getStartFrame() == 100); // timing restored too
        CHECK(n.isNeutralForOriginalWaveform());
        CHECK(f.roll.getInspectorTargets().size() == 2); // no longer a target
        CHECK(f.history.canUndo());
        f.history.undo();
        CHECK(!n.isDeleted());
        CHECK(near(n.getMidiNote(), 63.0f));
        CHECK(near(n.getFormantShift(), 3.0f));
        CHECK(near(n.getVolumeDb(), -6.0f));
        CHECK(n.getStartFrame() == 90);
        f.history.redo();
        CHECK(n.isDeleted());
        CHECK(near(n.getMidiNote(), 61.8f));
    }

    // --- Delete Segments: Backspace key, and a no-op with no selection -----
    {
        Fixture f;
        CHECK(!f.roll.deleteSelectedSegments());
        CHECK(!f.history.canUndo());
        f.note(0).setSelected(true);
        CHECK(f.roll.keyPressed(juce::KeyPress(juce::KeyPress::backspaceKey)));
        CHECK(f.note(0).isDeleted());
        CHECK(f.history.canUndo());
    }

    // --- Delete Segments: the flag survives a project save/load ------------
    {
        Fixture f;
        f.note(2).setSelected(true);
        CHECK(f.roll.deleteSelectedSegments());
        Project restored;
        CHECK(ProjectSerializer::fromJson(restored,
                                          ProjectSerializer::toJson(f.project)));
        CHECK(restored.getNotes().size() == 3);
        CHECK(restored.getNotes()[2].isDeleted());
        CHECK(!restored.getNotes()[0].isDeleted());
    }

    // --- Ctrl+click toggles a segment; Shift+Ctrl+click seeks instead -----
    {
        Fixture f;
        f.roll.fitPitchRangeToView(58.0f, 66.0f);
        const float x = PianoRollComponent::pianoKeysWidth +
                        static_cast<float>(framesToSeconds(150)) * f.roll.getPixelsPerSecond() -
                        static_cast<float>(f.roll.getScrollX());
        const float y = PianoRollComponent::headerHeight +
                        (MAX_MIDI_NOTE - 61.8f) * f.roll.getPixelsPerSemitone() +
                        f.roll.getPixelsPerSemitone() * 0.5f -
                        static_cast<float>(f.roll.getScrollY());
        // The command modifier is Ctrl on Windows, where this toggle lives;
        // a literal Ctrl+click would read as a popup-menu click on macOS.
        const auto ctrlClick = juce::ModifierKeys(
            juce::ModifierKeys::commandModifier |
            juce::ModifierKeys::leftButtonModifier);
        f.roll.mouseDown(event(f.roll, {x, y}, ctrlClick, 1));
        CHECK(f.note(1).isSelected()); // Ctrl+click adds it
        f.roll.mouseUp(event(f.roll, {x, y}, ctrlClick, 1));
        f.roll.mouseDown(event(f.roll, {x, y}, ctrlClick, 1));
        CHECK(!f.note(1).isSelected()); // and takes it away again
        f.roll.mouseUp(event(f.roll, {x, y}, ctrlClick, 1));

        double seekedTo = -1.0;
        f.roll.onSeek = [&seekedTo](double time) { seekedTo = time; };
        const auto shiftCtrlClick = juce::ModifierKeys(
            juce::ModifierKeys::commandModifier |
            juce::ModifierKeys::shiftModifier |
            juce::ModifierKeys::leftButtonModifier);
        f.roll.mouseDown(event(f.roll, {x, y}, shiftCtrlClick, 1));
        CHECK(seekedTo >= 0.0);          // the playhead moved...
        CHECK(!f.note(1).isSelected());  // ...and the click was not a toggle
        f.roll.mouseUp(event(f.roll, {x, y}, shiftCtrlClick, 1));
    }

    // --- Batch 2 (B12): Absolute snap is semitones only, even in Scale mode -
    {
        Fixture f;
        f.project.setProjectKey(0, ScaleMode::Major); // C major
        f.roll.setDragSnapMode(DragSnapMode::Scale);
        f.note(0).setSelected(true); // sung at 60.3
        // Arrow up (Absolute): the next semitone is C# (61), not the next
        // C-major scale tone D (62).
        CHECK(f.roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
        CHECK(near(f.note(0).getMidiNote(), 61.0f));
        f.history.undo();
        CHECK(near(f.note(0).getMidiNote(), 60.3f));

        // An Absolute drag also lands on the semitone grid: the raw target
        // 61.1 becomes C# (61), not the nearest scale tone D (62).
        f.roll.fitPitchRangeToView(56.0f, 68.0f);
        const auto held = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        const auto xAt = [&f](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * f.roll.getPixelsPerSecond() -
                   static_cast<float>(f.roll.getScrollX());
        };
        const auto yAt = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(f.roll.getScrollY());
        };
        const juce::Point<float> start(xAt(50), yAt(60.3f));
        const auto end = start + juce::Point<float>(0.0f, -0.8f * f.roll.getPixelsPerSemitone());
        f.roll.mouseDown(event(f.roll, start, held, 1));
        f.roll.mouseDrag(event(f.roll, start + juce::Point<float>(0.0f, -3.0f), held, 1));
        f.roll.mouseDrag(event(f.roll, end, held, 1));
        f.roll.mouseUp(event(f.roll, end, held, 1));
        CHECK(near(f.note(0).getMidiNote(), 61.0f));
    }

    // --- Batch 2: Ctrl held during a segment drag = fine adjust (no snap)
    {
        Fixture f;
        f.roll.fitPitchRangeToView(56.0f, 68.0f);
        const auto held = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        const auto ctrlHeld = juce::ModifierKeys(
            juce::ModifierKeys::leftButtonModifier |
            juce::ModifierKeys::ctrlModifier);
        const auto xAt = [&f](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * f.roll.getPixelsPerSecond() -
                   static_cast<float>(f.roll.getScrollX());
        };
        const auto yAt = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(f.roll.getScrollY());
        };
        const juce::Point<float> start(xAt(50), yAt(60.3f));
        // A third-of-a-semitone drag: snapped, it would land on 61.
        const auto end = start + juce::Point<float>(0.0f, -0.35f * f.roll.getPixelsPerSemitone());

        // With Ctrl held for the drag, the pitch follows the pointer
        // continuously: 60.3 + 0.35, not the semitone 61.
        f.roll.mouseDown(event(f.roll, start, held, 1));
        f.roll.mouseDrag(event(f.roll, start + juce::Point<float>(0.0f, -3.0f), ctrlHeld, 1));
        f.roll.mouseDrag(event(f.roll, end, ctrlHeld, 1));
        f.roll.mouseUp(event(f.roll, end, ctrlHeld, 1));
        CHECK(near(f.note(0).getMidiNote(), 60.65f, 0.03f));
        f.history.undo();
        CHECK(near(f.note(0).getMidiNote(), 60.3f));

        // Releasing Ctrl mid-drag re-engages the snap for the same total
        // offset: the segment lands on the semitone after all.
        f.roll.mouseDown(event(f.roll, start, held, 1));
        f.roll.mouseDrag(event(f.roll, end, ctrlHeld, 1));
        f.roll.mouseDrag(event(f.roll, end, held, 1));
        f.roll.mouseUp(event(f.roll, end, held, 1));
        CHECK(near(f.note(0).getMidiNote(), 61.0f));
    }

    // --- Option/Alt: wheel zooms vertically; drag zooms to a rectangle ----
    // (Batch 2, B2: Alt-drag used to zoom vertically only; it now zooms the
    // view to the dragged rectangle, and an Alt-click zooms out.)
    {
        Fixture f;
        f.roll.fitPitchRangeToView(50.0f, 70.0f);
        const float ppsBefore = f.roll.getPixelsPerSecond();
        const float semitoneBefore = f.roll.getPixelsPerSemitone();
        const juce::Point<float> at(400.0f, 200.0f);
        juce::MouseWheelDetails wheel{};
        wheel.deltaY = 0.5f;
        f.roll.mouseWheelMove(event(f.roll, at, juce::ModifierKeys::altModifier, 0), wheel);
        CHECK(f.roll.getPixelsPerSemitone() > semitoneBefore);
        CHECK(near(f.roll.getPixelsPerSecond(), ppsBefore));

        const float semitoneMid = f.roll.getPixelsPerSemitone();
        const auto altLeft = juce::ModifierKeys(juce::ModifierKeys::altModifier |
                                                juce::ModifierKeys::leftButtonModifier);
        // Start the drag on empty canvas (no segment there).
        const juce::Point<float> start(860.0f, 120.0f);
        f.roll.mouseDown(event(f.roll, start, altLeft, 1));
        f.roll.mouseDrag(event(f.roll, start + juce::Point<float>(40.0f, -60.0f), altLeft, 1));
        f.roll.mouseUp(event(f.roll, start + juce::Point<float>(40.0f, -60.0f), altLeft, 1));
        CHECK(f.roll.getPixelsPerSemitone() > semitoneMid); // rectangle height zooms in
        CHECK(f.roll.getPixelsPerSecond() > ppsBefore);    // rectangle width zooms in too

        // An Alt-click (no drag) zooms back out on both axes.
        const float ppsIn = f.roll.getPixelsPerSecond();
        const float semitoneIn = f.roll.getPixelsPerSemitone();
        f.roll.mouseDown(event(f.roll, start, altLeft, 1));
        f.roll.mouseUp(event(f.roll, start, altLeft, 1));
        CHECK(f.roll.getPixelsPerSecond() < ppsIn);
        CHECK(f.roll.getPixelsPerSemitone() < semitoneIn);
    }

    // --- Batch 2 (U1): middle-button drag pans; it no longer scrubs -------
    {
        Fixture f;
        // The timeline length comes from the waveform; give the project one
        // so horizontal scrolling has room to move.
        f.project.getAudioData().waveform.setSize(1, 600 * 512);
        f.roll.setPixelsPerSecond(200.0f);
        f.roll.setScrollX(100.0);
        bool seeked = false;
        f.roll.onSeek = [&seeked](double) { seeked = true; };
        const auto middle = juce::ModifierKeys(juce::ModifierKeys::middleButtonModifier);
        const juce::Point<float> start(400.0f, 200.0f);
        f.roll.mouseDown(event(f.roll, start, middle, 1));
        f.roll.mouseDrag(event(f.roll, start + juce::Point<float>(-50.0f, 0.0f), middle, 1));
        f.roll.mouseUp(event(f.roll, start + juce::Point<float>(-50.0f, 0.0f), middle, 1));
        CHECK(near(static_cast<float>(f.roll.getScrollX()), 150.0f, 0.5f));
        CHECK(!seeked); // the playhead did not move: pan, not scrub
    }

    // --- Batch 2 (U4): a plain drag on empty canvas is a box selection ----
    {
        Fixture f;
        f.roll.fitPitchRangeToView(56.0f, 68.0f);
        const auto held = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        const auto xAt = [&f](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * f.roll.getPixelsPerSecond() -
                   static_cast<float>(f.roll.getScrollX());
        };
        const auto yAt = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(f.roll.getScrollY());
        };
        const double scrollBefore = f.roll.getScrollX();
        // Drag a box around the first two segments: both become selected,
        // and the view does not pan (it used to pan from empty canvas).
        const juce::Point<float> start(xAt(90), yAt(62.5f));
        const juce::Point<float> end(xAt(210), yAt(58.5f));
        f.roll.mouseDown(event(f.roll, start, held, 1));
        f.roll.mouseDrag(event(f.roll, end, held, 1));
        f.roll.mouseUp(event(f.roll, end, held, 1));
        CHECK(f.note(0).isSelected());
        CHECK(f.note(1).isSelected());
        CHECK(!f.note(2).isSelected());
        CHECK(near(static_cast<float>(f.roll.getScrollX()),
                   static_cast<float>(scrollBefore), 0.5f));
    }

    // --- Batch 2 (B8): the split smart control divides the segment --------
    {
        Fixture f;
        f.roll.fitPitchRangeToView(58.0f, 66.0f);
        const auto xAt = [&f](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * f.roll.getPixelsPerSecond() -
                   static_cast<float>(f.roll.getScrollX());
        };
        const auto yAt = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(f.roll.getScrollY());
        };
        // Hover the second segment so its smart controls appear.
        f.roll.mouseMove(event(f.roll, {xAt(150), yAt(61.8f)}, juce::ModifierKeys(), 0));
        // The split control sits inside the bottom-centre edge of the note:
        // its centre is the note's bottom minus half the control plus 1px.
        const float splitY = yAt(61.8f) + f.roll.getPixelsPerSemitone() * 0.5f -
                             (PitchToolHandles::controlSize * 0.5f + 1.0f);
        const auto left = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        f.roll.mouseDown(event(f.roll, {xAt(150), splitY}, left, 1));
        f.roll.mouseUp(event(f.roll, {xAt(150), splitY}, left, 1));
        CHECK(f.project.getNotes().size() == 4);
        CHECK(std::abs(f.note(1).getEndFrame() - 150) <= 1); // at the click
        CHECK(f.note(2).getStartFrame() == f.note(1).getEndFrame());
        f.history.undo();
        CHECK(f.project.getNotes().size() == 3);
        CHECK(f.note(1).getEndFrame() == 200);
    }

    // --- Batch 2 (B5/B6): tilt anchor and Alt+tilt rotation -----------------
    {
        Fixture f;
        f.roll.fitPitchRangeToView(58.0f, 66.0f);
        f.roll.setShowAllSmartControls(true);
        const auto xAt = [&f](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * f.roll.getPixelsPerSecond() -
                   static_cast<float>(f.roll.getScrollX());
        };
        const auto yAt = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(f.roll.getScrollY());
        };
        const float ppS = f.roll.getPixelsPerSemitone();
        const float s = PitchToolHandles::controlSize;
        f.roll.mouseMove(event(f.roll, {xAt(150), yAt(61.8f)}, juce::ModifierKeys(), 0));

        // B6: Alt+drag the TiltRight handle up one semitone rotates the
        // contour around the centred anchor: right edge +1, left edge -1.
        const auto altLeft = juce::ModifierKeys(juce::ModifierKeys::altModifier |
                                                juce::ModifierKeys::leftButtonModifier);
        const juce::Point<float> tiltRightAt(xAt(200) - s * 0.5f,
                                             yAt(61.8f) - ppS * 0.5f - (s * 0.5f + 3.0f));
        const auto tiltEnd = tiltRightAt + juce::Point<float>(0.0f, -ppS);
        f.roll.mouseDown(event(f.roll, tiltRightAt, altLeft, 1));
        f.roll.mouseDrag(event(f.roll, tiltEnd, altLeft, 1));
        f.roll.mouseUp(event(f.roll, tiltEnd, altLeft, 1));
        CHECK(near(f.note(1).getTiltRight(), 1.0f, 0.05f));
        CHECK(near(f.note(1).getTiltLeft(), -1.0f, 0.05f));
        f.history.undo();
        CHECK(near(f.note(1).getTiltRight(), 0.0f));
        CHECK(near(f.note(1).getTiltLeft(), 0.0f));

        // B5: dragging the anchor diamond a quarter of the segment width
        // moves the pivot to 0.75 and changes no audio parameter.
        const auto left = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        const juce::Point<float> anchorAt(xAt(150), yAt(61.8f) - ppS * 0.5f);
        const float quarterWidth = (xAt(200) - xAt(100)) * 0.25f;
        const auto anchorEnd = anchorAt + juce::Point<float>(quarterWidth, 0.0f);
        // Handles are hidden during a drag and rebuilt on hover, so move
        // onto the control first, as a user would.
        f.roll.mouseMove(event(f.roll, anchorAt, juce::ModifierKeys(), 0));
        f.roll.mouseDown(event(f.roll, anchorAt, left, 1));
        f.roll.mouseDrag(event(f.roll, anchorEnd, left, 1));
        f.roll.mouseUp(event(f.roll, anchorEnd, left, 1));
        CHECK(near(f.note(1).getTiltAnchor(), 0.75f, 0.03f));
        CHECK(near(f.note(1).getTiltLeft(), 0.0f));
        CHECK(near(f.note(1).getTiltRight(), 0.0f));
        f.history.undo();
        CHECK(near(f.note(1).getTiltAnchor(), 0.5f));
    }

    // --- Batch 2 (B7): straighten-range boundaries -------------------------
    {
        Fixture f;
        f.roll.fitPitchRangeToView(58.0f, 66.0f);
        f.roll.setShowAllSmartControls(true);
        const auto xAt = [&f](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * f.roll.getPixelsPerSecond() -
                   static_cast<float>(f.roll.getScrollX());
        };
        const auto yAt = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(f.roll.getScrollY());
        };
        const float ppS = f.roll.getPixelsPerSemitone();
        const float s = PitchToolHandles::controlSize;
        const auto left = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        f.roll.mouseMove(event(f.roll, {xAt(150), yAt(61.8f)}, juce::ModifierKeys(), 0));

        // Dragging the left straighten-range boundary right by half the
        // segment moves the range start to 0.5; the end stays at 1.
        const juce::Point<float> rangeLeftAt(xAt(100),
                                             yAt(61.8f) - ppS * 0.5f + (s * 0.5f + 2.0f));
        const auto rangeEnd = rangeLeftAt + juce::Point<float>((xAt(200) - xAt(100)) * 0.5f, 0.0f);
        f.roll.mouseMove(event(f.roll, rangeLeftAt, juce::ModifierKeys(), 0));
        f.roll.mouseDown(event(f.roll, rangeLeftAt, left, 1));
        f.roll.mouseDrag(event(f.roll, rangeEnd, left, 1));
        f.roll.mouseUp(event(f.roll, rangeEnd, left, 1));
        CHECK(near(f.note(1).getStraightenRangeStart(), 0.5f, 0.03f));
        CHECK(near(f.note(1).getStraightenRangeEnd(), 1.0f));
        f.history.undo();
        CHECK(near(f.note(1).getStraightenRangeStart(), 0.0f));
    }

    // --- Batch 2 (B4): Alt+drag on a segment edge moves the boundary ------
    {
        Fixture f;
        f.roll.fitPitchRangeToView(58.0f, 66.0f);
        const auto xAt = [&f](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * f.roll.getPixelsPerSecond() -
                   static_cast<float>(f.roll.getScrollX());
        };
        const auto yAt = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(f.roll.getScrollY());
        };
        const auto altLeft = juce::ModifierKeys(juce::ModifierKeys::altModifier |
                                                juce::ModifierKeys::leftButtonModifier);
        // The shared edge between segments 0 and 1 (frame 100): dragging it
        // right by 20 frames moves both sides, output and source alike -
        // frames are reassigned, never stretched.
        const juce::Point<float> edgeAt(xAt(100) + 2.0f, yAt(61.8f));
        const auto edgeEnd = edgeAt + juce::Point<float>(xAt(120) - xAt(100), 0.0f);
        f.roll.mouseDown(event(f.roll, edgeAt, altLeft, 1));
        f.roll.mouseDrag(event(f.roll, edgeEnd, altLeft, 1));
        f.roll.mouseUp(event(f.roll, edgeEnd, altLeft, 1));
        CHECK(f.note(0).getEndFrame() == 120);
        CHECK(f.note(0).getSrcEndFrame() == 120);
        CHECK(f.note(1).getStartFrame() == 120);
        CHECK(f.note(1).getSrcStartFrame() == 120);
        f.history.undo();
        CHECK(f.note(0).getEndFrame() == 100);
        CHECK(f.note(1).getStartFrame() == 100);

        // Frames 200-250 belong to no segment (a gap). Segment 2's start
        // edge can move left into the gap but not into segment 1, and the
        // neighbour does not move.
        const juce::Point<float> gapEdgeAt(xAt(250) + 2.0f, yAt(64.0f));
        const auto gapEnd = gapEdgeAt + juce::Point<float>(xAt(160) - xAt(250), 0.0f);
        f.roll.mouseDown(event(f.roll, gapEdgeAt, altLeft, 1));
        f.roll.mouseDrag(event(f.roll, gapEnd, altLeft, 1));
        f.roll.mouseUp(event(f.roll, gapEnd, altLeft, 1));
        CHECK(f.note(2).getStartFrame() == 200); // clamped at segment 1's end
        CHECK(f.note(2).getSrcStartFrame() == 200);
        CHECK(f.note(1).getEndFrame() == 200);
    }

    // --- Batch 2 (U6): freehand pitch drawing - the stroke is the curve ---
    {
        // The drawing machinery works on the analysed curve (basePitch +
        // deltaPitch) and the mel frame count, so give these fixtures the
        // self-consistent analysis a real project would have: basePitch
        // follows each segment's sung pitch and the mel grid exists.
        const auto makeConsistent = [](Fixture& fx)
        {
            auto& audio = fx.project.getAudioData();
            audio.melSpectrogram.assign(600, std::vector<float>(1, 0.0f));
            for (auto& n : fx.project.getNotes())
                for (int fr = n.getStartFrame(); fr < n.getEndFrame(); ++fr)
                    audio.basePitch[static_cast<size_t>(fr)] = n.getMidiNote();
        };
        Fixture f;
        makeConsistent(f);
        f.roll.fitPitchRangeToView(58.0f, 66.0f);
        f.roll.setEditMode(EditMode::PitchDrawing);
        const auto xAt = [&f](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * f.roll.getPixelsPerSecond() -
                   static_cast<float>(f.roll.getScrollX());
        };
        const auto yAt = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(f.roll.getScrollY());
        };
        const auto left = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        // A flat stroke two semitones above segment 1's sung pitch,
        // covering nearly the whole segment (the preview re-centres the
        // segment on the mean of its resulting curve).
        f.roll.mouseDown(event(f.roll, {xAt(102), yAt(63.8f)}, left, 1));
        f.roll.mouseDrag(event(f.roll, {xAt(150), yAt(63.8f)}, left, 1));
        f.roll.mouseDrag(event(f.roll, {xAt(198), yAt(63.8f)}, left, 1));
        f.roll.mouseUp(event(f.roll, {xAt(198), yAt(63.8f)}, left, 1));
        CHECK(near(f.note(1).getMidiNote(), 63.8f, 0.15f)); // the segment followed the line
        CHECK(f.note(1).hasBakedDeltaPitch());              // on the baked-delta model
        CHECK(near(f.note(0).getMidiNote(), 60.3f));        // neighbours untouched

        // Discarding restores the analysed state exactly.
        f.roll.cancelPitchDrawingPreview();
        CHECK(near(f.note(1).getMidiNote(), 61.8f));
        CHECK(!f.note(1).hasBakedDeltaPitch());

        // Draw again and confirm: one undo step restores the original.
        f.roll.mouseDown(event(f.roll, {xAt(102), yAt(63.8f)}, left, 1));
        f.roll.mouseDrag(event(f.roll, {xAt(198), yAt(63.8f)}, left, 1));
        f.roll.mouseUp(event(f.roll, {xAt(198), yAt(63.8f)}, left, 1));
        auto* panel = findChildren<PitchDrawingConfirmationPanel>(f.roll).front();
        CHECK(panel != nullptr && panel->onApply != nullptr);
        panel->onApply();
        CHECK(near(f.note(1).getMidiNote(), 63.8f, 0.15f));
        CHECK(f.history.canUndo());
        f.history.undo();
        CHECK(near(f.note(1).getMidiNote(), 61.8f));
        CHECK(!f.note(1).hasBakedDeltaPitch());
        f.history.redo();
        CHECK(near(f.note(1).getMidiNote(), 63.8f, 0.15f));

        // A stroke that crosses the gap (frames 200-250 own no segment)
        // breaks there: it still reaches segment 2, but segment 0 and the
        // already-drawn segment 1 keep their curves.
        Fixture g;
        makeConsistent(g);
        g.roll.fitPitchRangeToView(58.0f, 66.0f);
        g.roll.setEditMode(EditMode::PitchDrawing);
        const auto gxAt = [&g](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * g.roll.getPixelsPerSecond() -
                   static_cast<float>(g.roll.getScrollX());
        };
        const auto gyAt = [&g](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * g.roll.getPixelsPerSemitone() +
                   g.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(g.roll.getScrollY());
        };
        g.roll.mouseDown(event(g.roll, {gxAt(190), gyAt(63.8f)}, left, 1));
        g.roll.mouseDrag(event(g.roll, {gxAt(225), gyAt(65.0f)}, left, 1)); // over the gap
        g.roll.mouseDrag(event(g.roll, {gxAt(260), gyAt(66.0f)}, left, 1));
        g.roll.mouseUp(event(g.roll, {gxAt(260), gyAt(66.0f)}, left, 1));
        CHECK(g.note(2).getMidiNote() > 64.2f);   // pulled toward the line
        CHECK(g.note(2).getMidiNote() < 64.9f);   // only its head was drawn
        CHECK(near(g.note(0).getMidiNote(), 60.3f));

        // A stroke that starts in the gap starts nothing.
        Fixture h;
        makeConsistent(h);
        h.roll.fitPitchRangeToView(58.0f, 66.0f);
        h.roll.setEditMode(EditMode::PitchDrawing);
        const auto hxAt = [&h](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * h.roll.getPixelsPerSecond() -
                   static_cast<float>(h.roll.getScrollX());
        };
        const auto hyAt = [&h](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * h.roll.getPixelsPerSemitone() +
                   h.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(h.roll.getScrollY());
        };
        h.roll.mouseDown(event(h.roll, {hxAt(225), hyAt(64.0f)}, left, 1));
        h.roll.mouseDrag(event(h.roll, {hxAt(260), hyAt(66.0f)}, left, 1));
        h.roll.mouseUp(event(h.roll, {hxAt(260), hyAt(66.0f)}, left, 1));
        CHECK(near(h.note(2).getMidiNote(), 64.0f));
        CHECK(!h.note(2).hasBakedDeltaPitch());

        // A deleted segment is outside editing: a stroke that starts on
        // it starts nothing, and a stroke that crosses it breaks there
        // exactly as it breaks across the gap - the segment keeps its
        // analysed pitch and gains no baked delta.
        Fixture d;
        makeConsistent(d);
        d.note(1).setDeleted(true);
        d.roll.fitPitchRangeToView(58.0f, 66.0f);
        d.roll.setEditMode(EditMode::PitchDrawing);
        const auto dxAt = [&d](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * d.roll.getPixelsPerSecond() -
                   static_cast<float>(d.roll.getScrollX());
        };
        const auto dyAt = [&d](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * d.roll.getPixelsPerSemitone() +
                   d.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(d.roll.getScrollY());
        };
        d.roll.mouseDown(event(d.roll, {dxAt(150), dyAt(63.8f)}, left, 1));
        d.roll.mouseDrag(event(d.roll, {dxAt(260), dyAt(66.0f)}, left, 1));
        d.roll.mouseUp(event(d.roll, {dxAt(260), dyAt(66.0f)}, left, 1));
        CHECK(near(d.note(1).getMidiNote(), 61.8f));
        CHECK(!d.note(1).hasBakedDeltaPitch());
        CHECK(near(d.note(2).getMidiNote(), 64.0f));
        CHECK(!d.note(2).hasBakedDeltaPitch());
        // Crossing from segment 0 into the deleted segment draws
        // segment 0 only.
        d.roll.mouseDown(event(d.roll, {dxAt(50), dyAt(62.3f)}, left, 1));
        d.roll.mouseDrag(event(d.roll, {dxAt(150), dyAt(62.3f)}, left, 1));
        d.roll.mouseUp(event(d.roll, {dxAt(150), dyAt(62.3f)}, left, 1));
        CHECK(d.note(0).hasBakedDeltaPitch());
        CHECK(near(d.note(1).getMidiNote(), 61.8f));
        CHECK(!d.note(1).hasBakedDeltaPitch());
    }

    // --- Drag feedback: the whole segment, then a short silent gap ---------
    {
        std::vector<float> sourceAudio(22050);
        for (size_t i = 0; i < sourceAudio.size(); ++i)
            sourceAudio[i] = 0.5f * std::sin(0.06f * static_cast<float>(i));
        juce::AudioBuffer<float> received;
        bool done = false;
        ResampledLoopAudition audition([&](juce::AudioBuffer<float> buffer)
                                       { received = std::move(buffer); done = true; });
        audition.request(sourceAudio, 12.0f, 72.0f, true); // an octave up: half length
        for (int i = 0; i < 100 && !done; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        CHECK(done);
        const int padding = static_cast<int>(ResampledLoopAudition::loopGapSeconds * 44100.0);
        CHECK(received.getNumSamples() == 11025 + padding);
        float tailPeak = 0.0f;
        for (int i = 11025; i < received.getNumSamples(); ++i)
            tailPeak = std::max(tailPeak, std::abs(received.getSample(0, i)));
        CHECK(tailPeak == 0.0f);
        CHECK(received.getMagnitude(0, 1000, 9000) > 0.1f); // sound in the body
    }

    // --- Pitch macro scope honours a selection in track mode ---------------
    {
        Project a, b;
        prepareAudio(a);
        prepareAudio(b);
        a.addNote(makeNote(0, 100, 60.0f));
        b.addNote(makeNote(0, 100, 62.0f));
        b.addNote(makeNote(100, 200, 64.0f));
        b.getNotes()[1].setSelected(true);
        const std::vector<juce::Range<int>> window{{0, 600}};
        bool onlySelected = hasSelectedPitchMacroNotes(a, true, window) ||
                            hasSelectedPitchMacroNotes(b, true, window);
        CHECK(onlySelected);
        CHECK(collectPitchMacroTargetNotes(a, true, window, onlySelected).empty());
        const auto bTargets = collectPitchMacroTargetNotes(b, true, window, onlySelected);
        CHECK(bTargets.size() == 1 && bTargets.front() == &b.getNotes()[1]);
        CHECK(collectPitchMacroTargetNotes(b, true, window, false).size() == 2);
    }

    // --- Editor scale menus: region key, project key, track view ----------
    {
        Project project;
        prepareAudio(project);
        project.setProjectKey(0, ScaleMode::Major); // C major
        CHECK(project.getUseProjectKey());
        bool trackView = false;
        int changes = 0;
        VariAudioInspector inspector;
        inspector.getCurrentProject = [&project] { return &project; };
        inspector.isProjectKeyScope = [&trackView] { return trackView; };
        inspector.onEditorScaleChanged = [&changes] { ++changes; };
        inspector.setSize(VariAudioInspector::preferredWidth, 560);
        inspector.refresh();

        juce::ComboBox* rootBox = nullptr;
        juce::ComboBox* modeBox = nullptr;
        for (auto* box : findChildren<juce::ComboBox>(inspector))
        {
            if (box->getNumItems() == 12 && box->getItemText(0) == "C")
                rootBox = box;
            else if (box->getNumItems() == 14)
                modeBox = box;
        }
        juce::ToggleButton* useProjectKey = nullptr;
        for (auto* toggle : findChildren<juce::ToggleButton>(inspector))
            if (toggle->getButtonText() == TR("scale.use_project_key"))
                useProjectKey = toggle;
        CHECK(rootBox != nullptr && modeBox != nullptr && useProjectKey != nullptr);
        CHECK(rootBox->getSelectedId() == 1 && modeBox->getSelectedId() == 1);
        CHECK(useProjectKey->isVisible() && useProjectKey->getToggleState());

        // Region view: picking E gives this region its own key (E major) and
        // leaves the project key alone.
        rootBox->setSelectedId(5, juce::sendNotificationSync);
        CHECK(changes == 1);
        CHECK(!project.getUseProjectKey());
        CHECK(project.getScaleRootNote() == 4);
        CHECK(project.getPreferredScaleMode() == ScaleMode::Major);
        CHECK(project.getProjectKeyRoot() == 0);
        CHECK(!useProjectKey->getToggleState());

        modeBox->setSelectedId(2, juce::sendNotificationSync); // minor
        CHECK(changes == 2);
        CHECK(project.getPreferredScaleMode() == ScaleMode::Minor);
        CHECK(project.getProjectKeyMode() == ScaleMode::Major);

        // Following the project key again shows C major.
        useProjectKey->setToggleState(true, juce::sendNotificationSync);
        CHECK(changes == 3);
        CHECK(project.getUseProjectKey());
        CHECK(project.getScaleRootNote() == 0);
        CHECK(rootBox->getSelectedId() == 1 && modeBox->getSelectedId() == 1);

        // Track view, region following the project key: the menus change
        // the project key itself.
        trackView = true;
        inspector.refresh();
        CHECK(useProjectKey->isVisible());
        rootBox->setSelectedId(8, juce::sendNotificationSync); // G
        modeBox->setSelectedId(5, juce::sendNotificationSync); // harmonic minor
        CHECK(project.getProjectKeyRoot() == 7);
        CHECK(project.getProjectKeyMode() == ScaleMode::HarmonicMinor);
        CHECK(project.getUseProjectKey()); // the region still follows it
        CHECK(project.getScaleRootNote() == 7);
        CHECK(changes == 5);

        // Track view, region with its own key: the menus show and change the
        // key harmony uses (the region's), not the project key.
        project.setScaleRootNote(2);                     // D, own key
        project.setPreferredScaleMode(ScaleMode::Dorian);
        inspector.refresh();
        CHECK(!useProjectKey->getToggleState());
        CHECK(rootBox->getSelectedId() == 3 && modeBox->getSelectedId() == 4);
        rootBox->setSelectedId(10, juce::sendNotificationSync); // A
        CHECK(project.getScaleRootNote() == 9);
        CHECK(project.getProjectKeyRoot() == 7); // project key untouched
        trackView = false;

        // A pick made through the popup arrives asynchronously; a refresh
        // in between (the 8 Hz timer) must not undo it.
        project.setUseProjectKey(true);
        inspector.refresh();
        rootBox->setSelectedId(5, juce::sendNotificationAsync); // E, pending
        inspector.refresh();
        CHECK(rootBox->getSelectedId() == 5);
        for (int i = 0; i < 20 && project.getUseProjectKey(); ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        CHECK(project.getScaleRootNote() == 4);
        CHECK(!project.getUseProjectKey());
        inspector.refresh();
        CHECK(rootBox->getSelectedId() == 5);

        // An unset root shows "-" instead of a guessed key.
        project.setScaleRootNote(-1);
        inspector.refresh();
        CHECK(rootBox->getSelectedId() == 0);
        CHECK(rootBox->getText().isEmpty());

        // Taller than the default editor: the column scrolls instead of
        // cutting the harmony buttons off.
        const auto viewports = findChildren<juce::Viewport>(inspector);
        CHECK(viewports.size() == 1);
        CHECK(viewports.front()->getViewedComponent()->getHeight() > inspector.getHeight());
    }

    // --- Correct Pitch survives harmony, arrow keys and snapped drags ----
    // (user report 2026-10-19: after a harmony shift Correct Pitch showed 0%
    // and the slider did nothing, because the move overwrote the sung pitch
    // the correction is measured from with the already-corrected pitch.)
    {
        Fixture f;
        f.project.setProjectKey(0, ScaleMode::Major); // C major
        f.note(0).setSelected(true);                  // sung 30 cents sharp of C
        const auto correctTo = [&f](float percent)
        {
            f.roll.beginInspectorEdit(Param::CorrectPitch);
            f.roll.setInspectorEditValue(percent);
            f.roll.endInspectorEdit();
        };
        correctTo(100.0f);
        CHECK(near(f.note(0).getMidiNote(), 60.0f));

        // Third up: C -> E. Still 100% corrected, and the slider still works.
        CHECK(f.roll.applyHarmonyInterval(HarmonyUtils::Interval::Third, true) ==
              HarmonyUtils::Result::Applied);
        CHECK(near(f.note(0).getMidiNote(), 64.0f));
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 100.0f));
        correctTo(0.0f);
        CHECK(near(f.note(0).getMidiNote(), 64.3f)); // the sung 30 cents, at E
        correctTo(100.0f);
        CHECK(near(f.note(0).getMidiNote(), 64.0f));

        // Undo walks the reference back too.
        f.history.undo(); // correct 100
        f.history.undo(); // correct 0
        f.history.undo(); // harmony
        CHECK(near(f.note(0).getMidiNote(), 60.0f));
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 60.3f));
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 100.0f));
        f.history.redo(); // harmony again
        CHECK(near(f.note(0).getMidiNote(), 64.0f));
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 64.3f));

        // Arrow key (Absolute: next semitone) keeps the correction too.
        CHECK(f.roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
        CHECK(near(f.note(0).getMidiNote(), 65.0f));
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 100.0f));
        // Shift+arrow (cents) toward the sung side eats into the correction.
        CHECK(f.roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey,
                                               juce::ModifierKeys::shiftModifier, 0)));
        CHECK(near(f.note(0).getMidiNote(), 65.01f));
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 96.67f, 0.05f));
    }

    // --- Pitch drags: snapped keeps the sung pitch, free places it ---------
    {
        Fixture f;
        f.roll.fitPitchRangeToView(56.0f, 68.0f);
        const auto left = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        const auto xAt = [&f](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * f.roll.getPixelsPerSecond() -
                   static_cast<float>(f.roll.getScrollX());
        };
        const auto yAt = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(f.roll.getScrollY());
        };
        const auto drag = [&](float fromMidi, float semitones, juce::ModifierKeys mods)
        {
            const juce::Point<float> start(xAt(50), yAt(fromMidi));
            const auto end = start + juce::Point<float>(0.0f, -semitones * f.roll.getPixelsPerSemitone());
            const auto held = juce::ModifierKeys(mods.getRawFlags() | left.getRawFlags());
            f.roll.mouseDown(event(f.roll, start, held, 1));
            f.roll.mouseDrag(event(f.roll, start + juce::Point<float>(0.0f, -6.0f), held, 1));
            f.roll.mouseDrag(event(f.roll, end, held, 1));
            f.roll.mouseUp(event(f.roll, end, held, 1));
        };

        // Absolute drag up two semitones: lands on D, counts as fully corrected.
        drag(60.3f, 2.0f, {});
        CHECK(near(f.note(0).getMidiNote(), 62.0f));
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 62.3f));
        f.note(0).setSelected(true);
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 100.0f));
        f.note(0).setSelected(false);
        f.history.undo();
        CHECK(near(f.note(0).getMidiNote(), 60.3f));
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 60.3f));

        // Free drag (Shift = snap off): the placed pitch is the new reference.
        drag(60.3f, 1.37f, juce::ModifierKeys::shiftModifier);
        const float placed = f.note(0).getMidiNote();
        CHECK(std::abs(placed - 61.67f) < 0.15f);
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), placed));
        f.history.undo();
        CHECK(near(f.note(0).getMidiNote(), 60.3f));
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 60.3f));
    }

    // --- Out and back leaves an unedited segment unedited ------------------
    // (review 2026-10-19: cents nudges past the sung pitch and back used to
    // leave the reference 5 cents off, so an unedited note read as 14%.)
    {
        Fixture f;
        f.note(0).setSelected(true); // sung and analysed at 60.3
        const auto key = [&f](int code, bool shift)
        {
            return f.roll.keyPressed(juce::KeyPress(
                code, shift ? juce::ModifierKeys::shiftModifier : juce::ModifierKeys(), 0));
        };
        const auto unedited = [&f]
        {
            return near(f.note(0).getMidiNote(), 60.3f) &&
                   near(f.note(0).getLastNonMacroMidiNote(), 60.3f) &&
                   near(*f.roll.getInspectorValue(Param::CorrectPitch), 0.0f);
        };
        for (int i = 0; i < 5; ++i) CHECK(key(juce::KeyPress::upKey, true));
        CHECK(near(f.note(0).getMidiNote(), 60.35f));
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 0.0f));
        for (int i = 0; i < 5; ++i) CHECK(key(juce::KeyPress::downKey, true));
        CHECK(unedited());

        // Absolute Up (lands on C#, 100%), then 70 cents back down.
        CHECK(key(juce::KeyPress::upKey, false));
        CHECK(near(f.note(0).getMidiNote(), 61.0f));
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 100.0f));
        for (int i = 0; i < 70; ++i) CHECK(key(juce::KeyPress::downKey, true));
        CHECK(unedited());

        // Down past the semitone and back up.
        for (int i = 0; i < 40; ++i) CHECK(key(juce::KeyPress::downKey, true));
        for (int i = 0; i < 40; ++i) CHECK(key(juce::KeyPress::upKey, true));
        CHECK(unedited());
    }

    // --- A pitch beyond 100% shows 0% and the slider starts there ---------
    {
        Fixture f;
        f.note(0).setSelected(true);
        // Overcorrected: sung 60.2, now 59.9 (past C). Not a 0-100% correction.
        f.note(0).setMidiNote(59.9f);
        f.note(0).setLastNonMacroMidiNote(60.2f);
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 0.0f));
        // Touching the slider without moving it changes nothing, no undo step.
        f.roll.beginInspectorEdit(Param::CorrectPitch);
        f.roll.endInspectorEdit();
        CHECK(!f.history.canUndo());
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 60.2f));
        // A small move stays small (no jump) and is one undo step.
        f.roll.beginInspectorEdit(Param::CorrectPitch);
        f.roll.setInspectorEditValue(10.0f);
        f.roll.endInspectorEdit();
        CHECK(std::abs(f.note(0).getMidiNote() - 59.9f) < 0.02f);
        f.history.undo();
        CHECK(near(f.note(0).getMidiNote(), 59.9f));
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 60.2f));
    }

    // --- Multi-note drags and splits keep every share within 0-100% -------
    {
        Fixture f;
        f.roll.fitPitchRangeToView(56.0f, 68.0f);
        const auto held = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        const auto xAt = [&f](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * f.roll.getPixelsPerSecond() -
                   static_cast<float>(f.roll.getScrollX());
        };
        const auto yAt = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(f.roll.getScrollY());
        };
        const auto shareOf = [&f](const Note& note)
        {
            return ScaleUtils::correctionShare(f.project, note.getLastNonMacroMidiNote(),
                                               note.getMidiNote(), 440);
        };
        // Grab note 0 (60.3) with note 1 (61.8) also selected; Absolute lands
        // the grabbed one on D and moves the other by the same amount.
        f.note(0).setSelected(true);
        f.note(1).setSelected(true);
        const juce::Point<float> start(xAt(50), yAt(60.3f));
        const auto end = start + juce::Point<float>(0.0f, -2.0f * f.roll.getPixelsPerSemitone());
        f.roll.mouseDown(event(f.roll, start, held, 1));
        f.roll.mouseDrag(event(f.roll, start + juce::Point<float>(0.0f, -6.0f), held, 1));
        f.roll.mouseDrag(event(f.roll, end, held, 1));
        f.roll.mouseUp(event(f.roll, end, held, 1));
        CHECK(near(f.note(0).getMidiNote(), 62.0f));
        CHECK(shareOf(f.note(0)).has_value() && near(*shareOf(f.note(0)), 1.0f));
        CHECK(near(f.note(1).getMidiNote(), 63.5f));
        CHECK(shareOf(f.note(1)).has_value()); // within 0-100%, no jump
        f.history.undo();
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 60.3f));
        CHECK(near(f.note(1).getLastNonMacroMidiNote(), 61.8f));

        // Correct to 100%, then split with Option-click: both halves stay
        // corrected (splitting is pitch-neutral).
        f.note(1).setSelected(false);
        f.roll.beginInspectorEdit(Param::CorrectPitch);
        f.roll.setInspectorEditValue(100.0f);
        f.roll.endInspectorEdit();
        CHECK(near(f.note(0).getMidiNote(), 60.0f));
        const size_t before = f.project.getNotes().size();
        const auto alt = juce::ModifierKeys(juce::ModifierKeys::altModifier |
                                            juce::ModifierKeys::leftButtonModifier);
        f.roll.mouseDown(event(f.roll, {xAt(50), yAt(60.0f)}, alt, 1));
        f.roll.mouseUp(event(f.roll, {xAt(50), yAt(60.0f)}, alt, 1));
        CHECK(f.project.getNotes().size() == before + 1);
        for (size_t i = 0; i < 2; ++i)
        {
            const auto share = shareOf(f.note(i));
            CHECK(share.has_value() && *share > 0.9f);
        }
    }

    // --- A right click on the cycle ruler does not hijack the next drag ----
    {
        Fixture f;
        f.roll.fitPitchRangeToView(56.0f, 68.0f);
        const auto loopBefore = f.project.getLoopRange();
        const juce::Point<float> ruler(400.0f, (PianoRollComponent::timelineHeight +
                                                PianoRollComponent::headerHeight) * 0.5f);
        const auto right = juce::ModifierKeys(juce::ModifierKeys::rightButtonModifier);
        f.roll.mouseDown(event(f.roll, ruler, right, 1));
        f.roll.mouseUp(event(f.roll, ruler, right, 1));
        const auto held = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        const float x = PianoRollComponent::pianoKeysWidth +
                        static_cast<float>(framesToSeconds(50)) * f.roll.getPixelsPerSecond() -
                        static_cast<float>(f.roll.getScrollX());
        const float y = PianoRollComponent::headerHeight +
                        (MAX_MIDI_NOTE - 60.3f) * f.roll.getPixelsPerSemitone() +
                        f.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(f.roll.getScrollY());
        const juce::Point<float> start(x, y);
        const auto end = start + juce::Point<float>(0.0f, -2.0f * f.roll.getPixelsPerSemitone());
        f.roll.mouseDown(event(f.roll, start, held, 1));
        f.roll.mouseDrag(event(f.roll, start + juce::Point<float>(0.0f, -6.0f), held, 1));
        f.roll.mouseDrag(event(f.roll, end, held, 1));
        f.roll.mouseUp(event(f.roll, end, held, 1));
        CHECK(near(f.note(0).getMidiNote(), 62.0f)); // the drag moved the note
        const auto loopAfter = f.project.getLoopRange();
        CHECK(loopAfter.enabled == loopBefore.enabled &&
              near(static_cast<float>(loopAfter.startSeconds), static_cast<float>(loopBefore.startSeconds)) &&
              near(static_cast<float>(loopAfter.endSeconds), static_cast<float>(loopBefore.endSeconds)));
    }

    // --- Defaults: Absolute snap, default smart controls, scale shown ----
    {
        Project project;
        prepareAudio(project);
        CHECK(project.getSnapToSemitones());
        CHECK(project.getScaleMode() != ScaleMode::Chromatic &&
              project.getScaleMode() != ScaleMode::None);
        PianoRollComponent roll;
        CHECK(!roll.getShowAllSmartControls());
        CHECK(roll.getAbsoluteDragSnap());

        VariAudioInspector inspector;
        inspector.getCurrentProject = [&project] { return &project; };
        inspector.refresh();
        juce::ComboBox* smartBox = nullptr;
        juce::ComboBox* snapBox = nullptr;
        for (auto* box : findChildren<juce::ComboBox>(inspector))
        {
            if (box->getNumItems() == 2)
                smartBox = box;
            else if (box->getNumItems() == 3)
                snapBox = box;
        }
        CHECK(smartBox != nullptr && snapBox != nullptr);
        CHECK(smartBox->getSelectedId() == 1); // default smart controls
        CHECK(snapBox->getSelectedId() == 1);  // Absolute
    }

    // --- Option-double-click on empty space: zoom to show everything ------
    {
        Fixture f;
        f.roll.fitPitchRangeToView(58.0f, 66.0f);
        int requests = 0;
        f.roll.onZoomFullRequested = [&requests] { ++requests; };
        const auto altLeft = juce::ModifierKeys(juce::ModifierKeys::altModifier |
                                                juce::ModifierKeys::leftButtonModifier);
        const auto yFor = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(f.roll.getScrollY());
        };
        const auto xFor = [&f](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * f.roll.getPixelsPerSecond() -
                   static_cast<float>(f.roll.getScrollX());
        };
        // No segment at frame 400 (the last one ends at 300).
        f.roll.mouseDoubleClick(event(f.roll, {xFor(400), yFor(60.0f)}, altLeft, 2));
        CHECK(requests == 1);
        // On a segment it is not a zoom.
        f.roll.mouseDoubleClick(event(f.roll, {xFor(50), yFor(60.3f)}, altLeft, 2));
        CHECK(requests == 1);
        // Without Option it is not a zoom either.
        f.roll.mouseDoubleClick(event(f.roll, {xFor(400), yFor(60.0f)},
                                      juce::ModifierKeys::leftButtonModifier, 2));
        CHECK(requests == 1);

        // A double-click whose first Option-click landed on a segment (and
        // split it) is not a zoom, even if the pointer ends up over no
        // segment once the halves are re-centred.
        const size_t notesBefore = f.project.getNotes().size();
        f.roll.mouseDown(event(f.roll, {xFor(50), yFor(60.3f)}, altLeft, 1));
        f.roll.mouseUp(event(f.roll, {xFor(50), yFor(60.3f)}, altLeft, 1));
        CHECK(f.project.getNotes().size() == notesBefore + 1); // Option-click split
        f.roll.mouseDoubleClick(event(f.roll, {xFor(50), yFor(64.0f)}, altLeft, 2));
        CHECK(requests == 1);
        // The next double-click that starts on empty space zooms again.
        f.roll.mouseDown(event(f.roll, {xFor(400), yFor(60.0f)}, altLeft, 1));
        f.roll.mouseUp(event(f.roll, {xFor(400), yFor(60.0f)}, altLeft, 1));
        f.roll.mouseDoubleClick(event(f.roll, {xFor(400), yFor(60.0f)}, altLeft, 2));
        CHECK(requests == 2);
    }

    std::cout << "VariAudio inspector tests passed\n";
    return 0;
}
