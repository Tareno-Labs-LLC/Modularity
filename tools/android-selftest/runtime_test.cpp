#include "RuntimeContent.h"
#include "Platform/AssetSource.h"
#include <cstdio>
#include <fstream>

namespace platform = Modularity::Platform;
struct AAssetManager;
namespace Modularity::Platform {
std::unique_ptr<AssetSource> MakeAndroidAssetSource(AAssetManager*);
}
class PackagedSource final : public platform::AssetSource {
    std::unique_ptr<platform::AssetSource> disk;
public:
    explicit PackagedSource(const fs::path& root) : disk(platform::MakeDesktopAssetSource(root.string())) {}
    bool Exists(const std::string&) const override { return true; }
    std::vector<uint8_t> ReadAll(const std::string&) const override { return disk->ReadAll("bundle.modbundle"); }
    std::unique_ptr<platform::AssetStream> Open(const std::string&) const override { return disk->Open("bundle.modbundle"); }
};
static std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const fs::path root = fs::absolute(argv[1]);
    fs::create_directories(root);
    const fs::path project = root / "project.modu", bundle = root / "bundle.modbundle", cache = root / "cache";
    std::string error;
    auto writeBundle = [&](const char* text) {
        std::ofstream(project, std::ios::binary | std::ios::trunc) << text;
        return WriteRuntimeContentBundle(bundle, {{project, "project.modu"}}, error);
    };
    if (!writeBundle("first")) return 1;
    const auto originalSize = fs::file_size(bundle);
    platform::SetAssetSource(std::make_unique<PackagedSource>(root));
    const fs::path logical = root / "apk-only.modbundle"; // no filesystem timestamp, like AAssetManager
    if (!ExtractRuntimeContentBundle(logical, cache, error) || read(cache / "project.modu") != "first") return 1;
    std::ofstream(cache / "keep-on-cache-hit") << "keep";
    if (!ExtractRuntimeContentBundle(logical, cache, error) || !fs::exists(cache / "keep-on-cache-hit")) return 1;
    if (!writeBundle("other") || fs::file_size(bundle) != originalSize ||
        !ExtractRuntimeContentBundle(logical, cache, error) || read(cache / "project.modu") != "other" ||
        fs::exists(cache / "keep-on-cache-hit")) {
        std::fprintf(stderr, "Packaged bundle update reused stale content: %s\n", error.c_str()); return 1;
    }
    std::puts("[PASS] unchanged APK bundles reuse extraction; same-size updates replace stale content");

    const fs::path clip = root / "Assets/Audio/voice.wav.ogg";
    fs::create_directories(clip.parent_path());
    std::ofstream(clip, std::ios::binary) << "voice";
    platform::SetPortableAssetRoot(root.string());
    auto android = platform::MakeAndroidAssetSource(nullptr);
    const std::string foreign = "Z:\\Old Computer\\Tenna\\Assets\\Audio\\voice.wav.ogg";
    const auto bytes = android->ReadAll(foreign);
    auto stream = android->Open(foreign);
    char data[5]{};
    if (!android->Exists(foreign) || std::string(bytes.begin(), bytes.end()) != "voice" ||
        !stream || stream->Read(data, sizeof(data)) != 5 || std::string(data, 5) != "voice" ||
        android->Exists("Z:\\Old Computer\\Tenna\\Assets\\missing.png")) return 1;
    std::puts("[PASS] Android Exists/ReadAll/Open resolve Windows-authored paths into extracted project assets");
    return 0;
}
