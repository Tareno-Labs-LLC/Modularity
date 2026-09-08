// Self-test for the ModuOBJ core: format, authoring, instantiation, nesting.
//
// Exercises src/ModuObj.{h,cpp} directly against a temp directory. The core is
// Engine-free by design, so this runs headlessly with no window, GL context or
// project.
//
// Build/run: tools/moduobj-selftest/run.sh
#include "ModuObj.h"
#include "ProjectManager.h"  // SceneSerializer, SkyboxSettings

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <sstream>
#include <unordered_set>

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

static fsys::path gTemp;

// Builds a small scene: Root -> (ChildA -> Grandchild, ChildB[light]).
static std::vector<SceneObject> MakeScene() {
    std::vector<SceneObject> objs;
    objs.emplace_back("Root", ObjectType::Empty, 10);
    objs.emplace_back("ChildA", ObjectType::Empty, 11);
    objs.emplace_back("Grandchild", ObjectType::Cube, 12);
    objs.emplace_back("ChildB", ObjectType::PointLight, 13);

    objs[0].parentId = -1;
    objs[0].position = glm::vec3(1.0f, 2.0f, 3.0f);
    objs[0].childIds = { 11, 13 };

    objs[1].parentId = 10;
    objs[1].position = glm::vec3(0.0f, 1.0f, 0.0f);
    objs[1].childIds = { 12 };

    objs[2].parentId = 11;
    objs[2].position = glm::vec3(0.0f, 0.0f, 1.0f);
    // A real renderable, so the round trip is checked against a component-bearing
    // object. Without hasRenderer the serializer normalizes the object to Empty,
    // which is existing engine behaviour rather than a ModuOBJ round-trip fault.
    objs[2].hasRenderer = true;
    objs[2].renderType = RenderType::Cube;
    objs[2].material.color = glm::vec3(0.25f, 0.5f, 0.75f);

    objs[3].parentId = 10;
    objs[3].hasLight = true;
    objs[3].light.type = LightType::Point;

    for (SceneObject& o : objs) {
        o.scale = glm::vec3(1.0f);
        o.localScale = glm::vec3(1.0f);
    }
    return objs;
}

static ModuObj::Asset BuildFrom(const std::vector<SceneObject>& scene,
                                const std::vector<int>& selection,
                                const char* name) {
    ModuObj::Asset asset;
    std::vector<std::string> external;
    std::string error;
    if (!ModuObj::BuildAssetFromSelection(scene, selection, name, asset, external, error)) {
        std::printf("FAIL: BuildAssetFromSelection(%s): %s\n", name, error.c_str());
        ++failures;
    }
    ++checks;
    return asset;
}

// ---------------------------------------------------------------------------

static void TestAssetIdentity() {
    std::printf("-- asset identity --\n");
    const std::string a = ModuObj::GenerateAssetId();
    const std::string b = ModuObj::GenerateAssetId();
    CHECK(ModuObj::IsWellFormedAssetId(a), "generated id is well formed");
    CHECK(a != b, "generated ids are unique");
    CHECK(!ModuObj::IsWellFormedAssetId("mobj-xyz"), "short id rejected");
    CHECK(!ModuObj::IsWellFormedAssetId("nope-0123456789abcdef"), "wrong prefix rejected");
    CHECK(!ModuObj::IsWellFormedAssetId(""), "empty id rejected");
}

static void TestAuthoring() {
    std::printf("-- authoring from selection --\n");
    const std::vector<SceneObject> scene = MakeScene();

    // Single root captures its whole hierarchy.
    {
        ModuObj::Asset asset = BuildFrom(scene, { 10 }, "Whole");
        CHECK(asset.objects.size() == 4, "single root captures all descendants");
        CHECK(asset.roots.size() == 1, "one root recorded");
    }

    // Selecting a parent AND its descendant must not duplicate the descendant.
    {
        ModuObj::Asset asset = BuildFrom(scene, { 10, 12 }, "ParentAndChild");
        CHECK(asset.objects.size() == 4, "parent+descendant selection captures 4 objects, not 5");
        CHECK(asset.roots.size() == 1, "reduced to a single root");
        std::unordered_set<int> localIds;
        bool dup = false;
        for (const SceneObject& o : asset.objects) {
            if (!localIds.insert(o.id).second) dup = true;
        }
        CHECK(!dup, "no duplicate source-local ids from overlapping selection");
    }

    // Two unrelated roots -> multi-root asset.
    {
        ModuObj::Asset asset = BuildFrom(scene, { 11, 13 }, "TwoRoots");
        CHECK(asset.roots.size() == 2, "two unrelated selections produce two roots");
        CHECK(asset.objects.size() == 3, "ChildA+Grandchild+ChildB captured");
        for (const SceneObject& o : asset.objects) {
            if (o.parentId == -1) continue;
            bool parentPresent = false;
            for (const SceneObject& p : asset.objects) if (p.id == o.parentId) parentPresent = true;
            CHECK(parentPresent, "every non-root parent exists inside the asset");
        }
    }

    // A child outside the capture is reported, not silently dropped.
    {
        ModuObj::Asset asset;
        std::vector<std::string> external;
        std::string error;
        // Capture only Root: its children are captured too, so nothing external.
        ModuObj::BuildAssetFromSelection(scene, { 10 }, "X", asset, external, error);
        CHECK(external.empty(), "full hierarchy capture reports no external refs");

        // Capture only ChildA's grandchild's parent chain partially: select
        // Root but pretend ChildB is missing from the scene entirely.
        std::vector<SceneObject> partial = scene;
        partial.erase(partial.begin() + 3);  // remove ChildB, Root still lists it
        std::vector<std::string> external2;
        ModuObj::BuildAssetFromSelection(partial, { 10 }, "Y", asset, external2, error);
        CHECK(!external2.empty(), "dangling child reference is reported, not silently nulled");
    }

    // Empty selection fails cleanly.
    {
        ModuObj::Asset asset;
        std::vector<std::string> external;
        std::string error;
        CHECK(!ModuObj::BuildAssetFromSelection(scene, {}, "Empty", asset, external, error),
              "empty selection rejected");
        CHECK(!error.empty(), "empty selection reports an error");
    }
}

static void TestSerialization() {
    std::printf("-- serialization --\n");
    const std::vector<SceneObject> scene = MakeScene();

    // Round trip a hierarchy.
    ModuObj::Asset asset = BuildFrom(scene, { 10 }, "Lamp");
    const fsys::path path = gTemp / "Lamp.moduobj";
    std::string error;
    CHECK(ModuObj::SaveAsset(path, asset, error), "save hierarchy asset");
    CHECK(fsys::exists(path), "asset file written");
    CHECK(!fsys::exists(fsys::path(path.string() + ".tmp")), "no temporary file left behind");

    ModuObj::Asset loaded;
    CHECK(ModuObj::LoadAsset(path, loaded, error), "load hierarchy asset");
    CHECK(loaded.assetId == asset.assetId, "asset id survives round trip");
    CHECK(loaded.name == "Lamp", "name survives round trip");
    CHECK(loaded.objects.size() == asset.objects.size(), "object count survives");
    CHECK(loaded.roots == asset.roots, "root list survives");
    CHECK(loaded.formatVersion == ModuObj::kFormatVersion, "format version recorded");

    // Components survive (ChildB had a light, Grandchild a renderer + material).
    bool foundLight = false;
    bool foundRenderer = false;
    bool materialPreserved = false;
    ObjectType grandchildType = ObjectType::Empty;
    for (const SceneObject& o : loaded.objects) {
        if (o.name == "ChildB" && o.hasLight) foundLight = true;
        if (o.name == "Grandchild") {
            foundRenderer = o.hasRenderer && o.renderType == RenderType::Cube;
            grandchildType = o.type;
            materialPreserved =
                std::abs(o.material.color.r - 0.25f) < 0.01f &&
                std::abs(o.material.color.g - 0.50f) < 0.01f &&
                std::abs(o.material.color.b - 0.75f) < 0.01f;
        }
    }
    CHECK(foundLight, "light component survives serialization");
    CHECK(foundRenderer, "renderer component and render type survive serialization");
    CHECK(materialPreserved, "material property values survive serialization");
    CHECK(grandchildType == ObjectType::Cube, "object type survives serialization");

    // Hierarchy survives.
    for (const SceneObject& o : loaded.objects) {
        if (o.name != "Grandchild") continue;
        bool parentIsChildA = false;
        for (const SceneObject& p : loaded.objects) {
            if (p.id == o.parentId && p.name == "ChildA") parentIsChildA = true;
        }
        CHECK(parentIsChildA, "internal parent reference survives by id");
    }

    // Source-local ids are stable across repeated saves.
    {
        std::vector<int> firstIds;
        for (const SceneObject& o : loaded.objects) firstIds.push_back(o.id);
        CHECK(ModuObj::SaveAsset(path, loaded, error), "re-save loaded asset");
        ModuObj::Asset again;
        CHECK(ModuObj::LoadAsset(path, again, error), "re-load asset");
        std::vector<int> secondIds;
        for (const SceneObject& o : again.objects) secondIds.push_back(o.id);
        CHECK(firstIds == secondIds, "source-local ids stable across repeated saves");
        CHECK(again.assetId == loaded.assetId, "asset id stable across repeated saves");
    }

    // Single object and multi-root assets.
    {
        ModuObj::Asset single = BuildFrom(scene, { 12 }, "Single");
        const fsys::path p = gTemp / "Single.moduobj";
        CHECK(ModuObj::SaveAsset(p, single, error), "save single-object asset");
        ModuObj::Asset back;
        CHECK(ModuObj::LoadAsset(p, back, error), "load single-object asset");
        CHECK(back.objects.size() == 1 && back.roots.size() == 1, "single object round trips");

        ModuObj::Asset multi = BuildFrom(scene, { 11, 13 }, "Multi");
        const fsys::path mp = gTemp / "Multi.moduobj";
        CHECK(ModuObj::SaveAsset(mp, multi, error), "save multi-root asset");
        ModuObj::Asset mback;
        CHECK(ModuObj::LoadAsset(mp, mback, error), "load multi-root asset");
        CHECK(mback.roots.size() == 2, "multi-root list round trips");
    }

    // Scene-global state must not be carried in the asset. Checked against the
    // real serialized bytes, and against the *flat* key spellings the writer
    // actually emits: an earlier block-shaped check passed vacuously while the
    // skybox and fog settings were in fact being written into every asset.
    {
        std::ifstream in(path, std::ios::binary);
        const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(body.find("MODU_OBJASSET") != std::string::npos, "asset header present");
        CHECK(body.find("/home/") == std::string::npos, "no absolute machine paths in asset");

        const char* forbidden[] = {
            "MODU_SKYBOX", "skyboxMode", "skyboxSunTexture", "skyboxMoonTexture",
            "skyboxEnvironmentReflections", "fogEnabled", "fogColor", "fogDensity",
            "timeOfDay",
        };
        for (const char* key : forbidden) {
            ++checks;
            if (body.find(key) != std::string::npos) {
                std::printf("FAIL: asset carries scene-global state \"%s\"\n", key);
                ++failures;
            }
        }
        CHECK(body.find("[Object]") != std::string::npos, "object payload still present");
    }
}

static void TestMalformedRejection() {
    std::printf("-- malformed rejection --\n");
    std::string error;
    ModuObj::Asset asset;

    auto loadText = [&](const std::string& text, const char* what) {
        std::istringstream in(text);
        ModuObj::Asset out;
        std::string err;
        const bool ok = ModuObj::ReadAssetStream(in, out, err);
        ++checks;
        if (ok) {
            std::printf("FAIL: %s was accepted but should be rejected\n", what);
            ++failures;
        } else if (err.empty()) {
            std::printf("FAIL: %s rejected without an error message\n", what);
            ++failures;
        }
        return err;
    };

    loadText("this is not a moduobj at all", "garbage text");
    loadText("MODU_OBJASSET {\n  assetId=\"mobj-0123456789abcdef\";\n}\n", "header with no formatVersion");
    loadText("MODU_OBJASSET {\n  formatVersion=1;\n  assetId=\"bad\";\n}\n", "malformed asset id");

    // A future format version must be refused with a clear message.
    {
        const std::string err = loadText(
            "MODU_OBJASSET {\n  formatVersion=9999;\n  assetId=\"mobj-0123456789abcdef\";\n}\n",
            "future format version");
        CHECK(err.find("9999") != std::string::npos, "future-version error names the version");
    }

    // Duplicate source-local ids must be rejected, not merged.
    {
        std::vector<SceneObject> scene = MakeScene();
        ModuObj::Asset dup = BuildFrom(scene, { 10 }, "Dup");
        dup.objects[1].id = dup.objects[0].id;  // force a collision
        std::ostringstream out;
        std::string err;
        ModuObj::WriteAssetStream(out, dup, err);
        const std::string message = loadText(out.str(), "duplicate source-local ids");
        CHECK(message.find("duplicate") != std::string::npos,
              "duplicate-id error explains the problem");
    }

    // Missing file.
    CHECK(!ModuObj::LoadAsset(gTemp / "does-not-exist.moduobj", asset, error),
          "missing asset file rejected");
    CHECK(!error.empty(), "missing asset reports an error");

    // A failed save must not damage an existing good asset.
    {
        const std::vector<SceneObject> scene = MakeScene();
        ModuObj::Asset good = BuildFrom(scene, { 10 }, "Keep");
        const fsys::path p = gTemp / "Keep.moduobj";
        std::string err;
        CHECK(ModuObj::SaveAsset(p, good, err), "save a good asset first");
        const std::string before = [&] {
            std::ifstream in(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }();

        ModuObj::Asset broken;  // empty: no id, no objects, no roots
        CHECK(!ModuObj::SaveAsset(p, broken, err), "saving an invalid asset fails");
        const std::string after = [&] {
            std::ifstream in(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }();
        CHECK(before == after, "failed save left the previous asset intact");
        CHECK(!fsys::exists(fsys::path(p.string() + ".tmp")), "failed save left no temp file");
    }
}

static void TestInstantiation() {
    std::printf("-- instantiation --\n");
    const std::vector<SceneObject> scene = MakeScene();
    ModuObj::Asset asset = BuildFrom(scene, { 10 }, "Lamp");

    int nextId = 1000;
    ModuObj::InstanceResult a = ModuObj::Instantiate(asset, nextId);
    CHECK(a.success, "instantiate succeeds");
    CHECK(a.objects.size() == 4, "all objects materialized");
    CHECK(a.rootObjectIds.size() == 1, "one instance root");
    CHECK(a.localToScene.size() == 4, "complete source-local -> scene mapping");
    CHECK(!a.instanceId.empty(), "instance id assigned");

    // Every object is stamped with instance metadata.
    for (const SceneObject& o : a.objects) {
        CHECK(o.hasModuObjInstance, "object stamped as instance member");
        CHECK(o.moduObjInstance.instanceId == a.instanceId, "object carries the instance id");
        CHECK(o.moduObjInstance.assetId == asset.assetId, "object carries the source asset id");
        CHECK(o.moduObjInstance.sourceLocalId >= 0, "object records its source-local id");
    }
    int rootCount = 0;
    for (const SceneObject& o : a.objects) if (o.moduObjInstance.isRoot) ++rootCount;
    CHECK(rootCount == 1, "exactly one object flagged as instance root");

    // Hierarchy rebuilt through the mapping, never by name.
    for (const SceneObject& o : a.objects) {
        if (o.parentId == -1) continue;
        bool parentInInstance = false;
        for (const SceneObject& p : a.objects) if (p.id == o.parentId) parentInInstance = true;
        CHECK(parentInInstance, "internal reference points inside the same instance");
    }

    // A second instance must share nothing.
    ModuObj::InstanceResult b = ModuObj::Instantiate(asset, nextId);
    CHECK(b.success, "second instantiate succeeds");
    CHECK(b.instanceId != a.instanceId, "instances get distinct instance ids");

    std::unordered_set<int> ids;
    for (const SceneObject& o : a.objects) ids.insert(o.id);
    bool collision = false;
    for (const SceneObject& o : b.objects) if (!ids.insert(o.id).second) collision = true;
    CHECK(!collision, "scene object ids are unique across instances");

    // Mutating one instance must not touch the other or the cached asset.
    b.objects[0].name = "MUTATED";
    b.objects[0].position = glm::vec3(99.0f);
    CHECK(a.objects[0].name != "MUTATED", "instances have independent object state");
    CHECK(asset.objects[0].name != "MUTATED", "source asset not mutated by an instance");
    CHECK(asset.objects[0].position != glm::vec3(99.0f), "source transform not mutated");

    // Instantiate under a parent.
    {
        int n = 5000;
        ModuObj::InstanceResult c = ModuObj::Instantiate(asset, n, 42);
        CHECK(c.success, "instantiate under a parent succeeds");
        for (const SceneObject& o : c.objects) {
            if (o.moduObjInstance.isRoot) {
                CHECK(o.parentId == 42, "instance root reparented to the requested parent");
            }
        }
    }

    // Instantiate at a transform: roots move, children keep local transforms.
    {
        int n = 6000;
        const glm::vec3 pos(7.0f, 8.0f, 9.0f);
        const glm::vec3 scl(2.0f);
        ModuObj::InstanceResult c = ModuObj::Instantiate(asset, n, -1, &pos, nullptr, &scl);
        CHECK(c.success, "instantiate at a transform succeeds");
        for (const SceneObject& o : c.objects) {
            if (o.moduObjInstance.isRoot) {
                CHECK(o.position == pos, "placement position applied to root");
                CHECK(o.scale == scl, "placement scale applied to root");
            } else if (o.name == "Grandchild") {
                CHECK(o.position == glm::vec3(0.0f, 0.0f, 1.0f),
                      "child local transform untouched by placement");
            }
        }
    }

    // Multi-root instantiation exposes every root, not just the first.
    {
        ModuObj::Asset multi = BuildFrom(scene, { 11, 13 }, "Multi");
        int n = 7000;
        ModuObj::InstanceResult c = ModuObj::Instantiate(multi, n);
        CHECK(c.success, "multi-root instantiate succeeds");
        CHECK(c.rootObjectIds.size() == 2, "both roots returned, not just the first");
    }

    // An invalid asset fails without producing objects.
    {
        ModuObj::Asset empty;
        int n = 9000;
        ModuObj::InstanceResult bad = ModuObj::Instantiate(empty, n);
        CHECK(!bad.success, "instantiating an invalid asset fails");
        CHECK(bad.objects.empty(), "failed instantiation produces no partial objects");
        CHECK(!bad.error.empty(), "failed instantiation reports an error");
        CHECK(n == 9000, "failed instantiation did not consume object ids");
    }
}

static void TestNestingAndCycles() {
    std::printf("-- nesting and cycle detection --\n");
    const std::vector<SceneObject> scene = MakeScene();

    ModuObj::Asset a = BuildFrom(scene, { 10 }, "A");
    ModuObj::Asset b = BuildFrom(scene, { 11 }, "B");
    ModuObj::Asset c = BuildFrom(scene, { 13 }, "C");

    const fsys::path pa = gTemp / "A.moduobj";
    const fsys::path pb = gTemp / "B.moduobj";
    const fsys::path pc = gTemp / "C.moduobj";

    // Resolver maps asset id -> file, exactly how the editor will.
    std::unordered_map<std::string, fsys::path> table;
    auto resolver = [&](const std::string& id, fsys::path& out) {
        auto it = table.find(id);
        if (it == table.end()) return false;
        out = it->second;
        return true;
    };

    auto save = [&](ModuObj::Asset& asset, const fsys::path& p) {
        std::string err;
        if (!ModuObj::SaveAsset(p, asset, err)) {
            std::printf("FAIL: save %s: %s\n", p.filename().string().c_str(), err.c_str());
            ++failures;
        }
        ++checks;
        table[asset.assetId] = p;
    };

    // Acyclic: A nests B.
    a.nested.push_back({ a.roots.front(), b.assetId, "B.moduobj" });
    save(a, pa); save(b, pb); save(c, pc);

    std::string chain;
    CHECK(!ModuObj::DetectCycle(a.assetId, resolver, chain), "A -> B is not a cycle");

    // Nested refs survive the round trip.
    {
        ModuObj::Asset back;
        std::string err;
        CHECK(ModuObj::LoadAsset(pa, back, err), "reload nested asset");
        CHECK(back.nested.size() == 1, "nested reference survives serialization");
        CHECK(back.nested[0].assetId == b.assetId, "nested asset id preserved");
        CHECK(back.nested[0].lastKnownPath == "B.moduobj", "nested path metadata preserved");
    }

    // Direct self-reference: A contains A.
    {
        ModuObj::Asset selfRef = a;
        selfRef.nested.clear();
        selfRef.nested.push_back({ selfRef.roots.front(), selfRef.assetId, "A.moduobj" });
        save(selfRef, pa);
        chain.clear();
        CHECK(ModuObj::DetectCycle(selfRef.assetId, resolver, chain), "direct self-reference detected");
        CHECK(chain.find("->") != std::string::npos, "self-reference reports a dependency chain");
        save(a, pa);  // restore A -> B
    }

    // Two-asset cycle: A -> B -> A.
    {
        ModuObj::Asset bCycle = b;
        bCycle.nested.push_back({ bCycle.roots.front(), a.assetId, "A.moduobj" });
        save(bCycle, pb);
        chain.clear();
        CHECK(ModuObj::DetectCycle(a.assetId, resolver, chain), "two-asset cycle detected");
        CHECK(chain.find(a.assetId) != std::string::npos, "cycle chain names the asset");
        save(b, pb);  // restore
    }

    // Longer indirect cycle: A -> B -> C -> A.
    {
        ModuObj::Asset bMid = b;
        bMid.nested.push_back({ bMid.roots.front(), c.assetId, "C.moduobj" });
        save(bMid, pb);
        ModuObj::Asset cEnd = c;
        cEnd.nested.push_back({ cEnd.roots.front(), a.assetId, "A.moduobj" });
        save(cEnd, pc);
        chain.clear();
        CHECK(ModuObj::DetectCycle(a.assetId, resolver, chain), "three-asset indirect cycle detected");
        save(b, pb); save(c, pc);
    }

    // A missing nested asset is not a cycle and must not hang or crash.
    {
        ModuObj::Asset missing = a;
        missing.nested.clear();
        missing.nested.push_back({ missing.roots.front(), "mobj-deadbeefdeadbeef", "Gone.moduobj" });
        save(missing, pa);
        chain.clear();
        CHECK(!ModuObj::DetectCycle(missing.assetId, resolver, chain),
              "missing nested asset is not reported as a cycle");
    }
}

// Scene save/load must preserve instance metadata, and scenes with no ModuOBJ
// must be completely unaffected. Uses the public SceneSerializer entry points, so
// this goes through the default (modular) save path rather than the raw stream
// the asset payload uses.
static void TestSceneRoundTrip() {
    std::printf("-- scene serialization of instances --\n");

    const std::vector<SceneObject> scene = MakeScene();
    ModuObj::Asset asset = BuildFrom(scene, { 10 }, "Lamp");

    int nextId = 2000;
    ModuObj::InstanceResult inst = ModuObj::Instantiate(asset, nextId);
    CHECK(inst.success, "instance created for scene round trip");

    const fsys::path scenePath = gTemp / "WithInstance.scene";
    CHECK(SceneSerializer::saveScene(scenePath, inst.objects, nextId, 0.5f, SkyboxSettings{}),
          "save scene containing a ModuOBJ instance");

    std::vector<SceneObject> loaded;
    int loadedNextId = 0;
    int version = 0;
    CHECK(SceneSerializer::loadScene(scenePath, loaded, loadedNextId, version,
                                     nullptr, nullptr, nullptr),
          "load scene containing a ModuOBJ instance");
    CHECK(loaded.size() == inst.objects.size(), "object count survives scene round trip");

    int stamped = 0;
    int roots = 0;
    for (const SceneObject& o : loaded) {
        if (!o.hasModuObjInstance) continue;
        ++stamped;
        if (o.moduObjInstance.isRoot) ++roots;
        CHECK(o.moduObjInstance.assetId == asset.assetId, "asset id survives scene round trip");
        CHECK(o.moduObjInstance.instanceId == inst.instanceId, "instance id survives scene round trip");
        CHECK(o.moduObjInstance.sourceLocalId >= 0, "source-local id survives scene round trip");
    }
    CHECK(stamped == static_cast<int>(inst.objects.size()),
          "every instance object keeps its membership after save/load");
    CHECK(roots == 1, "instance root flag survives scene round trip");

    // A scene with no ModuOBJ must not gain any ModuOBJ keys, and must reload
    // exactly as before.
    {
        const fsys::path plainPath = gTemp / "Plain.scene";
        std::vector<SceneObject> plain = MakeScene();
        int n = 100;
        CHECK(SceneSerializer::saveScene(plainPath, plain, n, 0.5f, SkyboxSettings{}),
              "save scene with no ModuOBJ");

        std::ifstream in(plainPath, std::ios::binary);
        const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(body.find("moduObj") == std::string::npos,
              "scene without ModuOBJ contains no ModuOBJ keys");

        std::vector<SceneObject> back;
        int backNext = 0, backVersion = 0;
        CHECK(SceneSerializer::loadScene(plainPath, back, backNext, backVersion,
                                         nullptr, nullptr, nullptr),
              "reload scene with no ModuOBJ");
        CHECK(back.size() == plain.size(), "plain scene object count unchanged");
        for (const SceneObject& o : back) {
            CHECK(!o.hasModuObjInstance, "plain scene objects are not marked as instances");
        }
    }
}

// Runtime API: spawn/destroy/query against a plain scene vector.
static void TestRuntimeApi() {
    std::printf("-- runtime api --\n");
    const std::vector<SceneObject> source = MakeScene();
    ModuObj::Asset asset = BuildFrom(source, { 10 }, "Lamp");
    ModuObj::Asset multi = BuildFrom(source, { 11, 13 }, "Multi");

    std::vector<SceneObject> scene;
    int nextId = 500;

    // Spawn.
    ModuObj::InstanceResult a = ModuObj::SpawnIntoScene(asset, scene, nextId);
    CHECK(a.success, "spawn into scene succeeds");
    CHECK(scene.size() == 4, "spawned objects appended to the scene");
    CHECK(nextId == 504, "nextObjectId advanced by the object count");

    // Queries.
    CHECK(ModuObj::GetInstanceRoots(a.instanceId, scene).size() == 1, "query roots");
    CHECK(ModuObj::GetInstanceAssetId(a.instanceId, scene) == asset.assetId, "query source asset");
    std::string err;
    CHECK(ModuObj::ValidateInstance(a.instanceId, scene, err), "validate instance");

    // Source-local lookup is the name-independent path.
    const int rootLocal = asset.roots.front();
    const int rootSceneId = ModuObj::FindInstanceObject(a.instanceId, rootLocal, scene);
    CHECK(rootSceneId == a.rootObjectIds.front(), "find object by source-local id");
    CHECK(ModuObj::FindInstanceObject(a.instanceId, 9999, scene) == -1,
          "unknown source-local id returns -1");
    CHECK(ModuObj::FindInstanceObjectByName(a.instanceId, "Grandchild", scene) != -1,
          "find object by name convenience");

    // Spawn under a parent: root attaches to the parent's child list.
    ModuObj::InstanceResult b = ModuObj::SpawnIntoScene(asset, scene, nextId, rootSceneId);
    CHECK(b.success, "spawn under a parent succeeds");
    {
        auto parent = std::find_if(scene.begin(), scene.end(),
                                   [&](const SceneObject& o) { return o.id == rootSceneId; });
        CHECK(parent != scene.end(), "parent still present");
        const bool linked = parent != scene.end() &&
                            std::find(parent->childIds.begin(), parent->childIds.end(),
                                      b.rootObjectIds.front()) != parent->childIds.end();
        CHECK(linked, "spawned root added to the parent's child list");
    }

    // Spawning under a missing parent fails and appends nothing.
    {
        const size_t before = scene.size();
        const int beforeNext = nextId;
        ModuObj::InstanceResult bad = ModuObj::SpawnIntoScene(asset, scene, nextId, 999999);
        CHECK(!bad.success, "spawn under a missing parent fails");
        CHECK(scene.size() == before, "failed spawn appended nothing");
        CHECK(nextId == beforeNext, "failed spawn did not consume object ids");
    }

    // Spawn disabled affects the root only.
    {
        ModuObj::InstanceResult d = ModuObj::SpawnIntoScene(asset, scene, nextId, -1,
                                                            nullptr, nullptr, nullptr, true);
        CHECK(d.success, "spawn disabled succeeds");
        for (const SceneObject& o : scene) {
            if (!o.hasModuObjInstance || o.moduObjInstance.instanceId != d.instanceId) continue;
            if (o.moduObjInstance.isRoot) CHECK(!o.enabled, "disabled spawn root is disabled");
        }
        ModuObj::DestroyInstance(d.instanceId, scene);
    }

    // Multi-root spawn exposes both roots.
    {
        ModuObj::InstanceResult m = ModuObj::SpawnIntoScene(multi, scene, nextId);
        CHECK(m.success, "multi-root spawn succeeds");
        CHECK(ModuObj::GetInstanceRoots(m.instanceId, scene).size() == 2,
              "multi-root spawn exposes both roots, none discarded");
        ModuObj::DestroyInstance(m.instanceId, scene);
    }

    // Destroying the outer instance also removes the nested-under-it instance.
    {
        const int removed = ModuObj::DestroyInstance(a.instanceId, scene);
        CHECK(removed >= 4, "destroy removes the whole instance");
        CHECK(ModuObj::GetInstanceRoots(a.instanceId, scene).empty(), "instance roots gone");
        CHECK(!ModuObj::ValidateInstance(a.instanceId, scene, err), "destroyed instance no longer validates");
        // b was parented under a's root, so it must have gone with it.
        CHECK(ModuObj::GetInstanceRoots(b.instanceId, scene).empty(),
              "instance parented under the destroyed one is removed too");
        // No surviving object may reference a removed id.
        std::unordered_set<int> alive;
        for (const SceneObject& o : scene) alive.insert(o.id);
        for (const SceneObject& o : scene) {
            for (int child : o.childIds) {
                CHECK(alive.count(child) != 0, "no dangling child id after destroy");
            }
        }
    }

    CHECK(ModuObj::DestroyInstance("minst-does-not-exist", scene) == 0,
          "destroying an unknown instance is a no-op");
}

// Cache must serve repeats, notice edits, and never hand out mutable template state.
static void TestCache() {
    std::printf("-- asset cache --\n");
    const std::vector<SceneObject> source = MakeScene();
    ModuObj::Asset asset = BuildFrom(source, { 10 }, "Cached");
    const fsys::path path = gTemp / "Cached.moduobj";
    std::string err;
    CHECK(ModuObj::SaveAsset(path, asset, err), "save asset for caching");

    ModuObj::AssetCache cache;
    const ModuObj::Asset* first = cache.get(path, err);
    CHECK(first != nullptr, "cache loads the asset");
    CHECK(cache.size() == 1, "cache holds one entry");

    const ModuObj::Asset* second = cache.get(path, err);
    CHECK(second == first, "repeat get returns the cached template without reparsing");

    // Instances must deep-copy: mutating a spawned object cannot touch the cache.
    {
        std::vector<SceneObject> scene;
        int nextId = 1;
        const std::string cachedName = first->objects.front().name;
        ModuObj::InstanceResult r = ModuObj::SpawnIntoScene(*first, scene, nextId);
        CHECK(r.success, "spawn from a cached template");
        scene.front().name = "MUTATED";
        CHECK(cache.get(path, err)->objects.front().name == cachedName,
              "instance mutation does not write back into the cached template");
    }

    // An edit on disk is picked up via modification time.
    {
        ModuObj::Asset edited = asset;
        edited.name = "CachedRenamed";
        // Ensure a distinct mtime even on coarse-resolution filesystems.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(ModuObj::SaveAsset(path, edited, err), "overwrite the asset on disk");
        const ModuObj::Asset* reloaded = cache.get(path, err);
        CHECK(reloaded != nullptr && reloaded->name == "CachedRenamed",
              "cache reloads after the file changes");
    }

    // A deleted asset must evict rather than serve stale data.
    {
        std::error_code ec;
        fsys::remove(path, ec);
        const ModuObj::Asset* gone = cache.get(path, err);
        CHECK(gone == nullptr, "deleted asset is not served from cache");
        CHECK(!err.empty(), "deleted asset reports an error");
        CHECK(cache.size() == 0, "deleted asset evicted from the cache");
    }

    cache.clear();
    CHECK(cache.size() == 0, "clear empties the cache");
}

int main(int argc, char** argv) {
    gTemp = (argc >= 2) ? fsys::path(argv[1])
                        : fsys::temp_directory_path() / "modularity-moduobj-selftest";
    std::error_code ec;
    fsys::remove_all(gTemp, ec);
    fsys::create_directories(gTemp, ec);

    TestAssetIdentity();
    TestAuthoring();
    TestSerialization();
    TestMalformedRejection();
    TestInstantiation();
    TestSceneRoundTrip();
    TestRuntimeApi();
    TestCache();
    TestNestingAndCycles();

    std::printf("\nmoduobj self-test: %d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
