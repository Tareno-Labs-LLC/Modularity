#pragma once
#include "ScriptRuntime.h"
#include "SceneObject.h"

// A dispatch owns identity, never a pointer into scene/script vector storage.
// Mutation APIs refresh every nested dispatch before returning to script code.
class ScriptContextBinding {
public:
    explicit ScriptContextBinding(ScriptContext& context) : ctx(context), previous(active) {
        objectId = ctx.object ? ctx.object->id : -1;
        if (ctx.object && ctx.script) {
            for (size_t i = 0; i < ctx.object->scripts.size(); ++i) {
                if (&ctx.object->scripts[i] == ctx.script) {
                    scriptIndex = i;
                    scriptPath = ctx.script->path;
                    break;
                }
            }
        }
        active = this;
    }
    ~ScriptContextBinding() { active = previous; }
    ScriptContextBinding(const ScriptContextBinding&) = delete;
    ScriptContextBinding& operator=(const ScriptContextBinding&) = delete;

    static void RefreshAll(Engine* engine) {
        for (auto* binding = active; binding; binding = binding->previous) {
            if (binding->ctx.engine != engine) continue;
            auto& ctx = binding->ctx;
            ctx.object = binding->objectId >= 0 ? ctx.FindObjectById(binding->objectId) : nullptr;
            ctx.script = nullptr;
            if (ctx.object && binding->scriptIndex < ctx.object->scripts.size()) {
                auto& script = ctx.object->scripts[binding->scriptIndex];
                if (script.path == binding->scriptPath) ctx.script = &script;
            }
        }
    }
private:
    ScriptContext& ctx;
    int objectId = -1;
    size_t scriptIndex = static_cast<size_t>(-1);
    std::string scriptPath;
    ScriptContextBinding* previous = nullptr;
    inline static thread_local ScriptContextBinding* active = nullptr;
};

class ScriptSceneMutationScope {
public:
    explicit ScriptSceneMutationScope(Engine* owner) : engine(owner) {}
    ~ScriptSceneMutationScope() { ScriptContextBinding::RefreshAll(engine); }
private:
    Engine* engine;
};
