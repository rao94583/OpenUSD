// Storm on Metal: renders a stage with UsdImagingGLEngine on an HgiMetal the
// app owns, and draws the color AOV into a Metal texture. No AppKit.
#pragma once

#import <Metal/Metal.h>

#include "app/vela/core/view_settings.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/camera.h"
#include "pxr/base/gf/vec2i.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/timeCode.h"

#include <memory>
#include <string>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

struct View {
    // The camera to render through, also used for the camera light.
    GfCamera camera;
    // A scene camera to render through instead of camera's matrices,
    // as usdrecord does.
    SdfPath cameraPath;
    UsdTimeCode time = UsdTimeCode::EarliestTime();
    // Render size in pixels.
    GfVec2i size{1, 1};
    GfVec4f clearColor{0.0f};
    // Draw mode, purposes, lighting and the like. The background is
    // clearColor, and highlight below decides on selection highlights.
    ViewSettings settings;
    // Whether selected prims are drawn highlighted.
    bool highlight = false;
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    id<MTLDevice> GetDevice() const;
    id<MTLCommandQueue> GetQueue() const;

    // unloadedAsBounds draws unloaded prims as bounding boxes (usdview's
    // --bboxStandin).
    void SetStage(const UsdStageRefPtr &stage, bool unloadedAsBounds = false);
    void SetSelection(const SdfPathVector &paths);

    // Renders the stage and encodes drawing the image, upright, into
    // target. Commands are submitted to GetQueue() in order, so committing
    // commandBuffer after this call draws the finished frame.
    bool Draw(const View &view, id<MTLTexture> target,
              id<MTLCommandBuffer> commandBuffer);
    // False while a progressive renderer is still refining the image.
    bool IsConverged() const;

    // Renders until converged and writes the image Draw would show.
    bool WriteImage(const View &view, const std::string &path);

    // The prim drawn nearest the center of a pick frustum.
    // Picking draws what the view settings show (purposes, draw mode).
    bool Pick(const GfFrustum &frustum, UsdTimeCode time,
              const ViewSettings &settings, SdfPath *path, GfVec3d *point);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace vela
