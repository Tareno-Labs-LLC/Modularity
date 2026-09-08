#pragma once

#include "Common.h"

struct RuntimeBundleEntry {
    fs::path sourcePath;
    fs::path archivePath;
};

bool WriteRuntimeContentBundle(const fs::path& bundlePath,
                               const std::vector<RuntimeBundleEntry>& entries,
                               std::string& error);

bool ExtractRuntimeContentBundle(const fs::path& bundlePath,
                                 const fs::path& outputRoot,
                                 std::string& error);
