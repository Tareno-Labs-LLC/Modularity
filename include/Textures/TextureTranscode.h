#pragma once
#include <string>

namespace Modularity {

// Sidecars for EXRs the bundled reader cannot decompress.
//
// TinyEXR does not implement DWAA/DWAB - the lossy DreamWorks compression that
// Poly Haven ships by default - and implementing it is a multi-thousand-line job.
// Instead the editor transcodes such a file once, through Blender, into a PNG
// sitting beside it, and every later load picks that up.
//
// The sidecar lives next to the source and is part of the project, which is what
// makes this work in a shipped build: the player resolves the same path and
// never needs Blender, or any knowledge that a transcode happened.

// "<path>.png" - deliberately keeping the .exr in the name, so the sidecar is
// obviously derived, sorts next to its source, and cannot collide with a real
// "<stem>.png" the artist exported themselves.
std::string TranscodedTexturePath(const std::string& exrPath);

// True for a path produced by the rule above. The file browser hides these: they
// are build output, not assets to pick.
bool IsTranscodedTexturePath(const std::string& path);

// The path that should actually be opened for `path`.
//
// Returns the sidecar when one exists and is not older than the EXR, otherwise
// `path` unchanged. The staleness check matters: re-downloading a texture must
// not leave the old transcode in place, silently shipping stale pixels.
//
// Anything that is not a .exr is returned untouched, so this is safe to call on
// every texture load.
std::string ResolveLoadableTexturePath(const std::string& path);

} // namespace Modularity
