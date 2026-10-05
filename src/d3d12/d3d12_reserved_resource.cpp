/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "d3d12_reserved_resource.hpp"
#include "d3d12_device.hpp"
#include "log/log.hpp"
#include <algorithm>
#include <bit>

namespace dxmt {

namespace {

/* D3D12 standard 2D tile shape (in format units) for a unit size; zero when there is none. */
D3D12_TILE_SHAPE
StandardShape2D(uint32_t bytes_per_unit) {
  switch (bytes_per_unit) {
  case 1: return {256, 256, 1};
  case 2: return {256, 128, 1};
  case 4: return {128, 128, 1};
  case 8: return {128, 64, 1};
  case 16: return {64, 64, 1};
  default: return {0, 0, 0};
  }
}

uint32_t
CeilDiv(uint64_t value, uint64_t divisor) {
  return uint32_t((value + divisor - 1) / divisor);
}

D3D12_TILE_SHAPE
StandardShape3D(uint32_t bytes_per_unit) {
  switch (bytes_per_unit) {
  case 1: return {64, 32, 32};
  case 2: return {32, 32, 32};
  case 4: return {32, 32, 16};
  case 8: return {32, 16, 16};
  case 16: return {16, 16, 16};
  default: return {};
  }
}

} // namespace

bool
ReservedResource::MatchesStandardShape2D(
    WMTSparseTileShape tile_shape, uint32_t block_width, uint32_t block_height, uint32_t bytes_per_unit
) {
  const D3D12_TILE_SHAPE standard = StandardShape2D(bytes_per_unit);
  /* Metal tiles are in texels; D3D12 shapes are in format units (blocks for BC). */
  return standard.WidthInTexels && tile_shape.depth == 1 && tile_shape.width == standard.WidthInTexels * block_width &&
         tile_shape.height == standard.HeightInTexels * block_height;
}

HRESULT
ReservedResource::CreateBufferTiling(const D3D12_RESOURCE_DESC &desc, std::unique_ptr<ReservedResource> &out) {
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || !desc.Width ||
      desc.Width > uint64_t(UINT32_MAX) * kReservedTileBytes)
    return E_INVALIDARG;
  auto tiling = std::make_unique<ReservedResource>();
  tiling->is_buffer_ = true;
  tiling->total_tiles_ = CeilDiv(desc.Width, kReservedTileBytes);
  tiling->shape_ = {kReservedTileBytes, 1, 1};
  tiling->bytes_per_unit_ = 1;
  tiling->standard_mips_ = 1;
  tiling->subresources_.push_back({tiling->total_tiles_, 1, 1, 0, 0, 0, false});
  tiling->targets_.resize(tiling->total_tiles_);
  out = std::move(tiling);
  return S_OK;
}

HRESULT
ReservedResource::CreateTextureTiling(
    const D3D12_RESOURCE_DESC &desc, WMT::Texture native, WMTSparseTileShape tile_shape, uint32_t block_width,
    uint32_t block_height, uint32_t bytes_per_unit, std::unique_ptr<ReservedResource> &out
) {
  if (!MatchesStandardShape2D(tile_shape, block_width, block_height, bytes_per_unit))
    return E_INVALIDARG;

  auto tiling = std::make_unique<ReservedResource>();
  tiling->shape_ = {tile_shape.width, tile_shape.height, 1};
  tiling->mip_levels_ = desc.MipLevels;
  tiling->array_size_ = desc.DepthOrArraySize;
  tiling->bytes_per_unit_ = bytes_per_unit;
  tiling->block_width_ = block_width;
  tiling->block_height_ = block_height;

  const auto tail = native.sparseTail();
  const uint32_t first_in_tail = uint32_t(std::min<uint64_t>(tail.first_mipmap_in_tail, desc.MipLevels));
  tiling->standard_mips_ = first_in_tail;
  tiling->packed_mips_ = desc.MipLevels - first_in_tail;
  tiling->metal_tail_level_ = first_in_tail;
  if (tiling->packed_mips_) {
    if (!tail.tail_size_in_bytes || tail.tail_size_in_bytes % kReservedTileBytes)
      return E_INVALIDARG;
    tiling->packed_tiles_ = uint32_t(tail.tail_size_in_bytes / kReservedTileBytes);
  }

  uint64_t next = 0;
  const uint64_t width = desc.Width, height = desc.Height;
  for (uint32_t slice = 0; slice < desc.DepthOrArraySize; slice++) {
    for (uint32_t mip = 0; mip < desc.MipLevels; mip++) {
      ReservedSubresourceTiling sub{};
      sub.mip = mip;
      sub.slice = slice;
      const uint64_t mip_width = std::max<uint64_t>(width >> mip, 1), mip_height = std::max<uint64_t>(height >> mip, 1);
      if (mip >= first_in_tail) {
        sub.packed = true;
      } else {
        /* Tier 2: a mip spanning at least one whole tile in every dimension is never packed, which
           the Metal tail must agree with; a later level inside the tail must not be that large. */
        sub.tiles_x = CeilDiv(mip_width, tile_shape.width);
        sub.tiles_y = CeilDiv(mip_height, tile_shape.height);
        sub.tiles_z = 1;
        sub.first_tile = uint32_t(next);
        next += uint64_t(sub.tiles_x) * sub.tiles_y;
      }
      if (sub.packed && mip == first_in_tail && mip_width >= tile_shape.width && mip_height >= tile_shape.height) {
        ERR("CreateReservedResource: Metal packs mip ", mip, " that covers a whole tile");
        return E_INVALIDARG;
      }
      tiling->subresources_.push_back(sub);
    }
    if (tiling->packed_mips_) {
      if (!slice)
        tiling->packed_first_tile_ = uint32_t(next);
      for (uint32_t mip = first_in_tail; mip < desc.MipLevels; mip++)
        tiling->subresources_[slice * desc.MipLevels + mip].first_tile = uint32_t(next);
      next += tiling->packed_tiles_;
    }
    if (next > UINT32_MAX)
      return E_INVALIDARG;
  }
  tiling->total_tiles_ = uint32_t(next);
  tiling->targets_.resize(tiling->total_tiles_);
  out = std::move(tiling);
  return S_OK;
}

uint64_t
ReservedResource::VolumeTileCount(const D3D12_RESOURCE_DESC &desc, uint32_t block_width,
                                   uint32_t block_height, uint32_t bytes_per_unit) {
  const auto shape=StandardShape3D(bytes_per_unit);
  if (!shape.WidthInTexels || !block_width || !block_height ||
      desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE3D) return 0;
  uint64_t total=0, width=desc.Width, height=desc.Height, depth=desc.DepthOrArraySize;
  const uint32_t count=desc.MipLevels ? desc.MipLevels : std::bit_width(std::max({width,height,depth}));
  if (count>64) return 0;
  for (uint32_t mip=0;mip<count;++mip) {
    total+=uint64_t(CeilDiv(std::max(width>>mip,1ull),uint64_t(shape.WidthInTexels)*block_width))*
        CeilDiv(std::max(height>>mip,1ull),uint64_t(shape.HeightInTexels)*block_height)*
        CeilDiv(std::max(depth>>mip,1ull),shape.DepthInTexels);
  }
  return total<=UINT32_MAX ? total : 0;
}

HRESULT
ReservedResource::CreateVolumeTiling(
    const D3D12_RESOURCE_DESC &desc, WMTSparseTileShape native_page, uint32_t block_width,
    uint32_t block_height, uint32_t bytes_per_unit, std::unique_ptr<ReservedResource> &out
) {
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D || !desc.Width || !desc.Height ||
      !desc.DepthOrArraySize || !desc.MipLevels ||
      !MatchesStandardShape2D(native_page, block_width, block_height, bytes_per_unit))
    return E_INVALIDARG;
  auto tiling = std::make_unique<ReservedResource>();
  tiling->volume_ = true;
  tiling->shape_ = StandardShape3D(bytes_per_unit);
  if (!tiling->shape_.WidthInTexels)
    return E_INVALIDARG;
  tiling->shape_.WidthInTexels *= block_width;
  tiling->shape_.HeightInTexels *= block_height;
  tiling->mip_levels_ = tiling->standard_mips_ = desc.MipLevels;
  tiling->bytes_per_unit_ = bytes_per_unit;
  tiling->block_width_ = block_width;
  tiling->block_height_ = block_height;
  uint64_t next = 0;
  for (uint32_t mip = 0; mip < desc.MipLevels; mip++) {
    ReservedSubresourceTiling sub{};
    sub.mip = mip;
    sub.tiles_x = CeilDiv(std::max<uint64_t>(desc.Width >> mip, 1), tiling->shape_.WidthInTexels);
    sub.tiles_y = CeilDiv(std::max<uint64_t>(desc.Height >> mip, 1), tiling->shape_.HeightInTexels);
    sub.tiles_z = CeilDiv(std::max<uint64_t>(desc.DepthOrArraySize >> mip, 1), tiling->shape_.DepthInTexels);
    sub.first_tile = uint32_t(next);
    next += uint64_t(sub.tiles_x) * sub.tiles_y * sub.tiles_z;
    if (next > UINT32_MAX)
      return E_INVALIDARG;
    tiling->subresources_.push_back(sub);
  }
  tiling->total_tiles_ = uint32_t(next);
  const uint32_t columns = std::min(uint32_t(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) / native_page.width,
                                    tiling->total_tiles_);
  const uint32_t rows = std::min(uint32_t(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) / native_page.height,
                                CeilDiv(tiling->total_tiles_, columns));
  if (CeilDiv(tiling->total_tiles_, uint64_t(columns) * rows) > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION)
    return E_INVALIDARG;
  const auto shape = tiling->shape_;
  tiling->volume_layout_ = {uint32_t(desc.Width), desc.Height, desc.DepthOrArraySize, desc.MipLevels,
      shape.WidthInTexels, shape.HeightInTexels, shape.DepthInTexels, block_width, block_height,
      native_page.width, native_page.height, columns, rows, tiling->total_tiles_};
  tiling->targets_.resize(tiling->total_tiles_);
  out = std::move(tiling);
  return S_OK;
}

std::optional<ReservedVolumeAddress>
ReservedResource::VolumeAddress(uint32_t mip, uint32_t x, uint32_t y, uint32_t z) const {
  if (!volume_ || mip >= subresources_.size())
    return std::nullopt;
  const auto &layout = volume_layout_;
  const auto &sub = subresources_[mip];
  const uint32_t sx = x / layout.shape_w, sy = y / layout.shape_h, sz = z / layout.shape_d;
  if (sx >= sub.tiles_x || sy >= sub.tiles_y || sz >= sub.tiles_z)
    return std::nullopt;
  const uint32_t tile = sub.first_tile + (sz * sub.tiles_y + sy) * sub.tiles_x + sx;
  const uint32_t local_x = (x % layout.shape_w) / layout.block_w;
  const uint32_t units_x = layout.shape_w / layout.block_w;
  const uint32_t flat = local_x + units_x * ((y % layout.shape_h) / layout.block_h +
      (layout.shape_h / layout.block_h) * (z % layout.shape_d));
  const uint32_t page_units_x = layout.page_w / layout.block_w;
  return ReservedVolumeAddress{
      (tile % layout.atlas_columns) * layout.page_w + (flat % page_units_x) * layout.block_w + x % layout.block_w,
      ((tile / layout.atlas_columns) % layout.atlas_rows) * layout.page_h + (flat / page_units_x) * layout.block_h +
          y % layout.block_h,
      tile / (layout.atlas_columns * layout.atlas_rows),
      std::min(units_x - local_x, page_units_x - flat % page_units_x)};
}

void
ReservedResource::GetResourceTiling(
    UINT *TotalTileCount, D3D12_PACKED_MIP_INFO *PackedMipInfo, D3D12_TILE_SHAPE *StandardTileShape,
    UINT *SubresourceTilingCount, UINT FirstSubresourceTiling, D3D12_SUBRESOURCE_TILING *SubresourceTilings
) const {
  if (TotalTileCount)
    *TotalTileCount = total_tiles_;
  if (PackedMipInfo) {
    *PackedMipInfo = {};
    if (!is_buffer_) {
      PackedMipInfo->NumStandardMips = UINT8(standard_mips_);
      PackedMipInfo->NumPackedMips = UINT8(packed_mips_);
      PackedMipInfo->NumTilesForPackedMips = packed_tiles_;
      PackedMipInfo->StartTileIndexInOverallResource = packed_mips_ ? packed_first_tile_ : 0;
    }
  }
  if (StandardTileShape) {
    /* Texels (BC formats included: BC1 is 512x256); bytes for buffers. */
    *StandardTileShape = shape_;
  }
  if (!SubresourceTilingCount)
    return;
  const UINT total = UINT(subresources_.size());
  const UINT count = FirstSubresourceTiling < total ? std::min(*SubresourceTilingCount, total - FirstSubresourceTiling)
                                                    : 0;
  *SubresourceTilingCount = count;
  if (!SubresourceTilings)
    return;
  for (UINT i = 0; i < count; i++) {
    const auto &sub = subresources_[FirstSubresourceTiling + i];
    SubresourceTilings[i] = sub.packed ? D3D12_SUBRESOURCE_TILING{0, 0, 0, D3D12_PACKED_TILE}
                                       : D3D12_SUBRESOURCE_TILING{sub.tiles_x, UINT16(sub.tiles_y),
                                                                  UINT16(sub.tiles_z), sub.first_tile};
  }
}

std::optional<uint32_t>
ReservedResource::TileIndex(const D3D12_TILED_RESOURCE_COORDINATE &coordinate) const {
  if (coordinate.Subresource >= subresources_.size())
    return std::nullopt;
  const auto &sub = subresources_[coordinate.Subresource];
  if (sub.packed) {
    /* Packed mips are addressed as a linear run of tiles from the first packed subresource. */
    if (coordinate.Y || coordinate.Z || coordinate.X >= packed_tiles_)
      return std::nullopt;
    return sub.first_tile + coordinate.X;
  }
  if (coordinate.X >= sub.tiles_x || coordinate.Y >= sub.tiles_y || coordinate.Z >= sub.tiles_z)
    return std::nullopt;
  return sub.first_tile + (uint64_t(coordinate.Z) * sub.tiles_y + coordinate.Y) * sub.tiles_x + coordinate.X;
}

bool
ReservedResource::ExpandRegion(
    const D3D12_TILED_RESOURCE_COORDINATE &start, const D3D12_TILE_REGION_SIZE &size, std::vector<uint32_t> &tiles
) const {
  if (!size.UseBox) {
    /* NumTiles consecutive tiles in linear order, which may continue into later subresources. */
    auto first = TileIndex(start);
    if (!first || size.NumTiles > total_tiles_ - *first)
      return false;
    for (uint32_t i = 0; i < size.NumTiles; i++)
      tiles.push_back(*first + i);
    return true;
  }
  if (!size.Width || !size.Height || !size.Depth ||
      uint64_t(size.Width) * size.Height * size.Depth != size.NumTiles)
    return false;
  if (start.Subresource >= subresources_.size())
    return false;
  const auto &sub = subresources_[start.Subresource];
  if (sub.packed) {
    if (size.Height != 1 || size.Depth != 1)
      return false;
  } else if (uint64_t(start.X) + size.Width > sub.tiles_x || uint64_t(start.Y) + size.Height > sub.tiles_y ||
             uint64_t(start.Z) + size.Depth > sub.tiles_z) {
    return false;
  }
  for (uint32_t z = 0; z < size.Depth; z++)
    for (uint32_t y = 0; y < size.Height; y++)
      for (uint32_t x = 0; x < size.Width; x++) {
        auto tile = TileIndex({start.X + x, start.Y + y, start.Z + z, start.Subresource});
        if (!tile)
          return false;
        tiles.push_back(*tile);
      }
  return true;
}

const ReservedSubresourceTiling *
ReservedResource::subresourceOfTile(uint32_t tile, uint32_t &x, uint32_t &y, uint32_t &z) const {
  for (const auto &sub : subresources_) {
    if (sub.packed)
      continue;
    const uint32_t count = sub.tiles_x * sub.tiles_y * sub.tiles_z;
    if (tile >= sub.first_tile && tile < sub.first_tile + count) {
      const uint32_t local = tile - sub.first_tile;
      x = local % sub.tiles_x;
      y = (local / sub.tiles_x) % sub.tiles_y;
      z = local / (sub.tiles_x * sub.tiles_y);
      return &sub;
    }
  }
  return nullptr;
}

void
ReservedResource::Retain(const ReservedTileTarget &target) {
  if (target.heap_tile == kReservedNullTile)
    return;
  auto [entry, inserted] = heaps_.try_emplace(target.heap.handle);
  if (inserted)
    entry->second.first = target.heap;
  entry->second.second++;
}

void
ReservedResource::Release(const ReservedTileTarget &target) {
  if (target.heap_tile == kReservedNullTile)
    return;
  auto entry = heaps_.find(target.heap.handle);
  if (entry != heaps_.end() && !--entry->second.second)
    heaps_.erase(entry);
}

ReservedTileTarget
ReservedResource::CurrentTarget(uint32_t tile) {
  std::lock_guard<dxmt::mutex> lock(mutex);
  return tile < targets_.size() ? targets_[tile] : ReservedTileTarget{};
}

void
ReservedResource::Apply(
    WMT::MTL4CommandQueue queue, WMT::Buffer buffer, WMT::Texture texture,
    const std::vector<std::pair<uint32_t, ReservedTileTarget>> &assignments
) {
  std::lock_guard<dxmt::mutex> lock(mutex);
  /* Later assignments to the same tile win, exactly like overlapping D3D12 regions. */
  std::vector<std::pair<uint32_t, ReservedTileTarget>> effective;
  effective.reserve(assignments.size());
  for (const auto &[tile, target] : assignments) {
    if (targets_[tile] == target)
      continue;
    Retain(target);
    Release(targets_[tile]);
    targets_[tile] = target;
    if (target.heap_tile == kReservedNullTile) mapped_tiles_.erase(tile);
    else mapped_tiles_.insert(tile);
    effective.emplace_back(tile, target);
  }
  if (effective.empty())
    return;
  mapping_generation_++;
  if (is_buffer_)
    EncodeBuffer(queue, buffer, effective);
  else
    EncodeTexture(queue, texture, effective);
  for (const auto &[format, atlas] : volume_cast_atlases_)
    EncodeTexture(queue, atlas, effective);
  if (volume_device && status_buffer) {
    const HRESULT hr = volume_device->PublishSparseVolumeResidency(status_buffer, effective);
    if (FAILED(hr)) volume_device->ReportRemoval(hr);
  }
  if (status_bytes)
    for (const auto &[tile, target] : effective)
      status_bytes[tile] = target.heap_tile != kReservedNullTile;
}

uint64_t
ReservedResource::CollectMappedTiles(uint64_t seen_generation, std::vector<uint32_t> &tiles, uint32_t subresource) {
  std::lock_guard<dxmt::mutex> lock(mutex);
  if (seen_generation == mapping_generation_)
    return mapping_generation_;
  tiles.clear();
  if (subresource != UINT32_MAX && subresource >= subresources_.size())
    return mapping_generation_;
  const uint32_t first = subresource == UINT32_MAX ? 0 : subresources_[subresource].first_tile;
  const uint64_t end = subresource == UINT32_MAX ? total_tiles_ : uint64_t(first) +
      uint64_t(subresources_[subresource].tiles_x) * subresources_[subresource].tiles_y * subresources_[subresource].tiles_z;
  for (uint32_t tile : mapped_tiles_)
    if (tile >= first && tile < end)
      tiles.push_back(tile);
  return mapping_generation_;
}

void
ReservedResource::MapCastTiles(WMT::MTL4CommandQueue queue, WMT::Texture texture, WMT::Heap heap,
                               const std::unordered_set<uint32_t> &tiles) {
  std::vector<std::pair<uint32_t,ReservedTileTarget>> assignments;
  assignments.reserve(tiles.size());
  uint32_t offset=0;
  for (uint32_t tile : tiles)
    assignments.push_back({tile,heap ? ReservedTileTarget{heap,offset++} : ReservedTileTarget{}});
  EncodeTexture(queue,texture,assignments);
}

WMT::Reference<WMT::Texture>
ReservedResource::GetVolumeCastAtlas(MTLD3D12Device *owner, WMTPixelFormat format, bool compressed) {
  if (block_width_==4 && !compressed)
    format=bytes_per_unit_==8 ? WMTPixelFormatRG32Uint : WMTPixelFormatRGBA32Uint;
  GPUExecutionGate::Ticket execution;
  // Encoding already owns shared or exclusive admission. Re-entering the
  // reader gate here can deadlock a writer waiting for that very submission.
  if (!owner->gpu_execution_gate.CurrentThreadAdmitted()) {
    execution = owner->gpu_execution_gate.AcquireShared();
    if (!execution) return {};
  }
  auto device = owner->GetMTLDevice();
  std::lock_guard<std::mutex> queue_lock(MappingQueueMutex);
  std::lock_guard<dxmt::mutex> lock(mutex);
  if (auto existing = volume_cast_atlases_.find(format); existing != volume_cast_atlases_.end()) return existing->second;
  if (!volume_ || !volume_mapping_queue) return {};
  WMTTextureInfo info{};
  info.type = WMTTextureType2DArray;
  info.pixel_format = format;
  info.width = volume_layout_.atlas_columns * volume_layout_.page_w / (compressed ? 1 : block_width_);
  info.height = volume_layout_.atlas_rows * volume_layout_.page_h / (compressed ? 1 : block_height_);
  info.depth = 1;
  info.array_length = (total_tiles_ + volume_layout_.atlas_columns * volume_layout_.atlas_rows - 1) /
      (volume_layout_.atlas_columns * volume_layout_.atlas_rows);
  info.mipmap_level_count = info.sample_count = 1;
  info.usage = WMTTextureUsage(WMTTextureUsageShaderRead | WMTTextureUsagePixelFormatView |
      (compressed ? 0 : WMTTextureUsageShaderWrite | WMTTextureUsageRenderTarget));
  info.options = WMTResourceOptions(WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate);
  auto atlas = device.newPlacementSparseTexture(info, WMTSparsePageSize64);
  auto completed = device.newSharedEvent();
  if (!atlas || !completed) return {};
  std::vector<std::pair<uint32_t, ReservedTileTarget>> assignments;
  assignments.reserve(mapped_tiles_.size());
  for (uint32_t tile : mapped_tiles_)
    assignments.emplace_back(tile, targets_[tile]);
  EncodeTexture(volume_mapping_queue, atlas, assignments);
  volume_mapping_queue.signalEvent(completed, 1);
  if (!completed.waitUntilSignaledValue(1, 10000)) return {};
  volume_cast_atlases_.emplace(format, atlas);
  return atlas;
}

void
ReservedResource::EncodeBuffer(
    WMT::MTL4CommandQueue queue, WMT::Buffer buffer,
    const std::vector<std::pair<uint32_t, ReservedTileTarget>> &assignments
) {
  /* One Metal call per heap (unmaps use a null heap); runs of consecutive tiles become one range. */
  std::vector<WMTSparseBufferMapping> operations;
  auto flush = [&](WMT::Heap heap) {
    if (!operations.empty())
      queue.updateBufferMappings(buffer, heap, operations.data(), operations.size());
    operations.clear();
  };
  WMT::Heap current{};
  for (const auto &[tile, target] : assignments) {
    const bool unmap = target.heap_tile == kReservedNullTile;
    const WMT::Heap heap = unmap ? WMT::Heap{} : target.heap;
    if (heap.handle != current.handle) {
      flush(current);
      current = heap;
    }
    if (!operations.empty()) {
      auto &last = operations.back();
      const bool contiguous = last.first_tile + last.tile_count == tile;
      if (contiguous && unmap && last.mode == WMTSparseMappingModeUnmap) {
        last.tile_count++;
        continue;
      }
      if (contiguous && !unmap && last.mode == WMTSparseMappingModeMap &&
          last.heap_offset + last.tile_count == target.heap_tile) {
        last.tile_count++;
        continue;
      }
    }
    operations.push_back({unmap ? WMTSparseMappingModeUnmap : WMTSparseMappingModeMap, 0, tile, 1,
                          unmap ? 0 : target.heap_tile});
  }
  flush(current);
}

void
ReservedResource::EncodeTexture(
    WMT::MTL4CommandQueue queue, WMT::Texture texture,
    const std::vector<std::pair<uint32_t, ReservedTileTarget>> &assignments
) {
  std::vector<WMTSparseTextureMapping> operations;
  auto flush = [&](WMT::Heap heap) {
    if (!operations.empty())
      queue.updateTextureMappings(texture, heap, operations.data(), operations.size());
    operations.clear();
  };
  WMT::Heap current{};
  for (const auto &[tile, target] : assignments) {
    const bool unmap = target.heap_tile == kReservedNullTile;
    const WMT::Heap heap = unmap ? WMT::Heap{} : target.heap;
    if (heap.handle != current.handle) {
      flush(current);
      current = heap;
    }
    WMTSparseTextureMapping op{};
    op.mode = unmap ? WMTSparseMappingModeUnmap : WMTSparseMappingModeMap;
    op.heap_offset = unmap ? 0 : target.heap_tile;
    op.size = {1, 1, 1};
    if (volume_) {
      op.level = 0;
      op.slice = tile / (volume_layout_.atlas_columns * volume_layout_.atlas_rows);
      op.origin = {tile % volume_layout_.atlas_columns,
          (tile / volume_layout_.atlas_columns) % volume_layout_.atlas_rows, 0};
    } else if (isPackedTile(tile)) {
      /* Each slice has its own tail, with the same offset within its linear tile range. */
      const uint32_t tiles_per_slice = total_tiles_ / array_size_;
      op.level = metal_tail_level_;
      op.slice = tile / tiles_per_slice;
      op.origin = {tile % tiles_per_slice - packed_first_tile_, 0, 0};
    } else {
      uint32_t x = 0, y = 0, z = 0;
      const auto *sub = subresourceOfTile(tile, x, y, z);
      if (!sub)
        continue;
      op.level = sub->mip;
      op.slice = sub->slice;
      op.origin = {x, y, z};
    }
    /* Metal assigns heap tiles in increasing order across a region's x, so extend runs along x. */
    if (!operations.empty()) {
      auto &last = operations.back();
      if (last.mode == op.mode && last.level == op.level && last.slice == op.slice && last.size.height == 1 &&
          last.origin.y == op.origin.y && last.origin.z == op.origin.z &&
          last.origin.x + last.size.width == op.origin.x &&
          (unmap || last.heap_offset + last.size.width == op.heap_offset)) {
        last.size.width++;
        continue;
      }
    }
    operations.push_back(op);
  }
  flush(current);
}

} // namespace dxmt
