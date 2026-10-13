#include "free_camera.h"

#include "pxr/base/gf/frustum.h"
#include "pxr/base/gf/math.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/ray.h"
#include "pxr/base/gf/rotation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vela {

namespace {

// Far / near ratios: the tightest we aim for, and the loosest we allow.
constexpr double GoodZResolution = 5e4;
constexpr double SafeZResolution = 1e6;

GfMatrix4d Rotate(const GfVec3d &axis, double degrees) {
    return GfMatrix4d(1.0).SetRotate(GfRotation(axis, degrees));
}

// Rotates a Y-up camera to Z-up; identity for Y-up stages.
GfMatrix4d UpAxisMatrix(bool zUp) {
    return zUp ? Rotate(GfVec3d::XAxis(), 90.0) : GfMatrix4d(1.0);
}

} // namespace

CameraDrag CameraDragFor(unsigned buttons, bool option, bool command) {
    if (!option) {
        return CameraDrag::None;
    }
    const bool left = buttons & MouseLeft;
    const bool middle = buttons & MouseMiddle;
    if ((left && middle) || (buttons & MouseRight)) {
        return CameraDrag::Dolly;
    }
    if (middle || (left && command)) {
        return CameraDrag::Track;
    }
    return left ? CameraDrag::Tumble : CameraDrag::None;
}

FreeCamera::FreeCamera(bool zUp, double fov) : _zUp(zUp), _fov(fov) {}

FreeCamera FreeCamera::FromGfCamera(
    const GfCamera &camera, bool zUp, double fallbackDistance)
{
    FreeCamera result(zUp, camera.GetFieldOfView(GfCamera::FOVVertical));
    const GfFrustum frustum = camera.GetFrustum();
    const double focus = camera.GetFocusDistance();
    result._dist = focus > 0.0 ? focus : fallbackDistance;
    result._selSize = result._dist / 10.0;
    result._center = frustum.GetPosition() +
        result._dist * frustum.ComputeViewDirection();
    GfMatrix4d transform =
        camera.GetTransform() * UpAxisMatrix(zUp).GetInverse();
    transform.Orthonormalize();
    const GfVec3d angles = transform.ExtractRotation().Decompose(
        GfVec3d::YAxis(), GfVec3d::XAxis(), GfVec3d::ZAxis());
    result._theta = -angles[0];
    result._phi = -angles[1];
    result._psi = -angles[2];
    return result;
}

void FreeCamera::Tumble(double dTheta, double dPhi) {
    _theta += dTheta;
    _phi += dPhi;
}

void FreeCamera::Track(double deltaRight, double deltaUp) {
    const GfMatrix4d transform = ComputeTransform();
    const GfVec3d right = transform.TransformDir(GfVec3d::XAxis());
    const GfVec3d up = transform.TransformDir(GfVec3d::YAxis());
    _center += deltaRight * right.GetNormalized() + deltaUp * up.GetNormalized();
}

void FreeCamera::Dolly(double scaleFactor) {
    _dist = std::max(_dist * scaleFactor,
                     std::numeric_limits<double>::epsilon() * _selSize);
}

bool FreeCamera::Frame(const GfBBox3d &bounds, double frameFit) {
    const GfRange3d range = bounds.ComputeAlignedRange();
    if (range.IsEmpty()) {
        return false;
    }
    const GfVec3d size = range.GetSize();
    const double selSize = std::max({size[0], size[1], size[2]});
    if (selSize <= 0.0) {
        return false;
    }
    _selSize = selSize;
    _center = bounds.ComputeCentroid();
    const double halfFov = GfDegreesToRadians(std::max(_fov, 1.0) * 0.5);
    // Fit the bounding sphere, so the bounds fit from every direction.
    const double radius = 0.5 * size.GetLength() * frameFit;
    _dist = radius / std::sin(halfFov);
    return true;
}

double FreeCamera::PixelsToWorld(double viewportHeight) const {
    const double windowHeight =
        2.0 * std::tan(GfDegreesToRadians(_fov * 0.5));
    return windowHeight * _dist / std::max(viewportHeight, 1.0);
}

void FreeCamera::Drag(
    CameraDrag mode, double dx, double dy, double viewportHeight)
{
    switch (mode) {
    case CameraDrag::Tumble:
        Tumble(0.25 * dx, 0.25 * dy);
        break;
    case CameraDrag::Track: {
        const double scale = PixelsToWorld(viewportHeight);
        Track(-dx * scale, dy * scale);
        break;
    }
    case CameraDrag::Dolly:
        Dolly(std::max(0.1, 1.0 - 0.002 * (dx + dy)));
        break;
    case CameraDrag::None:
        break;
    }
}

GfMatrix4d FreeCamera::ComputeTransform() const {
    return GfMatrix4d(1.0).SetTranslate(GfVec3d::ZAxis() * _dist) *
        Rotate(GfVec3d::ZAxis(), -_psi) *
        Rotate(GfVec3d::XAxis(), -_phi) *
        Rotate(GfVec3d::YAxis(), -_theta) *
        UpAxisMatrix(_zUp) *
        GfMatrix4d(1.0).SetTranslate(_center);
}

GfCamera FreeCamera::ComputeGfCamera(
    double aspectRatio, const GfBBox3d &stageBounds) const
{
    GfCamera camera;
    camera.SetPerspectiveFromAspectRatioAndFieldOfView(
        aspectRatio > 0.0 ? aspectRatio : 1.0, _fov, GfCamera::FOVVertical);
    camera.SetTransform(ComputeTransform());
    camera.SetFocusDistance(_dist);
    camera.SetClippingRange(
        ComputeClippingRange(camera.GetFrustum(), stageBounds));
    return camera;
}

GfRange1f FreeCamera::ComputeClippingRange(
    const GfFrustum &frustum, const GfBBox3d &bounds)
{
    const GfRange3d range = bounds.GetRange();
    if (range.IsEmpty()) {
        return GfRange1f(DefaultNear, DefaultFar);
    }
    const GfRay ray(frustum.GetPosition(), frustum.ComputeViewDirection());
    double nearest = std::numeric_limits<double>::infinity();
    double farthest = 0.0;
    for (int i = 0; i < 8; ++i) {
        double depth = 0.0;
        ray.FindClosestPoint(
            bounds.GetMatrix().Transform(range.GetCorner(i)), &depth);
        nearest = std::min(nearest, depth);
        farthest = std::max(farthest, depth);
    }
    if (farthest <= 0.0) {
        // Everything is behind the camera.
        return GfRange1f(DefaultNear, DefaultFar);
    }
    const double far = farthest * 1.01;
    // Inside the bounds, aim for good depth precision without clipping
    // geometry that is close to the camera.
    double near = nearest > 0.0 ? nearest * 0.99 : far / GoodZResolution;
    near = std::max(near, far / SafeZResolution);
    return GfRange1f(near, far);
}

} // namespace vela
