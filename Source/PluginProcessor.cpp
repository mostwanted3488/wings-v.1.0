#include "PluginProcessor.h"
#include "PluginEditor.h"

WingsProcessor::WingsProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

void WingsProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate);
}

bool WingsProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void WingsProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn()) engine.noteOnMidi (m.getNoteNumber(), m.getFloatVelocity());
    }
    const int n = buffer.getNumSamples();
    if (buffer.getNumChannels() >= 2)
        engine.render (buffer.getWritePointer (0), buffer.getWritePointer (1), n);
}

juce::AudioProcessorEditor* WingsProcessor::createEditor()
{
    return new WingsEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new WingsProcessor();
}
