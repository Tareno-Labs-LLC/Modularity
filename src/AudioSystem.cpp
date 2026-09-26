#define STB_VORBIS_HEADER_ONLY
#include "../include/ThirdParty/stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "../include/ThirdParty/miniaudio.h"
#include "AudioSystem.h"
#include <cmath>
#include <atomic>
#include <array>
#include "../include/Platform/AssetSource.h"
#include "AudioExportCompression.h"
#if defined(__ANDROID__)
#include <cstdio>
#endif
#if MODULARITY_HAS_SNDFILE
#include <sndfile.h>
#endif
#if MODULARITY_HAS_OPUSFILE
#include <opusfile.h>
#include <climits>
#include <cstdio>
#include <new>
#endif

#undef STB_VORBIS_HEADER_ONLY
#include "../include/ThirdParty/stb_vorbis.c"
#undef L
#undef C
#undef R
#undef PLAYBACK_MONO
#undef PLAYBACK_LEFT
#undef PLAYBACK_RIGHT

namespace {
constexpr size_t kPreviewBuckets = 800;
constexpr ma_uint32 kPreviewChunkFrames = 2048;
constexpr float kReverbSmoothing = 0.12f;
constexpr size_t kReverbCombCount = 4;
constexpr size_t kReverbAllpassCount = 2;
constexpr float kReverbPreDelayMaxSeconds = 0.2f;
constexpr float kReverbReflectionsMaxSeconds = 0.1f;

void BuildWaveformPreview(AudioClipPreview& preview, const float* samples, ma_uint64 totalFrames) {
    if (!samples || totalFrames == 0 || preview.channels == 0) {
        return;
    }

    const ma_uint64 framesPerBucket = std::max<ma_uint64>(1, totalFrames / kPreviewBuckets);
    preview.waveform.assign(static_cast<size_t>(kPreviewBuckets), 0.0f);
    if (preview.channels >= 2) {
        preview.waveformLeft.assign(static_cast<size_t>(kPreviewBuckets), 0.0f);
        preview.waveformRight.assign(static_cast<size_t>(kPreviewBuckets), 0.0f);
    }

    size_t bucketIndex = 0;
    ma_uint64 bucketCursor = 0;
    float bucketMax = 0.0f;
    float bucketMaxLeft = 0.0f;
    float bucketMaxRight = 0.0f;

    for (ma_uint64 frame = 0; frame < totalFrames && bucketIndex < preview.waveform.size(); ++frame) {
        const size_t frameOffset = static_cast<size_t>(frame * preview.channels);
        for (ma_uint32 channel = 0; channel < preview.channels; ++channel) {
            bucketMax = std::max(bucketMax, std::fabs(samples[frameOffset + channel]));
        }
        if (preview.channels >= 2) {
            bucketMaxLeft = std::max(bucketMaxLeft, std::fabs(samples[frameOffset]));
            bucketMaxRight = std::max(bucketMaxRight, std::fabs(samples[frameOffset + 1]));
        }

        bucketCursor++;
        if (bucketCursor >= framesPerBucket) {
            preview.waveform[bucketIndex] = std::clamp(bucketMax, 0.0f, 1.0f);
            if (preview.channels >= 2) {
                preview.waveformLeft[bucketIndex] = std::clamp(bucketMaxLeft, 0.0f, 1.0f);
                preview.waveformRight[bucketIndex] = std::clamp(bucketMaxRight, 0.0f, 1.0f);
            }
            bucketIndex++;
            bucketCursor = 0;
            bucketMax = 0.0f;
            bucketMaxLeft = 0.0f;
            bucketMaxRight = 0.0f;
        }
    }

    if (bucketIndex < preview.waveform.size() && bucketMax > 0.0f) {
        preview.waveform[bucketIndex] = std::clamp(bucketMax, 0.0f, 1.0f);
        if (preview.channels >= 2) {
            preview.waveformLeft[bucketIndex] = std::clamp(bucketMaxLeft, 0.0f, 1.0f);
            preview.waveformRight[bucketIndex] = std::clamp(bucketMaxRight, 0.0f, 1.0f);
        }
    }
}

float DbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

struct ReverbNodeVTable {
    ma_node_vtable vtable;
};

static void reverb_node_process(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn,
                                float** ppFramesOut, ma_uint32* pFrameCountOut) {
    if (!pNode || !ppFramesIn || !ppFramesOut) return;
    auto* node = reinterpret_cast<AudioSystem::SimpleReverbNode*>(pNode);
    (void)pFrameCountIn;
    const float* input = ppFramesIn[0];
    float* output = ppFramesOut[0];
    if (!input || !output) return;

    ma_uint32 frameCount = *pFrameCountOut;
    int channels = node->channels;
    float decayTime = std::max(0.1f, node->decayTime);
    float diffusion = std::clamp(node->diffusion, 0.0f, 100.0f);
    float density = std::clamp(node->density, 0.0f, 100.0f);
    float preDelaySeconds = std::clamp(node->preDelaySeconds, 0.0f, kReverbPreDelayMaxSeconds);
    float reflectionsDelaySeconds = std::clamp(node->reflectionsDelaySeconds, 0.0f, kReverbReflectionsMaxSeconds);
    size_t preDelayFrames = static_cast<size_t>(preDelaySeconds * static_cast<float>(node->sampleRate));
    size_t reflectionsDelayFrames = static_cast<size_t>(reflectionsDelaySeconds * static_cast<float>(node->sampleRate));
    float wetGain = std::clamp(node->wetGain, 0.0f, 2.0f);
    float reflectionsGain = std::clamp(node->reflectionsGain, 0.0f, 2.0f);

    float diffusionNorm = diffusion / 100.0f;
    float densityNorm = density / 100.0f;
    float allpassGain = 0.2f + 0.55f * diffusionNorm;
    float densityScale = 0.6f + 0.4f * densityNorm;
    float combGain = 1.0f / static_cast<float>(node->combBuffers.size());
    std::array<float, kReverbCombCount> combFeedback{};
    for (size_t i = 0; i < node->combBuffers.size(); ++i) {
        float delaySec = static_cast<float>(node->combBuffers[i].size() / channels) / static_cast<float>(node->sampleRate);
        combFeedback[i] = std::pow(10.0f, (-3.0f * delaySec) / decayTime) * densityScale;
    }

    float cutoffHz = std::clamp(node->hfReference * node->decayHFRatio, 500.0f, 20000.0f);
    float lpAlpha = std::exp(-2.0f * PI * cutoffHz / static_cast<float>(node->sampleRate));

    for (ma_uint32 frame = 0; frame < frameCount; ++frame) {
        size_t preReadIndex = node->preDelayMaxFrames > 0
            ? (node->preDelayIndex + node->preDelayMaxFrames - preDelayFrames) % node->preDelayMaxFrames
            : 0;
        size_t reflectionsReadIndex = node->reflectionsMaxFrames > 0
            ? (node->reflectionsIndex + node->reflectionsMaxFrames - reflectionsDelayFrames) % node->reflectionsMaxFrames
            : 0;

        for (int ch = 0; ch < channels; ++ch) {
            float inSample = input[frame * channels + ch];
            float preSample = inSample;
            if (!node->preDelayBuffer.empty()) {
                size_t writeBase = node->preDelayIndex * channels;
                size_t readBase = preReadIndex * channels;
                preSample = node->preDelayBuffer[readBase + ch];
                node->preDelayBuffer[writeBase + ch] = inSample;
            }

            float reflectionsSample = 0.0f;
            if (!node->reflectionsBuffer.empty()) {
                size_t writeBase = node->reflectionsIndex * channels;
                size_t readBase = reflectionsReadIndex * channels;
                reflectionsSample = node->reflectionsBuffer[readBase + ch];
                node->reflectionsBuffer[writeBase + ch] = preSample;
            }

            float combSum = 0.0f;
            for (size_t i = 0; i < node->combBuffers.size(); ++i) {
                auto& buffer = node->combBuffers[i];
                size_t idx = node->combIndex[i];
                size_t base = idx * channels + ch;
                float y = buffer[base];
                buffer[base] = preSample + y * combFeedback[i];
                combSum += y;
            }

            combSum *= combGain;
            float apOut = combSum;
            for (size_t i = 0; i < node->allpassBuffers.size(); ++i) {
                auto& buffer = node->allpassBuffers[i];
                size_t idx = node->allpassIndex[i];
                size_t base = idx * channels + ch;
                float buf = buffer[base];
                float y = -allpassGain * apOut + buf;
                buffer[base] = apOut + buf * allpassGain;
                apOut = y;
            }

            float wetSample = apOut * wetGain + reflectionsSample * reflectionsGain;
            float lp = node->lpState.empty() ? wetSample : (lpAlpha * node->lpState[ch] + (1.0f - lpAlpha) * wetSample);
            if (!node->lpState.empty()) node->lpState[ch] = lp;
            output[frame * channels + ch] = lp;
        }

        if (!node->preDelayBuffer.empty()) {
            node->preDelayIndex = (node->preDelayIndex + 1) % node->preDelayMaxFrames;
        }
        if (!node->reflectionsBuffer.empty()) {
            node->reflectionsIndex = (node->reflectionsIndex + 1) % node->reflectionsMaxFrames;
        }
        for (size_t i = 0; i < node->combIndex.size(); ++i) {
            node->combIndex[i] = (node->combIndex[i] + 1) % (node->combBuffers[i].size() / channels);
        }
        for (size_t i = 0; i < node->allpassIndex.size(); ++i) {
            node->allpassIndex[i] = (node->allpassIndex[i] + 1) % (node->allpassBuffers[i].size() / channels);
        }
    }
}

static ma_result reverb_node_get_required_input_frames(ma_node* pNode, ma_uint32 outputFrameCount, ma_uint32* pInputFrameCount) {
    (void)pNode;
    if (pInputFrameCount) *pInputFrameCount = outputFrameCount;
    return MA_SUCCESS;
}

static ma_node_vtable g_reverb_node_vtable = {
    reverb_node_process,
    reverb_node_get_required_input_frames,
    1,
    1,
    0
};

static void audiofx_node_process(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn,
                                 float** ppFramesOut, ma_uint32* pFrameCountOut) {
    if (!pNode || !ppFramesIn || !ppFramesOut) return;
    auto* node = reinterpret_cast<AudioSystem::AudioFXNode*>(pNode);
    (void)pFrameCountIn;
    const float* input = ppFramesIn[0];
    float* output = ppFramesOut[0];
    if (!input || !output) return;
    ma_uint32 frameCount = *pFrameCountOut;
    const int channels = node->channels;
    for (ma_uint32 i = 0; i < frameCount * static_cast<ma_uint32>(channels); ++i) output[i] = input[i];
    if (node->processor) node->processor->process(output, static_cast<int>(frameCount));
}

static ma_node_vtable g_audiofx_node_vtable = {
    audiofx_node_process,
    reverb_node_get_required_input_frames,
    1,
    1,
    0
};

#if MODULARITY_HAS_OPUSFILE

struct OpusReader {
    FILE* fp = nullptr;
    long baseOffset = 0; // bytes to skip before the real Ogg stream begins
    long fileSize = 0;
};

// Returns offset past an ID3v2 tag at the start of fp, or 0 if not present.
static long sniffID3v2Prefix(FILE* fp) {
    unsigned char header[10];
    if (std::fread(header, 1, 10, fp) != 10) {
        std::fseek(fp, 0, SEEK_SET);
        return 0;
    }
    std::fseek(fp, 0, SEEK_SET);
    if (header[0] != 'I' || header[1] != 'D' || header[2] != '3') return 0;
    // Syncsafe length: 4 bytes, each holds 7 bits.
    if ((header[6] | header[7] | header[8] | header[9]) & 0x80) return 0;
    long size = (static_cast<long>(header[6]) << 21)
              | (static_cast<long>(header[7]) << 14)
              | (static_cast<long>(header[8]) << 7)
              |  static_cast<long>(header[9]);
    long footer = (header[5] & 0x10) ? 10 : 0; // optional footer flag
    return 10 + size + footer;
}

static int opus_cb_read(void* stream, unsigned char* ptr, int nbytes) {
    OpusReader* r = static_cast<OpusReader*>(stream);
    return static_cast<int>(std::fread(ptr, 1, static_cast<size_t>(nbytes), r->fp));
}

static int opus_cb_seek(void* stream, opus_int64 offset, int whence) {
    OpusReader* r = static_cast<OpusReader*>(stream);
    long target = 0;
    switch (whence) {
        case SEEK_SET: target = r->baseOffset + static_cast<long>(offset); break;
        case SEEK_CUR: target = std::ftell(r->fp) + static_cast<long>(offset); break;
        case SEEK_END: target = r->fileSize + static_cast<long>(offset); break;
        default: return -1;
    }
    if (target < r->baseOffset) target = r->baseOffset;
    return std::fseek(r->fp, target, SEEK_SET);
}

static opus_int64 opus_cb_tell(void* stream) {
    OpusReader* r = static_cast<OpusReader*>(stream);
    long pos = std::ftell(r->fp);
    if (pos < 0) return -1;
    return pos - r->baseOffset;
}

static int opus_cb_close(void* stream) {
    OpusReader* r = static_cast<OpusReader*>(stream);
    if (r->fp) std::fclose(r->fp);
    delete r;
    return 0;
}

static OpusFileCallbacks g_opus_callbacks = {
    opus_cb_read,
    opus_cb_seek,
    opus_cb_tell,
    opus_cb_close
};

struct OpusDataSource {
    ma_data_source_base base{};
    OggOpusFile* opus = nullptr;
    ma_uint32 channels = 2;
    ma_uint32 sampleRate = 48000; // Opus decodes at a fixed 48kHz.
    ma_uint64 totalFrames = 0;
    ma_uint64 cursor = 0;
};

static ma_result opus_ds_read(ma_data_source* ds, void* framesOut, ma_uint64 frameCount, ma_uint64* framesRead) {
    OpusDataSource* self = static_cast<OpusDataSource*>(ds);
    float* out = static_cast<float*>(framesOut);
    ma_uint64 total = 0;
    while (total < frameCount) {
        int remaining = static_cast<int>(std::min<ma_uint64>(frameCount - total, INT_MAX / static_cast<ma_uint64>(self->channels)));
        int got = op_read_float(self->opus, out + total * self->channels, remaining * static_cast<int>(self->channels), nullptr);
        if (got < 0) {
            *framesRead = total;
            return MA_ERROR;
        }
        if (got == 0) break; // EOF
        total += static_cast<ma_uint64>(got);
    }
    self->cursor += total;
    *framesRead = total;
    return (total > 0 || frameCount == 0) ? MA_SUCCESS : MA_AT_END;
}

static ma_result opus_ds_seek(ma_data_source* ds, ma_uint64 frameIndex) {
    OpusDataSource* self = static_cast<OpusDataSource*>(ds);
    if (op_pcm_seek(self->opus, static_cast<ogg_int64_t>(frameIndex)) != 0) return MA_ERROR;
    self->cursor = frameIndex;
    return MA_SUCCESS;
}

static ma_result opus_ds_get_data_format(ma_data_source* ds, ma_format* format, ma_uint32* channels, ma_uint32* sampleRate, ma_channel* channelMap, size_t channelMapCap) {
    OpusDataSource* self = static_cast<OpusDataSource*>(ds);
    if (format) *format = ma_format_f32;
    if (channels) *channels = self->channels;
    if (sampleRate) *sampleRate = self->sampleRate;
    if (channelMap && channelMapCap > 0) {
        ma_channel_map_init_standard(ma_standard_channel_map_default, channelMap, channelMapCap, self->channels);
    }
    return MA_SUCCESS;
}

static ma_result opus_ds_get_cursor(ma_data_source* ds, ma_uint64* cursor) {
    *cursor = static_cast<OpusDataSource*>(ds)->cursor;
    return MA_SUCCESS;
}

static ma_result opus_ds_get_length(ma_data_source* ds, ma_uint64* length) {
    *length = static_cast<OpusDataSource*>(ds)->totalFrames;
    return MA_SUCCESS;
}

static ma_data_source_vtable g_opus_ds_vtable = {
    opus_ds_read,
    opus_ds_seek,
    opus_ds_get_data_format,
    opus_ds_get_cursor,
    opus_ds_get_length,
    nullptr,
    0
};

static ma_result opus_backend_open(const char* path, ma_data_source** ppBackend, const ma_allocation_callbacks* allocationCallbacks) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return MA_DOES_NOT_EXIST;

    std::fseek(fp, 0, SEEK_END);
    long fileSize = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    long base = sniffID3v2Prefix(fp);
    if (base > 0 && base < fileSize) {
        std::fseek(fp, base, SEEK_SET);
    } else {
        base = 0;
    }

    OpusReader* reader = new (std::nothrow) OpusReader();
    if (!reader) { std::fclose(fp); return MA_OUT_OF_MEMORY; }
    reader->fp = fp;
    reader->baseOffset = base;
    reader->fileSize = fileSize;

    int err = 0;
    OggOpusFile* of = op_open_callbacks(reader, &g_opus_callbacks, nullptr, 0, &err);
    if (!of) {
        // op_open_callbacks calls close on failure per libopusfile contract,
        // which routes through opus_cb_close and frees the reader+fp.
        return MA_INVALID_FILE;
    }

    OpusDataSource* self = static_cast<OpusDataSource*>(
        ma_malloc(sizeof(OpusDataSource), allocationCallbacks));
    if (!self) { op_free(of); return MA_OUT_OF_MEMORY; }
    new (self) OpusDataSource();
    self->opus = of;
    const OpusHead* head = op_head(of, -1);
    self->channels = head ? static_cast<ma_uint32>(head->channel_count) : 2;
    ogg_int64_t pcm = op_pcm_total(of, -1);
    self->totalFrames = pcm > 0 ? static_cast<ma_uint64>(pcm) : 0;

    ma_data_source_config dsConfig = ma_data_source_config_init();
    dsConfig.vtable = &g_opus_ds_vtable;
    ma_result r = ma_data_source_init(&dsConfig, &self->base);
    if (r != MA_SUCCESS) {
        op_free(of);
        self->~OpusDataSource();
        ma_free(self, allocationCallbacks);
        return r;
    }

    *ppBackend = self;
    return MA_SUCCESS;
}

static ma_result opus_backend_init_file(void* /*userData*/, const char* filePath, const ma_decoding_backend_config* /*config*/, const ma_allocation_callbacks* allocationCallbacks, ma_data_source** ppBackend) {
    return opus_backend_open(filePath, ppBackend, allocationCallbacks);
}

static void opus_backend_uninit(void* /*userData*/, ma_data_source* pBackend, const ma_allocation_callbacks* allocationCallbacks) {
    if (!pBackend) return;
    OpusDataSource* self = static_cast<OpusDataSource*>(pBackend);
    if (self->opus) op_free(self->opus);
    ma_data_source_uninit(&self->base);
    self->~OpusDataSource();
    ma_free(self, allocationCallbacks);
}

static ma_decoding_backend_vtable g_opus_backend_vtable = {
    nullptr,                  // onInit (callbacks-based)
    opus_backend_init_file,
    nullptr,                  // onInitFileW
    nullptr,                  // onInitMemory
    opus_backend_uninit
};

static ma_decoding_backend_vtable* g_custom_backend_table[] = {
    &g_opus_backend_vtable,
};
#endif // MODULARITY_HAS_OPUSFILE

}

#if defined(__ANDROID__)
namespace {
using Modularity::Platform::AssetStream;

ma_result AssetVfs_Open(ma_vfs*, const char* pFilePath, ma_uint32 openMode, ma_vfs_file* pFile) {
    if (!pFilePath || !pFile) return MA_INVALID_ARGS;
    if (openMode & MA_OPEN_MODE_WRITE) return MA_NOT_IMPLEMENTED;
    std::unique_ptr<AssetStream> stream = Modularity::Platform::GetAssetSource().Open(pFilePath);
    if (!stream) return MA_DOES_NOT_EXIST;
    *pFile = stream.release();
    return MA_SUCCESS;
}
ma_result AssetVfs_Close(ma_vfs*, ma_vfs_file file) {
    delete static_cast<AssetStream*>(file);
    return MA_SUCCESS;
}
ma_result AssetVfs_Read(ma_vfs*, ma_vfs_file file, void* pDst, size_t bytes, size_t* pBytesRead) {
    auto* s = static_cast<AssetStream*>(file);
    const size_t n = s ? s->Read(pDst, bytes) : 0;
    if (pBytesRead) *pBytesRead = n;
    return (n == 0 && bytes > 0) ? MA_AT_END : MA_SUCCESS;
}
ma_result AssetVfs_Seek(ma_vfs*, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin) {
    auto* s = static_cast<AssetStream*>(file);
    if (!s) return MA_INVALID_ARGS;
    const int whence = (origin == ma_seek_origin_start) ? SEEK_SET
                     : (origin == ma_seek_origin_end)   ? SEEK_END
                                                        : SEEK_CUR;
    return s->Seek(offset, whence) ? MA_SUCCESS : MA_ERROR;
}
ma_result AssetVfs_Tell(ma_vfs*, ma_vfs_file file, ma_int64* pCursor) {
    auto* s = static_cast<AssetStream*>(file);
    if (!s) return MA_INVALID_ARGS;
    const int64_t t = s->Tell();
    if (t < 0) return MA_ERROR;
    if (pCursor) *pCursor = static_cast<ma_int64>(t);
    return MA_SUCCESS;
}
ma_result AssetVfs_Info(ma_vfs*, ma_vfs_file file, ma_file_info* pInfo) {
    auto* s = static_cast<AssetStream*>(file);
    if (!s || !pInfo) return MA_INVALID_ARGS;
    const int64_t size = s->Size();
    pInfo->sizeInBytes = (size < 0) ? 0 : static_cast<ma_uint64>(size);
    return MA_SUCCESS;
}

struct AssetVfs { ma_vfs_callbacks cb; };
AssetVfs g_androidAssetVfs = { {
    AssetVfs_Open,
    nullptr,
    AssetVfs_Close,
    AssetVfs_Read,
    nullptr,
    AssetVfs_Seek,
    AssetVfs_Tell,
    AssetVfs_Info,
} };
} // namespace
#endif // __ANDROID__

namespace {
// Exported builds replace lossless clips with a Vorbis sidecar ("hit.wav" ships as "hit.wav.ogg",
// see AudioExportCompression.h) while scenes and scripts keep the original path.
std::string CompressedSidecarFor(const std::string& path) {
    if (!Modularity::AudioExport::IsLosslessClipPath(path)) return {};
    auto& assets = Modularity::Platform::GetAssetSource();
    if (assets.Exists(path)) return {};
    std::string sidecar = path + Modularity::AudioExport::kSidecarSuffix;
    return assets.Exists(sidecar) ? sidecar : std::string();
}

bool AudioClipExists(const std::string& path) {
    return Modularity::Platform::GetAssetSource().Exists(path) || !CompressedSidecarFor(path).empty();
}

std::string ResolveClipPath(const std::string& path) {
    const std::string sidecar = CompressedSidecarFor(path);
    return Modularity::Platform::ResolveAssetPath(sidecar.empty() ? path : sidecar);
}
} // namespace

bool AudioSystem::init() {
    if (initialized) return true;

    ma_engine_config engineConfig = ma_engine_config_init();
#if MODULARITY_HAS_OPUSFILE || defined(__ANDROID__)
    ma_resource_manager_config rmConfig = ma_resource_manager_config_init();
#if MODULARITY_HAS_OPUSFILE
    rmConfig.ppCustomDecodingBackendVTables = g_custom_backend_table;
    rmConfig.customDecodingBackendCount = sizeof(g_custom_backend_table) / sizeof(g_custom_backend_table[0]);
#endif
#if defined(__ANDROID__)
    rmConfig.pVFS = &g_androidAssetVfs;
#endif
    ma_result rmRes = ma_resource_manager_init(&rmConfig, &resourceManager);
    if (rmRes != MA_SUCCESS) {
        std::cerr << "AudioSystem: failed to init resource manager ("
                  << ma_result_description(rmRes) << ")\n";
        return false;
    }
    resourceManagerInitialized = true;
    engineConfig.pResourceManager = &resourceManager;
#endif

    ma_result res = ma_engine_init(&engineConfig, &engine);
    if (res != MA_SUCCESS) {
        std::cerr << "AudioSystem: failed to init miniaudio ("
                  << ma_result_description(res) << ")\n";
#if MODULARITY_HAS_OPUSFILE || defined(__ANDROID__)
        if (resourceManagerInitialized) {
            ma_resource_manager_uninit(&resourceManager);
            resourceManagerInitialized = false;
        }
#endif
        return false;
    }
    ma_uint32 channels = ma_engine_get_channels(&engine);
    ma_uint32 sampleRate = ma_engine_get_sample_rate(&engine);
    ma_splitter_node_config splitterConfig = ma_splitter_node_config_init(channels);
    res = ma_splitter_node_init(ma_engine_get_node_graph(&engine), &splitterConfig, nullptr, &reverbSplitter);
    if (res == MA_SUCCESS) {
        ma_node_config nodeConfig = ma_node_config_init();
        nodeConfig.vtable = &g_reverb_node_vtable;
        nodeConfig.pInputChannels = reinterpret_cast<const ma_uint32*>(&channels);
        nodeConfig.pOutputChannels = reinterpret_cast<const ma_uint32*>(&channels);
        res = ma_node_init(ma_engine_get_node_graph(&engine), &nodeConfig, nullptr, reinterpret_cast<ma_node*>(&reverbNode));
        if (res == MA_SUCCESS) {
            reverbNode.channels = static_cast<int>(channels);
            reverbNode.sampleRate = static_cast<int>(sampleRate);
            reverbNode.preDelayMaxFrames = static_cast<size_t>(kReverbPreDelayMaxSeconds * sampleRate);
            reverbNode.reflectionsMaxFrames = static_cast<size_t>(kReverbReflectionsMaxSeconds * sampleRate);
            reverbNode.preDelayBuffer.assign(reverbNode.preDelayMaxFrames * channels, 0.0f);
            reverbNode.reflectionsBuffer.assign(reverbNode.reflectionsMaxFrames * channels, 0.0f);
            reverbNode.lpState.assign(channels, 0.0f);

            const float combDelayMs[kReverbCombCount] = { 29.7f, 37.1f, 41.1f, 43.7f };
            reverbNode.combBuffers.resize(kReverbCombCount);
            reverbNode.combIndex.assign(kReverbCombCount, 0);
            for (size_t i = 0; i < kReverbCombCount; ++i) {
                size_t frames = static_cast<size_t>((combDelayMs[i] / 1000.0f) * sampleRate);
                frames = std::max<size_t>(1, frames);
                reverbNode.combBuffers[i].assign(frames * channels, 0.0f);
            }

            const float allpassDelayMs[kReverbAllpassCount] = { 5.0f, 1.7f };
            reverbNode.allpassBuffers.resize(kReverbAllpassCount);
            reverbNode.allpassIndex.assign(kReverbAllpassCount, 0);
            for (size_t i = 0; i < kReverbAllpassCount; ++i) {
                size_t frames = static_cast<size_t>((allpassDelayMs[i] / 1000.0f) * sampleRate);
                frames = std::max<size_t>(1, frames);
                reverbNode.allpassBuffers[i].assign(frames * channels, 0.0f);
            }

            ma_node_attach_output_bus(reinterpret_cast<ma_node*>(&reverbSplitter), 0, ma_engine_get_endpoint(&engine), 0);
            ma_node_attach_output_bus(reinterpret_cast<ma_node*>(&reverbSplitter), 1, reinterpret_cast<ma_node*>(&reverbNode), 0);
            ma_node_attach_output_bus(reinterpret_cast<ma_node*>(&reverbNode), 0, ma_engine_get_endpoint(&engine), 0);
            ma_sound_group_config groupConfig = ma_sound_group_config_init_2(&engine);
            groupConfig.pInitialAttachment = reinterpret_cast<ma_node*>(&reverbSplitter);
            groupConfig.initialAttachmentInputBusIndex = 0;
            res = ma_sound_group_init_ex(&engine, &groupConfig, &reverbGroup);
            if (res == MA_SUCCESS) {
                reverbReady = true;
                ma_sound_group_set_spatialization_enabled(&reverbGroup, MA_FALSE);
                ma_sound_group_set_attenuation_model(&reverbGroup, ma_attenuation_model_none);
                ma_sound_group_start(&reverbGroup);
                ma_node_set_output_bus_volume(reinterpret_cast<ma_node*>(&reverbSplitter), 0, 1.0f);
                initGlobalFXNode(channels, sampleRate);
            } else {
                ma_node_uninit(reinterpret_cast<ma_node*>(&reverbNode), nullptr);
                ma_splitter_node_uninit(&reverbSplitter, nullptr);
            }
        } else {
            ma_splitter_node_uninit(&reverbSplitter, nullptr);
        }
    }

    initialized = true;
    return true;
}

void AudioSystem::shutdown() {
    stopPreview();
    stopWeatherAmbience();
    destroyVideoStreams();
    destroyActiveSounds();
    destroyOneShotSounds();
    cleanupFinishedUiSounds(true);
    shutdownReverbGraph();
    if (initialized) {
        ma_engine_uninit(&engine);
        initialized = false;
    }
#if MODULARITY_HAS_OPUSFILE || defined(__ANDROID__)
    if (resourceManagerInitialized) {
        ma_resource_manager_uninit(&resourceManager);
        resourceManagerInitialized = false;
    }
#endif
}

void AudioSystem::teardownSound(ActiveSound& snd) {
    ma_sound_uninit(&snd.sound);
    if (snd.fxNodeReady) {
        ma_node_uninit(reinterpret_cast<ma_node*>(&snd.fxNode), nullptr);
        snd.fxNodeReady = false;
    }
    snd.fxProcessor.reset();
    releaseDecodedAudio(snd.decodedData);
}

void AudioSystem::destroyActiveSounds() {
    for (auto& kv : activeSounds) {
        if (kv.second) teardownSound(*kv.second);
    }
    activeSounds.clear();
}

void AudioSystem::destroyOneShotSounds() {
    for (auto& snd : oneShotSounds) {
        if (snd) {
            ma_sound_uninit(&snd->sound);
            releaseDecodedAudio(snd->decodedData);
        }
    }
    oneShotSounds.clear();
}

void AudioSystem::destroyVideoStreams() {
    for (auto& kv : videoStreams) {
        if (kv.second) {
            ma_sound_uninit(&kv.second->sound);
        }
    }
    videoStreams.clear();
}

void AudioSystem::cleanupFinishedOneShots() {
    for (auto it = oneShotSounds.begin(); it != oneShotSounds.end(); ) {
        bool erase = !(*it) || !ma_sound_is_playing(&(*it)->sound);
        if (erase) {
            if (*it) {
                ma_sound_uninit(&(*it)->sound);
                releaseDecodedAudio((*it)->decodedData);
            }
            it = oneShotSounds.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioSystem::cleanupFinishedUiSounds(bool destroyAll) {
    for (auto it = uiSounds.begin(); it != uiSounds.end(); ) {
        if (destroyAll || !(*it) || !ma_sound_is_playing(&(*it)->sound)) {
            if (*it) {
                ma_sound_uninit(&(*it)->sound);
                releaseDecodedAudio((*it)->decodedData);
            }
            it = uiSounds.erase(it);
        } else {
            ++it;
        }
    }
}

bool AudioSystem::playUiSound(const std::string& path, float volume) {
    if (path.empty()) return false;
    if (!initialized && !init()) return false;
    cleanupFinishedUiSounds(false);
    if (!AudioClipExists(path)) {
        if (missingClips.insert(path).second) {
            std::cerr << "AudioSystem: clip not found " << path << "\n";
        }
        return false;
    }
    missingClips.erase(path);

    auto snd = std::make_unique<OneShotSound>();
    // No group: straight to the engine endpoint, like the preview slot, so scene reverb never
    // colours a click.
    if (!initSoundFromPath(path, 0, nullptr, snd->sound, snd->decodedData)) {
        return false;
    }
    ma_sound_set_looping(&snd->sound, MA_FALSE);
    ma_sound_set_volume(&snd->sound, std::max(0.0f, volume));
    ma_sound_set_spatialization_enabled(&snd->sound, MA_FALSE);
    if (ma_sound_start(&snd->sound) != MA_SUCCESS) {
        ma_sound_uninit(&snd->sound);
        releaseDecodedAudio(snd->decodedData);
        return false;
    }
    uiSounds.emplace_back(std::move(snd));
    return true;
}

void AudioSystem::onPlayStart(const std::vector<SceneObject>& objects) {
    if (!initialized && !init()) return;
    destroyActiveSounds();
    destroyOneShotSounds();
    for (const auto& obj : objects) {
        if (!IsObjectEnabledInHierarchy(obj) || !obj.hasAudioSource || obj.audioSource.clipPath.empty()) continue;
        if (!obj.audioSource.enabled) continue;
        if (ensureSoundFor(obj) && obj.audioSource.playOnStart) {
            ma_sound_start(&activeSounds[obj.id]->sound);
        }
    }
}

void AudioSystem::onPlayStop() {
    destroyVideoStreams();
    destroyActiveSounds();
    destroyOneShotSounds();
}

bool AudioSystem::ensureSoundFor(const SceneObject& obj) {
    auto it = activeSounds.find(obj.id);
    if (it != activeSounds.end()) {
        if (it->second && it->second->clipPath == obj.audioSource.clipPath) {
            refreshSoundParams(obj, *it->second);
            return true;
        }
        if (it->second) teardownSound(*it->second);
        activeSounds.erase(it);
    }

    if (!AudioClipExists(obj.audioSource.clipPath)) {
        if (missingClips.insert(obj.audioSource.clipPath).second) {
            std::cerr << "AudioSystem: clip not found " << obj.audioSource.clipPath << "\n";
        }
        return false;
    }
    missingClips.erase(obj.audioSource.clipPath);

    if (!initialized && !init()) return false;

    auto snd = std::make_unique<ActiveSound>();
    if (!initSoundFromPath(obj.audioSource.clipPath, MA_SOUND_FLAG_STREAM, reverbReady ? &reverbGroup : nullptr, snd->sound, snd->decodedData)) {
        return false;
    }

    snd->clipPath = obj.audioSource.clipPath;
    snd->spatial = AudioSourceUsesSpatialization(obj.audioSource);
    snd->started = false;
    refreshSoundParams(obj, *snd);
    activeSounds.emplace(obj.id, std::move(snd));
    return true;
}

void AudioSystem::refreshSoundParams(const SceneObject& obj, ActiveSound& snd) {
    applyAudioSourceParams(obj, snd.sound, obj.audioSource.volume, obj.audioSource.loop);
    updateSourceFX(obj, snd);

    if (!ma_sound_is_playing(&snd.sound) && !snd.started && obj.audioSource.playOnStart && obj.audioSource.enabled) {
        ma_sound_start(&snd.sound);
        snd.started = true;
    }
}

void AudioSystem::applyAudioSourceParams(const SceneObject& obj, ma_sound& sound, float baseGain, bool loop) {
    const float spatialBlend = GetAudioSpatialBlend(obj.audioSource);
    const bool spatialEnabled = spatialBlend > 0.001f;
    const bool planar2D = shouldUsePlanar2DAudio(obj);
    float minDist = std::max(0.1f, obj.audioSource.minDistance);
    float maxDist = std::max(obj.audioSource.maxDistance, minDist + 0.5f);
    float gain = std::max(0.0f, baseGain);
    ma_sound_set_looping(&sound, loop ? MA_TRUE : MA_FALSE);

    if (planar2D) {
        const glm::vec3 planarSourcePos = computeSpatializedPosition(obj, spatialBlend, lastListenerPosition);
        const float attenuation = computeDistanceAttenuation(obj, lastListenerPosition, planarSourcePos);
        gain *= std::clamp(1.0f + (attenuation - 1.0f) * spatialBlend, 0.0f, 1.0f);
        ma_sound_set_pan_mode(&sound, ma_pan_mode_pan);
        ma_sound_set_pan(&sound, computePlanarPan(obj, lastListenerPosition, spatialBlend));
        ma_sound_set_spatialization_enabled(&sound, MA_FALSE);
        ma_sound_set_attenuation_model(&sound, ma_attenuation_model_none);
        ma_sound_set_position(&sound, 0.0f, 0.0f, 0.0f);
    } else {
        ma_sound_set_pan_mode(&sound, ma_pan_mode_balance);
        ma_sound_set_pan(&sound, 0.0f);
        ma_sound_set_spatialization_enabled(&sound, spatialEnabled ? MA_TRUE : MA_FALSE);
    }
    if (spatialEnabled && !planar2D) {
        switch (obj.audioSource.rolloffMode) {
            case AudioRolloffMode::Linear:
                ma_sound_set_attenuation_model(&sound, ma_attenuation_model_linear);
                break;
            case AudioRolloffMode::Exponential:
                ma_sound_set_attenuation_model(&sound, ma_attenuation_model_exponential);
                break;
            case AudioRolloffMode::Custom:
                ma_sound_set_attenuation_model(&sound, ma_attenuation_model_none);
                break;
            case AudioRolloffMode::Logarithmic:
            default:
                ma_sound_set_attenuation_model(&sound, ma_attenuation_model_inverse);
                break;
        }
        ma_sound_set_rolloff(&sound, std::max(0.01f, obj.audioSource.rolloff));
    } else {
        ma_sound_set_attenuation_model(&sound, ma_attenuation_model_none);
    }
    ma_sound_set_min_distance(&sound, minDist);
    ma_sound_set_max_distance(&sound, maxDist);
    if (!planar2D) {
        const glm::vec3 spatialPos = computeSpatializedPosition(obj, spatialBlend, lastListenerPosition);
        ma_sound_set_position(&sound, spatialPos.x, spatialPos.y, spatialPos.z);

        if (spatialEnabled && obj.audioSource.rolloffMode == AudioRolloffMode::Custom) {
            float attenuation = computeCustomAttenuation(obj, lastListenerPosition, spatialPos);
            gain *= attenuation;
        }
    }
    ma_sound_set_volume(&sound, gain);
}

void AudioSystem::update(const std::vector<SceneObject>& objects, const Camera& listenerCamera, bool playing) {
    if (!initialized) return;

    lastListenerPosition = listenerCamera.position;
    ma_engine_listener_set_position(&engine, 0, listenerCamera.position.x, listenerCamera.position.y, listenerCamera.position.z);
    ma_engine_listener_set_direction(&engine, 0, listenerCamera.front.x, listenerCamera.front.y, listenerCamera.front.z);
    ma_engine_listener_set_world_up(&engine, 0, listenerCamera.up.x, listenerCamera.up.y, listenerCamera.up.z);
    updateReverb(objects, listenerCamera.position);
    updateGlobalFXFromObjects(objects);

    if (!playing) {
        destroyActiveSounds();
        cleanupFinishedOneShots();
        return;
    }

    std::unordered_set<int> stillPresent;
    for (const auto& obj : objects) {
        if (!obj.hasAudioSource) continue;
        stillPresent.insert(obj.id);

        auto eraseIt = activeSounds.find(obj.id);
        if (!IsObjectEnabledInHierarchy(obj) || !obj.audioSource.enabled || obj.audioSource.clipPath.empty()) {
            if (eraseIt != activeSounds.end()) {
                if (eraseIt->second) teardownSound(*eraseIt->second);
                activeSounds.erase(eraseIt);
            }
            continue;
        }

        if (ensureSoundFor(obj)) {
            refreshSoundParams(obj, *activeSounds[obj.id]);
        }
    }

    for (auto it = activeSounds.begin(); it != activeSounds.end(); ) {
        if (stillPresent.find(it->first) == stillPresent.end()) {
            if (it->second) teardownSound(*it->second);
            it = activeSounds.erase(it);
        } else {
            ++it;
        }
    }

    cleanupFinishedOneShots();
}

bool AudioSystem::playWeatherAmbience(const std::string& path, float volume) {
    if (path.empty()) {
        stopWeatherAmbience();
        return false;
    }
    if (!initialized && !init()) return false;

    if (weatherActive && weatherAmbiencePath == path) {
        ma_sound_set_volume(&weatherSound, volume);
        return true;
    }

    stopWeatherAmbience();
    if (!initSoundFromPath(path, MA_SOUND_FLAG_STREAM, nullptr, weatherSound, weatherDecodedData)) {
        std::cerr << "AudioSystem: weather ambience load failed for " << path << "\n";
        return false;
    }
    ma_sound_set_looping(&weatherSound, MA_TRUE);
    ma_sound_set_volume(&weatherSound, volume);
    ma_sound_set_spatialization_enabled(&weatherSound, MA_FALSE);
    weatherAmbiencePath = path;
    weatherActive = ma_sound_start(&weatherSound) == MA_SUCCESS;
    if (!weatherActive) {
        ma_sound_uninit(&weatherSound);
        releaseDecodedAudio(weatherDecodedData);
        weatherAmbiencePath.clear();
    }
    return weatherActive;
}

void AudioSystem::stopWeatherAmbience() {
    if (!weatherActive) {
        weatherAmbiencePath.clear();
        return;
    }
    ma_sound_stop(&weatherSound);
    ma_sound_uninit(&weatherSound);
    releaseDecodedAudio(weatherDecodedData);
    weatherActive = false;
    weatherAmbiencePath.clear();
}

bool AudioSystem::playPreview(const std::string& path, float volume, bool loop) {
    if (path.empty()) return false;
    if (!initialized && !init()) return false;

    // Prime cached waveform metadata up front so streamed formats still expose duration/seek info.
    (void)getPreview(path);

    stopPreview();
    if (!initSoundFromPath(path, MA_SOUND_FLAG_STREAM, nullptr, previewSound, previewDecodedData)) {
        std::cerr << "AudioSystem: preview load failed for " << path << "\n";
        return false;
    }
    ma_sound_set_looping(&previewSound, loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_volume(&previewSound, volume);
    ma_sound_set_spatialization_enabled(&previewSound, MA_FALSE);
    previewPath = path;
    previewActive = ma_sound_start(&previewSound) == MA_SUCCESS;
    if (!previewActive) {
        ma_sound_uninit(&previewSound);
        releaseDecodedAudio(previewDecodedData);
    }
    return previewActive;
}

bool AudioSystem::playOneShot(const std::string& path, float volume, float pitch) {
    if (path.empty()) return false;
    if (!initialized && !init()) return false;
    if (!AudioClipExists(path)) {
        if (missingClips.insert(path).second) {
            std::cerr << "AudioSystem: clip not found " << path << "\n";
        }
        return false;
    }
    missingClips.erase(path);

    auto oneShot = std::make_unique<OneShotSound>();
    if (!initSoundFromPath(path, 0, reverbReady ? &reverbGroup : nullptr,
                           oneShot->sound, oneShot->decodedData))
    {
        return false;
    }

    ma_sound_set_looping(&oneShot->sound, MA_FALSE);
    ma_sound_set_volume(&oneShot->sound, std::max(0.0f, volume));
    ma_sound_set_pitch(&oneShot->sound, std::clamp(pitch, 0.05f, 4.0f));
    ma_sound_set_spatialization_enabled(&oneShot->sound, MA_FALSE);
    ma_sound_set_pan_mode(&oneShot->sound, ma_pan_mode_balance);
    ma_sound_set_pan(&oneShot->sound, 0.0f);
    ma_sound_set_attenuation_model(&oneShot->sound, ma_attenuation_model_none);
    ma_sound_set_position(&oneShot->sound, 0.0f, 0.0f, 0.0f);

    if (ma_sound_start(&oneShot->sound) != MA_SUCCESS) {
        ma_sound_uninit(&oneShot->sound);
        releaseDecodedAudio(oneShot->decodedData);
        return false;
    }

    oneShotSounds.emplace_back(std::move(oneShot));
    cleanupFinishedOneShots();
    return true;
}

void AudioSystem::stopPreview() {
    if (previewActive) {
        ma_sound_stop(&previewSound);
        ma_sound_uninit(&previewSound);
    }
    releaseDecodedAudio(previewDecodedData);
    previewActive = false;
    previewPath.clear();
}

bool AudioSystem::isPreviewing(const std::string& path) const {
    return previewActive && previewPath == path;
}

bool AudioSystem::getPreviewTime(const std::string& path, double& cursorSeconds, double& durationSeconds) const {
    if (!previewActive || previewPath != path) return false;

    ma_uint32 sampleRate = 0;
    durationSeconds = 0.0;
    auto it = previewCache.find(path);
    if (it != previewCache.end() && it->second.loaded && it->second.sampleRate > 0) {
        sampleRate = it->second.sampleRate;
    } else if (previewDecodedData && previewDecodedData->sampleRate > 0) {
        sampleRate = previewDecodedData->sampleRate;
    } else if (ma_sound_get_data_format(&previewSound, nullptr, nullptr, &sampleRate, nullptr, 0) != MA_SUCCESS || sampleRate == 0) {
        return false;
    }

    ma_uint64 cursorFrames = 0;
    if (ma_sound_get_cursor_in_pcm_frames(&previewSound, &cursorFrames) != MA_SUCCESS) return false;
    cursorSeconds = static_cast<double>(cursorFrames) / static_cast<double>(sampleRate);

    if (it != previewCache.end() && it->second.loaded && std::isfinite(it->second.durationSeconds) && it->second.durationSeconds > 0.0) {
        durationSeconds = it->second.durationSeconds;
    } else if (previewDecodedData && previewDecodedData->sampleRate > 0 && previewDecodedData->frameCount > 0) {
        durationSeconds = static_cast<double>(previewDecodedData->frameCount) / static_cast<double>(previewDecodedData->sampleRate);
    } else {
        ma_uint64 lengthFrames = 0;
        if (ma_sound_get_length_in_pcm_frames(&previewSound, &lengthFrames) == MA_SUCCESS && lengthFrames > 0) {
            durationSeconds = static_cast<double>(lengthFrames) / static_cast<double>(sampleRate);
        }
    }

    if (!std::isfinite(cursorSeconds) || !std::isfinite(durationSeconds) || durationSeconds <= 0.0) {
        return false;
    }

    cursorSeconds = std::clamp(cursorSeconds, 0.0, durationSeconds);
    return true;
}

bool AudioSystem::seekPreview(const std::string& path, double seconds) {
    if (!previewActive || previewPath != path) return false;

    ma_uint32 sourceSampleRate = 0;
    auto it = previewCache.find(path);
    if (it != previewCache.end() && it->second.loaded && it->second.sampleRate > 0) {
        sourceSampleRate = it->second.sampleRate;
    } else if (previewDecodedData && previewDecodedData->sampleRate > 0) {
        sourceSampleRate = previewDecodedData->sampleRate;
    } else if (ma_sound_get_data_format(&previewSound, nullptr, nullptr, &sourceSampleRate, nullptr, 0) != MA_SUCCESS || sourceSampleRate == 0) {
        return false;
    }

    double maxSeconds = 0.0;
    if (it != previewCache.end() && it->second.loaded && std::isfinite(it->second.durationSeconds) && it->second.durationSeconds > 0.0) {
        maxSeconds = it->second.durationSeconds;
    } else if (previewDecodedData && previewDecodedData->sampleRate > 0 && previewDecodedData->frameCount > 0) {
        maxSeconds = static_cast<double>(previewDecodedData->frameCount) / static_cast<double>(previewDecodedData->sampleRate);
    } else {
        ma_uint64 lengthFrames = 0;
        if (ma_sound_get_length_in_pcm_frames(&previewSound, &lengthFrames) == MA_SUCCESS && lengthFrames > 0) {
            maxSeconds = static_cast<double>(lengthFrames) / static_cast<double>(sourceSampleRate);
        }
    }
    if (maxSeconds > 0.0 && std::isfinite(maxSeconds)) {
        seconds = std::clamp(seconds, 0.0, maxSeconds);
    } else {
        seconds = std::max(0.0, seconds);
    }
    ma_uint64 targetFrame = static_cast<ma_uint64>(seconds * static_cast<double>(sourceSampleRate));
    ma_result res = ma_sound_seek_to_pcm_frame(&previewSound, targetFrame);
    return res == MA_SUCCESS;
}

bool AudioSystem::setPreviewLoop(bool loop) {
    if (!previewActive) return false;
    ma_sound_set_looping(&previewSound, loop ? MA_TRUE : MA_FALSE);
    return true;
}

bool AudioSystem::setPreviewVolume(float volume) {
    if (!previewActive) return false;
    ma_sound_set_volume(&previewSound, std::max(0.0f, volume));
    return true;
}

bool AudioSystem::playObjectSound(const SceneObject& obj) {
    if (!IsObjectEnabledInHierarchy(obj) || !obj.hasAudioSource || obj.audioSource.clipPath.empty() || !obj.audioSource.enabled) return false;
    if (!ensureSoundFor(obj)) return false;
    ActiveSound& snd = *activeSounds[obj.id];
    snd.started = true;
    return ma_sound_start(&snd.sound) == MA_SUCCESS;
}

bool AudioSystem::playObjectOneShot(const SceneObject& obj, const std::string& clipPathOverride, float volumeScale) {
    if (!IsObjectEnabledInHierarchy(obj) || !obj.hasAudioSource || !obj.audioSource.enabled) return false;
    const std::string& clipPath = clipPathOverride.empty() ? obj.audioSource.clipPath : clipPathOverride;
    if (clipPath.empty()) return false;
    if (!initialized && !init()) return false;

    if (!AudioClipExists(clipPath)) {
        if (missingClips.insert(clipPath).second) {
            std::cerr << "AudioSystem: clip not found " << clipPath << "\n";
        }
        return false;
    }
    missingClips.erase(clipPath);

    auto oneShot = std::make_unique<OneShotSound>();
    if (!initSoundFromPath(clipPath, 0, reverbReady ? &reverbGroup : nullptr,
                           oneShot->sound, oneShot->decodedData))
    {
        return false;
    }

    const float minDist = std::max(0.1f, obj.audioSource.minDistance);
    const float maxDist = std::max(obj.audioSource.maxDistance, minDist + 0.5f);
    const float spatialBlend = GetAudioSpatialBlend(obj.audioSource);
    const bool spatialEnabled = spatialBlend > 0.001f;
    const bool planar2D = shouldUsePlanar2DAudio(obj);
    const glm::vec3 spatialPos = computeSpatializedPosition(obj, spatialBlend, lastListenerPosition);
    float gain = std::max(0.0f, obj.audioSource.volume * volumeScale);
    ma_sound_set_looping(&oneShot->sound, MA_FALSE);
    ma_sound_set_pan_mode(&oneShot->sound, planar2D ? ma_pan_mode_pan : ma_pan_mode_balance);
    ma_sound_set_pan(&oneShot->sound, planar2D ? computePlanarPan(obj, lastListenerPosition, spatialBlend) : 0.0f);
    ma_sound_set_spatialization_enabled(&oneShot->sound, (spatialEnabled && !planar2D) ? MA_TRUE : MA_FALSE);
    ma_sound_set_min_distance(&oneShot->sound, minDist);
    ma_sound_set_max_distance(&oneShot->sound, maxDist);
    if (planar2D) {
        const float attenuation = computeDistanceAttenuation(obj, lastListenerPosition, spatialPos);
        gain *= std::clamp(1.0f + (attenuation - 1.0f) * spatialBlend, 0.0f, 1.0f);
        ma_sound_set_position(&oneShot->sound, 0.0f, 0.0f, 0.0f);
    } else {
        ma_sound_set_position(&oneShot->sound, spatialPos.x, spatialPos.y, spatialPos.z);
    }

    if (spatialEnabled && !planar2D) {
        switch (obj.audioSource.rolloffMode) {
            case AudioRolloffMode::Linear:
                ma_sound_set_attenuation_model(&oneShot->sound, ma_attenuation_model_linear);
                break;
            case AudioRolloffMode::Exponential:
                ma_sound_set_attenuation_model(&oneShot->sound, ma_attenuation_model_exponential);
                break;
            case AudioRolloffMode::Custom:
                ma_sound_set_attenuation_model(&oneShot->sound, ma_attenuation_model_none);
                break;
            case AudioRolloffMode::Logarithmic:
            default:
                ma_sound_set_attenuation_model(&oneShot->sound, ma_attenuation_model_inverse);
                break;
        }
        ma_sound_set_rolloff(&oneShot->sound, std::max(0.01f, obj.audioSource.rolloff));
    } else {
        ma_sound_set_attenuation_model(&oneShot->sound, ma_attenuation_model_none);
    }

    if (spatialEnabled && !planar2D && obj.audioSource.rolloffMode == AudioRolloffMode::Custom) {
        float attenuation = computeCustomAttenuation(obj, lastListenerPosition, spatialPos);
        gain *= attenuation;
    }
    ma_sound_set_volume(&oneShot->sound, gain);

    if (ma_sound_start(&oneShot->sound) != MA_SUCCESS) {
        ma_sound_uninit(&oneShot->sound);
        releaseDecodedAudio(oneShot->decodedData);
        return false;
    }

    oneShotSounds.emplace_back(std::move(oneShot));
    cleanupFinishedOneShots();
    return true;
}

bool AudioSystem::stopObjectSound(int objectId) {
    auto it = activeSounds.find(objectId);
    if (it == activeSounds.end()) return false;
    if (!it->second) return false;
    return ma_sound_stop(&it->second->sound) == MA_SUCCESS;
}

bool AudioSystem::setObjectLoop(const SceneObject& obj, bool loop) {
    if (!ensureSoundFor(obj)) return false;
    ActiveSound& snd = *activeSounds[obj.id];
    ma_sound_set_looping(&snd.sound, loop ? MA_TRUE : MA_FALSE);
    return true;
}

bool AudioSystem::setObjectVolume(const SceneObject& obj, float volume) {
    if (!ensureSoundFor(obj)) return false;
    (void)volume;
    ActiveSound& snd = *activeSounds[obj.id];
    refreshSoundParams(obj, snd);
    return true;
}

bool AudioSystem::attachVideoStream(int streamId, ma_data_source* dataSource) {
    if (streamId < 0 || dataSource == nullptr) return false;
    if (!initialized && !init()) return false;

    auto it = videoStreams.find(streamId);
    if (it != videoStreams.end() && it->second && it->second->dataSource == dataSource) {
        return true;
    }

    detachVideoStream(streamId);

    auto stream = std::make_unique<VideoStreamSound>();
    ma_result res = ma_sound_init_from_data_source(&engine, dataSource, 0,
                                                   reverbReady ? &reverbGroup : nullptr, &stream->sound);
    if (res != MA_SUCCESS) {
        std::cerr << "AudioSystem: failed to attach video audio stream " << streamId
                  << " (" << ma_result_description(res) << ")\n";
        return false;
    }

    stream->dataSource = dataSource;
    videoStreams.emplace(streamId, std::move(stream));
    return true;
}

void AudioSystem::detachVideoStream(int streamId) {
    auto it = videoStreams.find(streamId);
    if (it == videoStreams.end()) return;
    if (it->second) {
        ma_sound_stop(&it->second->sound);
        ma_sound_uninit(&it->second->sound);
    }
    videoStreams.erase(it);
}

bool AudioSystem::configureVideoStream(int streamId, const SceneObject* routeObject, float volume, bool muted, bool loop, float pitch) {
    auto it = videoStreams.find(streamId);
    if (it == videoStreams.end() || !it->second) return false;

    ma_sound& sound = it->second->sound;
    const float gain = muted ? 0.0f : std::max(0.0f, volume);
    ma_sound_set_pitch(&sound, std::max(0.01f, pitch));

    if (routeObject && routeObject->hasAudioSource) {
        applyAudioSourceParams(*routeObject, sound, routeObject->audioSource.volume * gain, loop);
        return true;
    }

    ma_sound_set_looping(&sound, loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_pan_mode(&sound, ma_pan_mode_balance);
    ma_sound_set_pan(&sound, 0.0f);
    ma_sound_set_spatialization_enabled(&sound, MA_FALSE);
    ma_sound_set_attenuation_model(&sound, ma_attenuation_model_none);
    ma_sound_set_position(&sound, 0.0f, 0.0f, 0.0f);
    ma_sound_set_volume(&sound, gain);
    return true;
}

bool AudioSystem::setVideoStreamPlaying(int streamId, bool playing) {
    auto it = videoStreams.find(streamId);
    if (it == videoStreams.end() || !it->second) return false;
    const ma_result result = playing ? ma_sound_start(&it->second->sound)
                                     : ma_sound_stop(&it->second->sound);
    if (result != MA_SUCCESS) {
        std::cerr << "AudioSystem: " << (playing ? "ma_sound_start" : "ma_sound_stop")
                  << " failed for video streamId=" << streamId
                  << " (" << ma_result_description(result) << ").\n";
    }
    return result == MA_SUCCESS;
}

bool AudioSystem::seekVideoStreamToSeconds(int streamId, double seconds) {
    auto it = videoStreams.find(streamId);
    if (it == videoStreams.end() || !it->second) return false;

    ma_uint32 sampleRate = 0;
    if (ma_sound_get_data_format(&it->second->sound, nullptr, nullptr, &sampleRate, nullptr, 0) != MA_SUCCESS ||
        sampleRate == 0) {
        return false;
    }

    const ma_uint64 targetFrame = static_cast<ma_uint64>(std::max(0.0, seconds) * static_cast<double>(sampleRate));
    return ma_sound_seek_to_pcm_frame(&it->second->sound, targetFrame) == MA_SUCCESS;
}

bool AudioSystem::getVideoStreamCursorSeconds(int streamId, double& cursorSeconds) const {
    cursorSeconds = 0.0;
    auto it = videoStreams.find(streamId);
    if (it == videoStreams.end() || !it->second) return false;

    ma_uint32 sampleRate = 0;
    ma_uint64 cursorFrames = 0;
    if (ma_sound_get_data_format(&it->second->sound, nullptr, nullptr, &sampleRate, nullptr, 0) != MA_SUCCESS ||
        sampleRate == 0 ||
        ma_sound_get_cursor_in_pcm_frames(&it->second->sound, &cursorFrames) != MA_SUCCESS) {
        return false;
    }

    cursorSeconds = static_cast<double>(cursorFrames) / static_cast<double>(sampleRate);
    return true;
}

bool AudioSystem::shouldUsePlanar2DAudio(const SceneObject& obj) const {
    return prefer2DSpatialAudio || HasUIComponent(obj);
}

float AudioSystem::computePlanarPan(const SceneObject& obj, const glm::vec3& listenerPos, float spatialBlend) const {
    const float panRange = std::max(0.5f, obj.audioSource.maxDistance * 0.35f);
    const float relativeX = obj.position.x - listenerPos.x;
    const float normalizedPan = std::clamp(relativeX / panRange, -1.0f, 1.0f);
    const float shapedPan = std::copysign(std::pow(std::abs(normalizedPan), 0.8f), normalizedPan);
    return std::clamp(shapedPan * std::clamp(spatialBlend, 0.0f, 1.0f), -1.0f, 1.0f);
}

float AudioSystem::computeDistanceAttenuation(const SceneObject& obj, const glm::vec3& listenerPos, const glm::vec3& sourcePos) const {
    if (obj.audioSource.rolloffMode == AudioRolloffMode::Custom) {
        return computeCustomAttenuation(obj, listenerPos, sourcePos);
    }

    const float minDist = std::max(0.1f, obj.audioSource.minDistance);
    const float maxDist = std::max(obj.audioSource.maxDistance, minDist + 0.5f);
    const float dist = glm::length(glm::vec2(listenerPos.x - sourcePos.x, listenerPos.y - sourcePos.y));
    if (dist <= minDist) return 1.0f;
    if (dist >= maxDist) return 0.0f;

    const float t = std::clamp((dist - minDist) / std::max(0.001f, maxDist - minDist), 0.0f, 1.0f);
    switch (obj.audioSource.rolloffMode) {
        case AudioRolloffMode::Linear:
            return 1.0f - t;
        case AudioRolloffMode::Exponential:
            return std::pow(1.0f - t, std::max(0.01f, obj.audioSource.rolloff));
        case AudioRolloffMode::Logarithmic:
        default: {
            const float falloff = 1.0f / (1.0f + std::max(0.01f, obj.audioSource.rolloff) * ((dist - minDist) / minDist));
            return std::clamp(falloff, 0.0f, 1.0f);
        }
    }
}

glm::vec3 AudioSystem::computeSpatializedPosition(const SceneObject& obj, float spatialBlend, const glm::vec3& listenerPos) const {
    glm::vec3 sourcePos = obj.position;
    if (prefer2DSpatialAudio || HasUIComponent(obj)) {
        sourcePos.z = listenerPos.z;
    }
    return glm::mix(listenerPos, sourcePos, std::clamp(spatialBlend, 0.0f, 1.0f));
}

float AudioSystem::computeCustomAttenuation(const SceneObject& obj, const glm::vec3& listenerPos, const glm::vec3& sourcePos) const {
    float minDist = std::max(0.1f, obj.audioSource.minDistance);
    float maxDist = std::max(obj.audioSource.maxDistance, minDist + 0.5f);
    float dist = glm::length(listenerPos - sourcePos);
    if (dist <= minDist) return 1.0f;
    if (dist >= maxDist) return std::clamp(obj.audioSource.customEndGain, 0.0f, 1.0f);

    float range = maxDist - minDist;
    float midRatio = std::clamp(obj.audioSource.customMidDistance, 0.0f, 1.0f);
    float midDist = minDist + range * midRatio;
    float midGain = std::clamp(obj.audioSource.customMidGain, 0.0f, 1.0f);
    float endGain = std::clamp(obj.audioSource.customEndGain, 0.0f, 1.0f);

    if (dist <= midDist) {
        float t = (dist - minDist) / std::max(0.001f, midDist - minDist);
        return std::clamp(1.0f + (midGain - 1.0f) * t, 0.0f, 1.0f);
    }

    float t = (dist - midDist) / std::max(0.001f, maxDist - midDist);
    return std::clamp(midGain + (endGain - midGain) * t, 0.0f, 1.0f);
}

void AudioSystem::updateReverb(const std::vector<SceneObject>& objects, const glm::vec3& listenerPos) {
    if (!reverbReady) return;
    float blend = 0.0f;
    ReverbSettings target = getReverbTarget(objects, listenerPos, blend);
    applyReverbSettings(target, blend);
}

AudioSystem::ReverbSettings AudioSystem::getReverbTarget(const std::vector<SceneObject>& objects, const glm::vec3& listenerPos, float& outBlend) const {
    ReverbSettings target{};
    float bestBlend = 0.0f;

    for (const auto& obj : objects) {
        if (!IsObjectEnabledInHierarchy(obj) || !obj.hasReverbZone || !obj.reverbZone.enabled) continue;
        const auto& zone = obj.reverbZone;
        float blend = 0.0f;

        if (zone.shape == ReverbZoneShape::Sphere) {
            float minDist = std::max(0.0f, zone.minDistance);
            float maxDist = std::max(zone.maxDistance, minDist + 0.01f);
            float radius = std::max(0.01f, zone.radius);
            float dist = glm::length(listenerPos - obj.position);
            if (dist > radius) continue;
            maxDist = std::min(maxDist, radius);
            if (dist >= maxDist) continue;
            if (dist <= minDist) {
                blend = 1.0f;
            } else {
                blend = std::clamp((maxDist - dist) / (maxDist - minDist), 0.0f, 1.0f);
            }
        } else {
            glm::vec3 halfSize = glm::max(zone.boxSize * 0.5f, glm::vec3(0.01f));
            glm::vec3 delta = glm::abs(listenerPos - obj.position);
            if (delta.x > halfSize.x || delta.y > halfSize.y || delta.z > halfSize.z) continue;
            float edgeDistance = std::min({halfSize.x - delta.x, halfSize.y - delta.y, halfSize.z - delta.z});
            if (zone.blendDistance <= 0.001f) {
                blend = 1.0f;
            } else {
                blend = std::clamp(edgeDistance / zone.blendDistance, 0.0f, 1.0f);
            }
        }

        if (blend > bestBlend) {
            bestBlend = blend;
            target.room = zone.room;
            target.roomHF = zone.roomHF;
            target.roomLF = zone.roomLF;
            target.decayTime = zone.decayTime;
            target.decayHFRatio = zone.decayHFRatio;
            target.reflections = zone.reflections;
            target.reflectionsDelay = zone.reflectionsDelay;
            target.reverb = zone.reverb;
            target.reverbDelay = zone.reverbDelay;
            target.hfReference = zone.hfReference;
            target.lfReference = zone.lfReference;
            target.roomRolloffFactor = zone.roomRolloffFactor;
            target.diffusion = zone.diffusion;
            target.density = zone.density;
        }
    }

    outBlend = bestBlend;
    return target;
}

void AudioSystem::applyReverbSettings(const ReverbSettings& target, float blend) {
    ReverbSettings mixed{};
    mixed.room = target.room;
    mixed.roomHF = target.roomHF;
    mixed.roomLF = target.roomLF;
    mixed.decayTime = std::max(0.1f, target.decayTime);
    mixed.decayHFRatio = std::clamp(target.decayHFRatio, 0.1f, 2.0f);
    mixed.reflections = target.reflections;
    mixed.reflectionsDelay = std::clamp(target.reflectionsDelay, 0.0f, kReverbReflectionsMaxSeconds);
    mixed.reverb = target.reverb;
    mixed.reverbDelay = std::clamp(target.reverbDelay, 0.0f, kReverbPreDelayMaxSeconds);
    mixed.hfReference = std::clamp(target.hfReference, 1000.0f, 20000.0f);
    mixed.lfReference = std::clamp(target.lfReference, 20.0f, 1000.0f);
    mixed.roomRolloffFactor = std::max(0.0f, target.roomRolloffFactor);
    mixed.diffusion = std::clamp(target.diffusion, 0.0f, 100.0f);
    mixed.density = std::clamp(target.density, 0.0f, 100.0f);

    currentReverb.room = currentReverb.room + (mixed.room - currentReverb.room) * kReverbSmoothing;
    currentReverb.roomHF = currentReverb.roomHF + (mixed.roomHF - currentReverb.roomHF) * kReverbSmoothing;
    currentReverb.roomLF = currentReverb.roomLF + (mixed.roomLF - currentReverb.roomLF) * kReverbSmoothing;
    currentReverb.decayTime = currentReverb.decayTime + (mixed.decayTime - currentReverb.decayTime) * kReverbSmoothing;
    currentReverb.decayHFRatio = currentReverb.decayHFRatio + (mixed.decayHFRatio - currentReverb.decayHFRatio) * kReverbSmoothing;
    currentReverb.reflections = currentReverb.reflections + (mixed.reflections - currentReverb.reflections) * kReverbSmoothing;
    currentReverb.reflectionsDelay = currentReverb.reflectionsDelay + (mixed.reflectionsDelay - currentReverb.reflectionsDelay) * kReverbSmoothing;
    currentReverb.reverb = currentReverb.reverb + (mixed.reverb - currentReverb.reverb) * kReverbSmoothing;
    currentReverb.reverbDelay = currentReverb.reverbDelay + (mixed.reverbDelay - currentReverb.reverbDelay) * kReverbSmoothing;
    currentReverb.hfReference = currentReverb.hfReference + (mixed.hfReference - currentReverb.hfReference) * kReverbSmoothing;
    currentReverb.lfReference = currentReverb.lfReference + (mixed.lfReference - currentReverb.lfReference) * kReverbSmoothing;
    currentReverb.roomRolloffFactor = currentReverb.roomRolloffFactor + (mixed.roomRolloffFactor - currentReverb.roomRolloffFactor) * kReverbSmoothing;
    currentReverb.diffusion = currentReverb.diffusion + (mixed.diffusion - currentReverb.diffusion) * kReverbSmoothing;
    currentReverb.density = currentReverb.density + (mixed.density - currentReverb.density) * kReverbSmoothing;

    constexpr float kDbSoftening = 0.5f;
    constexpr float kWetScale = 0.25f;
    float reflectionsGain = DbToLinear((currentReverb.reflections + currentReverb.room) * kDbSoftening) * (blend * kWetScale);
    float reverbGain = DbToLinear((currentReverb.reverb + currentReverb.room) * kDbSoftening) * (blend * kWetScale);
    float dry = std::clamp(1.0f - blend * (currentReverb.roomRolloffFactor * 0.05f), 0.2f, 1.0f);

    ma_node_set_output_bus_volume(reinterpret_cast<ma_node*>(&reverbSplitter), 0, dry);
    reverbNode.wetGain = std::clamp(reverbGain, 0.0f, 1.0f);
    reverbNode.reflectionsGain = std::clamp(reflectionsGain, 0.0f, 1.0f);
    reverbNode.decayTime = currentReverb.decayTime;
    reverbNode.decayHFRatio = currentReverb.decayHFRatio;
    reverbNode.diffusion = currentReverb.diffusion;
    reverbNode.density = currentReverb.density;
    reverbNode.hfReference = currentReverb.hfReference;
    reverbNode.preDelaySeconds = currentReverb.reverbDelay;
    reverbNode.reflectionsDelaySeconds = currentReverb.reflectionsDelay;
}

void AudioSystem::shutdownReverbGraph() {
    if (globalFXReady) {
        ma_node_uninit(reinterpret_cast<ma_node*>(&globalFXNode), nullptr);
        globalFXReady = false;
    }
    if (reverbReady) {
        ma_sound_group_uninit(&reverbGroup);
        ma_node_uninit(reinterpret_cast<ma_node*>(&reverbNode), nullptr);
        ma_splitter_node_uninit(&reverbSplitter, nullptr);
        reverbReady = false;
    }
    currentReverb = ReverbSettings{};
}

ma_node* AudioSystem::masterBusInputNode() const {
    if (reverbReady) return const_cast<ma_node*>(reinterpret_cast<const ma_node*>(&reverbGroup));
    return ma_engine_get_endpoint(const_cast<ma_engine*>(&engine));
}

void AudioSystem::initGlobalFXNode(ma_uint32 channels, ma_uint32 sampleRate) {
    if (globalFXReady || !reverbReady) return;
    ma_node_config nodeConfig = ma_node_config_init();
    nodeConfig.vtable = &g_audiofx_node_vtable;
    nodeConfig.pInputChannels = reinterpret_cast<const ma_uint32*>(&channels);
    nodeConfig.pOutputChannels = reinterpret_cast<const ma_uint32*>(&channels);
    if (ma_node_init(ma_engine_get_node_graph(&engine), &nodeConfig, nullptr,
                     reinterpret_cast<ma_node*>(&globalFXNode)) != MA_SUCCESS) {
        return;
    }
    globalFXNode.processor = &globalFXProcessor;
    globalFXNode.channels = static_cast<int>(channels);
    globalFXProcessor.configure(static_cast<int>(channels), static_cast<int>(sampleRate));
    globalFXProcessor.setChain(globalFXChain);
    ma_node_attach_output_bus(reinterpret_cast<ma_node*>(&reverbGroup), 0,
                              reinterpret_cast<ma_node*>(&globalFXNode), 0);
    ma_node_attach_output_bus(reinterpret_cast<ma_node*>(&globalFXNode), 0,
                              reinterpret_cast<ma_node*>(&reverbSplitter), 0);
    globalFXReady = true;
}

void AudioSystem::setGlobalAudioFX(const AudioFXChain& chain) {
    globalFXChain = chain;
    if (globalFXReady) globalFXProcessor.setChain(globalFXChain);
    else if (!initialized) init();
    if (globalFXReady) globalFXProcessor.setChain(globalFXChain);
}

void AudioSystem::updateGlobalFXFromObjects(const std::vector<SceneObject>& objects) {
    const AudioFXComponent* chosen = nullptr;
    for (const auto& obj : objects) {
        if (obj.hasAudioFX && obj.audioFX.enabled && obj.audioFX.global &&
            IsObjectEnabledInHierarchy(obj)) {
            chosen = &obj.audioFX;
            break;
        }
    }

    if (chosen) {
        std::string sig = AudioFXSerialize(chosen->chain);
        if (!globalFXFromComponent || sig != globalFXSignature) {
            setGlobalAudioFX(chosen->chain);
            globalFXSignature = std::move(sig);
            globalFXFromComponent = true;
        }
    } else if (globalFXFromComponent) {
        setGlobalAudioFX(AudioFXChain{});
        globalFXSignature.clear();
        globalFXFromComponent = false;
    }
}

void AudioSystem::updateSourceFX(const SceneObject& obj, ActiveSound& snd) {
    static const AudioFXChain kEmptyChain{};
    const bool localFX = obj.hasAudioFX && obj.audioFX.enabled && !obj.audioFX.global;
    const AudioFXChain& chain = localFX ? obj.audioFX.chain : kEmptyChain;
    const bool chainActive = chain.active();
    if (!chainActive && snd.fxSignature.empty() && !snd.fxNodeReady) return;

    float semis = chainActive ? chain.pitchSemitones() : 0.0f;
    ma_sound_set_pitch(&snd.sound, std::pow(2.0f, std::clamp(semis, -24.0f, 24.0f) / 12.0f));

    const std::string signature = chainActive ? AudioFXSerialize(chain) : std::string();
    if (signature == snd.fxSignature) return;
    snd.fxSignature = signature;

    if (!chain.active()) {
        destroySourceFX(snd);
        return;
    }

    ma_uint32 channels = ma_engine_get_channels(&engine);
    ma_uint32 sampleRate = ma_engine_get_sample_rate(&engine);
    if (!snd.fxNodeReady) {
        ma_node_config nodeConfig = ma_node_config_init();
        nodeConfig.vtable = &g_audiofx_node_vtable;
        nodeConfig.pInputChannels = reinterpret_cast<const ma_uint32*>(&channels);
        nodeConfig.pOutputChannels = reinterpret_cast<const ma_uint32*>(&channels);
        if (ma_node_init(ma_engine_get_node_graph(&engine), &nodeConfig, nullptr,
                         reinterpret_cast<ma_node*>(&snd.fxNode)) != MA_SUCCESS) {
            return;
        }
        snd.fxProcessor = std::make_unique<AudioFXProcessor>();
        snd.fxProcessor->configure(static_cast<int>(channels), static_cast<int>(sampleRate));
        snd.fxNode.processor = snd.fxProcessor.get();
        snd.fxNode.channels = static_cast<int>(channels);
        ma_node_attach_output_bus(reinterpret_cast<ma_node*>(&snd.sound), 0,
                                  reinterpret_cast<ma_node*>(&snd.fxNode), 0);
        ma_node_attach_output_bus(reinterpret_cast<ma_node*>(&snd.fxNode), 0, masterBusInputNode(), 0);
        snd.fxNodeReady = true;
    }
    if (snd.fxProcessor) snd.fxProcessor->setChain(chain);
}

void AudioSystem::destroySourceFX(ActiveSound& snd) {
    if (!snd.fxNodeReady) return;
    ma_node_attach_output_bus(reinterpret_cast<ma_node*>(&snd.sound), 0, masterBusInputNode(), 0);
    ma_node_uninit(reinterpret_cast<ma_node*>(&snd.fxNode), nullptr);
    snd.fxNodeReady = false;
    snd.fxProcessor.reset();
    snd.fxNode.processor = nullptr;
}

std::shared_ptr<AudioSystem::DecodedAudioData> AudioSystem::decodeClipToMemory(const std::string& rawPath) {
    const std::string path = ResolveClipPath(rawPath);
#if MODULARITY_HAS_SNDFILE
    SF_INFO info{};
    SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
    if (!file) {
        return nullptr;
    }

    if (info.frames <= 0 || info.channels <= 0 || info.samplerate <= 0) {
        sf_close(file);
        return nullptr;
    }

    auto decoded = std::make_shared<DecodedAudioData>();
    decoded->channels = static_cast<ma_uint32>(info.channels);
    decoded->sampleRate = static_cast<ma_uint32>(info.samplerate);
    decoded->frameCount = static_cast<ma_uint64>(info.frames);
    decoded->pcmFrames.resize(static_cast<size_t>(decoded->frameCount * decoded->channels));

    const sf_count_t framesRead = sf_readf_float(file, decoded->pcmFrames.data(), info.frames);
    sf_close(file);
    if (framesRead <= 0) {
        return nullptr;
    }

    decoded->frameCount = static_cast<ma_uint64>(framesRead);
    decoded->pcmFrames.resize(static_cast<size_t>(decoded->frameCount * decoded->channels));
    if (ma_audio_buffer_ref_init(ma_format_f32, decoded->channels, decoded->pcmFrames.data(), decoded->frameCount, &decoded->buffer) != MA_SUCCESS) {
        return nullptr;
    }

    decoded->initialized = true;
    return decoded;
#else
    (void)path;
    return nullptr;
#endif
}

bool AudioSystem::initSoundFromPath(const std::string& rawPath, ma_uint32 flags, ma_sound_group* group, ma_sound& sound,
                                    std::shared_ptr<DecodedAudioData>& decodedData) {
    const std::string path = ResolveClipPath(rawPath);
    ma_result res = ma_sound_init_from_file(&engine, path.c_str(), flags, group, nullptr, &sound);
    if (res == MA_SUCCESS) {
        return true;
    }

    decodedData = decodeClipToMemory(path);
    if (!decodedData) {
        std::cerr << "AudioSystem: miniaudio load failed for " << path << " (" << res << ")\n";
        return false;
    }

    res = ma_sound_init_from_data_source(&engine, &decodedData->buffer.ds, flags & ~MA_SOUND_FLAG_STREAM, group, &sound);
    if (res != MA_SUCCESS) {
        std::cerr << "AudioSystem: decoded fallback load failed for " << path << " (" << res << ")\n";
        releaseDecodedAudio(decodedData);
        return false;
    }

    return true;
}

void AudioSystem::releaseDecodedAudio(std::shared_ptr<DecodedAudioData>& decodedData) {
    if (decodedData && decodedData->initialized) {
        ma_audio_buffer_ref_uninit(&decodedData->buffer);
        decodedData->initialized = false;
    }
    decodedData.reset();
}

AudioClipPreview AudioSystem::loadPreview(const std::string& rawPath) {
    const std::string path = ResolveClipPath(rawPath);
    AudioClipPreview preview;
    preview.path = rawPath;

    ma_decoder decoder;
    ma_decoder_config decoderConfig = ma_decoder_config_init_default();
#if MODULARITY_HAS_OPUSFILE
    decoderConfig.ppCustomBackendVTables = g_custom_backend_table;
    decoderConfig.customBackendCount = sizeof(g_custom_backend_table) / sizeof(g_custom_backend_table[0]);
#endif
    if (ma_decoder_init_file(path.c_str(), &decoderConfig, &decoder) != MA_SUCCESS) {
        auto decoded = decodeClipToMemory(path);
        if (!decoded) {
            return preview;
        }
        preview.channels = decoded->channels;
        preview.sampleRate = decoded->sampleRate;
        preview.durationSeconds = (preview.sampleRate > 0)
            ? static_cast<double>(decoded->frameCount) / static_cast<double>(preview.sampleRate)
            : 0.0;
        BuildWaveformPreview(preview, decoded->pcmFrames.data(), decoded->frameCount);
        preview.loaded = true;
        releaseDecodedAudio(decoded);
        return preview;
    }

    ma_uint64 totalFrames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
    preview.channels = decoder.outputChannels;
    preview.sampleRate = decoder.outputSampleRate;
    preview.durationSeconds = (preview.sampleRate > 0) ? static_cast<double>(totalFrames) / static_cast<double>(preview.sampleRate) : 0.0;

    if (totalFrames == 0 || preview.channels == 0) {
        ma_decoder_uninit(&decoder);
        return preview;
    }

    std::vector<float> temp(kPreviewChunkFrames * preview.channels);
    std::vector<float> pcmFrames;
    pcmFrames.reserve(static_cast<size_t>(totalFrames * preview.channels));
    ma_uint64 frameCursor = 0;

    while (frameCursor < totalFrames) {
        ma_uint64 framesToRead = std::min<ma_uint64>(kPreviewChunkFrames, totalFrames - frameCursor);
        ma_uint64 framesRead = 0;
        ma_result readResult = ma_decoder_read_pcm_frames(&decoder, temp.data(), framesToRead, &framesRead);
        if (readResult != MA_SUCCESS && readResult != MA_AT_END) {
            break;
        }
        if (framesRead == 0) break;
        pcmFrames.insert(pcmFrames.end(), temp.begin(), temp.begin() + static_cast<std::ptrdiff_t>(framesRead * preview.channels));
        frameCursor += framesRead;
    }

    ma_decoder_uninit(&decoder);
    if (frameCursor == 0) {
        return preview;
    }
    BuildWaveformPreview(preview, pcmFrames.data(), frameCursor);
    preview.loaded = true;
    return preview;
}

const AudioClipPreview* AudioSystem::getPreview(const std::string& path) {
    if (path.empty()) return nullptr;
    auto it = previewCache.find(path);
    if (it == previewCache.end()) {
        previewCache[path] = loadPreview(path);
        it = previewCache.find(path);
    }
    if (it != previewCache.end() && it->second.loaded) {
        return &it->second;
    }
    return nullptr;
}
