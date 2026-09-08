#include "ManagedScriptRuntime.h"
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#if MODULARITY_USE_MONO
#include <mono/jit/jit.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/attrdefs.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/mono-gc.h>
#include <mono/metadata/mono-config.h>
#include <mono/metadata/threads.h>
#endif

#if MODULARITY_USE_MONO
namespace {
std::string trim(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string stripAssemblyQualifier(const std::string& typeName) {
    auto comma = typeName.find(',');
    if (comma == std::string::npos) {
        return trim(typeName);
    }
    return trim(typeName.substr(0, comma));
}

bool splitNamespaceAndName(const std::string& fullName, std::string& nameSpace, std::string& name) {
    auto dot = fullName.rfind('.');
    if (dot == std::string::npos) {
        nameSpace.clear();
        name = fullName;
        return !name.empty();
    }
    nameSpace = fullName.substr(0, dot);
    name = fullName.substr(dot + 1);
    return !name.empty();
}

std::string monoExceptionToString(MonoObject* exc) {
    if (!exc) return "Unknown managed exception";
    MonoClass* klass = mono_object_get_class(exc);
    if (!klass) return "Managed exception (no class)";
    const char* name = mono_class_get_name(klass);
    const char* ns = mono_class_get_namespace(klass);
    std::string out = "Managed exception";
    if (ns && ns[0] != '\0') {
        out += " (";
        out += ns;
        out += ".";
        out += name ? name : "Unknown";
        out += ")";
    } else if (name && name[0] != '\0') {
        out += " (";
        out += name;
        out += ")";
    }
    return out;
}

fs::path resolveMonoRoot() {
    const char* env = std::getenv("MODU_MONO_ROOT");
    if (env && env[0] != '\0') {
        return fs::path(env);
    }

    std::vector<fs::path> candidates;
    fs::path cwd = fs::current_path();
    candidates.push_back(cwd / "src" / "ThirdParty" / "mono");
    candidates.push_back(cwd / ".." / "src" / "ThirdParty" / "mono");
    candidates.push_back(cwd / "ThirdParty" / "mono");
    candidates.push_back(cwd / ".." / "ThirdParty" / "mono");

    for (const auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path, ec)) {
            return path;
        }
    }

    return {};
}

void configureMonoFromRoot(const fs::path& root) {
    fs::path libDir = root / "lib";
    fs::path etcDir = root / "etc";
    mono_set_dirs(libDir.string().c_str(), etcDir.string().c_str());

    fs::path assembliesDir = root / "lib" / "mono" / "4.5";
    if (fs::exists(assembliesDir)) {
        mono_set_assemblies_path(assembliesDir.string().c_str());
    }
    mono_config_parse(nullptr);
}

std::string toKey(const fs::path& assemblyPath, const std::string& typeName) {
    return assemblyPath.lexically_normal().string() + "|" + typeName;
}
} // namespace

struct ManagedScriptRuntime::MonoState {
    MonoDomain* rootDomain = nullptr;
    MonoDomain* scriptDomain = nullptr;
    fs::path monoRoot;
};

ManagedScriptRuntime::~ManagedScriptRuntime() {
    unloadAll();
    monoState.reset();
}

void ManagedScriptRuntime::MonoStateDeleter::operator()(MonoState* state) const {
    if (!state) return;
    if (state->rootDomain) {
        if (state->scriptDomain) {
            mono_domain_set(state->rootDomain, false);
            mono_domain_unload(state->scriptDomain);
            state->scriptDomain = nullptr;
        }
        mono_jit_cleanup(state->rootDomain);
    }
    delete state;
}

bool ManagedScriptRuntime::ensureHost(const fs::path& assemblyPath) {
    (void)assemblyPath;
    if (monoState && monoState->rootDomain && monoState->scriptDomain) return true;

    if (!monoState) {
        fs::path monoRoot = resolveMonoRoot();
        if (monoRoot.empty()) {
            lastError = "Mono root not found. Set MODU_MONO_ROOT or vendor Mono in src/ThirdParty/mono.";
            return false;
        }

        auto* state = new MonoState();
        state->monoRoot = monoRoot;
        configureMonoFromRoot(monoRoot);
        state->rootDomain = mono_jit_init_version("Modularity", "v4.0.30319");
        if (!state->rootDomain) {
            delete state;
            lastError = "Failed to initialize Mono JIT";
            return false;
        }
        monoState.reset(state);

        std::cerr << "[Managed] Mono root: " << monoRoot << std::endl;
    }

    if (!monoState->scriptDomain) {
        monoState->scriptDomain = mono_domain_create_appdomain(const_cast<char*>("ModularityScripts"), nullptr);
        if (!monoState->scriptDomain) {
            lastError = "Failed to create Mono appdomain";
            return false;
        }
    }

    mono_domain_set(monoState->scriptDomain, false);
    mono_thread_attach(monoState->scriptDomain);
    return true;
}

static MonoAssembly* loadAssembly(MonoDomain* domain, const fs::path& assemblyPath, MonoImage** outImage,
                                  std::string& error) {
    if (!outImage) {
        error = "Internal error: missing image output";
        return nullptr;
    }
    *outImage = nullptr;

    std::error_code ec;
    if (!fs::exists(assemblyPath, ec)) {
        error = "Missing managed assembly: " + assemblyPath.string();
        return nullptr;
    }

    mono_domain_set(domain, false);
    MonoAssembly* assembly = mono_domain_assembly_open(domain, assemblyPath.string().c_str());
    if (!assembly) {
        error = "Mono failed to load assembly: " + assemblyPath.string();
        return nullptr;
    }
    MonoImage* image = mono_assembly_get_image(assembly);
    if (!image) {
        error = "Mono failed to get image: " + assemblyPath.string();
        return nullptr;
    }

    *outImage = image;
    return assembly;
}

bool ManagedScriptRuntime::ensureApiInjected(const fs::path& assemblyPath) {
    if (apiInjected) return true;
    if (!monoState || !monoState->scriptDomain) return false;

    std::string error;
    MonoImage* image = nullptr;
    MonoAssembly* assembly = loadAssembly(monoState->scriptDomain, assemblyPath, &image, error);
    if (!assembly || !image) {
        lastError = error;
        return false;
    }

    MonoClass* hostClass = mono_class_from_name(image, "ModuCPP", "Host");
    if (!hostClass) {
        lastError = "Managed class ModuCPP.Host not found";
        return false;
    }

    MonoMethod* setApiMethod = mono_class_get_method_from_name(hostClass, "SetNativeApi", 1);
    if (!setApiMethod) {
        lastError = "Managed method ModuCPP.Host.SetNativeApi not found";
        return false;
    }

    mono_domain_set(monoState->scriptDomain, false);
    mono_thread_attach(monoState->scriptDomain);
    intptr_t apiPtr = reinterpret_cast<intptr_t>(&api);
    void* args[1] = { &apiPtr };
    MonoObject* exc = nullptr;
    mono_runtime_invoke(setApiMethod, nullptr, args, &exc);
    if (exc) {
        lastError = monoExceptionToString(exc);
        return false;
    }

    apiInjected = true;
    return true;
}

bool ManagedScriptRuntime::loadModuleMethods(Module& mod, const fs::path& assemblyPath,
                                             const std::string& typeName) {
    if (!monoState || !monoState->scriptDomain || typeName.empty()) {
        lastError = "Managed script type is required";
        return false;
    }

    std::string error;
    MonoImage* image = nullptr;
    MonoAssembly* assembly = loadAssembly(monoState->scriptDomain, assemblyPath, &image, error);
    if (!assembly || !image) {
        lastError = error;
        return false;
    }

    std::string normalized = stripAssemblyQualifier(typeName);
    std::string nameSpace;
    std::string className;
    if (!splitNamespaceAndName(normalized, nameSpace, className)) {
        lastError = "Managed script type name is invalid";
        return false;
    }

    MonoClass* klass = mono_class_from_name(image, nameSpace.c_str(), className.c_str());
    if (!klass) {
        lastError = "Managed type not found: " + normalized;
        return false;
    }

    auto findMethod = [&](const std::initializer_list<const char*>& names,
                          int paramCount) -> MonoMethod* {
        for (const char* name : names) {
            MonoMethod* method = mono_class_get_method_from_name(klass, name, paramCount);
            if (method) return method;
        }
        return nullptr;
    };

    auto isMethodStatic = [](MonoMethod* method) {
        if (!method) return true;
        uint32_t flags = mono_method_get_flags(method, nullptr);
        constexpr uint32_t kStaticMask = 0x0010;
        return (flags & kStaticMask) != 0;
    };

    auto loadSlot = [&](Module::MethodSlot& slot,
                        const std::initializer_list<const char*>& names,
                        bool allowDelta) {
        MonoMethod* method = nullptr;
        bool hasDelta = false;
        if (allowDelta) {
            method = findMethod(names, 2);
            if (method) {
                hasDelta = true;
            } else {
                method = findMethod(names, 1);
            }
        } else {
            method = findMethod(names, 1);
        }

        slot.method = method;
        slot.hasDelta = hasDelta;
        slot.isStatic = isMethodStatic(method);
    };

    loadSlot(mod.inspector, {"Script_OnInspector", "OnInspector"}, false);
    loadSlot(mod.begin, {"Script_Begin", "Begin"}, true);
    loadSlot(mod.spec, {"Script_Spec", "Spec"}, true);
    loadSlot(mod.testEditor, {"Script_TestEditor", "TestEditor"}, true);
    loadSlot(mod.update, {"Script_Update", "Update"}, true);
    loadSlot(mod.tickUpdate, {"Script_TickUpdate", "TickUpdate"}, true);

    if (!mod.inspector.method && !mod.begin.method && !mod.spec.method &&
        !mod.testEditor.method && !mod.update.method && !mod.tickUpdate.method) {
        lastError = "No managed script exports found";
        return false;
    }

    MonoClass* inspectorHost = mono_class_from_name(image, "ModuCPP", "Inspector");
    if (inspectorHost) {
        MonoMethod* autoMethod = mono_class_get_method_from_name(inspectorHost, "RenderAuto", 2);
        mod.autoInspectorMethod = autoMethod;
    }

    bool needsInstance = false;
    const Module::MethodSlot* slots[] = {
        &mod.inspector, &mod.begin, &mod.spec, &mod.testEditor, &mod.update, &mod.tickUpdate
    };
    for (const Module::MethodSlot* slot : slots) {
        if (slot->method && !slot->isStatic) {
            needsInstance = true;
            break;
        }
    }

    if (mod.autoInspectorMethod) {
        needsInstance = true;
    }

    if (needsInstance) {
        mono_domain_set(monoState->scriptDomain, false);
        mono_thread_attach(monoState->scriptDomain);
        MonoObject* instance = mono_object_new(monoState->scriptDomain, klass);
        if (!instance) {
            lastError = "Failed to create managed script instance";
            return false;
        }
        mono_runtime_object_init(instance);
        mod.instance = instance;
        mod.instanceHandle = mono_gchandle_new(instance, true);
    }

    return true;
}

ManagedScriptRuntime::Module* ManagedScriptRuntime::getModule(const fs::path& assemblyPath,
                                                              const std::string& typeName) {
    lastError.clear();
    if (assemblyPath.empty()) {
        lastError = "Managed assembly path is empty";
        return nullptr;
    }

    std::string key = toKey(assemblyPath, typeName);
    auto it = modules.find(key);
    if (it != modules.end()) return &it->second;

    if (!ensureHost(assemblyPath)) return nullptr;
    if (!ensureApiInjected(assemblyPath)) return nullptr;

    Module mod;
    mod.assemblyPath = assemblyPath;
    mod.typeName = typeName;
    if (!loadModuleMethods(mod, assemblyPath, typeName)) return nullptr;

    modules[key] = mod;
    return &modules[key];
}

static bool invokeMonoMethod(MonoDomain* domain, MonoMethod* method, MonoObject* instance,
                             ScriptContext* ctx, float deltaTime, bool hasDelta, std::string& error) {
    if (!method) return false;
    mono_domain_set(domain, false);
    mono_thread_attach(domain);

    intptr_t ctxPtr = reinterpret_cast<intptr_t>(ctx);
    void* argsWithDelta[2] = { &ctxPtr, &deltaTime };
    void* argsNoDelta[1] = { &ctxPtr };

    MonoObject* exc = nullptr;
    mono_runtime_invoke(method, instance, hasDelta ? argsWithDelta : argsNoDelta, &exc);
    if (exc) {
        error = monoExceptionToString(exc);
        return false;
    }
    return true;
}

bool ManagedScriptRuntime::invokeInspector(const fs::path& assemblyPath, const std::string& typeName,
                                           ScriptContext& ctx) {
    Module* mod = getModule(assemblyPath, typeName);
    if (!mod) return false;
    MonoMethod* inspector = reinterpret_cast<MonoMethod*>(mod->inspector.method);
    std::string error;
    if (!inspector) {
        MonoMethod* autoInspector = reinterpret_cast<MonoMethod*>(mod->autoInspectorMethod);
        if (!autoInspector) {
            lastError.clear();
            return false;
        }
        mono_domain_set(monoState->scriptDomain, false);
        mono_thread_attach(monoState->scriptDomain);
        intptr_t ctxPtr = reinterpret_cast<intptr_t>(&ctx);
        MonoObject* instanceObj = reinterpret_cast<MonoObject*>(mod->instance);
        void* args[2] = { &ctxPtr, instanceObj };
        MonoObject* exc = nullptr;
        mono_runtime_invoke(autoInspector, nullptr, args, &exc);
        if (exc) {
            lastError = monoExceptionToString(exc);
            return false;
        }
        return true;
    }
    MonoObject* instance = mod->inspector.isStatic ? nullptr : reinterpret_cast<MonoObject*>(mod->instance);
    bool ok = invokeMonoMethod(monoState->scriptDomain, inspector, instance, &ctx, 0.0f, false, error);
    if (!ok) {
        lastError = error;
    }
    return ok;
}

bool ManagedScriptRuntime::hasInspector(const fs::path& assemblyPath, const std::string& typeName) {
    Module* mod = getModule(assemblyPath, typeName);
    if (!mod) return false;
    if (!mod->inspector.method && !mod->autoInspectorMethod) {
        lastError.clear();
        return false;
    }
    return true;
}

void ManagedScriptRuntime::tickModule(const fs::path& assemblyPath, const std::string& typeName,
                                      ScriptContext& ctx, float deltaTime,
                                      bool runSpec, bool runTest) {
    Module* mod = getModule(assemblyPath, typeName);
    if (!mod) return;

    int objId = ctx.object ? ctx.object->id : -1;
    MonoMethod* begin = reinterpret_cast<MonoMethod*>(mod->begin.method);
    if (objId >= 0 && begin && mod->beginCalledObjects.find(objId) == mod->beginCalledObjects.end()) {
        std::string error;
        MonoObject* instance = mod->begin.isStatic ? nullptr : reinterpret_cast<MonoObject*>(mod->instance);
        if (!invokeMonoMethod(monoState->scriptDomain, begin, instance,
                              &ctx, deltaTime, mod->begin.hasDelta, error)) {
            lastError = error;
            return;
        }
        mod->beginCalledObjects.insert(objId);
    }

    MonoMethod* tickUpdate = reinterpret_cast<MonoMethod*>(mod->tickUpdate.method);
    MonoMethod* update = reinterpret_cast<MonoMethod*>(mod->update.method);
    if (tickUpdate || update) {
        std::string error;
        MonoMethod* method = tickUpdate ? tickUpdate : update;
        bool hasDelta = tickUpdate ? mod->tickUpdate.hasDelta : mod->update.hasDelta;
        MonoObject* instance = (tickUpdate ? mod->tickUpdate.isStatic : mod->update.isStatic)
            ? nullptr
            : reinterpret_cast<MonoObject*>(mod->instance);
        if (!invokeMonoMethod(monoState->scriptDomain, method, instance,
                              &ctx, deltaTime, hasDelta, error)) {
            lastError = error;
            return;
        }
    }

    if (runSpec) {
        MonoMethod* spec = reinterpret_cast<MonoMethod*>(mod->spec.method);
        if (spec) {
            std::string error;
            MonoObject* instance = mod->spec.isStatic ? nullptr : reinterpret_cast<MonoObject*>(mod->instance);
            if (!invokeMonoMethod(monoState->scriptDomain, spec, instance,
                                  &ctx, deltaTime, mod->spec.hasDelta, error)) {
                lastError = error;
                return;
            }
        }
    }
    if (runTest) {
        MonoMethod* test = reinterpret_cast<MonoMethod*>(mod->testEditor.method);
        if (test) {
            std::string error;
            MonoObject* instance = mod->testEditor.isStatic ? nullptr : reinterpret_cast<MonoObject*>(mod->instance);
            if (!invokeMonoMethod(monoState->scriptDomain, test, instance,
                                  &ctx, deltaTime, mod->testEditor.hasDelta, error)) {
                lastError = error;
                return;
            }
        }
    }
}

void ManagedScriptRuntime::unloadAll() {
    if (monoState && monoState->scriptDomain) {
        mono_domain_set(monoState->scriptDomain, false);
        mono_thread_attach(monoState->scriptDomain);
        for (auto& entry : modules) {
            if (entry.second.instanceHandle != 0) {
                mono_gchandle_free(entry.second.instanceHandle);
                entry.second.instanceHandle = 0;
                entry.second.instance = nullptr;
            }
        }
    }
    modules.clear();
    apiInjected = false;
    lastError.clear();
    if (monoState && monoState->rootDomain && monoState->scriptDomain) {
        mono_domain_set(monoState->rootDomain, false);
        mono_domain_unload(monoState->scriptDomain);
        monoState->scriptDomain = nullptr;
    }
}

ManagedScriptRuntime::GcStats ManagedScriptRuntime::getGcStats() const {
    GcStats stats;
    if (!monoState || !monoState->scriptDomain) {
        return stats;
    }

    stats.available = true;
    stats.usedBytes = static_cast<uint64_t>(std::max<int64_t>(0, mono_gc_get_used_size()));
    stats.heapBytes = static_cast<uint64_t>(std::max<int64_t>(0, mono_gc_get_heap_size()));
    for (int generation = 0; generation < 3; ++generation) {
        stats.collectionCounts[generation] = static_cast<uint32_t>(
            std::max(0, mono_gc_collection_count(generation)));
    }
    return stats;
}
#else
ManagedScriptRuntime::~ManagedScriptRuntime() = default;

struct ManagedScriptRuntime::MonoState {};

void ManagedScriptRuntime::MonoStateDeleter::operator()(MonoState* state) const {
    delete state;
}

bool ManagedScriptRuntime::hasInspector(const fs::path& assemblyPath, const std::string& typeName) {
    (void)assemblyPath;
    (void)typeName;
    lastError = "Managed scripts disabled (Mono not built).";
    return false;
}

bool ManagedScriptRuntime::invokeInspector(const fs::path& assemblyPath, const std::string& typeName, ScriptContext& ctx) {
    (void)assemblyPath;
    (void)typeName;
    (void)ctx;
    lastError = "Managed scripts disabled (Mono not built).";
    return false;
}

void ManagedScriptRuntime::tickModule(const fs::path& assemblyPath, const std::string& typeName,
                                      ScriptContext& ctx, float deltaTime, bool runSpec, bool runTest) {
    (void)assemblyPath;
    (void)typeName;
    (void)ctx;
    (void)deltaTime;
    (void)runSpec;
    (void)runTest;
    lastError = "Managed scripts disabled (Mono not built).";
}

void ManagedScriptRuntime::unloadAll() {
    modules.clear();
    monoState.reset();
    apiInjected = false;
    lastError.clear();
}

ManagedScriptRuntime::GcStats ManagedScriptRuntime::getGcStats() const {
    return {};
}
#endif
