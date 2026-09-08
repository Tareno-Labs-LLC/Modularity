#pragma once

#include <cstdint>

namespace Modu2DStats {

struct Snapshot {
    uint64_t spriteQuads = 0;
    uint64_t spriteBatches = 0;
    uint64_t postFxPasses = 0;
    uint64_t viewportRedraws = 0;
    uint64_t skippedRedraws = 0;
    uint64_t cachedLayerReuses = 0;
    uint64_t uiDirScans = 0;
    uint64_t uiWindowsSkipped = 0;
};

void CountSpriteQuads(uint64_t count = 1);
void CountSpriteBatch();
void CountPostFxPass();
void CountViewportRedraw();
void CountSkippedRedraw();
void CountCachedLayerReuse();
void CountDirScan(uint64_t count = 1);
void CountWindowSkipped();

void Reset();
Snapshot Read();

} // namespace Modu2DStats
