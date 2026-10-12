// A desk rectangle in OpenVR standing space, retaining the legacy volume wire form.
// Pure placement/manipulation only. enabled is legacy data, never camera intent.
#pragma once

#include <array>

namespace passthrough {
using Point = std::array<double, 3>;
struct Volume {
    bool enabled = false; // preserved legacy field; never camera On intent
    Point center{0, 0.75, -0.65};
    Point size{1, 0.35, 0.65};  // width, height, depth
    // Standing_from_volume = T(center) Ry(yaw) Rx(pitch) Rz(roll), degrees.
    double yaw = 0, pitch = 0, roll = 0;
};

using Pose = std::array<std::array<double, 4>, 3>;
Pose VolumePose(const Volume &v);
// Plane consumed by the native camera mask renderer.
// Top center C + R*(0,h/2,0), basis R*[X,-Z,Y], width=x, length=z.
struct Plane { Pose pose; double width, length; };
Plane TopFace(const Volume &v);
bool ValidPose(const Pose &pose);
bool Valid(const Volume &v);
Point ToLocal(const Volume &v, const Point &p);
Point ToWorld(const Volume &v, const Point &p);
struct Edge { Point a, b; };
// Visible rectangle and controls use only the legacy volume's top plane.
// Storage remains compatible with saved layouts; no side or bottom faces render.
std::array<Edge, 4> RectangleEdges(const Volume &v, double outset = 0);
std::array<Point, 6> RectangleControls(const Volume &v);
// Grab bar outside the near-user (+local Z) edge on the rectangle plane.
Point GrabCenter(const Volume &v);
// Controls 0/1 resize width, 4/5 resize depth. Height is saved-layout compatibility only.
// Delta is metres along the positive local axis from the initial dragged edge.
// Opposite face stays fixed; dimensions and standing center limits clamp together.
bool ResizeFace(Volume &out, const Volume &start, int face, double delta);
struct Ray { Point origin, direction; };
// One device owns a drag. Cancellation rolls back; an external set simply discards it.
class ResizeDrag {
public:
    bool Begin(const Volume &v, int face, unsigned device, const Ray &ray);
    bool Update(Volume &v, const Ray &ray);
    bool Finish(const Volume &v, unsigned device);
    void Cancel(Volume &v);
    void Reset() { face_ = -1; }
    bool Active() const { return face_ >= 0; }
    int Face() const { return face_; }
    unsigned Device() const { return device_; }
private:
    bool Coordinate(const Ray &ray, double &value) const;
    Volume start_;
    Point axis_{}, point_{}, normal_{}, gesture_{};
    double distance_ = 0, grab_ = 0;
    int face_ = -1;
    unsigned device_ = 0;
};
// Grab ownership/rollback with the desktop carry transform:
// device_now * inverse(device_press) * volume_press.
class TranslateDrag {
public:
    bool UpdatePose(Volume &v, const Pose &pose);
    bool Begin(const Volume &v, unsigned device);
    bool Finish(const Volume &v, unsigned device);
    void Cancel(Volume &v);
    void Reset() { active_ = false; }
    bool Active() const { return active_; }
    unsigned Device() const { return device_; }
private:
    Volume start_;
    unsigned device_ = 0;
    bool active_ = false;
};
// Returns false for another subsystem's command. State is safe to query with --no-vr.
bool Command(const char *cmd, Volume &v, char *reply, int size, ResizeDrag *drag = nullptr,
             TranslateDrag *translation = nullptr);
}  // namespace passthrough
