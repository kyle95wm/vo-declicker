#include "PluginEditor.h"

VODeclickerEditor::VODeclickerEditor (VODeclickerProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p),
      footer (p.updateChecker),
      sensitivityAttachment (p.apvts, VODeclickerProcessor::sensitivityId, sensitivityKnob),
      frequencyAttachment (p.apvts, VODeclickerProcessor::frequencyId, frequencyKnob),
      lengthAttachment (p.apvts, VODeclickerProcessor::lengthId, lengthKnob),
      strongAttachment (p.apvts, VODeclickerProcessor::strongId, strongButton),
      listenAttachment (p.apvts, VODeclickerProcessor::listenId, listenButton),
      bypassAttachment (p.apvts, VODeclickerProcessor::bypassId, bypassButton)
{
    setLookAndFeel (&lnf);

    setupKnob (sensitivityKnob, sensitivityLabel, "SENSITIVITY", 50.0,
               "How readily a sound counts as a click. 0 % turns repairs off.");
    setupKnob (frequencyKnob, frequencyLabel, "FREQUENCY", 3000.0,
               "Only content above this frequency is checked and rebuilt. The low band is never touched.");
    setupKnob (lengthKnob, lengthLabel, "LENGTH", 4.0,
               "The longest noise treated as a click. Raise it for wet lip smacks; anything longer is left alone as speech.");

    brand::makePill (strongButton, "GENTLE", "STRONG");
    strongButton.setTooltip ("Gentle repairs clicks in pauses and at word edges only. "
                             "Strong also repairs clicks inside voiced words.");
    addAndMakeVisible (strongButton);

    brand::makePill (listenButton, "LISTEN", "LISTENING");
    listenButton.setTooltip ("Hear only what is being removed, to check it isn't eating consonants.");
    addAndMakeVisible (listenButton);

    brand::makePill (bypassButton, "ACTIVE", "BYPASSED");
    bypassButton.setTooltip ("Bypass the processing (also linked to your DAW's bypass).");
    addAndMakeVisible (bypassButton);

    addAndMakeVisible (footer);

    shownRepairs = audioProcessor.getRepairCount();
    setSize (460, 440);
    startTimerHz (30);
}

VODeclickerEditor::~VODeclickerEditor()
{
    setLookAndFeel (nullptr);
}

void VODeclickerEditor::setupKnob (juce::Slider& knob, juce::Label& label, const juce::String& name,
                                   double defaultValue, const juce::String& tooltip)
{
    // Add controls *before* styling them (see the template's note on text boxes).
    addAndMakeVisible (knob);
    knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                              juce::MathConstants<float>::pi * 2.75f, true);
    knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 22);
    knob.setDoubleClickReturnValue (true, defaultValue);
    knob.setVelocityBasedMode (false);
    knob.setTooltip (tooltip);

    addAndMakeVisible (label);
    label.setText (name, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (juce::FontOptions (brand::pillSize, juce::Font::bold));
    label.setColour (juce::Label::textColourId, brand::textDim);
}

void VODeclickerEditor::timerCallback()
{
    const auto n = audioProcessor.getRepairCount();
    if (n != shownRepairs)
    {
        shownRepairs = n;
        flash = 1.0f;
        repaint (counterArea);
    }
    else if (flash > 0.0f)
    {
        flash = std::max (0.0f, flash - 0.06f);
        repaint (counterArea);
    }
}

void VODeclickerEditor::paint (juce::Graphics& g)
{
    g.fillAll (brand::bg);
    brand::drawHeader (g, getLocalBounds(), JucePlugin_Name, UpdateChecker::getCurrentVersion(),
                       juce::String (juce::CharPointer_UTF8 ("Mouth clicks & lip smacks  \xc2\xb7  rebuilt, not muted")));

    // Repair counter: a dot that lights up in the accent on each repair.
    auto r = counterArea.toFloat();
    g.setColour (brand::panel);
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);

    const float d = 8.0f;
    const auto dot = juce::Rectangle<float> (d, d).withCentre ({ r.getX() + 18.0f, r.getCentreY() });
    g.setColour (brand::track.interpolatedWith (brand::accent, flash));
    g.fillEllipse (dot);

    const juce::String text = juce::String (shownRepairs) + (shownRepairs == 1 ? " click repaired" : " clicks repaired");
    g.setColour (shownRepairs > 0 ? brand::textMain : brand::textDim);
    g.setFont (juce::FontOptions (brand::captionSize));
    g.drawText (text, r.withTrimmedLeft (30.0f).withTrimmedRight (14.0f), juce::Justification::centredLeft);
}

void VODeclickerEditor::resized()
{
    auto area = getLocalBounds().reduced (brand::margin);
    area.removeFromTop (brand::headerHeight);

    footer.setBounds (area.removeFromBottom (brand::footerHeight));
    area.removeFromBottom (10);

    auto pills = area.removeFromBottom (36);
    const int pillW = 120, gap = 12;
    auto row = pills.withSizeKeepingCentre (3 * pillW + 2 * gap, 30);
    strongButton.setBounds (row.removeFromLeft (pillW));
    row.removeFromLeft (gap);
    listenButton.setBounds (row.removeFromLeft (pillW));
    row.removeFromLeft (gap);
    bypassButton.setBounds (row.removeFromLeft (pillW));
    area.removeFromBottom (12);

    counterArea = area.removeFromBottom (26).withSizeKeepingCentre (190, 26);
    area.removeFromBottom (8);

    area.removeFromTop (8);
    const int colW = area.getWidth() / 3;
    juce::Slider* knobs[] = { &sensitivityKnob, &frequencyKnob, &lengthKnob };
    juce::Label* labels[] = { &sensitivityLabel, &frequencyLabel, &lengthLabel };
    for (int i = 0; i < 3; ++i)
    {
        auto col = area.removeFromLeft (colW);
        labels[i]->setBounds (col.removeFromTop (20));
        knobs[i]->setBounds (col.withSizeKeepingCentre (std::min (col.getWidth(), col.getHeight()), col.getHeight()));
    }
}
