// A Maya-style free camera: tumble, track and dolly around a center of
// interest. Ported from usdview's freeCamera.py, with no UI dependencies.
#pragma once

#include "pxr/pxr.h"
#include "pxr/base/gf/bbox3d.h"
#include "pxr/base/gf/camera.h"
#include "pxr/base/gf/range1f.h"
#include "pxr/base/gf/vec3d.h"

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

// Bits of NSEvent.pressedMouseButtons.
enum MouseButton : unsigned {
    MouseLeft = 1u << 0,
    MouseRight = 1u << 1,
    MouseMiddle = 1u << 2,
};

enum class CameraDrag { None, Tumble, Track, Dolly };

// The Maya navigation mode for the held buttons and modifiers. Option is
// required; Option + Command + left stands in for the middle button.
CameraDrag CameraDragFor(unsigned buttons, bool option, bool command);

class FreeCamera {
public:
    static constexpr double DefaultNear = 1.0;
    static constexpr double DefaultFar = 2000000.0;

    explicit FreeCamera(bool zUp = false, double fov = 60.0);

    // A free camera that matches a scene camera's view, for detaching from
    // it. fallbackDistance is used when the camera has no focus distance.
    static FreeCamera FromGfCamera(
        const GfCamera &camera, bool zUp, double fallbackDistance);

    // Rotates around the center of interest by degrees: theta around the
    // up axis and phi around the camera's right axis, so the camera stays
    // upright.
    void Tumble(double dTheta, double dPhi);
    // Moves the center of interest and camera in the view plane, by world
    // units.
    void Track(double deltaRight, double deltaUp);
    // Scales the distance to the center of interest.
    void Dolly(double scaleFactor);
    // Centers on the bounds and moves back until they fit a view of the
    // given aspect ratio (width / height), in both directions. Returns
    // false, changing nothing, for empty bounds.
    bool Frame(const GfBBox3d &bounds, double frameFit = 1.1,
               double aspectRatio = 1.0);

    // Applies a mouse drag of (dx, dy) pixels, y down, in a viewport
    // viewportHeight pixels high. Tracking moves the point under the cursor
    // with it, at the center of interest's depth.
    void Drag(CameraDrag mode, double dx, double dy, double viewportHeight);
    // World units per pixel at the center of interest's depth.
    double PixelsToWorld(double viewportHeight) const;

    // The camera to world transform.
    GfMatrix4d ComputeTransform() const;
    // The camera for a view of the given aspect ratio, with clipping planes
    // fitted to stageBounds (or defaults when they are empty).
    GfCamera ComputeGfCamera(
        double aspectRatio, const GfBBox3d &stageBounds) const;
    // Near and far planes that contain the bounds as seen from the frustum.
    static GfRange1f ComputeClippingRange(
        const GfFrustum &frustum, const GfBBox3d &bounds);

    const GfVec3d &GetCenter() const { return _center; }
    double GetDistance() const { return _dist; }
    double GetFieldOfView() const { return _fov; }
    bool IsZUp() const { return _zUp; }

private:
    bool _zUp;
    double _fov;
    double _theta = 0.0;
    double _phi = 0.0;
    double _psi = 0.0;
    GfVec3d _center{0.0};
    double _dist = 100.0;
    double _selSize = 10.0;
};

} // namespace vela
