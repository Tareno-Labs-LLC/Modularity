#pragma once

// Exports re-encode lossless audio (WAV, AIFF, FLAC) to Ogg Vorbis, as Unity imports audio as
// Vorbis by default. The encoded clip ships beside the original path with ".ogg" appended
// (Assets/sfx/hit.wav -> Assets/sfx/hit.wav.ogg) and the original is left out of the build, so
// scenes and scripts keep saying "hit.wav": AudioSystem resolves a missing lossless clip to that
// sidecar. Appending rather than replacing the extension means hit.wav and hit.ogg never collide.
//
// MP3 and OGG are left alone - re-encoding lossy audio costs quality for little size.

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace Modularity::AudioExport {

inline constexpr const char* kSidecarSuffix = ".ogg";

inline bool IsLosslessClipPath(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".wav" || ext == ".wave" || ext == ".aif" || ext == ".aiff" || ext == ".flac";
}

struct CompressionStats {
    int converted = 0;
    int reusedFromCache = 0;
    int keptOriginal = 0;  // encoded result was not meaningfully smaller
    int failed = 0;
    uint64_t bytesBefore = 0;
    uint64_t bytesAfter = 0;
};

// False when the build has no FFmpeg or FFmpeg has no libvorbis encoder.
bool EncoderAvailable();

// For the LGPL notice an export ships with the FFmpeg libraries. Empty without FFmpeg.
// Version looks like "n8.1.2-54-gc573a95381", which names the exact upstream revision.
std::string FfmpegVersion();
std::string FfmpegConfiguration();

// quality is libvorbis's 0-10 scale (ffmpeg -q:a); 5 is about 160 kbps for stereo 44.1 kHz.
bool EncodeFileToVorbis(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        float quality,
                        std::string& error);

// Replaces every lossless clip under stageRoot/Assets with its Vorbis sidecar, encoding on several
// threads. cacheDir keeps results keyed by source content and quality, so an unchanged clip is
// not re-encoded on the next export. log receives one line per failure; cancelled is polled.
CompressionStats CompressStagedAudio(const std::filesystem::path& stageRoot,
                                     const std::filesystem::path& cacheDir,
                                     float quality,
                                     const std::function<void(const std::string&)>& log,
                                     const std::function<bool()>& cancelled);

} // namespace Modularity::AudioExport
