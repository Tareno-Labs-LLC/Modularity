#ifndef TEXTURE_H
#define TEXTURE_H

#include <string>
#include <cstddef>
#include "../Graphics/OpenGL.h"

// Controls the GPU-side storage format a texture is uploaded at. The compact
// 16bpp formats halve VRAM versus the default RGBA8, which matters when a lot
// of sprites are resident at once on low-VRAM hardware.
enum class TextureFormatPolicy {
    Auto,      // adaptive: pick the smallest format the texture's content allows
    Full,      // force RGBA8 / RGB8 (original behavior, highest quality)
    RGB565,    // 16bpp, no alpha - opaque textures
    RGB5_A1,   // 16bpp, 1-bit alpha - binary/cutout transparency
    RGBA4      // 16bpp, 4-bit alpha - smooth/blended transparency (may band)
};

// Stable string names used for persistence (project file) and UI. Keep these in
// sync with the enum; unknown strings fall back to Auto.
inline const char* ToString(TextureFormatPolicy policy) {
    switch (policy) {
        case TextureFormatPolicy::Full:    return "Full";
        case TextureFormatPolicy::RGB565:  return "RGB565";
        case TextureFormatPolicy::RGB5_A1: return "RGB5_A1";
        case TextureFormatPolicy::RGBA4:   return "RGBA4";
        case TextureFormatPolicy::Auto:
        default:                           return "Auto";
    }
}

inline TextureFormatPolicy TextureFormatPolicyFromString(const std::string& value) {
    if (value == "Full")    return TextureFormatPolicy::Full;
    if (value == "RGB565")  return TextureFormatPolicy::RGB565;
    if (value == "RGB5_A1") return TextureFormatPolicy::RGB5_A1;
    if (value == "RGBA4")   return TextureFormatPolicy::RGBA4;
    return TextureFormatPolicy::Auto;
}

class Texture
{
public:
    // Upper bound on either dimension at upload time, applied engine-wide.
    // A 4096x4096 RGBA8 texture with its mip chain is 85 MB of VRAM; a room
    // dressed with a dozen Poly Haven assets therefore asks for well over a
    // gigabyte, which oversubscribes VRAM and makes the driver page textures
    // in and out as the camera moves - the stutter reads as a frame spike, not
    // as an out-of-memory error. Halving to 2048 costs a quarter of that.
    // 0 means no limit. Only exact power-of-two halvings are used, so the
    // result is a clean box-filtered reduction with no resampling artifacts.
    static void SetMaxDimension(int maxDimension);
    static int GetMaxDimension();

    // load from file; format and wrap/filter are optional
    Texture(const std::string& path,
            GLenum wrapS = GL_REPEAT,
            GLenum wrapT = GL_REPEAT,
            GLenum minFilter = GL_LINEAR_MIPMAP_LINEAR,
            GLenum magFilter = GL_LINEAR,
            TextureFormatPolicy formatPolicy = TextureFormatPolicy::Auto);
    ~Texture();

    void Bind(GLenum unit = GL_TEXTURE0) const;
    void Unbind() const;

    // Why the last load failed, empty on success. The decoders produce genuinely
    // actionable messages (an EXR saved with DWAA names the compression and says
    // what to re-save it as), and dropping them on the floor is what makes an
    // unsupported file look like the engine silently ignoring it.
    const std::string& GetLoadError() const { return m_LoadError; }

    GLuint GetID() const { return m_ID; }
    int GetWidth() const { return m_Width; }
    int GetHeight() const { return m_Height; }
    // Source dimensions before any downscale, so the editor can show what the
    // file actually holds rather than what survived the size cap.
    int GetSourceWidth() const { return m_SourceWidth; }
    int GetSourceHeight() const { return m_SourceHeight; }
    bool WasDownscaled() const { return m_SourceWidth != m_Width || m_SourceHeight != m_Height; }
    size_t GetApproxMemoryBytes() const { return m_ApproxMemoryBytes; }
    bool HasAlphaChannel() const { return m_Channels == 4; }
    bool HasBinaryAlpha() const { return m_HasBinaryAlpha; }
    bool UsesAlphaBlending() const { return m_UsesAlphaBlending; }
    GLenum GetInternalFormat() const { return m_InternalFormat; }
    TextureFormatPolicy GetFormatPolicy() const { return m_FormatPolicy; }

private:
    GLuint m_ID = 0;
    int m_Width = 0;
    int m_Height = 0;
    int m_SourceWidth = 0;
    int m_SourceHeight = 0;
    int m_Channels = 0;
    GLenum m_InternalFormat = GL_RGBA;
    GLenum m_DataFormat = GL_RGBA;
    size_t m_ApproxMemoryBytes = 0;
    bool m_HasBinaryAlpha = false;
    bool m_UsesAlphaBlending = false;
    std::string m_LoadError;
    TextureFormatPolicy m_FormatPolicy = TextureFormatPolicy::Auto;
};

#endif
