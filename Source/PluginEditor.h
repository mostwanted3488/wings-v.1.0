#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"

class WingsEditor : public juce::AudioProcessorEditor
{
public:
    explicit WingsEditor (WingsProcessor&);
    ~WingsEditor() override;
    void resized() override;

private:
    void onEvent (const juce::var& v);
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);

    WingsProcessor& proc;
    std::unique_ptr<juce::WebBrowserComponent> web;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WingsEditor)
};
