# wings v1.0

VST3 (Windows + macOS) and AU (macOS) instrument by **@buyorbealoser**.

A looping dove on a black stage. Press and hold it: the light rises, matter flies everywhere, a riser builds. Hold until the ring completes and release for the drop. Every action plays generative notes through a built-in synth, feedback delay, reverb and a glitch/SFX engine.

- Audio engine: native C++ (JUCE)
- Interface: embedded web UI (WebView2 on Windows, WKWebView on macOS)
- Also reacts to incoming MIDI notes

## Install
Download the zip for your system from the **Releases** page.

**Windows:** unzip, copy `wings v1.0.vst3` to `C:\Program Files\Common Files\VST3\`. Needs the Microsoft WebView2 runtime (already included in Windows 10/11).

**macOS:** unzip, copy `wings v1.0.vst3` to `/Library/Audio/Plug-Ins/VST3/` and `wings v1.0.component` to `/Library/Audio/Plug-Ins/Components/`. The plugin is not notarized: run `xattr -cr "<path to plugin>"` once if macOS blocks it.

## Build
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release`
CI builds both platforms on every push (see `.github/workflows/build.yml`).
