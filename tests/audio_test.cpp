#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "audio/mixer.hpp"
#include "audio/music.hpp"
#include "audio/synth.hpp"

using namespace tetris;

namespace {

constexpr int kRate = 48000;

float peakOf(const std::vector<float>& samples) {
    float peak = 0.0f;
    for (float s : samples) peak = std::max(peak, std::abs(s));
    return peak;
}

int zeroCrossings(const std::vector<float>& samples, std::size_t count) {
    int crossings = 0;
    for (std::size_t i = 1; i < count && i < samples.size(); ++i)
        if ((samples[i - 1] < 0.0f) != (samples[i] < 0.0f)) ++crossings;
    return crossings;
}

}  // namespace

TEST_CASE("audio: every sound renders a short, audible, in-range buffer") {
    for (int i = 0; i < kSoundCount; ++i) {
        CAPTURE(i);
        const std::vector<float> samples = render(static_cast<Sound>(i), kRate);
        REQUIRE_FALSE(samples.empty());
        CHECK(samples.size() < static_cast<std::size_t>(kRate * kMaxSoundMs / 1000));
        CHECK(peakOf(samples) <= 1.0f);
        CHECK(peakOf(samples) > 0.05f);
    }
}

TEST_CASE("audio: rendering is deterministic") {
    for (int i = 0; i < kSoundCount; ++i) {
        CAPTURE(i);
        CHECK(render(static_cast<Sound>(i), kRate) == render(static_cast<Sound>(i), kRate));
    }
}

TEST_CASE("audio: sounds may run up to 2.5 s for the jingles") { CHECK(kMaxSoundMs == 2500); }

TEST_CASE("audio: each combo step is higher than the one before") {
    const std::size_t mainNote = kRate * 80 / 1000;  // inside the 90 ms square note
    int previous = 0;
    for (int i = 0; i < kComboSteps; ++i) {
        CAPTURE(i);
        const Sound sound = static_cast<Sound>(static_cast<int>(Sound::Combo1) + i);
        const int crossings = zeroCrossings(render(sound, kRate), mainNote);
        CHECK(crossings > previous);
        previous = crossings;
    }
}

TEST_CASE("audio: win and lose jingles last well over a second") {
    CHECK(render(Sound::Won, kRate).size() > static_cast<std::size_t>(kRate * 3 / 2));
    CHECK(render(Sound::Lost, kRate).size() > static_cast<std::size_t>(kRate * 2));
}

namespace {

std::vector<float> mixFrames(Mixer& mixer, int frames) {
    std::vector<float> out(static_cast<std::size_t>(frames), 7.0f);  // junk, to prove mix overwrites
    mixer.mix(out.data(), frames);
    return out;
}

}  // namespace

TEST_CASE("audio: a mixer with no voices writes silence") {
    Mixer mixer(kRate);
    const std::vector<float> out = mixFrames(mixer, 256);
    CHECK(std::all_of(out.begin(), out.end(), [](float s) { return s == 0.0f; }));
}

TEST_CASE("audio: voices add up at the master gain") {
    Mixer one(kRate);
    one.play(Sound::Move);
    Mixer two(kRate);
    two.play(Sound::Move);
    two.play(Sound::Move);
    const std::vector<float> a = mixFrames(one, 512);
    const std::vector<float> b = mixFrames(two, 512);
    const std::vector<float> raw = render(Sound::Move, kRate);
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i] == doctest::Approx(raw[i] * kMasterGain));
        CHECK(b[i] == doctest::Approx(2.0f * a[i]));
    }
}

TEST_CASE("audio: loud overlapping voices are clamped to [-1, 1]") {
    Mixer mixer(kRate);
    for (int i = 0; i < 4; ++i) mixer.play(Sound::Bomb);
    const std::vector<float> out = mixFrames(mixer, kRate / 10);
    CHECK(peakOf(out) == doctest::Approx(1.0f));
}

TEST_CASE("audio: finished voices are removed") {
    Mixer mixer(kRate);
    mixer.play(Sound::Move);
    CHECK(mixer.activeVoices() == 1);
    mixFrames(mixer, kRate / 10);  // Move lasts 25 ms
    CHECK(mixer.activeVoices() == 0);
}

TEST_CASE("audio: a ninth voice replaces the oldest") {
    Mixer mixer(kRate);
    mixer.play(Sound::Tetris);  // long; it would still be playing after 100 ms
    for (int i = 0; i < kMaxVoices; ++i) mixer.play(Sound::Move);
    CHECK(mixer.activeVoices() == kMaxVoices);
    mixFrames(mixer, kRate / 10);
    CHECK(mixer.activeVoices() == 0);
}

TEST_CASE("audio: muting silences, drops playing voices and ignores play") {
    Mixer mixer(kRate);
    mixer.play(Sound::Tetris);
    mixer.setMuted(true);
    CHECK(mixer.muted());
    CHECK(mixer.activeVoices() == 0);
    mixer.play(Sound::Tetris);
    CHECK(mixer.activeVoices() == 0);
    CHECK(peakOf(mixFrames(mixer, 256)) == 0.0f);
    mixer.setMuted(false);
    mixer.play(Sound::Tetris);
    CHECK(mixer.activeVoices() == 1);
}

TEST_CASE("audio: the sound volume scales the effects") {
    Mixer full(kRate);
    Mixer half(kRate);
    Mixer silent(kRate);
    half.setSoundVolume(0.5f);
    silent.setSoundVolume(0.0f);
    for (Mixer* m : {&full, &half, &silent}) m->play(Sound::Move);
    const std::vector<float> a = mixFrames(full, 512);
    const std::vector<float> b = mixFrames(half, 512);
    CHECK(peakOf(a) > 0.0f);
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(b[i] == doctest::Approx(0.5f * a[i]));
    CHECK(peakOf(mixFrames(silent, 512)) == 0.0f);
}

TEST_CASE("audio: the music volume scales only the music") {
    Mixer full(kRate);
    Mixer half(kRate);
    half.setMusicVolume(0.5f);
    for (Mixer* m : {&full, &half}) m->startMusic();
    const std::vector<float> a = mixFrames(full, 4800);
    const std::vector<float> b = mixFrames(half, 4800);
    CHECK(peakOf(a) > 0.0f);
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(b[i] == doctest::Approx(0.5f * a[i]));

    Mixer effects(kRate);
    Mixer effectsNoMusic(kRate);
    effectsNoMusic.setMusicVolume(0.0f);  // no music is playing, so the effects must be untouched
    for (Mixer* m : {&effects, &effectsNoMusic}) m->play(Sound::Move);
    CHECK(mixFrames(effects, 512) == mixFrames(effectsNoMusic, 512));
}

TEST_CASE("audio: the sound volume leaves the music alone") {
    Mixer full(kRate);
    Mixer quietEffects(kRate);
    quietEffects.setSoundVolume(0.0f);
    for (Mixer* m : {&full, &quietEffects}) m->startMusic();
    CHECK(mixFrames(full, 4800) == mixFrames(quietEffects, 4800));
}

TEST_CASE("audio: volumes are clamped to [0, 1]") {
    Mixer loud(kRate);
    Mixer full(kRate);
    loud.setSoundVolume(5.0f);
    for (Mixer* m : {&loud, &full}) m->play(Sound::Move);
    CHECK(mixFrames(loud, 512) == mixFrames(full, 512));
    Mixer negative(kRate);
    negative.setSoundVolume(-1.0f);
    negative.play(Sound::Move);
    CHECK(peakOf(mixFrames(negative, 512)) == 0.0f);
}

namespace {

std::vector<float> playMusic(Music& music, int frames, float gain = 1.0f) {
    std::vector<float> out(static_cast<std::size_t>(frames), 0.0f);
    music.mix(out.data(), frames, gain);
    return out;
}

}  // namespace

TEST_CASE("music: both voices of the score last 96 beats") {
    CHECK(beatsOf(melodyScore()) == doctest::Approx(kScoreBeats));
    CHECK(beatsOf(bassScore()) == doctest::Approx(kScoreBeats));
}

TEST_CASE("music: silent until started and after stopping") {
    Music music(kRate);
    CHECK_FALSE(music.playing());
    CHECK(peakOf(playMusic(music, 4800)) == 0.0f);
    music.start();
    CHECK(music.playing());
    CHECK(peakOf(playMusic(music, 4800)) > 0.1f);
    music.stop();
    CHECK_FALSE(music.playing());
    CHECK(peakOf(playMusic(music, 4800)) == 0.0f);
}

TEST_CASE("music: two players produce the same samples") {
    Music a(kRate);
    Music b(kRate);
    a.start();
    b.start();
    CHECK(playMusic(a, kRate) == playMusic(b, kRate));
}

TEST_CASE("music: pausing writes nothing and resumes exactly where it stopped") {
    Music paused(kRate);
    Music straight(kRate);
    paused.start();
    straight.start();
    const std::vector<float> first = playMusic(paused, 1000);
    paused.setPaused(true);
    CHECK(paused.playing());
    CHECK(peakOf(playMusic(paused, 1000)) == 0.0f);
    paused.setPaused(false);
    const std::vector<float> resumed = playMusic(paused, 1000);
    const std::vector<float> reference = playMusic(straight, 2000);
    CHECK(std::equal(first.begin(), first.end(), reference.begin()));
    CHECK(std::equal(resumed.begin(), resumed.end(), reference.begin() + 1000));
}

TEST_CASE("music: start rewinds to the top") {
    Music music(kRate);
    music.start();
    const std::vector<float> opening = playMusic(music, 2000);
    playMusic(music, kRate);
    music.start();
    CHECK(playMusic(music, 2000) == opening);
}

TEST_CASE("music: the gain scales what is added") {
    Music full(kRate);
    Music quiet(kRate);
    full.start();
    quiet.start();
    const std::vector<float> a = playMusic(full, 2000);
    const std::vector<float> b = playMusic(quiet, 2000, 0.25f);
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(b[i] == doctest::Approx(a[i] * 0.25f));
}

TEST_CASE("music: a tempo change waits for the next note") {
    Music changed(kRate);
    Music steady(kRate);
    changed.start();
    steady.start();
    std::vector<float> a = playMusic(changed, 100);
    changed.setTempo(240.0f);
    const std::vector<float> rest = playMusic(changed, 11900);
    a.insert(a.end(), rest.begin(), rest.end());
    CHECK(a == playMusic(steady, 12000));  // the first bass note lasts half a beat: 12000 samples at 120 BPM
}

TEST_CASE("music: the loop repeats exactly, and a faster tempo shortens it") {
    Music music(kRate);
    music.setTempo(240.0f);
    music.start();
    const int loop = static_cast<int>(kScoreBeats) * kRate * 60 / 240;  // 12000 samples per beat
    const std::vector<float> out = playMusic(music, loop + kRate);
    CHECK(std::equal(out.begin(), out.begin() + kRate, out.begin() + loop));
    CHECK(peakOf(std::vector<float>(out.begin() + loop - 4800, out.begin() + loop + 4800)) > 0.1f);  // no gap

    Music slower(kRate);
    slower.start();  // 120 BPM: halfway through the song at the same point
    const std::vector<float> half = playMusic(slower, loop + kRate);
    CHECK_FALSE(std::equal(half.begin(), half.begin() + kRate, half.begin() + loop));
}

TEST_CASE("music: after a tempo change melody and bass start notes together") {
    Music music(kRate);
    music.start();
    playMusic(music, 60000);  // 120 BPM, half way through the 1-beat D5 that starts on beat 2
    music.setTempo(168.0f);   // 17142.86 samples per beat: eighth notes don't round evenly
    int melodyStarts = 0;
    std::vector<int> bassStarts;
    for (int i = 1; i <= 16 * 17143; ++i) {  // four bars at 168 BPM
        playMusic(music, 1);
        if (music.notePos(1) == 1) bassStarts.push_back(i);
        if (music.notePos(0) != 1) continue;
        ++melodyStarts;
        CAPTURE(i);
        CHECK(music.notePos(1) == 1);
    }
    CHECK(melodyStarts == 18);  // beats 3 to 18.5 of the score
    REQUIRE(bassStarts.size() > 2);
    const int lastBass = bassStarts.back() - bassStarts[bassStarts.size() - 2];
    CHECK((lastBass == 8571 || lastBass == 8572));  // the bass plays at 168 BPM too
}

TEST_CASE("mixer: music plays under the effects at the music gain") {
    Mixer mixer(kRate);
    mixer.startMusic();
    CHECK(mixer.musicPlaying());
    Music reference(kRate);
    reference.start();
    const std::vector<float> expected = playMusic(reference, 2000, kMusicGain * kMasterGain);
    const std::vector<float> out = mixFrames(mixer, 2000);
    for (std::size_t i = 0; i < out.size(); ++i) CHECK(out[i] == doctest::Approx(expected[i]));
}

TEST_CASE("mixer: muting the music leaves only the effects") {
    Mixer withMusic(kRate);
    withMusic.startMusic();
    withMusic.setMusicMuted(true);
    CHECK(withMusic.musicMuted());
    withMusic.play(Sound::Tetris);
    Mixer effectsOnly(kRate);
    effectsOnly.play(Sound::Tetris);
    CHECK(mixFrames(withMusic, 4000) == mixFrames(effectsOnly, 4000));
}

TEST_CASE("mixer: muted music keeps its place in the song") {
    for (bool masterMute : {true, false}) {
        CAPTURE(masterMute);
        Mixer muted(kRate);
        Mixer reference(kRate);
        muted.startMusic();
        reference.startMusic();
        if (masterMute)
            muted.setMuted(true);
        else
            muted.setMusicMuted(true);
        CHECK(peakOf(mixFrames(muted, 1000)) == 0.0f);
        CHECK(muted.musicPlaying());
        muted.setMuted(false);
        muted.setMusicMuted(false);
        mixFrames(reference, 1000);
        CHECK(mixFrames(muted, 1000) == mixFrames(reference, 1000));
    }
}
