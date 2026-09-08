#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

enum class AudioFXType : int {
    LowPass = 0,
    HighPass,
    BassBoost,
    Distortion,
    Reverb,
    Echo,
    Pitch,
    WowFlutter,
    NoiseHiss,
    Count
};

const char* AudioFXTypeName(AudioFXType type);

struct AudioFXEffect {
    AudioFXType type = AudioFXType::LowPass;
    bool enabled = true;
    float freq = 1000.0f;
    float q = 0.707f;
    float gainDb = 6.0f;
    float amount = 0.5f;
    float mix = 1.0f;
    float param1 = 0.3f;
    float param2 = 0.4f;
    float param3 = 0.0f;
};

struct AudioFXChain {
    std::string name;
    bool enabled = true;
    float stereoWidth = 1.0f;
    std::vector<AudioFXEffect> effects;

    bool active() const { return enabled && !effects.empty(); }
    float pitchSemitones() const;
};

namespace AudioFXPresets {
    const std::vector<std::string>& names();
    AudioFXChain get(const std::string& name);
}

std::string AudioFXSerialize(const AudioFXChain& chain);
AudioFXChain AudioFXParse(const std::string& text);

class AudioFXProcessor {
public:
    static constexpr int kMaxEffects = 12;

    void configure(int channels, int sampleRate);
    void setChain(const AudioFXChain& chain);
    void process(float* io, int frameCount);
    bool active() const;

private:
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };
    struct EffectState {
        std::vector<float> x1, x2, y1, y2;
        Biquad coeff;
        std::vector<float> delay;
        int delayFrames = 0;
        int delayPos = 0;
        std::vector<float> combA, combB, allpass;
        int combAFrames = 0, combBFrames = 0, allpassFrames = 0;
        int combAPos = 0, combBPos = 0, allpassPos = 0;
        std::vector<float> lp;
        float lfoPhase = 0.0f;
        float toneState = 0.0f;
    };

    void rebuildLocked();
    void applyEffect(const AudioFXEffect& fx, EffectState& st, float* io, int frameCount);

    mutable std::mutex mutex_;
    int channels_ = 0;
    int sampleRate_ = 48000;

    int count_ = 0;
    AudioFXEffect effects_[kMaxEffects];
    float stereoWidth_ = 1.0f;
    bool chainActive_ = false;
    std::vector<EffectState> state_;

    uint32_t rng_ = 0x1234567u;
    float nextNoise();
};
