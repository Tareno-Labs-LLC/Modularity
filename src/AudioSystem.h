#pragma once
#include "Common.h"
#include "SceneObject.h"
#include "Camera.h"
#include "AudioFX.h"
#include "../include/ThirdParty/miniaudio.h"
#include <unordered_map>
#include <unordered_set>
#include <memory>
struct AudioClipPreview {
    bool loaded = false;
    std::string path;
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    double durationSeconds = 0.0;
    std::vector<float> waveform;
    std::vector<float> waveformLeft;
    std::vector<float> waveformRight;
};
class AudioSystem {
public:
    struct DecodedAudioData {
        std::vector<float> pcmFrames;
        ma_audio_buffer_ref buffer{};
        ma_uint32 channels = 0;
        ma_uint32 sampleRate = 0;
        ma_uint64 frameCount = 0;
        bool initialized = false;
    };
    bool init();
    void shutdown();
    bool isReady() const { return initialized; }
    void onPlayStart(const std::vector<SceneObject>& objects);
    void onPlayStop();
    void update(const std::vector<SceneObject>& objects, const Camera& listenerCamera, bool playing);
    bool playPreview(const std::string& path, float volume = 1.0f, bool loop = false);
    bool playOneShot(const std::string& path, float volume = 1.0f, float pitch = 1.0f);
    // Editor interface sounds. Dry (never through the scene's reverb), never in the preview slot
    // the asset / Inspector audio player uses, and not cut when Play mode starts or stops.
    bool playUiSound(const std::string& path, float volume);
    void stopPreview();
    bool playWeatherAmbience(const std::string& path, float volume);
    void stopWeatherAmbience();
    const std::string& getWeatherAmbiencePath() const { return weatherAmbiencePath; }
    bool isPreviewing(const std::string& path) const;
    const AudioClipPreview* getPreview(const std::string& path);
    bool getPreviewTime(const std::string& path, double& cursorSeconds, double& durationSeconds) const;
    bool seekPreview(const std::string& path, double seconds);
    bool setPreviewLoop(bool loop);
    bool setPreviewVolume(float volume);
    void setPrefer2DSpatialAudio(bool enabled) { prefer2DSpatialAudio = enabled; }
    bool playObjectSound(const SceneObject& obj);
    bool playObjectOneShot(const SceneObject& obj, const std::string& clipPathOverride = "", float volumeScale = 1.0f);
    bool stopObjectSound(int objectId);
    bool setObjectLoop(const SceneObject& obj, bool loop);
    bool setObjectVolume(const SceneObject& obj, float volume);
    bool attachVideoStream(int streamId, ma_data_source* dataSource);
    void detachVideoStream(int streamId);
    bool configureVideoStream(int streamId, const SceneObject* routeObject, float volume, bool muted, bool loop, float pitch);
    bool setVideoStreamPlaying(int streamId, bool playing);
    bool seekVideoStreamToSeconds(int streamId, double seconds);
    bool getVideoStreamCursorSeconds(int streamId, double& cursorSeconds) const;

    void setGlobalAudioFX(const AudioFXChain& chain);
    AudioFXChain getGlobalAudioFX() const { return globalFXChain; }

    struct AudioFXNode {
        ma_node_base baseNode;
        AudioFXProcessor* processor = nullptr;
        int channels = 0;
    };
    struct SimpleReverbNode {
        ma_node_base baseNode;
        int channels = 0;
        int sampleRate = 0;
        std::vector<std::vector<float>> combBuffers;
        std::vector<size_t> combIndex;
        std::vector<std::vector<float>> allpassBuffers;
        std::vector<size_t> allpassIndex;
        std::vector<float> preDelayBuffer;
        size_t preDelayIndex = 0;
        std::vector<float> reflectionsBuffer;
        size_t reflectionsIndex = 0;
        std::vector<float> lpState;
        float wetGain = 0.0f;
        float reflectionsGain = 0.0f;
        float decayTime = 1.5f;
        float decayHFRatio = 0.5f;
        float diffusion = 100.0f;
        float density = 100.0f;
        float hfReference = 5000.0f;
        float preDelaySeconds = 0.01f;
        float reflectionsDelaySeconds = 0.01f;
        size_t preDelayMaxFrames = 0;
        size_t reflectionsMaxFrames = 0;
    };
private:
    struct ReverbSettings {
        float room = -10000.0f;
        float roomHF = -10000.0f;
        float roomLF = -10000.0f;
        float decayTime = 1.5f;
        float decayHFRatio = 0.5f;
        float reflections = -10000.0f;
        float reflectionsDelay = 0.01f;
        float reverb = -10000.0f;
        float reverbDelay = 0.01f;
        float hfReference = 5000.0f;
        float lfReference = 250.0f;
        float roomRolloffFactor = 0.0f;
        float diffusion = 100.0f;
        float density = 100.0f;
        float dry = 1.0f;
    };
    struct ActiveSound {
        ma_sound sound;
        std::string clipPath;
        bool spatial = true;
        bool started = false;
        std::shared_ptr<DecodedAudioData> decodedData;
        std::unique_ptr<AudioFXProcessor> fxProcessor;
        AudioFXNode fxNode{};
        bool fxNodeReady = false;
        std::string fxSignature;
    };
    struct OneShotSound {
        ma_sound sound{};
        std::shared_ptr<DecodedAudioData> decodedData;
    };
    struct VideoStreamSound {
        ma_sound sound{};
        ma_data_source* dataSource = nullptr;
    };
    ma_engine engine{};
    ma_resource_manager resourceManager{};
    bool resourceManagerInitialized = false;
    bool initialized = false;
    std::unordered_map<int, std::unique_ptr<ActiveSound>> activeSounds;
    std::vector<std::unique_ptr<OneShotSound>> oneShotSounds;
    std::vector<std::unique_ptr<OneShotSound>> uiSounds;  // playUiSound(); only shutdown clears it
    std::unordered_map<int, std::unique_ptr<VideoStreamSound>> videoStreams;
    std::unordered_map<std::string, AudioClipPreview> previewCache;
    std::unordered_set<std::string> missingClips;
    SimpleReverbNode reverbNode{};
    ma_splitter_node reverbSplitter{};
    ma_sound_group reverbGroup{};
    bool reverbReady = false;
    ReverbSettings currentReverb{};
    AudioFXProcessor globalFXProcessor;
    AudioFXNode globalFXNode{};
    bool globalFXReady = false;
    AudioFXChain globalFXChain;
    std::string globalFXSignature;
    bool globalFXFromComponent = false;
    ma_sound previewSound{};
    bool previewActive = false;
    std::string previewPath;
    std::shared_ptr<DecodedAudioData> previewDecodedData;

    ma_sound weatherSound{};
    bool weatherActive = false;
    std::string weatherAmbiencePath;
    std::shared_ptr<DecodedAudioData> weatherDecodedData;
    // The engine asks for the ambience every frame, so a path that fails to load is left
    // alone until this time (steady clock, seconds) instead of re-read from disk each frame.
    std::string weatherFailedPath;
    double weatherRetryAfterSec = 0.0;
    bool prefer2DSpatialAudio = false;
    glm::vec3 lastListenerPosition = glm::vec3(0.0f);
    void destroyActiveSounds();
    void destroyOneShotSounds();
    void destroyVideoStreams();
    void cleanupFinishedOneShots();
    void cleanupFinishedUiSounds(bool destroyAll);
    bool ensureSoundFor(const SceneObject& obj);
    void refreshSoundParams(const SceneObject& obj, ActiveSound& snd);
    void applyAudioSourceParams(const SceneObject& obj, ma_sound& sound, float baseGain, bool loop);
    bool shouldUsePlanar2DAudio(const SceneObject& obj) const;
    float computePlanarPan(const SceneObject& obj, const glm::vec3& listenerPos, float spatialBlend) const;
    float computeDistanceAttenuation(const SceneObject& obj, const glm::vec3& listenerPos, const glm::vec3& sourcePos) const;
    glm::vec3 computeSpatializedPosition(const SceneObject& obj, float spatialBlend, const glm::vec3& listenerPos) const;
    float computeCustomAttenuation(const SceneObject& obj, const glm::vec3& listenerPos, const glm::vec3& sourcePos) const;
    AudioClipPreview loadPreview(const std::string& path);
    std::shared_ptr<DecodedAudioData> decodeClipToMemory(const std::string& path);
    bool initSoundFromPath(const std::string& path, ma_uint32 flags, ma_sound_group* group, ma_sound& sound, std::shared_ptr<DecodedAudioData>& decodedData);
    void releaseDecodedAudio(std::shared_ptr<DecodedAudioData>& decodedData);
    void updateReverb(const std::vector<SceneObject>& objects, const glm::vec3& listenerPos);
    ReverbSettings getReverbTarget(const std::vector<SceneObject>& objects, const glm::vec3& listenerPos, float& outBlend) const;
    void applyReverbSettings(const ReverbSettings& target, float blend);
    void shutdownReverbGraph();
    ma_node* masterBusInputNode() const;
    void initGlobalFXNode(ma_uint32 channels, ma_uint32 sampleRate);
    void updateGlobalFXFromObjects(const std::vector<SceneObject>& objects);
    void updateSourceFX(const SceneObject& obj, ActiveSound& snd);
    void destroySourceFX(ActiveSound& snd);
    void teardownSound(ActiveSound& snd);
};
