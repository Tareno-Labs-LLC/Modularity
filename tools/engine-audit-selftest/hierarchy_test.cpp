#include "EditorWindows/HierarchySnapshot.h"
#include "ScriptContextBinding.h"
class Engine {};
static std::vector<SceneObject>* bindingScene = nullptr;
SceneObject* ScriptContext::FindObjectById(int id) {
    if (!bindingScene) return nullptr;
    for (auto& object : *bindingScene) if (object.id == id) return &object;
    return nullptr;
}
#include <cstdio>

static int failures = 0;
static void check(bool pass, const char* label) {
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
    if (!pass) ++failures;
}
static SceneObject node(int id, int parent, const char* name) {
    SceneObject obj(name, ObjectType::Empty, id);
    obj.parentId = parent;
    return obj;
}
static void checkContextBindings() {
    Engine engine;
    std::vector<SceneObject> objects;
    objects.push_back(node(10, -1, "Other"));
    objects.push_back(node(20, -1, "Scripted"));
    objects.back().scripts.emplace_back();
    objects.back().scripts.back().path = "script.moducpp";
    bindingScene = &objects;
    ScriptContext ctx;
    ctx.engine = &engine;
    ctx.object = &objects[1];
    ctx.script = &objects[1].scripts[0];
    ScriptContextBinding binding(ctx);
    {
        ScriptSceneMutationScope mutation(&engine);
        objects.reserve(objects.capacity() + 32);
    }
    check(ctx.object == &objects[1] && ctx.script == &objects[1].scripts[0], "script context rebinds after scene storage reallocates");
    {
        ScriptSceneMutationScope mutation(&engine);
        objects[1].scripts.reserve(objects[1].scripts.capacity() + 32);
        objects[1].scripts.emplace_back();
    }
    check(ctx.script == &objects[1].scripts[0], "attaching a script refreshes the current component pointer");
    {
        ScriptSceneMutationScope mutation(&engine);
        std::swap(objects[0], objects[1]);
    }
    check(ctx.object == &objects[0] && ctx.object->id == 20, "in-place scene reorder preserves the bound object identity");
    {
        ScriptContext nested;
        nested.engine = &engine;
        nested.object = &objects[1];
        ScriptContextBinding nestedBinding(nested);
        {
            ScriptSceneMutationScope mutation(&engine);
            objects.reserve(objects.capacity() + 32);
        }
        check(ctx.object == &objects[0] && nested.object == &objects[1], "scene mutation refreshes nested dispatch contexts");
    }
    {
        ScriptSceneMutationScope mutation(&engine);
        objects.erase(objects.begin() + 1);
    }
    check(ctx.object == &objects[0] && ctx.script, "deleting another object leaves the active script bound");
    {
        ScriptSceneMutationScope mutation(&engine);
        objects[0].scripts[0].path = "replacement.moducpp";
    }
    check(ctx.object == &objects[0] && !ctx.script, "replaced script slots are not mistaken for the original component");
    {
        ScriptSceneMutationScope mutation(&engine);
        objects.clear();
    }
    check(!ctx.object && !ctx.script, "self-deletion clears both context pointers before returning to the caller");
    bindingScene = nullptr;
}
int main() {
    checkContextBindings();
    std::vector<SceneObject> scene;
    scene.push_back(node(1, -1, "Canvas"));
    scene.back().hasUI = true; scene.back().ui.type = UIElementType::Canvas;
    scene.push_back(node(2, 1, "Grid Layout Group"));
    scene.back().hasUI = true; scene.back().ui.type = UIElementType::Panel;
    scene.back().ui.layout = UILayoutMode::Grid;
    scene.push_back(node(3, 2, "Cell"));
    Modularity::HierarchySnapshot snapshot;
    auto refresh = [&](const std::string& filter = "") {
        snapshot.refresh(scene, filter, [](auto&, const auto&) {});
    };
    refresh();
    check(snapshot.rootIds == std::vector<int>{1}, "Canvas is the root of the UI hierarchy");
    check(snapshot.find(1)->childIds == std::vector<int>{2}, "grid is visible even when Canvas childIds is missing");
    check(snapshot.find(2)->childIds == std::vector<int>{3}, "layout children are reconstructed from parent IDs");
    refresh("gRiD");
    check(snapshot.find(2)->matchesFilter && snapshot.find(1)->matchesFilter, "search keeps the matching grid's parent path");
    check(!snapshot.find(3)->matchesFilter, "unmatched descendants are filtered");
    const auto* cachedGrid = snapshot.find(2);
    const auto oldChildren = cachedGrid->childIds;
    const size_t capacity = scene.capacity();
    while (scene.size() <= capacity) scene.push_back(node(int(scene.size() + 1), -1, "New"));
    scene.erase(scene.begin() + 1);
    check(cachedGrid->name == "Grid Layout Group" && cachedGrid->childIds == oldChildren,
          "owned cached row survives scene reallocation and erase during traversal");
    refresh();
    check(!snapshot.find(2), "refresh removes the deleted grid row");
    check(snapshot.find(1)->childIds.empty(), "refresh removes stale links to deleted objects");
    check(std::find(snapshot.rootIds.begin(), snapshot.rootIds.end(), 3) != snapshot.rootIds.end(),
          "a child with a missing parent remains inspectable");
    auto* storage = snapshot.nodes.data();
    refresh();
    check(storage == snapshot.nodes.data(), "steady-state refresh reuses row storage");
    scene = {node(10, -1, "Root"), node(11, 10, "First"), node(12, 10, "Second"), node(13, -1, "Other")};
    scene[0].childIds = {12, 11, 12, 999, 13};
    refresh();
    check(snapshot.find(10)->childIds == std::vector<int>({12, 11}), "sibling order is preserved and stale/duplicate links are ignored");
    scene[1].parentId = 13;
    refresh();
    check(snapshot.find(10)->childIds == std::vector<int>{12} && snapshot.find(13)->childIds == std::vector<int>{11},
          "reparenting updates both cached branches");
    scene = {node(20, 21, "A"), node(21, 20, "B"), node(22, 22, "Self")};
    refresh("B");
    check(snapshot.rootIds.size() == 2, "cycles and self-parented rows remain inspectable");
    check(snapshot.find(20)->matchesFilter && snapshot.find(21)->matchesFilter, "cyclic ancestor filtering terminates");
    scene.clear(); refresh();
    check(snapshot.nodes.empty() && snapshot.rootIds.empty() && !snapshot.find(20), "empty scenes clear all cached objects");
    std::printf("Hierarchy snapshot: %d failures\n", failures);
    return failures ? 1 : 0;
}
