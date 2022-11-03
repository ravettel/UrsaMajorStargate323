/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
NewProjectAudioProcessor::NewProjectAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
#endif
{
}

NewProjectAudioProcessor::~NewProjectAudioProcessor()
{
}

//==============================================================================
const juce::String NewProjectAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool NewProjectAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool NewProjectAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool NewProjectAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double NewProjectAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int NewProjectAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int NewProjectAudioProcessor::getCurrentProgram()
{
    return 0;
}

void NewProjectAudioProcessor::setCurrentProgram (int index)
{
}

const juce::String NewProjectAudioProcessor::getProgramName (int index)
{
    return {};
}

void NewProjectAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

//==============================================================================
void NewProjectAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    //initialize delay buffer
    const int numInputChannels = 1;
    const int delayBufferSize = 16384;
    mDelayBuffer.setSize(numInputChannels, delayBufferSize);
    // clear delaybuffer to prevent feedback / NaNs
    for (auto i = 0; i < numInputChannels; ++i)
        mDelayBuffer.clear(i, 0, delayBufferSize);
    //set up filters
    lastSampleRate = sampleRate;
    halfSampleRate = sampleRate * 0.5f;
    s1b0 = 1.0f;
    s1b1 = 1.224283695220947265625f;
    s1b2 = 1.0f;
    s1a0 = 1.0f;
    s1a1 = 0.4837161600589752197265625f;
    s1a2 = 0.717677175998687744140625f;
    s2b0 = 1.0f;
    s2b1 = 1.828480243682861328125f;
    s2b2 = 1.0f;
    s2a0 = 1.0f;
    s2a1 = -0.2016279697418212890625f;
    s2a2 = 0.19512404501438140869140625f;
    s3b0 = 1.0f;
    s3b1 = 0.982987344264984130859375f;
    s3b2 = 1.0f;
    s3a0 = 1.0f;
    s3a1 = 0.7548847198486328125f;
    s3a2 = 0.942220151424407958984375f;
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = samplesPerBlock;
    spec.numChannels = getTotalNumOutputChannels();
    inputHighPass.prepare(spec);
    inputHighPass.reset();
    inputLowPass.prepare(spec);
    inputLowPass.reset();
    preEmphasis.prepare(spec);
    preEmphasis.reset();
    deEmphasis.prepare(spec);
    deEmphasis.reset();
    feedBackHighPass.prepare(spec);
    feedBackHighPass.reset();
    feedBackLowPass.prepare(spec);
    feedBackLowPass.reset();
    feedBackDip.prepare(spec);
    feedBackDip.reset();
    antiAliasFirstStage.prepare(spec);
    antiAliasFirstStage.reset();
    antiAliasSecondStage.prepare(spec);
    antiAliasSecondStage.reset();
    antiAliasThirdStage.prepare(spec);
    antiAliasThirdStage.reset();
    gainModule.prepare(spec);
}

void NewProjectAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool NewProjectAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

int calculateAddress(unsigned short rowInput, unsigned short columnInput)
{
    // calculate address row
    unsigned short result = rowInput;
    uint8_t bit6 = result << 1;
    bit6 = bit6 >> 7;
    uint8_t MSB = result;
    MSB = MSB >> 7;
    uint8_t delayCarryOut = result >> 8;
    uint8_t rowDelay = result << 2;
    rowDelay = (rowDelay >> 1) | bit6 | (MSB << 7);
    // calculate address column
    result = columnInput + delayCarryOut;
    uint8_t columnDelay = result << 2;
    columnDelay = columnDelay >> 2;
    return (rowDelay)+(columnDelay * 256);
}

int countWriteAddress(short writeAddress)
{
    // advance write address & wraparound if < 0
    short writeAddressIncr = writeAddress - 1;
    if (writeAddressIncr < 0)
    {
        writeAddressIncr = 16383;
    }
    return writeAddressIncr;
}

float roundBits(float inputSample) {
    short roundedSample = inputSample * 32768;
    float outputSample = roundedSample * 0.000030518;
    return (outputSample);
}

void NewProjectAudioProcessor::updateFilter()
{
    *inputHighPass.state = *juce::dsp::IIR::Coefficients<float>::makeFirstOrderHighPass(lastSampleRate, 14.0f);
    *inputLowPass.state = *juce::dsp::IIR::Coefficients<float>::makeFirstOrderLowPass(lastSampleRate, halfSampleRate);
    *preEmphasis.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(lastSampleRate, 4000.0f, 0.5f, 4.0f);
    *deEmphasis.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(lastSampleRate, 4000.0f, 0.5f, 0.25f);
    *feedBackHighPass.state = *juce::dsp::IIR::Coefficients<float>::makeFirstOrderHighPass(lastSampleRate, 22.0f);
    *feedBackLowPass.state = *juce::dsp::IIR::Coefficients<float>::makeFirstOrderLowPass(lastSampleRate, 18000.0f);
    *feedBackDip.state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(lastSampleRate, 9000.0f, 0.5f, 0.85f);
    *antiAliasFirstStage.state = juce::dsp::IIR::Coefficients<float>::Coefficients(s1b0,s1b1,s1b2,s1a0,s1a1,s1a2);
    *antiAliasSecondStage.state = juce::dsp::IIR::Coefficients<float>::Coefficients(s2b0, s2b1, s2b2, s2a0, s2a1, s2a2);
    *antiAliasThirdStage.state = juce::dsp::IIR::Coefficients<float>::Coefficients(s3b0, s3b1, s3b2, s3a0, s3a1, s3a2);
}

void NewProjectAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // In case we have more outputs than inputs, this code clears any output
    // channels that didn't contain input data, (because these aren't
    // guaranteed to be empty - they may contain garbage).
    // This is here to avoid people getting screaming feedback
    // when they first compile a plugin, but obviously you don't need to keep
    // this code if your algorithm always overwrites all the output channels.
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    mInputBuffer.setSize(1, buffer.getNumSamples());
    mFeedbackBuffer.setSize(1, buffer.getNumSamples());
    if (mSampleRateCount == 0)
    {
        mFeedbackBuffer.clear(0, 0, buffer.getNumSamples());
        mSampleRateCount = 1;
    }

    mOutputBuffer.setSize(getTotalNumInputChannels(), buffer.getNumSamples());
    
    //main loop
    for (int channel = totalNumInputChannels - 1; channel >= 0; --channel)
    {
        const int bufferLength = buffer.getNumSamples();
        const int numInputChannels = getTotalNumInputChannels();
        const int delayBufferLength = mDelayBuffer.getNumSamples();

        mInputGain = 1.0f / totalNumInputChannels; //Adjust input buffer gain
        mInputBuffer.addFrom(0, 0, buffer, channel, 0, bufferLength, mInputGain); //sum input channels together
        if (channel == 0) //only process delay calculation on the last channel
        { 
            juce::dsp::AudioBlock <float> feedbackBlock(mFeedbackBuffer);//assign feedbackbuffer as audioblock
            juce::dsp::AudioBlock <float> inputBlock(mInputBuffer); //assign inputbuffer as audioblock
            juce::dsp::AudioBlock <float> outputBlock(mOutputBuffer); //assign outputbuffer as audioblock
            updateFilter();
            feedBackHighPass.process(juce::dsp::ProcessContextReplacing <float>(feedbackBlock));
            feedBackLowPass.process(juce::dsp::ProcessContextReplacing <float>(feedbackBlock));
            feedBackDip.process(juce::dsp::ProcessContextReplacing <float>(feedbackBlock));

            mInputBuffer.addFrom(0, 0, mFeedbackBuffer, 0, 0, bufferLength, -1.0f); //sum inputbuffer & feedbackbuffer
            mFeedbackBuffer.clear(0, 0, bufferLength); //clear feedbackbuffer

            preEmphasis.process(juce::dsp::ProcessContextReplacing <float>(inputBlock)); //preEmphasis
            inputHighPass.process(juce::dsp::ProcessContextReplacing <float>(inputBlock));
            inputLowPass.process(juce::dsp::ProcessContextReplacing <float>(inputBlock));
            //elliptical anti aliasing filter @ 48khz
            gainModule.setGainLinear(1.16304862499237060546875f);
            gainModule.process(juce::dsp::ProcessContextReplacing <float>(inputBlock));
            antiAliasFirstStage.process(juce::dsp::ProcessContextReplacing <float>(inputBlock));
            gainModule.setGainLinear(3.4951908588409423828125f);
            gainModule.process(juce::dsp::ProcessContextReplacing <float>(inputBlock));
            antiAliasSecondStage.process(juce::dsp::ProcessContextReplacing <float>(inputBlock));
            gainModule.setGainLinear(0.0372033305466175079345703125f);
            gainModule.process(juce::dsp::ProcessContextReplacing <float>(inputBlock));
            antiAliasThirdStage.process(juce::dsp::ProcessContextReplacing <float>(inputBlock));

            //iterate through all the samples in the buffer
            for (int y = 0; y < bufferLength; y++)
            {
                // calculate base address factors
                gainBaseAddr = (decayTime << 5) | (program << 8);
                preDelay_high = preDelay >> 3;
                preDelay_low = preDelay << 5;
                preDelay_low = preDelay_low >> 5;
                delayBaseAddr = (preDelay_low << 6) | (program << 9) | (preDelay_high << 12);

                // calculate write tap (=test tap)
                rowInput = nROW;
                columnInput = nCOLUMN;
                mWritePosition = static_cast<int>(calculateAddress(rowInput, columnInput));//store address
                mDelayBuffer.copyFrom(channel, mWritePosition, mInputBuffer, channel, y, 1);//write sample

                // calculate feedback taps
                dly_mod_addr = delayModBaseAddr + 7;
                dly_addr = delayBaseAddr + 16;
                gainModContAddress = gainModContBaseAddr + 8;
                gainAddress = gainBaseAddr + 8;
                for (int d = 0; d < 15; d++)
                {
                    rowInput = U79[dly_mod_addr + d] + nROW;
                    columnInput = U69[dly_addr + d * 2] + nCOLUMN;
                    delayTaps[1 + d] = calculateAddress(rowInput, columnInput);

                    gainModContOut = U76[gainModContAddress + d];
                    nGainModEnable = gainModContOut >> 3;
                    gainModContOut = gainModContOut << 5;
                    gainModContOut = gainModContOut >> 5;

                    gainModAddress = gainModContOut | gainModBaseAddr;
                    gainModOut = U77[gainModAddress];

                    gainOut = U78[gainAddress + d] << 1;
                    if (gainModOut < gainOut && nGainModEnable == 0)
                    {
                        gainCeiling[1 + d] = gainModOut;
                    }
                    else
                    {
                        gainCeiling[1 + d] = gainOut;
                    }
                    nGSN = U78[gainAddress + d] >> 7;
                    signMod[1 + d] = nGSN;

                    mReadPosition = static_cast<int>(delayTaps[1 + d]);
                    if (signMod[1 + d] == 0)
                    {
                        mFeedbackGain = (gainCeiling[1 + d] / 256.0f) * -1.0f;
                    }
                    else
                    {
                        mFeedbackGain = (gainCeiling[1 + d] / 256.0f);
                    }
                    mFeedbackTaps += mDelayBuffer.getSample(0, mReadPosition) * mFeedbackGain;
                }
                mFeedbackTaps = mFeedbackTaps / 15.0f;
                mFeedbackBuffer.setSample(0, y, mFeedbackTaps);

                // calculate output taps
                gainAddress = gainBaseAddr + 23;
                dly_mod_addr = delayBaseAddr + 45;
                dly_addr = delayBaseAddr + 46;

                //left output taps
                for (int d = 0; d < 3; d++)
                {
                    rowInput = U69[dly_mod_addr + d * 2] + nROW;
                    columnInput = U69[dly_addr + d * 2] + nCOLUMN;
                    delayTaps[16 + d] = calculateAddress(rowInput, columnInput);

                    gainOut = U78[gainAddress + d] << 1;
                    gainCeiling[16 + d] = gainOut;
                    nGSN = U78[gainAddress + d] >> 7;
                    signMod[16 + d] = nGSN;

                    mReadPosition = static_cast<int>(delayTaps[16 + d]);
                    if (signMod[16 + d] == 0)
                    {
                        mOutputGain = (gainCeiling[16 + d] / 256.0f) * -1.0f;
                    }
                    else
                    {
                        mOutputGain = (gainCeiling[16 + d] / 256.0f);
                    }
                    mOutputTaps += mDelayBuffer.getSample(0, mReadPosition) * mOutputGain;
                }
                mOutputTaps = mOutputTaps / 4.0f;
                mOutputBuffer.setSample(0, y, mOutputTaps);

                //right output taps
                for (int d = 4; d < 8; d++)
                {
                    rowInput = U69[dly_mod_addr + d * 2] + nROW;
                    columnInput = U69[dly_addr + d * 2] + nCOLUMN;
                    delayTaps[16 + d] = calculateAddress(rowInput, columnInput);

                    gainOut = U78[gainAddress + d] << 1;
                    gainCeiling[16 + d] = gainOut;
                    nGSN = U78[gainAddress + d] >> 7;
                    signMod[16 + d] = nGSN;

                    mReadPosition = static_cast<int>(delayTaps[16 + d]);
                    if (signMod[16 + d] == 0)
                    {
                        mOutputGain = (gainCeiling[16 + d] / 256.0f) * -1.0f;
                    }
                    else
                    {
                        mOutputGain = (gainCeiling[16 + d] / 256.0f);
                    }
                    mOutputTaps += mDelayBuffer.getSample(0, mReadPosition) * mOutputGain;
                }
                mOutputTaps = mOutputTaps / 4.0f;
                mOutputBuffer.setSample(1, y, mOutputTaps);

                // mod rate counter
                modClockOut = modClockOut + 1;
                if (modClockOut == 16)
                {
                    modRateCount = rateLevel | (program << 4);
                    modClockOut = U71[modRateCount];
                }
                modCarry = (modClockOut + 1) >> 4;

                nROW = countWriteAddress(writeAddress);
                nCOLUMN = countWriteAddress(writeAddress) >> 8;
                writeAddress = countWriteAddress(writeAddress);

                MCCK = modCarry;
                if (MCCK == 1)
                {
                    modCount = modCount + 1;
                    if (modCount > 8191)
                    {
                        modCount = 0;
                    }

                    gainModContBaseAddr = (modCount >> 6) << 5;
                    gainModBaseAddr = modCount << 7;
                    gainModBaseAddr = gainModBaseAddr >> 4;
                    delayModCount = modCount >> 6;
                    delayModBaseAddr = delayModCount << 5;
                }
            }
            deEmphasis.process(juce::dsp::ProcessContextReplacing <float>(outputBlock)); //deEmphasis
            //elliptical anti aliasing filter @ 48khz
            /*gainModule.setGainLinear(1.16304862499237060546875f);
            gainModule.process(juce::dsp::ProcessContextReplacing <float>(outputBlock));
            antiAliasFirstStage.process(juce::dsp::ProcessContextReplacing <float>(outputBlock));
            gainModule.setGainLinear(3.4951908588409423828125f);
            gainModule.process(juce::dsp::ProcessContextReplacing <float>(outputBlock));
            antiAliasSecondStage.process(juce::dsp::ProcessContextReplacing <float>(outputBlock));
            gainModule.setGainLinear(0.0372033305466175079345703125f);
            gainModule.process(juce::dsp::ProcessContextReplacing <float>(outputBlock));
            antiAliasThirdStage.process(juce::dsp::ProcessContextReplacing <float>(outputBlock));*/
            //add output samples to buffer
            buffer.copyFrom(0, 0, mOutputBuffer, 0, 0, bufferLength);
            buffer.copyFrom(1, 0, mOutputBuffer, 1, 0, bufferLength);
        }
        //clear input buffer
        mInputBuffer.clear(0, 0, bufferLength);
    }
}

//==============================================================================
bool NewProjectAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* NewProjectAudioProcessor::createEditor()
{
    return new NewProjectAudioProcessorEditor (*this);
}

//==============================================================================
void NewProjectAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // You should use this method to store your parameters in the memory block.
    // You could do that either as raw data, or use the XML or ValueTree classes
    // as intermediaries to make it easy to save and load complex data.
}

void NewProjectAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // You should use this method to restore your parameters from this memory block,
    // whose contents will have been created by the getStateInformation() call.
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NewProjectAudioProcessor();
}