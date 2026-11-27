#pragma once

#include <filesystem>
#include <optional>

namespace Modularity {

bool OpenMacPath(const std::filesystem::path& path);
void RevealMacPath(const std::filesystem::path& path);
std::optional<std::filesystem::path> ChooseMacPath(
    const std::filesystem::path& initialDirectory, bool chooseDirectory);

} // namespace Modularity
