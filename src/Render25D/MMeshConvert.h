#pragma once

#include "ModelLoader.h"
#include "Render25D/MMesh.h"

namespace Modularity::Render25D {

MMeshAsset BuildMMeshFromRawMesh(const RawMeshAsset& raw);
RawMeshAsset BuildRawMeshFromMMesh(const MMeshAsset& asset);

}
