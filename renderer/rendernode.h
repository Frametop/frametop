// The GPU render node and one GBM device shared by the overlay's output buffers.
#pragma once

#define FT_RENDER_NODE "/dev/dri/renderD128"

#ifdef __cplusplus
struct gbm_device;

namespace rendernode {

// The shared device, opened by the first Acquire; nullptr (and a line on stderr naming `who`)
// if it can't be. Each Acquire that returned a device takes one Release; the last one closes it.
// One thread only (the overlay's VR thread).
gbm_device *Acquire(const char *who);
void Release();

}  // namespace rendernode
#endif
