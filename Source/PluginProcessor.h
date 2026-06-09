#pragma once

#include <JuceHeader.h>

/**
 * Reignited
 *
 * 「あの時の熱や勢いを取り戻す…かもしれない」エフェクター。
 * 4バンドEQ + 5つ目の "Reignited" ノブで、EQから本格的なキャラクターエフェクターへ性格が変化する。
 *
 * このC++実装は、WebのGrok + Cmajor で作っている本家プラグインの「参考実装」としてここに置く。
 * 仕様がまだ流動的なので、まず「気持ちいい方向」に動くものを作ってから調整していく。
 */
class ReignitedAudioProcessor : public juce::AudioProcessor
{
public:
    ReignitedAudioProcessor();
    ~ReignitedAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

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

    // APVTS for parameters + easy future GUI binding
    juce::AudioProcessorValueTreeState apvts;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    // Parameters (0..1 normalized in host, mapped inside)
    juce::AudioParameterFloat* lowParam     = nullptr;  // Low band gain
    juce::AudioParameterFloat* midParam     = nullptr;
    juce::AudioParameterFloat* highParam    = nullptr;
    juce::AudioParameterFloat* presenceParam= nullptr;
    juce::AudioParameterFloat* reignitedParam = nullptr; // THE knob

    // New parameters
    juce::AudioParameterChoice* modeParam = nullptr;       // Guitar / Bass / Mastering
    juce::AudioParameterFloat* outputParam = nullptr;      // Output volume
    juce::AudioParameterBool* oversamplingParam = nullptr; // Oversampling mode

    // Smoothed values (to avoid clicks)
    juce::SmoothedValue<float> lowGainSm, midGainSm, highGainSm, presenceGainSm;
    juce::SmoothedValue<float> reignitedSm;
    juce::SmoothedValue<float> outputSm;

    // 4-band EQ filters (serial). We keep separate instances for L and R for correct stereo imaging.
    using Filter = juce::dsp::IIR::Filter<float>;
    using Coefficients = juce::dsp::IIR::Coefficients<float>;

    std::unique_ptr<Filter> lowFilterL, midFilterL, highFilterL, presenceFilterL;
    std::unique_ptr<Filter> lowFilterR, midFilterR, highFilterR, presenceFilterR;

    // Simple "SS character" engine state (glue envelope) — linked for stereo glue feel
    float glueEnv = 0.0f;

    // Oversampling for the saturation stage (reduces aliasing when drive is high)
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;

    // Current sample rate
    double currentSampleRate = 44100.0;

    // Time-based glue coefficients (for ms-based Long Glue)
    float attackCoeff = 0.0f;
    float releaseCoeff = 0.0f;

    // Lookahead buffers for dynamic saturation movement (per channel for correct stereo)
    // Time-based (fixed ms) for high sample rate compatibility.
    std::vector<float> laBufferL;
    std::vector<float> laBufferR;
    int laIndexL = 0;
    int laIndexR = 0;
    float laPeakSmL = 0.0f;
    float laPeakSmR = 0.0f;
    int currentLaSamples = 0;

    // Dry delay lines (per channel) to align dry with the delayed character wet in the final mix.
    // This eliminates the comb-filter / reverb-like artifact when turning EQ knobs.
    std::vector<float> dryDelayL;
    std::vector<float> dryDelayR;
    int dryDelayPos = 0;

    // Helper to update EQ coefficients from smoothed gains + reignited influence (mode selects Guitar/Bass/Mastering ranges)
    void updateEQFilters (float lowDB, float midDB, float highDB, float presDB, float reignited, int mode);

    // The core "Reignited" character processor (saturation + glue + magic)
    // dynDrive is pre-computed from lookahead peak (per channel in caller)
    float applyReignitedCharacter (float input, float dynDrive, float glueAmt, float r, float bandEmphasis);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReignitedAudioProcessor)
};
