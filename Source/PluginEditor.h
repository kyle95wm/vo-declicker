#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "BrandLookAndFeel.h"
#include "UpdateFooter.h"

// Window layout (see CONVENTIONS.md):
//   header  - plugin name, version, subtitle
//   body    - Sensitivity / Frequency / Length knobs, then a "clicks repaired" counter
//   pills   - GENTLE|STRONG, LISTEN, ACTIVE|BYPASSED
//   footer  - shared UpdateFooter
class VODeclickerEditor : public juce::AudioProcessorEditor,
                          private juce::Timer
{
public:
    explicit VODeclickerEditor (VODeclickerProcessor&);
    ~VODeclickerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void setupKnob (juce::Slider&, juce::Label&, const juce::String& name, double defaultValue,
                    const juce::String& tooltip);

    VODeclickerProcessor& audioProcessor;
    brand::LookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };

    juce::Slider sensitivityKnob, frequencyKnob, lengthKnob;
    juce::Label sensitivityLabel, frequencyLabel, lengthLabel;
    juce::ToggleButton strongButton { "Strong" }, listenButton { "Listen" }, bypassButton { "Bypass" };
    UpdateFooter footer;

    juce::AudioProcessorValueTreeState::SliderAttachment sensitivityAttachment, frequencyAttachment, lengthAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment strongAttachment, listenAttachment, bypassAttachment;

    juce::Rectangle<int> counterArea;
    uint32_t shownRepairs = 0;
    float flash = 0.0f;   // 1 right after a repair, fades out

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VODeclickerEditor)
};
