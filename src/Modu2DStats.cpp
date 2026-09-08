#include "Modu2DStats.h"

namespace Modu2DStats {
namespace {
Snapshot gStats;
} // namespace

void CountSpriteQuads(uint64_t count) { gStats.spriteQuads += count; }
void CountSpriteBatch() { ++gStats.spriteBatches; }
void CountPostFxPass() { ++gStats.postFxPasses; }
void CountViewportRedraw() { ++gStats.viewportRedraws; }
void CountSkippedRedraw() { ++gStats.skippedRedraws; }
void CountCachedLayerReuse() { ++gStats.cachedLayerReuses; }
void CountDirScan(uint64_t count) { gStats.uiDirScans += count; }
void CountWindowSkipped() { ++gStats.uiWindowsSkipped; }

void Reset() { gStats = Snapshot{}; }
Snapshot Read() { return gStats; }

} // namespace Modu2DStats
