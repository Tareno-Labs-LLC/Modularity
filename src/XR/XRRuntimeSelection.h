#pragma once

#include <string>
#include <vector>

namespace Modularity::XR {

struct XRInstalledRuntime {
    std::string displayName;
    std::string manifestPath;
    std::string libraryPath;
    bool isSystemDefault = false;
    bool manifestExists = false;
};

std::vector<XRInstalledRuntime> EnumerateInstalledRuntimes();

std::string SystemDefaultRuntimeManifestPath();

std::string DescribeRuntimeManifest(const std::string& manifestPath);

bool ApplyRuntimeOverride(const std::string& manifestPath, std::string& outError);

const std::string& ActiveRuntimeOverride();

void NoteRuntimeLoaded();
bool HasLoadedRuntimeThisSession();

}
