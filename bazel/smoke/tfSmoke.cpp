// Phase 0 smoke test: tf links (including libpython) and its static
// registrations survive linking. Nothing here references debugNotice.cpp, so
// TfDebugSymbolsChangedNotice is only registered if alwayslink kept it.

#include "pxr/pxr.h"
#include "pxr/base/tf/notice.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/tf/type.h"

#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE

int main()
{
    const TfType t = TfType::FindByName("TfDebugSymbolsChangedNotice");
    std::printf("TfDebugSymbolsChangedNotice registered: %s\n",
                t.IsUnknown() ? "no" : "yes");
    if (t.IsUnknown() || !t.IsA<TfNotice>()) {
        return 1;
    }

    const TfToken token("hello");
    const std::string s = TfStringPrintf("%s-%d", token.GetText(), 42);
    std::printf("%s\n", s.c_str());
    return s == "hello-42" ? 0 : 1;
}
