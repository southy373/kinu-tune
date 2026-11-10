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

// The batch 2 GlobalFaderSlider that used to live here is gone (batch
// 4): its only remaining users, the inspector's three project-wide
// faders, are VariAudioInspector::ValueRow components now, and the
// double-click numeric entry / Alt+click reset interactions moved
// into that class with them. The insert prototype's two faders, its
// other user, were removed in batch 3.
