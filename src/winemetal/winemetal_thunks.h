
#ifndef __WINEMETAL_THUNKS_H
#define __WINEMETAL_THUNKS_H

#include "winemetal.h"
#include <stddef.h>

#pragma pack(push, 8)

/* No native pointers: the identical structure is consumed by PE32, PE64,
 * and the native/WoW64 tables. argument is a zero-extended caller address. */
struct WMTCompositionCall {
  uint32_t operation;
  uint32_t count;
  uint64_t object;
  uint64_t argument;
  uint64_t result;
  int32_t status;
  uint32_t reserved;
};
struct WMTCompositionCall_DATA32 {
  uint32_t operation;
  uint32_t count;
  uint64_t object;
  uint64_t argument;
  uint64_t result;
  int32_t status;
  uint32_t reserved;
};
#ifdef __cplusplus
static_assert(sizeof(WMTCompositionCall) == 40);
static_assert(sizeof(WMTCompositionCall_DATA32) == 40);
static_assert(offsetof(WMTCompositionCall, argument) == 16);
static_assert(offsetof(WMTCompositionCall_DATA32, argument) == 16);
static_assert(offsetof(WMTCompositionFrame, restrict_output) == 32);
static_assert(offsetof(WMTCompositionFrame, restricted_display_id) == 36);
#else
_Static_assert(sizeof(struct WMTCompositionCall) == 40, "composition native/PE ABI");
_Static_assert(sizeof(struct WMTCompositionCall_DATA32) == 40, "composition WoW64 ABI");
_Static_assert(sizeof(struct WMTCompositionTargetState) == 24, "composition target ABI");
_Static_assert(sizeof(struct WMTCompositionFrame) == 40, "composition frame ABI");
_Static_assert(offsetof(struct WMTCompositionCall, argument) == 16, "composition caller pointer ABI");
_Static_assert(offsetof(struct WMTCompositionCall_DATA32, argument) == 16, "composition PE32 caller pointer ABI");
_Static_assert(offsetof(struct WMTCompositionFrame, restrict_output) == 32, "composition restrict flag ABI");
_Static_assert(offsetof(struct WMTCompositionFrame, restricted_display_id) == 36, "composition display ID ABI");
#endif

struct unixcall_get_pipeline_cache_identity {
  struct WMTPipelineCacheIdentity identity;
  uint64_t ret_success;
};

struct unixcall_generic_obj_ret {
  obj_handle_t ret;
};

struct unixcall_generic_obj_noret {
  obj_handle_t handle;
};

struct unixcall_generic_obj_obj_ret {
  obj_handle_t handle;
  obj_handle_t ret;
};

struct unixcall_generic_obj_uint64_obj_ret {
  obj_handle_t handle;
  uint64_t arg;
  obj_handle_t ret;
};

struct unixcall_generic_obj_uint64_ret {
  obj_handle_t handle;
  uint64_t ret;
};

struct unixcall_generic_obj_uint64_noret {
  obj_handle_t handle;
  uint64_t arg;
};

struct unixcall_generic_obj_ptr_noret {
  obj_handle_t handle;
  struct WMTMemoryPointer arg;
};

struct unixcall_generic_obj_constptr_noret {
  obj_handle_t handle;
  struct WMTConstMemoryPointer arg;
};

struct unixcall_generic_obj_obj_uint64_noret {
  obj_handle_t handle;
  obj_handle_t arg0;
  uint64_t arg1;
};

struct unixcall_generic_obj_uint64_uint64_ret {
  obj_handle_t handle;
  uint64_t arg;
  uint64_t ret;
};

struct unixcall_nsstring_getcstring {
  obj_handle_t str;
  uint64_t buffer_ptr;
  uint64_t max_length;
  enum WMTStringEncoding encoding;
  uint32_t ret;
};

struct unixcall_mtldevice_newbuffer {
  obj_handle_t device;
  struct WMTMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newsamplerstate {
  obj_handle_t device;
  struct WMTMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newdepthstencilstate {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newtexture {
  obj_handle_t device;
  struct WMTMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtlbuffer_newtexture {
  obj_handle_t buffer;
  struct WMTMemoryPointer info;
  uint64_t offset;
  uint64_t bytes_per_row;
  obj_handle_t ret;
};

struct unixcall_mtltexture_newtextureview {
  obj_handle_t texture;
  enum WMTPixelFormat format;
  enum WMTTextureType texture_type;
  uint16_t level_start;
  uint16_t level_count;
  uint16_t slice_start;
  uint16_t slice_count;
  struct WMTTextureSwizzleChannels swizzle;
  obj_handle_t ret;
  uint64_t gpu_resource_id;
};

struct unixcall_mtldevice_newlibrary {
  obj_handle_t device;
  obj_handle_t data;
  obj_handle_t ret_error;
  obj_handle_t ret_library;
};

struct unixcall_mtldevice_newlibrarywithsource {
  obj_handle_t device;
  struct WMTConstMemoryPointer source;
  uint64_t length;
  obj_handle_t ret_error;
  obj_handle_t ret_library;
};

struct unixcall_mtldevice_newcomputepso {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret_error;
  obj_handle_t ret_pso;
};

struct unixcall_mtldevice_newrenderpso {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret_error;
  obj_handle_t ret_pso;
};

struct unixcall_mtldevice_newmeshrenderpso {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret_error;
  obj_handle_t ret_pso;
};

struct unixcall_generic_obj_cmd_noret {
  obj_handle_t encoder;
  struct WMTConstMemoryPointer cmd_head;
};

struct unixcall_mtltexture_replaceregion {
  obj_handle_t texture;
  struct WMTOrigin origin;
  struct WMTSize size;
  uint64_t level;
  uint64_t slice;
  struct WMTMemoryPointer data;
  uint64_t bytes_per_row;
  uint64_t bytes_per_image;
};

struct unixcall_generic_obj_obj_noret {
  obj_handle_t handle;
  obj_handle_t arg;
};

struct unixcall_generic_obj_obj_obj_noret {
  obj_handle_t handle;
  obj_handle_t arg0;
  obj_handle_t arg1;
};

struct unixcall_generic_obj_obj_obj_uint64_ret {
  obj_handle_t handle;
  obj_handle_t arg0;
  obj_handle_t arg1;
  uint64_t ret;
};

struct unixcall_generic_obj_obj_double_noret {
  obj_handle_t handle;
  obj_handle_t arg0;
  double arg1;
};

struct unixcall_mtlcapturemanager_startcapture {
  obj_handle_t capture_manager;
  struct WMTMemoryPointer info;
  bool ret;
};

struct unixcall_mtldevice_newfxtemporalscaler {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newfxspatialscaler {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtlcommandbuffer_temporal_scale {
  obj_handle_t cmdbuf;
  obj_handle_t scaler;
  obj_handle_t color;
  obj_handle_t output;
  obj_handle_t depth;
  obj_handle_t motion;
  obj_handle_t exposure;
  obj_handle_t fence;
  struct WMTConstMemoryPointer props;
};

struct unixcall_mtlcommandbuffer_spatial_scale {
  obj_handle_t cmdbuf;
  obj_handle_t scaler;
  obj_handle_t color;
  obj_handle_t output;
  obj_handle_t fence;
};

struct unixcall_nsstring_string {
  struct WMTConstMemoryPointer buffer_ptr;
  enum WMTStringEncoding encoding;
  obj_handle_t ret;
};

struct unixcall_create_metal_view_from_hwnd {
  uint64_t hwnd;
  obj_handle_t device;
  obj_handle_t ret_view;
  obj_handle_t ret_layer;
};

struct unixcall_enumerate {
  obj_handle_t enumerable;
  uint64_t start;
  uint64_t buffer_size;
  struct WMTMemoryPointer buffer;
  uint64_t ret_read;
};

struct unixcall_mtllibrary_newfunction_with_constants {
  obj_handle_t library;
  struct WMTConstMemoryPointer name;
  struct WMTConstMemoryPointer constants;
  uint64_t num_constants;
  obj_handle_t ret;
  obj_handle_t ret_error;
};

struct unixcall_query_display_setting {
  uint64_t display_id;
  enum WMTColorSpace colorspace;
  struct WMTMemoryPointer hdr_metadata;
  bool ret;
};

struct unixcall_update_display_setting {
  uint64_t display_id;
  enum WMTColorSpace colorspace;
  struct WMTConstMemoryPointer hdr_metadata;
};

struct unixcall_query_display_setting_for_layer {
  obj_handle_t layer;
  uint64_t version;
  enum WMTColorSpace colorspace;
  struct WMTMemoryPointer hdr_metadata;
  struct WMTEDRValue edr_value;
};

struct unixcall_mtlbuffer_updatecontents {
  obj_handle_t buffer;
  uint64_t offset;
  struct WMTConstMemoryPointer data;
  uint64_t length;
};

struct unixcall_mtlsharedevent_setevent {
  obj_handle_t event_handle;
  obj_handle_t shared_event_listener;
  struct WMTConstMemoryPointer values;
  uint32_t count;
  uint32_t wait_all;
  uint32_t ret;
};

struct unixcall_mtlsharedevent_createmachport {
  obj_handle_t event;
  uint32_t ret_mach_port;
};

struct unixcall_mtldevice_newsharedeventwithmachport {
  obj_handle_t device;
  uint32_t mach_port;
  obj_handle_t ret_event;
};

struct unixcall_get_os_version {
  uint64_t ret_major;
  uint64_t ret_minor;
  uint64_t ret_patch;
};

struct unixcall_mtldevice_newbinaryarchive {
  obj_handle_t device;
  struct WMTConstMemoryPointer url;
  obj_handle_t ret_archive;
  obj_handle_t ret_error;
};

struct unixcall_mtlbinaryarchive_serialize {
  obj_handle_t archive;
  struct WMTConstMemoryPointer url;
  obj_handle_t ret_error;
};

struct unixcall_cache_alloc_init {
  struct WMTConstMemoryPointer path;
  uint64_t version;
  obj_handle_t ret_cache;
};

struct unixcall_cache_get {
  obj_handle_t cache;
  struct WMTConstMemoryPointer key;
  uint64_t key_length;
  obj_handle_t ret_data;
};

struct unixcall_cache_set {
  obj_handle_t cache;
  struct WMTConstMemoryPointer key;
  uint64_t key_length;
  obj_handle_t value_data;
};

struct unixcall_setmetalcachepath {
  struct WMTConstMemoryPointer path;
  uint64_t ret_success;
};

struct unixcall_bootstrap {
  char name[128];
  uint32_t mach_port;
  uint32_t reserved;
};

/* Fixed-width for WoW64: shared by the 64-bit and 32-bit tables (232..237). */
struct unixcall_share_rendezvous {
  char name[64];   // out (create) / in (lookup)
  uint64_t handle; // rendezvous owner handle
  uint32_t port;   // in (create: object send right) / out (lookup: object send right)
  uint32_t reserved;
};

struct unixcall_share_identity {
  obj_handle_t object;
  uint32_t kind; // 0: MTLSharedEvent, 1: shareable MTLTexture
  uint32_t reserved;
  uint64_t ret;
};

struct unixcall_share_hashname {
  uint16_t name[256];
  uint32_t length; // UTF-16 code units, 1..256
  char hash[36];   // out: 32 hex digits
};

struct unixcall_mtlsharedevent_waituntilsignaledvalue {
  obj_handle_t event;
  uint64_t value;
  uint64_t timeout_ms;
  bool ret_timeout;
};

struct unixcall_mtlcountersamplebuffer_newtimestampbuffer {
  obj_handle_t device;
  uint32_t sample_count;
  uint32_t shared;
  obj_handle_t ret;
};

struct unixcall_mtlcountersamplebuffer_resolvecounterrange {
  obj_handle_t sample_buffer;
  uint32_t start;
  uint32_t len;
  struct WMTMemoryPointer data_out;
  uint64_t data_length;
};

struct unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers {
  obj_handle_t cmdbuf;
  struct WMTMemoryPointer attachments;
  uint64_t num_attachments;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newresidencyset {
  obj_handle_t device;
  uint64_t init_capacity;
  obj_handle_t ret_error;
  obj_handle_t ret_set;
};

struct unixcall_mtlresidencyset_addallocations {
  obj_handle_t set;
  struct WMTConstMemoryPointer allocations;
  uint64_t count;
};

struct unixcall_mtldevice_newheap {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtldevice_heapbuffersizeandalign {
  obj_handle_t device;
  uint64_t length;
  enum WMTResourceOptions options;
  uint32_t ret_size;
  uint32_t ret_align;
};

struct unixcall_mtldevice_heaptexturesizeandalign {
  obj_handle_t device;
  struct WMTMemoryPointer info;
  uint32_t ret_size;
  uint32_t ret_align;
};

struct unixcall_mtlheap_newbuffer {
  obj_handle_t heap;
  struct WMTMemoryPointer info;
  uint64_t offset;
  obj_handle_t ret;
};

struct unixcall_mtlheap_newtexture {
  obj_handle_t heap;
  struct WMTMemoryPointer info;
  uint64_t offset;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newicb {
  obj_handle_t device;
  struct WMTMemoryPointer info;
  uint64_t max_count;
  uint64_t options;
  obj_handle_t ret;
};

/* Raytracing pipeline primitives. */
struct unixcall_mtldevice_newcomputepipeline_linked {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  struct WMTConstMemoryPointer functions;
  uint32_t count;
  obj_handle_t error;
  obj_handle_t ret;
};

struct unixcall_mtlpipeline_newfunctiontable {
  obj_handle_t pipeline;
  uint32_t count;
  obj_handle_t ret;
};

struct unixcall_mtlpipeline_functionhandle {
  obj_handle_t pipeline;
  obj_handle_t function;
  obj_handle_t ret;
};

struct unixcall_mtlpipeline_maxthreads {
  obj_handle_t pipeline;
  uint64_t ret;
};

struct unixcall_mtlfunctiontable_set {
  obj_handle_t table;
  obj_handle_t function;
  uint64_t index;
};

/* The intersection function table's own buffer argument table. `buffer` carries either an
 * MTLBuffer (with `offset`) or an MTLVisibleFunctionTable, whichever the call sets. */
struct unixcall_mtlintersectiontable_setbuffer {
  obj_handle_t table;
  obj_handle_t buffer;
  uint64_t offset;
  uint64_t index;
};

/* Acceleration-structure calls. Every argument is transported as a 64-bit word so one
 * structure covers all of them; handles and byte sizes are both naturally 64-bit here. */
struct unixcall_wmt_as {
  /* The prepass entry carries the most arguments: 23. */
  uint64_t args[24];
  uint64_t ret[3];
};

struct unixcall_wmt_resource_id {
  obj_handle_t resource;
  uint64_t ret;
};

struct unixcall_mtldevice_sampletimestamps {
  obj_handle_t device;
  uint64_t cpu_timestamp;
  uint64_t gpu_timestamp;
};

struct unixcall_wmt_presentation_observe {
  obj_handle_t drawable;
  obj_handle_t observer;
  uint64_t present_id;
  uint32_t ret;
};

struct unixcall_wmt_presentation_wait {
  obj_handle_t observer;
  struct WMTPresentationEvent event;
  int32_t ret;
};

/* All fields are 8 bytes, so the layout is identical for WoW64 callers. */
struct unixcall_mtlcommandbuffer_present_if_current {
  obj_handle_t cmdbuf;
  obj_handle_t drawable;
  obj_handle_t token;
  uint64_t epoch;
  double after;
  obj_handle_t observer;
  uint64_t present_id;
};

struct unixcall_wmt_presentation_cancel {
  obj_handle_t observer;
  uint64_t present_id;
};

struct unixcall_wmt_clock_sample {
  double host_seconds;
};

struct unixcall_wmt_observer_view {
  obj_handle_t observer;
  obj_handle_t view;
  uint32_t ret;
};

struct unixcall_wmt_refresh_sample {
  obj_handle_t observer;
  struct WMTDisplayRefreshSample sample;
  uint32_t ret;
};

struct unixcall_mtlcomputeencoder_functiontable {
  obj_handle_t encoder;
  obj_handle_t table;
  uint64_t index;
};

struct unixcall_mtldevice_newplacementheap {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  uint64_t max_sparse_page_size;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newplacementsparseresource {
  obj_handle_t device;
  struct WMTMemoryPointer info;
  uint64_t page_size;
  obj_handle_t ret;
};

struct unixcall_mtlbuffer_newplacementsparsetextureview {
  obj_handle_t buffer;
  struct WMTMemoryPointer info;
  uint64_t offset;
  uint64_t bytes_per_row;
  uint64_t page_size;
  obj_handle_t ret;
};

struct unixcall_mtldevice_sparsetilesize {
  obj_handle_t device;
  uint32_t type;
  uint32_t format;
  uint32_t sample_count;
  uint32_t page_size;
  struct WMTSparseTileShape ret;
};

struct unixcall_mtltexture_sparsetail {
  obj_handle_t texture;
  struct WMTSparseTextureTail ret;
};

struct unixcall_mtl4commandqueue_updatemappings {
  obj_handle_t queue;
  obj_handle_t resource;
  obj_handle_t heap;
  struct WMTConstMemoryPointer mappings;
  uint64_t count;
};

struct unixcall_mtl4commandqueue_signalevent {
  obj_handle_t queue;
  obj_handle_t event;
  uint64_t value;
};

struct unixcall_mtldevice_newsamplerstatewithreduction {
  obj_handle_t device;
  struct WMTMemoryPointer info;
  uint64_t reduction;
  obj_handle_t ret;
};

#pragma pack(pop)

#endif
