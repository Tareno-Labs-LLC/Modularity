#pragma once

#if defined(MODULARITY_HAS_WPE_WEBKIT)
#include <glad/glad.h>
#include <dlfcn.h>
#include <wpe/webkit.h>
#include <wpe/headless/wpe-headless.h>

namespace CodexPluginSetup {

// Resolve WebKit only when opening the companion so its optional dependencies
// cannot prevent the editor executable from starting.
#define MODU_WPE_SYMBOLS(X) \
    X(webkit_image_as_bytes) \
    X(webkit_image_get_height) \
    X(webkit_image_get_stride) \
    X(webkit_image_get_width) \
    X(webkit_navigation_action_get_request) \
    X(webkit_navigation_action_is_user_gesture) \
    X(webkit_navigation_policy_decision_get_navigation_action) \
    X(webkit_policy_decision_ignore) \
    X(webkit_uri_request_get_uri) \
    X(webkit_web_view_get_snapshot) \
    X(webkit_web_view_get_snapshot_finish) \
    X(webkit_web_view_get_type) \
    X(webkit_web_view_get_wpe_view) \
    X(webkit_web_view_is_loading) \
    X(webkit_web_view_load_uri) \
    X(wpe_display_connect) \
    X(wpe_display_headless_new) \
    X(wpe_event_keyboard_new) \
    X(wpe_event_pointer_button_new) \
    X(wpe_event_pointer_move_new) \
    X(wpe_event_scroll_new) \
    X(wpe_event_unref) \
    X(wpe_view_event) \
    X(wpe_view_focus_in) \
    X(wpe_view_focus_out) \
    X(wpe_view_get_has_focus) \
    X(wpe_view_map) \
    X(wpe_view_resized)

struct CodexWebKitApi {
    void* library = nullptr;
    std::string error;
#define MODU_DECLARE_WPE(name) decltype(&::name) name = nullptr;
    MODU_WPE_SYMBOLS(MODU_DECLARE_WPE)
#undef MODU_DECLARE_WPE

    bool Load() {
        if (library) return true;
        // Prefer the WebKit bundled next to the executable, then the working directory, then the system.
        std::error_code ec;
        const auto exeDir = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
        if (!ec) library = dlopen((exeDir / "WPEWebKit/lib/libWPEWebKit-2.0.so.1").c_str(), RTLD_NOW | RTLD_LOCAL);
        ec.clear();
        const auto cwd = std::filesystem::current_path(ec);
        if (!library && !ec) library = dlopen((cwd / "WPEWebKit/lib/libWPEWebKit-2.0.so.1").c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!library) library = dlopen("libWPEWebKit-2.0.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            const char* detail = dlerror();
            error = detail ? detail : "WPE WebKit is unavailable.";
            return false;
        }
#define MODU_LOAD_WPE(name) name = reinterpret_cast<decltype(name)>(dlsym(library, #name));
        MODU_WPE_SYMBOLS(MODU_LOAD_WPE)
#undef MODU_LOAD_WPE
#define MODU_CHECK_WPE(name) if (!name) { error = "WPE WebKit is missing the required symbol " #name; dlclose(library); library = nullptr; return false; }
        MODU_WPE_SYMBOLS(MODU_CHECK_WPE)
#undef MODU_CHECK_WPE
        return true;
    }
};
#undef MODU_WPE_SYMBOLS

inline CodexWebKitApi api;
struct CodexWebView {
    WebKitWebView* page = nullptr;
    WebKitImage* frame = nullptr;
    GLuint texture = 0;
    int width = 0, height = 0, textureWidth = 0, textureHeight = 0;
    bool pending = false;
    bool pointerDown = false;
    double nextFrame = 0.0;
    std::string origin;

    bool Open(const std::string& url) {
        auto* display = api.wpe_display_headless_new();
        const bool connected = api.wpe_display_connect(display, nullptr);
        if (connected) {
            page = reinterpret_cast<WebKitWebView*>(g_object_new(api.webkit_web_view_get_type(), "display", display, nullptr));
        }
        g_object_unref(display);
        if (!page) return false;
        origin = url.substr(0, url.find('/', sizeof("http://") - 1) + 1);
        g_signal_connect(page, "decide-policy", G_CALLBACK(+[](WebKitWebView*, WebKitPolicyDecision* decision,
                          WebKitPolicyDecisionType type, gpointer data) -> gboolean {
            if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) return FALSE;
            const auto* self = static_cast<const CodexWebView*>(data);
            auto* navigation = reinterpret_cast<WebKitNavigationPolicyDecision*>(decision);
            auto* action = api.webkit_navigation_policy_decision_get_navigation_action(navigation);
            const std::string_view uri = api.webkit_uri_request_get_uri(api.webkit_navigation_action_get_request(action));
            if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && uri.starts_with(self->origin)) return FALSE;
            // Keep authentication in the system browser, outside the embedded session.
            if (api.webkit_navigation_action_is_user_gesture(action) &&
                (uri.starts_with("https://auth.openai.com/") || uri.starts_with("https://chatgpt.com/"))) {
                g_app_info_launch_default_for_uri(uri.data(), nullptr, nullptr);
            }
            api.webkit_policy_decision_ignore(decision);
            return TRUE;
        }), this);
        api.wpe_view_map(api.webkit_web_view_get_wpe_view(page));
        api.webkit_web_view_load_uri(page, url.c_str());
        return true;
    }

    void Close() {
        if (page) g_clear_object(&page);
        if (frame) g_clear_object(&frame);
        if (texture && glfwGetCurrentContext()) glDeleteTextures(1, &texture);
        texture = 0;
        width = height = textureWidth = textureHeight = 0;
        pending = false;
        pointerDown = false;
    }

    void Draw() {
        if (!glfwGetCurrentContext()) {
            ImGui::TextDisabled("The Codex web view requires an OpenGL editor session.");
            return;
        }
        for (int i = 0; i < 12 && g_main_context_pending(nullptr); ++i) g_main_context_iteration(nullptr, FALSE);
        auto* view = api.webkit_web_view_get_wpe_view(page);
        const auto space = ImGui::GetContentRegionAvail();
        if (space.x < 2 || space.y < 2) return;
        const int newWidth = std::min(1920, static_cast<int>(space.x));
        const int newHeight = std::min(1200, static_cast<int>(space.y));
        if (newWidth != width || newHeight != height) {
            width = newWidth;
            height = newHeight;
            api.wpe_view_resized(view, width, height);
        }

        if (frame) {
            const int w = api.webkit_image_get_width(frame), h = api.webkit_image_get_height(frame);
            const auto stride = api.webkit_image_get_stride(frame);
            GLint binding = 0, rowLength = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
            glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
            if (!texture) glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(stride / 4));
            const auto* pixels = g_bytes_get_data(api.webkit_image_as_bytes(frame), nullptr);
            if (w != textureWidth || h != textureHeight) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
                textureWidth = w;
                textureHeight = h;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
            }
            glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(binding));
            g_clear_object(&frame);
        }

        const auto originPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("Web content", space);
        if (texture) ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(texture), originPos,
                                                         ImGui::GetItemRectMax());
        const auto& io = ImGui::GetIO();
        const guint32 time = static_cast<guint32>(ImGui::GetTime() * 1000);
        const auto send = [view](WPEEvent* event) {
            if (!event) return;
            api.wpe_view_event(view, event);
            api.wpe_event_unref(event);
        };
        const auto modifiers = static_cast<WPEModifiers>((io.KeyCtrl ? WPE_MODIFIER_KEYBOARD_CONTROL : 0) |
            (io.KeyShift ? WPE_MODIFIER_KEYBOARD_SHIFT : 0) | (io.KeyAlt ? WPE_MODIFIER_KEYBOARD_ALT : 0));
        if (ImGui::IsItemHovered() || pointerDown) {
            const double x = (io.MousePos.x - originPos.x) * width / space.x;
            const double y = (io.MousePos.y - originPos.y) * height / space.y;
            send(api.wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, view, WPE_INPUT_SOURCE_MOUSE, time, modifiers, x, y, 0, 0));
            if (ImGui::IsItemClicked()) {
                pointerDown = true;
                api.wpe_view_focus_in(view);
                send(api.wpe_event_pointer_button_new(WPE_EVENT_POINTER_DOWN, view, WPE_INPUT_SOURCE_MOUSE, time, modifiers, 1, x, y, 1));
            }
            if (pointerDown && !ImGui::IsMouseDown(0)) {
                pointerDown = false;
                send(api.wpe_event_pointer_button_new(WPE_EVENT_POINTER_UP, view, WPE_INPUT_SOURCE_MOUSE, time, modifiers, 1, x, y, 0));
            }
            if (ImGui::IsItemHovered() && (io.MouseWheel || io.MouseWheelH)) {
                send(api.wpe_event_scroll_new(view, WPE_INPUT_SOURCE_MOUSE, time, modifiers,
                                          -io.MouseWheelH * 48, -io.MouseWheel * 48, TRUE, FALSE, x, y));
            }
        }
        if (!ImGui::IsWindowFocused() && api.wpe_view_get_has_focus(view)) api.wpe_view_focus_out(view);
        if (api.wpe_view_get_has_focus(view)) {
            const auto key = [&](guint sym) {
                send(api.wpe_event_keyboard_new(WPE_EVENT_KEYBOARD_KEY_DOWN, view, WPE_INPUT_SOURCE_KEYBOARD, time, modifiers, 0, sym));
                send(api.wpe_event_keyboard_new(WPE_EVENT_KEYBOARD_KEY_UP, view, WPE_INPUT_SOURCE_KEYBOARD, time, modifiers, 0, sym));
            };
            for (ImWchar ch : io.InputQueueCharacters) if (ch >= 32) key(ch <= 255 ? ch : (0x01000000u | ch));
            const std::pair<ImGuiKey, guint> navigation[] = {{ImGuiKey_Backspace, 0xff08}, {ImGuiKey_Tab, 0xff09},
                {ImGuiKey_Enter, 0xff0d}, {ImGuiKey_Delete, 0xffff}, {ImGuiKey_LeftArrow, 0xff51},
                {ImGuiKey_RightArrow, 0xff53}, {ImGuiKey_UpArrow, 0xff52}, {ImGuiKey_DownArrow, 0xff54}};
            for (const auto& [button, symbol] : navigation) if (ImGui::IsKeyPressed(button)) key(symbol);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) key('a');
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) key('v');
        }

        if (!pending && ImGui::GetTime() >= nextFrame && !api.webkit_web_view_is_loading(page)) {
            nextFrame = ImGui::GetTime() + 0.08;
            pending = true;
            api.webkit_web_view_get_snapshot(page, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE,
                nullptr, +[](GObject* object, GAsyncResult* result, gpointer data) {
                    auto& self = *static_cast<CodexWebView*>(data);
                    auto* image = api.webkit_web_view_get_snapshot_finish(reinterpret_cast<WebKitWebView*>(object), result, nullptr);
                    if (self.page == reinterpret_cast<WebKitWebView*>(object)) {
                        self.pending = false;
                        if (self.frame) g_object_unref(self.frame);
                        self.frame = image;
                    } else if (image) g_object_unref(image);
                }, this);
        }
    }
};
}
#endif
