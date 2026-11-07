#include "../winemetal_metal4.h"

/* Shared by the bridge and owned temporal providers, including standalone probes.
 * Opaque MetalFX framework work is outside this accounting boundary. */
static uint32_t counters[WMTMetal4HUDCounterCount];

void m4_hud_count(enum WMTMetal4HUDCounter counter) {
  __atomic_fetch_add(&counters[counter], 1, __ATOMIC_RELAXED);
}

void m4_hud_drain(uint64_t *values) {
  for (unsigned i = 0; i < WMTMetal4HUDCounterCount; ++i)
    values[i] = __atomic_exchange_n(&counters[i], 0, __ATOMIC_RELAXED);
}
