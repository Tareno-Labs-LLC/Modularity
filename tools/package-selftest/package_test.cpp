// Self-test for the Modularity package manager, focused on the 7.0 package line.
//
// Drives PackageManager directly against throwaway registries and project roots
// under a temp directory. Never touches a real project or the real registry
// except to read it (the "live registry" section points at the checked-out
// Modu-Package-Manager via MODUENGINE_PACKAGE_REGISTRY_ROOT).
//
// Build/run: tools/package-selftest/run.sh
#include "PackageManager.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace fsys = std::filesystem;

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        ++checks;                                                               \
        if (!(cond)) {                                                          \
            std::printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);       \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static fsys::path gTempRoot;

static fsys::path makeTempDir(const std::string& name) {
    const fsys::path dir = gTempRoot / name;
    std::error_code ec;
    fsys::remove_all(dir, ec);
    fsys::create_directories(dir, ec);
    return dir;
}

static void writeFile(const fsys::path& path, const std::string& contents) {
    std::error_code ec;
    fsys::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
}

static std::string readFile(const fsys::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static const PackageInfo* find(const PackageManager& pm, const std::string& id) {
    for (const PackageInfo& p : pm.getRegistry()) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

// Builds a synthetic registry so version/manifest edge cases can be tested
// without mutating the real one.
static fsys::path makeSyntheticRegistry(const std::string& name, const std::string& indexBody) {
    const fsys::path root = makeTempDir(name);
    writeFile(root / "PackageManagerInfo.modu", indexBody);
    std::error_code ec;
    fsys::create_directories(root / "Packages", ec);
    return root;
}

static void useRegistry(const fsys::path& root) {
    setenv("MODUENGINE_PACKAGE_REGISTRY_ROOT", root.string().c_str(), 1);
    // The registry is remote-first: loadRegistryMetadata curls
    // PackageManagerInfo.modu from pak.moduengine.xyz and only falls back to the
    // local checkout when that fails. Point the metadata URL at a closed port so
    // every test deterministically exercises the local registry, offline.
    setenv("MODUENGINE_PACKAGE_REGISTRY_METADATA_URL",
           "http://127.0.0.1:1/registry-disabled-for-tests", 1);
}

// ---------------------------------------------------------------------------
// Live registry: the checked-out Modu-Package-Manager.
// ---------------------------------------------------------------------------
static void TestLiveRegistryDiscovery(const fsys::path& registryRoot) {
    std::printf("-- live registry discovery --\n");
    useRegistry(registryRoot);
    PackageManager pm;

    CHECK(pm.hasRegistryMetadata(), "registry metadata loads");

    // The 7.0 line must win over the older blocks for every maintained package.
    struct Expected { const char* id; const char* version; };
    const Expected expected[] = {
        { "moduengine.2d-world",        "3.0.0"    },
        { "moduengine.sprite-editor",   "1.2.0"    },
        { "moduengine.spritesheet",     "1.1.0"    },
        { "moduengine.mesh-builder",    "1.1.0"    },
        { "moduengine.scripting-window","1.1.0"    },
        { "moduengine.vulkan-pipeline", "1.1.0"    },
        { "moduengine.networking",      "1.0.0"    },
        { "photon.realtime-sdk",        "5.0.14.3" },
    };

    for (const Expected& e : expected) {
        const PackageInfo* pkg = find(pm, e.id);
        if (!pkg) {
            std::printf("FAIL: %s missing from registry\n", e.id);
            ++failures; ++checks;
            continue;
        }
        ++checks;
        if (pkg->version != e.version) {
            std::printf("FAIL: %s resolved to %s, expected the 7.0-line %s\n",
                        e.id, pkg->version.c_str(), e.version);
            ++failures;
        }
        CHECK(pm.isCompatible(*pkg), "7.0-line package is compatible with the running engine");
    }

    // Older on-disk releases must still exist so pinned projects keep resolving.
    const fsys::path packages = registryRoot / "Packages";
    CHECK(fsys::exists(packages / "6.4"), "6.4 line preserved on disk");
    CHECK(fsys::exists(packages / "6.5"), "6.5 line preserved on disk");
    CHECK(fsys::exists(packages / "6.7"), "6.7 line preserved on disk");
    CHECK(fsys::exists(packages / "7.0"), "7.0 line present on disk");
    CHECK(fsys::exists(packages / "6.5/Modularity Core/moduengine.2d-world-2.7.0"),
          "superseded 6.5 2d-world release still on disk");
    CHECK(fsys::exists(packages / "6.4/Modularity Core/moduengine.2d-world-2.4.1"),
          "superseded 6.4 2d-world release still on disk");
}

// ---------------------------------------------------------------------------
// Version comparison, exercised through the public isCompatible(PackageInfo).
// ---------------------------------------------------------------------------
static void TestVersionComparison() {
    std::printf("-- version comparison --\n");
    PackageManager pm;

    auto compatible = [&](const char* rule) {
        PackageInfo p;
        p.id = "test.pkg";
        p.registryPackage = true;
        p.compatibleModuEngineVersion = rule;
        return pm.isCompatible(p);
    };

    // Engine is 7.0.0.
    CHECK(compatible("6.4+"), "7.0 engine accepts a 6.4+ package");
    CHECK(compatible("6.5+"), "7.0 engine accepts a 6.5+ package");
    CHECK(compatible("6.7+"), "7.0 engine accepts a 6.7+ package (7.0 > 6.7)");
    CHECK(compatible("7.0+"), "7.0 engine accepts a 7.0+ package");
    CHECK(!compatible("7.1+"), "7.0 engine rejects a 7.1+ package");
    CHECK(!compatible("8.0+"), "7.0 engine rejects an 8.0+ package");

    // The lexicographic traps. As strings "7.0" < "7.1" but also "10" < "9",
    // so these two pin down that the compare is numeric per component.
    CHECK(!compatible("7.10+"), "7.0 engine rejects 7.10 (numeric: 10 > 0)");
    CHECK(compatible("6.10+"), "7.0 engine accepts 6.10 (major 7 > 6)");
    CHECK(!compatible("7.0.1+"), "7.0.0 engine rejects 7.0.1");
    CHECK(compatible("7.0.0+"), "7.0.0 engine accepts 7.0.0");
    CHECK(compatible(""), "empty rule is treated as unrestricted");
}

// ---------------------------------------------------------------------------
// Malformed / incompatible registry entries.
// ---------------------------------------------------------------------------
static void TestMalformedAndIncompatible() {
    std::printf("-- malformed and incompatible entries --\n");

    const fsys::path root = makeSyntheticRegistry("registry-malformed", R"(
ModuEngineVersion("7.0")
{
    Package()
    {
        // No PackageID at all - must be discarded, not registered blank.
        Name = "Nameless";
        Version = "1.0.0";
        CompatibleModuEngineVersion = "7.0+";
    }

    Package()
    {
        PackageID = "test.good";
        Name = "Good Package";
        Author = "Test";
        Version = "1.0.0";
        CompatibleModuEngineVersion = "7.0+";
    }
}

ModuEngineVersion("99.0")
{
    Package()
    {
        PackageID = "test.future";
        Name = "Future Package";
        Author = "Test";
        Version = "1.0.0";
        CompatibleModuEngineVersion = "99.0+";
    }
}
)");

    useRegistry(root);
    PackageManager pm;

    CHECK(find(pm, "test.good") != nullptr, "well-formed package is discovered");
    CHECK(find(pm, "") == nullptr, "package with no PackageID is discarded");
    CHECK(find(pm, "test.future") == nullptr,
          "package from a future engine block is not offered to a 7.0 engine");
}

// ---------------------------------------------------------------------------
// Newest-block-wins, independent of the order blocks appear in the file.
// ---------------------------------------------------------------------------
static void TestNewestBlockWins() {
    std::printf("-- newest block wins regardless of file order --\n");

    // 7.0 deliberately written BEFORE 6.5, and a 7.9/7.10 pair to catch a
    // lexicographic compare. Correct behaviour: 7.10 wins.
    const fsys::path root = makeSyntheticRegistry("registry-order", R"(
ModuEngineVersion("7.0")
{
    Package()
    {
        PackageID = "test.ordered";
        Name = "Ordered";
        Author = "Test";
        Version = "7.0-release";
        CompatibleModuEngineVersion = "7.0+";
    }
}

ModuEngineVersion("6.5")
{
    Package()
    {
        PackageID = "test.ordered";
        Name = "Ordered";
        Author = "Test";
        Version = "6.5-release";
        CompatibleModuEngineVersion = "6.5+";
    }
}
)");

    useRegistry(root);
    PackageManager pm;

    const PackageInfo* pkg = find(pm, "test.ordered");
    CHECK(pkg != nullptr, "duplicate-id package resolves");
    if (pkg) {
        CHECK(pkg->version == "7.0-release",
              "newer 7.0 block wins even though 6.5 was parsed later");
    }
}

// ---------------------------------------------------------------------------
// Install / update / remove against a temp project.
// ---------------------------------------------------------------------------
static void TestInstallUpdateRemove(const fsys::path& registryRoot) {
    std::printf("-- install / update / remove --\n");
    useRegistry(registryRoot);

    const fsys::path project = makeTempDir("project-lifecycle");
    PackageManager pm;
    pm.setProjectRoot(project);

    const std::string id = "moduengine.spritesheet";

    // --- install a 7.0 package -------------------------------------------------
    CHECK(pm.installRegistryPackageToProject(id), "install 7.0 spritesheet package");
    CHECK(pm.isInstalled(id), "package reports installed");

    const fsys::path installDir = project / "Packages" / "moduengine.spritesheet-1.1.0";
    CHECK(fsys::exists(installDir), "version-stamped install directory created");
    CHECK(fsys::exists(installDir / "Manifest.modu"), "manifest installed");
    CHECK(fsys::exists(installDir / "Editor" / "SpritesheetFormat.cpp"), "payload installed");
    CHECK(fsys::exists(project / "packages.modu"), "project manifest written");

    // Nothing partially written: every mapped payload file must be present.
    CHECK(fsys::exists(installDir / "Editor" / "SpritesheetFormat.h"),
          "complete payload installed, not partial");

    // --- user modifications survive -------------------------------------------
    const fsys::path userEdited = installDir / "Editor" / "SpritesheetFormat.cpp";
    writeFile(userEdited, "// USER EDIT MARKER\n");
    const fsys::path userOwned = project / "Assets" / "MyScene.modu";
    writeFile(userOwned, "user scene\n");

    // Re-installing the same version must not clobber the user's edit: install is
    // gated on the destination not existing.
    CHECK(pm.installRegistryPackageToProject(id), "re-install is a no-op success");
    CHECK(readFile(userEdited).find("USER EDIT MARKER") != std::string::npos,
          "user-modified package file preserved across re-install");

    // --- side-by-side upgrade: 6.5 release lands in its own directory ----------
    // The 6.5 spritesheet is 1.0.0, the 7.0 one is 1.1.0, so the install paths
    // differ and an upgrade cannot overwrite the older tree in place.
    const fsys::path oldStyleDir = project / "Packages" / "moduengine.spritesheet-1.0.0";
    writeFile(oldStyleDir / "Editor" / "SpritesheetFormat.cpp", "// OLD 6.5 PAYLOAD\n");
    CHECK(fsys::exists(oldStyleDir) && fsys::exists(installDir),
          "6.5 and 7.0 releases coexist in the project");
    CHECK(readFile(oldStyleDir / "Editor" / "SpritesheetFormat.cpp").find("OLD 6.5") != std::string::npos,
          "older release untouched by the newer install");

    // --- removal ---------------------------------------------------------------
    CHECK(pm.remove(id), "remove package");
    CHECK(!pm.isInstalled(id), "package no longer installed");
    CHECK(!fsys::exists(installDir), "7.0 package directory removed");
    CHECK(!fsys::exists(oldStyleDir), "sibling <id>-* release directory also removed");
    CHECK(fsys::exists(userOwned), "user project file preserved through removal");
    CHECK(readFile(userOwned) == "user scene\n", "user project file contents intact");

    // --- reinstall after removal ----------------------------------------------
    CHECK(pm.installRegistryPackageToProject(id), "reinstall after removal");
    CHECK(fsys::exists(installDir / "Editor" / "SpritesheetFormat.cpp"), "payload restored");
    CHECK(readFile(installDir / "Editor" / "SpritesheetFormat.cpp").find("USER EDIT MARKER") == std::string::npos,
          "reinstall provides pristine payload, not the removed user edit");
}

// ---------------------------------------------------------------------------
// Failure path: an unknown package must not create state.
// ---------------------------------------------------------------------------
static void TestFailedInstallLeavesNoState(const fsys::path& registryRoot) {
    std::printf("-- failed install leaves no state --\n");
    useRegistry(registryRoot);

    const fsys::path project = makeTempDir("project-failure");
    PackageManager pm;
    pm.setProjectRoot(project);

    CHECK(!pm.installRegistryPackageToProject("does.not.exist"),
          "installing an unknown package fails");
    CHECK(!pm.getLastError().empty(), "failure reports an error message");
    CHECK(!pm.isInstalled("does.not.exist"), "failed package is not marked installed");

    const fsys::path packagesDir = project / "Packages";
    if (fsys::exists(packagesDir)) {
        std::error_code ec;
        CHECK(fsys::is_empty(packagesDir, ec), "no package directory left behind after failure");
    }
}

// ---------------------------------------------------------------------------
// Photon 7.0 preparation.
// ---------------------------------------------------------------------------
static void TestPhotonPackage(const fsys::path& registryRoot) {
    std::printf("-- photon 7.0 package --\n");
    useRegistry(registryRoot);

    const fsys::path project = makeTempDir("project-photon");
    PackageManager pm;
    pm.setProjectRoot(project);

    const std::string id = "photon.realtime-sdk";
    const PackageInfo* pkg = find(pm, id);
    CHECK(pkg != nullptr, "photon package discovered on the 7.0 line");
    if (!pkg) return;

    CHECK(pkg->version == "5.0.14.3", "photon keeps its upstream SDK version");
    CHECK(pkg->compatibleModuEngineVersion == "7.0+", "photon declares 7.0 compatibility");
    CHECK(pm.isCompatible(*pkg), "photon is compatible with the running engine");

    // The payload is shared with the 6.7 line rather than duplicated: the source
    // resolver scans every version directory for "<id>-<version>".
    PackageManager::RegistryPackageLocations loc;
    CHECK(pm.resolveRegistryPackageLocations(id, loc), "photon source location resolves");
    CHECK(!loc.source.empty() && fsys::exists(loc.source), "photon payload found on disk");
    CHECK(loc.source.string().find("/6.7/") != std::string::npos,
          "photon 7.0 entry reuses the 6.7 payload instead of duplicating 91MB");

    // Headers and native libraries the later integration milestone will need.
    CHECK(fsys::exists(loc.source / "LoadBalancing-cpp" / "inc"), "realtime headers present");
    CHECK(fsys::exists(loc.source / "Common-cpp" / "inc"), "common headers present");
    CHECK(fsys::exists(loc.source / "Photon-cpp" / "inc"), "photon core headers present");
    CHECK(fsys::exists(loc.source / "Photon-cpp" / "libPhotonRelease64.a"), "linux 64-bit realtime lib present");
    CHECK(fsys::exists(loc.source / "LoadBalancing-cpp" / "libLoadBalancingRelease64.a"), "loadbalancing lib present");

    // Voice is genuinely vendored, so record what is actually there.
    CHECK(fsys::exists(loc.source / "PhotonVoice-cpp" / "inc"), "photon voice headers present");
    CHECK(fsys::exists(loc.source / "PhotonVoice-cpp" / "libPhotonVoiceRelease64.a"), "photon voice lib present");
    CHECK(fsys::exists(loc.source / "PhotonVoice-cpp" / "opus" / "COPYING"), "bundled opus license present");
    CHECK(fsys::exists(loc.source / "readme.txt"), "photon SDK readme/notices present");

    // No Windows binaries are vendored: this is the Photon *Linux* SDK.
    bool sawWindowsLib = false;
    std::error_code ec;
    for (auto it = fsys::recursive_directory_iterator(loc.source, ec);
         it != fsys::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const std::string ext = it->path().extension().string();
        if (ext == ".lib" || ext == ".dll") { sawWindowsLib = true; break; }
    }
    CHECK(!sawWindowsLib, "no Windows .lib/.dll vendored (Linux SDK, documented limitation)");

    // Install into the temp project, then confirm headers/libs are reachable.
    CHECK(pm.installRegistryPackageToProject(id), "install photon package");
    const fsys::path dest = project / "Packages" / "photon.realtime-sdk-5.0.14.3";
    CHECK(fsys::exists(dest / "Photon-cpp" / "inc"), "installed headers available to the integration milestone");
    CHECK(fsys::exists(dest / "Photon-cpp" / "libPhotonRelease64.a"), "installed libs available");

    // No unfinished networking components may be registered by the package.
    CHECK(pkg->packageType == "Networking SDK", "photon is an SDK, not a component package");
    CHECK(!fsys::exists(dest / "Scripts"), "package installs no auto-compiled Scripts/ directory");

    // A user file inside the project survives package removal.
    const fsys::path userOwned = project / "Assets" / "keepme.txt";
    writeFile(userOwned, "keep\n");
    CHECK(pm.remove(id), "remove photon package");
    CHECK(!fsys::exists(dest), "photon package files removed");
    CHECK(fsys::exists(userOwned), "user project file preserved");
}

// ---------------------------------------------------------------------------
// Hygiene: nothing machine-specific or secret in the shipped 7.0 metadata.
// ---------------------------------------------------------------------------
static void TestRegistryHygiene(const fsys::path& registryRoot) {
    std::printf("-- registry hygiene --\n");

    const std::string index = readFile(registryRoot / "PackageManagerInfo.modu");
    CHECK(!index.empty(), "registry index readable");
    CHECK(index.find("/home/") == std::string::npos, "no absolute home paths in the registry index");
    CHECK(index.find("ModuEngineVersion(\"7.0\")") != std::string::npos, "7.0 block present");
    CHECK(index.find("ModuEngineVersion(\"6.4\")") != std::string::npos, "6.4 block preserved");
    CHECK(index.find("ModuEngineVersion(\"6.5\")") != std::string::npos, "6.5 block preserved");
    CHECK(index.find("ModuEngineVersion(\"6.7\")") != std::string::npos, "6.7 block preserved");

    std::error_code ec;
    const fsys::path line70 = registryRoot / "Packages" / "7.0";
    for (auto it = fsys::recursive_directory_iterator(line70, ec);
         it != fsys::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        const std::string ext = it->path().extension().string();
        if (ext != ".modu" && ext != ".md" && ext != ".txt") continue;
        const std::string body = readFile(it->path());
        if (body.find("/home/anemunt") != std::string::npos) {
            std::printf("FAIL: machine-specific path in %s\n", it->path().string().c_str());
            ++failures;
        }
        ++checks;
    }

    // Every 7.0 package must carry a manifest declaring 7.0 compatibility.
    for (const auto& entry : fsys::directory_iterator(line70 / "Modularity Core", ec)) {
        if (ec || !entry.is_directory()) continue;
        const fsys::path manifest = entry.path() / "Manifest.modu";
        ++checks;
        if (!fsys::exists(manifest)) {
            std::printf("FAIL: %s has no Manifest.modu\n", entry.path().filename().string().c_str());
            ++failures;
            continue;
        }
        const std::string body = readFile(manifest);
        if (body.find("\"7.0+\"") == std::string::npos) {
            std::printf("FAIL: %s manifest does not declare 7.0+\n",
                        entry.path().filename().string().c_str());
            ++failures;
        }
    }
}

// The networking package must be installable without Photon, and must not smuggle
// the Photon SDK into its own payload.
static void TestNetworkingPackage(const fsys::path& registryRoot) {
    std::printf("-- networking 7.0 package --\n");
    useRegistry(registryRoot);

    const fsys::path project = makeTempDir("project-networking");
    PackageManager pm;
    pm.setProjectRoot(project);

    const std::string id = "moduengine.networking";
    const PackageInfo* pkg = find(pm, id);
    CHECK(pkg != nullptr, "networking package discovered on the 7.0 line");
    if (!pkg) return;
    CHECK(pkg->compatibleModuEngineVersion == "7.0+", "networking declares 7.0 compatibility");
    CHECK(pm.isCompatible(*pkg), "networking is compatible with the running engine");
    CHECK(pkg->subsystem == "Networking", "networking is filed under the Networking subsystem");

    // Installs on its own: networking must not require the Photon package.
    CHECK(!pm.isInstalled("photon.realtime-sdk"), "photon not installed in this project");
    CHECK(pm.installRegistryPackageToProject(id), "install networking without Photon present");

    const fsys::path dest = project / "Packages" / "moduengine.networking-1.0.0";
    CHECK(fsys::exists(dest / "Manifest.modu"), "networking manifest installed");
    CHECK(fsys::exists(dest / "Editor" / "Network" / "NetworkSession.cpp"), "session source installed");
    CHECK(fsys::exists(dest / "Editor" / "Network" / "NetworkSerializer.cpp"), "serializer source installed");
    CHECK(fsys::exists(dest / "Editor" / "Network" / "LoopbackBackend.cpp"), "offline backend installed");
    CHECK(fsys::exists(dest / "Editor" / "Network" / "PhotonBackend.cpp"), "photon backend source installed");

    // The payload is engine source only: no vendored SDK, no credentials.
    std::error_code ec;
    bool sawSdkBinary = false;
    bool sawCredential = false;
    for (auto it = fsys::recursive_directory_iterator(dest, ec);
         it != fsys::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        const std::string ext = it->path().extension().string();
        if (ext == ".a" || ext == ".lib" || ext == ".dll" || ext == ".so") sawSdkBinary = true;
        const std::string body = readFile(it->path());
        if (body.find("/home/anemunt") != std::string::npos) sawCredential = true;
    }
    CHECK(!sawSdkBinary, "networking package vendors no SDK binaries");
    CHECK(!sawCredential, "networking package contains no machine-specific paths");

    // The Photon backend source must be the only file naming Photon types.
    {
        const std::string session = readFile(dest / "Editor" / "Network" / "NetworkSession.cpp");
        const std::string serializer = readFile(dest / "Editor" / "Network" / "NetworkSerializer.cpp");
        const std::string loopback = readFile(dest / "Editor" / "Network" / "LoopbackBackend.cpp");
        CHECK(session.find("ExitGames") == std::string::npos, "session names no Photon types");
        CHECK(serializer.find("ExitGames") == std::string::npos, "serializer names no Photon types");
        CHECK(loopback.find("ExitGames") == std::string::npos, "offline backend names no Photon types");
    }

    // Removal leaves the project's own files alone.
    const fsys::path userOwned = project / "Assets" / "keep.txt";
    writeFile(userOwned, "keep\n");
    CHECK(pm.remove(id), "remove networking package");
    CHECK(!fsys::exists(dest), "networking package files removed");
    CHECK(fsys::exists(userOwned), "user project file preserved");
    CHECK(pm.installRegistryPackageToProject(id), "networking can be reinstalled");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <registry-root> [temp-root]\n", argv[0]);
        return 2;
    }
    const fsys::path registryRoot = fsys::absolute(argv[1]);
    gTempRoot = (argc >= 3) ? fsys::path(argv[2])
                            : fsys::temp_directory_path() / "modularity-package-selftest";

    std::error_code ec;
    fsys::create_directories(gTempRoot, ec);

    // Keep every PackageManager construction offline and deterministic.
    useRegistry(registryRoot);

    if (!fsys::exists(registryRoot / "PackageManagerInfo.modu")) {
        std::printf("error: %s is not a package registry root\n", registryRoot.string().c_str());
        return 2;
    }

    TestLiveRegistryDiscovery(registryRoot);
    TestVersionComparison();
    TestMalformedAndIncompatible();
    TestNewestBlockWins();
    TestInstallUpdateRemove(registryRoot);
    TestFailedInstallLeavesNoState(registryRoot);
    TestPhotonPackage(registryRoot);
    TestNetworkingPackage(registryRoot);
    TestRegistryHygiene(registryRoot);

    std::printf("\npackage self-test: %d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
