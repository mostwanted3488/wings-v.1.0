cmake_minimum_required(VERSION 3.22)
project(WingsPlugin VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.4
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

set(WINEXTRA "")
if(WIN32)
  set(WINEXTRA NEEDS_WEBVIEW2 TRUE)
endif()
set(FORMATS VST3)
if(APPLE)
  list(APPEND FORMATS AU)
endif()

juce_add_plugin(Wings
  COMPANY_NAME "BUYORBEALOSER"
  PRODUCT_NAME "wings v1.0"
  PLUGIN_MANUFACTURER_CODE Bobl
  PLUGIN_CODE Wng1
  FORMATS ${FORMATS}
  IS_SYNTH TRUE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS TRUE
  VST3_CATEGORIES Instrument Synth
  AU_MAIN_TYPE kAudioUnitType_MusicDevice
  COPY_PLUGIN_AFTER_BUILD FALSE
  ${WINEXTRA})

juce_add_binary_data(WingsData SOURCES web/index.html)

target_sources(Wings PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)

target_compile_definitions(Wings PUBLIC
  JUCE_WEB_BROWSER=1
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_DISPLAY_SPLASH_SCREEN=0
  JUCE_REPORT_APP_USAGE=0)
if(WIN32)
  target_compile_definitions(Wings PUBLIC JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1)
endif()

target_link_libraries(Wings PRIVATE
  WingsData
  juce::juce_audio_utils
  juce::juce_gui_extra
  PUBLIC
  juce::juce_recommended_config_flags
  juce::juce_recommended_lto_flags)
