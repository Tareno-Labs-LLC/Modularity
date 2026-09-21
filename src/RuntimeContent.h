#pragma once

#include "Common.h"

#include <cstdint>
#include <functional>

struct RuntimeBundleEntry {
    fs::path sourcePath;
    fs::path archivePath;
};

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
