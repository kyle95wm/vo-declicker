#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "BrandLookAndFeel.h"
#include "UpdateFooter.h"

// Window layout (see CONVENTIONS.md):
//   header  - plugin name, version, subtitle
//   body    - one big Sensitivity knob, then a "clicks repaired" counter
//   pills   - LISTEN, ACTIVE|BYPASSED
//   link    - "Advanced", which opens a panel below with Frequency / Length
//             and GENTLE|STRONG (the window grows to fit; the open/closed
//             state is saved with the session, not as a parameter)
//   footer  - shared UpdateFooter
class VODeclickerEditor : public juce::AudioProcessorEditor,
                          private juce::Timer
{
public:
    static constexpr int width = 460, collapsedHeight = 500, advancedHeight = 160;
    static constexpr const char* advancedStateId = "advancedOpen";   // property on apvts.state

    explicit VODeclickerEditor (VODeclickerProcessor&);
    ~VODeclickerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void setupKnob (juce::Slider&, juce::Label&, const juce::String& name, double defaultValue,
                    const juce::String& tooltip, int textBoxWidth);
    void setAdvancedOpen (bool open);
    bool isAdvancedOpen() const;

    VODeclickerProcessor& audioProcessor;
    brand::LookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };

    juce::Slider sensitivityKnob, frequencyKnob, lengthKnob;
    juce::Label sensitivityLabel, frequencyLabel, lengthLabel;
    juce::ToggleButton strongButton { "Strong" }, listenButton { "Listen" }, bypassButton { "Bypass" };
    juce::Label modeLabel;
    juce::HyperlinkButton advancedLink;
    UpdateFooter footer;

    juce::AudioProcessorValueTreeState::SliderAttachment sensitivityAttachment, frequencyAttachment, lengthAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment strongAttachment, listenAttachment, bypassAttachment;

    juce::Rectangle<int> counterArea, advancedArea;
    uint32_t shownRepairs = 0;
    float flash = 0.0f;   // 1 right after a repair, fades out

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VODeclickerEditor)
};
