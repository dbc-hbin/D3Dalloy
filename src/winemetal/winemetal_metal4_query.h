// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "winemetal_metal4.h"

/* ABI 314 query domain. All handles, indices, sizes and results occupy complete
 * uint64_t slots; no embedded pointer or host-sized member crosses WoW64. */
enum WMTMetal4QueryOperation {
  WMTMetal4QueryCreateTimestampHeap = 192, /* device,count -> retained heap */
  WMTMetal4QueryTimestampFrequency = 193, /* device -> ticks/second */
  WMTMetal4QueryTimestampEntrySize = 194, /* device -> bytes */
  WMTMetal4QueryWriteTimestamp = 195, /* command context,heap,index */
  WMTMetal4QueryResolveTimestamps = 196, /* command,heap,first,count,buffer,offset,wait,update */
  WMTMetal4QuerySampleCalibration = 197, /* device -> CPU host ns,GPU heap ticks */
  WMTMetal4QuerySetVisibilityMode = 198 /* render context,mode,byte offset */
};

struct WMTMetal4TimestampHeapEntry {
  uint64_t timestamp;
};

#ifdef __cplusplus
static_assert(sizeof(WMTMetal4TimestampHeapEntry) == 8);
#endif
