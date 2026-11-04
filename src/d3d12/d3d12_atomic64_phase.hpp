#pragma once

#include "d3d12.h"
#include "com/com_pointer.hpp"
#include "Metal4.hpp"
#include "d3dalloy_buffer.hpp"
#include "d3dalloy_texture.hpp"
#include <memory>
#include <span>
#include <vector>

namespace d3dalloy {
class MTLD3D12Device;
class MTLD3D12Resource;
class Atomic64PhaseCache;
struct StandardSwizzleTableUse;
struct ShaderImmutableConstants;

struct Atomic64PhasePipelines {
  WMT::Reference<WMT::ComputePipelineState> import;
  WMT::Reference<WMT::ComputePipelineState> export_buffers;
  WMT::Reference<WMT::ComputePipelineState> export_2d;
  WMT::Reference<WMT::ComputePipelineState> export_array;
  WMT::Reference<WMT::ComputePipelineState> export_3d;
};

struct Atomic64RootBinding {
  uint64_t address = 0;
  uint64_t byteLength = UINT64_MAX;
  bool writable = false;
};

struct Atomic64BufferBinding {
  Rc<BufferAllocation> allocation;
  uint64_t byteOffset = 0;
  uint64_t byteLength = 0;
  bool writable = false;
  Rc<Buffer> buffer;
  BufferViewKey view{};
  uint32_t firstElement = 0;
  // Internal node payloads may have no public D3D12 GPU-VA registration.
  WMT::Reference<WMT::Buffer> nativeBuffer;
  uint64_t nativeAddress = 0;
  uint64_t nativeLength = 0;
};

struct Atomic64TextureBinding {
  Rc<Texture> texture;
  Rc<TextureAllocation> allocation;
  TextureViewKey view{};
  bool writable = false;
  uint16_t first_w_slice = 0;
  uint16_t w_size = 0;
  uint64_t shader_resource_id = 0;
};

struct Atomic64PhaseBindings {
  std::vector<Atomic64BufferBinding> buffers;
  std::vector<Atomic64TextureBinding> textures;
  // The phase and its inflight lease retain the D3D12 owners, not just their
  // native allocations; view metadata/counter/reserved state belongs to them.
  std::vector<Com<MTLD3D12Resource, false>> resources;
};

// Appends global/local root bindings; preparation merges repeated descriptors.
HRESULT CollectAtomic64PhaseBindings(MTLD3D12Device *device,
    std::span<const StandardSwizzleTableUse> tables, std::span<const Atomic64RootBinding> roots,
    Atomic64PhaseBindings &bindings);

// The queue retains this owner until actual Metal retirement, not commit.
// Prepare/import/app/export run under one SC phase permit. Unknown/aliasing
// work retains device-wide exclusion; proven private contexts use finite access
// admission. Legacy stages additionally publish/clear the shared header.
class Atomic64Phase {
  struct Storage;
public:
  explicit Atomic64Phase(std::shared_ptr<Storage> storage);
  ~Atomic64Phase();
  // Queue phases can publish the context in an existing texture-import blit.
  // The default retains the separate publication contract used by work graphs.
  HRESULT EncodeImport(WMT::MTL4CommandBuffer command_buffer, WMT::Fence fence, bool publish_context = false);
  HRESULT EncodeExport(WMT::MTL4CommandBuffer command_buffer, WMT::Fence fence);
  HRESULT PublishContext(WMT::MTL4CommandBuffer command_buffer, WMT::Fence fence);
  HRESULT ClearContext(WMT::MTL4CommandBuffer command_buffer, WMT::Fence fence);
  uint64_t ContextAddress() const;
  HRESULT CreatePrivateConstants(const ShaderImmutableConstants &source);
  WMT::Buffer ConstantsBuffer() const { return constants_; }
  uint64_t ConstantsOffset() const { return constants_offset_; }
  // Only successful export/clear retirement returns this context to the cache.
  // Dropping an unsubmitted or failed lease never makes it reusable.
  void MarkRetired();

private:
  std::shared_ptr<Storage> storage_;
  WMT::Reference<WMT::Buffer> constants_;
  uint64_t constants_offset_ = 0;
  friend class Atomic64PhaseCache;
  friend HRESULT PrepareAtomic64Phase(MTLD3D12Device *, uint64_t, const Atomic64PhaseBindings &,
                                      std::shared_ptr<Atomic64Phase> &, std::shared_ptr<Atomic64PhaseCache> &);
};

// The cache belongs to one queue, whose device reference outlives its entries.
// It is created lazily; it must not be owned by the resources' parent device.
HRESULT PrepareAtomic64Phase(MTLD3D12Device *device, uint64_t participant_bound,
    const Atomic64PhaseBindings &bindings, std::shared_ptr<Atomic64Phase> &phase,
    std::shared_ptr<Atomic64PhaseCache> &cache);
} // namespace d3dalloy
