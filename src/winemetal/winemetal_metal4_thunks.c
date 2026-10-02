#ifdef _WIN32
#define WINEMETAL_API __declspec(dllexport)
#else
#define WINEMETAL_API
#endif
#include "winemetal_metal4.h"
#include <wineunixlib.h>

WINEMETAL_API int32_t WMTMetal4Call(struct WMTMetal4Call *params) {
  return WINE_UNIX_CALL(314, params);
}
