#pragma once

#include "Common.h"

#include <cstdint>
#include <functional>

namespace Modularity::Platform { class AssetSource; class AssetStream; }

struct RuntimeBundleEntry {
    fs::path sourcePath;
    fs::path archivePath;
};

// Reserved for the future portable/bytecode target (the PS2 port): a named code unit that lives
// in the bundle's dedicated CODE section, separate from the asset blobs, the way data.win keeps
// its CODE chunk apart from sprites and audio. Empty today; the format carries it so the
// container will not need another break once ModuCPP has a bytecode backend.
struct RuntimeBundleCodeUnit {
    std::string name;             // e.g. a script id
    std::vector<uint8_t> bytes;   // compiled bytecode blob
};

struct RuntimeBundleWriteOptions {
    bool includeIntegrity = true; // write per-file and whole-bundle SHA-256
};

// ---- v1 (sequential; extracted to disk before use). Kept for back-compat. ----
bool WriteRuntimeContentBundle(const fs::path& bundlePath,
                               const std::vector<RuntimeBundleEntry>& entries,
                               std::string& error);

// Called with bytes of the bundle consumed so far and the bundle's total size. Return false to
// cancel; extraction then fails and leaves no ready marker, so the partial output is pruned later.
using RuntimeBundleProgress = std::function<bool(uint64_t bytesDone, uint64_t bytesTotal)>;

bool ExtractRuntimeContentBundle(const fs::path& bundlePath,
                                 const fs::path& outputRoot,
                                 std::string& error,
                                 const RuntimeBundleProgress& progress = {});

// ---- v2 (central directory; random access, mountable in place like data.win). ----
bool WriteRuntimeContentBundleV2(const fs::path& bundlePath,
                                 const std::vector<RuntimeBundleEntry>& entries,
                                 const std::vector<RuntimeBundleCodeUnit>& codeUnits,
                                 const RuntimeBundleWriteOptions& options,
                                 std::string& error);

enum class RuntimeBundleFormat { Unknown, V1Sequential, V2Directory };

struct RuntimeBundleIndexEntry {
    std::string path;      // forward-slash, relative
    uint64_t offset = 0;   // absolute offset of the payload in the bundle file
    uint64_t size = 0;
    bool hasHash = false;
    uint8_t sha256[32] = {};
};

struct RuntimeBundleCodeIndexEntry {
    std::string name;
    uint64_t offset = 0;
    uint64_t size = 0;
};

// The directory of a bundle, enough to random-access every entry without reading payloads.
// Works for both v1 (scanned) and v2 (footer + directory).
struct RuntimeBundleIndex {
    RuntimeBundleFormat format = RuntimeBundleFormat::Unknown;
    std::vector<RuntimeBundleIndexEntry> entries;
    std::vector<RuntimeBundleCodeIndexEntry> codeUnits;
    bool hasBundleHash = false;
    uint8_t bundleHash[32] = {};
};

// Reads only the directory. `bundlePath` is opened through `source` so this works on desktop and
// on Android (APK-backed). Returns false and sets `error` on a malformed or truncated bundle.
bool ReadRuntimeBundleIndex(Modularity::Platform::AssetSource& source,
                            const fs::path& bundlePath,
                            RuntimeBundleIndex& outIndex,
                            std::string& error);

// Recomputes the whole-bundle SHA-256 (v2 only) and compares it to the stored value. This is the
// built-in tamper check the runtime runs when integrity verification is enabled in Build Settings.
// Returns true when the hash matches (or the bundle has none to check); false + error otherwise.
bool VerifyRuntimeBundleIntegrity(Modularity::Platform::AssetSource& source,
                                  const fs::path& bundlePath,
                                  const RuntimeBundleIndex& index,
                                  std::string& error);
