#include "audio/mixer.hpp"

#include <algorithm>

#include "audio/synth.hpp"

namespace tetris {

Mixer::Mixer(int sampleRate) : music_(sampleRate) {
    cache_.reserve(kSoundCount);
    for (int i = 0; i < kSoundCount; ++i) cache_.push_back(render(static_cast<Sound>(i), sampleRate));
    voices_.reserve(kMaxVoices);
}

void Mixer::play(Sound sound) {
    if (muted_) return;
    if (static_cast<int>(voices_.size()) == kMaxVoices) voices_.erase(voices_.begin());
    voices_.push_back({static_cast<int>(sound), 0});
}

void Mixer::mix(float* out, int frames) {
    std::fill(out, out + frames, 0.0f);
    for (Voice& voice : voices_) {
        const std::vector<float>& samples = cache_[static_cast<std::size_t>(voice.sound)];
        const std::size_t n = std::min(static_cast<std::size_t>(frames), samples.size() - voice.pos);
        for (std::size_t i = 0; i < n; ++i) out[i] += samples[voice.pos + i] * kMasterGain;
        voice.pos += n;
    }
    std::erase_if(voices_, [this](const Voice& v) { return v.pos >= cache_[static_cast<std::size_t>(v.sound)].size(); });
    const float musicGain = muted_ || musicMuted_ ? 0.0f : kMusicGain * kMasterGain;
    music_.mix(out, frames, musicGain);  // runs even when silent, so the song stays in time
    for (int i = 0; i < frames; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

void Mixer::setMuted(bool muted) {
    muted_ = muted;
    if (muted_) voices_.clear();
}

}  // namespace tetris
