#pragma once

#include <SDL3/SDL.h>

#include <vector>

#include "audio/mixer.hpp"

namespace tetris {

inline constexpr int kAudioSampleRate = 48000;

// Plays sound effects and music on the default output device. If none can be opened it logs once and stays silent.
class AudioOut {
public:
    AudioOut();  // needs SDL_Init to have run
    ~AudioOut();
    AudioOut(const AudioOut&) = delete;  // SDL holds `this` for the callback
    AudioOut& operator=(const AudioOut&) = delete;

    void play(Sound sound);
    void setMuted(bool muted);
    void setSoundVolume(float volume);
    void setMusicVolume(float volume);
    void startMusic();
    void stopMusic();
    void setMusicPaused(bool paused);
    void setMusicTempo(float bpm);
    void setMusicMuted(bool muted);

private:
    // Runs `change` on the mixer under the stream lock, which SDL also holds while it runs feed().
    template <class Change>
    void locked(Change change) {
        if (stream_ == nullptr) return;
        SDL_LockAudioStream(stream_);
        change(mixer_);
        SDL_UnlockAudioStream(stream_);
    }

    static void SDLCALL feed(void* userdata, SDL_AudioStream* stream, int additionalBytes, int totalBytes);

    Mixer mixer_{kAudioSampleRate};
    std::vector<float> buffer_;  // reused by the callback
    SDL_AudioStream* stream_ = nullptr;
};

}  // namespace tetris
