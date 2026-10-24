#include "Engine.h"
#include "ScriptContextBinding.h"
#include <cstdio>
#include <fstream>

static int failures = 0;
static int checks = 0;
static void check(bool pass, const char* name) {
    ++checks;
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
    std::fflush(stdout);
    if (!pass) ++failures;
}
static bool waitProcess(Engine& engine, int id, std::string& output, bool& success, int milliseconds = 5000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < end) {
        std::string chunk; bool done = false; int exitCode = -1;
        if (!engine.pollProcessFromScript(id, chunk, done, success, exitCode)) return false;
        output += chunk;
        if (done) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    engine.cancelProcessFromScript(id);
    return false;
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    ImGui::CreateContext();
    {
        auto engine = std::make_unique<Engine>();
        ScriptContext ctx;
        ctx.engine = engine.get();
        int objectId = engine->createSceneObjectFromScript("Empty", "Scripted", -1);
        ctx.object = engine->findObjectById(objectId);
        ctx.object->scripts.emplace_back();
        ctx.script = &ctx.object->scripts.front();
        ctx.script->path = "original.moducpp";
        {
            ScriptContextBinding binding(ctx);
            bool moved = false;
            for (int i = 0; i < 24; ++i) {
                auto* old = ctx.object;
                ctx.CreateSceneObject("Empty", "New", -1);
                moved |= old != ctx.object;
                check(ctx.object == engine->findObjectById(objectId) && ctx.script == &ctx.object->scripts[0],
                      "production create rebinds active context");
            }
            check(moved, "creation test exercised scene vector reallocation");
            std::ofstream("attached.cpp") << "// attachment fixture\n";
            auto* beforeAttach = ctx.script;
            check(ctx.AttachObjectScript(objectId, fs::absolute("attached.cpp").string()), "attach script through production API");
            check(ctx.script == &ctx.object->scripts[0] && ctx.script != beforeAttach, "production attach refreshes script storage");
            check(ctx.SetSceneObjectParent(objectId, -1), "reparent current object to root");
            check(ctx.object == engine->findObjectById(objectId) && ctx.object->id == objectId, "root reorder preserves context identity");
            check(ctx.DeleteSceneObject(objectId), "delete current object");
            check(!ctx.object && !ctx.script, "production self-deletion clears context immediately");
        }
        ScriptRuntime runtime;
        auto runScript = [&](const char* name) {
            const int id = engine->createSceneObjectFromScript("Empty", name, -1);
            ScriptContext call;
            call.engine = engine.get();
            call.object = engine->findObjectById(id);
            call.object->scripts.emplace_back();
            call.script = &call.object->scripts.back();
            call.script->path = argv[2];
            runtime.tickModule(argv[2], call, 0.016f, false, false);
            check(runtime.getLastError().empty(), "native regression fixture loads with current ABI");
            return id;
        };
        const int grownId = runScript("GrowInBegin");
        check(engine->findObjectById(grownId)->position == glm::vec3(7), "Update uses refreshed context after Begin reallocates scene");
        const int deletedId = runScript("DeleteInBegin");
        check(!engine->findObjectById(deletedId) && !engine->findObjectByName("UnexpectedUpdate"), "self-deletion in Begin prevents Update dispatch");
        const int taskId = runScript("DeleteInTask");
        check(!engine->findObjectById(taskId) && !engine->findObjectByName("UnexpectedTask"), "self-deleting IEnum prevents later copied tasks from dispatching");

        const std::string base = argv[1];
        for (bool stream : {false, true}) {
            for (const auto& endpoint : {std::string("/ok"), std::string("/error"), std::string("/empty")}) {
                int request = engine->startHttpPostFromScript(base + endpoint, "text/plain", "body", "", stream);
                const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                bool done = false, success = false;
                std::string response;
                while (!done && std::chrono::steady_clock::now() < end) {
                    std::string chunk;
                    engine->pollHttpPostFromScript(request, chunk, done, success);
                    response += chunk;
                    if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                check(done && success == (endpoint != "/error"), "HTTP completion reflects status independently of response body");
                if (endpoint == "/ok") check(response == "failed cancelled HTTP 500", "valid body is delivered unchanged");
            }
        }
        int invalid = engine->startHttpPostFromScript("not-a-url", "", "", "", false);
        bool done = false, success = true;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!done && std::chrono::steady_clock::now() < end) {
            std::string chunk;
            engine->pollHttpPostFromScript(invalid, chunk, done, success);
            if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(done && !success, "HTTP transport failure is reported as failure");

        std::string output; bool processSuccess = false;
        check(waitProcess(*engine, engine->startProcessFromScript("echo process-ok", "", false), output, processSuccess) &&
              processSuccess && output.find("process-ok") != std::string::npos, "Windows process captures output and successful exit");
        int exitCode = 0;
        output = engine->runProcessBlockingFromScript("cmd /c exit 7", "", 5000, &exitCode);
        check(exitCode == 7, "Windows process preserves nonzero exit code");
        fs::remove("child-completed.txt");
        int child = engine->startProcessFromScript("call .\\child-test.cmd", "", false);
        const auto readyEnd = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!fs::exists("child-ready.txt") && std::chrono::steady_clock::now() < readyEnd)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!fs::exists("child-ready.txt")) {
            std::string diagnostic; bool childDone = false, childOk = false; int childCode = -1;
            engine->pollProcessFromScript(child, diagnostic, childDone, childOk, childCode);
            std::printf("Child diagnostic: cwd=%s done=%d exit=%d %s\n", fs::current_path().string().c_str(), childDone, childCode, diagnostic.c_str());
        }
        check(fs::exists("child-ready.txt"), "cancellation test starts descendant process");
        engine->cancelProcessFromScript(child);
        output.clear(); processSuccess = true;
        check(waitProcess(*engine, child, output, processSuccess) && !processSuccess, "cancelling Windows process finishes and reports failure");
        std::this_thread::sleep_for(std::chrono::milliseconds(2200));
        check(!fs::exists("child-completed.txt"), "Windows cancellation terminates descendant process tree");
        fs::remove("child-ready.txt");
        output = engine->runProcessBlockingFromScript("call .\\child-test.cmd", "", 100, &exitCode);
        std::printf("Timeout diagnostic: exit=%d %s\n", exitCode, output.c_str());
        check(output.find("process terminated]") != std::string::npos && exitCode != 0, "blocking timeout confirms termination before reporting it");
    }
    ImGui::DestroyContext();
    std::printf("Engine regressions: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
