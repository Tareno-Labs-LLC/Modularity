// The asset tests do not construct GPU meshes. These explicit traps satisfy
// references from the loader's unused rendering methods without linking the
// whole engine or letting a test silently exercise a fake renderer.
#include "Rendering.h"
#include <cstdlib>

Mesh::Mesh(const float*, size_t) { std::abort(); }
Mesh::~Mesh() { std::abort(); }
