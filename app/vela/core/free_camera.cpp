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

std::string CameraRenderError(const GfCamera &camera, bool embree) {
    const auto clip = camera.GetClippingRange();
    if (!std::isfinite(clip.GetMin()) || !std::isfinite(clip.GetMax()) ||
        clip.GetMin() >= clip.GetMax() ||
        (camera.GetProjection() == GfCamera::Perspective && clip.GetMin() <= 0)) {
        return "The camera has an invalid clipping range";
    }
    const GfFrustum frustum = camera.GetFrustum();
    for (const GfMatrix4d &matrix : {camera.GetTransform(), frustum.ComputeViewMatrix(),
                                    frustum.ComputeProjectionMatrix()}) {
        for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) {
            if (!std::isfinite(matrix[row][col])) return "The camera contains a non-finite transform or projection";
        }
        double determinant = 0;
        const GfMatrix4d inverse = matrix.GetInverse(&determinant);
        if (!std::isfinite(determinant) || determinant == 0) return "The camera transform or projection is singular";
        for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) {
            if (!std::isfinite(inverse[row][col])) return "The camera inverse is non-finite";
        }
    }
    if (embree) {
        // vendor/embree/kernels/common/ray.h: ray.valid() uses FLT_LARGE,
        // not FLT_MAX, to leave headroom for traversal arithmetic.
        const GfVec3d position = frustum.GetPosition();
        for (int i = 0; i < 3; ++i) if (std::abs(position[i]) > 1.844e18f) {
            return "The camera position exceeds Embree's supported coordinate range";
        }
    }
    return {};
}

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

bool FreeCamera::Frame(const GfBBox3d &bounds, double frameFit,
                       double aspectRatio) {
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
    // Keep the current orientation and find the distance at which every
    // corner of the bounds fits the view, horizontally and vertically.
    // frameFit leaves a margin by narrowing the view.
    const double tanV = std::tan(GfDegreesToRadians(std::max(_fov, 1.0) * 0.5)) /
        std::max(frameFit, 1.0);
    const double tanH = tanV * (aspectRatio > 0.0 ? aspectRatio : 1.0);
    const GfMatrix4d transform = ComputeTransform();
    const GfVec3d right = transform.TransformDir(GfVec3d::XAxis()).GetNormalized();
    const GfVec3d up = transform.TransformDir(GfVec3d::YAxis()).GetNormalized();
    const GfVec3d back = transform.TransformDir(GfVec3d::ZAxis()).GetNormalized();
    double dist = 0.0;
    for (int i = 0; i < 8; ++i) {
        const GfVec3d offset =
            bounds.GetMatrix().Transform(bounds.GetRange().GetCorner(i)) - _center;
        // A corner at depth (dist - toward) must satisfy |x| <= depth * tan.
        const double toward = GfDot(offset, back);
        dist = std::max({dist,
                         toward + std::abs(GfDot(offset, right)) / tanH,
                         toward + std::abs(GfDot(offset, up)) / tanV});
    }
    _dist = dist;
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
