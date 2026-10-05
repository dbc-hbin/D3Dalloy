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

#pragma once

#include "d3d12.h"
#include "Metal.hpp"
#include "thread.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dxmt {

class MTLD3D12Device;

/*
 * Reserved (tiled) resources over Metal placement sparse resources.
 *
 * Every D3D12 tile is one 64KB Metal sparse page (MTLSparsePageSize64). Native 2D tile shapes are
 * checked per format. A 3D tile's row-major format units are flattened into one sparse 2D-array
 * atlas page, retaining exact independent mapping, NULL writes and shared-heap aliases even when
 * native 3D sparse pages have a different shape. MSAA reserved resources are not offered.
 *
 * Tile mappings are queue operations. A queue applies an update on its submission thread after the
 * GPU work it submitted earlier has completed (unless the caller passed NO_HAZARD), so every command
 * buffer committed before the update observes the old mapping and every later one the new mapping.
 * Metal applies MTL4 mapping updates when they are enqueued, not in event order, which is why the
 * ordering is established on the CPU. The mapping state below changes only in that order.
 */
constexpr uint32_t kReservedTileBytes = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
constexpr uint32_t kReservedNullTile = ~0u;

struct ReservedSubresourceTiling {
  uint32_t tiles_x = 0, tiles_y = 0, tiles_z = 0;
  uint32_t first_tile = 0;
  uint32_t mip = 0, slice = 0;
  bool packed = false;
};

/* Shader ABI: fourteen uint32 header words followed by four words per mip, then
 * an aligned uint64 residency-table address (zero means immutable fully backed).
 * One logical 64KB volume tile is one native sparse 2D page; its format units
 * are flattened x, y, z inside that page. Shared by shader addressing and copies. */
struct ReservedVolumeLayout {
  uint32_t width, height, depth, mips;
  uint32_t shape_w, shape_h, shape_d, block_w, block_h;
  uint32_t page_w, page_h, atlas_columns, atlas_rows, total_tiles;
};
static_assert(sizeof(ReservedVolumeLayout) == 14 * sizeof(uint32_t));
constexpr uint64_t kReservedVolumeDescriptorTag = 1ull << 62;

struct ReservedVolumeAddress {
  uint32_t x, y, slice;
  /* Contiguous format units before a logical tile or native page row boundary. */
  uint32_t units;
};

/* One Metal page of one heap, or the null mapping. */
struct ReservedTileTarget {
  WMT::Heap heap{};
  uint32_t heap_tile = kReservedNullTile;

  bool
  operator==(const ReservedTileTarget &other) const {
    return heap.handle == other.heap.handle && heap_tile == other.heap_tile;
  }
};

class ReservedResource {
public:
  static inline std::mutex MappingQueueMutex;
  ReservedResource() { live_.fetch_add(1, std::memory_order_relaxed); }
  ~ReservedResource() { live_.fetch_sub(1, std::memory_order_relaxed); }
  ReservedResource(const ReservedResource &) = delete;
  ReservedResource &operator=(const ReservedResource &) = delete;

  /*
   * Whether any reserved resource exists in the process. Metal keeps sparse tiles that share a heap
   * page (REUSE_SINGLE_TILE, CopyTileMappings, overlapping UpdateTileMappings) coherent only across a
   * command buffer or an event signal/wait: a write through one tile stays invisible through another
   * after an MTLFence pass boundary or memory barrier (GPU probe, M5 Pro, macOS 27). Barriers
   * therefore record a QueryEncoderOp::AliasFlush while reserved resources exist.
   */
  static bool
  AnyLive() {
    return live_.load(std::memory_order_relaxed) != 0;
  }

  /*
   * Whether a Metal 2D tile of `tile_shape` texels equals the D3D12 standard tile for a format of
   * `bytes_per_unit` bytes per unit (a block of block_width x block_height texels for BC).
   */
  static bool MatchesStandardShape2D(
      WMTSparseTileShape tile_shape, uint32_t block_width, uint32_t block_height, uint32_t bytes_per_unit
  );

  /* Validates a reserved buffer description and builds its tiling. */
  static HRESULT CreateBufferTiling(const D3D12_RESOURCE_DESC &desc, std::unique_ptr<ReservedResource> &out);

  /*
   * Validates a reserved texture description against the Metal texture `native` just created for it
   * and builds its tiling. `tile_shape` is Metal's tile for the format, in texels.
   */
  static HRESULT CreateTextureTiling(
      const D3D12_RESOURCE_DESC &desc, WMT::Texture native, WMTSparseTileShape tile_shape, uint32_t block_width,
      uint32_t block_height, uint32_t bytes_per_unit, std::unique_ptr<ReservedResource> &out
  );

  static HRESULT CreateVolumeTiling(
      const D3D12_RESOURCE_DESC &desc, WMTSparseTileShape native_page, uint32_t block_width,
      uint32_t block_height, uint32_t bytes_per_unit, std::unique_ptr<ReservedResource> &out
  );
  static uint64_t VolumeTileCount(const D3D12_RESOURCE_DESC &desc, uint32_t block_width,
                                  uint32_t block_height, uint32_t bytes_per_unit);
  std::optional<ReservedVolumeAddress> VolumeAddress(uint32_t mip, uint32_t x, uint32_t y, uint32_t z) const;
  const ReservedVolumeLayout *volumeLayout() const { return volume_ ? &volume_layout_ : nullptr; }
  WMT::Reference<WMT::Buffer> volume_layout_buffer;
  uint64_t volume_layout_address = 0;
  WMT::MTL4CommandQueue volume_mapping_queue{};
  MTLD3D12Device *volume_device = nullptr;
  // Appended after the immutable 14 + 4*mips layout words; zero for ordinary volumes.
  uint64_t volume_status_address = 0;

  struct VolumeCastLayout { WMT::Reference<WMT::Buffer> buffer; uint64_t address; };
  std::unordered_map<uint32_t, VolumeCastLayout> volume_cast_layouts;
  WMT::Reference<WMT::Texture> GetVolumeCastAtlas(MTLD3D12Device *owner, WMTPixelFormat format, bool compressed);

  bool
  isBuffer() const {
    return is_buffer_;
  }

  uint32_t
  totalTiles() const {
    return total_tiles_;
  }

  void GetResourceTiling(
      UINT *TotalTileCount, D3D12_PACKED_MIP_INFO *PackedMipInfo, D3D12_TILE_SHAPE *StandardTileShape,
      UINT *SubresourceTilingCount, UINT FirstSubresourceTiling, D3D12_SUBRESOURCE_TILING *SubresourceTilings
  ) const;

  /* Linear tile index of a coordinate, or nullopt when it is outside the resource. */
  std::optional<uint32_t> TileIndex(const D3D12_TILED_RESOURCE_COORDINATE &coordinate) const;

  /*
   * Expands a D3D12 tile region into linear tile indices, in D3D12 walk order (x, then y, then z for
   * boxes; linear across subresources otherwise). Returns false for a region outside the resource
   * or with inconsistent NumTiles.
   */
  bool ExpandRegion(
      const D3D12_TILED_RESOURCE_COORDINATE &start, const D3D12_TILE_REGION_SIZE &size,
      std::vector<uint32_t> &tiles
  ) const;

  const ReservedSubresourceTiling *
  subresourceOfTile(uint32_t tile, uint32_t &x, uint32_t &y, uint32_t &z) const;

  bool
  isPackedTile(uint32_t tile) const {
    return packed_tiles_ && tile < total_tiles_ &&
           tile % (total_tiles_ / array_size_) >= packed_first_tile_;
  }

  const std::vector<ReservedSubresourceTiling> &
  subresources() const {
    return subresources_;
  }

  D3D12_TILE_SHAPE
  tileShape() const {
    return shape_;
  }

  uint32_t
  bytesPerUnit() const {
    return bytes_per_unit_;
  }

  uint32_t
  blockWidth() const {
    return block_width_;
  }

  uint32_t
  blockHeight() const {
    return block_height_;
  }

  uint32_t
  mipLevels() const {
    return mip_levels_;
  }

  /*
   * Queue-ordered mapping state. `assignments` pairs a linear tile with its new target. The Metal
   * update operations are enqueued on `queue`; `status` (buffers) receives a byte per tile.
   */
  void Apply(
      WMT::MTL4CommandQueue queue, WMT::Buffer buffer, WMT::Texture texture,
      const std::vector<std::pair<uint32_t, ReservedTileTarget>> &assignments
  );

  /* The current target of each tile, read in queue order by CopyTileMappings. */
  ReservedTileTarget CurrentTarget(uint32_t tile);
  uint64_t CollectMappedTiles(uint64_t seen_generation, std::vector<uint32_t> &tiles,
                              uint32_t subresource = UINT32_MAX);
  void MapCastTiles(WMT::MTL4CommandQueue queue, WMT::Texture texture, WMT::Heap heap,
                    const std::unordered_set<uint32_t> &tiles);

  /*
   * Buffers: CPU-visible residency bytes, biased in BufferAllocation::residencyStatusBase.
   * Reserved volumes: GPU-published uint32 words per logical tile; status_bytes stays null.
   */
  WMT::Reference<WMT::Buffer> status_buffer;
  uint8_t *status_bytes = nullptr;

  dxmt::mutex mutex;

private:
  void EncodeTexture(
      WMT::MTL4CommandQueue queue, WMT::Texture texture,
      const std::vector<std::pair<uint32_t, ReservedTileTarget>> &assignments
  );
  void EncodeBuffer(
      WMT::MTL4CommandQueue queue, WMT::Buffer buffer,
      const std::vector<std::pair<uint32_t, ReservedTileTarget>> &assignments
  );
  void Retain(const ReservedTileTarget &target);
  void Release(const ReservedTileTarget &target);

  static inline std::atomic<uint32_t> live_{0};

  bool is_buffer_ = false;
  bool volume_ = false;
  ReservedVolumeLayout volume_layout_{};
  uint32_t total_tiles_ = 0;
  D3D12_TILE_SHAPE shape_{};
  uint32_t mip_levels_ = 1;
  uint32_t array_size_ = 1;
  uint32_t standard_mips_ = 0;
  uint32_t packed_mips_ = 0;
  uint32_t packed_tiles_ = 0;
  uint32_t packed_first_tile_ = 0;
  uint32_t metal_tail_level_ = 0;
  uint32_t bytes_per_unit_ = 0;
  uint32_t block_width_ = 1, block_height_ = 1;
  std::vector<ReservedSubresourceTiling> subresources_;

  /* Guarded by `mutex`. Heaps stay alive while any tile maps into them. */
  std::vector<ReservedTileTarget> targets_;
  std::unordered_set<uint32_t> mapped_tiles_;
  uint64_t mapping_generation_ = 0;
  std::unordered_map<WMTPixelFormat, WMT::Reference<WMT::Texture>> volume_cast_atlases_;
  std::unordered_map<obj_handle_t, std::pair<WMT::Reference<WMT::Heap>, uint32_t>> heaps_;
};

} // namespace dxmt
