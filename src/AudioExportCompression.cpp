#include "AudioExportCompression.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#if MODULARITY_HAS_FFMPEG
extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/audio_fifo.h>
    #include <libavutil/avutil.h>
    #include <libavutil/channel_layout.h>
    #include <libavutil/error.h>
    #include <libswresample/swresample.h>
}
#endif

namespace fs = std::filesystem;

namespace Modularity::AudioExport {
namespace {

std::string Utf8(const fs::path& path) {
    const auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

#if MODULARITY_HAS_FFMPEG

std::string FfmpegError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

struct EncodeResources {
    AVFormatContext* input = nullptr;
    AVCodecContext* decoder = nullptr;
    AVCodecContext* encoder = nullptr;
    AVFormatContext* output = nullptr;
    SwrContext* resampler = nullptr;
    AVAudioFifo* fifo = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* decoded = nullptr;
    AVFrame* chunk = nullptr;
    uint8_t** converted = nullptr;
    int convertedCapacity = 0;

    ~EncodeResources() {
        if (converted) {
            av_freep(&converted[0]);
            av_freep(&converted);
        }
        av_frame_free(&chunk);
        av_frame_free(&decoded);
        av_packet_free(&packet);
        if (fifo) av_audio_fifo_free(fifo);
        swr_free(&resampler);
        if (output) {
            if (output->pb) avio_closep(&output->pb);
            avformat_free_context(output);
        }
        avcodec_free_context(&encoder);
        avcodec_free_context(&decoder);
        avformat_close_input(&input);
    }
};

bool EncodeWithFfmpeg(const fs::path& inputPath, const fs::path& outputPath, float quality, std::string& error) {
    EncodeResources r;
    auto fail = [&](const std::string& what, int code) {
        error = what + (code < 0 ? ": " + FfmpegError(code) : std::string());
        return false;
    };

    const std::string inputUtf8 = Utf8(inputPath);
    const std::string outputUtf8 = Utf8(outputPath);

    int ret = avformat_open_input(&r.input, inputUtf8.c_str(), nullptr, nullptr);
    if (ret < 0) return fail("cannot open input", ret);
    ret = avformat_find_stream_info(r.input, nullptr);
    if (ret < 0) return fail("cannot read stream info", ret);

    const AVCodec* decoderCodec = nullptr;
    const int streamIndex = av_find_best_stream(r.input, AVMEDIA_TYPE_AUDIO, -1, -1, &decoderCodec, 0);
    if (streamIndex < 0 || !decoderCodec) return fail("no audio stream", streamIndex);

    r.decoder = avcodec_alloc_context3(decoderCodec);
    if (!r.decoder) return fail("out of memory", 0);
    ret = avcodec_parameters_to_context(r.decoder, r.input->streams[streamIndex]->codecpar);
    if (ret < 0) return fail("bad decoder parameters", ret);
    ret = avcodec_open2(r.decoder, decoderCodec, nullptr);
    if (ret < 0) return fail("cannot open decoder", ret);
    if (r.decoder->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        const int channels = r.decoder->ch_layout.nb_channels;
        av_channel_layout_uninit(&r.decoder->ch_layout);
        av_channel_layout_default(&r.decoder->ch_layout, channels);
    }

    const AVCodec* encoderCodec = avcodec_find_encoder_by_name("libvorbis");
    if (!encoderCodec) return fail("FFmpeg has no libvorbis encoder", 0);

    ret = avformat_alloc_output_context2(&r.output, nullptr, "ogg", outputUtf8.c_str());
    if (ret < 0 || !r.output) return fail("cannot create Ogg output", ret);

    r.encoder = avcodec_alloc_context3(encoderCodec);
    if (!r.encoder) return fail("out of memory", 0);
    ret = av_channel_layout_copy(&r.encoder->ch_layout, &r.decoder->ch_layout);
    if (ret < 0) return fail("bad channel layout", ret);
    r.encoder->sample_rate = r.decoder->sample_rate;
    r.encoder->sample_fmt = AV_SAMPLE_FMT_FLTP;  // the only format libvorbis takes
    r.encoder->time_base = AVRational{1, r.decoder->sample_rate};
    r.encoder->flags |= AV_CODEC_FLAG_QSCALE;
    r.encoder->global_quality = static_cast<int>(std::clamp(quality, 0.0f, 10.0f) * FF_QP2LAMBDA);
    if (r.output->oformat->flags & AVFMT_GLOBALHEADER) {
        r.encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    ret = avcodec_open2(r.encoder, encoderCodec, nullptr);
    if (ret < 0) return fail("cannot open Vorbis encoder", ret);

    AVStream* outStream = avformat_new_stream(r.output, nullptr);
    if (!outStream) return fail("cannot create output stream", 0);
    ret = avcodec_parameters_from_context(outStream->codecpar, r.encoder);
    if (ret < 0) return fail("bad encoder parameters", ret);
    outStream->time_base = r.encoder->time_base;

    ret = avio_open(&r.output->pb, outputUtf8.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) return fail("cannot open output file", ret);
    ret = avformat_write_header(r.output, nullptr);
    if (ret < 0) return fail("cannot write Ogg header", ret);

    ret = swr_alloc_set_opts2(&r.resampler,
                              &r.encoder->ch_layout, r.encoder->sample_fmt, r.encoder->sample_rate,
                              &r.decoder->ch_layout, r.decoder->sample_fmt, r.decoder->sample_rate,
                              0, nullptr);
    if (ret < 0) return fail("cannot configure resampler", ret);
    ret = swr_init(r.resampler);
    if (ret < 0) return fail("cannot start resampler", ret);

    const int channels = r.encoder->ch_layout.nb_channels;
    // libvorbis fixes frame_size (64); every frame but the last must be exactly that long.
    const int frameSize = r.encoder->frame_size > 0 ? r.encoder->frame_size : 1024;
    r.fifo = av_audio_fifo_alloc(r.encoder->sample_fmt, channels, frameSize * 8);
    r.packet = av_packet_alloc();
    r.decoded = av_frame_alloc();
    r.chunk = av_frame_alloc();
    if (!r.fifo || !r.packet || !r.decoded || !r.chunk) return fail("out of memory", 0);

    r.chunk->format = r.encoder->sample_fmt;
    r.chunk->sample_rate = r.encoder->sample_rate;
    av_channel_layout_copy(&r.chunk->ch_layout, &r.encoder->ch_layout);
    r.chunk->nb_samples = frameSize;
    ret = av_frame_get_buffer(r.chunk, 0);
    if (ret < 0) return fail("out of memory", ret);

    int64_t nextPts = 0;
    auto drainEncoder = [&]() -> int {
        for (;;) {
            int rc = avcodec_receive_packet(r.encoder, r.packet);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return 0;
            if (rc < 0) return rc;
            av_packet_rescale_ts(r.packet, r.encoder->time_base, outStream->time_base);
            r.packet->stream_index = outStream->index;
            rc = av_interleaved_write_frame(r.output, r.packet);
            if (rc < 0) return rc;
        }
    };
    auto encodeFromFifo = [&](bool flushPartial) -> int {
        while (av_audio_fifo_size(r.fifo) >= frameSize ||
               (flushPartial && av_audio_fifo_size(r.fifo) > 0)) {
            const int samples = std::min(av_audio_fifo_size(r.fifo), frameSize);
            int rc = av_frame_make_writable(r.chunk);
            if (rc < 0) return rc;
            r.chunk->nb_samples = samples;
            if (av_audio_fifo_read(r.fifo, reinterpret_cast<void**>(r.chunk->data), samples) < samples) {
                return AVERROR(EIO);
            }
            r.chunk->pts = nextPts;
            nextPts += samples;
            rc = avcodec_send_frame(r.encoder, r.chunk);
            if (rc < 0) return rc;
            rc = drainEncoder();
            if (rc < 0) return rc;
        }
        return 0;
    };
    auto pushConverted = [&](const uint8_t** input, int inputSamples) -> int {
        const int capacity = swr_get_out_samples(r.resampler, inputSamples);
        if (capacity <= 0) return 0;
        if (capacity > r.convertedCapacity) {
            if (r.converted) {
                av_freep(&r.converted[0]);
                av_freep(&r.converted);
            }
            int rc = av_samples_alloc_array_and_samples(&r.converted, nullptr, channels, capacity,
                                                        r.encoder->sample_fmt, 0);
            if (rc < 0) return rc;
            r.convertedCapacity = capacity;
        }
        const int produced = swr_convert(r.resampler, r.converted, capacity, input, inputSamples);
        if (produced < 0) return produced;
        if (produced > 0 &&
            av_audio_fifo_write(r.fifo, reinterpret_cast<void**>(r.converted), produced) < produced) {
            return AVERROR(ENOMEM);
        }
        return encodeFromFifo(false);
    };
    auto receiveDecoded = [&]() -> int {
        for (;;) {
            int rc = avcodec_receive_frame(r.decoder, r.decoded);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return 0;
            if (rc < 0) return rc;
            rc = pushConverted(static_cast<const uint8_t**>(static_cast<void*>(r.decoded->extended_data)),
                               r.decoded->nb_samples);
            av_frame_unref(r.decoded);
            if (rc < 0) return rc;
        }
    };

    while ((ret = av_read_frame(r.input, r.packet)) >= 0) {
        if (r.packet->stream_index == streamIndex) {
            ret = avcodec_send_packet(r.decoder, r.packet);
            av_packet_unref(r.packet);
            if (ret < 0) return fail("decode failed", ret);
            ret = receiveDecoded();
            if (ret < 0) return fail("encode failed", ret);
        } else {
            av_packet_unref(r.packet);
        }
    }
    if (ret != AVERROR_EOF) return fail("read failed", ret);

    ret = avcodec_send_packet(r.decoder, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return fail("decoder flush failed", ret);
    ret = receiveDecoded();
    if (ret < 0) return fail("encode failed", ret);
    ret = pushConverted(nullptr, 0);  // resampler's buffered tail
    if (ret < 0) return fail("resampler flush failed", ret);
    ret = encodeFromFifo(true);
    if (ret < 0) return fail("encode failed", ret);
    ret = avcodec_send_frame(r.encoder, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return fail("encoder flush failed", ret);
    ret = drainEncoder();
    if (ret < 0) return fail("encoder flush failed", ret);
    ret = av_write_trailer(r.output);
    if (ret < 0) return fail("cannot finish Ogg file", ret);
    return true;
}

#endif

uint64_t HashFileContents(const fs::path& path, bool& ok) {
    ok = false;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return 0;
    uint64_t hash = 1469598103934665603ull;
    std::vector<char> buffer(1 << 20);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = in.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<size_t>(i)]);
            hash *= 1099511628211ull;
        }
    }
    ok = true;
    return hash;
}

} // namespace

bool EncoderAvailable() {
#if MODULARITY_HAS_FFMPEG
    return avcodec_find_encoder_by_name("libvorbis") != nullptr;
#else
    return false;
#endif
}

std::string FfmpegVersion() {
#if MODULARITY_HAS_FFMPEG
    const char* version = av_version_info();
    return version ? version : std::string();
#else
    return {};
#endif
}

std::string FfmpegConfiguration() {
#if MODULARITY_HAS_FFMPEG
    const char* configuration = avcodec_configuration();
    return configuration ? configuration : std::string();
#else
    return {};
#endif
}

bool EncodeFileToVorbis(const fs::path& input, const fs::path& output, float quality, std::string& error) {
#if MODULARITY_HAS_FFMPEG
    // Byte-identical clips share one cache entry, so two export threads can encode the same
    // output at once: give each its own temp file, and if another thread's rename won, use that.
    static std::atomic<uint64_t> counter{0};
    std::error_code ec;
    fs::path temp = output;
    temp += ".encoding-" + std::to_string(counter.fetch_add(1));
    if (!EncodeWithFfmpeg(input, temp, quality, error)) {
        fs::remove(temp, ec);
        return false;
    }
    fs::rename(temp, output, ec);
    if (ec) {
        std::error_code existsEc;
        const bool otherThreadFinished = fs::exists(output, existsEc) && !existsEc;
        if (!otherThreadFinished) {
            error = "cannot move encoded file into place: " + ec.message();
        }
        fs::remove(temp, existsEc);
        return otherThreadFinished;
    }
    return true;
#else
    (void)input;
    (void)output;
    (void)quality;
    error = "this build has no FFmpeg";
    return false;
#endif
}

CompressionStats CompressStagedAudio(const fs::path& stageRoot,
                                     const fs::path& cacheDir,
                                     float quality,
                                     const std::function<void(const std::string&)>& log,
                                     const std::function<bool()>& cancelled) {
    CompressionStats stats;
    std::vector<fs::path> clips;
    std::error_code ec;
    const fs::path assetsRoot = stageRoot / "Assets";
    for (auto it = fs::recursive_directory_iterator(assetsRoot, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code entryEc;
        if (it->is_regular_file(entryEc) && IsLosslessClipPath(it->path().filename().string())) {
            clips.push_back(it->path());
        }
    }
    if (clips.empty()) return stats;

    fs::create_directories(cacheDir, ec);
    const int qualityTag = static_cast<int>(std::clamp(quality, 0.0f, 10.0f) * 10.0f + 0.5f);

    std::mutex mutex;
    std::vector<std::string> failures;
    std::atomic<size_t> next{0};
    auto worker = [&]() {
        for (;;) {
            if (cancelled && cancelled()) return;
            const size_t index = next.fetch_add(1);
            if (index >= clips.size()) return;
            const fs::path& clip = clips[index];

            std::error_code fileEc;
            const uint64_t originalSize = fs::file_size(clip, fileEc);
            bool hashed = false;
            const uint64_t hash = HashFileContents(clip, hashed);
            if (fileEc || !hashed) {
                std::lock_guard<std::mutex> lock(mutex);
                ++stats.failed;
                failures.push_back(clip.string() + ": cannot read");
                continue;
            }

            char cacheName[64];
            std::snprintf(cacheName, sizeof(cacheName), "%016llx-q%d.ogg",
                          static_cast<unsigned long long>(hash), qualityTag);
            const fs::path cached = cacheDir / cacheName;
            bool fromCache = fs::exists(cached, fileEc) && !fileEc;
            if (!fromCache) {
                std::string error;
                if (!EncodeFileToVorbis(clip, cached, quality, error)) {
                    std::lock_guard<std::mutex> lock(mutex);
                    ++stats.failed;
                    failures.push_back(clip.string() + ": " + error);
                    continue;
                }
            }

            const uint64_t encodedSize = fs::file_size(cached, fileEc);
            std::lock_guard<std::mutex> lock(mutex);
            // Tiny clips can come out no smaller (Ogg and Vorbis headers are a few KB); keep those
            // as they are and skip the decode cost.
            if (fileEc || encodedSize * 10 >= originalSize * 9) {
                ++stats.keptOriginal;
                continue;
            }
            fs::path sidecar = clip;
            sidecar += kSidecarSuffix;
            std::error_code copyEc;
            fs::copy_file(cached, sidecar, fs::copy_options::overwrite_existing, copyEc);
            if (copyEc) {
                ++stats.failed;
                failures.push_back(clip.string() + ": cannot stage encoded clip: " + copyEc.message());
                continue;
            }
            fs::remove(clip, copyEc);
            ++stats.converted;
            if (fromCache) ++stats.reusedFromCache;
            stats.bytesBefore += originalSize;
            stats.bytesAfter += encodedSize;
        }
    };

    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const size_t threadCount = std::min<size_t>(clips.size(), std::max(1u, hardware - 1));
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (size_t i = 0; i < threadCount; ++i) threads.emplace_back(worker);
    for (std::thread& thread : threads) thread.join();

    if (log) {
        for (const std::string& failure : failures) {
            log("Warning: audio left uncompressed - " + failure);
        }
    }
    return stats;
}

} // namespace Modularity::AudioExport
