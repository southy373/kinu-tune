#pragma once

#include "../JuceHeader.h"
#include "../UI/Sliders.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/Theme.h"

/**
 * Batch 2 (insert prototype): the whole UI of the non-ARA insert mode -
 * exactly two faders bound to the processor's insert parameters
 * (pitch %, formant semitones) through APVTS attachments, so host
 * automation, state save/restore and the faders all share one value.
 * The faders are the shared GlobalFaderSlider: double-click opens
 * numeric entry, Alt+click resets to neutral (100% / 0 st). No capture
 * UI exists in this mode.
 */
class InsertEffectView : public juce::Component
{
public:
    InsertEffectView(juce::AudioProcessorValueTreeState &apvts,
                     const juce::String &pitchParamId,
                     const juce::String &formantParamId)
    {
        noteLabel.setText(TR("insert.note"), juce::dontSendNotification);
        noteLabel.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(noteLabel);

        pitchLabel.setText(TR("insert.pitch"), juce::dontSendNotification);
        formantLabel.setText(TR("insert.formant"), juce::dontSendNotification);
        addAndMakeVisible(pitchLabel);
        addAndMakeVisible(formantLabel);

        for (auto *slider : {&pitchSlider, &formantSlider})
        {
            slider->setSliderStyle(juce::Slider::LinearHorizontal);
            slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 24);
            addAndMakeVisible(*slider);
        }
        pitchSlider.setNeutralValue(100.0);
        pitchSlider.setTextValueSuffix(" %");
        formantSlider.setNeutralValue(0.0);

        pitchAttachment = std::make_unique<
            juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, pitchParamId, pitchSlider);
        formantAttachment = std::make_unique<
            juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, formantParamId, formantSlider);
    }

    void paint(juce::Graphics &g) override { g.fillAll(APP_COLOR_BACKGROUND); }

    void resized() override
    {
        auto area = getLocalBounds().withSizeKeepingCentre(
            juce::jmin(getWidth() - 48, 460), 150);
        noteLabel.setBounds(area.removeFromTop(24));
        area.removeFromTop(14);
        auto pitchRow = area.removeFromTop(28);
        pitchLabel.setBounds(pitchRow.removeFromLeft(130));
        pitchSlider.setBounds(pitchRow);
        area.removeFromTop(12);
        auto formantRow = area.removeFromTop(28);
        formantLabel.setBounds(formantRow.removeFromLeft(130));
        formantSlider.setBounds(formantRow);
    }

private:
    juce::Label noteLabel, pitchLabel, formantLabel;
    GlobalFaderSlider pitchSlider, formantSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        pitchAttachment, formantAttachment;
};
