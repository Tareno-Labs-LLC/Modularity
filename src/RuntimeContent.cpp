#include "RuntimeContent.h"
#include "../include/Platform/AssetSource.h"
#include "Sha256.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace {
constexpr char kBundleMagic[] = "MODRUNTIME1";
constexpr uint32_t kBundleVersion = 1u;
constexpr char kBundleReadyMarker[] = ".runtime_bundle_ready";

// v2 (central directory) format.
constexpr char kBundleV2Magic[] = "MODBUNDLE2";   // 10 bytes, no terminator on disk
constexpr char kBundleV2EndMagic[] = "MODBEND2";  // 8 bytes, no terminator on disk
constexpr uint32_t kBundleV2Version = 2u;
constexpr uint32_t kBundleFlagIntegrity = 0x1u;   // per-file SHA-256 present in the directory
constexpr size_t kBundleV2MagicLen = 10;
constexpr size_t kBundleV2EndMagicLen = 8;
// footer: dirOffset,u64 + entryCount,u64 + codeDirOffset,u64 + codeCount,u64 + hash[32] + endMagic[8]
constexpr size_t kBundleV2FooterSize = 4 * sizeof(uint64_t) + 32 + kBundleV2EndMagicLen;

bool ReadStreamPod(Modularity::Platform::AssetStream& s, void* dst, size_t bytes) {
    size_t total = 0;
    auto* out = static_cast<unsigned char*>(dst);
    while (total < bytes) {
        const size_t n = s.Read(out + total, bytes - total);
        if (n == 0) return false;
        total += n;
    }
    return true;
}
template <typename T>
bool ReadStreamValue(Modularity::Platform::AssetStream& s, T& value) {
    return ReadStreamPod(s, &value, sizeof(T));
}

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
                                 std::string& error,
                                 const RuntimeBundleProgress& progress) {
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

    const uint64_t bundleSize = static_cast<uint64_t>(std::max<int64_t>(stream->Size(), 0));
    uint64_t bytesDone = 0;
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
            bytesDone += static_cast<uint64_t>(chunk);
            if (progress && !progress(bytesDone, bundleSize)) {
                error = "Runtime content extraction cancelled.";
                return false;
            }
        }
    }

    return WriteBundleReadyMarker(outputRoot, bundlePath, error);
}

// ---------------------------------------------------------------------------
// v2: central-directory bundle, mountable in place (data.win style).
// ---------------------------------------------------------------------------

namespace {
std::string NormalizeBundleKey(const std::string& raw) {
    std::string s = raw;
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.rfind("./", 0) == 0) s.erase(0, 2);
    while (!s.empty() && s.front() == '/') s.erase(0, 1);
    return s;
}
} // namespace

bool WriteRuntimeContentBundleV2(const fs::path& bundlePath,
                                 const std::vector<RuntimeBundleEntry>& entries,
                                 const std::vector<RuntimeBundleCodeUnit>& codeUnits,
                                 const RuntimeBundleWriteOptions& options,
                                 std::string& error) {
    if (entries.empty() && codeUnits.empty()) {
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

    // Everything written before the footer is folded into one running hash; that is the
    // whole-bundle integrity value the runtime re-computes to detect tampering.
    Modularity::Sha256 bundleHash;
    bool writeOk = true;
    auto raw = [&](const void* data, size_t bytes) {
        if (!writeOk) return;
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
        bundleHash.update(data, bytes);
        if (!out) writeOk = false;
    };
    auto rawU32 = [&](uint32_t v) { raw(&v, sizeof(v)); };
    auto rawU64 = [&](uint64_t v) { raw(&v, sizeof(v)); };
    auto tell = [&]() -> uint64_t {
        return static_cast<uint64_t>(static_cast<std::streamoff>(out.tellp()));
    };

    const uint32_t flags = options.includeIntegrity ? kBundleFlagIntegrity : 0u;
    raw(kBundleV2Magic, kBundleV2MagicLen);
    rawU32(kBundleV2Version);
    rawU32(flags);

    struct DirRecord {
        std::string path;
        uint64_t offset = 0;
        uint64_t size = 0;
        uint8_t hash[32] = {};
    };
    std::vector<DirRecord> dir;
    dir.reserve(entries.size());

    std::vector<char> buffer(64 * 1024);
    for (const RuntimeBundleEntry& entry : entries) {
        std::error_code ec;
        const fs::path source = fs::absolute(entry.sourcePath, ec);
        if (ec || !fs::exists(source) || !fs::is_regular_file(source)) {
            error = "Runtime bundle source file is missing: " + entry.sourcePath.string();
            return false;
        }
        const std::string archivePath = NormalizeBundleKey(entry.archivePath.generic_string());
        if (archivePath.empty()) {
            error = "Runtime bundle archive path is empty for: " + source.string();
            return false;
        }

        std::ifstream in(source, std::ios::binary);
        if (!in.is_open()) {
            error = "Failed to open runtime bundle source file: " + source.string();
            return false;
        }

        DirRecord record;
        record.path = archivePath;
        record.offset = tell();
        Modularity::Sha256 fileHash;
        uint64_t written = 0;
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize n = in.gcount();
            if (n > 0) {
                raw(buffer.data(), static_cast<size_t>(n));
                if (options.includeIntegrity) fileHash.update(buffer.data(), static_cast<size_t>(n));
                written += static_cast<uint64_t>(n);
                if (!writeOk) { error = "Failed to write runtime bundle payload: " + archivePath; return false; }
            }
        }
        record.size = written;
        if (options.includeIntegrity) fileHash.finish(record.hash);
        dir.push_back(std::move(record));
    }

    std::vector<DirRecord> codeDir;
    codeDir.reserve(codeUnits.size());
    for (const RuntimeBundleCodeUnit& unit : codeUnits) {
        DirRecord record;
        record.path = unit.name;
        record.offset = tell();
        record.size = static_cast<uint64_t>(unit.bytes.size());
        if (!unit.bytes.empty()) raw(unit.bytes.data(), unit.bytes.size());
        codeDir.push_back(std::move(record));
    }

    const uint64_t dirOffset = tell();
    for (const DirRecord& rec : dir) {
        rawU32(static_cast<uint32_t>(rec.path.size()));
        raw(rec.path.data(), rec.path.size());
        rawU64(rec.offset);
        rawU64(rec.size);
        if (options.includeIntegrity) raw(rec.hash, sizeof(rec.hash));
    }

    const uint64_t codeDirOffset = tell();
    for (const DirRecord& rec : codeDir) {
        rawU32(static_cast<uint32_t>(rec.path.size()));
        raw(rec.path.data(), rec.path.size());
        rawU64(rec.offset);
        rawU64(rec.size);
    }

    if (!writeOk) {
        error = "Failed to write runtime bundle directory.";
        return false;
    }

    // Footer is NOT hashed (it carries the hash of everything before it).
    uint8_t digest[32];
    bundleHash.finish(digest);
    auto footU64 = [&](uint64_t v) { out.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
    footU64(dirOffset);
    footU64(static_cast<uint64_t>(dir.size()));
    footU64(codeDirOffset);
    footU64(static_cast<uint64_t>(codeDir.size()));
    out.write(reinterpret_cast<const char*>(digest), sizeof(digest));
    out.write(kBundleV2EndMagic, kBundleV2EndMagicLen);

    out.flush();
    if (!out) {
        error = "Failed to finalize runtime bundle: " + bundlePath.string();
        return false;
    }
    return true;
}

namespace {
bool ReadV1Index(Modularity::Platform::AssetStream& stream, RuntimeBundleIndex& out, std::string& error) {
    // Header already consumed up to just after the 11-byte magic; read version + entry count.
    uint32_t version = 0;
    uint32_t entryCount = 0;
    if (!ReadStreamValue(stream, version) || version != kBundleVersion ||
        !ReadStreamValue(stream, entryCount)) {
        error = "Unsupported runtime bundle format.";
        return false;
    }
    out.format = RuntimeBundleFormat::V1Sequential;
    out.entries.reserve(entryCount);
    for (uint32_t i = 0; i < entryCount; ++i) {
        uint32_t pathLen = 0;
        uint64_t fileSize = 0;
        if (!ReadStreamValue(stream, pathLen) || !ReadStreamValue(stream, fileSize) || pathLen == 0) {
            error = "Failed to read runtime bundle entry metadata.";
            return false;
        }
        std::string path(pathLen, '\0');
        if (!ReadStreamPod(stream, path.data(), path.size())) {
            error = "Failed to read runtime bundle entry path.";
            return false;
        }
        RuntimeBundleIndexEntry entry;
        entry.path = NormalizeBundleKey(path);
        entry.offset = static_cast<uint64_t>(stream.Tell());
        entry.size = fileSize;
        out.entries.push_back(std::move(entry));
        if (!stream.Seek(static_cast<int64_t>(fileSize), SEEK_CUR)) {
            error = "Runtime bundle payload truncated.";
            return false;
        }
    }
    return true;
}

bool ReadV2Index(Modularity::Platform::AssetStream& stream, uint32_t flags,
                 RuntimeBundleIndex& out, std::string& error) {
    const int64_t fileSize = stream.Size();
    if (fileSize < 0 || static_cast<uint64_t>(fileSize) < kBundleV2FooterSize) {
        error = "Runtime bundle is too small.";
        return false;
    }
    if (!stream.Seek(fileSize - static_cast<int64_t>(kBundleV2FooterSize), SEEK_SET)) {
        error = "Failed to seek runtime bundle footer.";
        return false;
    }
    uint64_t dirOffset = 0, entryCount = 0, codeDirOffset = 0, codeCount = 0;
    uint8_t storedHash[32] = {};
    char endMagic[kBundleV2EndMagicLen] = {};
    if (!ReadStreamValue(stream, dirOffset) || !ReadStreamValue(stream, entryCount) ||
        !ReadStreamValue(stream, codeDirOffset) || !ReadStreamValue(stream, codeCount) ||
        !ReadStreamPod(stream, storedHash, sizeof(storedHash)) ||
        !ReadStreamPod(stream, endMagic, sizeof(endMagic))) {
        error = "Failed to read runtime bundle footer.";
        return false;
    }
    if (std::memcmp(endMagic, kBundleV2EndMagic, kBundleV2EndMagicLen) != 0) {
        error = "Runtime bundle footer is corrupt.";
        return false;
    }
    out.format = RuntimeBundleFormat::V2Directory;
    std::memcpy(out.bundleHash, storedHash, sizeof(storedHash));
    out.hasBundleHash = true;

    const bool hasHashes = (flags & kBundleFlagIntegrity) != 0;
    if (!stream.Seek(static_cast<int64_t>(dirOffset), SEEK_SET)) {
        error = "Failed to seek runtime bundle directory.";
        return false;
    }
    out.entries.reserve(entryCount);
    for (uint64_t i = 0; i < entryCount; ++i) {
        uint32_t pathLen = 0;
        if (!ReadStreamValue(stream, pathLen) || pathLen == 0 || pathLen > (1u << 20)) {
            error = "Runtime bundle directory entry is corrupt.";
            return false;
        }
        std::string path(pathLen, '\0');
        RuntimeBundleIndexEntry entry;
        if (!ReadStreamPod(stream, path.data(), path.size()) ||
            !ReadStreamValue(stream, entry.offset) || !ReadStreamValue(stream, entry.size)) {
            error = "Failed to read runtime bundle directory entry.";
            return false;
        }
        entry.path = NormalizeBundleKey(path);
        if (hasHashes) {
            if (!ReadStreamPod(stream, entry.sha256, sizeof(entry.sha256))) {
                error = "Failed to read runtime bundle entry hash.";
                return false;
            }
            entry.hasHash = true;
        }
        out.entries.push_back(std::move(entry));
    }

    if (!stream.Seek(static_cast<int64_t>(codeDirOffset), SEEK_SET)) {
        error = "Failed to seek runtime bundle code directory.";
        return false;
    }
    out.codeUnits.reserve(codeCount);
    for (uint64_t i = 0; i < codeCount; ++i) {
        uint32_t nameLen = 0;
        if (!ReadStreamValue(stream, nameLen) || nameLen == 0 || nameLen > (1u << 20)) {
            error = "Runtime bundle code directory is corrupt.";
            return false;
        }
        std::string name(nameLen, '\0');
        RuntimeBundleCodeIndexEntry unit;
        if (!ReadStreamPod(stream, name.data(), name.size()) ||
            !ReadStreamValue(stream, unit.offset) || !ReadStreamValue(stream, unit.size)) {
            error = "Failed to read runtime bundle code entry.";
            return false;
        }
        unit.name = name;
        out.codeUnits.push_back(std::move(unit));
    }
    return true;
}
} // namespace

bool ReadRuntimeBundleIndex(Modularity::Platform::AssetSource& source,
                            const fs::path& bundlePath,
                            RuntimeBundleIndex& outIndex,
                            std::string& error) {
    outIndex = RuntimeBundleIndex{};
    auto stream = source.Open(bundlePath.string());
    if (!stream) {
        error = "Failed to open runtime bundle: " + bundlePath.string();
        return false;
    }

    char magic[16] = {};
    if (!ReadStreamPod(*stream, magic, kBundleV2MagicLen)) {
        error = "Runtime bundle is truncated: " + bundlePath.string();
        return false;
    }
    if (std::memcmp(magic, kBundleV2Magic, kBundleV2MagicLen) == 0) {
        uint32_t version = 0, flags = 0;
        if (!ReadStreamValue(*stream, version) || version != kBundleV2Version ||
            !ReadStreamValue(*stream, flags)) {
            error = "Unsupported runtime bundle version: " + bundlePath.string();
            return false;
        }
        return ReadV2Index(*stream, flags, outIndex, error);
    }

    // Fall back to v1: its magic is one byte longer, so read the remaining byte and compare.
    if (!stream->Seek(0, SEEK_SET)) {
        error = "Runtime bundle is not seekable: " + bundlePath.string();
        return false;
    }
    char v1magic[sizeof(kBundleMagic) - 1] = {};
    if (!ReadStreamPod(*stream, v1magic, sizeof(v1magic)) ||
        std::memcmp(v1magic, kBundleMagic, sizeof(v1magic)) != 0) {
        error = "Invalid runtime bundle header: " + bundlePath.string();
        return false;
    }
    return ReadV1Index(*stream, outIndex, error);
}

bool VerifyRuntimeBundleIntegrity(Modularity::Platform::AssetSource& source,
                                  const fs::path& bundlePath,
                                  const RuntimeBundleIndex& index,
                                  std::string& error) {
    if (index.format != RuntimeBundleFormat::V2Directory || !index.hasBundleHash) {
        return true;  // nothing to verify (v1 bundles carry no whole-bundle hash)
    }
    auto stream = source.Open(bundlePath.string());
    if (!stream) {
        error = "Failed to open runtime bundle for verification: " + bundlePath.string();
        return false;
    }
    const int64_t fileSize = stream->Size();
    if (fileSize < 0 || static_cast<uint64_t>(fileSize) < kBundleV2FooterSize) {
        error = "Runtime bundle is too small to verify.";
        return false;
    }
    uint64_t remaining = static_cast<uint64_t>(fileSize) - kBundleV2FooterSize;
    Modularity::Sha256 hash;
    std::vector<char> buffer(64 * 1024);
    while (remaining > 0) {
        const size_t chunk = static_cast<size_t>(
            std::min<uint64_t>(remaining, static_cast<uint64_t>(buffer.size())));
        if (!ReadStreamPod(*stream, buffer.data(), chunk)) {
            error = "Runtime bundle truncated during verification.";
            return false;
        }
        hash.update(buffer.data(), chunk);
        remaining -= chunk;
    }
    uint8_t digest[32];
    hash.finish(digest);
    if (std::memcmp(digest, index.bundleHash, sizeof(digest)) != 0) {
        error = "Runtime bundle integrity check failed (content hash mismatch).";
        return false;
    }
    return true;
}
