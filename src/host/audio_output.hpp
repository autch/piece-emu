#pragma once
#include <atomic>
#include <cstdint>

struct SDL_AudioStream;
class Sound;

// ============================================================================
// AudioOutput — SDL3-based audio sink for the P/ECE PWM emulator.
//
// Usage:
//   AudioOutput audio;
//   if (audio.open(sound)) { ... }  // opens the default audio device
//   audio.close();
//
// Samples are pulled from `sound` in the SDL audio callback thread.  SDL3
// handles resampling from the native 32 kHz rate to the host device rate
// via SDL_AudioStream.
// ============================================================================

class AudioOutput {
public:
    ~AudioOutput();

    // Open the default audio device at 32 kHz / mono / s16, wire up the
    // callback to pull from `sound`.  Returns true on success.
    bool open(Sound& sound);

    // Stop audio playback and release SDL resources.
    void close();

    // True if a device is currently open.
    bool is_open() const { return stream_ != nullptr; }

    // Bytes currently queued in the SDL audio stream (pending playback).
    // Returns 0 when closed.  Used by the CPU loop's audio-clock pacing.
    int queued_bytes() const;

    // True once the SDL audio callback has fired at least once — i.e. the
    // device is actively consuming samples.  Lets the CPU loop tell
    // "audio open but silent" (no pull yet) apart from "audio running".
    bool is_active() const { return cb_count_.load() > 0; }

    // Enable stderr trace for diagnosing audio problems.
    void set_trace(bool v) { trace_ = v; }
    bool trace() const { return trace_; }

private:
    SDL_AudioStream* stream_ = nullptr;
    Sound*           sound_  = nullptr;
    bool             trace_  = false;
    std::atomic<uint64_t> cb_count_{0};

    // SDL audio callback (thread: SDL audio thread).
    static void audio_cb(void* userdata, SDL_AudioStream* stream,
                         int additional_amount, int total_amount);
};
