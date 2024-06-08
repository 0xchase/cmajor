//
//     ,ad888ba,                              88
//    d8"'    "8b
//   d8            88,dba,,adba,   ,aPP8A.A8  88     The Cmajor Toolkit
//   Y8,           88    88    88  88     88  88
//    Y8a.   .a8P  88    88    88  88,   ,88  88     (C)2024 Cmajor Software Ltd
//     '"Y888Y"'   88    88    88  '"8bbP"Y8  88     https://cmajor.dev
//                                           ,88
//                                        888P"
//
//  The Cmajor project is subject to commercial or open-source licensing.
//  You may use it under the terms of the GPLv3 (see www.gnu.org/licenses), or
//  visit https://cmajor.dev to learn about our commercial licence options.
//
//  CMAJOR IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
//  EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
//  DISCLAIMED.

#include <thread>
#include "cmaj_AudioPlayer.h"

namespace cmaj::audio_utils
{

//==============================================================================
/**
 *  An AudioMIDIPlayer implementation that runs a fake audio device on a thread,
 *  reading/writing its data via functions supplied by the caller.
 */
struct RenderingAudioMIDIPlayer  : public AudioMIDIPlayer
{
    using ProvideInputFn = std::function<bool(choc::buffer::ChannelArrayView<float> audioInput,
                                              std::vector<choc::midi::ShortMessage>& midiMessages,
                                              std::vector<uint32_t>& midiMessageTimes)>;

    using HandleOutputFn = std::function<bool(choc::buffer::ChannelArrayView<const float> audioOutput)>;

    RenderingAudioMIDIPlayer (const AudioDeviceOptions&, ProvideInputFn, HandleOutputFn);
    ~RenderingAudioMIDIPlayer() override;

    AvailableAudioDevices getAvailableDevices() override    { return {}; }

    void start (AudioMIDICallback& c) override;
    void stop() override;

private:
    ProvideInputFn provideInput;
    HandleOutputFn handleOutput;
    std::mutex startLock;
    AudioMIDICallback* callback = nullptr;
    std::thread renderThread;

    void render();
};




//==============================================================================
//        _        _           _  _
//     __| |  ___ | |_   __ _ (_)| | ___
//    / _` | / _ \| __| / _` || || |/ __|
//   | (_| ||  __/| |_ | (_| || || |\__ \ _  _  _
//    \__,_| \___| \__| \__,_||_||_||___/(_)(_)(_)
//
//   Code beyond this point is implementation detail...
//
//==============================================================================

inline RenderingAudioMIDIPlayer::RenderingAudioMIDIPlayer (const AudioDeviceOptions& o, ProvideInputFn in, HandleOutputFn out)
    : AudioMIDIPlayer (o), provideInput (std::move (in)), handleOutput (std::move (out))
{
}

inline RenderingAudioMIDIPlayer::~RenderingAudioMIDIPlayer()
{
    stop();
}

inline void RenderingAudioMIDIPlayer::start (AudioMIDICallback& c)
{
    const std::lock_guard<decltype(startLock)> lock (startLock);

    if (callback == nullptr)
    {
        callback = std::addressof (c);
        renderThread = std::thread ([this] { render(); });
    }
}

inline void RenderingAudioMIDIPlayer::stop()
{
    {
        const std::lock_guard<decltype(startLock)> lock (startLock);
        callback = nullptr;
    }

    if (renderThread.joinable())
        renderThread.join();
}

inline void RenderingAudioMIDIPlayer::render()
{
    CMAJ_ASSERT (options.blockSize != 0);
    choc::buffer::ChannelArrayBuffer<float> audioInput  (options.inputChannelCount,  options.blockSize);
    choc::buffer::ChannelArrayBuffer<float> audioOutput (options.outputChannelCount, options.blockSize);
    std::vector<choc::midi::ShortMessage> midiMessages;
    std::vector<uint32_t> midiMessageTimes;
    midiMessages.reserve (512);
    midiMessageTimes.reserve (512);

    for (;;)
    {
        audioInput.clear();
        audioOutput.clear();
        midiMessages.clear();
        midiMessageTimes.clear();

        const std::lock_guard<decltype(startLock)> lock (startLock);

        if (callback == nullptr)
            return;

        if (! provideInput (audioInput, midiMessages, midiMessageTimes))
        {
            callback = nullptr;
            return;
        }

        callback->prepareToStart (options.sampleRate, [] (uint32_t, choc::midi::ShortMessage) {});

        CMAJ_ASSERT (midiMessages.size() == midiMessageTimes.size());

        if (auto totalNumMIDIMessages = static_cast<uint32_t> (midiMessages.size()))
        {
            auto frameRange = audioOutput.getFrameRange();
            uint32_t midiStart = 0;

            while (frameRange.start < frameRange.end)
            {
                auto chunkToDo = frameRange;
                auto endOfMIDI = midiStart;

                while (endOfMIDI < totalNumMIDIMessages)
                {
                    auto eventTime = midiMessageTimes[endOfMIDI];

                    if (eventTime > chunkToDo.start)
                    {
                        chunkToDo.end = eventTime;
                        break;
                    }

                    ++endOfMIDI;
                }

                for (uint32_t i = midiStart; i < endOfMIDI; ++i)
                    callback->addIncomingMIDIEvent (midiMessages[i].data, midiMessages[i].size());

                callback->process (audioInput.getFrameRange (chunkToDo),
                                    audioOutput.getFrameRange (chunkToDo),
                                    true);

                frameRange.start = chunkToDo.end;
                midiStart = endOfMIDI;
            }
        }
        else
        {
            callback->process (audioInput, audioOutput, true);
        }

        if (! handleOutput (audioOutput))
        {
            callback = nullptr;
            return;
        }
    }
}

} // namespace cmaj::audio_utils
