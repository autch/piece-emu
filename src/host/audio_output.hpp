#pragma once
#include <cstdint>

struct SDL_AudioStream;
class Sound;
class AudioLog;

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

    // Enable stderr trace for diagnosing audio problems.
    void set_trace(bool v) { trace_ = v; }
    bool trace() const { return trace_; }

    // Attach an AudioLog to record every PULL event.  Null disables.
    void set_log(AudioLog* log) { log_ = log; }

private:
    SDL_AudioStream* stream_ = nullptr;
    Sound*           sound_  = nullptr;
    bool             trace_  = false;
    AudioLog*        log_    = nullptr;

    // SDL audio callback (thread: SDL audio thread).
    static void audio_cb(void* userdata, SDL_AudioStream* stream,
                         int additional_amount, int total_amount);
};
