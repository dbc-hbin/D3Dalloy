#pragma once
#include <cstdint>
#include <cstring>
#include <span>

namespace d3dalloy {

// The source has already been relocated for the immutable PSO. Rebase every
// pool-relative pointer, replace every context pointer, and retain absolute
// device lock-table pointers byte for byte. No shared PSO word is modified.
template<class Relocation>
bool CopyAtomic64Constants(void *destination, const void *source, uint64_t size, uint64_t address,
    uint64_t context, std::span<const Relocation> relocations, uint64_t context_target, uint64_t lock_target) {
  if (!destination || !source || !context || !size || size > SIZE_MAX) return false;
  bool has_context = false;
  for (const auto &relocation : relocations) {
    if (size < sizeof(uint64_t) || relocation.Offset > size - sizeof(uint64_t)) return false;
    has_context |= relocation.TargetOffset == context_target;
  }
  if (!has_context) return false;
  std::memcpy(destination, source, size);
  auto *bytes = static_cast<uint8_t *>(destination);
  for (const auto &relocation : relocations) {
    if (relocation.TargetOffset == lock_target) continue;
    const uint64_t value = relocation.TargetOffset == context_target ? context : address + relocation.TargetOffset;
    std::memcpy(bytes + relocation.Offset, &value, sizeof(value));
  }
  return true;
}

} // namespace d3dalloy
