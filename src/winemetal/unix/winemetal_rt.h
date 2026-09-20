#ifndef __WINEMETAL_UNIX_RT_H
#define __WINEMETAL_UNIX_RT_H

#include "../winemetal.h"

obj_handle_t winemetal_rt_new_compute_pipeline(
    obj_handle_t device, const struct WMTComputePipelineInfo *info,
    const obj_handle_t *linked_functions, uint32_t linked_function_count,
    obj_handle_t *error_out);
obj_handle_t winemetal_rt_new_visible_function_table(obj_handle_t pipeline, uint32_t function_count);
obj_handle_t winemetal_rt_new_intersection_function_table(obj_handle_t pipeline, uint32_t function_count);
obj_handle_t winemetal_rt_function_handle(obj_handle_t pipeline, obj_handle_t function);
uint64_t winemetal_rt_max_total_threads_per_threadgroup(obj_handle_t pipeline);
void winemetal_rt_visible_function_table_set(obj_handle_t table, obj_handle_t function_handle, uint64_t index);
void winemetal_rt_intersection_function_table_set(
    obj_handle_t table, obj_handle_t function_handle, uint64_t index);

#endif
