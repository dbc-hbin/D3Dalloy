#include "d3d12_atomic64_phase.hpp"
#include "d3d12_atomic64_gpu.hpp"
#include "d3d12_atomic64_storage.hpp"
#include "d3d12_device.hpp"
#include "d3d12_reserved_resource.hpp"
#include "dxmt_format.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace dxmt {
namespace {
using atomic64::CanonicalKind;
using atomic64::CanonicalOwner;

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
    auto append_descriptor = [&](const ShaderVisibleDescriptorCPUStorage &descriptor) {
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
          bindings.textures.push_back({texture, texture->current(), descriptor.SRVTexture.view, false});
        break;
      case ShaderVisibleDescriptorType::UAVTexture:
        if (auto *texture = descriptor.UAVTexture.texture; texture && texture->current())
          bindings.textures.push_back({texture, texture->current(), descriptor.UAVTexture.view, true,
              descriptor.UAVTexture.first_w_slice, descriptor.UAVTexture.w_size});
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
            append_buffer(descriptor.UAVBuffer.counter, {descriptor.UAVBuffer.counter_offset, 4}, true);
          }
        }
        break;
      case ShaderVisibleDescriptorType::SamplerFeedback:
        append_buffer(descriptor.SamplerFeedback.buffer, descriptor.SamplerFeedback.slice, true); break;
      case ShaderVisibleDescriptorType::ConstantBuffer: {
        uint64_t offset = 0;
        auto *allocation = device->LookupBufferByVA(descriptor.ConstantBuffer.address, &offset);
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
        for (uint32_t index = 0; index < desc.NumDescriptors; ++index)
          append_descriptor(table.heap->GetDescriptor(index));
        continue;
      }
      if (table.restrict_to_shader) {
        table.rootsig->ForEachUsedDescriptorRange(table.rootsig->GetTableRanges(table.parameter), table.shader_bindings,
            [&](uint64_t offset, uint64_t count, D3D12_DESCRIPTOR_RANGE_TYPE) {
          if (offset >= desc.NumDescriptors - base) return;
          const uint64_t first = base + offset;
          count = std::min<uint64_t>(count, desc.NumDescriptors - first);
          for (uint64_t index = 0; index < count; ++index)
            append_descriptor(table.heap->GetDescriptor(first + index));
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
          append_descriptor(table.heap->GetDescriptor(first + index));
      }
    }
    for (const auto &root : roots) {
      if (!root.address || !root.byteLength) continue;
      uint64_t offset = 0;
      auto *allocation = device->LookupBufferByVA(root.address, &offset);
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
  Atomic64PhaseBindings bindings;
  std::vector<BufferStorage> buffers;
  std::vector<WMT::Reference<WMT::Texture>> resident_views;
  std::vector<WMT::Reference<WMT::Heap>> heaps;
  std::vector<Com<MTLD3D12Resource, false>> canonical_resources;
  std::vector<TextureTransfer> textures;
  atomic64::CanonicalCellMap cells;
  std::vector<Atomic64ImportCell> import_cells, export_cells;
  std::map<ReservedResource *, std::vector<ReservedTileTarget>> mappings;
  std::vector<SM50_ATOMIC64_RAW_RANGE> ranges;
  std::vector<SM50_ATOMIC64_TYPED_VIEW> views;
  BufferStorage context;
  Atomic64TransferParams params{};
  uint64_t export_address = 0;
  std::shared_ptr<Atomic64PhasePipelines> pipelines;

  explicit Storage(MTLD3D12Device *device) : device(device) {}
  ~Storage() {
    for (const auto &view : resident_views) device->UnregisterResidency(view);
    for (const auto &buffer : buffers) device->UnregisterResidency(buffer.buffer);
  }

  HRESULT NewBuffer(uint64_t length, BufferStorage &result) {
    if (!length) length = 16;
    if (length > device->GetMTLDevice().maxBufferLength() || length > SIZE_MAX) return E_OUTOFMEMORY;
    WMTBufferInfo info{};
    info.length = length;
    info.options = WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked | WMT_RESOURCE_CPU_ADDRESSABLE;
    result.buffer = device->GetMTLDevice().newBuffer(info);
    result.address = info.gpu_address;
    result.mapped = info.memory.get_accessible_or_null();
    if (!result.buffer || !result.address || !result.mapped) return E_OUTOFMEMORY;
    buffers.push_back(result);
    const HRESULT hr = device->RegisterResidency(result.buffer);
    if (FAILED(hr)) { buffers.pop_back(); return hr; }
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
      buffers.push_back({binding.nativeBuffer, address, nullptr});
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

  HRESULT AddTexture(const Atomic64TextureBinding &binding) {
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
    auto canonical = device->LookupStandardSwizzleTexture(texture);
    if (canonical) canonical_resources.push_back(canonical);
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
                if (canonical && canonical->swizzle_layout && canonical->buffer) {
                  const auto offset = canonical->swizzle_layout->offset(actual_mip, actual_array, x, y, z);
                  if (!offset) continue;
                  current_owner = BufferOwner(canonical->buffer->current());
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
    views.push_back({native_view.gpuResourceID, sub_address, static_cast<uint32_t>(subresources.size()), mip_count,
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

Atomic64Phase::Atomic64Phase(std::unique_ptr<Storage> storage) : storage_(std::move(storage)) {}
Atomic64Phase::~Atomic64Phase() = default;
uint64_t Atomic64Phase::ContextAddress() const { return storage_->context.address; }

HRESULT PrepareAtomic64Phase(MTLD3D12Device *device, uint64_t participant_bound,
    const Atomic64PhaseBindings &bindings, std::shared_ptr<Atomic64Phase> &phase) {
  phase.reset();
  if (!device || !atomic64::ArenaLayout::Make(0, participant_bound)) return E_OUTOFMEMORY;
  try {
    auto storage = std::make_unique<Atomic64Phase::Storage>(device);
    std::map<std::tuple<BufferAllocation *, uint64_t, uint64_t, uint64_t, uint64_t, uint32_t>, size_t> buffer_indices;
    for (const auto &binding : bindings.buffers) {
      const auto [entry, inserted] = buffer_indices.emplace(
          std::tuple{binding.allocation.ptr(), binding.nativeBuffer.handle, binding.byteOffset, binding.byteLength, binding.view.impl_, binding.firstElement},
          storage->bindings.buffers.size());
      if (inserted) storage->bindings.buffers.push_back(binding);
      else storage->bindings.buffers[entry->second].writable |= binding.writable;
    }
    std::map<std::tuple<TextureAllocation *, uint64_t, uint16_t, uint16_t>, size_t> texture_indices;
    for (const auto &binding : bindings.textures) {
      const auto [entry, inserted] = texture_indices.emplace(
          std::tuple{binding.allocation.ptr(), binding.view.impl_, binding.first_w_slice, binding.w_size},
          storage->bindings.textures.size());
      if (inserted) storage->bindings.textures.push_back(binding);
      else storage->bindings.textures[entry->second].writable |= binding.writable;
    }
    // Mapping updates use this same lock, and the queue's exclusive execution
    // permit remains held until export retirement. No tile may change identity.
    std::lock_guard mappings(ReservedResource::MappingQueueMutex);
    for (const auto &binding : storage->bindings.buffers) {
      const HRESULT hr = storage->AddBuffer(binding);
      if (FAILED(hr)) return hr;
    }
    for (const auto &binding : storage->bindings.textures) {
      const HRESULT hr = storage->AddTexture(binding);
      if (FAILED(hr)) return hr;
    }
    if (storage->ranges.size() > UINT32_MAX || storage->views.size() > UINT32_MAX) return E_OUTOFMEMORY;
    const uint32_t cells = storage->cells.Count();
    const auto arena = atomic64::ArenaLayout::Make(cells, participant_bound);
    if (!arena) return E_OUTOFMEMORY;
    Atomic64Phase::Storage::BufferStorage roots, nodes, bitmap, baseline;
    HRESULT hr = storage->NewBuffer(arena->roots_bytes, roots);
    if (FAILED(hr)) return hr;
    hr = storage->NewBuffer(arena->nodes_bytes, nodes);
    if (FAILED(hr)) return hr;
    hr = storage->NewBuffer(arena->bitmap_bytes, bitmap);
    if (FAILED(hr)) return hr;
    hr = storage->NewBuffer(uint64_t(cells) * 8, baseline);
    if (FAILED(hr)) return hr;
    auto *root_words = static_cast<uint32_t *>(roots.mapped);
    std::iota(root_words, root_words + cells, 0u);
    auto *node_words = static_cast<SM50_ATOMIC64_NODE *>(nodes.mapped);
    for (uint32_t index = 0; index < arena->node_count; ++index)
      node_words[index] = {index < cells ? 0u : SM50_ATOMIC64_NODE_RETIRED, 0, 0, 0};
    auto *bitmap_words = static_cast<uint32_t *>(bitmap.mapped);
    std::memset(bitmap_words, 0, arena->bitmap_bytes);
    for (uint32_t index = cells; index < arena->node_count; ++index)
      bitmap_words[index / 32] |= 1u << (index % 32);
    SM50_ATOMIC64_CONTEXT context{};
    context.roots = roots.address;
    context.nodes = nodes.address;
    context.freeBitmap = bitmap.address;
    context.rawCount = storage->ranges.size();
    context.typedCount = storage->views.size();
    context.nodeCapacity = arena->node_count;
    context.bitmapWordCount = arena->bitmap_words;
    context.cursor = context.hint = cells / 32;
    hr = storage->Upload<SM50_ATOMIC64_RAW_RANGE>(storage->ranges, context.rawRanges);
    if (FAILED(hr)) return hr;
    hr = storage->Upload<SM50_ATOMIC64_TYPED_VIEW>(storage->views, context.typedViews);
    if (FAILED(hr)) return hr;
    hr = storage->NewBuffer(sizeof(context), storage->context);
    if (FAILED(hr)) return hr;
    std::memcpy(storage->context.mapped, &context, sizeof(context));
    storage->params.roots = roots.address;
    storage->params.nodes = nodes.address;
    storage->params.baseline = baseline.address;
    storage->params.cellCount = cells;
    hr = storage->Upload<Atomic64ImportCell>(storage->import_cells, storage->params.sources);
    if (FAILED(hr)) return hr;
    hr = storage->Upload<Atomic64ImportCell>(storage->export_cells, storage->export_address);
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
    phase = std::make_shared<Atomic64Phase>(std::move(storage));
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
  copy.src = storage_->context.buffer;
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

HRESULT Atomic64Phase::EncodeImport(WMT::MTL4CommandBuffer command_buffer, WMT::Fence fence) {
  for (const auto &transfer : storage_->textures) {
    wmtcmd_blit_copy_from_texture_to_buffer_withblitoption copy{};
    copy.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
    copy.src = transfer.source;
    copy.level = transfer.level;
    copy.slice = transfer.slice;
    copy.dst = transfer.staging.buffer;
    copy.bytes_per_row = transfer.staging_row_pitch;
    copy.bytes_per_image = transfer.type == WMTTextureType3D ? transfer.staging_slice_pitch : 0;
    if (transfer.scattered) {
      if (transfer.regions.empty()) continue;
      auto encoder = command_buffer.blitCommandEncoder();
      encoder.waitForFence(fence);
      for (const auto &region : transfer.regions) {
        copy.origin = region.origin;
        copy.size = region.size;
        copy.slice = region.slice;
        copy.offset = region.offset;
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
      }
      encoder.updateFence(fence);
      encoder.endEncoding();
    } else {
      copy.size = {transfer.params.width, transfer.params.height, transfer.params.depth};
      auto encoder = command_buffer.blitCommandEncoder();
      encoder.waitForFence(fence);
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
      encoder.updateFence(fence);
      encoder.endEncoding();
    }
  }
  if (storage_->params.cellCount)
    storage_->Dispatch(command_buffer, fence, storage_->pipelines->import, storage_->params,
                       {storage_->params.cellCount, 1, 1});
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
} // namespace dxmt
