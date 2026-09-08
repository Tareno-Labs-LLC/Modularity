#include "AudioFX.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kEchoMaxSeconds = 1.5f;
constexpr float kWowBaseMs = 4.0f;
constexpr float kWowDepthMs = 3.0f;
constexpr float kReverbCombAMs = 36.0f;
constexpr float kReverbCombBMs = 47.0f;
constexpr float kReverbAllpassMs = 8.0f;

float DbToLin(float db) { return std::pow(10.0f, db / 20.0f); }
int MsToFrames(float ms, int sampleRate) {
    return std::max(1, static_cast<int>((ms / 1000.0f) * static_cast<float>(sampleRate)));
}
} // namespace

const char* AudioFXTypeName(AudioFXType type) {
    switch (type) {
        case AudioFXType::LowPass:    return "Low-Pass";
        case AudioFXType::HighPass:   return "High-Pass";
        case AudioFXType::BassBoost:  return "Bass Boost";
        case AudioFXType::Distortion: return "Distortion";
        case AudioFXType::Reverb:     return "Reverb";
        case AudioFXType::Echo:       return "Echo / Delay";
        case AudioFXType::Pitch:      return "Pitch";
        case AudioFXType::WowFlutter: return "Wow / Flutter";
        case AudioFXType::NoiseHiss:  return "Noise / Hiss";
        default:                      return "Effect";
    }
}

float AudioFXChain::pitchSemitones() const {
    float semis = 0.0f;
    for (const auto& fx : effects) {
        if (fx.enabled && fx.type == AudioFXType::Pitch) semis += fx.param3;
    }
    return semis;
}

namespace {
AudioFXEffect MakeLowPass(float freq, float q = 0.707f) {
    AudioFXEffect e; e.type = AudioFXType::LowPass; e.freq = freq; e.q = q; return e;
}
AudioFXEffect MakeHighPass(float freq, float q = 0.707f) {
    AudioFXEffect e; e.type = AudioFXType::HighPass; e.freq = freq; e.q = q; return e;
}
AudioFXEffect MakeBassBoost(float freq, float gainDb, float looseness = 0.0f) {
    AudioFXEffect e; e.type = AudioFXType::BassBoost; e.freq = freq; e.gainDb = gainDb; e.param2 = looseness; return e;
}
AudioFXEffect MakeDistortion(float drive, float tone, float mix) {
    AudioFXEffect e; e.type = AudioFXType::Distortion; e.amount = drive; e.param1 = tone; e.mix = mix; return e;
}
AudioFXEffect MakeReverb(float room, float damping, float mix) {
    AudioFXEffect e; e.type = AudioFXType::Reverb; e.param1 = room; e.param2 = damping; e.mix = mix; return e;
}
AudioFXEffect MakeEcho(float time, float feedback, float mix) {
    AudioFXEffect e; e.type = AudioFXType::Echo; e.param1 = time; e.param2 = feedback; e.mix = mix; return e;
}
AudioFXEffect MakePitch(float semis) {
    AudioFXEffect e; e.type = AudioFXType::Pitch; e.param3 = semis; return e;
}
AudioFXEffect MakeWowFlutter(float rate, float depth) {
    AudioFXEffect e; e.type = AudioFXType::WowFlutter; e.param1 = rate; e.amount = depth; return e;
}
AudioFXEffect MakeHiss(float level) {
    AudioFXEffect e; e.type = AudioFXType::NoiseHiss; e.amount = level; return e;
}

AudioFXChain BuildPreset(const std::string& name) {
    AudioFXChain c;
    c.name = name;
    if (name == "CRT / VHS Television") {
        c.effects = { MakeHighPass(220.0f), MakeLowPass(4200.0f), MakeDistortion(1.6f, 0.5f, 0.35f),
                      MakeWowFlutter(3.5f, 0.15f), MakeHiss(0.02f) };
        c.stereoWidth = 0.4f;
    } else if (name == "Old Radio") {
        c.effects = { MakeHighPass(400.0f), MakeLowPass(3200.0f), MakeDistortion(2.2f, 0.4f, 0.45f),
                      MakeHiss(0.03f) };
        c.stereoWidth = 0.0f;
    } else if (name == "Telephone / Intercom") {
        c.effects = { MakeHighPass(500.0f), MakeLowPass(3000.0f), MakeDistortion(1.8f, 0.35f, 0.4f) };
        c.stereoWidth = 0.0f;
    } else if (name == "Muffled / Behind Wall") {
        c.effects = { MakeLowPass(700.0f, 0.6f), MakeBassBoost(120.0f, 3.0f), MakeReverb(0.5f, 0.7f, 0.2f) };
        c.stereoWidth = 0.6f;
    } else if (name == "Boombox") {
        c.effects = { MakeHighPass(90.0f), MakeLowPass(11000.0f), MakeBassBoost(110.0f, 5.0f, 0.2f),
                      MakeDistortion(1.4f, 0.6f, 0.25f), MakeHiss(0.01f) };
        c.stereoWidth = 0.85f;
    } else if (name == "Worn Cassette Boombox") {
        c.effects = { MakeHighPass(120.0f), MakeLowPass(8000.0f), MakeBassBoost(100.0f, 4.0f, 0.6f),
                      MakeDistortion(1.5f, 0.5f, 0.3f), MakeWowFlutter(2.2f, 0.35f), MakeHiss(0.04f) };
        c.stereoWidth = 0.55f;
    } else if (name == "Bass-Heavy Boombox") {
        c.effects = { MakeHighPass(60.0f), MakeLowPass(12000.0f), MakeBassBoost(80.0f, 9.0f, 0.35f),
                      MakeDistortion(1.3f, 0.7f, 0.2f) };
        c.stereoWidth = 0.9f;
    } else if (name == "Distant Boombox") {
        c.effects = { MakeHighPass(150.0f), MakeLowPass(5000.0f), MakeBassBoost(120.0f, 3.0f, 0.4f),
                      MakeReverb(0.6f, 0.5f, 0.3f), MakeHiss(0.015f) };
        c.stereoWidth = 0.3f;
    }
    return c;
}
} // namespace

namespace AudioFXPresets {
const std::vector<std::string>& names() {
    static const std::vector<std::string> kNames = {
        "None",
        "CRT / VHS Television",
        "Old Radio",
        "Telephone / Intercom",
        "Muffled / Behind Wall",
        "Boombox",
        "Worn Cassette Boombox",
        "Bass-Heavy Boombox",
        "Distant Boombox",
    };
    return kNames;
}

AudioFXChain get(const std::string& name) {
    if (name.empty() || name == "None") {
        AudioFXChain c; c.name = "None"; return c;
    }
    return BuildPreset(name);
}
} // namespace AudioFXPresets

namespace {
std::string EscapeName(const std::string& s) {
    std::string out;
    for (char ch : s) {
        if (ch == '~' || ch == ';' || ch == ',' || ch == '\\' || ch == '\n') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}
std::string UnescapeName(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) { out.push_back(s[++i]); continue; }
        out.push_back(s[i]);
    }
    return out;
}
} // namespace

std::string AudioFXSerialize(const AudioFXChain& chain) {
    std::ostringstream os;
    os << "1~" << EscapeName(chain.name) << '~' << (chain.enabled ? 1 : 0) << '~' << chain.stereoWidth;
    for (const auto& e : chain.effects) {
        os << '~' << static_cast<int>(e.type) << ',' << (e.enabled ? 1 : 0) << ','
           << e.freq << ',' << e.q << ',' << e.gainDb << ',' << e.amount << ','
           << e.mix << ',' << e.param1 << ',' << e.param2 << ',' << e.param3;
    }
    return os.str();
}

AudioFXChain AudioFXParse(const std::string& text) {
    AudioFXChain chain;
    if (text.empty()) return chain;

    std::vector<std::string> fields;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) { cur.push_back(text[i]); cur.push_back(text[++i]); continue; }
        if (text[i] == '~') { fields.push_back(cur); cur.clear(); continue; }
        cur.push_back(text[i]);
    }
    fields.push_back(cur);
    if (fields.size() < 4) return chain;

    chain.name = UnescapeName(fields[1]);
    try { chain.enabled = std::stoi(fields[2]) != 0; } catch (...) {}
    try { chain.stereoWidth = std::stof(fields[3]); } catch (...) {}

    for (size_t f = 4; f < fields.size(); ++f) {
        std::vector<std::string> parts;
        std::stringstream es(fields[f]);
        std::string p;
        while (std::getline(es, p, ',')) parts.push_back(p);
        if (parts.size() < 10) continue;
        AudioFXEffect e;
        try {
            int t = std::stoi(parts[0]);
            if (t < 0 || t >= static_cast<int>(AudioFXType::Count)) continue;
            e.type = static_cast<AudioFXType>(t);
            e.enabled = std::stoi(parts[1]) != 0;
            e.freq = std::stof(parts[2]);
            e.q = std::stof(parts[3]);
            e.gainDb = std::stof(parts[4]);
            e.amount = std::stof(parts[5]);
            e.mix = std::stof(parts[6]);
            e.param1 = std::stof(parts[7]);
            e.param2 = std::stof(parts[8]);
            e.param3 = std::stof(parts[9]);
        } catch (...) { continue; }
        chain.effects.push_back(e);
    }
    return chain;
}

void AudioFXProcessor::configure(int channels, int sampleRate) {
    std::lock_guard<std::mutex> lock(mutex_);
    channels_ = std::max(1, channels);
    sampleRate_ = std::max(8000, sampleRate);
    rebuildLocked();
}

void AudioFXProcessor::setChain(const AudioFXChain& chain) {
    std::lock_guard<std::mutex> lock(mutex_);
    count_ = std::min<int>(kMaxEffects, static_cast<int>(chain.effects.size()));
    for (int i = 0; i < count_; ++i) effects_[i] = chain.effects[i];
    stereoWidth_ = std::clamp(chain.stereoWidth, 0.0f, 1.0f);
    chainActive_ = chain.active();
    rebuildLocked();
}

bool AudioFXProcessor::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return chainActive_ && channels_ > 0;
}

float AudioFXProcessor::nextNoise() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return (static_cast<float>(rng_ >> 8) / 8388608.0f) - 1.0f;
}

void AudioFXProcessor::rebuildLocked() {
    const int ch = channels_;
    if (ch <= 0) { state_.clear(); return; }
    state_.assign(static_cast<size_t>(count_), EffectState{});

    for (int i = 0; i < count_; ++i) {
        EffectState& st = state_[i];
        const AudioFXEffect& fx = effects_[i];
        st.x1.assign(ch, 0.0f); st.x2.assign(ch, 0.0f);
        st.y1.assign(ch, 0.0f); st.y2.assign(ch, 0.0f);
        st.lp.assign(ch, 0.0f);

        switch (fx.type) {
            case AudioFXType::Echo: {
                float t = std::clamp(fx.param1, 0.001f, kEchoMaxSeconds);
                st.delayFrames = std::max(1, static_cast<int>(t * sampleRate_));
                st.delay.assign(static_cast<size_t>(st.delayFrames) * ch, 0.0f);
                st.delayPos = 0;
                break;
            }
            case AudioFXType::WowFlutter: {
                int maxFrames = MsToFrames(kWowBaseMs + kWowDepthMs + 2.0f, sampleRate_);
                st.delayFrames = maxFrames;
                st.delay.assign(static_cast<size_t>(maxFrames) * ch, 0.0f);
                st.delayPos = 0;
                break;
            }
            case AudioFXType::Reverb: {
                st.combAFrames = MsToFrames(kReverbCombAMs, sampleRate_);
                st.combBFrames = MsToFrames(kReverbCombBMs, sampleRate_);
                st.allpassFrames = MsToFrames(kReverbAllpassMs, sampleRate_);
                st.combA.assign(static_cast<size_t>(st.combAFrames) * ch, 0.0f);
                st.combB.assign(static_cast<size_t>(st.combBFrames) * ch, 0.0f);
                st.allpass.assign(static_cast<size_t>(st.allpassFrames) * ch, 0.0f);
                break;
            }
            default: break;
        }

        if (fx.type == AudioFXType::LowPass || fx.type == AudioFXType::HighPass ||
            fx.type == AudioFXType::BassBoost) {
            float freq = std::clamp(fx.freq, 20.0f, sampleRate_ * 0.45f);
            float q = std::max(0.05f, fx.q);
            float w0 = 2.0f * kPi * freq / static_cast<float>(sampleRate_);
            float cosw = std::cos(w0);
            float sinw = std::sin(w0);
            float alpha = sinw / (2.0f * q);
            Biquad c;
            if (fx.type == AudioFXType::LowPass) {
                float b0 = (1.0f - cosw) * 0.5f, b1 = 1.0f - cosw, b2 = (1.0f - cosw) * 0.5f;
                float a0 = 1.0f + alpha, a1 = -2.0f * cosw, a2 = 1.0f - alpha;
                c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0; c.a1 = a1 / a0; c.a2 = a2 / a0;
            } else if (fx.type == AudioFXType::HighPass) {
                float b0 = (1.0f + cosw) * 0.5f, b1 = -(1.0f + cosw), b2 = (1.0f + cosw) * 0.5f;
                float a0 = 1.0f + alpha, a1 = -2.0f * cosw, a2 = 1.0f - alpha;
                c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0; c.a1 = a1 / a0; c.a2 = a2 / a0;
            } else {
                float A = std::pow(10.0f, fx.gainDb / 40.0f);
                float shelfQ = std::max(0.2f, 0.9f - std::clamp(fx.param2, 0.0f, 1.0f) * 0.6f);
                float alphaS = sinw / (2.0f * shelfQ);
                float twoSqrtAalpha = 2.0f * std::sqrt(A) * alphaS;
                float a0 = (A + 1.0f) + (A - 1.0f) * cosw + twoSqrtAalpha;
                c.b0 = A * ((A + 1.0f) - (A - 1.0f) * cosw + twoSqrtAalpha) / a0;
                c.b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw) / a0;
                c.b2 = A * ((A + 1.0f) - (A - 1.0f) * cosw - twoSqrtAalpha) / a0;
                c.a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw) / a0;
                c.a2 = ((A + 1.0f) + (A - 1.0f) * cosw - twoSqrtAalpha) / a0;
            }
            st.coeff = c;
        }
    }
}

void AudioFXProcessor::applyEffect(const AudioFXEffect& fx, EffectState& st, float* io, int frameCount) {
    const int ch = channels_;
    switch (fx.type) {
        case AudioFXType::LowPass:
        case AudioFXType::HighPass:
        case AudioFXType::BassBoost: {
            const Biquad& c = st.coeff;
            for (int f = 0; f < frameCount; ++f) {
                for (int k = 0; k < ch; ++k) {
                    float x = io[f * ch + k];
                    float y = c.b0 * x + c.b1 * st.x1[k] + c.b2 * st.x2[k] - c.a1 * st.y1[k] - c.a2 * st.y2[k];
                    st.x2[k] = st.x1[k]; st.x1[k] = x;
                    st.y2[k] = st.y1[k]; st.y1[k] = y;
                    io[f * ch + k] = y;
                }
            }
            break;
        }
        case AudioFXType::Distortion: {
            float drive = 1.0f + std::max(0.0f, fx.amount) * 8.0f;
            float mix = std::clamp(fx.mix, 0.0f, 1.0f);
            float tone = std::clamp(fx.param1, 0.0f, 1.0f);
            float toneA = std::exp(-2.0f * kPi * (200.0f + tone * 8000.0f) / static_cast<float>(sampleRate_));
            float norm = 1.0f / std::tanh(drive);
            for (int f = 0; f < frameCount; ++f) {
                for (int k = 0; k < ch; ++k) {
                    float x = io[f * ch + k];
                    float d = std::tanh(x * drive) * norm;
                    float& ts = st.y1.empty() ? st.toneState : st.y1[k];
                    ts = toneA * ts + (1.0f - toneA) * d;
                    io[f * ch + k] = x * (1.0f - mix) + ts * mix;
                }
            }
            break;
        }
        case AudioFXType::Echo: {
            if (st.delayFrames <= 0 || st.delay.empty()) break;
            float fb = std::clamp(fx.param2, 0.0f, 0.95f);
            float mix = std::clamp(fx.mix, 0.0f, 1.0f);
            for (int f = 0; f < frameCount; ++f) {
                for (int k = 0; k < ch; ++k) {
                    size_t idx = static_cast<size_t>(st.delayPos) * ch + k;
                    float delayed = st.delay[idx];
                    float x = io[f * ch + k];
                    st.delay[idx] = x + delayed * fb;
                    io[f * ch + k] = x * (1.0f - mix) + delayed * mix;
                }
                st.delayPos = (st.delayPos + 1) % st.delayFrames;
            }
            break;
        }
        case AudioFXType::WowFlutter: {
            if (st.delayFrames <= 0 || st.delay.empty()) break;
            float depth = std::clamp(fx.amount, 0.0f, 1.0f);
            float rate = std::clamp(fx.param1, 0.1f, 12.0f);
            float phaseInc = 2.0f * kPi * rate / static_cast<float>(sampleRate_);
            float baseFrames = (kWowBaseMs / 1000.0f) * sampleRate_;
            float depthFrames = (kWowDepthMs / 1000.0f) * sampleRate_ * depth;
            for (int f = 0; f < frameCount; ++f) {
                float mod = baseFrames + depthFrames * std::sin(st.lfoPhase);
                st.lfoPhase += phaseInc;
                if (st.lfoPhase > 2.0f * kPi) st.lfoPhase -= 2.0f * kPi;
                for (int k = 0; k < ch; ++k) {
                    st.delay[static_cast<size_t>(st.delayPos) * ch + k] = io[f * ch + k];
                    float readPos = static_cast<float>(st.delayPos) - mod;
                    while (readPos < 0.0f) readPos += st.delayFrames;
                    int i0 = static_cast<int>(readPos) % st.delayFrames;
                    int i1 = (i0 + 1) % st.delayFrames;
                    float frac = readPos - std::floor(readPos);
                    float s0 = st.delay[static_cast<size_t>(i0) * ch + k];
                    float s1 = st.delay[static_cast<size_t>(i1) * ch + k];
                    io[f * ch + k] = s0 + (s1 - s0) * frac;
                }
                st.delayPos = (st.delayPos + 1) % st.delayFrames;
            }
            break;
        }
        case AudioFXType::Reverb: {
            if (st.combA.empty()) break;
            float room = std::clamp(fx.param1, 0.0f, 1.0f);
            float damp = std::clamp(fx.param2, 0.0f, 1.0f);
            float mix = std::clamp(fx.mix, 0.0f, 1.0f);
            float fb = 0.7f + room * 0.28f;
            float dampA = damp;
            for (int f = 0; f < frameCount; ++f) {
                for (int k = 0; k < ch; ++k) {
                    float x = io[f * ch + k];
                    size_t ia = static_cast<size_t>(st.combAPos) * ch + k;
                    size_t ib = static_cast<size_t>(st.combBPos) * ch + k;
                    float ca = st.combA[ia];
                    float cb = st.combB[ib];
                    st.lp[k] = ca * (1.0f - dampA) + st.lp[k] * dampA;
                    st.combA[ia] = x + st.lp[k] * fb;
                    st.combB[ib] = x + cb * fb;
                    float wet = (ca + cb) * 0.5f;
                    size_t ip = static_cast<size_t>(st.allpassPos) * ch + k;
                    float ap = st.allpass[ip];
                    float y = -0.5f * wet + ap;
                    st.allpass[ip] = wet + ap * 0.5f;
                    io[f * ch + k] = x * (1.0f - mix) + y * mix;
                }
                st.combAPos = (st.combAPos + 1) % st.combAFrames;
                st.combBPos = (st.combBPos + 1) % st.combBFrames;
                st.allpassPos = (st.allpassPos + 1) % st.allpassFrames;
            }
            break;
        }
        case AudioFXType::NoiseHiss: {
            float level = std::clamp(fx.amount, 0.0f, 1.0f) * 0.2f;
            if (level <= 0.0f) break;
            for (int f = 0; f < frameCount; ++f) {
                float n = nextNoise();
                st.toneState = 0.5f * n + 0.5f * st.toneState;
                float hiss = (n - st.toneState) * level;
                for (int k = 0; k < ch; ++k) io[f * ch + k] += hiss;
            }
            break;
        }
        case AudioFXType::Pitch:
        default:
            break;
    }
}

void AudioFXProcessor::process(float* io, int frameCount) {
    if (!io || frameCount <= 0) return;
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (!chainActive_ || channels_ <= 0) return;

    const int ch = channels_;
    for (int i = 0; i < count_ && i < static_cast<int>(state_.size()); ++i) {
        if (!effects_[i].enabled || effects_[i].type == AudioFXType::Pitch) continue;
        applyEffect(effects_[i], state_[i], io, frameCount);
    }

    if (ch >= 2 && stereoWidth_ < 0.999f) {
        float w = std::clamp(stereoWidth_, 0.0f, 1.0f);
        for (int f = 0; f < frameCount; ++f) {
            float l = io[f * ch + 0];
            float r = io[f * ch + 1];
            float mid = (l + r) * 0.5f;
            float side = (l - r) * 0.5f * w;
            io[f * ch + 0] = mid + side;
            io[f * ch + 1] = mid - side;
        }
    }
}
