#include "PluginEditor.h"
#include <BinaryData.h>

WingsEditor::WingsEditor (WingsProcessor& p) : juce::AudioProcessorEditor (&p), proc (p)
{
    using Opt = juce::WebBrowserComponent::Options;
    Opt opts = Opt{};
#if JUCE_WINDOWS
    opts = opts.withBackend (Opt::Backend::webview2)
               .withWinWebView2Options (Opt::WinWebView2{}
                                            .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("wings_v1_webview"))
                                            .withBackgroundColour (juce::Colours::black));
#endif
    opts = opts.withNativeIntegrationEnabled()
               .withResourceProvider ([this] (const juce::String& url) { return getResource (url); })
               .withEventListener ("wings", [this] (juce::var v) { onEvent (v); });

    web = std::make_unique<juce::WebBrowserComponent> (opts);
    addAndMakeVisible (*web);
    setResizable (true, true);
    getConstrainer()->setFixedAspectRatio (900.0 / 800.0);
    setResizeLimits (600, 533, 1800, 1600);
    setSize (900, 800);
    web->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
}

WingsEditor::~WingsEditor() = default;

void WingsEditor::resized()
{
    if (web != nullptr) web->setBounds (getLocalBounds());
}

std::optional<juce::WebBrowserComponent::Resource> WingsEditor::getResource (const juce::String& url)
{
    const auto path = url == "/" || url.isEmpty() ? juce::String ("/index.html") : url;
    if (path == "/index.html")
    {
        std::vector<std::byte> data ((size_t) BinaryData::index_htmlSize);
        std::memcpy (data.data(), BinaryData::index_html, (size_t) BinaryData::index_htmlSize);
        return juce::WebBrowserComponent::Resource { std::move (data), juce::String ("text/html") };
    }
    return std::nullopt;
}

void WingsEditor::onEvent (const juce::var& v)
{
    auto& E = proc.engine;
    const auto t = v.getProperty ("t", juce::var()).toString();
    auto num = [&] (const char* k) { return (float) (double) v.getProperty (k, juce::var (0.0)); };
    if (t == "n")
    {
        wings::Event e; e.type = 1; e.a = num ("m"); e.b = num ("v"); e.c = num ("a"); e.d = num ("l"); e.e = num ("d"); E.push (e);
    }
    else if (t == "g")
    {
        wings::Event e; e.type = 2; e.a = num ("s"); e.b = num ("w"); e.mask = (int) num ("fx"); E.push (e);
    }
    else if (t == "rs") { wings::Event e; e.type = 3; E.push (e); }
    else if (t == "re") { wings::Event e; e.type = 4; E.push (e); }
    else if (t == "d") { wings::Event e; e.type = 5; e.a = num ("s"); E.push (e); }
    else if (t == "p")
    {
        auto& P = E.params;
        P.vol = num ("vol"); P.rev = num ("rev"); P.fb = num ("fb"); P.fbT = num ("fbT"); P.pitch = num ("pitch");
        P.olap = num ("olap"); P.curve = num ("curve"); P.slw = num ("slw"); P.pulse = num ("pulse"); P.pulseW = num ("pulseW"); P.res = num ("res");
        if (auto* arr = v.getProperty ("n", juce::var()).getArray())
            for (int i = 0; i < 5 && i < arr->size(); ++i) P.n[(size_t) i] = (float) (double) (*arr)[i];
    }
}
