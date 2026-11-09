#pragma once

#include "../JuceHeader.h"

class SliderBox : public juce::Slider
{
public:
    explicit SliderBox(juce::String helpText = {});
    ~SliderBox() override;

    double snapValue(double attemptedValue, DragMode dragMode) override;
    void setEnabled(bool shouldBeEnabled);
    void setHideSuffixWhileEditing(bool shouldHide);
    bool shouldHideSuffixWhileEditing() const;

private:
    bool hideSuffixWhileEditing = false;
};

/**
 * Batch 2: a SliderBox for the project-wide faders (global pitch %,
 * global formant, global length %, and the insert prototype's two
 * faders), with the two interactions the user asked for on every one of
 * them:
 *  - double-click opens the numeric text editor (showTextBox);
 *  - Alt+click resets to the neutral value (100% / no correction) and
 *    is swallowed, so it never starts a drag or an absolute set.
 * The reset goes through setValue with sendNotificationSync, so the
 * owner's onValueChange treats it exactly like a committed text entry.
 */
class GlobalFaderSlider : public SliderBox
{
public:
    explicit GlobalFaderSlider(juce::String helpText = {})
        : SliderBox(std::move(helpText))
    {
    }

    void setNeutralValue(double neutral) { neutralValue = neutral; }
    double getNeutralValue() const { return neutralValue; }

    void mouseDown(const juce::MouseEvent &e) override
    {
        if (e.mods.isAltDown())
        {
            setValue(neutralValue, juce::sendNotificationSync);
            return;
        }
        SliderBox::mouseDown(e);
    }

    void mouseDoubleClick(const juce::MouseEvent &e) override
    {
        juce::ignoreUnused(e);
        showTextBox();
    }

private:
    double neutralValue = 0.0;
};
