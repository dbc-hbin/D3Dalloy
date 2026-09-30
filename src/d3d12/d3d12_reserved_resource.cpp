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
#include "log/log.hpp"
#include <algorithm>

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
    /* Tier 2: packed mips and array slices cannot be combined (Tier 4 lifts this). */
    if (desc.DepthOrArraySize > 1)
      return E_INVALIDARG;
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
      tiling->packed_first_tile_ = uint32_t(next);
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
    return packed_first_tile_ + coordinate.X;
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
    effective.emplace_back(tile, target);
  }
  if (effective.empty())
    return;
  if (is_buffer_)
    EncodeBuffer(queue, buffer, effective);
  else
    EncodeTexture(queue, texture, effective);
  if (status_bytes)
    for (const auto &[tile, target] : effective)
      status_bytes[tile] = target.heap_tile != kReservedNullTile;
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
    if (isPackedTile(tile)) {
      /* The tail of one slice (arrays never have packed mips here): x indexes its tiles. */
      op.level = metal_tail_level_;
      op.slice = 0;
      op.origin = {tile - packed_first_tile_, 0, 0};
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
