#include "../../include/Textures/TextureTranscode.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace Modularity {

namespace {

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool HasSuffix(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

std::string TranscodedTexturePath(const std::string& exrPath) {
    return exrPath + ".png";
}

bool IsTranscodedTexturePath(const std::string& path) {
    return HasSuffix(ToLowerCopy(path), ".exr.png");
}

std::string ResolveLoadableTexturePath(const std::string& path) {
    if (path.empty()) return path;
    const std::string lower = ToLowerCopy(path);
    if (!HasSuffix(lower, ".exr")) return path;

    const std::string sidecar = TranscodedTexturePath(path);
    std::error_code ec;
    if (!fs::exists(sidecar, ec) || ec) return path;

    const auto sidecarTime = fs::last_write_time(sidecar, ec);
    if (ec) return path;
    std::error_code sourceEc;
    const auto sourceTime = fs::last_write_time(path, sourceEc);
    if (!sourceEc && sidecarTime < sourceTime) return path;

    return sidecar;
}

} // namespace Modularity
