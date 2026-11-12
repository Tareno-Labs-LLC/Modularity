// Compile/link the real backend-prepared shader on an attached Android GPU.
// Unused Shader methods are removed with -ffunction-sections / --gc-sections.
#include "Shaders/Shader_Manager/Shader.cpp"
#include <EGL/egl.h>
#include <cstdio>
#include <iterator>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(display, nullptr, nullptr)) { std::puts("eglInitialize failed"); return 1; }
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(display, attributes, &config, 1, &count) || !count) return 1;
    const EGLint size[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, size);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    if (!eglMakeCurrent(display, surface, surface, context)) { std::puts("eglMakeCurrent failed"); return 1; }
    std::printf("GPU: %s\n", glGetString(GL_RENDERER));
    GLuint shaders[2]{};
    bool success = true;
    for (int i = 0; i < 2; ++i) {
        std::ifstream input(argv[i + 1], std::ios::binary);
        if (!input) return 1;
        const std::string raw{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const std::string prepared = prepareShaderSourceForBackend(raw);
        const char* text = prepared.c_str();
        shaders[i] = glCreateShader(i == 0 ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
        glShaderSource(shaders[i], 1, &text, nullptr);
        glCompileShader(shaders[i]);
        GLint ok = 0;
        glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
        char log[16384]{};
        glGetShaderInfoLog(shaders[i], sizeof(log), nullptr, log);
        std::printf("%s: %s\n%s\n", argv[i + 1], ok ? "PASS" : "FAIL", log);
        success = success && ok;
    }
    GLuint program = glCreateProgram();
    for (GLuint shader : shaders) glAttachShader(program, shader);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    char log[16384]{};
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    std::printf("Link: %s\n%s\n", linked ? "PASS" : "FAIL", log);
    glDeleteProgram(program);
    for (GLuint shader : shaders) glDeleteShader(shader);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    return success && linked ? 0 : 1;
}
