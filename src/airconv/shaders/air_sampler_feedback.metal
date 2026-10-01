#include <metal_stdlib>

using namespace metal;

/*
 * WriteSamplerFeedback* (DXIL ops 174..177) for SamplerFeedbackTier 1.0.
 *
 * A feedback map is an owned buffer of 32 bit cells, one per decoded R8_UINT texel
 * (src/d3d12/d3d12_sampler_feedback.hpp describes the layout and the descriptor words).
 * Zero is "cleared" for both kinds: MIN_MIP stores 255 - mip and is updated with an atomic
 * max, MIP_REGION_USED stores 1 and is updated with an atomic or.
 *
 * The helper establishes the sampling pattern the paired Sample would use: the view-space
 * LOD from the gradients (D3D isotropic or anisotropic LOD, sampler and instruction bias,
 * sampler MinLOD/MaxLOD, the instruction clamp and the view's ResourceMinLODClamp), the
 * one or two mips of the mip filter, the anisotropic taps along the major axis, the 1 or
 * 2x2 texel footprint of the min/mag filter and the sampler's address modes. Every texel
 * of that footprint is mapped to its mip region and recorded. Texels a BORDER address
 * mode places outside the texture are not read and record nothing.
 */

namespace {

struct Layout {
  uint width, height, mips, slices;
  uint region_w_log2, region_h_log2;
  bool region_used;
};

inline Layout unpack(ulong q) {
  Layout l;
  l.width = uint(q & 0xffffu);
  l.height = uint((q >> 16) & 0xffffu);
  l.mips = uint((q >> 32) & 0x1fu);
  l.slices = uint((q >> 37) & 0xfffu);
  l.region_w_log2 = uint((q >> 49) & 0xfu);
  l.region_h_log2 = uint((q >> 53) & 0xfu);
  l.region_used = ((q >> 57) & 1u) != 0;
  return l;
}

inline uint grid_w(Layout l) { return (l.width + (1u << l.region_w_log2) - 1u) >> l.region_w_log2; }
inline uint grid_h(Layout l) { return (l.height + (1u << l.region_h_log2) - 1u) >> l.region_h_log2; }

/* D3D12_TEXTURE_ADDRESS_MODE: WRAP 1, MIRROR 2, CLAMP 3, BORDER 4, MIRROR_ONCE 5. */
inline bool apply_address(int t, int size, uint mode, thread int &out) {
  switch (mode) {
  case 1: {
    int r = t % size;
    out = r < 0 ? r + size : r;
    return true;
  }
  case 2: {
    const int period = 2 * size;
    int r = t % period;
    r = r < 0 ? r + period : r;
    out = r >= size ? period - 1 - r : r;
    return true;
  }
  case 4:
    out = t;
    return t >= 0 && t < size;
  case 5:
    out = clamp(t < 0 ? -t - 1 : t, 0, size - 1);
    return true;
  default:
    out = clamp(t, 0, size - 1);
    return true;
  }
}

inline int texel_floor(float x) { return int(floor(clamp(x, -16777216.0f, 16777216.0f))); }

/*
 * A texel of mip `level` is mapped by its top-left corner projected into mip-0 texel space,
 * x0 = floor(tx * width / mip_width), and then to the mip-0 region x0 >> log2(region width).
 * MIN_MIP records into that region (the spec's "top left texel only" representation when a
 * coarse texel spans several regions); MIP_REGION_USED records into the cell (region >> level)
 * of the decoded mip grid, max(1, grid0 >> level) cells wide. For power-of-two textures this
 * is exactly region tx >> log2(region width) of the mip; for others it is the spec's projected
 * grid. It never leaves the decoded grid.
 */
inline void record(device atomic_uint *map, Layout l, uint slice, uint level, int tx, int ty, uint sx, uint sy) {
  const uint gw0 = grid_w(l), gh0 = grid_h(l);
  const uint x0 = uint(tx) * l.width / sx, y0 = uint(ty) * l.height / sy;
  uint gx = gw0, gy = gh0, offset = 0, rx = x0 >> l.region_w_log2, ry = y0 >> l.region_h_log2;
  if (l.region_used) {
    uint slice_cells = 0;
    for (uint m = 0; m < l.mips; m++) {
      const uint cells = max(1u, gw0 >> m) * max(1u, gh0 >> m);
      if (m < level)
        offset += cells;
      slice_cells += cells;
    }
    offset += slice * slice_cells;
    gx = max(1u, gw0 >> level);
    gy = max(1u, gh0 >> level);
    rx >>= level;
    ry >>= level;
  } else {
    offset = slice * gw0 * gh0;
  }
  rx = min(gx - 1u, rx);
  ry = min(gy - 1u, ry);
  device atomic_uint *cell = map + offset + ry * gx + rx;
  if (l.region_used)
    atomic_fetch_or_explicit(cell, 1u, memory_order_relaxed);
  else
    atomic_fetch_max_explicit(cell, 255u - level, memory_order_relaxed);
}

} // namespace

void dxmt_sampler_feedback_write(
    device uint *cells, ulong layout_word, ulong sampler_meta, ulong sampler_lod, ulong srv_meta,
    ulong srv_range, float u, float v, float layer, float dudx, float dvdx, float dudy, float dvdy, float value,
    float lod_clamp, uint explicit_level, uint skip
) asm("dxmt.sampler_feedback.write");

void dxmt_sampler_feedback_write(
    device uint *cells, ulong layout_word, ulong sampler_meta, ulong sampler_lod, ulong srv_meta,
    ulong srv_range, float u, float v, float layer, float dudx, float dvdx, float dudy, float dvdy, float value,
    float lod_clamp, uint explicit_level, uint skip
) {
  if (skip || !cells)
    return;
  device atomic_uint *map = (device atomic_uint *)cells;
  const Layout l = unpack(layout_word);
  if (!l.width || !l.height || !l.mips || !l.slices)
    return;

  /* View range (SRV descriptor qword 2); absent for a null SRV: the whole resource. */
  uint first_mip = 0, view_mips = l.mips, first_slice = 0;
  if (srv_range >> 63) {
    first_mip = uint(srv_range & 0xffu);
    view_mips = uint((srv_range >> 8) & 0xffu) - first_mip;
    first_slice = uint((srv_range >> 16) & 0xffffu);
  }
  if (first_mip >= l.mips || !view_mips)
    return;
  view_mips = min(view_mips, l.mips - first_mip);
  const uint view_slices = max(1u, uint(srv_meta >> 32));
  const float srv_min_lod = as_type<float>(uint(srv_meta));

  /* Sampler state (sampler descriptor qwords 2 and 3). */
  const uint flags = uint(sampler_meta >> 32);
  const bool valid_sampler = (flags >> 31) != 0;
  const uint address_u = valid_sampler ? (flags & 7u) : 3u;
  const uint address_v = valid_sampler ? ((flags >> 3) & 7u) : 3u;
  const bool min_linear = valid_sampler ? ((flags >> 6) & 1u) : true;
  const bool mag_linear = valid_sampler ? ((flags >> 7) & 1u) : true;
  const bool mip_linear = valid_sampler ? ((flags >> 8) & 1u) : true;
  const bool anisotropic = valid_sampler && ((flags >> 9) & 1u);
  const uint max_anisotropy = max(1u, (flags >> 10) & 0x1fu);
  const float sampler_bias = as_type<float>(uint(sampler_meta));
  const float sampler_min = valid_sampler ? as_type<float>(uint(sampler_lod)) : 0.0f;
  const float sampler_max = valid_sampler ? as_type<float>(uint(sampler_lod >> 32)) : 3.402823466e+38f;

  const float view_w = float(max(1u, l.width >> first_mip));
  const float view_h = float(max(1u, l.height >> first_mip));

  float lod;
  uint taps = 1;
  float major_u = 0.0f, major_v = 0.0f;
  if (explicit_level) {
    lod = value + sampler_bias;
  } else {
    const float ax = dudx * view_w, ay = dvdx * view_h;
    const float bx = dudy * view_w, by = dvdy * view_h;
    const float px = precise::sqrt(ax * ax + ay * ay);
    const float py = precise::sqrt(bx * bx + by * by);
    const float p_max = max(px, py), p_min = min(px, py);
    float ratio = 1.0f;
    if (anisotropic && p_max > 0.0f) {
      ratio = p_min > 0.0f ? min(ceil(p_max / p_min), float(max_anisotropy)) : float(max_anisotropy);
      taps = uint(ratio);
      major_u = px >= py ? dudx : dudy;
      major_v = px >= py ? dvdx : dvdy;
    }
    lod = precise::log2(p_max / ratio) + sampler_bias + value;
  }
  lod = max(lod, max(max(sampler_min, lod_clamp), srv_min_lod));
  lod = min(lod, sampler_max);

  const bool linear = lod <= 0.0f ? mag_linear : min_linear;
  const float last = float(view_mips - 1u);
  const float clamped = clamp(lod, 0.0f, last);
  uint m0, m1;
  if (mip_linear) {
    m0 = uint(floor(clamped));
    m1 = clamped > float(m0) ? min(m0 + 1u, view_mips - 1u) : m0;
  } else {
    m0 = m1 = min(uint(floor(clamped + 0.5f)), view_mips - 1u);
  }

  uint slice = first_slice;
  if (l.slices > 1 || view_slices > 1)
    slice += uint(clamp(rint(layer), 0.0f, float(view_slices - 1u)));
  if (slice >= l.slices)
    return;

  for (uint pass = 0; pass < (m1 != m0 ? 2u : 1u); pass++) {
    const uint level = first_mip + (pass ? m1 : m0);
    const uint sx = max(1u, l.width >> level), sy = max(1u, l.height >> level);
    for (uint tap = 0; tap < taps; tap++) {
      const float offset = taps > 1 ? (float(tap) + 0.5f) / float(taps) - 0.5f : 0.0f;
      const float su = (u + offset * major_u) * float(sx);
      const float sv = (v + offset * major_v) * float(sy);
      const int x0 = linear ? texel_floor(su - 0.5f) : texel_floor(su);
      const int y0 = linear ? texel_floor(sv - 0.5f) : texel_floor(sv);
      const int extent = linear ? 2 : 1;
      for (int dy = 0; dy < extent; dy++) {
        int ty;
        if (!apply_address(y0 + dy, int(sy), address_v, ty))
          continue;
        for (int dx = 0; dx < extent; dx++) {
          int tx;
          if (!apply_address(x0 + dx, int(sx), address_u, tx))
            continue;
          record(map, l, slice, level, tx, ty, sx, sy);
        }
      }
    }
  }
}
