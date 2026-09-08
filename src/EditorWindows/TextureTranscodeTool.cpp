#include "Engine.h"
#include "../include/Textures/TextureTranscode.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <system_error>

namespace {

const char* const kBlenderSearchRoots[] = {
#ifdef _WIN32
    "C:/Program Files/Blender Foundation",
    "C:/Program Files (x86)/Blender Foundation",
#elif defined(__APPLE__)
    "/Applications",
#else
    "/usr/share",
    "/opt",
#endif
};

#ifdef _WIN32
constexpr const char* kBlenderExecutableName = "blender.exe";
#else
constexpr const char* kBlenderExecutableName = "blender";
#endif

bool LooksLikeExr(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".exr";
}

// contain spaces ("Program Files", "vr thingy"), and an unquoted one silently
std::string Quote(const std::string& value) {
    return "\"" + value + "\"";
}

} // namespace

#pragma region EXR Transcode
std::string Engine::findBlenderExecutable() const {
    std::error_code ec;

    if (!blenderExecutableOverride.empty()) {
        if (fs::exists(blenderExecutableOverride, ec) && !ec) {
            return blenderExecutableOverride;
        }
    }

    for (const char* root : kBlenderSearchRoots) {
        if (!fs::exists(root, ec) || ec) continue;
        std::string best;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!entry.is_directory(ec)) continue;
            const fs::path direct = entry.path() / kBlenderExecutableName;
            const fs::path bundle =
                entry.path() / "Contents" / "MacOS" / kBlenderExecutableName;
            if (fs::exists(direct, ec) && !ec) best = direct.string();
            else if (fs::exists(bundle, ec) && !ec) best = bundle.string();
        }
        if (!best.empty()) return best;
    }

    std::string probe;
    if (runToolCommand(std::string(kBlenderExecutableName) + " --version", probe) &&
        probe.find("Blender") != std::string::npos) {
        return kBlenderExecutableName;
    }
    return std::string();
}

bool Engine::transcodeExrViaBlender(const fs::path& exrPath, std::string& outError) {
    outError.clear();

    const std::string blender = findBlenderExecutable();
    if (blender.empty()) {
        outError =
            "Blender was not found. Install it, or set its path in Project Settings, "
            "to read EXRs saved with DWAA/DWAB compression.";
        return false;
    }

    const fs::path script =
        fs::path("Resources") / "Engine-Root" / "Tools" / "blender_exr_transcode.py";
    std::error_code ec;
    if (!fs::exists(script, ec) || ec) {
        outError = "Missing " + script.string();
        return false;
    }

    const std::string outPath = Modularity::TranscodedTexturePath(exrPath.string());

    std::string command = Quote(blender) +
                          " --background --factory-startup --python " + Quote(script.string()) +
                          " -- " + Quote(exrPath.string()) + " " + Quote(outPath) + " 2>&1";

    std::string output;
    runToolCommand(command, output);

    if (output.find("TRANSCODE_OK") == std::string::npos) {
        const size_t at = output.find("TRANSCODE_FAIL");
        outError = (at != std::string::npos)
                       ? output.substr(at, output.find('\n', at) - at)
                       : "Blender did not report success";
        return false;
    }
    if (!fs::exists(outPath, ec) || ec) {
        outError = "Blender reported success but wrote no file";
        return false;
    }

    renderer.invalidateTexture(exrPath.string());
    return true;
}

int Engine::transcodeProjectExrTextures() {
    if (!projectManager.currentProject.isLoaded) {
        addConsoleMessage("Open a project before transcoding textures.",
                          ConsoleMessageType::Warning);
        return 0;
    }

    const fs::path root = projectManager.currentProject.projectPath.parent_path();
    std::error_code ec;
    if (!fs::exists(root, ec) || ec) {
        addConsoleMessage("Project folder not found: " + root.string(),
                          ConsoleMessageType::Error);
        return 0;
    }

    std::vector<fs::path> pending;
    int exrSeen = 0;
    int alreadyFine = 0;
    for (const auto& entry : fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) continue;
        const fs::path& path = entry.path();
        if (!LooksLikeExr(path)) continue;
        ++exrSeen;
        if (Modularity::ResolveLoadableTexturePath(path.string()) != path.string()) {
            ++alreadyFine;
            continue;
        }
        if (renderer.getTexture(path.string()) != nullptr) {
            ++alreadyFine;
            continue;
        }
        pending.push_back(path);
    }

    if (pending.empty()) {
        if (exrSeen == 0) {
            addConsoleMessage("No EXR textures found under " + root.string(),
                              ConsoleMessageType::Info);
        } else {
            addConsoleMessage(
                "All " + std::to_string(exrSeen) + " EXR texture(s) already load fine.",
                ConsoleMessageType::Info);
        }
        return 0;
    }

    const std::string blenderPath = findBlenderExecutable();
    if (blenderPath.empty()) {
        addConsoleMessage(
            std::to_string(pending.size()) +
                " EXR texture(s) need transcoding, but Blender was not found. Install "
                "Blender, or set its path in the editor settings.",
            ConsoleMessageType::Error);
        return 0;
    }
    addConsoleMessage("Transcoding " + std::to_string(pending.size()) + " of " +
                          std::to_string(exrSeen) + " EXR texture(s) using " + blenderPath +
                          ". This may take a while.",
                      ConsoleMessageType::Info);

    int converted = 0;
    int failed = 0;
    std::string firstError;
    for (const fs::path& path : pending) {
        std::string error;
        if (transcodeExrViaBlender(path, error)) {
            ++converted;
        } else {
            ++failed;
            if (firstError.empty()) firstError = error;
            if (error.find("Blender was not found") != std::string::npos) break;
        }
    }

    if (converted > 0) {
        addConsoleMessage("Transcoded " + std::to_string(converted) + " EXR texture(s).",
                          ConsoleMessageType::Success);
    }
    if (failed > 0) {
        addConsoleMessage(std::to_string(failed) + " failed: " + firstError,
                          ConsoleMessageType::Error);
    }
    return converted;
}
#pragma endregion
