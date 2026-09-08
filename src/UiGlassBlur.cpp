#include "UiGlassBlur.h"
#include "Common.h"
#include <cstdlib>

namespace Modularity {
namespace UiGlassBlur {
namespace {

GLuint texA = 0;
GLuint texB = 0;
GLuint fboA = 0;
GLuint fboB = 0;
GLuint blurProgram = 0;
GLuint emptyVao = 0;
GLint dirUniform = -1;
int targetW = 0;
int targetH = 0;
bool installed = false;
bool failed = false;

constexpr int kDownscale = 4;

const char* VertexSrc()
{
#if MODULARITY_OPENGL_ES
    return "#version 300 es\n"
#else
    return "#version 330\n"
#endif
           "out vec2 vUV;\n"
           "void main(){\n"
           "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
           "    vUV = p;\n"
           "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
           "}\n";
}

const char* FragmentSrc()
{
#if MODULARITY_OPENGL_ES
    return "#version 300 es\n"
           "precision mediump float;\n"
#else
    return "#version 330\n"
#endif
           "in vec2 vUV;\n"
           "out vec4 oColor;\n"
           "uniform sampler2D uTex;\n"
           "uniform vec2 uDir;\n"
           "void main(){\n"
           "    vec3 c = texture(uTex, vUV).rgb * 0.227027;\n"
           "    c += (texture(uTex, vUV + uDir * 1.0).rgb + texture(uTex, vUV - uDir * 1.0).rgb) * 0.1945946;\n"
           "    c += (texture(uTex, vUV + uDir * 2.0).rgb + texture(uTex, vUV - uDir * 2.0).rgb) * 0.1216216;\n"
           "    c += (texture(uTex, vUV + uDir * 3.0).rgb + texture(uTex, vUV - uDir * 3.0).rgb) * 0.054054;\n"
           "    c += (texture(uTex, vUV + uDir * 4.0).rgb + texture(uTex, vUV - uDir * 4.0).rgb) * 0.016216;\n"
           "    oColor = vec4(c, 1.0);\n"
           "}\n";
}

void GiveUp(const char* why)
{
    std::cerr << "[WARN] UiGlassBlur disabled: " << why << std::endl;
    failed = true;
    if (ImGui::GetCurrentContext())
        ImGui::SetGlassBlurRenderer(nullptr, nullptr, 0);
}

GLuint CompileStage(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[512] = {0};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::cerr << "[WARN] UiGlassBlur shader compile: " << log << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool EnsureProgram()
{
    if (blurProgram != 0)
        return true;
    GLuint vs = CompileStage(GL_VERTEX_SHADER, VertexSrc());
    GLuint fs = CompileStage(GL_FRAGMENT_SHADER, FragmentSrc());
    if (vs == 0 || fs == 0) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    blurProgram = glCreateProgram();
    glAttachShader(blurProgram, vs);
    glAttachShader(blurProgram, fs);
    glLinkProgram(blurProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(blurProgram, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteProgram(blurProgram);
        blurProgram = 0;
        return false;
    }
    glUseProgram(blurProgram);
    glUniform1i(glGetUniformLocation(blurProgram, "uTex"), 0);
    dirUniform = glGetUniformLocation(blurProgram, "uDir");
    if (emptyVao == 0)
        glGenVertexArrays(1, &emptyVao);
    return true;
}

void AllocTexture(GLuint tex, int w, int h)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

bool EnsureTargets(int w, int h)
{
    if (w == targetW && h == targetH && fboA != 0)
        return true;
    if (texB == 0)
        glGenTextures(1, &texB);
    if (fboA == 0)
        glGenFramebuffers(1, &fboA);
    if (fboB == 0)
        glGenFramebuffers(1, &fboB);
    AllocTexture(texA, w, h);
    AllocTexture(texB, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, fboA);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texA, 0);
    const bool okA = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, fboB);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texB, 0);
    const bool okB = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!okA || !okB)
        return false;
    targetW = w;
    targetH = h;
    return true;
}

void BlurPass(GLuint srcTex, GLuint dstFbo, float dirX, float dirY)
{
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glUniform2f(dirUniform, dirX, dirY);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void CaptureCallback(const ImDrawList*, const ImDrawCmd*)
{
    if (failed)
        return;

    GLint prevDrawFbo = 0;
    GLint prevReadFbo = 0;
    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    const int fbW = viewport[2];
    const int fbH = viewport[3];
    if (fbW < kDownscale || fbH < kDownscale)
        return;

    if (!EnsureProgram()) {
        GiveUp("blur shader failed to build");
        return;
    }
    const int w = fbW / kDownscale;
    const int h = fbH / kDownscale;
    if (!EnsureTargets(w, h)) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDrawFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevReadFbo);
        GiveUp("blur framebuffers incomplete");
        return;
    }

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevDrawFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboA);
    glBlitFramebuffer(0, 0, fbW, fbH, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    glUseProgram(blurProgram);
    glBindVertexArray(emptyVao);
    glActiveTexture(GL_TEXTURE0);
    glViewport(0, 0, w, h);
    const float tx = 1.0f / (float)w;
    const float ty = 1.0f / (float)h;
    BlurPass(texA, fboB, tx * 1.2f, 0.0f);
    BlurPass(texB, fboA, 0.0f, ty * 1.2f);
    BlurPass(texA, fboB, tx * 2.4f, 0.0f);
    BlurPass(texB, fboA, 0.0f, ty * 2.4f);

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDrawFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevReadFbo);
}

} // namespace

void Install()
{
    if (installed || failed)
        return;
    if (const char* off = std::getenv("MODULARITY_DISABLE_GLASS_BLUR")) {
        if (*off && *off != '0') {
            failed = true;
            return;
        }
    }
    if (texA == 0) {
        glGenTextures(1, &texA);
        AllocTexture(texA, 1, 1);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    ImGui::SetGlassBlurRenderer(&CaptureCallback, nullptr, (ImTextureID)(intptr_t)texA);
    installed = true;
}

bool EmitBackdropCapture(ImDrawList* drawList)
{
    if (drawList == nullptr || !installed || failed || texA == 0)
        return false;
    drawList->AddCallback(&CaptureCallback, nullptr);
    drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    return true;
}

ImTextureID BackdropTexture()
{
    if (!installed || failed)
        return 0;
    return (ImTextureID)(intptr_t)texA;
}

void Shutdown()
{
    if (ImGui::GetCurrentContext())
        ImGui::SetGlassBlurRenderer(nullptr, nullptr, 0);
    if (texA) glDeleteTextures(1, &texA);
    if (texB) glDeleteTextures(1, &texB);
    if (fboA) glDeleteFramebuffers(1, &fboA);
    if (fboB) glDeleteFramebuffers(1, &fboB);
    if (blurProgram) glDeleteProgram(blurProgram);
    if (emptyVao) glDeleteVertexArrays(1, &emptyVao);
    texA = texB = fboA = fboB = blurProgram = emptyVao = 0;
    dirUniform = -1;
    targetW = targetH = 0;
    installed = false;
    failed = false;
}

} // namespace UiGlassBlur
} // namespace Modularity
