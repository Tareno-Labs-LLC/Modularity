#include "ProjectManager.h"
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const fs::path root = fs::absolute(argv[1]);
    Project project;
    if (!project.load(root / "project.modu")) {
        std::fprintf(stderr, "Could not load exported project\n"); return 1;
    }
    if (project.pipeline != ProjectPipeline::Pipeline2D || project.currentSceneName != "Main" ||
        !project.physicsSettings.enable2DPhysics || !project.playerSettings.cursorVisible ||
        project.name.find('\r') != std::string::npos) {
        std::fprintf(stderr, "Windows-authored project parsed incorrectly: pipeline=%s scene='%s' physics2D=%d cursorVisible=%d\n",
                     ProjectPipelineLabel(project.pipeline), project.currentSceneName.c_str(),
                     project.physicsSettings.enable2DPhysics, project.playerSettings.cursorVisible);
        return 1;
    }
    std::vector<SceneObject> objects;
    int nextId = 0, version = 0;
    if (!SceneSerializer::loadSceneDeferred(project.getSceneFilePath(project.currentSceneName), objects, nextId, version) || objects.size() != 61) {
        std::fprintf(stderr, "Exported Main scene failed to load: %zu objects\n", objects.size()); return 1;
    }
    std::puts("[PASS] exported Tenna CRLF project loads as 2D, retains boolean settings, and opens Main with 61 objects");
    return 0;
}
