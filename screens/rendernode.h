// The GPU's render node, and in ft-screens one GBM device on it that its parts share: the overlay
// textures (overlaytex.cpp) and the hand cutouts' Renderer (handrender.cpp) used to open the node
// each. The path is for C too (compositor.c tells clients about the node).
#pragma once

#define FT_RENDER_NODE "/dev/dri/renderD128"

#ifdef __cplusplus
struct gbm_device;

namespace rendernode {

// The shared device, opened by the first Acquire; nullptr (and a line on stderr naming `who`)
// if it can't be. Each Acquire that returned a device takes one Release; the last one closes it.
// One thread only (ft-screens' VR thread).
gbm_device *Acquire(const char *who);
void Release();

}  // namespace rendernode
#endif
