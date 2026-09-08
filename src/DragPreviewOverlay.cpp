#include "DragPreviewOverlay.h"
#include <cmath>
#include <string>
namespace DragPreview {
    namespace {
        struct State {
            bool        active               = false;
            bool        dropping             = false;
            std::string displayName;
            std::string payloadType;
            ImTextureID icon                 = (ImTextureID)0;
            ImVec2      iconUv0                {0.0f, 1.0f};
            ImVec2      iconUv1                {1.0f, 0.0f};
            ImVec2      currentPos             {0.0f, 0.0f};
            ImVec2      velocity               {0.0f, 0.0f};
            ImVec2      lastTargetPos          {0.0f, 0.0f};
            ImVec2      cursorVelocity         {0.0f, 0.0f};
            float       rotation                = 0.0f;
            float       angularVelocity         = 0.0f;
            float       scale                   = 1.0f;
            float       targetScale             = 1.0f;
            float       opacity                 = 0.0f;
            int         lastCaptureFrame        = -2;
            float       dropTimer               = 0.0f;
            float       dropFallVelY            = 0.0f;
            float       dropRotationVel         = 0.0f;
            double      floatTimeAccum         = 0.0;
        };
        State& Get()       { static State    s;      return s;   }
        Settings& SettRef(){ static Settings cfg;    return cfg; }
        float ExpSmooth(float current, float target, float rate, float dt) {
            if (rate <= 0.0f) return target;
            const float a = 1.0f - std::exp(-rate * dt);
            return current + (target - current) * a;
        }
        ImU32 ApplyAlpha(ImU32 col, float mult) { // why in the FUCK do i have to make Alpha like this...
            const float baseA = static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFFu);
            int a = static_cast<int>(baseA * mult);
            if (a < 0) a = 0; else if (a > 255) a = 255;
            return (col & ~(0xFFu << IM_COL32_A_SHIFT)) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
        }
        void HardReset() {
            State& s = Get();
            s.active = false;
            s.dropping = false;
            s.displayName.clear();
            s.payloadType.clear();
            s.icon = (ImTextureID)0;
            s.iconUv0 = ImVec2(0.0f, 1.0f);
            s.iconUv1 = ImVec2(1.0f, 0.0f);
            s.velocity = ImVec2(0.0f, 0.0f);
            s.lastTargetPos = ImVec2(0.0f, 0.0f);
            s.cursorVelocity = ImVec2(0.0f, 0.0f);
            s.rotation = 0.0f;
            s.angularVelocity = 0.0f;
            s.scale = 1.0f;
            s.targetScale = 1.0f;
            s.opacity = 0.0f;
            s.lastCaptureFrame = -2;
            s.dropTimer = 0.0f;
            s.dropFallVelY = 0.0f;
            s.dropRotationVel = 0.0f;
            s.floatTimeAccum = 0.0;
        }
        void StartOrRefresh(const char* name, ImTextureID icon, const char* type,
                            ImVec2 iconUv0, ImVec2 iconUv1) {
            State& s = Get();
            const Settings& cfg = SettRef();
            const bool restartingFromIdle = (!s.active || s.dropping);
            if (restartingFromIdle) {
                const ImVec2 mouse = ImGui::GetMousePos();
                const ImVec2 finalTarget(mouse.x + cfg.cursorOffsetX, mouse.y + cfg.cursorOffsetY);
                // Pop-in: spawn stuff.
                s.currentPos = ImVec2(finalTarget.x, finalTarget.y - cfg.popInRiseDistance);
                s.lastTargetPos = finalTarget;
                s.velocity = ImVec2(0.0f, 60.0f);
                s.cursorVelocity = ImVec2(0.0f, 0.0f);
                s.rotation = 0.0f;
                s.angularVelocity = 0.0f;
                s.scale = cfg.popInStartScale;
                s.targetScale = 1.0f;
                s.opacity = 0.0f;
                s.dropTimer = 0.0f;
                s.dropFallVelY = 0.0f;
                s.dropRotationVel = 0.0f;
                s.floatTimeAccum = 0.0;
            }
            s.active = true;
            s.dropping = false;
            s.icon = icon;
            s.iconUv0 = iconUv0;
            s.iconUv1 = iconUv1;
            const char* safeName = (name && *name) ? name : "(unnamed)";
            if (s.displayName != safeName) s.displayName.assign(safeName);
            if (type && *type) {if (s.payloadType != type) s.payloadType.assign(type);}
            else if (!s.payloadType.empty()) s.payloadType.clear();
            s.lastCaptureFrame = ImGui::GetFrameCount();
        }
    }
    Settings& GetSettings() { return SettRef(); }
    bool BeginSource(ImGuiDragDropFlags userFlags) {
        if (SettRef().enabled) {userFlags |= ImGuiDragDropFlags_SourceNoPreviewTooltip;}
        return ImGui::BeginDragDropSource(userFlags);
    }
    void EndSource() {ImGui::EndDragDropSource();}
    void SubmitMeta(const char* displayName, ImTextureID icon, const char* payloadType,
                    ImVec2 iconUv0, ImVec2 iconUv1) {
        const Settings& cfg = SettRef();
        if (cfg.enabled) {StartOrRefresh(displayName, icon, payloadType, iconUv0, iconUv1); return;}
        // (Thanks AI for helping me explain this monstrosity of WHAT THE HELL DOES 'displayName && *displayName' MEAN???)
        if (displayName && *displayName) {ImGui::TextUnformatted(displayName);}
    }
    void Cancel() {HardReset();}
    void UpdateAndRender() {
        State& s = Get();
        const Settings& cfg = SettRef();
        if (!cfg.enabled) {if (s.active) HardReset(); return;}
        if (!s.active) return;
        ImGuiIO& io = ImGui::GetIO();
        const float dt = ImClamp(io.DeltaTime, 1.0f / 240.0f, 1.0f / 30.0f);
        const int frame = ImGui::GetFrameCount();
        const bool payloadAlive = (ImGui::GetDragDropPayload() != nullptr);
        const bool capturedThisFrame = (s.lastCaptureFrame == frame);
        if (!s.dropping && !payloadAlive && !capturedThisFrame) { // Begin drop animation.
            s.dropping = true;
            s.dropTimer = 0.0f;
            const float baseFall = cfg.dropFallDistance / std::max(0.001f, cfg.dropFadeDuration);
            s.dropFallVelY = std::max(s.velocity.y * 0.5f, 0.0f) + baseFall * 0.55f;
            const float dir = (s.velocity.x >= 0.0f) ? 1.0f : -1.0f;
            s.dropRotationVel = dir * cfg.dropExtraRotationDeg / std::max(0.001f, cfg.dropFadeDuration);
            s.targetScale = cfg.dropEndScale;
        }
        if (!s.dropping) {const ImVec2 mouse = ImGui::GetMousePos(); ImVec2 targetPos(mouse.x + cfg.cursorOffsetX, mouse.y + cfg.cursorOffsetY);
            // Idle micro-float drift stuff.
            s.floatTimeAccum += static_cast<double>(dt);
            const float ft = static_cast<float>(s.floatTimeAccum);
            targetPos.x += std::sin(ft * cfg.floatFrequency)         * cfg.floatAmplitude;
            targetPos.y += std::cos(ft * cfg.floatFrequency * 0.9f)  * cfg.floatAmplitude;
            // Cursor velocity stuff.
            const ImVec2 rawCursorVel((targetPos.x - s.lastTargetPos.x) / dt, (targetPos.y - s.lastTargetPos.y) / dt);
            s.lastTargetPos = targetPos;
            const float cursorBlend = 1.0f - std::exp(-25.0f * dt);
            s.cursorVelocity.x += (rawCursorVel.x - s.cursorVelocity.x) * cursorBlend;
            s.cursorVelocity.y += (rawCursorVel.y - s.cursorVelocity.y) * cursorBlend;
            // Position spring-damper stuff.
            const ImVec2 toTarget(targetPos.x - s.currentPos.x, targetPos.y - s.currentPos.y);
            const ImVec2 accel(toTarget.x * cfg.followStiffness - s.velocity.x * cfg.followDamping, toTarget.y * cfg.followStiffness - s.velocity.y * cfg.followDamping);
            s.velocity.x += accel.x * dt;
            s.velocity.y += accel.y * dt;
            s.currentPos.x += s.velocity.x * dt;
            s.currentPos.y += s.velocity.y * dt;
            float targetRotDeg = s.cursorVelocity.x * cfg.rotationDrive;
            targetRotDeg = ImClamp(targetRotDeg, -cfg.maxRotationDeg, cfg.maxRotationDeg);
            const float angAccel = (targetRotDeg - s.rotation) * cfg.rotationStiffness - s.angularVelocity * cfg.rotationDamping;
            s.angularVelocity += angAccel * dt;
            s.rotation += s.angularVelocity * dt;
            if (s.rotation > cfg.maxRotationDeg)  { s.rotation = cfg.maxRotationDeg;  if (s.angularVelocity > 0.0f) s.angularVelocity = 0.0f; }
            if (s.rotation < -cfg.maxRotationDeg) { s.rotation = -cfg.maxRotationDeg; if (s.angularVelocity < 0.0f) s.angularVelocity = 0.0f; }
            // Make the scale respond well to the overall motion magnitude.
            const float speed = std::sqrt(s.velocity.x * s.velocity.x + s.velocity.y * s.velocity.y);
            const float speedNorm = ImClamp(speed * 0.00012f, 0.0f, 1.0f);
            s.targetScale = 1.0f + speedNorm * cfg.scaleResponse;
            s.scale = ExpSmooth(s.scale, s.targetScale, cfg.scaleSmoothing, dt);
            s.opacity = ExpSmooth(s.opacity, cfg.opacity, cfg.popInFadeRate, dt);
        } else {
            s.dropTimer += dt;
            const float t = ImClamp(s.dropTimer / std::max(0.001f, cfg.dropFadeDuration), 0.0f, 1.0f);
            s.currentPos.x += s.velocity.x * dt * 0.4f;
            s.currentPos.y += s.dropFallVelY * dt;
            s.dropFallVelY += cfg.dropGravity * dt;
            s.rotation += s.dropRotationVel * dt;
            const float rotLimit = cfg.maxRotationDeg + cfg.dropExtraRotationDeg;
            s.rotation = ImClamp(s.rotation, -rotLimit, rotLimit);
            s.scale = ExpSmooth(s.scale, cfg.dropEndScale, 8.0f, dt);
            const float fade = 1.0f - (t * t * (3.0f - 2.0f * t));
            s.opacity = cfg.opacity * fade;
            if (t >= 1.0f) {HardReset(); return;}
        }
        if (s.displayName.empty() || s.opacity <= 0.005f) return;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        if (!dl) return;
        const bool hasIcon = (s.icon != (ImTextureID)0);
        const float iconSz = hasIcon ? cfg.iconSize : 0.0f;
        const float iconGap = hasIcon ? cfg.iconGap : 0.0f;
        const ImVec2 textSz = ImGui::CalcTextSize(s.displayName.c_str());
        const float innerH = (textSz.y > iconSz) ? textSz.y : iconSz;
        const float innerW = textSz.x + iconSz + iconGap;
        const float cardW = innerW + cfg.cardPaddingX * 2.0f;
        const float cardH = innerH + cfg.cardPaddingY * 2.0f;
        // currentPos is the pendulum pivot for the dangling thingy. (Remember that future self!)
        const ImVec2 pivot = s.currentPos;
        const float scl = s.scale;
        const float angle = s.rotation * (3.14159265f / 180.0f);
        const float ca = std::cos(angle);
        const float sa = std::sin(angle);
        auto rotate = [&](float lx, float ly) { return ImVec2(pivot.x + (lx * ca - ly * sa) * scl, pivot.y + (lx * sa + ly * ca) * scl);};
        const float hw = cardW * 0.5f;
        const ImVec2 p0 = rotate(-hw, 0.0f);    // top-left
        const ImVec2 p1 = rotate( hw, 0.0f);    // top-right
        const ImVec2 p2 = rotate( hw, cardH);   // bottom-right
        const ImVec2 p3 = rotate(-hw, cardH);   // bottom-left
        const float shadow = cfg.shadowOffset;
        const ImVec2 sh0(p0.x, p0.y + shadow);
        const ImVec2 sh1(p1.x, p1.y + shadow);
        const ImVec2 sh2(p2.x, p2.y + shadow);
        const ImVec2 sh3(p3.x, p3.y + shadow);
        dl->AddQuadFilled(sh0, sh1, sh2, sh3, ApplyAlpha(cfg.cardShadowColor, s.opacity));
        // Card body + thin accent border.
        dl->AddQuadFilled(p0, p1, p2, p3, ApplyAlpha(cfg.cardColor,       s.opacity));
        dl->AddQuad      (p0, p1, p2, p3, ApplyAlpha(cfg.cardBorderColor, s.opacity), 1.0f);
        const float cardMidY = cardH * 0.5f;
        float contentLeft = -innerW * 0.5f;
        if (hasIcon) {
            const float ix = contentLeft;
            const float iy = cardMidY - iconSz * 0.5f;
            const ImVec2 i0 = rotate(ix,            iy);
            const ImVec2 i1 = rotate(ix + iconSz,   iy);
            const ImVec2 i2 = rotate(ix + iconSz,   iy + iconSz);
            const ImVec2 i3 = rotate(ix,            iy + iconSz);
            dl->AddImageQuad(s.icon, i0, i1, i2, i3,
                             s.iconUv0,
                             ImVec2(s.iconUv1.x, s.iconUv0.y),
                             s.iconUv1,
                             ImVec2(s.iconUv0.x, s.iconUv1.y),
                             ApplyAlpha(IM_COL32_WHITE, s.opacity));
            contentLeft += iconSz + iconGap;
        }
        ImFont* font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize();
        if (font && fontSize > 0.0f) {
            ImFontBaked* baked = font->GetFontBaked(fontSize);
            if (baked) {
                const float glyphScale = fontSize / baked->Size;
                const ImTextureRef texRef = ImGui::GetIO().Fonts->TexRef;
                const ImU32 textColTinted   = ApplyAlpha(cfg.textColor, s.opacity);
                const ImU32 textColUntinted = textColTinted | ~IM_COL32_A_MASK;
                const float textOriginLx = contentLeft;
                const float textOriginLy = cardMidY - textSz.y * 0.5f;
                const char* str = s.displayName.c_str();
                const char* end = str + s.displayName.size();
                float gx = textOriginLx;
                while (str < end) {
                    unsigned int c = (unsigned int)(unsigned char)*str;
                    if (c < 0x80) str += 1;
                    else {const int n = ImTextCharFromUtf8(&c, str, end); if (n == 0) break; str += n;}
                    if (c == '\r' || c == '\n' || c == 0) continue;
                    ImFontGlyph* g = baked->FindGlyph((ImWchar)c);
                    if (!g) continue;
                    if (g->Visible) {
                        const float x1 = gx + g->X0 * glyphScale;
                        const float x2 = gx + g->X1 * glyphScale;
                        const float y1 = textOriginLy + g->Y0 * glyphScale;
                        const float y2 = textOriginLy + g->Y1 * glyphScale;
                        const ImVec2 q0 = rotate(x1, y1);
                        const ImVec2 q1 = rotate(x2, y1);
                        const ImVec2 q2 = rotate(x2, y2);
                        const ImVec2 q3 = rotate(x1, y2);
                        const ImU32 glyphCol = g->Colored ? textColUntinted : textColTinted;
                        dl->AddImageQuad(texRef, q0, q1, q2, q3, ImVec2(g->U0, g->V0), ImVec2(g->U1, g->V0), ImVec2(g->U1, g->V1), ImVec2(g->U0, g->V1), glyphCol);
                    }
                    gx += g->AdvanceX * glyphScale;
                }
            }
        }
    }
}