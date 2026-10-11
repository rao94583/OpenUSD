// Phase 0 smoke test: arch links and runs. Replaced by ported tests later.

#include "pxr/pxr.h"
#include "pxr/base/arch/systemInfo.h"

#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE

int main()
{
    const std::string exe = ArchGetExecutablePath();
    std::printf("executable: %s\npage size: %d\n",
                exe.c_str(), ArchGetPageSize());
    return (!exe.empty() && ArchGetPageSize() > 0) ? 0 : 1;
}
