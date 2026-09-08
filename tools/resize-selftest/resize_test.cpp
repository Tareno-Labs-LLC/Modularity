// Standalone self-test for the Renderer::resize validation policy.
//
// Covers the cross-platform defensive guards added alongside the Windows
// minimize/restore work: a minimized or mid-restore window reports a 0x0 client
// area, and that must never reach glTexImage2D / glRenderbufferStorage.
//
// RendererResizePolicy.h is pure (no GL, no GLFW, no engine), so this runs
// headlessly with no context and no engine link.
#include "RendererResizePolicy.h"

#include <cstdio>

using namespace ModuRenderer;

static int failures = 0;

static void Expect(ResizeDecision actual, ResizeDecision expected, const char* what) {
    if (actual != expected) {
        std::printf("FAIL: %s -> got %s, expected %s\n", what,
                    ResizeDecisionName(actual), ResizeDecisionName(expected));
        ++failures;
    }
}

// A renderer in the normal steady state: initialized, idle, running at 1280x720.
static ResizeRequest Healthy(int w, int h) {
    ResizeRequest r;
    r.width = w;
    r.height = h;
    r.currentWidth = 1280;
    r.currentHeight = 720;
    r.initialized = true;
    r.resizeInProgress = false;
    r.shuttingDown = false;
    return r;
}

int main() {
    // --- zero / negative dimensions -------------------------------------------
    // The Windows minimized case is 0x0 specifically.
    Expect(EvaluateResizeRequest(Healthy(0, 0)), ResizeDecision::RejectInvalidSize,
           "0x0 minimized client area");
    Expect(EvaluateResizeRequest(Healthy(0, 720)), ResizeDecision::RejectInvalidSize,
           "zero width");
    Expect(EvaluateResizeRequest(Healthy(1280, 0)), ResizeDecision::RejectInvalidSize,
           "zero height");
    Expect(EvaluateResizeRequest(Healthy(-1, -1)), ResizeDecision::RejectInvalidSize,
           "negative both");
    Expect(EvaluateResizeRequest(Healthy(-1280, 720)), ResizeDecision::RejectInvalidSize,
           "negative width");
    Expect(EvaluateResizeRequest(Healthy(1280, -720)), ResizeDecision::RejectInvalidSize,
           "negative height");

    // An invalid size is rejected even while shutting down or reentrant, i.e. the
    // size check is ordered first and never falls through to a GL call.
    {
        ResizeRequest r = Healthy(0, 0);
        r.shuttingDown = true;
        r.resizeInProgress = true;
        r.initialized = false;
        Expect(EvaluateResizeRequest(r), ResizeDecision::RejectInvalidSize,
               "0x0 with every other guard also tripped");
    }

    // --- lifecycle guards ------------------------------------------------------
    {
        ResizeRequest r = Healthy(1920, 1080);
        r.shuttingDown = true;
        Expect(EvaluateResizeRequest(r), ResizeDecision::RejectShuttingDown,
               "valid size during shutdown");
    }
    {
        ResizeRequest r = Healthy(1920, 1080);
        r.resizeInProgress = true;
        Expect(EvaluateResizeRequest(r), ResizeDecision::RejectReentrant,
               "reentrant resize");
    }
    {
        ResizeRequest r = Healthy(1920, 1080);
        r.initialized = false;
        Expect(EvaluateResizeRequest(r), ResizeDecision::RejectNotInitialized,
               "resize before initialize()");
    }

    // --- no-op and accept ------------------------------------------------------
    Expect(EvaluateResizeRequest(Healthy(1280, 720)), ResizeDecision::RejectUnchanged,
           "same size is a no-op");
    Expect(EvaluateResizeRequest(Healthy(1920, 1080)), ResizeDecision::Accept,
           "valid new size");
    Expect(EvaluateResizeRequest(Healthy(1, 1)), ResizeDecision::Accept,
           "1x1 is small but legal");

    // The restore sequence: 0x0 while minimized must not disturb the stored size,
    // then the first valid size after restore is accepted.
    {
        ResizeRequest minimized = Healthy(0, 0);
        Expect(EvaluateResizeRequest(minimized), ResizeDecision::RejectInvalidSize,
               "minimize step");
        // currentWidth/Height are untouched by a rejection, so the restore below
        // still sees 1280x720 as the last known-good size and reallocates.
        Expect(EvaluateResizeRequest(Healthy(1600, 900)), ResizeDecision::Accept,
               "restore step reallocates from last known-good size");
    }

    // --- GPU limit clamp -------------------------------------------------------
    {
        int w = 40000, h = 40000;
        ClampResizeToLimit(w, h, 8192);
        if (w != 8192 || h != 8192) {
            std::printf("FAIL: clamp to 8192 gave %dx%d\n", w, h);
            ++failures;
        }
    }
    {
        int w = 1920, h = 1080;
        ClampResizeToLimit(w, h, 8192);
        if (w != 1920 || h != 1080) {
            std::printf("FAIL: under-limit size was altered to %dx%d\n", w, h);
            ++failures;
        }
    }
    {
        // A failed glGetIntegerv gives 0; fall back to the conservative bound.
        int w = 40000, h = 100;
        ClampResizeToLimit(w, h, 0);
        if (w != 16384 || h != 100) {
            std::printf("FAIL: unknown-limit fallback gave %dx%d\n", w, h);
            ++failures;
        }
    }

    if (failures == 0) {
        std::printf("resize self-test: all checks passed\n");
        return 0;
    }
    std::printf("resize self-test: %d failure(s)\n", failures);
    return 1;
}
