#include "PluginProcessor.h"

//==============================================================================
// Parameter IDs (nice for state and future GUI)
namespace ParamIDs
{
    static constexpr auto low      = "low";
    static constexpr auto mid      = "mid";
    static constexpr auto high     = "high";
    static constexpr auto presence = "presence";
    static constexpr auto reignited = "reignited";
}

//==============================================================================
ReignitedAudioProcessor::ReignitedAudioProcessor()
    : AudioProcessor (BusesProperties()
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    lowParam      = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(ParamIDs::low));
    midParam      = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(ParamIDs::mid));
    highParam     = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(ParamIDs::high));
    presenceParam = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(ParamIDs::presence));
    reignitedParam= dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(ParamIDs::reignited));

    modeParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("mode"));
    outputParam = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("output"));
    oversamplingParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("oversampling"));

    jassert (lowParam && midParam && highParam && presenceParam && reignitedParam);
    jassert (modeParam && outputParam && oversamplingParam);

    // Prepare smoothers (fast enough for musical use, ~5-10ms)
    lowGainSm.reset(100);
    midGainSm.reset(100);
    highGainSm.reset(100);
    presenceGainSm.reset(100);
    reignitedSm.reset(60); // a bit slower for the main knob feel
}

ReignitedAudioProcessor::~ReignitedAudioProcessor() = default;

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout ReignitedAudioProcessor::createParameterLayout()
{
    using namespace juce;
    using Range = NormalisableRange<float>;

    // Gain params: 0.0 = -15dB, 0.5 = 0dB, 1.0 = +12dB (musical range)
    auto gainRange = Range { 0.0f, 1.0f, 0.001f };
    gainRange.setSkewForCentre (0.5f); // more resolution around 0dB

    // Reignited: 0 = clean-ish EQ, 1 = full effector mode
    // We will curve the internal response (slow at first, then steep after ~0.55-0.6)
    auto reignitedRange = Range { 0.0f, 1.0f, 0.001f };

    // New params
    juce::StringArray modeChoices { "Guitar", "Bass", "Mastering" };

    // Output: -12dB to +6dB
    auto outputRange = Range { 0.0f, 1.0f, 0.001f };
    outputRange.setSkewForCentre (0.5f);

    return {
        std::make_unique<AudioParameterFloat> (ParamIDs::low,      "Low",      gainRange, 0.5f,
            AudioParameterFloatAttributes().withLabel ("dB")),
        std::make_unique<AudioParameterFloat> (ParamIDs::mid,      "Mid",      gainRange, 0.5f,
            AudioParameterFloatAttributes().withLabel ("dB")),
        std::make_unique<AudioParameterFloat> (ParamIDs::high,     "High",     gainRange, 0.5f,
            AudioParameterFloatAttributes().withLabel ("dB")),
        std::make_unique<AudioParameterFloat> (ParamIDs::presence, "Presence", gainRange, 0.5f,
            AudioParameterFloatAttributes().withLabel ("dB")),
        std::make_unique<AudioParameterFloat> (ParamIDs::reignited, "Reignited", reignitedRange, 0.0f,
            AudioParameterFloatAttributes().withLabel ("%")),

        // New
        std::make_unique<AudioParameterChoice> ("mode", "Mode", modeChoices, 0),
        std::make_unique<AudioParameterFloat> ("output", "Output", outputRange, 0.5f,
            AudioParameterFloatAttributes().withLabel ("dB")),
        std::make_unique<AudioParameterBool> ("oversampling", "Oversampling", true)
    };
}

//==============================================================================
void ReignitedAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    // Reset smoothers with actual sample rate
    const double sr = sampleRate;
    lowGainSm.reset (sr, 0.008);
    midGainSm.reset (sr, 0.008);
    highGainSm.reset (sr, 0.008);
    presenceGainSm.reset (sr, 0.008);
    reignitedSm.reset (sr, 0.012);
    outputSm.reset (sr, 0.05); // slow for master output volume

    // Oversampling for non-linear stage (user selectable)
    oversampler = std::make_unique<juce::dsp::Oversampling<float>> (2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR); // 4x for better quality
    oversampler->initProcessing ((size_t) samplesPerBlock);

    // Create fresh filters (L + R)
    lowFilterL      = std::make_unique<Filter>();
    midFilterL      = std::make_unique<Filter>();
    highFilterL     = std::make_unique<Filter>();
    presenceFilterL = std::make_unique<Filter>();

    lowFilterR      = std::make_unique<Filter>();
    midFilterR      = std::make_unique<Filter>();
    highFilterR     = std::make_unique<Filter>();
    presenceFilterR = std::make_unique<Filter>();

    glueEnv = 0.0f;

    // Time-based glue (ms) for any sample rate (replaces old sample-based 16 sample lookahead)
    float attackMs  = 5.0f;
    float releaseMs = 200.0f; // long glue
    attackCoeff  = 1.0f - std::exp (-1.0f / (attackMs  * 0.001f * sr));
    releaseCoeff = 1.0f - std::exp (-1.0f / (releaseMs * 0.001f * sr));

    // Prime the filters with current params (Guitar mode)
    updateEQFilters (0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0);
}

void ReignitedAudioProcessor::releaseResources()
{
    oversampler.reset();
}

//==============================================================================
bool ReignitedAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Only stereo or mono->stereo for now
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo() &&
        layouts.getMainInputChannelSet() != juce::AudioChannelSet::mono())
        return false;

    return true;
}

//==============================================================================
void ReignitedAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    // Get raw param values
    const float lowRaw   = lowParam->get();
    const float midRaw   = midParam->get();
    const float highRaw  = highParam->get();
    const float presRaw  = presenceParam->get();
    const float reignRaw = reignitedParam->get();
    const float outputRaw = outputParam ? outputParam->get() : 0.5f;
    const bool useOS = oversamplingParam ? oversamplingParam->get() : true;

    // Smooth them
    lowGainSm.setTargetValue (lowRaw);
    midGainSm.setTargetValue (midRaw);
    highGainSm.setTargetValue (highRaw);
    presenceGainSm.setTargetValue (presRaw);
    reignitedSm.setTargetValue (reignRaw);

    float outputDB = juce::jmap (outputRaw, 0.0f, 1.0f, -12.0f, 6.0f);
    outputSm.setTargetValue (outputDB);

    // Update filters (block rate) with current mode
    int currentMode = modeParam ? modeParam->getIndex() : 0;
    updateEQFilters (lowGainSm.getCurrentValue(),
                     midGainSm.getCurrentValue(),
                     highGainSm.getCurrentValue(),
                     presenceGainSm.getCurrentValue(),
                     reignitedSm.getCurrentValue(),
                     currentMode);

    // --- Main processing ---
    // Save dry (original input) and post-EQ wet for later character + mix + output
    juce::AudioBuffer<float> dryBuffer (numChannels, numSamples);
    juce::AudioBuffer<float> wetBuffer (numChannels, numSamples);

    std::vector<float> reiSamples (numSamples);
    std::vector<float> emphSamples (numSamples);
    std::vector<float> mixSamples (numSamples);
    std::vector<float> outGainSamples (numSamples);

    auto* left  = buffer.getWritePointer (0);
    auto* right = (numChannels > 1) ? buffer.getWritePointer (1) : nullptr;

    const float mixBase = juce::jmap (reignitedSm.getCurrentValue(), 0.0f, 1.0f, 0.0f, 0.92f);

    for (int i = 0; i < numSamples; ++i)
    {
        // Advance ALL smoothers every sample
        const float lG [[maybe_unused]] = lowGainSm.getNextValue();
        const float mG = midGainSm.getNextValue();
        const float hG [[maybe_unused]] = highGainSm.getNextValue();
        const float pG [[maybe_unused]] = presenceGainSm.getNextValue();
        const float rei = reignitedSm.getNextValue();
        const float outDBThis = outputSm.getNextValue();

        float dryL = left[i];
        float dryR = right ? right[i] : dryL;

        dryBuffer.setSample (0, i, dryL);
        if (right) dryBuffer.setSample (1, i, dryR);

        // EQ stage (serial)
        float postL = lowFilterL->processSample (dryL);
        postL = midFilterL->processSample (postL);
        postL = highFilterL->processSample (postL);
        postL = presenceFilterL->processSample (postL);

        float postR = dryR;
        if (right)
        {
            postR = lowFilterR->processSample (dryR);
            postR = midFilterR->processSample (postR);
            postR = highFilterR->processSample (postR);
            postR = presenceFilterR->processSample (postR);
        }
        else
        {
            postR = postL;
        }

        wetBuffer.setSample (0, i, postL);
        if (right) wetBuffer.setSample (1, i, postR);

        // Save per-sample values for character / mix / output
        reiSamples[i] = rei;
        emphSamples[i] = 1.0f + (mG - 0.5f) * 0.6f;
        const float mix = juce::jlimit (0.0f, 0.98f, mixBase + (rei - 0.5f) * 0.04f);
        mixSamples[i] = mix;
        outGainSamples[i] = juce::Decibels::decibelsToGain (outDBThis);
    }

    // --- Reignited character engine (with optional oversampling) ---
    float blockRei = reignitedSm.getCurrentValue();
    float blockMG = midGainSm.getCurrentValue();
    float blockEmph = 1.0f + (blockMG - 0.5f) * 0.6f;

    if (useOS && oversampler)
    {
        juce::dsp::AudioBlock<float> wetBlock (wetBuffer);
        auto upBlock = oversampler->processSamplesUp (wetBlock);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* data = upBlock.getChannelPointer (ch);
            const size_t upSamps = upBlock.getNumSamples();
            for (size_t s = 0; s < upSamps; ++s)
            {
                data[s] = applyReignitedCharacter (data[s], blockRei, blockEmph);
            }
        }

        oversampler->processSamplesDown (wetBlock);
    }
    else
    {
        // No OS: per-sample character using saved rei/emph
        for (int i = 0; i < numSamples; ++i)
        {
            float wL = wetBuffer.getSample (0, i);
            wL = applyReignitedCharacter (wL, reiSamples[i], emphSamples[i]);
            wetBuffer.setSample (0, i, wL);

            if (right)
            {
                float wR = wetBuffer.getSample (1, i);
                wR = applyReignitedCharacter (wR, reiSamples[i], emphSamples[i]);
                wetBuffer.setSample (1, i, wR);
            }
        }
    }

    // --- Parallel mix + Output volume ---
    for (int i = 0; i < numSamples; ++i)
    {
        float dryL = dryBuffer.getSample (0, i);
        float procL = wetBuffer.getSample (0, i);
        left[i] = dryL + (procL - dryL) * mixSamples[i];
        left[i] *= outGainSamples[i];

        if (right)
        {
            float dryR = dryBuffer.getSample (1, i);
            float procR = wetBuffer.getSample (1, i);
            right[i] = dryR + (procR - dryR) * mixSamples[i];
            right[i] *= outGainSamples[i];
        }
    }

    // Safety limiter for extreme settings
    if (reignitedSm.getCurrentValue() > 0.85f)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                d[i] = juce::jlimit (-1.5f, 1.5f, d[i]);
        }
    }
}

//==============================================================================
void ReignitedAudioProcessor::updateEQFilters (float low01, float mid01, float high01, float pres01, float reignited01, int mode)
{
    const double sr = currentSampleRate;
    if (sr <= 0.0) return;

    auto toDB = [](float x) { return juce::jmap (x, 0.0f, 1.0f, -15.0f, 12.0f); };

    float lowDB   = toDB (low01);
    float midDB   = toDB (mid01);
    float highDB  = toDB (high01);
    float presDB  = toDB (pres01);

    // === "EQ changes with Reignited" magic ===
    // Made significantly more aggressive for "全体的にもっと派手" feedback.
    const float r = reignited01;

    // Mid scoop (stronger and starts earlier)
    const float midScoop = juce::jmax (0.0f, (r - 0.38f) * 1.6f) * 12.0f; // up to ~ -14dB extra cut
    midDB -= midScoop;

    // Extra low tighten
    const float lowTight = juce::jmax (0.0f, (r - 0.48f) * 1.1f) * 7.0f;
    lowDB -= lowTight;

    // Presence/air boost
    const float airBoost = juce::jmax (0.0f, (r - 0.52f) * 1.3f) * 9.0f;
    presDB += airBoost;

    // High shelf extra bite
    highDB += juce::jmax (0.0f, (r - 0.55f) * 0.9f) * 5.5f;

    // === Filter coefficients based on MODE (Guitar / Bass / Mastering) ===
    // Guitar: current (guitar/vocal focused)
    // Bass: lower frequencies typical for bass guitar
    // Mastering: balanced 4-band mastering EQ ranges
    float lowBase = 140.0f, midBase = 620.0f, highBase = 2800.0f, presBase = 5800.0f;
    float midQ = 0.85f, presQ = 1.35f;

    switch (mode)
    {
        case 1: // Bass mode
            lowBase = 60.0f;
            midBase = 250.0f;
            highBase = 850.0f;
            presBase = 3200.0f;
            midQ = 0.75f;
            presQ = 1.0f;
            break;
        case 2: // Mastering mode (nice 4-band mastering ranges)
            lowBase = 90.0f;
            midBase = 420.0f;
            highBase = 1600.0f;
            presBase = 5200.0f;
            midQ = 0.65f;
            presQ = 0.9f;
            break;
        default: // Guitar (0)
            break;
    }

    const float gL = juce::Decibels::decibelsToGain (lowDB);
    const float gM = juce::Decibels::decibelsToGain (midDB);
    const float gH = juce::Decibels::decibelsToGain (highDB);
    const float gP = juce::Decibels::decibelsToGain (presDB);

    // Low: LowShelf
    lowFilterL->coefficients = juce::dsp::IIR::Coefficients<float>::makeLowShelf (sr, lowBase, 0.7f, gL);
    lowFilterR->coefficients = juce::dsp::IIR::Coefficients<float>::makeLowShelf (sr, lowBase, 0.7f, gL);

    // Mid: Peak (Bell)
    midFilterL->coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sr, midBase, midQ, gM);
    midFilterR->coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sr, midBase, midQ, gM);

    // High: HighShelf
    highFilterL->coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, highBase, 0.6f, gH);
    highFilterR->coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, highBase, 0.6f, gH);

    // Presence: Peak (Bell)
    presenceFilterL->coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sr, presBase, presQ, gP);
    presenceFilterR->coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sr, presBase, presQ, gP);
}

//==============================================================================
float ReignitedAudioProcessor::applyReignitedCharacter (float input, float reignited, float bandEmphasis)
{
    if (reignited < 0.001f)
        return input; // completely bypass the character engine when knob is at 0

    // =======================================================================
    // === Reignited knob response curve (spec: slow at first, steep after ~70%)
    // This mapping is critical for the "EQ → エフェクター" personality shift.
    // =======================================================================
    float r = reignited;

    // Piecewise curve: flatter in the low-mid range, then accelerates hard.
    // Made steeper overall for "もっと派手" request.
    constexpr float knee          = 0.50f;
    constexpr float lowScale      = 0.65f;
    constexpr float highOffset    = 0.35f;
    constexpr float highSteepness = 1.95f;

    if (r < knee)
        r = r * lowScale;
    else
        r = highOffset + (r - knee) * highSteepness;

    // =======================================================================
    // Amount mapping (these are the "SS strength" and "glue" controls)
    // =======================================================================
    // Increased ranges for more dramatic "派手" character as per feedback.
    constexpr float minDrive   = 0.7f;
    constexpr float maxDrive   = 7.5f;
    constexpr float minGlue    = 0.0f;
    constexpr float maxGlue    = 0.95f;

    const float drive   = juce::jmap (r, 0.0f, 1.0f, minDrive, maxDrive);   // main saturation drive
    const float glueAmt = juce::jmap (r, 0.0f, 1.0f, minGlue,  maxGlue);     // "Long Glue" amount

    // Per-band push: stronger with "派手" tuning.
    const float effectiveDrive = drive * (0.85f + bandEmphasis * 0.7f);

    // -----------------------------------------------------------------------
    // Saturation stage (SS2 / "気持ちいい" character approximation)
    // -----------------------------------------------------------------------
    float x = input * effectiveDrive;

    // Stronger asymmetrical saturation + bloom for more dramatic effect.
    float sat = std::tanh (x * 0.88f) * 1.05f;
    sat += 0.18f * (x * x * (x > 0.0f ? 1.0f : -0.55f));   // boosted even harmonic bloom

    // Less compensation so it gets dirtier/fatter when drive is high
    float shaped = sat * (1.0f / juce::jmax (0.55f, effectiveDrive * 0.55f));

    // -----------------------------------------------------------------------
    // "Long Glue" / bus glue stage
    // Very slow envelope follower + gentle gain reduction.
    // This is the part that "まとめる" the energy and gives cohesion.
    // Now uses ms-based coefficients (set in prepareToPlay) for any sample rate.
    // -----------------------------------------------------------------------
    const float absIn = std::abs (shaped);

    if (absIn > glueEnv)
        glueEnv = glueEnv * (1.0f - attackCoeff)  + absIn * attackCoeff;
    else
        glueEnv = glueEnv * (1.0f - releaseCoeff) + absIn * releaseCoeff;

    // Stronger glue reaction (more obvious "まとまり" when Reignited is high)
    constexpr float glueThresh = 0.68f;
    float gr = 1.0f;

    if (glueEnv > glueThresh && glueAmt > 0.01f)
    {
        const float over = (glueEnv - glueThresh) * (1.8f + glueAmt * 2.2f);
        gr = 1.0f / (1.0f + over * glueAmt * 1.55f);
        gr = juce::jlimit (0.55f, 1.0f, gr);
    }

    float glued = shaped * gr;

    // -----------------------------------------------------------------------
    // Sparkle / "キラキラ" air — boosted for more dramatic high-end excitement.
    // -----------------------------------------------------------------------
    const float sparkle = juce::jmap (r, 0.0f, 1.0f, 0.0f, 0.28f);
    glued = glued * (1.0f + sparkle) - sparkle * 0.35f * glued;

    // -----------------------------------------------------------------------
    // Final parallel blend — reaches "full effector" faster and harder.
    // -----------------------------------------------------------------------
    const float charMix = juce::jmap (r, 0.0f, 1.0f, 0.12f, 0.99f);
    return input * (1.0f - charMix) + glued * charMix;
}

//==============================================================================
juce::AudioProcessorEditor* ReignitedAudioProcessor::createEditor()
{
    // For rapid prototyping we use JUCE's built-in generic editor.
    // This shows all 5 parameters as sliders + labels immediately.
    // Later we can replace with a custom look (knobs that feel "ヤバい").
    return new juce::GenericAudioProcessorEditor (*this);
}

//==============================================================================
void ReignitedAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void ReignitedAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState.get() != nullptr)
        if (xmlState->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
}

//==============================================================================
// This creates new instances of the plugin
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ReignitedAudioProcessor();
}
