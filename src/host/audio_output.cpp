#include "audio_output.hpp"
#include "audio_log.hpp"
#include "peripheral_sound.hpp"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdint>

AudioOutput::~AudioOutput()
{
    close();
}

bool AudioOutput::open(Sound& sound)
{
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_InitSubSystem(AUDIO) failed: %s\n",
                     SDL_GetError());
        return false;
    }

    SDL_AudioSpec spec;
    spec.format   = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq     = Sound::SAMPLE_RATE;

    sound_ = &sound;
    stream_ = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec, &AudioOutput::audio_cb, this);
    if (!stream_) {
        std::fprintf(stderr, "SDL_OpenAudioDeviceStream failed: %s\n",
                     SDL_GetError());
        return false;
    }

    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(stream_);
    if (dev) SDL_ResumeAudioDevice(dev);
    std::fprintf(stderr, "SDL3 audio: %d Hz mono s16 (dev=%u)\n",
                 Sound::SAMPLE_RATE, static_cast<unsigned>(dev));
    return true;
}

void AudioOutput::close()
{
    if (stream_) {
        SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(stream_);
        if (dev) SDL_PauseAudioDevice(dev);
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    sound_ = nullptr;
}

void AudioOutput::audio_cb(void* userdata, SDL_AudioStream* stream,
                           int additional_amount, int /*total_amount*/)
{
    auto* self = static_cast<AudioOutput*>(userdata);
    if (!self || !self->sound_ || additional_amount <= 0) return;

    // additional_amount is in bytes.  Convert to sample count (int16_t).
    const int want_samples = additional_amount / static_cast<int>(sizeof(int16_t));
    if (want_samples <= 0) return;

    // Reusable buffer owned by the AudioOutput instance.  Heap-allocated
    // (class member) rather than on the audio thread's stack — Windows
    // audio threads can have very small stacks (~64 KB).  SDL calls the
    // callback only on its one audio thread so no locking is needed.
    std::size_t want = static_cast<std::size_t>(want_samples);
    if (want > CB_BUF_SAMPLES) want = CB_BUF_SAMPLES; // hard cap

    const std::size_t avail_before = self->sound_->available();
    const std::size_t got = self->sound_->pop(self->cb_buf_, want);
    // Underrun: pad remaining with silence.
    if (got < want)
        std::memset(self->cb_buf_ + got, 0, (want - got) * sizeof(int16_t));

    SDL_PutAudioStreamData(stream, self->cb_buf_,
                           static_cast<int>(want * sizeof(int16_t)));

    if (self->log_) {
        int sdl_queued = SDL_GetAudioStreamQueued(stream);
        self->log_->log_pull(static_cast<int64_t>(want),
                             static_cast<int64_t>(got),
                             static_cast<int64_t>(avail_before),
                             static_cast<int64_t>(sdl_queued));
    }

    if (self->trace_) {
        static int cb_count = 0;
        int c = ++cb_count;
        if (c <= 8 || (c % 64) == 0)
            std::fprintf(stderr, "[AUD] cb#%d want=%zu got=%zu\n", c, want, got);
    }
}
