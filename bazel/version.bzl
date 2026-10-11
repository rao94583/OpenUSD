"""OpenUSD version, mirroring cmake/defaults/Version.cmake.

Keep in sync with Version.cmake; cmake2bazel --check verifies this.
"""

PXR_MAJOR_VERSION = "0"
PXR_MINOR_VERSION = "27"
PXR_PATCH_VERSION = "02"

PXR_VERSION = str(
    int(PXR_MAJOR_VERSION) * 10000 +
    int(PXR_MINOR_VERSION) * 100 +
    int(PXR_PATCH_VERSION),
)

PXR_EXTERNAL_NAMESPACE = "pxr"
PXR_INTERNAL_NAMESPACE = "pxrInternal_v{}_{}_{}".format(
    PXR_MAJOR_VERSION,
    PXR_MINOR_VERSION,
    PXR_PATCH_VERSION,
)
