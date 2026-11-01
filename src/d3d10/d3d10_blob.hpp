#pragma once
#include "d3dcommon.h"

namespace d3dalloy {

HRESULT CreateBlobFromMalloc(SIZE_T Size, ID3D10Blob **ppBlob);

}; // namespace d3dalloy