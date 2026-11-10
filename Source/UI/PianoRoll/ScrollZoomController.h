#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "CoordinateMapper.h"
#include <functional>

/**
 * Handles scroll and zoom operations for the piano roll.
 * Manages scrollbars and mouse wheel/magnify gestures.
 *
 * Batch 4 note: this controller is currently NOT wired into
 * PianoRollComponent (nothing calls handleMouseWheel/handleMagnify,
 * and its scrollbars are never given bounds), so it is unreachable in
 * the shipped editor. It writes the CoordinateMapper directly,
 * bypassing PianoRollComponent's guarded view-state setters
 * (batch 3.2/4). If it is ever wired in, route its scroll/zoom writes
 * through PianoRollComponent::setScrollX/setScrollY/
 * setPixelsPerSecond/setPixelsPerSemitone instead of letting it own
 * a second, unguarded path into the view state.
 */
class ScrollZoomController : public juce::ScrollBar::Listener {
public:
    ScrollZoomController();
    ~ScrollZoomController() override;

    void setCoordinateMapper(CoordinateMapper* mapper) { coordMapper = mapper; }
    void setProject(Project* proj) { project = proj; }

    // ScrollBar::Listener
    void scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) override;

    // Mouse handling
    void handleMouseWheel(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel,
                         int pianoKeysWidth, int headerHeight, int componentWidth, int componentHeight);
    void handleMagnify(const juce::MouseEvent& e, float scaleFactor, int pianoKeysWidth);

    // Programmatic control
    void setScrollX(double x);
    void setPixelsPerSecond(float pps, bool centerOnCursor = false, double cursorTime = 0.0);
    void setPixelsPerSemitone(float pps, float anchorContentY = -1.0f,
                              int visibleHeight = 0);
    void centerOnPitchRange(float minMidi, float maxMidi, int visibleHeight);

    // Update scrollbar ranges
    void updateScrollBars(int visibleWidth, int visibleHeight);

    // Access scrollbars for layout
    juce::ScrollBar& getHorizontalScrollBar() { return horizontalScrollBar; }
    juce::ScrollBar& getVerticalScrollBar() { return verticalScrollBar; }

    // Callbacks
    std::function<void(float)> onZoomChanged;
    std::function<void(double)> onScrollChanged;
    std::function<void()> onRepaintNeeded;

private:
    CoordinateMapper* coordMapper = nullptr;
    Project* project = nullptr;

    juce::ScrollBar horizontalScrollBar{false};
    juce::ScrollBar verticalScrollBar{true};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScrollZoomController)
};
