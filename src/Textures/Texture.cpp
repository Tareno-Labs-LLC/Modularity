#include "../../include/Textures/Texture.h"
#include "../../include/Platform/AssetSource.h"
#include "../../include/Textures/ExrLoader.h"
// Ahead of STB_IMAGE_IMPLEMENTATION on purpose: this pulls in stb_image.h for the
// declarations, and stb leaves its implementation block outside the header guard,
// so including it again after the define would emit every definition twice.
#include "../../include/Textures/StbFlipScope.h"

#define STB_IMAGE_IMPLEMENTATION
#include "../../include/ThirdParty/stb_image.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace {

int g_maxTextureDimension = 2048;

void HalveImage(const unsigned char* src, int width, int height, int channels,
                std::vector<unsigned char>& out) {
    const int dstWidth = std::max(1, width / 2);
    const int dstHeight = std::max(1, height / 2);
    out.resize(static_cast<size_t>(dstWidth) * dstHeight * channels);
    for (int y = 0; y < dstHeight; ++y) {
        // Clamp rather than wrap: an odd source dimension leaves a final row or
        const int y0 = std::min(y * 2, height - 1);
        const int y1 = std::min(y * 2 + 1, height - 1);
        for (int x = 0; x < dstWidth; ++x) {
            const int x0 = std::min(x * 2, width - 1);
            const int x1 = std::min(x * 2 + 1, width - 1);
            for (int c = 0; c < channels; ++c) {
                const int a = src[(static_cast<size_t>(y0) * width + x0) * channels + c];
                const int b = src[(static_cast<size_t>(y0) * width + x1) * channels + c];
                const int d = src[(static_cast<size_t>(y1) * width + x0) * channels + c];
                const int e = src[(static_cast<size_t>(y1) * width + x1) * channels + c];
                out[(static_cast<size_t>(y) * dstWidth + x) * channels + c] =
                    static_cast<unsigned char>((a + b + d + e + 2) / 4);
            }
        }
    }
}

}  // namespace

void Texture::SetMaxDimension(int maxDimension) {
    g_maxTextureDimension = (maxDimension >= 16) ? maxDimension : 0;
}

int Texture::GetMaxDimension() { return g_maxTextureDimension; }

Texture::Texture(const std::string& path,
                 GLenum wrapS,
                 GLenum wrapT,
                 GLenum minFilter,
                 GLenum magFilter,
                 TextureFormatPolicy formatPolicy)
    : m_FormatPolicy(formatPolicy)
{
    // Scene textures are stored bottom-up to match GL's texture origin.
    const StbFlipScope flipForScene(true);
    std::vector<uint8_t> fileBytes =
        Modularity::Platform::GetAssetSource().ReadAll(path);
    if (fileBytes.empty()) {
        m_LoadError = "file is empty or could not be read";
        std::cerr << "Failed to load texture: " << path << "\n";
        return;
    }
    std::vector<uint8_t> exrPixels;
    unsigned char* data = nullptr;
    bool dataOwnedByStb = true;
    if (Modularity::IsExrData(fileBytes.data(), fileBytes.size())) {
        std::string exrError;
        if (!Modularity::LoadExrToRgba8(fileBytes.data(), fileBytes.size(),
                                        m_Width, m_Height, exrPixels, exrError)) {
            m_LoadError = exrError;
            std::cerr << "Failed to decode EXR texture: " << path << " (" << exrError << ")\n";
            return;
        }
        data = exrPixels.data();
        dataOwnedByStb = false;
        m_Channels = 4;
    } else {
        data = stbi_load_from_memory(fileBytes.data(),
                                     static_cast<int>(fileBytes.size()),
                                     &m_Width, &m_Height, &m_Channels, 0);
    }
    if (!data) {
        const char* stbReason = stbi_failure_reason();
        m_LoadError = stbReason ? stbReason : "unsupported or corrupt image data";
        std::cerr << "Failed to decode texture: " << path << " (" << m_LoadError << ")\n";
        return;
    }

    // Grey + alpha has no upload format below: it fell through to GL_RGBA and the driver read
    // twice as many bytes as stb allocated. Expand it to RGBA (grey, grey, grey, alpha).
    if (m_Channels == 2) {
        const size_t pixelCount = static_cast<size_t>(m_Width) * static_cast<size_t>(m_Height);
        std::vector<uint8_t> expanded(pixelCount * 4u);
        for (size_t i = 0; i < pixelCount; ++i) {
            const uint8_t grey = data[i * 2u];
            expanded[i * 4u + 0u] = grey;
            expanded[i * 4u + 1u] = grey;
            expanded[i * 4u + 2u] = grey;
            expanded[i * 4u + 3u] = data[i * 2u + 1u];
        }
        if (dataOwnedByStb) {
            stbi_image_free(data);
            dataOwnedByStb = false;
        }
        exrPixels.swap(expanded);
        data = exrPixels.data();
        m_Channels = 4;
    }

    m_SourceWidth = m_Width;
    m_SourceHeight = m_Height;

    if (g_maxTextureDimension > 0 && m_Channels > 0 &&
        (m_Width > g_maxTextureDimension || m_Height > g_maxTextureDimension)) {
        std::vector<unsigned char> scaled;
        std::vector<unsigned char> scratch;
        const unsigned char* srcPixels = data;
        int srcWidth = m_Width;
        int srcHeight = m_Height;
        while ((srcWidth > g_maxTextureDimension || srcHeight > g_maxTextureDimension) &&
               (srcWidth > 1 || srcHeight > 1)) {
            HalveImage(srcPixels, srcWidth, srcHeight, m_Channels, scratch);
            srcWidth = std::max(1, srcWidth / 2);
            srcHeight = std::max(1, srcHeight / 2);
            scaled.swap(scratch);
            srcPixels = scaled.data();
        }
        if (!scaled.empty()) {
            if (dataOwnedByStb) {
                stbi_image_free(data);
                dataOwnedByStb = false;
            }
            exrPixels.swap(scaled);
            data = exrPixels.data();
            m_Width = srcWidth;
            m_Height = srcHeight;
        }
    }

    if (m_Channels == 4) {
        const size_t pixelCount = static_cast<size_t>(m_Width) * static_cast<size_t>(m_Height);
        for (size_t i = 0; i < pixelCount; ++i) {
            const unsigned char alpha = data[i * 4 + 3];
            if (alpha == 0 || alpha == 255) {
                if (alpha == 0) {
                    m_HasBinaryAlpha = true;
                }
                continue;
            }

            m_UsesAlphaBlending = true;
            break;
        }
    }

    if (m_Channels == 1) {
        m_DataFormat = GL_RED;
    } else if (m_Channels == 3) {
        m_DataFormat = GL_RGB;
    } else {
        m_DataFormat = GL_RGBA;
    }

    const bool hasAlpha = (m_Channels == 4);
    auto autoFormat = [&]() -> GLenum {
        if (m_Channels == 1) return GL_R8;
        if (!hasAlpha) return GL_RGB565;
        if (m_UsesAlphaBlending) return GL_RGBA4;
        if (m_HasBinaryAlpha) return GL_RGB5_A1;
        return GL_RGB565;
    };
    switch (m_FormatPolicy) {
        case TextureFormatPolicy::Full:
            m_InternalFormat = (m_Channels == 1) ? GL_R8
                             : (m_Channels == 3) ? GL_RGB8
                                                 : GL_RGBA8;
            break;
        case TextureFormatPolicy::RGB565:  m_InternalFormat = GL_RGB565;  break;
        case TextureFormatPolicy::RGB5_A1: m_InternalFormat = GL_RGB5_A1; break;
        case TextureFormatPolicy::RGBA4:   m_InternalFormat = GL_RGBA4;   break;
        case TextureFormatPolicy::Auto:
        default:
            m_InternalFormat = autoFormat();
            break;
    }

#ifdef __ANDROID__
    if (m_DataFormat == GL_RGBA) {
        if (m_InternalFormat == GL_RGB565)    m_InternalFormat = GL_RGB5_A1;
        else if (m_InternalFormat == GL_RGB8) m_InternalFormat = GL_RGBA8;
    }
#endif

    glGenTextures(1, &m_ID);
    glBindTexture(GL_TEXTURE_2D, m_ID);

    // upload
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, m_InternalFormat, m_Width, m_Height, 0, m_DataFormat, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);
    int bytesPerPixel;
    switch (m_InternalFormat) {
        case GL_R8:                       bytesPerPixel = 1; break;
        case GL_RGB565:
        case GL_RGB5_A1:
        case GL_RGBA4:                    bytesPerPixel = 2; break;
        case GL_RGB8:                     bytesPerPixel = 3; break;
        default:                          bytesPerPixel = 4; break;
    }
    m_ApproxMemoryBytes = static_cast<size_t>(m_Width) *
                          static_cast<size_t>(m_Height) *
                          static_cast<size_t>(bytesPerPixel);
    // Mip chains for 2D textures are roughly 33% extra memory.
    m_ApproxMemoryBytes += m_ApproxMemoryBytes / 3;

    // params
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);

    glBindTexture(GL_TEXTURE_2D, 0);

    if (dataOwnedByStb) {
        stbi_image_free(data);
    }
}

Texture::~Texture()
{
    if (m_ID) {
        glDeleteTextures(1, &m_ID);
        m_ID = 0;
    }
}

void Texture::Bind(GLenum unit) const
{
    glActiveTexture(unit);
    glBindTexture(GL_TEXTURE_2D, m_ID);
}

void Texture::Unbind() const
{
    glBindTexture(GL_TEXTURE_2D, 0);
}
