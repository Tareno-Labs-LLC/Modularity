#include "ScriptRuntime.h"
extern "C" MODULARITY_SCRIPT_EXPORT int Modularity_ScriptAbiVersion() { return MODULARITY_NATIVE_SCRIPT_ABI_VERSION; }
extern "C" MODULARITY_SCRIPT_EXPORT unsigned long long Modularity_ScriptLayoutSignature() { return MODULARITY_SCRIPT_LAYOUT_SIGNATURE(); }
static void deleteTask(ScriptContext& ctx, float) { ctx.DeleteSceneObject(ctx.object->id); }
static void lateTask(ScriptContext& ctx, float) { ctx.CreateSceneObject("Empty", "UnexpectedTask", -1); }
extern "C" MODULARITY_SCRIPT_EXPORT void Script_Begin(ScriptContext& ctx, float) {
    if (ctx.object->name == "DeleteInBegin") { ctx.DeleteSceneObject(ctx.object->id); return; }
    if (ctx.object->name == "DeleteInTask") { ctx.StartIEnum(deleteTask); ctx.StartIEnum(lateTask); return; }
    for (int i = 0; i < 24; ++i) ctx.CreateSceneObject("Empty", "FromBegin", -1);
    ctx.SetPosition(glm::vec3(3));
}
extern "C" MODULARITY_SCRIPT_EXPORT void Script_Update(ScriptContext& ctx, float) {
    if (!ctx.object) { ctx.CreateSceneObject("Empty", "UnexpectedUpdate", -1); return; }
    ctx.SetPosition(glm::vec3(7));
}
