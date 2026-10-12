// CPU-only boundary for unrelated GPU/keyboard/hand subsystems. The real vr.cpp
// adapter is compiled unchanged. Rendering and hardware initialization must abort,
// not silently pass. These tests make no claims about per-pixel rendering.
#include "../handcut.h"
#include "../keyboard.h"
#include <cstdlib>
#include <cmath>

namespace handcut {
Renderer::~Renderer() = default;
bool Renderer::Init(const std::vector<uint64_t> &, std::function<void(const Output *)>) { std::abort(); }
const Output *Renderer::Composite(int, const void *, uint64_t, const ft_dmabuf &, const std::vector<Capsule2D>[]) { std::abort(); }
void Renderer::Forget(const void *) { std::abort(); }
void Renderer::DropPanel(int) { std::abort(); }
bool Hands::Update(const Mat &, int64_t) { return false; } // no hands file reads
void Hands::SetPrediction(bool, double) { std::abort(); }
void EyePositions(const Mat &, double out[2][3]) {
    // No screen buffers in these tests, so these values must never reach Composite.
    for (int e = 0; e < 2; ++e) for (int k = 0; k < 3; ++k) out[e][k] = 0;
}
bool Project(const Panel &, const std::vector<Capsule> &, const double[2][3], std::vector<Capsule2D>[]) { std::abort(); }
double CurveRadius(double width, double requested) { return requested > 0 ? width / (2 * std::acos(-1.) * requested) : 0; }
}
#ifdef FT_CPU_KEYBOARD
#include "keyboard-boundary.inc"
#else
namespace keyboard {
bool Show(const vr::HmdMatrix34_t &) { std::abort(); }
void Hide() {}
bool Shown() { return false; }
bool Aimed(const vr::HmdMatrix34_t &) { return false; } // keyboard absent in this fixture
const vr::HmdMatrix34_t &Pose() { std::abort(); }
void EndDragBy(uint32_t) {}
void SetLasers(bool) {}
void Poll(void (*)(const Event &, void *), void *) {}
void Destroy() {}
}
#endif
