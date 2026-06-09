cmake_minimum_required(VERSION 3.22)

# Suppress deprecation errors from JUCE on macOS 15+ (CGWindowListCreateImage etc.)
# Must be set early so it affects JUCE's own compilation during FetchContent.
if(APPLE)
  add_compile_options(-Wno-error=unavailable-declarations)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum OS X deployment version" FORCE)
endif()

project(Reignited VERSION 0.1.0)

# JUCE framework (fetched automatically)
include(FetchContent)
FetchContent_Declare(
  juce
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG        8.0.13   # Latest JUCE 8 with full macOS 15+ / ScreenCaptureKit support
)
FetchContent_MakeAvailable(juce)

# Optional: If you have the VST3 SDK locally, you can point to it.
# set(JUCE_VST3_SDK_PATH "C:/path/to/VST3_SDK" CACHE PATH "Path to VST3 SDK")

juce_add_plugin(Reignited
    COMPANY_NAME "Optimal Notes Music lab"
    BUNDLE_ID com.optimalnotes.reignited.time
    PLUGIN_MANUFACTURER_CODE OptN
    PLUGIN_CODE Rgni
    # Now building Standalone + VST3 + AU.
    # AU (Audio Unit) is macOS-only. Build on Mac with Xcode to generate .component.
    FORMATS Standalone VST3 AU
    PRODUCT_NAME "Reignited (Time)"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE   # Set TRUE if you want auto copy to common plugin folder
)

# Source files
target_sources(Reignited
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
)

# JUCE modules we need
target_compile_definitions(Reignited
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_USE_WIN_WEBVIEW2=0
)

# This is the key missing piece for modern JUCE CMake:
# It generates JuceHeader.h (and sets up include paths) so that
# #include <JuceHeader.h> works in your source files.
juce_generate_juce_header(Reignited)

target_link_libraries(Reignited
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_gui_basics   # For GenericAudioProcessorEditor (quick UI for prototype)
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Help with Japanese comments / source encoding on Japanese Windows (code page 932).
# Without this you get C4819 warnings or worse.
target_compile_options(Reignited PRIVATE /utf-8)


# For faster iteration during prototype: use the built-in Generic editor
# (we can add a custom one later when the sound is good)
# If you want a blank custom editor from the start, add PluginEditor.cpp/h and link juce_gui_extra if needed.

# Windows specific niceties
if (WIN32)
    target_compile_definitions(Reignited PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
