#include "EmbeddedScript.h"
#include <atomic>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

#if MODULARITY_HAS_EMBEDDED_COMPILERS && !MODULARITY_RUNTIME_ONLY
#include <cling/Interpreter/Interpreter.h>
#include <clang/Basic/Diagnostic.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <libtcc.h>
#include <dlfcn.h>
#endif

namespace EmbeddedScript {
namespace fs = std::filesystem;
namespace {
constexpr const char* kMagic = "MODU-EMBEDDED-SCRIPT-1";
constexpr size_t kMaxArtifact = 1024 * 1024;
std::mutex compilerMutex;

std::string encode(const Request& request) {
    std::ostringstream out;
    static std::atomic<unsigned long long> serial{0};
    out << kMagic << '\n' << ++serial << '\n';
    for (const auto& path : {request.cppSource, request.cSource, request.toolchain})
        out << std::quoted(path.generic_string()) << '\n';
    out << request.arguments.size() << '\n';
    for (const auto& value : request.arguments) out << std::quoted(value) << '\n';
    out << request.includes.size() << '\n';
    for (const auto& path : request.includes) out << std::quoted(path.generic_string()) << '\n';
    out << request.defines.size() << '\n';
    for (const auto& value : request.defines) out << std::quoted(value) << '\n';
    return out.str();
}

bool decode(const std::string& data, Request& request) {
    std::istringstream in(data);
    std::string magic, token, value;
    std::getline(in, magic);
    std::getline(in, token);
    if (magic != kMagic || token.empty()) return false;
    if (!(in >> std::quoted(value))) return false;
    request.cppSource = value;
    if (!(in >> std::quoted(value))) return false;
    request.cSource = value;
    if (!(in >> std::quoted(value))) return false;
    request.toolchain = value;
    size_t count = 0;
    if (!(in >> count) || count > 4096) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!(in >> std::quoted(value))) return false;
        request.arguments.push_back(value);
    }
    if (!(in >> count) || count > 4096) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!(in >> std::quoted(value))) return false;
        request.includes.emplace_back(value);
    }
    if (!(in >> count) || count > 4096) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!(in >> std::quoted(value))) return false;
        request.defines.push_back(value);
    }
    return !request.cppSource.empty() && !request.toolchain.empty();
}

#if MODULARITY_HAS_EMBEDDED_COMPILERS && !MODULARITY_RUNTIME_ONLY
struct Diagnostics : clang::DiagnosticConsumer {
    std::string text;
    void HandleDiagnostic(clang::DiagnosticsEngine::Level level, const clang::Diagnostic& info) override {
        clang::DiagnosticConsumer::HandleDiagnostic(level, info);
        llvm::SmallString<256> message;
        info.FormatDiagnostic(message);
        text += (level >= clang::DiagnosticsEngine::Error ? "error: " : "warning: ");
        text.append(message.data(), message.size());
        text += '\n';
    }
};
struct Module {
    // Diagnostics outlives Interpreter, which holds a non-owning pointer to it.
    Diagnostics diagnostics;
    std::unique_ptr<cling::Interpreter> interpreter;
    std::unordered_map<std::string, void*> exports;
};
// Deliberately process-lived: scene objects, callbacks and vtables can retain
// addresses after a reload. Never destroy a published interpreter here.
auto& modules() {
    static auto* retained = new std::unordered_map<std::string, std::unique_ptr<Module>>;
    return *retained;
}
constexpr const char* kExports[] = {
    "Modularity_ScriptAbiVersion", "Modularity_ScriptLayoutSignature",
    "Script_OnInspector", "Script_Begin", "Script_Spec", "Script_TestEditor",
    "Script_Update", "Script_TickUpdate", "RenderEditorWindow", "ExitRenderEditorWindow",
    "Script_ResetState", "Script_OnCollideEnter", "Script_OnCollideHold",
    "Script_OnCollideExit", "Script_OnCollideHoldDuration"
};
std::unique_ptr<Module> build(const Request& request, std::string& log, std::string& error) {
    static const bool initialized = [] {
        return !llvm::InitializeNativeTarget() && !llvm::InitializeNativeTargetAsmPrinter();
    }();
    if (!initialized) { error = "LLVM could not initialize the device JIT target."; return {}; }
    auto module = std::make_unique<Module>();
    std::vector<std::string> args{"modularity-cling", "-fPIC"};
    args.insert(args.end(), request.arguments.begin(), request.arguments.end());
    for (const auto& path : request.includes) { args.emplace_back("-I"); args.push_back(path.string()); }
    for (const auto& value : request.defines) args.push_back("-D" + value);
    std::vector<const char*> argv;
    for (const auto& arg : args) argv.push_back(arg.c_str());
    Dl_info host{};
    void* hostHandle = nullptr;
    if (dladdr(reinterpret_cast<void*>(&Available), &host) && host.dli_fname)
        hostHandle = dlopen(host.dli_fname, RTLD_NOW); // retained with the JIT
    const std::string llvmRoot = (request.toolchain / "cling").string();
    module->interpreter = std::make_unique<cling::Interpreter>(
        static_cast<int>(argv.size()), argv.data(), llvmRoot.c_str(),
        cling::Interpreter::ModuleFileExtensions{}, hostHandle);
    auto& interpreter = *module->interpreter;
    if (!interpreter.getCIOrNull() || !interpreter.getExecutionEngine()) {
        error = "Cling initialization failed; check the bundled target sysroot and Clang resource headers.";
        return {};
    }
    interpreter.replaceDiagnosticConsumer(&module->diagnostics, false);

    const fs::path tccRuntime = request.toolchain / "tinycc/libtcc1.a";
    if (fs::exists(tccRuntime)) {
        auto& jit = *interpreter.getExecutionEngine();
        auto generator = llvm::orc::StaticLibraryDefinitionGenerator::Load(
            jit.getObjLinkingLayer(), tccRuntime.string().c_str());
        if (!generator) {
            error = "Cannot load TinyCC runtime helpers: " + llvm::toString(generator.takeError()); return {};
        }
        jit.getMainJITDylib().addGenerator(std::move(*generator));
    }

    if (!request.cSource.empty()) {
        std::unique_ptr<TCCState, decltype(&tcc_delete)> tcc(tcc_new(), tcc_delete);
        if (!tcc) { error = "TinyCC could not allocate a compilation context."; return {}; }
        tcc_set_error_func(tcc.get(), &log, [](void* context, const char* text) {
            *static_cast<std::string*>(context) += std::string(text) + '\n';
        });
        const std::string runtime = (request.toolchain / "tinycc").string();
        tcc_set_lib_path(tcc.get(), runtime.c_str());
        tcc_set_options(tcc.get(), "-nostdlib -std=c11 -fPIC");
        if (tcc_set_output_type(tcc.get(), TCC_OUTPUT_OBJ) < 0) {
            error = "TinyCC could not select relocatable object output."; return {};
        }
        for (const auto& path : request.includes) tcc_add_include_path(tcc.get(), path.string().c_str());
        for (const auto& path : {request.toolchain / "sysroot/usr/include",
                                 request.toolchain / "sysroot/usr/include/aarch64-linux-android"})
            tcc_add_sysinclude_path(tcc.get(), path.string().c_str());
        tcc_define_symbol(tcc.get(), "__ANDROID_API__", "26");
        for (const auto& def : request.defines) {
            const auto pos = def.find('=');
            tcc_define_symbol(tcc.get(), def.substr(0, pos).c_str(),
                              pos == std::string::npos ? "1" : def.substr(pos + 1).c_str());
        }
        fs::path object = request.cSource;
        object += ".tinycc.o";
        if (tcc_add_file(tcc.get(), request.cSource.string().c_str()) < 0 ||
            tcc_output_file(tcc.get(), object.string().c_str()) < 0) {
            error = "TinyCC C compilation failed."; return {};
        }
        auto buffer = llvm::MemoryBuffer::getFile(object.string());
        std::error_code ec;
        fs::remove(object, ec);
        if (!buffer) { error = "Unable to read TinyCC object: " + buffer.getError().message(); return {}; }
        if (auto failure = interpreter.getExecutionEngine()->addObjectFile(std::move(*buffer))) {
            error = "Cling could not link the C object: " + llvm::toString(std::move(failure)); return {};
        }
    }
    if (interpreter.loadHeader(request.cppSource.string()) != cling::Interpreter::kSuccess) {
        log += module->diagnostics.text;
        error = "Cling C++ compilation failed."; return {};
    }
    for (const char* name : kExports) {
        if (void* address = interpreter.getAddressOfGlobal(name)) module->exports.emplace(name, address);
    }
    log += module->diagnostics.text;
    if (!module->exports.count(kExports[0])) { error = "Embedded script is missing its ABI export."; return {}; }
    log += request.cSource.empty() ? "Compiled C++ with embedded Cling.\n"
                                  : "Compiled C with TinyCC and its C++ context bridge with Cling.\n";
    return module;
}
#endif
}

bool Available() {
#if MODULARITY_HAS_EMBEDDED_COMPILERS && !MODULARITY_RUNTIME_ONLY
    return true;
#else
    return false;
#endif
}
bool Compile(const Request& request, const fs::path& artifact, std::string& log, std::string& error) {
#if MODULARITY_HAS_EMBEDDED_COMPILERS && !MODULARITY_RUNTIME_ONLY
    std::lock_guard<std::mutex> lock(compilerMutex);
    auto module = build(request, log, error);
    if (!module) return false;
    const std::string data = encode(request);
    if (data.size() > kMaxArtifact) { error = "Embedded compilation request is too large."; return false; }
    std::error_code ec;
    fs::create_directories(artifact.parent_path(), ec);
    std::ofstream out(artifact, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) { error = "Unable to write embedded script artifact: " + artifact.string(); return false; }
    modules().emplace(data, std::move(module));
    return true;
#else
    (void)request; (void)artifact; (void)log;
    error = "This editor was built without embedded Cling/TinyCC support.";
    return false;
#endif
}
LoadResult Load(const fs::path& artifact, void*& module, std::string& error) {
    module = nullptr;
    std::ifstream in(artifact, std::ios::binary);
    char prefix[sizeof("MODU-EMBEDDED-SCRIPT-1")]{};
    in.read(prefix, sizeof(prefix));
    if (std::string(prefix, static_cast<size_t>(in.gcount())) != std::string(kMagic) + '\n')
        return LoadResult::NativeLibrary;
    std::error_code ec;
    if (fs::file_size(artifact, ec) > kMaxArtifact || ec) {
        error = "Invalid embedded script artifact size."; return LoadResult::Failed;
    }
    in.clear(); in.seekg(0);
    std::ostringstream buffer; buffer << in.rdbuf();
    const std::string data = buffer.str();
    Request request;
    if (!decode(data, request)) { error = "Invalid embedded script artifact."; return LoadResult::Failed; }
#if MODULARITY_HAS_EMBEDDED_COMPILERS && !MODULARITY_RUNTIME_ONLY
    std::lock_guard<std::mutex> lock(compilerMutex);
    auto found = modules().find(data);
    if (found == modules().end()) {
        std::string log;
        auto built = build(request, log, error);
        if (!built) { error += "\n" + log; return LoadResult::Failed; }
        found = modules().emplace(data, std::move(built)).first;
    }
    module = found->second.get();
    return LoadResult::Loaded;
#else
    error = "This script needs the embedded Cling/TinyCC editor backend. Recompile it with the native compiler.";
    return LoadResult::Failed;
#endif
}
void* Symbol(void* module, const char* name) {
#if MODULARITY_HAS_EMBEDDED_COMPILERS && !MODULARITY_RUNTIME_ONLY
    if (!module) return nullptr;
    const auto& exports = static_cast<Module*>(module)->exports;
    auto it = exports.find(name);
    return it == exports.end() ? nullptr : it->second;
#else
    (void)module; (void)name;
    return nullptr;
#endif
}
}
