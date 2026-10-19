// Regression for viewport capture aborting in Embree's instance traversal.
// A nonempty instance BVH is necessary: empty scenes skip ray validation.
#include <embree4/rtcore.h>
#include <cmath>
#include <cstdio>
#include <limits>

int main() {
    RTCDevice device = rtcNewDevice(nullptr);
    RTCScene child = rtcNewScene(device);
    RTCGeometry triangle = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
    auto *vertices = static_cast<float *>(rtcSetNewGeometryBuffer(
        triangle, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3 * sizeof(float), 3));
    const float points[] = {-1,-1,0, 1,-1,0, 0,1,0};
    for (int i = 0; i < 9; ++i) vertices[i] = points[i];
    auto *indices = static_cast<unsigned *>(rtcSetNewGeometryBuffer(
        triangle, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(unsigned), 1));
    indices[0] = 0; indices[1] = 1; indices[2] = 2;
    rtcCommitGeometry(triangle); rtcAttachGeometry(child, triangle); rtcReleaseGeometry(triangle);
    rtcCommitScene(child);
    RTCScene scene = rtcNewScene(device);
    RTCGeometry instance = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
    rtcSetGeometryInstancedScene(instance, child);
    rtcCommitGeometry(instance); rtcAttachGeometry(scene, instance); rtcReleaseGeometry(instance);
    rtcCommitScene(scene);
    int failures = 0;
    for (int test = 0; test < 5; ++test) {
        RTCRayHit ray{};
        ray.ray.org_z = 1; ray.ray.dir_z = -1;
        ray.ray.tfar = std::numeric_limits<float>::infinity(); ray.ray.mask = ~0u;
        ray.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        for (auto &id : ray.hit.instID) id = RTC_INVALID_GEOMETRY_ID;
        if (test == 1) ray.ray.org_x = std::numeric_limits<float>::quiet_NaN();
        if (test == 2) ray.ray.dir_y = std::numeric_limits<float>::infinity();
        if (test == 3) ray.ray.org_x = 2e18f; // Beyond Embree's FLT_LARGE bound.
        rtcIntersect1(scene, &ray);
        const bool hit = ray.hit.geomID != RTC_INVALID_GEOMETRY_ID;
        if (hit != (test == 0 || test == 4)) ++failures;
    }
    rtcReleaseScene(scene); rtcReleaseScene(child); rtcReleaseDevice(device);
    if (failures) std::fprintf(stderr, "%d ray checks failed\n", failures);
    return failures ? 1 : 0;
}
