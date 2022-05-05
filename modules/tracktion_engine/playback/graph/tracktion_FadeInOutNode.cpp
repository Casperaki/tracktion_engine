/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2018
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

namespace tracktion { inline namespace engine
{

//==============================================================================
//==============================================================================
FadeInOutNode::FadeInOutNode (std::unique_ptr<tracktion::graph::Node> inputNode,
                              tracktion::graph::PlayHeadState& playHeadStateToUse,
                              TimeRange in, TimeRange out,
                              AudioFadeCurve::Type fadeInType_, AudioFadeCurve::Type fadeOutType_,
                              bool clearSamplesOutsideFade)
    : input (std::move (inputNode)),
      playHeadState (playHeadStateToUse),
      fadeIn (in),
      fadeOut (out),
      fadeInType (fadeInType_),
      fadeOutType (fadeOutType_),
      clearExtraSamples (clearSamplesOutsideFade)
{
    jassert (! (fadeIn.isEmpty() && fadeOut.isEmpty()));

    setOptimisations ({ tracktion::graph::ClearBuffers::no,
                        tracktion::graph::AllocateAudioBuffer::yes });
}

//==============================================================================
tracktion::graph::NodeProperties FadeInOutNode::getNodeProperties()
{
    auto props = input->getNodeProperties();
    props.nodeID = 0;

    return props;
}

std::vector<tracktion::graph::Node*> FadeInOutNode::getDirectInputNodes()
{
    return { input.get() };
}

void FadeInOutNode::prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo& info)
{
    fadeInSampleRange = tracktion::toSamples (fadeIn, info.sampleRate);
    fadeOutSampleRange = tracktion::toSamples (fadeOut, info.sampleRate);
}

bool FadeInOutNode::isReadyToProcess()
{
    return input->hasProcessed();
}

void FadeInOutNode::process (ProcessContext& pc)
{
    const auto splitTimelineRange = referenceSampleRangeToSplitTimelineRange (playHeadState.playHead, pc.referenceSampleRange);
    jassert (! splitTimelineRange.isSplit);
    const auto timelineRange = splitTimelineRange.timelineRange1;
    jassert (timelineRange.isEmpty() || timelineRange.getLength() == pc.referenceSampleRange.getLength());
    
    auto sourceBuffers = input->getProcessedOutput();
    auto destAudioBlock = pc.buffers.audio;
    auto& destMidiBlock = pc.buffers.midi;
    jassert (sourceBuffers.audio.getSize() == destAudioBlock.getSize());

    destMidiBlock.copyFrom (sourceBuffers.midi);

    if (! renderingNeeded (timelineRange))
    {
        // If we don't need to apply the fade, just pass through the buffer
        setAudioOutput (input.get(), sourceBuffers.audio);
        return;
    }

    // Otherwise copy the source in to the dest ready for fading
    tracktion::graph::copyIfNotAliased (destAudioBlock, sourceBuffers.audio);

    auto numSamples = destAudioBlock.getNumFrames();
    jassert (numSamples == timelineRange.getLength());

    if (timelineRange.intersects (fadeInSampleRange) && fadeInSampleRange.getLength() > 0)
    {
        double alpha1 = 0;
        auto startSamp = int (fadeInSampleRange.getStart() - timelineRange.getStart());

        if (startSamp > 0)
        {
            if (clearExtraSamples)
                destAudioBlock.getStart ((choc::buffer::FrameCount) startSamp).clear();
        }
        else
        {
            alpha1 = (timelineRange.getStart() - fadeInSampleRange.getStart()) / (double) fadeInSampleRange.getLength();
            startSamp = 0;
        }

        int endSamp;
        double alpha2;

        if (timelineRange.getEnd() >= fadeInSampleRange.getEnd())
        {
            endSamp = int (fadeInSampleRange.getEnd() - timelineRange.getStart());
            alpha2 = 1.0;
        }
        else
        {
            endSamp = (int) timelineRange.getLength();
            alpha2 = juce::jmax (0.0, (timelineRange.getEnd() - fadeInSampleRange.getStart()) / (double) fadeInSampleRange.getLength());
        }

        if (endSamp > startSamp)
        {
            const int numFadeInSamples = endSamp - startSamp;
            jassert (numFadeInSamples <= (int) fadeInSampleRange.getLength());
            auto buffer = tracktion::graph::toAudioBuffer (destAudioBlock);
            AudioFadeCurve::applyCrossfadeSection (buffer,
                                                   startSamp, numFadeInSamples,
                                                   fadeInType,
                                                   (float) alpha1,
                                                   (float) alpha2);
        }
    }

    const bool pastFadeOutTime = timelineRange.getStart() >= fadeOutSampleRange.getEnd();
    const bool intersectsFadeOutTime = timelineRange.getEnd() >= fadeOutSampleRange.getEnd();

    if (clearExtraSamples && pastFadeOutTime)
    {
        destAudioBlock.clear();
    }
    else if (timelineRange.intersects (fadeOutSampleRange) && fadeOutSampleRange.getLength() > 0)
    {
        double alpha1 = 0;
        auto startSamp = int (fadeOutSampleRange.getStart() - timelineRange.getStart());

        if (startSamp <= 0)
        {
            startSamp = 0;
            alpha1 = (timelineRange.getStart() - fadeOutSampleRange.getStart()) / (double) fadeOutSampleRange.getLength();
        }

        uint32_t endSamp;
        double alpha2;

        if (intersectsFadeOutTime)
        {
            endSamp = (uint32_t) (timelineRange.getEnd() - fadeOutSampleRange.getEnd());
            alpha2 = 1.0;

            if (clearExtraSamples && endSamp < numSamples)
                destAudioBlock.fromFrame (endSamp).clear();
        }
        else
        {
            endSamp = numSamples;
            alpha2 = (timelineRange.getEnd() - fadeOutSampleRange.getStart()) / (double) fadeOutSampleRange.getLength();
        }

        if (endSamp > (uint32_t) startSamp)
        {
            auto buffer = tracktion::graph::toAudioBuffer (destAudioBlock);
            AudioFadeCurve::applyCrossfadeSection (buffer,
                                                   startSamp, (int) endSamp - startSamp,
                                                   fadeOutType,
                                                   juce::jlimit (0.0f, 1.0f, (float) (1.0 - alpha1)),
                                                   juce::jlimit (0.0f, 1.0f, (float) (1.0 - alpha2)));
        }
    }
}

bool FadeInOutNode::renderingNeeded (const juce::Range<int64_t>& timelineSampleRange) const
{
    if (! playHeadState.playHead.isPlaying())
        return false;

    return fadeInSampleRange.intersects (timelineSampleRange)
        || fadeOutSampleRange.intersects (timelineSampleRange)
        || (clearExtraSamples && (timelineSampleRange.getStart() <= fadeInSampleRange.getStart()
                                  || timelineSampleRange.getEnd() >= fadeOutSampleRange.getEnd()));
}

}} // namespace tracktion { inline namespace engine
