#include "VideoPlayer.h"
#include "AudioSystem.h"
#include <algorithm>
#include <cmath>
#include <iostream>

#if !MODULARITY_HAS_FFMPEG

struct VideoPlayer::DecoderState {};
struct VideoPlayer::AudioBufferSource {};

VideoPlayer::VideoPlayer() = default;
VideoPlayer::~VideoPlayer() = default;

bool VideoPlayer::LoadVideo(const std::string&, bool) { return false; }
void VideoPlayer::Play() {}
void VideoPlayer::Pause() {}
void VideoPlayer::Stop() {}
void VideoPlayer::SetLoop(bool loop) { m_loop = loop; }
void VideoPlayer::SetPlaybackSpeed(float speed) { m_playbackSpeed = speed; }
void VideoPlayer::SetPointFiltering(bool enabled) { m_pointFiltering = enabled; }
void VideoPlayer::SetAudioSystem(AudioSystem* audioSystem, int streamId) {
    m_audioSystem = audioSystem; m_audioStreamId = streamId;
}
void VideoPlayer::SetPlayAudioFromVideo(bool enabled) { m_playAudioFromVideo = enabled; }
void VideoPlayer::SetSyncAudioToVideo(bool enabled) { m_syncAudioToVideo = enabled; }
void VideoPlayer::SetAudioSyncTolerance(float t) { m_audioSyncToleranceSeconds = t; }
void VideoPlayer::Update(float) {}
std::string VideoPlayer::GetLastError() const { return m_lastError; }
void VideoPlayer::SetLastError(std::string error) { m_lastError = std::move(error); }
void VideoPlayer::UploadPixelsToTexture(const unsigned char*, int, int) {}
void VideoPlayer::StartWorker() {}
void VideoPlayer::StopWorker() {}

#else

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/error.h>
    #include <libavutil/frame.h>
    #include <libavutil/samplefmt.h>
    #include <libavutil/opt.h>
    #include <libswresample/swresample.h>
    #include <libswscale/swscale.h>
}

namespace {
constexpr ma_uint32 kAudioBufferTargetMilliseconds = 1500;
constexpr ma_uint32 kAudioBufferMinDecodeFrames = 2048;

std::string FfmpegErrorString(int errorCode) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errorCode, buffer, sizeof(buffer));
    return std::string(buffer);
}

double RationalToSeconds(const AVRational rational) {
    return (rational.num > 0 && rational.den > 0) ? av_q2d(rational) : 0.0;
}

int GetChannelCount(const AVChannelLayout& layout) {
    return layout.nb_channels > 0 ? layout.nb_channels : 0;
}

int GetCodecContextChannels(const AVCodecContext* codecContext) {
    if (codecContext == nullptr) return 0;
    const int layoutChannels = GetChannelCount(codecContext->ch_layout);
    if (layoutChannels > 0) return layoutChannels;
#if FF_API_OLD_CHANNEL_LAYOUT
    if (codecContext->channels > 0) return codecContext->channels;
#endif
    return 0;
}

int64_t SecondsToStreamTimestamp(double seconds, AVRational timeBase) {
    return av_rescale_q(static_cast<int64_t>(std::max(0.0, seconds) * static_cast<double>(AV_TIME_BASE)),
                        AVRational{1, AV_TIME_BASE},
                        timeBase);
}
} // namespace

struct VideoPlayer::AudioBufferSource {
    ma_data_source_base base{};
    ma_pcm_rb ringBuffer{};
    ma_uint32 channels = 0;
    ma_uint32 sampleRate = 0;
    ma_uint64 lengthInFrames = 0;
    ma_uint64 cursorInFrames = 0;
    bool initialized = false;

    static ma_result Read(ma_data_source* dataSource, void* framesOut, ma_uint64 frameCount, ma_uint64* framesRead) {
        auto* self = reinterpret_cast<AudioBufferSource*>(dataSource);
        if (framesRead) *framesRead = 0;
        if (!self || !self->initialized || self->channels == 0) {
            return MA_INVALID_OPERATION;
        }

        float* out = static_cast<float*>(framesOut);
        ma_uint64 totalRead = 0;
        while (totalRead < frameCount) {
            ma_uint32 chunkFrames = static_cast<ma_uint32>(std::min<ma_uint64>(frameCount - totalRead, 4096));
            void* readPtr = nullptr;
            ma_result result = ma_pcm_rb_acquire_read(&self->ringBuffer, &chunkFrames, &readPtr);
            if (result != MA_SUCCESS) {
                return result;
            }

            if (chunkFrames == 0 || readPtr == nullptr) {
                if (out) {
                    std::fill_n(out + totalRead * self->channels,
                                static_cast<size_t>((frameCount - totalRead) * self->channels),
                                0.0f);
                }
                totalRead = frameCount;
                break;
            }

            if (out) {
                std::copy_n(static_cast<const float*>(readPtr),
                            static_cast<size_t>(chunkFrames * self->channels),
                            out + totalRead * self->channels);
            }
            ma_pcm_rb_commit_read(&self->ringBuffer, chunkFrames);
            totalRead += chunkFrames;
        }

        self->cursorInFrames += totalRead;
        if (framesRead) *framesRead = totalRead;
        return MA_SUCCESS;
    }

    static ma_result Seek(ma_data_source* dataSource, ma_uint64 frameIndex) {
        auto* self = reinterpret_cast<AudioBufferSource*>(dataSource);
        if (!self || !self->initialized) return MA_INVALID_OPERATION;
        ma_pcm_rb_reset(&self->ringBuffer);
        self->cursorInFrames = frameIndex;
        return MA_SUCCESS;
    }

    static ma_result GetDataFormat(ma_data_source* dataSource, ma_format* format, ma_uint32* channels, ma_uint32* sampleRate,
                                   ma_channel* channelMap, size_t channelMapCap) {
        auto* self = reinterpret_cast<AudioBufferSource*>(dataSource);
        if (!self || !self->initialized) return MA_INVALID_OPERATION;
        if (format) *format = ma_format_f32;
        if (channels) *channels = self->channels;
        if (sampleRate) *sampleRate = self->sampleRate;
        if (channelMap && channelMapCap > 0) {
            ma_channel_map_init_standard(ma_standard_channel_map_default, channelMap, channelMapCap, self->channels);
        }
        return MA_SUCCESS;
    }

    static ma_result GetCursor(ma_data_source* dataSource, ma_uint64* cursor) {
        auto* self = reinterpret_cast<AudioBufferSource*>(dataSource);
        if (!self || !self->initialized || cursor == nullptr) return MA_INVALID_OPERATION;
        *cursor = self->cursorInFrames;
        return MA_SUCCESS;
    }

    static ma_result GetLength(ma_data_source* dataSource, ma_uint64* length) {
        auto* self = reinterpret_cast<AudioBufferSource*>(dataSource);
        if (!self || !self->initialized || length == nullptr) return MA_INVALID_OPERATION;
        *length = self->lengthInFrames;
        return self->lengthInFrames > 0 ? MA_SUCCESS : MA_NOT_IMPLEMENTED;
    }

    static ma_result SetLooping(ma_data_source* dataSource, ma_bool32 isLooping) {
        (void)dataSource;
        (void)isLooping;
        return MA_SUCCESS;
    }

    bool Init(ma_uint32 initChannels, ma_uint32 initSampleRate, ma_uint64 initLengthFrames) {
        Uninit();
        if (initChannels == 0 || initSampleRate == 0) return false;

        channels = initChannels;
        sampleRate = initSampleRate;
        lengthInFrames = initLengthFrames;

        ma_data_source_config config = ma_data_source_config_init();
        static ma_data_source_vtable vtable = {
            Read,
            Seek,
            GetDataFormat,
            GetCursor,
            GetLength,
            SetLooping,
            0
        };
        config.vtable = &vtable;

        ma_result result = ma_data_source_init(&config, reinterpret_cast<ma_data_source*>(this));
        if (result != MA_SUCCESS) {
            return false;
        }

        const ma_uint32 capacityFrames = std::max<ma_uint32>(
            kAudioBufferMinDecodeFrames * 4,
            static_cast<ma_uint32>((static_cast<uint64_t>(sampleRate) * kAudioBufferTargetMilliseconds) / 1000));
        result = ma_pcm_rb_init(ma_format_f32, channels, capacityFrames, nullptr, nullptr, &ringBuffer);
        if (result != MA_SUCCESS) {
            ma_data_source_uninit(reinterpret_cast<ma_data_source*>(this));
            return false;
        }

        cursorInFrames = 0;
        initialized = true;
        return true;
    }

    void Uninit() {
        if (initialized) {
            ma_pcm_rb_uninit(&ringBuffer);
            ma_data_source_uninit(reinterpret_cast<ma_data_source*>(this));
            initialized = false;
        }
        channels = 0;
        sampleRate = 0;
        lengthInFrames = 0;
        cursorInFrames = 0;
    }

    void Reset(ma_uint64 cursorFrames = 0) {
        if (!initialized) return;
        ma_pcm_rb_reset(&ringBuffer);
        cursorInFrames = cursorFrames;
    }

    ma_uint32 AvailableReadFrames() const {
        return initialized ? ma_pcm_rb_available_read(const_cast<ma_pcm_rb*>(&ringBuffer)) : 0;
    }

    ma_uint32 AvailableWriteFrames() const {
        return initialized ? ma_pcm_rb_available_write(const_cast<ma_pcm_rb*>(&ringBuffer)) : 0;
    }

    bool WriteFrames(const float* frames, ma_uint32 frameCount) {
        if (!initialized || frames == nullptr || frameCount == 0) return false;

        ma_uint32 remainingFrames = frameCount;
        const float* cursor = frames;
        while (remainingFrames > 0) {
            ma_uint32 chunkFrames = remainingFrames;
            void* writePtr = nullptr;
            ma_result result = ma_pcm_rb_acquire_write(&ringBuffer, &chunkFrames, &writePtr);
            if (result != MA_SUCCESS || chunkFrames == 0 || writePtr == nullptr) {
                return false;
            }

            std::copy_n(cursor, static_cast<size_t>(chunkFrames * channels), static_cast<float*>(writePtr));
            ma_pcm_rb_commit_write(&ringBuffer, chunkFrames);
            cursor += static_cast<size_t>(chunkFrames * channels);
            remainingFrames -= chunkFrames;
        }

        return true;
    }

    ma_data_source* DataSource() {
        return reinterpret_cast<ma_data_source*>(this);
    }
};

struct VideoPlayer::DecoderState {
    AVFormatContext* videoFormatContext = nullptr;
    AVCodecContext* videoCodecContext = nullptr;
    SwsContext* swsContext = nullptr;
    AVPacket* videoPacket = nullptr;
    AVFrame* videoFrame = nullptr;
    int videoStreamIndex = -1;
    AVRational videoTimeBase = AVRational{1, 1};
    double fallbackFrameDurationSeconds = 1.0 / 30.0;
    mutable double nextFallbackPtsSeconds = 0.0;
    bool drainingVideoDecoder = false;
    bool hasPendingVideoPacket = false;

    AVFormatContext* audioFormatContext = nullptr;
    AVCodecContext* audioCodecContext = nullptr;
    SwrContext* swrContext = nullptr;
    AVPacket* audioPacket = nullptr;
    AVFrame* audioFrame = nullptr;
    int audioStreamIndex = -1;
    AVRational audioTimeBase = AVRational{1, 1};
    ma_uint32 audioOutputChannels = 0;
    ma_uint32 audioOutputSampleRate = 0;
    ma_uint64 audioLengthFrames = 0;
    std::vector<float> audioConvertBuffer;
    bool hasAudioStream = false;
    bool drainingAudioDecoder = false;
    bool hasPendingAudioPacket = false;
};

VideoPlayer::VideoPlayer() = default;

VideoPlayer::~VideoPlayer() {
    StopWorker();
    ShutdownAudioOutput();
    ShutdownDecoder();
    DestroyTexture();
}

std::string VideoPlayer::GetLastError() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_lastError;
}

void VideoPlayer::SetLastError(std::string error) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_lastError = std::move(error);
}

bool VideoPlayer::LoadVideo(const std::string& path, bool startWorker) {
    StopWorker();
    ShutdownAudioOutput();
    ShutdownDecoder();

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ClearQueuedFramesLocked();
        m_requestSeekToStart = false;
        m_requestAudioSeek = false;
        m_decoderReachedEnd = false;
        m_audioDecoderReachedEnd = false;
        m_waitingForBuffer = false;
    }

    m_loaded = false;
    m_loadedPath.clear();
    SetLastError("");
    m_width = 0;
    m_height = 0;
    m_durationSeconds = 0.0;
    m_playbackTimeSeconds = 0.0;
    m_playing = false;
    m_paused = false;
    m_hasPresentedFrame = false;
    m_waitingForBuffer = false;
    m_audioWarningLogged = false;
    m_lastAudioSyncSeekPlaybackSeconds = -1.0;

    if (path.empty()) {
        SetLastError("Video path is empty.");
        return false;
    }

    if (!OpenDecoder(path)) {
        return false;
    }

    if (!EnsureTextureAllocated()) {
        ShutdownDecoder();
        SetLastError("Failed to allocate OpenGL texture for video playback.");
        return false;
    }

    const size_t pixelBytes = static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * 4u;
    m_uploadBuffer.assign(pixelBytes, 0u);
    for (FrameSlot& slot : m_frameQueue) {
        slot.pixels.assign(pixelBytes, 0u);
        slot.ptsSeconds = 0.0;
    }

    double firstFramePtsSeconds = 0.0;
    const DecodeResult firstDecodeResult = DecodeIntoSlot(m_frameQueue[0], firstFramePtsSeconds);
    if (firstDecodeResult == DecodeResult::FrameReady) {
        m_frameQueue[0].ptsSeconds = firstFramePtsSeconds;
        m_queueReadIndex = 0;
        m_queueCount = 1;
        UploadPixelsToTexture(m_frameQueue[0].pixels.data(), m_width, m_height);
        m_hasPresentedFrame = true;
    } else if (firstDecodeResult == DecodeResult::Error) {
        ShutdownDecoder();
        SetLastError(GetLastError().empty() ? "Failed to decode the first video frame." : GetLastError());
        return false;
    } else {
        ShutdownDecoder();
        SetLastError("The video stream ended before a frame could be decoded.");
        return false;
    }

    if (m_audioBufferSource) {
        m_audioBufferSource->Reset(0);
        FillAudioBuffer();
    }

    m_loaded = true;
    m_loadedPath = path;
    RefreshAudioBinding();
    if (startWorker) {
        StartWorker();
    } else {
        ShutdownAudioOutput();
        ShutdownDecoder();
    }
    return true;
}

void VideoPlayer::Play() {
    if (!m_loaded) return;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_decoderReachedEnd && m_queueCount == 0 && !m_loop) {
            m_requestSeekToStart = true;
            m_decoderReachedEnd = false;
            m_audioDecoderReachedEnd = false;
            m_playbackTimeSeconds = 0.0;
            m_hasPresentedFrame = false;
        }
        m_waitingForBuffer = !m_decoderReachedEnd && m_queueCount < kFrameQueuePrerollCount;
    }

    m_playing = true;
    m_paused = false;
    RefreshAudioBinding();
    if (m_audioStreamAttached && m_audioSystem && !m_waitingForBuffer) {
        m_audioSystem->setVideoStreamPlaying(m_audioStreamId, true);
    }
    m_queueCv.notify_all();
}

void VideoPlayer::Pause() {
    if (!m_loaded) return;
    if (m_paused) return;
    m_paused = true;
    if (m_audioStreamAttached && m_audioSystem) {
        m_audioSystem->setVideoStreamPlaying(m_audioStreamId, false);
    }
}

void VideoPlayer::Stop() {
    if (!m_loaded) return;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ClearQueuedFramesLocked();
        m_requestSeekToStart = true;
        m_requestAudioSeek = false;
        m_decoderReachedEnd = false;
        m_audioDecoderReachedEnd = false;
        m_waitingForBuffer = true;
    }

    if (m_audioBufferSource) {
        m_audioBufferSource->Reset(0);
    }
    if (m_audioStreamAttached && m_audioSystem) {
        m_audioSystem->setVideoStreamPlaying(m_audioStreamId, false);
        m_audioSystem->seekVideoStreamToSeconds(m_audioStreamId, 0.0);
    }

    m_playbackTimeSeconds = 0.0;
    m_lastAudioSyncSeekPlaybackSeconds = -1.0;
    m_playing = false;
    m_paused = false;
    m_hasPresentedFrame = false;
    m_queueCv.notify_all();
}

void VideoPlayer::SetLoop(bool loop) {
    m_loop = loop;
    m_queueCv.notify_all();
}

void VideoPlayer::SetPlaybackSpeed(float speed) {
    m_playbackSpeed = std::max(0.0f, speed);
}

void VideoPlayer::SetPointFiltering(bool enabled) {
    if (m_pointFiltering == enabled) return;
    m_pointFiltering = enabled;
    ApplyTextureFilter();
}

void VideoPlayer::SetAudioSystem(AudioSystem* audioSystem, int streamId) {
    if (m_audioSystem == audioSystem && m_audioStreamId == streamId) {
        return;
    }
    DetachFromAudioSystem();
    m_audioSystem = audioSystem;
    m_audioStreamId = streamId;
    RefreshAudioBinding();
}

void VideoPlayer::SetPlayAudioFromVideo(bool enabled) {
    if (m_playAudioFromVideo == enabled) return;
    m_playAudioFromVideo = enabled;
    RefreshAudioBinding();
    if (!m_playAudioFromVideo && m_audioSystem && m_audioStreamAttached) {
        m_audioSystem->setVideoStreamPlaying(m_audioStreamId, false);
    }
}

void VideoPlayer::SetSyncAudioToVideo(bool enabled) {
    m_syncAudioToVideo = enabled;
}

void VideoPlayer::SetAudioSyncTolerance(float toleranceSeconds) {
    m_audioSyncToleranceSeconds = std::max(0.0f, toleranceSeconds);
}

void VideoPlayer::Update(float deltaSeconds) {
    if (!m_loaded || m_decoder == nullptr || m_textureId == 0) {
        return;
    }

    RefreshAudioBinding();

    bool bufferReadyToPlay = true;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_waitingForBuffer) {
            bufferReadyToPlay = m_decoderReachedEnd || m_queueCount >= kFrameQueuePrerollCount;
            if (bufferReadyToPlay) {
                m_waitingForBuffer = false;
            }
        } else if (!m_decoderReachedEnd && m_queueCount == 0) {
            m_waitingForBuffer = true;
            bufferReadyToPlay = false;
        }
    }

    if (m_audioStreamAttached && m_audioSystem && m_playing && !m_paused) {
        m_audioSystem->setVideoStreamPlaying(m_audioStreamId, bufferReadyToPlay);
    }

    if (m_playing && !m_paused && bufferReadyToPlay) {
        const float clampedDelta = std::max(0.0f, deltaSeconds);
        m_playbackTimeSeconds += static_cast<double>(clampedDelta) * static_cast<double>(m_playbackSpeed);
        if (!m_loop && m_durationSeconds > 0.0) {
            m_playbackTimeSeconds = std::min(m_playbackTimeSeconds, m_durationSeconds);
        }
    }

    SyncAudioToPlayback();

    constexpr double kPresentLeadSeconds = 0.0015;
    bool shouldUpload = false;
    int uploadWidth = 0;
    int uploadHeight = 0;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);

        while (m_queueCount > 1) {
            const size_t nextIndex = (m_queueReadIndex + 1) % kFrameQueueCapacity;
            if (m_frameQueue[nextIndex].ptsSeconds <= m_playbackTimeSeconds + kPresentLeadSeconds) {
                m_queueReadIndex = nextIndex;
                --m_queueCount;
                continue;
            }
            break;
        }

        if (m_queueCount > 0) {
            const FrameSlot& front = m_frameQueue[m_queueReadIndex];
            const bool shouldPrimeTexture = !m_hasPresentedFrame;
            if (shouldPrimeTexture || front.ptsSeconds <= m_playbackTimeSeconds + kPresentLeadSeconds) {
                if (m_uploadBuffer.size() != front.pixels.size()) {
                    m_uploadBuffer.resize(front.pixels.size());
                }
                std::copy(front.pixels.begin(), front.pixels.end(), m_uploadBuffer.begin());
                shouldUpload = true;
                uploadWidth = m_width;
                uploadHeight = m_height;
                m_queueReadIndex = (m_queueReadIndex + 1) % kFrameQueueCapacity;
                --m_queueCount;
            }
        }
    }

    if (shouldUpload) {
        UploadPixelsToTexture(m_uploadBuffer.data(), uploadWidth, uploadHeight);
        m_hasPresentedFrame = true;
        m_queueCv.notify_all();
    }

    bool finishedPlayback = false;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        finishedPlayback = !m_loop &&
                           m_decoderReachedEnd &&
                           m_queueCount == 0 &&
                           m_durationSeconds > 0.0 &&
                           m_playbackTimeSeconds >= (m_durationSeconds - kPresentLeadSeconds);
    }
    if (finishedPlayback) {
        m_playing = false;
        m_paused = false;
        if (m_audioStreamAttached && m_audioSystem) {
            m_audioSystem->setVideoStreamPlaying(m_audioStreamId, false);
        }
    }
}

void VideoPlayer::UploadPixelsToTexture(const unsigned char* pixels, int width, int height) {
    if (m_textureId == 0 || pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }

    glBindTexture(GL_TEXTURE_2D, m_textureId);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
#ifdef GL_UNPACK_ROW_LENGTH
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
#ifdef GL_UNPACK_SKIP_PIXELS
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
#endif
#ifdef GL_UNPACK_SKIP_ROWS
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
#endif
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void VideoPlayer::StartWorker() {
    if (m_workerThread.joinable() || !m_loaded || m_decoder == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_workerExitRequested = false;
    }

    m_workerThread = std::thread(&VideoPlayer::WorkerMain, this);
}

void VideoPlayer::StopWorker() {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_workerExitRequested = true;
    }
    m_queueCv.notify_all();

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_workerExitRequested = false;
        ClearQueuedFramesLocked();
        m_requestSeekToStart = false;
        m_requestAudioSeek = false;
        m_decoderReachedEnd = false;
        m_audioDecoderReachedEnd = false;
        m_waitingForBuffer = false;
    }
}

void VideoPlayer::DestroyTexture() {
    if (m_textureId != 0) {
        glDeleteTextures(1, &m_textureId);
        m_textureId = 0;
    }
    m_hasPresentedFrame = false;
}

void VideoPlayer::ShutdownDecoder() {
    if (m_decoder == nullptr) {
        return;
    }

    if (m_decoder->videoPacket != nullptr) {
        av_packet_free(&m_decoder->videoPacket);
    }
    if (m_decoder->videoFrame != nullptr) {
        av_frame_free(&m_decoder->videoFrame);
    }
    if (m_decoder->audioPacket != nullptr) {
        av_packet_free(&m_decoder->audioPacket);
    }
    if (m_decoder->audioFrame != nullptr) {
        av_frame_free(&m_decoder->audioFrame);
    }
    if (m_decoder->swsContext != nullptr) {
        sws_freeContext(m_decoder->swsContext);
        m_decoder->swsContext = nullptr;
    }
    if (m_decoder->swrContext != nullptr) {
        swr_free(&m_decoder->swrContext);
    }
    if (m_decoder->videoCodecContext != nullptr) {
        avcodec_free_context(&m_decoder->videoCodecContext);
    }
    if (m_decoder->audioCodecContext != nullptr) {
        avcodec_free_context(&m_decoder->audioCodecContext);
    }
    if (m_decoder->videoFormatContext != nullptr) {
        avformat_close_input(&m_decoder->videoFormatContext);
    }
    if (m_decoder->audioFormatContext != nullptr) {
        avformat_close_input(&m_decoder->audioFormatContext);
    }

    m_decoder.reset();
}

void VideoPlayer::DetachFromAudioSystem() {
    if (m_audioSystem && m_audioStreamAttached && m_audioStreamId >= 0) {
        m_audioSystem->detachVideoStream(m_audioStreamId);
    }
    m_audioStreamAttached = false;
}

void VideoPlayer::ShutdownAudioOutput() {
    DetachFromAudioSystem();
    if (m_audioBufferSource) {
        m_audioBufferSource->Uninit();
        m_audioBufferSource.reset();
    }
}

void VideoPlayer::ClearQueuedFramesLocked() {
    m_queueReadIndex = 0;
    m_queueCount = 0;
    for (FrameSlot& slot : m_frameQueue) {
        slot.ptsSeconds = 0.0;
    }
}

bool VideoPlayer::OpenDecoder(const std::string& path) {
    avformat_network_init();

    std::unique_ptr<DecoderState> decoder = std::make_unique<DecoderState>();

    int result = avformat_open_input(&decoder->videoFormatContext, path.c_str(), nullptr, nullptr);
    if (result < 0) {
        SetLastError("Failed to open video '" + path + "': " + FfmpegErrorString(result));
        return false;
    }

    result = avformat_find_stream_info(decoder->videoFormatContext, nullptr);
    if (result < 0) {
        SetLastError("Failed to read video stream info: " + FfmpegErrorString(result));
        return false;
    }

    decoder->videoStreamIndex = av_find_best_stream(decoder->videoFormatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (decoder->videoStreamIndex < 0) {
        SetLastError("No video stream found in '" + path + "'.");
        return false;
    }

    AVStream* videoStream = decoder->videoFormatContext->streams[decoder->videoStreamIndex];
    const AVCodecParameters* videoParams = videoStream->codecpar;
    const AVCodec* videoCodec = avcodec_find_decoder(videoParams->codec_id);
    if (videoCodec == nullptr) {
        SetLastError("No decoder available for the video's codec.");
        return false;
    }

    decoder->videoCodecContext = avcodec_alloc_context3(videoCodec);
    if (decoder->videoCodecContext == nullptr) {
        SetLastError("Failed to allocate FFmpeg codec context.");
        return false;
    }

    result = avcodec_parameters_to_context(decoder->videoCodecContext, videoParams);
    if (result < 0) {
        SetLastError("Failed to copy codec parameters: " + FfmpegErrorString(result));
        return false;
    }

    result = avcodec_open2(decoder->videoCodecContext, videoCodec, nullptr);
    if (result < 0) {
        SetLastError("Failed to open video decoder: " + FfmpegErrorString(result));
        return false;
    }

    decoder->videoPacket = av_packet_alloc();
    decoder->videoFrame = av_frame_alloc();
    if (decoder->videoPacket == nullptr || decoder->videoFrame == nullptr) {
        SetLastError("Failed to allocate FFmpeg packet/frame buffers.");
        return false;
    }

    m_width = std::max(1, decoder->videoCodecContext->width);
    m_height = std::max(1, decoder->videoCodecContext->height);
    decoder->videoTimeBase = videoStream->time_base;

    AVRational frameRate = videoStream->avg_frame_rate;
    if (frameRate.num <= 0 || frameRate.den <= 0) {
        frameRate = videoStream->r_frame_rate;
    }
    if (frameRate.num > 0 && frameRate.den > 0) {
        decoder->fallbackFrameDurationSeconds = std::max(1.0 / 240.0, av_q2d(av_inv_q(frameRate)));
    } else {
        decoder->fallbackFrameDurationSeconds = 1.0 / 30.0;
    }

    if (videoStream->duration > 0) {
        m_durationSeconds = static_cast<double>(videoStream->duration) * RationalToSeconds(videoStream->time_base);
    } else if (decoder->videoFormatContext->duration > 0) {
        m_durationSeconds = static_cast<double>(decoder->videoFormatContext->duration) / static_cast<double>(AV_TIME_BASE);
    } else {
        m_durationSeconds = 0.0;
    }

    auto disableAudioStream = [&]() {
        if (decoder->audioPacket != nullptr) {
            av_packet_free(&decoder->audioPacket);
        }
        if (decoder->audioFrame != nullptr) {
            av_frame_free(&decoder->audioFrame);
        }
        if (decoder->swrContext != nullptr) {
            swr_free(&decoder->swrContext);
        }
        if (decoder->audioCodecContext != nullptr) {
            avcodec_free_context(&decoder->audioCodecContext);
        }
        if (decoder->audioFormatContext != nullptr) {
            avformat_close_input(&decoder->audioFormatContext);
        }
        decoder->audioStreamIndex = -1;
        decoder->audioTimeBase = AVRational{1, 1};
        decoder->audioOutputChannels = 0;
        decoder->audioOutputSampleRate = 0;
        decoder->audioLengthFrames = 0;
        decoder->audioConvertBuffer.clear();
        decoder->hasAudioStream = false;
    };

    result = avformat_open_input(&decoder->audioFormatContext, path.c_str(), nullptr, nullptr);
    if (result >= 0) {
        result = avformat_find_stream_info(decoder->audioFormatContext, nullptr);
    }

    if (result >= 0) {
        decoder->audioStreamIndex = av_find_best_stream(decoder->audioFormatContext, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    }

    if (result >= 0 && decoder->audioStreamIndex >= 0) {
        AVStream* audioStream = decoder->audioFormatContext->streams[decoder->audioStreamIndex];
        const AVCodecParameters* audioParams = audioStream->codecpar;
        const AVCodec* audioCodec = avcodec_find_decoder(audioParams->codec_id);
        if (audioCodec == nullptr) {
            std::cerr << "VideoPlayer: audio stream detected in '" << path << "', but no decoder is available.\n";
            disableAudioStream();
        } else {
            decoder->audioCodecContext = avcodec_alloc_context3(audioCodec);
            if (decoder->audioCodecContext == nullptr ||
                avcodec_parameters_to_context(decoder->audioCodecContext, audioParams) < 0 ||
                avcodec_open2(decoder->audioCodecContext, audioCodec, nullptr) < 0) {
                std::cerr << "VideoPlayer: failed to initialize embedded audio decoder for '" << path << "'.\n";
                disableAudioStream();
            } else {
                decoder->audioPacket = av_packet_alloc();
                decoder->audioFrame = av_frame_alloc();
                if (decoder->audioPacket == nullptr || decoder->audioFrame == nullptr) {
                    std::cerr << "VideoPlayer: failed to allocate embedded audio buffers for '" << path << "'.\n";
                    disableAudioStream();
                } else {
                    const int inputChannels = std::max(1, GetCodecContextChannels(decoder->audioCodecContext));
                    decoder->audioOutputChannels = static_cast<ma_uint32>(std::clamp(inputChannels, 1, 2));
                    decoder->audioOutputSampleRate = static_cast<ma_uint32>(std::max(1, decoder->audioCodecContext->sample_rate));
                    decoder->audioTimeBase = audioStream->time_base;
                    if (audioStream->duration > 0 && decoder->audioOutputSampleRate > 0) {
                        const double audioDurationSeconds = static_cast<double>(audioStream->duration) * RationalToSeconds(audioStream->time_base);
                        decoder->audioLengthFrames = static_cast<ma_uint64>(audioDurationSeconds * static_cast<double>(decoder->audioOutputSampleRate));
                    }

                    AVChannelLayout outputLayout{};
                    av_channel_layout_default(&outputLayout, static_cast<int>(decoder->audioOutputChannels));
                    result = swr_alloc_set_opts2(&decoder->swrContext,
                                                 &outputLayout,
                                                 AV_SAMPLE_FMT_FLT,
                                                 static_cast<int>(decoder->audioOutputSampleRate),
                                                 &decoder->audioCodecContext->ch_layout,
                                                 decoder->audioCodecContext->sample_fmt,
                                                 decoder->audioCodecContext->sample_rate,
                                                 0,
                                                 nullptr);
                    av_channel_layout_uninit(&outputLayout);
                    if (result < 0 || decoder->swrContext == nullptr || swr_init(decoder->swrContext) < 0) {
                        std::cerr << "VideoPlayer: failed to create embedded audio resampler for '" << path << "'.\n";
                        disableAudioStream();
                    } else {
                        decoder->hasAudioStream = true;
                    }
                }
            }
        }
    } else {
        disableAudioStream();
    }

    if (decoder->hasAudioStream) {
        m_audioBufferSource = std::make_unique<AudioBufferSource>();
        if (!m_audioBufferSource->Init(decoder->audioOutputChannels, decoder->audioOutputSampleRate, decoder->audioLengthFrames)) {
            std::cerr << "VideoPlayer: failed to initialize audio playback buffer for '" << path << "'.\n";
            m_audioBufferSource.reset();
            disableAudioStream();
        }
    }

    m_decoder = std::move(decoder);
    return true;
}

bool VideoPlayer::EnsureTextureAllocated() {
    if (m_width <= 0 || m_height <= 0) {
        return false;
    }

    if (m_textureId == 0) {
        glGenTextures(1, &m_textureId);
        if (m_textureId == 0) {
            return false;
        }
    }

    glBindTexture(GL_TEXTURE_2D, m_textureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    ApplyTextureFilter();
    glBindTexture(GL_TEXTURE_2D, m_textureId);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
#ifdef GL_UNPACK_ROW_LENGTH
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
#ifdef GL_UNPACK_SKIP_PIXELS
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
#endif
#ifdef GL_UNPACK_SKIP_ROWS
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
#endif
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

bool VideoPlayer::EnsureConversionContextForFrame() {
    if (m_decoder == nullptr || m_decoder->videoFrame == nullptr) {
        SetLastError("No decoded video frame is available for color conversion.");
        return false;
    }

    const int sourceWidth = std::max(1, m_decoder->videoFrame->width);
    const int sourceHeight = std::max(1, m_decoder->videoFrame->height);
    const AVPixelFormat sourceFormat = static_cast<AVPixelFormat>(m_decoder->videoFrame->format);
    if (sourceFormat == AV_PIX_FMT_NONE) {
        SetLastError("Decoded video frame reported an invalid pixel format.");
        return false;
    }

    SwsContext* swsContext = sws_getCachedContext(
        m_decoder->swsContext,
        sourceWidth,
        sourceHeight,
        sourceFormat,
        m_width,
        m_height,
        AV_PIX_FMT_RGBA,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);
    if (swsContext == nullptr) {
        SetLastError("Failed to create FFmpeg swscale conversion context.");
        return false;
    }

    m_decoder->swsContext = swsContext;
    return true;
}

void VideoPlayer::ApplyTextureFilter() {
    if (m_textureId == 0) return;

    GLint previouslyBoundTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previouslyBoundTexture);
    glBindTexture(GL_TEXTURE_2D, m_textureId);
    const GLint filter = m_pointFiltering ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previouslyBoundTexture));
}

bool VideoPlayer::SeekToStart() {
    if (m_decoder == nullptr || m_decoder->videoFormatContext == nullptr || m_decoder->videoCodecContext == nullptr) {
        return false;
    }

    const int seekResult = av_seek_frame(m_decoder->videoFormatContext, m_decoder->videoStreamIndex, 0, AVSEEK_FLAG_BACKWARD);
    if (seekResult < 0) {
        SetLastError("Failed to seek video back to the start: " + FfmpegErrorString(seekResult));
        return false;
    }

    avcodec_flush_buffers(m_decoder->videoCodecContext);
    av_frame_unref(m_decoder->videoFrame);
    av_packet_unref(m_decoder->videoPacket);
    m_decoder->nextFallbackPtsSeconds = 0.0;
    m_decoder->drainingVideoDecoder = false;
    m_decoder->hasPendingVideoPacket = false;
    m_decoderReachedEnd = false;

    if (!SeekAudioToTime(0.0)) {
        m_audioDecoderReachedEnd = false;
    }

    return true;
}

bool VideoPlayer::SeekAudioToTime(double seconds) {
    if (m_decoder == nullptr || !m_decoder->hasAudioStream || m_decoder->audioFormatContext == nullptr ||
        m_decoder->audioCodecContext == nullptr || m_audioBufferSource == nullptr) {
        return false;
    }

    const int64_t timestamp = SecondsToStreamTimestamp(seconds, m_decoder->audioTimeBase);
    const int seekResult = av_seek_frame(m_decoder->audioFormatContext, m_decoder->audioStreamIndex, timestamp, AVSEEK_FLAG_BACKWARD);
    if (seekResult < 0) {
        std::cerr << "VideoPlayer: failed to seek embedded audio for '" << m_loadedPath << "': "
                  << FfmpegErrorString(seekResult) << "\n";
        return false;
    }

    avcodec_flush_buffers(m_decoder->audioCodecContext);
    av_frame_unref(m_decoder->audioFrame);
    av_packet_unref(m_decoder->audioPacket);
    m_decoder->drainingAudioDecoder = false;
    m_decoder->hasPendingAudioPacket = false;
    m_audioDecoderReachedEnd = false;

    const ma_uint64 cursorFrames = static_cast<ma_uint64>(
        std::max(0.0, seconds) * static_cast<double>(m_decoder->audioOutputSampleRate));
    m_audioBufferSource->Reset(cursorFrames);
    return true;
}

VideoPlayer::DecodeResult VideoPlayer::DecodeIntoSlot(FrameSlot& slot, double& outPtsSeconds) {
    if (m_decoder == nullptr) {
        return DecodeResult::Error;
    }

    uint8_t* dstData[4] = { slot.pixels.data(), nullptr, nullptr, nullptr };
    int dstLinesize[4] = { m_width * 4, 0, 0, 0 };

    for (;;) {
        const int receiveResult = avcodec_receive_frame(m_decoder->videoCodecContext, m_decoder->videoFrame);
        if (receiveResult == 0) {
            if (!EnsureConversionContextForFrame()) {
                av_frame_unref(m_decoder->videoFrame);
                return DecodeResult::Error;
            }

            const int sourceHeight = std::max(1, m_decoder->videoFrame->height);
            const int scaledRows = sws_scale(m_decoder->swsContext,
                                             m_decoder->videoFrame->data,
                                             m_decoder->videoFrame->linesize,
                                             0,
                                             sourceHeight,
                                             dstData,
                                             dstLinesize);
            if (scaledRows <= 0) {
                SetLastError("Failed to convert decoded video frame to RGBA.");
                av_frame_unref(m_decoder->videoFrame);
                return DecodeResult::Error;
            }

            outPtsSeconds = ResolveFramePtsSeconds();
            av_frame_unref(m_decoder->videoFrame);
            return DecodeResult::FrameReady;
        }

        if (receiveResult == AVERROR_EOF) {
            m_decoder->drainingVideoDecoder = false;
            return DecodeResult::EndOfStream;
        }

        if (receiveResult != AVERROR(EAGAIN) && receiveResult != AVERROR_EOF) {
            SetLastError("Failed while decoding video frame: " + FfmpegErrorString(receiveResult));
            return DecodeResult::Error;
        }

        if (m_decoder->drainingVideoDecoder) {
            m_decoder->drainingVideoDecoder = false;
            return DecodeResult::EndOfStream;
        }

        if (!m_decoder->hasPendingVideoPacket) {
            const int readResult = av_read_frame(m_decoder->videoFormatContext, m_decoder->videoPacket);
            if (readResult == AVERROR_EOF) {
                m_decoder->drainingVideoDecoder = true;
                const int flushResult = avcodec_send_packet(m_decoder->videoCodecContext, nullptr);
                if (flushResult < 0 && flushResult != AVERROR_EOF) {
                    SetLastError("Failed to flush video decoder: " + FfmpegErrorString(flushResult));
                    return DecodeResult::Error;
                }
                continue;
            }
            if (readResult < 0) {
                SetLastError("Failed to read video packet: " + FfmpegErrorString(readResult));
                return DecodeResult::Error;
            }

            if (m_decoder->videoPacket->stream_index != m_decoder->videoStreamIndex) {
                av_packet_unref(m_decoder->videoPacket);
                continue;
            }

            m_decoder->hasPendingVideoPacket = true;
        }

        const int sendResult = avcodec_send_packet(m_decoder->videoCodecContext, m_decoder->videoPacket);
        if (sendResult == 0) {
            av_packet_unref(m_decoder->videoPacket);
            m_decoder->hasPendingVideoPacket = false;
            continue;
        }
        if (sendResult == AVERROR(EAGAIN)) {
            continue;
        }
        av_packet_unref(m_decoder->videoPacket);
        m_decoder->hasPendingVideoPacket = false;
        if (sendResult < 0) {
            SetLastError("Failed to submit packet to decoder: " + FfmpegErrorString(sendResult));
            return DecodeResult::Error;
        }
    }
}

bool VideoPlayer::FillAudioBuffer() {
    if (m_decoder == nullptr || !m_decoder->hasAudioStream || m_audioBufferSource == nullptr) {
        return true;
    }

    while (!m_audioDecoderReachedEnd &&
           m_audioBufferSource->AvailableWriteFrames() >= kAudioBufferMinDecodeFrames) {
        const int receiveResult = avcodec_receive_frame(m_decoder->audioCodecContext, m_decoder->audioFrame);
        if (receiveResult == 0) {
            const int maxOutputFrames = swr_get_out_samples(m_decoder->swrContext, m_decoder->audioFrame->nb_samples);
            if (maxOutputFrames <= 0) {
                av_frame_unref(m_decoder->audioFrame);
                continue;
            }

            m_decoder->audioConvertBuffer.resize(
                static_cast<size_t>(maxOutputFrames) * static_cast<size_t>(m_decoder->audioOutputChannels));
            uint8_t* outData[1] = {
                reinterpret_cast<uint8_t*>(m_decoder->audioConvertBuffer.data())
            };

            const int convertedFrames = swr_convert(m_decoder->swrContext,
                                                    outData,
                                                    maxOutputFrames,
                                                    const_cast<const uint8_t**>(m_decoder->audioFrame->extended_data),
                                                    m_decoder->audioFrame->nb_samples);
            av_frame_unref(m_decoder->audioFrame);
            if (convertedFrames < 0) {
                SetLastError("Failed to convert embedded audio samples: " + FfmpegErrorString(convertedFrames));
                return false;
            }

            if (convertedFrames > 0 &&
                !m_audioBufferSource->WriteFrames(m_decoder->audioConvertBuffer.data(), static_cast<ma_uint32>(convertedFrames))) {
                break;
            }
            continue;
        }

        if (receiveResult == AVERROR_EOF) {
            m_decoder->drainingAudioDecoder = false;
            m_audioDecoderReachedEnd = true;
            break;
        }

        if (receiveResult != AVERROR(EAGAIN) && receiveResult != AVERROR_EOF) {
            SetLastError("Failed while decoding embedded audio: " + FfmpegErrorString(receiveResult));
            return false;
        }

        if (m_decoder->drainingAudioDecoder) {
            m_decoder->drainingAudioDecoder = false;
            m_audioDecoderReachedEnd = true;
            break;
        }

        if (!m_decoder->hasPendingAudioPacket) {
            const int readResult = av_read_frame(m_decoder->audioFormatContext, m_decoder->audioPacket);
            if (readResult == AVERROR_EOF) {
                m_decoder->drainingAudioDecoder = true;
                const int flushResult = avcodec_send_packet(m_decoder->audioCodecContext, nullptr);
                if (flushResult < 0 && flushResult != AVERROR_EOF) {
                    SetLastError("Failed to flush embedded audio decoder: " + FfmpegErrorString(flushResult));
                    return false;
                }
                continue;
            }
            if (readResult < 0) {
                SetLastError("Failed to read embedded audio packet: " + FfmpegErrorString(readResult));
                return false;
            }
            if (m_decoder->audioPacket->stream_index != m_decoder->audioStreamIndex) {
                av_packet_unref(m_decoder->audioPacket);
                continue;
            }
            m_decoder->hasPendingAudioPacket = true;
        }

        const int sendResult = avcodec_send_packet(m_decoder->audioCodecContext, m_decoder->audioPacket);
        if (sendResult == 0) {
            av_packet_unref(m_decoder->audioPacket);
            m_decoder->hasPendingAudioPacket = false;
            continue;
        }
        if (sendResult == AVERROR(EAGAIN)) {
            continue;
        }

        av_packet_unref(m_decoder->audioPacket);
        m_decoder->hasPendingAudioPacket = false;
        if (sendResult < 0) {
            SetLastError("Failed to submit embedded audio packet: " + FfmpegErrorString(sendResult));
            return false;
        }
    }

    return true;
}

double VideoPlayer::ResolveFramePtsSeconds() const {
    if (m_decoder == nullptr || m_decoder->videoFrame == nullptr) {
        return 0.0;
    }

    const AVFrame* frame = m_decoder->videoFrame;
    int64_t timestamp = frame->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE) {
        timestamp = frame->pts;
    }

    if (timestamp != AV_NOPTS_VALUE) {
        const double seconds = static_cast<double>(timestamp) * RationalToSeconds(m_decoder->videoTimeBase);
        m_decoder->nextFallbackPtsSeconds = seconds + m_decoder->fallbackFrameDurationSeconds;
        return seconds;
    }

    const double fallback = m_decoder->nextFallbackPtsSeconds;
    m_decoder->nextFallbackPtsSeconds += m_decoder->fallbackFrameDurationSeconds;
    return fallback;
}

void VideoPlayer::RefreshAudioBinding() {
    const bool shouldAttach = m_loaded &&
                              m_playAudioFromVideo &&
                              m_audioSystem != nullptr &&
                              m_audioStreamId >= 0 &&
                              m_decoder != nullptr &&
                              m_decoder->hasAudioStream &&
                              m_audioBufferSource != nullptr;

    if (!shouldAttach) {
        if (m_audioStreamAttached && m_audioSystem) {
            m_audioSystem->detachVideoStream(m_audioStreamId);
        }
        m_audioStreamAttached = false;
        return;
    }

    if (!m_audioStreamAttached) {
        m_audioStreamAttached = m_audioSystem->attachVideoStream(m_audioStreamId, m_audioBufferSource->DataSource());
        if (!m_audioStreamAttached && !m_audioWarningLogged) {
            std::cerr << "VideoPlayer: embedded audio output initialization failed for '" << m_loadedPath
                      << "'. Video playback will continue without audio.\n";
            m_audioWarningLogged = true;
        }
        if (m_audioStreamAttached && m_playing && !m_paused) {
            m_audioSystem->setVideoStreamPlaying(m_audioStreamId, true);
        }
    }
}

void VideoPlayer::SyncAudioToPlayback() {
    if (!m_syncAudioToVideo ||
        !m_playAudioFromVideo ||
        !m_audioStreamAttached ||
        m_audioSystem == nullptr ||
        !m_playing ||
        m_paused) {
        return;
    }

    double audioCursorSeconds = 0.0;
    if (!m_audioSystem->getVideoStreamCursorSeconds(m_audioStreamId, audioCursorSeconds)) {
        return;
    }

    const double driftSeconds = audioCursorSeconds - m_playbackTimeSeconds;
    const double hardSeekThreshold = std::max(0.25, static_cast<double>(m_audioSyncToleranceSeconds));
    if (std::fabs(driftSeconds) <= hardSeekThreshold) {
        return;
    }

    if (audioCursorSeconds <= 0.0 && m_playbackTimeSeconds < 0.35) {
        return;
    }

    if (m_lastAudioSyncSeekPlaybackSeconds >= 0.0 &&
        std::fabs(m_playbackTimeSeconds - m_lastAudioSyncSeekPlaybackSeconds) < 0.25) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_requestAudioSeek = true;
        m_requestedAudioSeekSeconds = m_playbackTimeSeconds;
    }
    m_lastAudioSyncSeekPlaybackSeconds = m_playbackTimeSeconds;
    m_audioSystem->seekVideoStreamToSeconds(m_audioStreamId, m_playbackTimeSeconds);
    m_queueCv.notify_all();
}

void VideoPlayer::WorkerMain() {
    for (;;) {
        bool doVideoDecode = false;
        bool doAudioDecode = false;
        size_t writeIndex = 0;
        bool needFullRestart = false;
        bool needAudioSeek = false;
        double audioSeekSeconds = 0.0;

        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [&]() {
                const bool needsVideoFrame = !m_decoderReachedEnd && m_queueCount < kFrameQueueCapacity;
                const bool needsAudioFrames = m_playAudioFromVideo &&
                                             m_audioBufferSource != nullptr &&
                                             !m_audioDecoderReachedEnd &&
                                             m_audioBufferSource->AvailableWriteFrames() >= kAudioBufferMinDecodeFrames;
                const bool needsLoopRestart = m_loop && m_decoderReachedEnd && m_queueCount == 0;
                return m_workerExitRequested ||
                       m_requestSeekToStart ||
                       m_requestAudioSeek ||
                       needsLoopRestart ||
                       needsVideoFrame ||
                       needsAudioFrames;
            });

            if (m_workerExitRequested) {
                return;
            }

            if (m_requestSeekToStart) {
                needFullRestart = true;
                m_requestSeekToStart = false;
                ClearQueuedFramesLocked();
                m_waitingForBuffer = m_playing && !m_paused;
                if (m_audioBufferSource) {
                    m_audioBufferSource->Reset(0);
                }
            } else if (m_requestAudioSeek) {
                needAudioSeek = true;
                audioSeekSeconds = m_requestedAudioSeekSeconds;
                m_requestAudioSeek = false;
            } else if (m_loop && m_decoderReachedEnd && m_queueCount == 0) {
                needFullRestart = true;
                ClearQueuedFramesLocked();
                m_waitingForBuffer = m_playing && !m_paused;
                if (m_audioBufferSource) {
                    m_audioBufferSource->Reset(0);
                }
            } else {
                if (!m_decoderReachedEnd && m_queueCount < kFrameQueueCapacity) {
                    writeIndex = (m_queueReadIndex + m_queueCount) % kFrameQueueCapacity;
                    doVideoDecode = true;
                }
                if (m_playAudioFromVideo &&
                    m_audioBufferSource != nullptr &&
                    !m_audioDecoderReachedEnd &&
                    m_audioBufferSource->AvailableWriteFrames() >= kAudioBufferMinDecodeFrames) {
                    doAudioDecode = true;
                }
            }
        }

        if (needFullRestart) {
            if (!SeekToStart()) {
                return;
            }
            continue;
        }

        if (needAudioSeek) {
            SeekAudioToTime(audioSeekSeconds);
            continue;
        }

        if (doAudioDecode) {
            if (!FillAudioBuffer()) {
                std::cerr << "VideoPlayer audio decode error";
                if (!m_loadedPath.empty()) {
                    std::cerr << " for '" << m_loadedPath << "'";
                }
                const std::string lastError = GetLastError();
                if (!lastError.empty()) {
                    std::cerr << ": " << lastError;
                }
                std::cerr << std::endl;
                m_audioDecoderReachedEnd = true;
            }
        }

        if (!doVideoDecode) {
            continue;
        }

        double ptsSeconds = 0.0;
        const DecodeResult decodeResult = DecodeIntoSlot(m_frameQueue[writeIndex], ptsSeconds);

        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_workerExitRequested) {
            return;
        }

        if (decodeResult == DecodeResult::FrameReady) {
            m_frameQueue[writeIndex].ptsSeconds = ptsSeconds;
            ++m_queueCount;
            m_queueCv.notify_all();
            continue;
        }

        if (decodeResult == DecodeResult::EndOfStream) {
            m_decoderReachedEnd = true;
            m_queueCv.notify_all();
            continue;
        }

        m_decoderReachedEnd = true;
        std::cerr << "VideoPlayer decode error";
        if (!m_loadedPath.empty()) {
            std::cerr << " for '" << m_loadedPath << "'";
        }
        const std::string lastError = GetLastError();
        if (!lastError.empty()) {
            std::cerr << ": " << lastError;
        }
        std::cerr << std::endl;
    }
}

#endif // MODULARITY_HAS_FFMPEG
