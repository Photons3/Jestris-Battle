#include "app/audio_out.hpp"

#include <cstdio>

namespace tetris {

AudioOut::AudioOut() {
    buffer_.reserve(4096);
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "Sound off: %s\n", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 1, kAudioSampleRate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &AudioOut::feed, this);
    if (stream_ == nullptr || !SDL_ResumeAudioStreamDevice(stream_)) {
        std::fprintf(stderr, "Sound off: %s\n", SDL_GetError());
        if (stream_ != nullptr) SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

AudioOut::~AudioOut() {
    if (stream_ == nullptr) return;
    SDL_DestroyAudioStream(stream_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void AudioOut::play(Sound sound) {
    locked([sound](Mixer& m) { m.play(sound); });
}

void AudioOut::setMuted(bool muted) {
    locked([muted](Mixer& m) { m.setMuted(muted); });
}

void AudioOut::setSoundVolume(float volume) {
    locked([volume](Mixer& m) { m.setSoundVolume(volume); });
}

void AudioOut::setMusicVolume(float volume) {
    locked([volume](Mixer& m) { m.setMusicVolume(volume); });
}

void AudioOut::startMusic() {
    locked([](Mixer& m) { m.startMusic(); });
}

void AudioOut::stopMusic() {
    locked([](Mixer& m) { m.stopMusic(); });
}

void AudioOut::setMusicPaused(bool paused) {
    locked([paused](Mixer& m) { m.setMusicPaused(paused); });
}

void AudioOut::setMusicTempo(float bpm) {
    locked([bpm](Mixer& m) { m.setMusicTempo(bpm); });
}

void AudioOut::setMusicMuted(bool muted) {
    locked([muted](Mixer& m) { m.setMusicMuted(muted); });
}

void SDLCALL AudioOut::feed(void* userdata, SDL_AudioStream* stream, int additionalBytes, int) {
    auto& self = *static_cast<AudioOut*>(userdata);
    const int frames = additionalBytes / static_cast<int>(sizeof(float));
    if (frames <= 0) return;
    self.buffer_.resize(static_cast<std::size_t>(frames));
    self.mixer_.mix(self.buffer_.data(), frames);
    SDL_PutAudioStreamData(stream, self.buffer_.data(), frames * static_cast<int>(sizeof(float)));
}

}  // namespace tetris
