#pragma once

#include "../../JuceHeader.h"

namespace WindowSizing {

struct Constraints {
  int minWidth = 960;
  int minHeight = 600;
  float initialMaxFraction = 0.92f;
  int initialMargin = 24;
};

// Batch 4 fix (0.4.2): the default editor window grows from
// 1000x628 to 1200x840 - mainly taller, matching how the editor is
// actually worked in (trial log, 2026-11-11: the old default opened
// at 1000x568 on the user's machine; their working size is about
// 930x752). A taller window at the fixed default scales is what puts
// a clip's segments on screen at open (no automatic fit). The clamp
// in getDefaultMainViewSize still keeps it inside the display.
constexpr int kDefaultWidth = 1200;
constexpr int kDefaultHeight = 840;

const juce::Displays::Display *getPrimaryDisplay();
const juce::Displays::Display *getDisplayForComponent(
    const juce::Component *component);

/**
 * Get initial window bounds.
 * The desiredWidth/Height are in logical pixels.
 */
juce::Rectangle<int> getInitialBounds(int desiredWidth, int desiredHeight,
                                      const juce::Displays::Display &display,
                                      const Constraints &constraints);

/**
 * Get clamped window size.
 */
juce::Point<int> getClampedSize(int desiredWidth, int desiredHeight,
                                const juce::Displays::Display &display,
                                const Constraints &constraints);

/**
 * Get maximum bounds for a display (the usable area).
 */
juce::Rectangle<int> getMaxBounds(const juce::Displays::Display &display);

} // namespace WindowSizing
