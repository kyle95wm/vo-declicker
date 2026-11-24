#include "PluginProcessor.h"
#include "PluginEditor.h"

VODeclickerProcessor::VODeclickerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VODeclicker", createLayout())
{
    sensitivityParam = apvts.getRawParameterValue (sensitivityId);
    frequencyParam   = apvts.getRawParameterValue (frequencyId);
    lengthParam      = apvts.getRawParameterValue (lengthId);
    strongParam      = apvts.getRawParameterValue (strongId);
    listenParam      = apvts.getRawParameterValue (listenId);
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (bypassId));
    jassert (sensitivityParam && frequencyParam && lengthParam && strongParam && listenParam && bypassParam);
}

juce::AudioProcessorValueTreeState::ParameterLayout VODeclickerProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { sensitivityId, 1 }, "Sensitivity",
        juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("%")
            .withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v)) + " %"; })));

    juce::NormalisableRange<float> freqRange (1000.0f, 8000.0f, 1.0f);
    freqRange.setSkewForCentre (3000.0f);
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { frequencyId, 1 }, "Frequency", freqRange, 3000.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("Hz")
            .withStringFromValueFunction ([] (float v, int) { return juce::String (v / 1000.0f, 1) + " kHz"; })));

    juce::NormalisableRange<float> lengthRange (0.5f, (float) declick::kMaxLengthMs, 0.01f);
    lengthRange.setSkewForCentre (3.0f);
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { lengthId, 1 }, "Length", lengthRange, 6.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("ms")
            .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1) + " ms"; })));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { strongId, 1 }, "Strong mode", false,
        juce::AudioParameterBoolAttributes()
            .withStringFromValueFunction ([] (bool v, int) { return juce::String (v ? "Strong" : "Gentle"); })));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { listenId, 1 }, "Listen to clicks", false));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { bypassId, 1 }, "Bypass", false));

    return layout;
}

bool VODeclickerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

declick::Settings VODeclickerProcessor::currentSettings() const
{
    declick::Settings s;
    s.sensitivity = sensitivityParam->load() * 0.01f;
    s.frequencyHz = frequencyParam->load();
    s.lengthMs    = lengthParam->load();
    s.strong      = strongParam->load() > 0.5f;
    return s;
}

void VODeclickerProcessor::prepareToPlay (double sampleRate, int)
{
    engine.setSettings (currentSettings());
    engine.prepare (sampleRate, std::max (1, getTotalNumInputChannels()));
    setLatencySamples (engine.getLatencySamples());

    bypassMix.reset (sampleRate, 0.02);   // 20 ms: click-free bypass and Listen
    listenMix.reset (sampleRate, 0.02);
    bypassMix.setCurrentAndTargetValue (bypassParam->get() ? 1.0f : 0.0f);
    listenMix.setCurrentAndTargetValue (listenParam->load() > 0.5f ? 1.0f : 0.0f);
}

void VODeclickerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numIn = std::min (2, getTotalNumInputChannels());
    for (int ch = numIn; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());
    if (numIn == 0)
        return;

    engine.setSettings (currentSettings());
    bypassMix.setTargetValue (bypassParam->get() ? 1.0f : 0.0f);
    listenMix.setTargetValue (listenParam->load() > 0.5f ? 1.0f : 0.0f);

    float* io[2] = { buffer.getWritePointer (0), buffer.getWritePointer (numIn > 1 ? 1 : 0) };
    float in[2], dry[2], corr[2];

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        for (int ch = 0; ch < numIn; ++ch)
            in[ch] = io[ch][i];

        engine.processFrame (in, numIn, dry, corr);

        const bool smoothing = bypassMix.isSmoothing() || listenMix.isSmoothing();
        const float g = bypassMix.getNextValue();
        const float l = listenMix.getNextValue();

        for (int ch = 0; ch < numIn; ++ch)
        {
            float out;
            if (! smoothing)
                // Exact paths: bypass is the delayed input bit for bit, and so is
                // the processed signal anywhere no click was repaired.
                out = g >= 1.0f ? dry[ch] : (l >= 1.0f ? corr[ch] : dry[ch] - corr[ch]);
            else
            {
                const float processed = (1.0f - l) * (dry[ch] - corr[ch]) + l * corr[ch];
                out = processed + g * (dry[ch] - processed);
            }
            io[ch][i] = out;
        }
    }
}

void VODeclickerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void VODeclickerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* VODeclickerProcessor::createEditor()
{
    return new VODeclickerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VODeclickerProcessor();
}
