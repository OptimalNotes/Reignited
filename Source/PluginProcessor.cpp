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

    jassert (lowParam && midParam && highParam && presenceParam && reignitedParam);

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
            AudioParameterFloatAttributes().withLabel ("%"))
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

    // Oversampling 2x for the non-linear stage (good enough for prototype)
    oversampler = std::make_unique<juce::dsp::Oversampling<float>> (2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR);
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

    // Prime the filters with current params
    updateEQFilters (0.5f, 0.5f, 0.5f, 0.5f, 0.0f);
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

    // Smooth them
    lowGainSm.setTargetValue (lowRaw);
    midGainSm.setTargetValue (midRaw);
    highGainSm.setTargetValue (highRaw);
    presenceGainSm.setTargetValue (presRaw);
    reignitedSm.setTargetValue (reignRaw);

    // Update filters occasionally (not every sample, but we do it every block for simplicity + safety)
    // In a tighter version we would only update when a param actually changed.
    updateEQFilters (lowGainSm.getCurrentValue(),
                     midGainSm.getCurrentValue(),
                     highGainSm.getCurrentValue(),
                     presenceGainSm.getCurrentValue(),
                     reignitedSm.getCurrentValue());

    // --- Main processing ---
    // We process the "EQ tone" first (serial), then feed into the Reignited character engine.
    // NOTE: We maintain separate filter states for left and right for proper stereo.

    auto* left  = buffer.getWritePointer (0);
    auto* right = (numChannels > 1) ? buffer.getWritePointer (1) : nullptr;

    const float mixBase = juce::jmap (reignitedSm.getCurrentValue(), 0.0f, 1.0f, 0.0f, 0.92f); // more wet as reignited rises

    for (int i = 0; i < numSamples; ++i)
    {
        // Advance smoothers every sample (required for correct smoothing behavior).
        // We only actually *use* mG (for band emphasis) and rei (for character amount) inside the loop.
        const float lG [[maybe_unused]] = lowGainSm.getNextValue();
        const float mG = midGainSm.getNextValue();
        const float hG [[maybe_unused]] = highGainSm.getNextValue();
        const float pG [[maybe_unused]] = presenceGainSm.getNextValue();
        const float rei = reignitedSm.getNextValue();

        // (EQ coefficients are updated at block rate in updateEQFilters using the smoothed targets.
        // We no longer need per-sample dB values here.)

        // We already updated coefficients in updateEQFilters using the *smoothed target*,
        // but for per-sample we re-use the last coefficients (filters are stateful).

        float dryL = left[i];
        float dryR = right ? right[i] : dryL;

        // --- EQ stage (serial, classic "tone" chain) ---
        // Left channel filters
        float wetL = lowFilterL->processSample (dryL);
        wetL = midFilterL->processSample (wetL);
        wetL = highFilterL->processSample (wetL);
        wetL = presenceFilterL->processSample (wetL);

        // Right channel filters (independent state)
        float wetR = dryR;
        if (right)
        {
            wetR = lowFilterR->processSample (dryR);
            wetR = midFilterR->processSample (wetR);
            wetR = highFilterR->processSample (wetR);
            wetR = presenceFilterR->processSample (wetR);
        }
        else
        {
            wetR = wetL;
        }

        // --- Reignited character engine (the fun part) ---
        // We apply slightly different emphasis to L/R for stereo width feel, but keep it simple.
        const float bandEmphasisL = 1.0f + (mG - 0.5f) * 0.6f; // mid gain influences how much "push" into sat
        const float bandEmphasisR = 1.0f + (mG - 0.5f) * 0.6f;

        wetL = applyReignitedCharacter (wetL, rei, bandEmphasisL);
        wetR = applyReignitedCharacter (wetR, rei, bandEmphasisR);

        // Parallel mix (dry tone EQ vs the full Reignited effector sound)
        const float mix = juce::jlimit (0.0f, 0.98f, mixBase + (rei - 0.5f) * 0.04f); // extra wet at high end
        left[i]  = dryL + (wetL - dryL) * mix;
        if (right)
            right[i] = dryR + (wetR - dryR) * mix;
    }

    // Optional: very gentle global limiter at extreme settings (safety)
    if (reignitedSm.getCurrentValue() > 0.85f)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                d[i] = juce::jlimit (-1.5f, 1.5f, d[i]); // soft safety, real limiting would be better
        }
    }
}

//==============================================================================
void ReignitedAudioProcessor::updateEQFilters (float low01, float mid01, float high01, float pres01, float reignited01)
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

    // === Filter coefficients (musical starting points for guitar/vocal "character") ===
    // These numbers are chosen by ear for "気持ちいい" starting point. Tweak freely.
    const float gL = juce::Decibels::decibelsToGain (lowDB);
    const float gM = juce::Decibels::decibelsToGain (midDB);
    const float gH = juce::Decibels::decibelsToGain (highDB);
    const float gP = juce::Decibels::decibelsToGain (presDB);

    // Low: LowShelf around 140Hz
    lowFilterL->coefficients = juce::dsp::IIR::Coefficients<float>::makeLowShelf (sr, 140.0f, 0.7f, gL);
    lowFilterR->coefficients = juce::dsp::IIR::Coefficients<float>::makeLowShelf (sr, 140.0f, 0.7f, gL);

    // Mid: gentle bell around 620Hz (boxy/muddy control + body)
    midFilterL->coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sr, 620.0f, 0.85f, gM);
    midFilterR->coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sr, 620.0f, 0.85f, gM);

    // High: HighShelf ~2.8kHz (bite / edge)
    highFilterL->coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 2800.0f, 0.6f, gH);
    highFilterR->coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 2800.0f, 0.6f, gH);

    // Presence: upper presence / air, 5.8kHz
    presenceFilterL->coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sr, 5800.0f, 1.35f, gP);
    presenceFilterR->coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sr, 5800.0f, 1.35f, gP);
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
    // Attack is relatively fast so transients still poke through a little.
    // Release is deliberately long.
    // -----------------------------------------------------------------------
    const float absIn   = std::abs (shaped);
    constexpr float attack  = 0.0008f;   // ~ a few ms at 44.1k
    constexpr float release = 0.00065f;  // slower → "long" glue character

    if (absIn > glueEnv)
        glueEnv = glueEnv * (1.0f - attack)  + absIn * attack;
    else
        glueEnv = glueEnv * (1.0f - release) + absIn * release;

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
