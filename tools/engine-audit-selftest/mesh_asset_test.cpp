#include "Render25D/MMeshLoader.h"

#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>

using namespace Modularity::Render25D;
static int failures = 0;
static void check(bool pass, const char* name) {
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
    if (!pass) ++failures;
}

static MMeshAsset triangle() {
    MMeshAsset asset;
    asset.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    asset.indices = {0, 1, 2, 0, 2, 1};
    asset.materials.emplace_back();
    asset.surfaces.push_back({0, 6, 0});
    return asset;
}

static void writeBytes(const fs::path& path, const std::vector<char>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

static void putUInt(std::vector<char>& bytes, size_t offset, uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string error;
    MMeshAsset asset = triangle();
    check(ValidateMMeshAsset(asset, error), "valid triangle mesh is accepted");
    asset.surfaces[0] = {3, std::numeric_limits<uint32_t>::max(), 0};
    check(!ValidateMMeshAsset(asset, error), "wrapping surface range is rejected before rendering");
    asset.surfaces[0] = {6, 0, 0};
    check(ValidateMMeshAsset(asset, error), "empty surface at buffer end remains valid");
    asset.surfaces[0] = {6, 3, 0};
    check(!ValidateMMeshAsset(asset, error), "non-wrapping out-of-bounds surface is rejected");
    if (argc > 1 && std::string(argv[1]) == "--range-only") return failures ? 1 : 0;

    const fs::path folder = argc > 1 ? argv[1] : "build/Dev-Resources/engine-audit-check/fixtures";
    fs::create_directories(folder);
    const auto path = folder / "mesh.mmesh";
    MMeshLoader loader;
    asset = triangle();
    check(loader.saveAsset(asset, path.string(), error), "valid mesh saves");
    MMeshAsset loaded;
    check(loader.loadAsset(path.string(), loaded, error) && loaded.indices == asset.indices,
          "version 2 mesh round-trips");
    std::ifstream input(path, std::ios::binary);
    const std::vector<char> good{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    for (size_t offset : {12u, 16u, 20u, 24u, 28u}) {
        auto bad = good;
        putUInt(bad, offset, std::numeric_limits<uint32_t>::max());
        writeBytes(path, bad);
        error.clear();
        check(!loader.loadAsset(path.string(), loaded, error) &&
              error.find("counts exceed") != std::string::npos,
              "impossible file count is rejected before allocation");
    }
    // First material begins after header, bounds, vertices and indices. The
    // saved fixture has no UV or normal data, so this is its first name length.
    const size_t nameOffset = 32 + 2 * sizeof(glm::vec3) +
        asset.vertices.size() * sizeof(glm::vec3) + asset.indices.size() * sizeof(uint32_t);
    auto badString = good;
    putUInt(badString, nameOffset, std::numeric_limits<uint32_t>::max());
    writeBytes(path, badString);
    check(!loader.loadAsset(path.string(), loaded, error), "oversized material string is rejected");
    auto truncated = good;
    truncated.resize(36);
    writeBytes(path, truncated);
    check(!loader.loadAsset(path.string(), loaded, error), "truncated mesh is rejected");
    check(loaded.indices == asset.indices && loaded.vertices == asset.vertices,
          "failed loads leave caller's last valid mesh intact");
    // Version 1 lacks material presentation fields; preserve its readability.
    auto legacy = good;
    putUInt(legacy, 4, 1);
    const size_t presentationOffset = nameOffset + 2 * sizeof(uint32_t) + std::string("Default").size();
    const size_t presentationBytes = sizeof(glm::vec4) + sizeof(uint32_t) + sizeof(bool) + sizeof(float);
    legacy.erase(legacy.begin() + presentationOffset, legacy.begin() + presentationOffset + presentationBytes);
    writeBytes(path, legacy);
    check(loader.loadAsset(path.string(), loaded, error), "version 1 mesh remains readable");
    asset.uvs = {{0, 0}, {1, 0}, {0, 1}};
    asset.normals.assign(3, glm::vec3(0, 0, 1));
    asset.metadataHooks.push_back({"test", "value"});
    check(loader.saveAsset(asset, path.string(), error) &&
          loader.loadAsset(path.string(), loaded, error) &&
          loaded.hasUVs && loaded.hasNormals && loaded.metadataHooks.size() == 1,
          "UV, normal and metadata sections remain readable");
    std::printf("Mesh audit: %d failures\n", failures);
    return failures ? 1 : 0;
}
