#pragma once

#include "../ThirdParty/stb_image.h"

// Scoped control over stb_image's vertical flip.
//
// stb compiles with STBI_THREAD_LOCAL here, which makes its two flip setters
// behave very differently. stbi_set_flip_vertically_on_load_thread latches the
// calling thread into "ignore the global from now on", and stb exposes no way
// to clear that latch. Any site that later used the global setter on that same
// thread silently kept the latched value instead, so whether an image decoded
// bottom-up depended on what that thread had already decoded this session.
//
// Every decode site states the orientation it wants with one of these, and the
// enclosing value is restored on scope exit, so no site can leak its choice
// into the next one.
class StbFlipScope {
public:
    explicit StbFlipScope(bool flipVertically)
        : m_Previous(CurrentFlip())
    {
        Apply(flipVertically);
    }

    ~StbFlipScope() { Apply(m_Previous); }

    StbFlipScope(const StbFlipScope&) = delete;
    StbFlipScope& operator=(const StbFlipScope&) = delete;

private:
    // Mirrors the latched per-thread value, which stb has no getter for.
    static bool& CurrentFlip() {
        static thread_local bool flip = false;
        return flip;
    }

    static void Apply(bool flipVertically) {
        CurrentFlip() = flipVertically;
#ifdef STBI_THREAD_LOCAL
        stbi_set_flip_vertically_on_load_thread(flipVertically ? 1 : 0);
#else
        stbi_set_flip_vertically_on_load(flipVertically ? 1 : 0);
#endif
    }

    bool m_Previous;
};
