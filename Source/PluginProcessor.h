#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "UpdateChecker.h"
#include "VODeclickerDSP.h"

// VODeclicker: finds mouth clicks and lip smacks in a voice and rebuilds them
// from the audio either side (see VODeclickerDSP.h). Three knobs, Gentle/Strong,
// Listen, and an internal Bypass that is also the host's bypass parameter.
class VODeclickerProcessor : public juce::AudioProcessor
{
public:
    VODeclickerProcessor();
    ~VODeclickerProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    // Spans repaired since the plugin was created (for the UI's counter).
    uint32_t getRepairCount() const noexcept { return engine.repairs.load (std::memory_order_relaxed); }
    const declick::Engine& getEngine() const noexcept { return engine; }

    juce::AudioProcessorValueTreeState apvts;
    UpdateChecker updateChecker;

    // Parameter IDs are part of saved sessions and automation: never rename
    // one after a release (add a new ID instead).
    static constexpr const char* sensitivityId = "sensitivity";
    static constexpr const char* frequencyId   = "frequency";
    static constexpr const char* lengthId      = "length";
    static constexpr const char* strongId      = "strong";
    static constexpr const char* listenId      = "listen";
    static constexpr const char* bypassId      = "bypass";

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    declick::Settings currentSettings() const;

    std::atomic<float>* sensitivityParam = nullptr;
    std::atomic<float>* frequencyParam = nullptr;
    std::atomic<float>* lengthParam = nullptr;
    std::atomic<float>* strongParam = nullptr;
    std::atomic<float>* listenParam = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;

    declick::Engine engine;
    juce::SmoothedValue<float> bypassMix { 0.0f };   // 1 = bypassed
    juce::SmoothedValue<float> listenMix { 0.0f };   // 1 = listening to what's removed

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VODeclickerProcessor)
};
