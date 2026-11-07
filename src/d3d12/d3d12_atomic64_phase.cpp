#include "d3d12_atomic64_phase.hpp"
#include "d3d12_atomic64_gpu.hpp"
#include "d3d12_atomic64_ranges.hpp"
#include "d3d12_atomic64_storage.hpp"
#include "d3d12_device.hpp"
#include "d3d12_reserved_resource.hpp"
#include "d3dalloy_format.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace d3dalloy {
namespace {
using atomic64::CanonicalKind;
using atomic64::CanonicalOwner;

constexpr size_t kAtomic64CacheEntries = 4;
constexpr uint64_t kAtomic64CacheBytes = 64 * 1024 * 1024;

CanonicalOwner BufferOwner(BufferAllocation *allocation) {
  if (allocation->canonicalHeap)
    return {CanonicalKind::HeapBytes, allocation->canonicalHeap.handle, allocation->canonicalHeapOffset};
  if (allocation->mappedMemory(0))
    return {CanonicalKind::HostBytes, 0, reinterpret_cast<uintptr_t>(allocation->mappedMemory(0))};
  return {CanonicalKind::Buffer, allocation->buffer().handle};
}

struct UIntStorage {
  WMTPixelFormat format;
  uint32_t channel_bits;
  uint32_t channels;
};

std::optional<UIntStorage> UIntFormat(WMTPixelFormat encoded) {
  switch (ORIGINAL_FORMAT(encoded)) {
  case WMTPixelFormatA8Unorm:
  case WMTPixelFormatR8Unorm: case WMTPixelFormatR8Unorm_sRGB:
  case WMTPixelFormatR8Snorm: case WMTPixelFormatR8Uint: case WMTPixelFormatR8Sint:
    return UIntStorage{WMTPixelFormatR8Uint, 8, 1};
  case WMTPixelFormatR16Unorm: case WMTPixelFormatR16Snorm: case WMTPixelFormatR16Uint:
  case WMTPixelFormatR16Sint: case WMTPixelFormatR16Float:
    return UIntStorage{WMTPixelFormatR16Uint, 16, 1};
  case WMTPixelFormatRG8Unorm: case WMTPixelFormatRG8Unorm_sRGB: case WMTPixelFormatRG8Snorm:
  case WMTPixelFormatRG8Uint: case WMTPixelFormatRG8Sint:
    return UIntStorage{WMTPixelFormatRG8Uint, 0x0808, 2};
  case WMTPixelFormatR32Uint: case WMTPixelFormatR32Sint: case WMTPixelFormatR32Float:
    return UIntStorage{WMTPixelFormatR32Uint, 32, 1};
  case WMTPixelFormatRG16Unorm: case WMTPixelFormatRG16Snorm: case WMTPixelFormatRG16Uint:
  case WMTPixelFormatRG16Sint: case WMTPixelFormatRG16Float:
    return UIntStorage{WMTPixelFormatRG16Uint, 0x1010, 2};
  case WMTPixelFormatRGBA8Unorm: case WMTPixelFormatRGBA8Unorm_sRGB:
  case WMTPixelFormatRGBA8Snorm: case WMTPixelFormatRGBA8Uint: case WMTPixelFormatRGBA8Sint:
    return UIntStorage{WMTPixelFormatRGBA8Uint, 0x08080808, 4};
  case WMTPixelFormatRG32Uint: case WMTPixelFormatRG32Sint: case WMTPixelFormatRG32Float:
    return UIntStorage{WMTPixelFormatRG32Uint, 0x2020, 2};
  case WMTPixelFormatRGBA16Unorm: case WMTPixelFormatRGBA16Snorm: case WMTPixelFormatRGBA16Uint:
  case WMTPixelFormatRGBA16Sint: case WMTPixelFormatRGBA16Float:
    return UIntStorage{WMTPixelFormatRGBA16Uint, 0x10101010, 4};
  case WMTPixelFormatRGBA32Uint: case WMTPixelFormatRGBA32Sint: case WMTPixelFormatRGBA32Float:
    return UIntStorage{WMTPixelFormatRGBA32Uint, 0x20202020, 4};
  case WMTPixelFormatRGB10A2Unorm: case WMTPixelFormatRGB10A2Uint:
    return UIntStorage{WMTPixelFormatRGB10A2Uint, 0x020a0a0a, 4};
  default: return std::nullopt;
  }
}
} // namespace

HRESULT CollectAtomic64PhaseBindings(MTLD3D12Device *device, std::span<const StandardSwizzleTableUse> tables,
    std::span<const Atomic64RootBinding> roots, Atomic64PhaseBindings &bindings) {
  const size_t buffer_count = bindings.buffers.size();
  const size_t texture_count = bindings.textures.size();
  const size_t resource_count = bindings.resources.size();
  try {
    auto append_buffer = [&](Buffer *buffer, BufferSlice slice, bool writable, BufferViewKey view = {}) {
      if (!buffer || !buffer->current() || !slice.byteLength) return;
      bindings.buffers.push_back({buffer->current(), slice.byteOffset, slice.byteLength, writable, buffer, view, slice.firstElement});
    };
    auto append_descriptor = [&](const ShaderVisibleDescriptorCPUStorage &descriptor, uint64_t texture_id) {
      if (descriptor.type != ShaderVisibleDescriptorType::ConstantBuffer) {
        const void *backing = nullptr;
        switch (descriptor.type) {
        case ShaderVisibleDescriptorType::SRVTexture: backing = descriptor.SRVTexture.texture; break;
        case ShaderVisibleDescriptorType::UAVTexture: backing = descriptor.UAVTexture.texture; break;
        case ShaderVisibleDescriptorType::SRVTexelBuffer: backing = descriptor.SRVTexelBuffer.buffer; break;
        case ShaderVisibleDescriptorType::UAVTexelBuffer: backing = descriptor.UAVTexelBuffer.buffer; break;
        case ShaderVisibleDescriptorType::SRVBuffer: backing = descriptor.SRVBuffer.buffer; break;
        case ShaderVisibleDescriptorType::SRVRGB32Buffer: backing = descriptor.SRVRGB32Buffer.buffer; break;
        case ShaderVisibleDescriptorType::UAVBuffer: backing = descriptor.UAVBuffer.buffer; break;
        case ShaderVisibleDescriptorType::SamplerFeedback: backing = descriptor.SamplerFeedback.buffer; break;
        default: return;
        }
        if (!backing || !descriptor.owner_id) return;
        auto resource = device->LookupDescriptorResource(backing, descriptor.owner_id);
        if (!resource) return;
        bindings.resources.push_back(std::move(resource));
      }
      switch (descriptor.type) {
      case ShaderVisibleDescriptorType::SRVTexture:
        if (auto *texture = descriptor.SRVTexture.texture; texture && texture->current())
          bindings.textures.push_back({texture, texture->current(), descriptor.SRVTexture.view, false, 0, 0, texture_id});
        break;
      case ShaderVisibleDescriptorType::UAVTexture:
        if (auto *texture = descriptor.UAVTexture.texture; texture && texture->current())
          bindings.textures.push_back({texture, texture->current(), descriptor.UAVTexture.view, true,
              descriptor.UAVTexture.first_w_slice, descriptor.UAVTexture.w_size, texture_id});
        break;
      case ShaderVisibleDescriptorType::SRVTexelBuffer:
        append_buffer(descriptor.SRVTexelBuffer.buffer, descriptor.SRVTexelBuffer.slice, false, descriptor.SRVTexelBuffer.view);
        break;
      case ShaderVisibleDescriptorType::UAVTexelBuffer:
        append_buffer(descriptor.UAVTexelBuffer.buffer, descriptor.UAVTexelBuffer.slice, true, descriptor.UAVTexelBuffer.view);
        break;
      case ShaderVisibleDescriptorType::SRVBuffer:
        append_buffer(descriptor.SRVBuffer.buffer, descriptor.SRVBuffer.slice, false); break;
      case ShaderVisibleDescriptorType::SRVRGB32Buffer:
        append_buffer(descriptor.SRVRGB32Buffer.buffer, descriptor.SRVRGB32Buffer.slice, false); break;
      case ShaderVisibleDescriptorType::UAVBuffer:
        append_buffer(descriptor.UAVBuffer.buffer, descriptor.UAVBuffer.slice, true);
        if (descriptor.UAVBuffer.counter && descriptor.counter_owner_id) {
          auto counter = device->LookupDescriptorResource(descriptor.UAVBuffer.counter, descriptor.counter_owner_id);
          if (counter) {
            bindings.resources.push_back(std::move(counter));
            append_buffer(descriptor.UAVBuffer.counter,
                {descriptor.counter_page * D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT, 4}, true);
          }
        }
        break;
      case ShaderVisibleDescriptorType::SamplerFeedback:
        append_buffer(descriptor.SamplerFeedback.buffer, descriptor.SamplerFeedback.slice, true); break;
      case ShaderVisibleDescriptorType::ConstantBuffer: {
        uint64_t offset = 0;
        auto allocation = device->LookupBufferByVA(descriptor.ConstantBuffer.address, &offset);
        if (allocation && offset <= allocation->byteLength())
          bindings.buffers.push_back({allocation, offset,
              std::min(descriptor.ConstantBuffer.size, allocation->byteLength() - offset), false});
        break;
      }
      default: break;
      }
    };
    for (const auto &table : tables) {
      if (!table.heap || (table.parameter != UINT32_MAX && !table.rootsig)) continue;
      D3D12_DESCRIPTOR_HEAP_DESC desc{};
      D3D12_GPU_DESCRIPTOR_HANDLE start{};
      table.heap->GetDesc(&desc);
      table.heap->GetGPUDescriptorHandleForHeapStart(&start);
      if (!start.ptr || table.base < start.ptr || (table.base - start.ptr) % 32) continue;
      const uint64_t base = (table.base - start.ptr) / 32;
      if (base >= desc.NumDescriptors) continue;
      if (table.parameter == UINT32_MAX) {
        // Only direct heaps need deduplication; ordinary table lists can be large.
        if (std::any_of(tables.data(), &table, [&](const auto &previous) {
              return previous.heap == table.heap && previous.parameter == UINT32_MAX &&
                  previous.base >= start.ptr && (previous.base - start.ptr) % 32 == 0 &&
                  (previous.base - start.ptr) / 32 < desc.NumDescriptors;
            }))
          continue;
        for (uint32_t index = 0; index < desc.NumDescriptors; ++index)
          append_descriptor(table.heap->GetDescriptor(index), table.heap->GetTextureResourceId(index));
        continue;
      }
      if (table.restrict_to_shader) {
        table.rootsig->ForEachUsedDescriptorRange(table.rootsig->GetTableRanges(table.parameter), table.shader_bindings,
            [&](uint64_t offset, uint64_t count, D3D12_DESCRIPTOR_RANGE_TYPE) {
          if (offset >= desc.NumDescriptors - base) return;
          const uint64_t first = base + offset;
          count = std::min<uint64_t>(count, desc.NumDescriptors - first);
          for (uint64_t index = 0; index < count; ++index)
            append_descriptor(table.heap->GetDescriptor(first + index), table.heap->GetTextureResourceId(first + index));
        });
        continue;
      }
      for (const auto &range : table.rootsig->GetTableRanges(table.parameter)) {
        if (range.type != D3D12_DESCRIPTOR_RANGE_TYPE_SRV && range.type != D3D12_DESCRIPTOR_RANGE_TYPE_UAV &&
            range.type != D3D12_DESCRIPTOR_RANGE_TYPE_CBV) continue;
        if (range.offset >= desc.NumDescriptors - base) continue;
        const uint64_t first = base + range.offset;
        const uint64_t count = std::min<uint64_t>(range.count, desc.NumDescriptors - first);
        for (uint64_t index = 0; index < count; ++index)
          append_descriptor(table.heap->GetDescriptor(first + index), table.heap->GetTextureResourceId(first + index));
      }
    }
    for (const auto &root : roots) {
      if (!root.address || !root.byteLength) continue;
      uint64_t offset = 0;
      auto allocation = device->LookupBufferByVA(root.address, &offset);
      if (!allocation || offset >= allocation->byteLength()) continue;
      bindings.buffers.push_back({allocation, offset,
          std::min(root.byteLength, allocation->byteLength() - offset), root.writable});
    }
    return S_OK;
  } catch (const std::bad_alloc &) {
    bindings.buffers.resize(buffer_count);
    bindings.textures.resize(texture_count);
    bindings.resources.resize(resource_count);
    return E_OUTOFMEMORY;
  } catch (const std::length_error &) {
    bindings.buffers.resize(buffer_count);
    bindings.textures.resize(texture_count);
    bindings.resources.resize(resource_count);
    return E_OUTOFMEMORY;
  }
}

struct Atomic64Phase::Storage {
  struct BufferStorage {
    WMT::Reference<WMT::Buffer> buffer;
    uint64_t address = 0;
    void *mapped = nullptr;
    uint64_t length = 0;
  };
  struct ExecutionContext {
    BufferStorage roots, nodes, bitmap, baseline, header;
    uint32_t node_count = 0;
  };
  struct NativeSource {
    WMT::Reference<WMT::Allocation> allocation;
    uint64_t bytes = 0;
  };
  struct TextureTransfer {
    struct Region { WMTOrigin origin; WMTSize size; uint32_t slice; uint64_t offset; };
    WMT::Reference<WMT::Texture> source;
    WMT::Reference<WMT::Texture> destination;
    BufferStorage staging;
    uint32_t level = 0, slice = 0;
    uint32_t staging_row_pitch = 0, staging_slice_pitch = 0;
    Atomic64TransferParams params{};
    WMTTextureType type = WMTTextureType2D;
    bool writable = false;
    bool scattered = false;
    std::vector<Region> regions;
  };
  MTLD3D12Device *device;
  // Maps and transfer recipes are immutable after preparation. Texture staging
  // is GPU-written, but its addresses are embedded in the immutable import map,
  // so it is lent exclusively with this entry, never shared by live phases.
  Atomic64PhaseBindings bindings;
  std::vector<uint64_t> key;
  std::vector<NativeSource> native_sources;
  std::vector<BufferStorage> buffers;
  std::vector<WMT::Reference<WMT::Texture>> resident_views;
  std::vector<WMT::Reference<WMT::Heap>> heaps;
  std::vector<Com<MTLD3D12Resource, false>> canonical_resources;
  std::vector<Rc<BufferAllocation>> canonical_allocations;
  std::vector<TextureTransfer> textures;
  atomic64::CanonicalCellMap cells;
  std::vector<Atomic64ImportCell> import_cells, export_cells;
  std::map<ReservedResource *, std::vector<ReservedTileTarget>> mappings;
  std::vector<SM50_ATOMIC64_RAW_RANGE> ranges;
  std::vector<SM50_ATOMIC64_TYPED_VIEW> views;
  ExecutionContext execution;
  uint64_t raw_ranges = 0, typed_views = 0;
  Atomic64TransferParams params{};
  uint64_t export_address = 0;
  std::shared_ptr<Atomic64PhasePipelines> pipelines;
  uint64_t topology_id = 0, cache_bytes = 0, last_use = 0;
  bool cache_eligible = true, retired = false;

  explicit Storage(MTLD3D12Device *device) : device(device) {}
  ~Storage() {
    for (const auto &view : resident_views) device->UnregisterResidency(view);
    for (const auto &buffer : buffers) device->UnregisterResidency(buffer.buffer);
    for (const auto *buffer : {&execution.roots, &execution.nodes, &execution.bitmap,
                              &execution.baseline, &execution.header})
      if (buffer->buffer) device->UnregisterResidency(buffer->buffer);
  }

  void ReleaseBindingOwners() {
    // A cached entry keeps only native snapshots. Keeping a resource owner,
    // Buffer/Texture::current(), or their view caches while idle could retain
    // newly remapped heaps or renamed/private allocations outside our budget.
    bindings.buffers.clear();
    bindings.textures.clear();
    canonical_allocations.clear();
    bindings.resources.clear();
    canonical_resources.clear();
  }

  HRESULT AllocateBuffer(uint64_t length, BufferStorage &result) {
    if (!length) length = 16;
    if (length > device->GetMTLDevice().maxBufferLength() || length > SIZE_MAX) return E_OUTOFMEMORY;
    WMTBufferInfo info{};
    info.length = length;
    info.options = WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked | WMT_RESOURCE_CPU_ADDRESSABLE;
    result.buffer = device->GetMTLDevice().newBuffer(info);
    result.address = info.gpu_address;
    result.mapped = info.memory.get_accessible_or_null();
    result.length = length;
    if (!result.buffer || !result.address || !result.mapped) return E_OUTOFMEMORY;
    return S_OK;
  }

  HRESULT NewBuffer(uint64_t length, BufferStorage &result) {
    HRESULT hr = AllocateBuffer(length, result);
    if (FAILED(hr)) return hr;
    buffers.push_back(result);
    hr = device->RegisterResidency(result.buffer);
    if (FAILED(hr)) { buffers.pop_back(); return hr; }
    return S_OK;
  }

  HRESULT EnsureContextBuffer(uint64_t length, BufferStorage &buffer) {
    if (buffer.buffer && buffer.length >= length) return S_OK;
    BufferStorage replacement;
    HRESULT hr = AllocateBuffer(length, replacement);
    if (FAILED(hr)) return hr;
    hr = device->RegisterResidency(replacement.buffer);
    if (FAILED(hr)) return hr;
    // A cache hit has no live phase owners and was explicitly retired. Register
    // the replacement before releasing the old residency reference.
    if (buffer.buffer) device->UnregisterResidency(buffer.buffer);
    buffer = std::move(replacement);
    return S_OK;
  }

  HRESULT ResetContext(uint64_t participants, bool &reused) {
    const uint32_t count = params.cellCount;
    const auto arena = atomic64::ArenaLayout::Make(count, participants);
    if (!arena) return E_OUTOFMEMORY;
    reused = execution.header.buffer && execution.nodes.length >= arena->nodes_bytes &&
        execution.bitmap.length >= arena->bitmap_bytes;
    const std::array<std::pair<uint64_t, BufferStorage *>, 5> required{{
        {arena->roots_bytes, &execution.roots}, {arena->nodes_bytes, &execution.nodes},
        {arena->bitmap_bytes, &execution.bitmap}, {uint64_t(count) * 8, &execution.baseline},
        {sizeof(SM50_ATOMIC64_CONTEXT), &execution.header}}};
    for (const auto &[length, buffer] : required) {
      const HRESULT hr = EnsureContextBuffer(length, *buffer);
      if (FAILED(hr)) return hr;
    }
    auto *roots = static_cast<uint32_t *>(execution.roots.mapped);
    std::iota(roots, roots + count, 0u);
    auto *nodes = static_cast<SM50_ATOMIC64_NODE *>(execution.nodes.mapped);
    for (uint32_t index = 0; index < arena->node_count; ++index)
      nodes[index] = {index < count ? 0u : SM50_ATOMIC64_NODE_RETIRED, 0, 0, 0};
    auto *bitmap = static_cast<uint32_t *>(execution.bitmap.mapped);
    std::memset(bitmap, 0, arena->bitmap_bytes);
    for (uint32_t index = count; index < arena->node_count; ++index)
      bitmap[index / 32] |= 1u << (index % 32);
    std::memset(execution.baseline.mapped, 0, uint64_t(count) * 8);
    SM50_ATOMIC64_CONTEXT context{};
    context.roots = execution.roots.address;
    context.nodes = execution.nodes.address;
    context.freeBitmap = execution.bitmap.address;
    context.rawRanges = raw_ranges;
    context.typedViews = typed_views;
    context.rawCount = ranges.size();
    context.typedCount = views.size();
    // Allocation capacity can be larger, but no stale node/bitmap suffix is
    // exposed to a smaller phase. Every published word is initialized above.
    context.nodeCapacity = arena->node_count;
    context.bitmapWordCount = arena->bitmap_words;
    context.firstFreeWord = context.hint = count / 32;
    std::memcpy(execution.header.mapped, &context, sizeof(context));
    execution.node_count = arena->node_count;
    params.roots = execution.roots.address;
    params.nodes = execution.nodes.address;
    params.baseline = execution.baseline.address;
    return S_OK;
  }

  template<typename T> HRESULT Upload(std::span<const T> values, uint64_t &address) {
    if (values.size() > SIZE_MAX / sizeof(T)) return E_OUTOFMEMORY;
    BufferStorage storage;
    const HRESULT hr = NewBuffer(values.size_bytes(), storage);
    if (FAILED(hr)) return hr;
    if (!values.empty()) std::memcpy(storage.mapped, values.data(), values.size_bytes());
    address = storage.address;
    return S_OK;
  }

  void CaptureKey(std::span<const std::pair<size_t, uint64_t>> texture_ids) {
    auto append = [&](std::initializer_list<uint64_t> words) { key.insert(key.end(), words); };
    auto pointer = [](const void *value) { return uint64_t(reinterpret_cast<uintptr_t>(value)); };
    // These vectors were sized before the mapping lock. Keep the owners and
    // backing allocations even on cold paths: a canonical resource may rename
    // its buffer independently of the public texture allocation.
    for (size_t index = 0; index < bindings.textures.size(); ++index) {
      const auto &binding = bindings.textures[index];
      if (!binding.texture) continue;
      auto &canonical = canonical_resources[index];
      canonical = device->LookupStandardSwizzleTexture(binding.texture.ptr());
      if (canonical && canonical->buffer) canonical_allocations[index] = canonical->buffer->current();
    }
    auto owns_reserved = [&](ReservedResource *reserved) {
      auto owns = [=](const auto &resource) { return resource && resource->reserved.get() == reserved; };
      return std::any_of(bindings.resources.begin(), bindings.resources.end(), owns) ||
          std::any_of(canonical_resources.begin(), canonical_resources.end(), owns);
    };
    auto owns_backing = [&](const void *backing) {
      auto owns = [=](const auto &resource) {
        return resource && (resource->buffer.ptr() == backing || resource->texture.ptr() == backing);
      };
      return std::any_of(bindings.resources.begin(), bindings.resources.end(), owns) ||
          std::any_of(canonical_resources.begin(), canonical_resources.end(), owns);
    };
    auto reserved_key = [&](ReservedResource *reserved) {
      const bool owned = !reserved || owns_reserved(reserved);
      cache_eligible &= owned;
      append({pointer(reserved), reserved && owned ? reserved->MappingGeneration() : 0});
    };
    auto allocation_key = [&](BufferAllocation *allocation) {
      append({pointer(allocation), allocation ? allocation->buffer().handle : 0,
          allocation ? allocation->gpuAddress() : 0, allocation ? allocation->byteLength() : 0,
          allocation ? allocation->canonicalHeap.handle : 0, allocation ? allocation->canonicalHeapOffset : 0,
          allocation ? pointer(allocation->mappedMemory(0)) : 0});
      if (allocation && allocation->flags().test(BufferAllocationFlag::CpuPlaced)) cache_eligible = false;
      reserved_key(allocation ? allocation->reservedResource : nullptr);
    };
    append({bindings.resources.size()});
    for (const auto &resource : bindings.resources) {
      append({resource ? resource->owned_id : 0});
      if (!resource) continue;
      // Their external section/host heap or private cast/volume storage is not
      // fully measurable through the bound native allocation. They still use
      // the normal cold preparation path; the cache must not pin it unbounded.
      D3D12_RESOURCE_DESC desc{};
      resource->GetDesc(&desc);
      if (resource->share || !resource->owned_castable_formats.empty() ||
          (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR) ||
          (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || desc.SampleDesc.Count > 1 ||
          (resource->texture && resource->texture->reservedVolumeLayoutWords))
        cache_eligible = false;
    }
    append({bindings.buffers.size()});
    for (const auto &binding : bindings.buffers) {
      auto *allocation = binding.allocation.ptr();
      if (binding.buffer && !owns_backing(binding.buffer.ptr())) cache_eligible = false;
      allocation_key(allocation);
      append({pointer(binding.buffer.ptr()), binding.nativeBuffer.handle, binding.nativeAddress, binding.nativeLength,
          binding.byteOffset, binding.byteLength, binding.view.impl_, binding.firstElement, uint64_t(binding.writable)});
      if (binding.buffer && binding.view.valid_bits && allocation) {
        const auto &view = binding.buffer->view_(binding.view, allocation);
        append({view.texture.handle, view.gpu_resource_id});
      } else append({0, 0});
    }
    append({bindings.textures.size()});
    for (size_t index = 0; index < bindings.textures.size(); ++index) {
      const auto &binding = bindings.textures[index];
      auto *texture = binding.texture.ptr();
      auto *allocation = binding.allocation.ptr();
      if (texture) {
        if (!owns_backing(texture) || texture->sampleCount() > 1) cache_eligible = false;
        // Depth and MSAA views can own private shadows/sample-ID storage. Keep
        // their existing cold preparation until those complete footprints have
        // a dedicated immutable snapshot, even when this particular view is SRV.
        switch (ORIGINAL_FORMAT(texture->pixelFormat())) {
        case WMTPixelFormatDepth16Unorm: case WMTPixelFormatDepth32Float:
        case WMTPixelFormatStencil8: case WMTPixelFormatDepth24Unorm_Stencil8:
        case WMTPixelFormatDepth32Float_Stencil8: cache_eligible = false; break;
        default: break;
        }
      }
      append({pointer(texture), pointer(allocation), binding.view.impl_, uint64_t(binding.writable),
          binding.first_w_slice, binding.w_size, binding.shader_resource_id,
          allocation ? allocation->texture().handle : 0,
          allocation ? allocation->canonicalHeap.handle : 0, allocation ? allocation->canonicalHeapOffset : 0});
      reserved_key(texture ? texture->reservedResource : nullptr);
      if (texture && allocation && UIntFormat(texture->viewDescriptor(binding.view).format)) {
        const auto &view = texture->view(binding.view, allocation);
        append({view.texture.handle, view.gpuResourceID, allocation->msaaCanonical.handle});
        if (view.castBlockSize || view.reservedVolumeRender || texture->reservedVolumeLayoutWords ||
            allocation->mappedMemory)
          cache_eligible = false;
      } else append({0, 0, 0});
      const auto &canonical = canonical_resources[index];
      append({canonical ? canonical->owned_id : 0, canonical ? pointer(canonical->swizzle_layout.get()) : 0});
      allocation_key(canonical_allocations[index].ptr());
      if (canonical && (!canonical->swizzle_layout || !canonical_allocations[index])) cache_eligible = false;
    }
    append({texture_ids.size()});
    for (const auto &[index, id] : texture_ids) append({index, id});
  }

  void CaptureNativeSources() {
    if (!cache_eligible) return;
    std::map<uint64_t, size_t> seen;
    uint64_t bytes = 0;
    auto retain = [&](WMT::Allocation allocation, uint64_t minimum = 0, bool view = false) {
      if (!allocation || !cache_eligible || seen.contains(allocation.handle)) return;
      // Known views only alias an already captured buffer/texture. Full native
      // allocations and heaps must have a measurable footprint (or sparse VA
      // extent), never merely the small binding span that happens to use them.
      const uint64_t size = view ? 0 : std::max(allocation.allocatedSize(), minimum);
      if ((!view && !size) || size > kAtomic64CacheBytes - bytes) {
        cache_eligible = false;
        return;
      }
      seen.emplace(allocation.handle, native_sources.size());
      native_sources.push_back({allocation, size});
      bytes += size;
    };
    auto buffer = [&](BufferAllocation *allocation) {
      if (!allocation) return;
      retain(allocation->buffer(), allocation->byteLength());
      retain(allocation->canonicalHeap);
    };
    for (const auto &binding : bindings.buffers) {
      buffer(binding.allocation.ptr());
      if (!binding.allocation) retain(binding.nativeBuffer, binding.nativeLength);
      if (binding.buffer && binding.view.valid_bits && binding.allocation)
        retain(binding.buffer->view_(binding.view, binding.allocation.ptr()).texture, 0, true);
    }
    for (const auto &binding : bindings.textures) {
      auto *texture = binding.texture.ptr();
      auto *allocation = binding.allocation.ptr();
      if (!texture || !allocation) continue;
      const uint64_t sparse_bytes = texture->reservedResource ?
          uint64_t(texture->reservedResource->totalTiles()) * kReservedTileBytes : 0;
      retain(allocation->texture(), sparse_bytes);
      retain(allocation->canonicalHeap);
      if (UIntFormat(texture->viewDescriptor(binding.view).format))
        retain(texture->view(binding.view, allocation).texture, 0, true);
    }
    for (const auto &allocation : canonical_allocations) buffer(allocation.ptr());
    for (const auto &heap : heaps) retain(heap);
  }

  uint64_t RetainedBytes() const {
    uint64_t bytes = sizeof(*this);
    auto add = [&](uint64_t count, uint64_t size = 1) {
      if (bytes > kAtomic64CacheBytes || size > kAtomic64CacheBytes ||
          count > (kAtomic64CacheBytes - bytes) / size)
        bytes = kAtomic64CacheBytes + 1;
      else bytes += count * size;
    };
    auto native = [&](WMT::Allocation allocation, uint64_t minimum = 0) {
      if (!allocation || bytes > kAtomic64CacheBytes) return;
      const uint64_t size = std::max(allocation.allocatedSize(), minimum);
      add(size ? size : kAtomic64CacheBytes + 1);
    };
    add(key.capacity(), sizeof(uint64_t));
    add(native_sources.capacity(), sizeof(NativeSource));
    add(bindings.buffers.capacity(), sizeof(Atomic64BufferBinding));
    add(bindings.textures.capacity(), sizeof(Atomic64TextureBinding));
    add(bindings.resources.capacity(), sizeof(Com<MTLD3D12Resource, false>));
    add(buffers.capacity(), sizeof(BufferStorage));
    add(resident_views.capacity(), sizeof(WMT::Reference<WMT::Texture>));
    add(heaps.capacity(), sizeof(WMT::Reference<WMT::Heap>));
    add(canonical_resources.capacity(), sizeof(Com<MTLD3D12Resource, false>));
    add(canonical_allocations.capacity(), sizeof(Rc<BufferAllocation>));
    add(textures.capacity(), sizeof(TextureTransfer));
    add(ranges.capacity(), sizeof(SM50_ATOMIC64_RAW_RANGE));
    add(views.capacity(), sizeof(SM50_ATOMIC64_TYPED_VIEW));
    add(import_cells.capacity() + export_cells.capacity(), sizeof(Atomic64ImportCell));
    add(cells.RunCount(), sizeof(atomic64::CanonicalCell) + 2 * sizeof(uint64_t) + 4 * sizeof(void *));
    for (const auto &[reserved, targets] : mappings) {
      add(1, sizeof(reserved) + sizeof(targets) + 4 * sizeof(void *));
      add(targets.capacity(), sizeof(ReservedTileTarget));
    }
    for (const auto &transfer : textures) add(transfer.regions.capacity(), sizeof(TextureTransfer::Region));
    for (const auto &buffer : buffers) native(buffer.buffer, buffer.length);
    for (const auto *buffer : {&execution.roots, &execution.nodes, &execution.bitmap,
                              &execution.baseline, &execution.header})
      native(buffer->buffer, buffer->length);
    // No idle Buffer/Texture/ReservedResource pointer is dereferenced here.
    // These costs and references were frozen with the topology; phase-only
    // metadata owners are released before the entry becomes a sole cache owner.
    for (const auto &source : native_sources) add(source.bytes);
    return bytes;
  }

  uint32_t Resolve(CanonicalOwner owner, uint64_t offset, uint64_t source, uint32_t count,
                   bool writable, uint32_t first_writable = 0) {
    const uint32_t cell = cells.Resolve(owner, offset);
    if (cell >= import_cells.size()) {
      import_cells.resize(uint64_t(cell) + 1);
      export_cells.resize(uint64_t(cell) + 1);
    }
    const uint32_t byte = (owner.base + offset) & 7;
    for (uint32_t i = 0; i < count; ++i) {
      if (!import_cells[cell].bytes[byte + i]) import_cells[cell].bytes[byte + i] = source + i;
      if (writable && i >= first_writable && !export_cells[cell].bytes[byte + i])
        export_cells[cell].bytes[byte + i] = source + i;
    }
    return cell;
  }

  const std::vector<ReservedTileTarget> &Snapshot(ReservedResource *reserved) {
    const auto found = mappings.find(reserved);
    if (found != mappings.end()) return found->second;
    std::vector<ReservedTileTarget> targets(reserved->totalTiles());
    for (uint32_t tile = 0; tile < targets.size(); ++tile) {
      targets[tile] = reserved->CurrentTarget(tile);
      if (targets[tile].heap) heaps.push_back(targets[tile].heap);
    }
    return mappings.emplace(reserved, std::move(targets)).first->second;
  }

  HRESULT AddBuffer(const Atomic64BufferBinding &binding) {
    auto *allocation = binding.allocation.ptr();
    const auto native = allocation ? allocation->buffer() : WMT::Buffer(binding.nativeBuffer);
    if (!native || !binding.byteLength) return S_OK;
    const uint64_t allocation_length = allocation ? allocation->byteLength() : binding.nativeLength;
    const uint64_t address = allocation ? allocation->gpuAddress() : binding.nativeAddress;
    if (!address || binding.byteOffset > allocation_length ||
        binding.byteLength > allocation_length - binding.byteOffset) return E_INVALIDARG;
    if (!allocation) {
      buffers.push_back({binding.nativeBuffer, address, nullptr, allocation_length});
      const HRESULT hr = device->RegisterResidency(binding.nativeBuffer);
      if (FAILED(hr)) { buffers.pop_back(); return hr; }
    }
    const uint64_t begin = binding.byteOffset & ~uint64_t(7);
    const uint64_t end = binding.byteOffset + binding.byteLength;
    const uint64_t count = (end - begin + 7) / 8;
    if (count > UINT32_MAX || count > device->GetMTLDevice().maxBufferLength() / 4) return E_OUTOFMEMORY;
    std::vector<uint32_t> map(count, SM50_ATOMIC64_UNMAPPED_CELL);
    CanonicalOwner owner = allocation ? BufferOwner(allocation) : CanonicalOwner{CanonicalKind::Buffer, native.handle};
    for (uint64_t offset = begin; offset < end; offset += 8) {
      CanonicalOwner current_owner = owner;
      uint64_t current_offset = offset;
      if (auto *reserved = allocation ? allocation->reservedResource : nullptr) {
        const auto target = Snapshot(reserved)[offset / kReservedTileBytes];
        if (!target.heap || target.heap_tile == kReservedNullTile) continue;
        current_owner = {CanonicalKind::HeapBytes, target.heap.handle, uint64_t(target.heap_tile) * kReservedTileBytes};
        current_offset = offset % kReservedTileBytes;
      }
      const uint32_t bytes = std::min<uint64_t>(8, end - offset);
      const uint32_t cell = Resolve(current_owner, current_offset, address + offset,
                                    bytes, binding.writable, offset < binding.byteOffset ? binding.byteOffset - offset : 0);
      map[(offset - begin) / 8] = cell;
    }
    uint64_t map_address = 0;
    HRESULT hr = Upload<uint32_t>(map, map_address);
    if (FAILED(hr)) return hr;
    ranges.push_back({address + begin, count * 8, map_address});
    if (binding.buffer && binding.view.valid_bits) {
      const auto descriptor = binding.buffer->viewDescriptor(binding.view);
      const uint32_t unit_bytes = MTLGetTexelSize(descriptor.format);
      if (!unit_bytes || binding.byteLength / unit_bytes > UINT32_MAX) return E_INVALIDARG;
      const auto &native = binding.buffer->view_(binding.view, allocation);
      SM50_ATOMIC64_TYPED_SUBRESOURCE sub{map_address, binding.byteOffset - begin,
          static_cast<uint32_t>(binding.byteLength / unit_bytes), 1, 1, 0};
      uint64_t sub_address = 0;
      hr = Upload<SM50_ATOMIC64_TYPED_SUBRESOURCE>(std::span(&sub, 1), sub_address);
      if (FAILED(hr)) return hr;
      views.push_back({native.gpu_resource_id, sub_address, 1, 1, 1, 1, unit_bytes,
                      static_cast<uint32_t>(descriptor.format), 0, 0, 0, binding.firstElement, 0});
    }
    return S_OK;
  }

  HRESULT AddTexture(const Atomic64TextureBinding &binding, MTLD3D12Resource *canonical,
                     BufferAllocation *canonical_allocation) {
    auto *texture = binding.texture.ptr();
    auto *allocation = binding.allocation.ptr();
    if (!texture || !allocation) return S_OK;
    const auto descriptor = texture->viewDescriptor(binding.view);
    const auto uint_format = UIntFormat(descriptor.format);
    // Compressed sampled views have no scalar raw-bit texture operation. Their
    // writable cast shadows are separate, uncompressed views collected below.
    if (!uint_format) return S_OK;
    const uint32_t unit_bytes = MTLGetTexelSize(descriptor.format);
    if (!unit_bytes || unit_bytes > 16) return E_INVALIDARG;
    auto &native_view = texture->view(binding.view, allocation);
    const uint32_t mip_count = binding.view.mip_end - binding.view.mip_start;
    const uint32_t array_count = binding.view.array_end - binding.view.array_start;
    const bool volume_atlas = texture->reservedVolumeLayoutWords != nullptr;
    const bool shadow = native_view.castBlockSize || texture->sampleCount() > 1;
    const bool volume = descriptor.type == WMTTextureType3D;
    const uint32_t samples = texture->sampleCount();
    if (!mip_count || !array_count) return S_OK;
    CanonicalOwner owner{allocation->canonicalHeap ? CanonicalKind::HeapTextureUnits : CanonicalKind::TextureUnits,
        allocation->canonicalHeap ? allocation->canonicalHeap.handle : allocation->texture().handle,
        allocation->canonicalHeap ? allocation->canonicalHeapOffset : 0,
        {texture->width(), texture->height(), texture->depth(), texture->arrayLength(), texture->miplevelCount(),
         texture->sampleCount(), static_cast<uint32_t>(ORIGINAL_FORMAT(texture->pixelFormat())),
         static_cast<uint32_t>(texture->textureType())}};
    std::vector<SM50_ATOMIC64_TYPED_SUBRESOURCE> subresources;
    for (uint32_t array = 0; array < array_count; ++array) {
      for (uint32_t mip = 0; mip < mip_count; ++mip) {
        const uint32_t actual_mip = binding.view.mip_start + mip;
        const uint32_t actual_array = binding.view.array_start + array;
        const uint32_t width = std::max(1u, (volume_atlas ? texture->reservedVolumeLayoutWords[0] : texture->width()) >> actual_mip);
        const uint32_t height = std::max(1u, (volume_atlas ? texture->reservedVolumeLayoutWords[1] : texture->height()) >> actual_mip);
        const uint32_t depth = volume ? std::max(1u, (volume_atlas ? texture->reservedVolumeLayoutWords[2] : texture->depth()) >> actual_mip) : 1;
        const uint32_t logical_width = native_view.castBlockSize && !native_view.castCompressed ? (width + 3) / 4 : width;
        const uint32_t logical_height = native_view.castBlockSize && !native_view.castCompressed ? (height + 3) / 4 : height;
        const uint64_t row_bytes = uint64_t(logical_width) * unit_bytes * samples;
        const uint64_t row_pitch = (row_bytes + 7) & ~uint64_t(7);
        if (row_pitch > UINT32_MAX || depth > UINT64_MAX / (row_pitch * logical_height)) return E_OUTOFMEMORY;
        const uint64_t map_bytes = row_pitch * logical_height * depth;
        if (map_bytes / 8 > UINT32_MAX ||
            map_bytes > device->GetMTLDevice().maxBufferLength()) return E_OUTOFMEMORY;
        std::vector<uint32_t> map(map_bytes / 8, SM50_ATOMIC64_UNMAPPED_CELL);
        // Each sample owns a native single-sample array page. The public typed
        // map interleaves sample words, preserving sample IDs independently.
        for (uint32_t sample = 0; sample < samples; ++sample) {
          TextureTransfer transfer{};
          transfer.writable = binding.writable;
          transfer.scattered = volume_atlas;
          transfer.source = samples > 1 && !descriptor.msaaShadow ? allocation->msaaCanonical :
              shadow || volume_atlas ? WMT::Texture(native_view.texture) : allocation->texture();
          if (!transfer.source) return E_FAIL;
          transfer.level = volume_atlas ? 0 : shadow ? mip : actual_mip;
          transfer.slice = samples > 1 ? (descriptor.msaaShadow ? array : actual_array) * samples + sample : shadow ? array : actual_array;
          transfer.type = volume_atlas || samples > 1 ? WMTTextureType2DArray : volume ? WMTTextureType3D : WMTTextureType2D;
          transfer.params.width = logical_width;
          transfer.params.height = logical_height;
          transfer.params.depth = depth;
          transfer.params.rowPitch = row_pitch;
          transfer.params.unitBytes = unit_bytes;
          transfer.params.texelStride = unit_bytes * samples;
          transfer.params.byteBase = sample * unit_bytes;
          transfer.params.channelBits = uint_format->channel_bits;
          transfer.params.channels = uint_format->channels;
          const uint64_t staging_row_pitch = (uint64_t(logical_width) * unit_bytes + 255) & ~uint64_t(255);
          if (staging_row_pitch > UINT32_MAX) return E_OUTOFMEMORY;
          transfer.staging_row_pitch = staging_row_pitch;
          const uint64_t slice_pitch = uint64_t(transfer.staging_row_pitch) * logical_height;
          if (slice_pitch > UINT32_MAX) return E_OUTOFMEMORY;
          transfer.staging_slice_pitch = slice_pitch;
          HRESULT hr = NewBuffer(slice_pitch * depth, transfer.staging);
          if (FAILED(hr)) return hr;
          if (transfer.writable) {
            uint64_t resource_id = 0;
            transfer.destination = transfer.source.newTextureView(uint_format->format, transfer.type,
                transfer.level, 1, volume_atlas ? 0 : transfer.slice,
                volume_atlas ? transfer.source.arrayLength() : 1, {}, resource_id);
            if (!transfer.destination) return E_OUTOFMEMORY;
            resident_views.push_back(transfer.destination);
            hr = device->RegisterResidency(transfer.destination);
            if (FAILED(hr)) { resident_views.pop_back(); return hr; }
          }
          uint64_t mip_base = 0;
          for (uint32_t previous = 0; previous < actual_mip; ++previous) {
            const uint32_t previous_width = std::max(1u, texture->width() >> previous);
            const uint32_t previous_height = std::max(1u, texture->height() >> previous);
            const uint64_t previous_row = (uint64_t(native_view.castBlockSize && !native_view.castCompressed ? (previous_width + 3) / 4 : previous_width) * unit_bytes * samples + 7) & ~uint64_t(7);
            mip_base += previous_row * (native_view.castBlockSize && !native_view.castCompressed ? (previous_height + 3) / 4 : previous_height) *
                (volume ? std::max(1u, texture->depth() >> previous) : 1);
          }
          uint64_t slice_bytes = mip_base + map_bytes;
          for (uint32_t later = actual_mip + 1; later < texture->miplevelCount(); ++later) {
            const uint32_t later_width = std::max(1u, texture->width() >> later);
            const uint32_t later_height = std::max(1u, texture->height() >> later);
            slice_bytes += ((uint64_t(native_view.castBlockSize && !native_view.castCompressed ? (later_width + 3) / 4 : later_width) * unit_bytes * samples + 7) & ~uint64_t(7)) *
                (native_view.castBlockSize && !native_view.castCompressed ? (later_height + 3) / 4 : later_height) * (volume ? std::max(1u, texture->depth() >> later) : 1);
          }
          std::vector<std::array<uint32_t, 4>> native_positions;
          if (volume_atlas) native_positions.resize(uint64_t(logical_width) * logical_height * depth);
          for (uint32_t z = 0; z < depth; ++z) {
            for (uint32_t y = 0; y < logical_height; ++y) {
              for (uint32_t x = 0; x < logical_width; ++x) {
                const uint64_t logical_byte = uint64_t(z) * row_pitch * logical_height + uint64_t(y) * row_pitch +
                                              (uint64_t(x) * samples + sample) * unit_bytes;
                CanonicalOwner current_owner = owner;
                uint64_t canonical_byte = actual_array * slice_bytes + mip_base + logical_byte;
                if (canonical && canonical->swizzle_layout && canonical_allocation) {
                  const auto offset = canonical->swizzle_layout->offset(actual_mip, actual_array, x, y, z);
                  if (!offset) continue;
                  current_owner = BufferOwner(canonical_allocation);
                  canonical_byte = *offset;
                }
                if (auto *reserved = texture->reservedResource) {
                  const auto shape = reserved->tileShape();
                  const uint32_t px = native_view.castBlockSize ? x * reserved->blockWidth() : x;
                  const uint32_t py = native_view.castBlockSize ? y * reserved->blockHeight() : y;
                  D3D12_TILED_RESOURCE_COORDINATE coord{px / shape.WidthInTexels, py / shape.HeightInTexels,
                      z / shape.DepthInTexels, actual_array * reserved->mipLevels() + actual_mip};
                  const auto tile = reserved->TileIndex(coord);
                  if (!tile) continue;
                  const auto target = Snapshot(reserved)[*tile];
                  if (!target.heap || target.heap_tile == kReservedNullTile) continue;
                  current_owner = {CanonicalKind::HeapBytes, target.heap.handle, uint64_t(target.heap_tile) * kReservedTileBytes};
                  canonical_byte = uint64_t(reserved->bytesPerUnit()) *
                      ((px % shape.WidthInTexels) / reserved->blockWidth() +
                       uint64_t(shape.WidthInTexels / reserved->blockWidth()) *
                       ((py % shape.HeightInTexels) / reserved->blockHeight() +
                        uint64_t(shape.HeightInTexels / reserved->blockHeight()) * (z % shape.DepthInTexels)));
                  if (volume_atlas) {
                    const auto position = reserved->VolumeAddress(actual_mip, px, py, z);
                    if (!position) return E_INVALIDARG;
                    const uint32_t block_w = native_view.castBlockSize && !native_view.castCompressed ? reserved->blockWidth() : 1;
                    const uint32_t block_h = native_view.castBlockSize && !native_view.castCompressed ? reserved->blockHeight() : 1;
                    native_positions[(uint64_t(z) * logical_height + y) * logical_width + x] =
                        {position->x / block_w, position->y / block_h, position->slice, 0};
                    const uint64_t staging_offset = uint64_t(z) * slice_pitch +
                        uint64_t(y) * transfer.staging_row_pitch + uint64_t(x) * unit_bytes;
                    if (!transfer.regions.empty() && transfer.regions.back().slice == position->slice &&
                        transfer.regions.back().origin.y == position->y / block_h &&
                        transfer.regions.back().origin.x + transfer.regions.back().size.width == position->x / block_w &&
                        transfer.regions.back().offset + transfer.regions.back().size.width * unit_bytes == staging_offset)
                      ++transfer.regions.back().size.width;
                    else transfer.regions.push_back({{position->x / block_w, position->y / block_h, 0},
                        {1, 1, 1}, position->slice, staging_offset});
                  }
                }
                const uint64_t source = transfer.staging.address + uint64_t(z) * slice_pitch +
                                        uint64_t(y) * transfer.staging_row_pitch + uint64_t(x) * unit_bytes;
                for (uint32_t byte = 0; byte < unit_bytes;) {
                  const uint32_t bytes = std::min<uint32_t>(unit_bytes - byte, 8 - ((current_owner.base + canonical_byte + byte) & 7));
                  const uint32_t cell = Resolve(current_owner, canonical_byte + byte, source + byte, bytes, false);
                  map[(logical_byte + byte) / 8] = cell;
                  byte += bytes;
                }
              }
            }
          }
          if (!native_positions.empty()) {
            hr = Upload<std::array<uint32_t, 4>>(native_positions, transfer.params.nativePositions);
            if (FAILED(hr)) return hr;
          }
          textures.push_back(std::move(transfer));
        }
        uint64_t map_address = 0;
        HRESULT hr = Upload<uint32_t>(map, map_address);
        if (FAILED(hr)) return hr;
        for (size_t i = textures.size() - samples; i < textures.size(); ++i)
          textures[i].params.cellMap = map_address;
        subresources.push_back({map_address, 0, logical_width, logical_height, depth, static_cast<uint32_t>(row_pitch)});
      }
    }
    uint64_t sub_address = 0;
    HRESULT hr = Upload<SM50_ATOMIC64_TYPED_SUBRESOURCE>(subresources, sub_address);
    if (FAILED(hr)) return hr;
    views.push_back({binding.shader_resource_id ? binding.shader_resource_id : native_view.gpuResourceID,
        sub_address, static_cast<uint32_t>(subresources.size()), mip_count,
        array_count, samples, unit_bytes, static_cast<uint32_t>(descriptor.format), binding.first_w_slice,
        binding.w_size, 0, 0, 0});
    return S_OK;
  }

  void UseResources(WMT::MTL4ComputeCommandEncoder encoder) {
    wmtcmd_compute_useresource use{};
    use.type = WMTComputeCommandUseResource;
    use.usage = WMTResourceUsageRead | WMTResourceUsageWrite;
    for (const auto &buffer : buffers) {
      use.resource = buffer.buffer;
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&use));
    }
    for (const auto *buffer : {&execution.roots, &execution.nodes, &execution.bitmap,
                              &execution.baseline, &execution.header}) {
      use.resource = buffer->buffer;
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&use));
    }
    for (const auto &binding : bindings.buffers) {
      if (!binding.allocation) continue;
      use.resource = binding.allocation->buffer();
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&use));
    }
  }

  void Dispatch(WMT::MTL4CommandBuffer command_buffer, WMT::Fence fence, WMT::ComputePipelineState pipeline,
                const Atomic64TransferParams &parameters, WMTSize size, WMT::Texture destination = {}) {
    auto encoder = command_buffer.computeCommandEncoder(false);
    encoder.waitForFence(fence);
    UseResources(encoder);
    wmtcmd_compute_setpso pso{};
    pso.type = WMTComputeCommandSetPSO;
    pso.pso = pipeline;
    pso.threadgroup_size = destination ? WMTSize{8, 8, 1} : WMTSize{32, 1, 1};
    wmtcmd_compute_setbytes bytes{};
    bytes.type = WMTComputeCommandSetBytes;
    bytes.index = 0;
    bytes.length = sizeof(parameters);
    bytes.bytes.set(const_cast<Atomic64TransferParams *>(&parameters));
    pso.next.set(&bytes);
    wmtcmd_compute_settexture texture{};
    texture.type = WMTComputeCommandSetTexture;
    texture.texture = destination;
    texture.index = 0;
    wmtcmd_compute_dispatch dispatch{};
    dispatch.type = WMTComputeCommandDispatchThreads;
    dispatch.size = size;
    if (destination) {
      bytes.next.set(&texture);
      texture.next.set(&dispatch);
    } else bytes.next.set(&dispatch);
    encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pso));
    encoder.updateFence(fence);
    encoder.endEncoding();
  }
};

// Only the queue owns this bounded cache. A live Atomic64Phase adds one storage
// reference regardless of how many inflight owners retain that phase; therefore
// sole cache ownership is proof that all of those owners have released it.
class Atomic64PhaseCache {
public:
  std::shared_ptr<Atomic64Phase::Storage> Take(MTLD3D12Device *device, const std::vector<uint64_t> &key) {
    for (auto &entry : entries_) {
      if (entry && entry.use_count() == 1 && entry->retired && entry->device == device && entry->key == key) {
        entry->retired = false;
        return std::move(entry);
      }
    }
    return {};
  }

  size_t Size() const {
    return std::count_if(entries_.begin(), entries_.end(), [](const auto &entry) { return bool(entry); });
  }
  uint64_t Bytes() const {
    uint64_t bytes = 0;
    for (const auto &entry : entries_) if (entry) bytes += entry->cache_bytes;
    return bytes;
  }
  void Admit(const std::shared_ptr<Atomic64Phase::Storage> &storage) {
    if (!storage->cache_eligible) return;
    storage->cache_bytes = storage->RetainedBytes();
    if (storage->cache_bytes > kAtomic64CacheBytes) return;
    while (Size() == entries_.size() || Bytes() > kAtomic64CacheBytes - storage->cache_bytes) {
      auto oldest = entries_.end();
      for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (!*it || it->use_count() != 1) continue;
        if (oldest == entries_.end() || (*it)->last_use < (*oldest)->last_use) oldest = it;
      }
      if (oldest == entries_.end()) return; // Busy entries never add a GPU wait.
      // Called outside MappingQueueMutex: releasing a last D3D12 owner can
      // unregister reserved/standard-swizzle metadata and acquire that lock.
      oldest->reset();
    }
    for (auto &entry : entries_) if (!entry) {
      storage->last_use = ++clock_;
      entry = storage;
      break;
    }
  }

private:
  std::array<std::shared_ptr<Atomic64Phase::Storage>, kAtomic64CacheEntries> entries_;
  uint64_t clock_ = 0;
};

Atomic64Phase::Atomic64Phase(std::shared_ptr<Storage> storage) : storage_(std::move(storage)) {}
Atomic64Phase::~Atomic64Phase() {
  // This is the last Phase owner, after native retirement or an unsubmitted
  // failure. Release owners on both paths before storage_.use_count() can reach
  // the cache-only value; only successful MarkRetired leases can be borrowed.
  if (constants_) storage_->device->UnregisterResidency(constants_);
  storage_->ReleaseBindingOwners();
}
uint64_t Atomic64Phase::ContextAddress() const { return storage_->execution.header.address; }
HRESULT Atomic64Phase::CreatePrivateConstants(const ShaderImmutableConstants &source) {
  if (constants_ || !source.size || !source.alignment ||
      (source.alignment & (source.alignment - 1)) || source.size > UINT64_MAX - (source.alignment - 1))
    return E_INVALIDARG;
  Storage::BufferStorage copy;
  HRESULT hr = storage_->AllocateBuffer(source.size + source.alignment - 1, copy);
  if (FAILED(hr)) return hr;
  const uint64_t offset = (-copy.address) & (source.alignment - 1);
  hr = source.CopyForAtomic64(ContextAddress(), copy.address + offset, static_cast<uint8_t *>(copy.mapped) + offset);
  if (FAILED(hr)) return hr;
  hr = storage_->device->RegisterResidency(copy.buffer);
  if (FAILED(hr)) return hr;
  constants_ = std::move(copy.buffer);
  constants_offset_ = offset;
  return S_OK;
}
void Atomic64Phase::MarkRetired() {
  storage_->retired = true;
  owned_graphics::Trace("atomic64_retired", ",\"topology_id\":", storage_->topology_id);
}

HRESULT PrepareAtomic64Phase(MTLD3D12Device *device, uint64_t participant_bound,
    const Atomic64PhaseBindings &bindings, std::shared_ptr<Atomic64Phase> &phase,
    std::shared_ptr<Atomic64PhaseCache> &cache) {
  phase.reset();
  if (!device || !atomic64::ArenaLayout::Make(0, participant_bound)) return E_OUTOFMEMORY;
  try {
    auto fresh = std::make_shared<Atomic64Phase::Storage>(device);
    std::map<std::tuple<Buffer *, BufferAllocation *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint32_t>, size_t> buffer_indices;
    for (const auto &binding : bindings.buffers) {
      const auto [entry, inserted] = buffer_indices.emplace(
          std::tuple{binding.buffer.ptr(), binding.allocation.ptr(), binding.nativeBuffer.handle,
              binding.nativeAddress, binding.nativeLength, binding.byteOffset, binding.byteLength,
              binding.view.impl_, binding.firstElement}, fresh->bindings.buffers.size());
      if (inserted) fresh->bindings.buffers.push_back(binding);
      else fresh->bindings.buffers[entry->second].writable |= binding.writable;
    }
    std::map<std::tuple<Texture *, TextureAllocation *, uint64_t, uint16_t, uint16_t>, size_t> texture_indices;
    std::vector<std::pair<size_t,uint64_t>> texture_ids;
    for (const auto &binding : bindings.textures) {
      const auto [entry, inserted] = texture_indices.emplace(
          std::tuple{binding.texture.ptr(), binding.allocation.ptr(), binding.view.impl_,
              binding.first_w_slice, binding.w_size}, fresh->bindings.textures.size());
      if (inserted) fresh->bindings.textures.push_back(binding);
      else fresh->bindings.textures[entry->second].writable |= binding.writable;
      if (!inserted && binding.shader_resource_id &&
          binding.shader_resource_id != fresh->bindings.textures[entry->second].shader_resource_id)
        texture_ids.emplace_back(entry->second,binding.shader_resource_id);
    }
    std::sort(texture_ids.begin(), texture_ids.end());
    texture_ids.erase(std::unique(texture_ids.begin(), texture_ids.end()), texture_ids.end());
    // Rc allocations alone do not keep reserved/counter/view metadata alive.
    fresh->bindings.resources = bindings.resources;
    auto &owners = fresh->bindings.resources;
    std::sort(owners.begin(), owners.end(), [](const auto &a, const auto &b) {
      return (a ? a->owned_id : 0) < (b ? b->owned_id : 0);
    });
    owners.erase(std::unique(owners.begin(), owners.end(), [](const auto &a, const auto &b) {
      return a.ptr() == b.ptr();
    }), owners.end());
    fresh->canonical_resources.resize(fresh->bindings.textures.size());
    fresh->canonical_allocations.resize(fresh->bindings.textures.size());
    std::shared_ptr<Atomic64Phase::Storage> storage;
    bool topology_hit = false;
    {
      // Key capture, lookup and cold construction observe one mapping snapshot.
      // The existing exclusive phase permit keeps it fixed until export retires.
      std::lock_guard mappings(ReservedResource::MappingQueueMutex);
      fresh->CaptureKey(texture_ids);
      if (cache && fresh->cache_eligible) storage = cache->Take(device, fresh->key);
      topology_hit = bool(storage);
      if (!storage) {
        storage = fresh;
        storage->topology_id = owned_graphics::NewObjectId();
        for (const auto &binding : storage->bindings.buffers) {
          const HRESULT hr = storage->AddBuffer(binding);
          if (FAILED(hr)) return hr;
        }
        for (size_t index = 0; index < storage->bindings.textures.size(); ++index) {
          const size_t before = storage->views.size();
          const HRESULT hr = storage->AddTexture(storage->bindings.textures[index],
              storage->canonical_resources[index].ptr(), storage->canonical_allocations[index].ptr());
          if (FAILED(hr)) return hr;
          // Descriptor-pool aliases share the same map but keep every public ID.
          if (storage->views.size() != before) {
            const auto prototype = storage->views.back();
            for (const auto &[texture_index, id] : texture_ids) if (texture_index == index) {
              auto alias = prototype;
              alias.resourceID = id;
              storage->views.push_back(alias);
            }
          }
        }
        atomic64::IndexRawRanges(storage->ranges);
        // Keep the original first match when several windows share a Metal ResourceID.
        auto &views = storage->views;
        std::stable_sort(views.begin(), views.end(), [](const auto &a, const auto &b) {
          return a.resourceID < b.resourceID;
        });
        views.erase(std::unique(views.begin(), views.end(), [](const auto &a, const auto &b) {
          return a.resourceID == b.resourceID;
        }), views.end());
        if (storage->ranges.size() > UINT32_MAX || storage->views.size() > UINT32_MAX) return E_OUTOFMEMORY;
        storage->params.cellCount = storage->cells.Count();
        HRESULT hr = storage->Upload<SM50_ATOMIC64_RAW_RANGE>(storage->ranges, storage->raw_ranges);
        if (FAILED(hr)) return hr;
        hr = storage->Upload<SM50_ATOMIC64_TYPED_VIEW>(storage->views, storage->typed_views);
        if (FAILED(hr)) return hr;
        hr = storage->Upload<Atomic64ImportCell>(storage->import_cells, storage->params.sources);
        if (FAILED(hr)) return hr;
        hr = storage->Upload<Atomic64ImportCell>(storage->export_cells, storage->export_address);
        if (FAILED(hr)) return hr;
        // Only the GPU tables and transfer recipes survive preparation. The
        // canonical builder and per-byte CPU arrays are not needed on a hit.
        storage->cells = {};
        std::vector<Atomic64ImportCell>().swap(storage->import_cells);
        std::vector<Atomic64ImportCell>().swap(storage->export_cells);
        storage->mappings.clear();
        auto &heaps = storage->heaps;
        std::sort(heaps.begin(), heaps.end(), [](const auto &a, const auto &b) { return a.handle < b.handle; });
        heaps.erase(std::unique(heaps.begin(), heaps.end(), [](const auto &a, const auto &b) {
          return a.handle == b.handle;
        }), heaps.end());
      }
    }
    // Neither fresh-key owners nor evicted entries are destroyed under the
    // mapping lock. Failed growth also leaves no partially updated cache entry.
    if (topology_hit) {
      storage->bindings = std::move(fresh->bindings);
      storage->canonical_resources = std::move(fresh->canonical_resources);
      storage->canonical_allocations = std::move(fresh->canonical_allocations);
    } else storage->CaptureNativeSources();
    fresh.reset();
    bool context_reused = false;
    HRESULT hr = storage->ResetContext(participant_bound, context_reused);
    if (FAILED(hr)) return hr;
    // No pool pointer is exposed until every allocation and kernel is ready.
    if (!device->atomic64_phase_pipelines) {
      auto pipelines = std::make_shared<Atomic64PhasePipelines>();
      auto library = WMT4::Library(device->GetLib().getLibrary());
      WMT::Reference<WMT::Error> error;
      auto compiler = device->GetMetal4Compiler();
      pipelines->import = compiler.newComputePipelineState(library.newFunction("d3d12_atomic64_import"), error);
      pipelines->export_buffers = compiler.newComputePipelineState(library.newFunction("d3d12_atomic64_export_buffers"), error);
      pipelines->export_2d = compiler.newComputePipelineState(library.newFunction("d3d12_atomic64_export_2d"), error);
      pipelines->export_array = compiler.newComputePipelineState(library.newFunction("d3d12_atomic64_export_array"), error);
      pipelines->export_3d = compiler.newComputePipelineState(library.newFunction("d3d12_atomic64_export_3d"), error);
      if (!pipelines->import || !pipelines->export_buffers || !pipelines->export_2d || !pipelines->export_array ||
          !pipelines->export_3d) return E_FAIL;
      device->atomic64_phase_pipelines = std::move(pipelines);
    }
    storage->pipelines = device->atomic64_phase_pipelines;
    if (!cache) cache = std::make_shared<Atomic64PhaseCache>();
    phase = std::make_shared<Atomic64Phase>(storage);
    cache->Admit(storage);
    owned_graphics::Trace("atomic64_prepare", ",\"topology_id\":", storage->topology_id,
        ",\"topology_hit\":", topology_hit ? 1 : 0, ",\"context_reused\":", context_reused ? 1 : 0,
        ",\"participants\":", participant_bound, ",\"cells\":", storage->params.cellCount,
        ",\"node_capacity\":", storage->execution.node_count, ",\"cache_entries\":", cache->Size(),
        ",\"cache_bytes\":", cache->Bytes());
    return S_OK;
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  } catch (const std::length_error &) {
    return E_OUTOFMEMORY;
  }
}

HRESULT Atomic64Phase::PublishContext(WMT::MTL4CommandBuffer command_buffer, WMT::Fence fence) {
  const auto destination = storage_->device->GetAtomic64ContextBuffer();
  if (!destination) return E_OUTOFMEMORY;
  wmtcmd_blit_copy_from_buffer_to_buffer copy{};
  copy.type = WMTBlitCommandCopyFromBufferToBuffer;
  copy.src = storage_->execution.header.buffer;
  copy.dst = destination;
  copy.copy_length = sizeof(SM50_ATOMIC64_CONTEXT);
  auto encoder = command_buffer.blitCommandEncoder();
  encoder.waitForFence(fence);
  encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
  encoder.updateFence(fence);
  encoder.endEncoding();
  return S_OK;
}

HRESULT Atomic64Phase::ClearContext(WMT::MTL4CommandBuffer command_buffer, WMT::Fence fence) {
  wmtcmd_blit_fillbuffer fill{};
  fill.type = WMTBlitCommandFillBuffer;
  fill.buffer = storage_->device->GetAtomic64ContextBuffer();
  fill.length = sizeof(SM50_ATOMIC64_CONTEXT);
  if (!fill.buffer) return E_OUTOFMEMORY;
  auto encoder = command_buffer.blitCommandEncoder();
  encoder.waitForFence(fence);
  encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&fill));
  encoder.updateFence(fence);
  encoder.endEncoding();
  return S_OK;
}

HRESULT Atomic64Phase::EncodeImport(WMT::MTL4CommandBuffer command_buffer, WMT::Fence fence, bool publish_context) {
  const WMT::Buffer context = publish_context ? storage_->device->GetAtomic64ContextBuffer() : WMT::Buffer{};
  if (publish_context && !context) return E_OUTOFMEMORY;
  WMT::MTL4ComputeCommandEncoder encoder;
  uint64_t texture_copies = 0;
  for (const auto &transfer : storage_->textures) {
    if (transfer.scattered && transfer.regions.empty()) continue;
    if (!encoder) {
      // Sources are read-only and every transfer owns a distinct staging
      // buffer. Publish all copies together before the import kernel.
      encoder = command_buffer.blitCommandEncoder();
      encoder.waitForFence(fence);
    }
    wmtcmd_blit_copy_from_texture_to_buffer_withblitoption copy{};
    copy.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
    copy.src = transfer.source;
    copy.level = transfer.level;
    copy.slice = transfer.slice;
    copy.dst = transfer.staging.buffer;
    copy.bytes_per_row = transfer.staging_row_pitch;
    copy.bytes_per_image = transfer.type == WMTTextureType3D ? transfer.staging_slice_pitch : 0;
    if (transfer.scattered) {
      for (const auto &region : transfer.regions) {
        copy.origin = region.origin;
        copy.size = region.size;
        copy.slice = region.slice;
        copy.offset = region.offset;
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
        ++texture_copies;
      }
    } else {
      copy.size = {transfer.params.width, transfer.params.height, transfer.params.depth};
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
      ++texture_copies;
    }
  }
  if (encoder) {
    if (publish_context) {
      // The exclusive phase permit excludes every other GPU consumer. This
      // immutable header and texture staging copies are independent writes;
      // the app still waits for the import kernel's fence before using cells.
      wmtcmd_blit_copy_from_buffer_to_buffer copy{};
      copy.type = WMTBlitCommandCopyFromBufferToBuffer;
      copy.src = storage_->execution.header.buffer;
      copy.dst = context;
      copy.copy_length = sizeof(SM50_ATOMIC64_CONTEXT);
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
    }
    encoder.updateFence(fence);
    encoder.endEncoding();
  }
  if (storage_->params.cellCount)
    storage_->Dispatch(command_buffer, fence, storage_->pipelines->import, storage_->params,
                       {storage_->params.cellCount, 1, 1});
  if (publish_context) {
    // Buffer-only phases have no import blit to share; retain their original
    // import-compute then publication order and encoder count.
    if (!encoder) {
      const HRESULT hr = PublishContext(command_buffer, fence);
      if (FAILED(hr)) return hr;
    }
    owned_graphics::Trace("atomic64_import_publication", ",\"folded\":", bool(encoder),
        ",\"texture_copies\":", texture_copies, ",\"publication_extra_encoders\":", encoder ? 0 : 1,
        ",\"context_bytes\":", sizeof(SM50_ATOMIC64_CONTEXT));
  }
  return S_OK;
}

HRESULT Atomic64Phase::EncodeExport(WMT::MTL4CommandBuffer command_buffer, WMT::Fence fence) {
  if (storage_->params.cellCount) {
    auto params = storage_->params;
    params.sources = storage_->export_address;
    storage_->Dispatch(command_buffer, fence, storage_->pipelines->export_buffers, params,
                       {params.cellCount, 1, 1});
  }
  for (const auto &transfer : storage_->textures) {
    if (!transfer.writable) continue;
    auto params = transfer.params;
    params.roots = storage_->params.roots;
    params.nodes = storage_->params.nodes;
    params.baseline = storage_->params.baseline;
    auto pipeline = transfer.type == WMTTextureType3D ? WMT::ComputePipelineState(storage_->pipelines->export_3d) :
        transfer.type == WMTTextureType2DArray ? WMT::ComputePipelineState(storage_->pipelines->export_array) :
                                               WMT::ComputePipelineState(storage_->pipelines->export_2d);
    storage_->Dispatch(command_buffer, fence, pipeline, params, {params.width, params.height, params.depth}, transfer.destination);
  }
  return S_OK;
}
} // namespace d3dalloy
