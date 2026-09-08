#include "RuntimeContent.h"
#include "../include/Platform/AssetSource.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace {
constexpr char kBundleMagic[] = "MODRUNTIME1";
constexpr uint32_t kBundleVersion = 1u;
constexpr char kBundleReadyMarker[] = ".runtime_bundle_ready";

template <typename T>
bool WritePod(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return static_cast<bool>(out);
}

template <typename T>
bool ReadPod(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

template <typename T>
bool ReadPodStream(Modularity::Platform::AssetStream& s, T& value) {
    return s.Read(&value, sizeof(T)) == sizeof(T);
}

bool ReadStreamExact(Modularity::Platform::AssetStream& s, void* dst, size_t bytes) {
    size_t total = 0;
    auto* out = static_cast<unsigned char*>(dst);
    while (total < bytes) {
        const size_t n = s.Read(out + total, bytes - total);
        if (n == 0) return false;
        total += n;
    }
    return true;
}

bool EnsureParentDirectory(const fs::path& path, std::string& error) {
    std::error_code ec;
    const fs::path parent = path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            error = "Failed to create directory: " + parent.string();
            return false;
        }
    }
    return true;
}

bool IsReusableBundleExtraction(const fs::path& outputRoot) {
    std::error_code ec;
    if (!fs::exists(outputRoot / kBundleReadyMarker, ec) || ec) {
        return false;
    }
    ec.clear();
    return fs::exists(outputRoot / "project.modu", ec) && !ec;
}

bool WriteBundleReadyMarker(const fs::path& outputRoot,
                            const fs::path& bundlePath,
                            std::string& error) {
    const fs::path markerPath = outputRoot / kBundleReadyMarker;
    if (!EnsureParentDirectory(markerPath, error)) {
        return false;
    }

    std::ofstream marker(markerPath, std::ios::trunc);
    if (!marker.is_open()) {
        error = "Failed to write runtime extraction marker: " + markerPath.string();
        return false;
    }

    marker << bundlePath.filename().string() << "\n";
    if (!marker) {
        error = "Failed to finalize runtime extraction marker: " + markerPath.string();
        return false;
    }
    return true;
}
} // namespace

bool WriteRuntimeContentBundle(const fs::path& bundlePath,
                               const std::vector<RuntimeBundleEntry>& entries,
                               std::string& error) {
    if (entries.empty()) {
        error = "Runtime bundle has no files to write.";
        return false;
    }
    if (!EnsureParentDirectory(bundlePath, error)) {
        return false;
    }

    std::ofstream out(bundlePath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        error = "Failed to open runtime bundle for writing: " + bundlePath.string();
        return false;
    }

    out.write(kBundleMagic, sizeof(kBundleMagic) - 1);
    if (!out) {
        error = "Failed to write runtime bundle header.";
        return false;
    }
    if (!WritePod(out, kBundleVersion)) {
        error = "Failed to write runtime bundle version.";
        return false;
    }

    const uint32_t entryCount = static_cast<uint32_t>(entries.size());
    if (!WritePod(out, entryCount)) {
        error = "Failed to write runtime bundle entry count.";
        return false;
    }

    std::vector<char> buffer(64 * 1024);
    for (const RuntimeBundleEntry& entry : entries) {
        std::error_code ec;
        const fs::path source = fs::absolute(entry.sourcePath, ec);
        if (ec || !fs::exists(source)) {
            error = "Runtime bundle source file is missing: " + entry.sourcePath.string();
            return false;
        }
        if (!fs::is_regular_file(source)) {
            error = "Runtime bundle source is not a file: " + entry.sourcePath.string();
            return false;
        }

        const std::string archivePath = entry.archivePath.generic_string();
        if (archivePath.empty()) {
            error = "Runtime bundle archive path is empty for: " + source.string();
            return false;
        }

        const uint32_t pathLength = static_cast<uint32_t>(archivePath.size());
        const uint64_t fileSize = static_cast<uint64_t>(fs::file_size(source, ec));
        if (ec) {
            error = "Failed to stat runtime bundle source file: " + source.string();
            return false;
        }

        if (!WritePod(out, pathLength) || !WritePod(out, fileSize)) {
            error = "Failed to write runtime bundle entry metadata.";
            return false;
        }
        out.write(archivePath.data(), static_cast<std::streamsize>(archivePath.size()));
        if (!out) {
            error = "Failed to write runtime bundle archive path.";
            return false;
        }

        std::ifstream in(source, std::ios::binary);
        if (!in.is_open()) {
            error = "Failed to open runtime bundle source file: " + source.string();
            return false;
        }

        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize bytesRead = in.gcount();
            if (bytesRead > 0) {
                out.write(buffer.data(), bytesRead);
                if (!out) {
                    error = "Failed to write runtime bundle file payload: " + archivePath;
                    return false;
                }
            }
        }
    }

    out.flush();
    if (!out) {
        error = "Failed to finalize runtime bundle: " + bundlePath.string();
        return false;
    }
    return true;
}

bool ExtractRuntimeContentBundle(const fs::path& bundlePath,
                                 const fs::path& outputRoot,
                                 std::string& error) {
    auto stream = Modularity::Platform::GetAssetSource().Open(bundlePath.string());
    if (!stream) {
        error = "Failed to open runtime bundle: " + bundlePath.string();
        return false;
    }

    char magic[sizeof(kBundleMagic) - 1] = {};
    if (!ReadStreamExact(*stream, magic, sizeof(magic)) ||
        std::memcmp(magic, kBundleMagic, sizeof(magic)) != 0) {
        error = "Invalid runtime bundle header: " + bundlePath.string();
        return false;
    }

    uint32_t version = 0;
    uint32_t entryCount = 0;
    if (!ReadPodStream(*stream, version) || version != kBundleVersion ||
        !ReadPodStream(*stream, entryCount)) {
        error = "Unsupported runtime bundle format: " + bundlePath.string();
        return false;
    }

    if (IsReusableBundleExtraction(outputRoot)) {
        return WriteBundleReadyMarker(outputRoot, bundlePath, error);
    }

    std::error_code ec;
    fs::remove_all(outputRoot, ec);
    ec.clear();
    fs::create_directories(outputRoot, ec);
    if (ec) {
        error = "Failed to create runtime extraction directory: " + outputRoot.string();
        return false;
    }

    std::vector<char> buffer(64 * 1024);
    for (uint32_t entryIndex = 0; entryIndex < entryCount; ++entryIndex) {
        uint32_t pathLength = 0;
        uint64_t fileSize = 0;
        if (!ReadPodStream(*stream, pathLength) || !ReadPodStream(*stream, fileSize) || pathLength == 0) {
            error = "Failed to read runtime bundle entry metadata.";
            return false;
        }

        std::string archivePath(pathLength, '\0');
        if (!ReadStreamExact(*stream, archivePath.data(), archivePath.size())) {
            error = "Failed to read runtime bundle entry path.";
            return false;
        }

        fs::path relativePath = fs::path(archivePath).lexically_normal();
        if (relativePath.is_absolute()) {
            error = "Runtime bundle entry must be relative: " + archivePath;
            return false;
        }
        for (const auto& part : relativePath) {
            if (part == "..") {
                error = "Runtime bundle entry escapes output root: " + archivePath;
                return false;
            }
        }

        const fs::path outPath = outputRoot / relativePath;
        if (!EnsureParentDirectory(outPath, error)) {
            return false;
        }

        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            error = "Failed to create extracted runtime file: " + outPath.string();
            return false;
        }

        uint64_t remaining = fileSize;
        while (remaining > 0) {
            const size_t chunk = static_cast<size_t>(
                std::min<uint64_t>(remaining, static_cast<uint64_t>(buffer.size())));
            if (!ReadStreamExact(*stream, buffer.data(), chunk)) {
                error = "Runtime bundle payload truncated for: " + archivePath;
                return false;
            }
            out.write(buffer.data(), static_cast<std::streamsize>(chunk));
            if (!out) {
                error = "Failed to write extracted runtime file: " + outPath.string();
                return false;
            }
            remaining -= static_cast<uint64_t>(chunk);
        }
    }

    return WriteBundleReadyMarker(outputRoot, bundlePath, error);
}
