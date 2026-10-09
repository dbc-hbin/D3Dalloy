#include "wsi_platform.hpp"
#include <cstdlib>

namespace dxmt::wsi {

void *aligned_malloc(size_t size, size_t alignment) {
  void *ptr = nullptr;
  return posix_memalign(&ptr, alignment, size) ? nullptr : ptr;
}

void aligned_free(void *ptr) { return free(ptr); }

} // namespace dxmt::wsi