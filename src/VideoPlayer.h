#pragma once
#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../include/Graphics/OpenGL.h"
class AudioSystem;
class VideoPlayer {
    public:
        VideoPlayer();
        ~VideoPlayer();
        bool LoadVideo(const std::string& path, bool startWorker = true);
        void Play();
        void Pause();
        void Stop();
        void SetLoop(bool loop);
        void SetPlaybackSpeed(float speed);
        void SetPointFiltering(bool enabled);
        void SetAudioSystem(AudioSystem* audioSystem, int streamId);
        void SetPlayAudioFromVideo(bool enabled);
        void SetSyncAudioToVideo(bool enabled);
        void SetAudioSyncTolerance(float toleranceSeconds);
        void Update(float deltaSeconds);
        GLuint GetTextureId() const { return m_textureId; }
        bool HasTextureOverride() const { return m_textureId != 0 && m_hasPresentedFrame; }
        bool IsLoaded() const { return m_loaded; }
        bool IsPlaying() const { return m_playing && !m_paused; }
        const std::string& GetLoadedPath() const { return m_loadedPath; }
        std::string GetLastError() const;
        int GetWidth() const { return m_width; }
        int GetHeight() const { return m_height; }
        double GetDurationSeconds() const { return m_durationSeconds; }
    private:
        struct DecoderState;
        struct AudioBufferSource;
        struct FrameSlot        {std::vector<unsigned char> pixels; double ptsSeconds = 0.0;};
        enum class DecodeResult {FrameReady, EndOfStream, Error};
        static constexpr size_t kFrameQueueCapacity = 12;
        static constexpr size_t kFrameQueuePrerollCount = 4;
        void StartWorker();
        void StopWorker();
        void DestroyTexture();
        void ShutdownDecoder();
        void DetachFromAudioSystem();
        void ShutdownAudioOutput();
        void ClearQueuedFramesLocked();
        bool OpenDecoder(const std::string& path);
        bool EnsureTextureAllocated();
        bool EnsureConversionContextForFrame();
        void UploadPixelsToTexture(const unsigned char* pixels, int width, int height);
        void ApplyTextureFilter();
        bool SeekToStart();
        bool SeekAudioToTime(double seconds);
        DecodeResult DecodeIntoSlot(FrameSlot& slot, double& outPtsSeconds);
        bool FillAudioBuffer();
        double ResolveFramePtsSeconds() const;
        void RefreshAudioBinding();
        void SyncAudioToPlayback();
        void WorkerMain();
        void SetLastError(std::string error);
        std::unique_ptr<DecoderState> m_decoder;
        std::unique_ptr<AudioBufferSource> m_audioBufferSource;
        std::thread m_workerThread;
        std::mutex m_queueMutex;
        mutable std::mutex m_stateMutex;
        std::condition_variable m_queueCv;
        std::array<FrameSlot, kFrameQueueCapacity> m_frameQueue;
        std::vector<unsigned char> m_uploadBuffer;
        size_t m_queueReadIndex = 0;
        size_t m_queueCount = 0;
        bool m_workerExitRequested = false;
        bool m_requestSeekToStart = false;
        bool m_requestAudioSeek = false;
        double m_requestedAudioSeekSeconds = 0.0;
        bool m_decoderReachedEnd = false;
        bool m_audioDecoderReachedEnd = false;
        bool m_waitingForBuffer = false;
        std::string m_loadedPath;
        std::string m_lastError;
        int m_width = 0;
        int m_height = 0;
        double m_durationSeconds = 0.0;
        double m_playbackTimeSeconds = 0.0;
        float m_playbackSpeed = 1.0f;
        bool m_loop = true;
        bool m_playing = false;
        bool m_paused = false;
        bool m_loaded = false;
        bool m_pointFiltering = false;
        bool m_hasPresentedFrame = false;
        bool m_playAudioFromVideo = true;
        bool m_syncAudioToVideo = true;
        float m_audioSyncToleranceSeconds = 0.05f;
        double m_lastAudioSyncSeekPlaybackSeconds = -1.0;
        bool m_audioWarningLogged = false;
        bool m_audioStreamAttached = false;
        AudioSystem* m_audioSystem = nullptr;
        int m_audioStreamId = -1;
        GLuint m_textureId = 0;
};
