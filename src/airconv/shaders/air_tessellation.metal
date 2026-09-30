#include <metal_stdlib>

using namespace metal;

enum class partitioning {
  integer,
  pow2,
  fractional_odd,
  fractional_even,
};

using fxp32 = uint;

constexpr constant fxp32 fxp32_0 = 0x0u;
constexpr constant fxp32 fxp32_1 = 0x10000u;
constexpr constant fxp32 fxp32_2 = 0x20000u;
constexpr constant fxp32 fxp32_half = 0x8000u;

uint fxp32_ceil(fxp32 f) {
  return (f >> 16) + ((f & 0xffff) > 0 ? 1 : 0);
}

fxp32 fxp32_invb(uint n, fxp32 v) {
  return (fxp32)(((ulong)n << 32) / v);
}

using fxp16 = ushort;
using fxp16v2 = ushort2;

constexpr constant fxp16 fxp16_0 = ushort{0x0000};
constexpr constant fxp16 fxp16_one_third = ushort{0x2aab};
constexpr constant fxp16 fxp16_1 = ushort{0x8000};

constexpr constant float fxp16_1_f = 32768.0;

using fxp16v2 = ushort2; // 0x0000 -> 0.0 ; 0x8000 -> 1.0

fxp16v2 mix(fxp16v2 a, fxp16v2 b, fxp32 factor) {

  uint2 ai = uint2(a);
  uint2 bi = uint2(b);

  uint f = min(factor >> 1, (uint)fxp16_1);
  uint invf = fxp16_1 - f;

  uint2 ri = ((ai * invf + bi * f) + fxp16_1 / 2) / fxp16_1; // effective right shift 15, round to nearest
  ri = min(ri, uint2(fxp16_1));
  return fxp16v2(ri);
};

float2 to_float(fxp16v2 unorm) {
  return (float2)unorm / float2(fxp16_1_f);
}

template <partitioning partition>
char
get_int_factor(fxp32 factor) {
  return fxp32_ceil(factor);
}

template <>
char
get_int_factor<partitioning::fractional_odd>(fxp32 factor) {
  char c = fxp32_ceil(factor);
  return c & 1 ? c : c + 1;
}

template <>
char
get_int_factor<partitioning::fractional_even>(fxp32 factor) {
  short c = fxp32_ceil(factor);
  return c & 1 ? c + 1 : c;
}

template <partitioning partition>
fxp32
place_in_1d(short index, fxp32 factor);

/* D3D11 fixed-function placement (CHWTessellator::ComputeTessFactorContext + PlacePointIn1D) of a point in the
 * lower half of an edge, index <= half the segment count; callers mirror the upper half. */
uint
remove_msb(uint v) {
  return v ? v & ~(1u << (31 - clz(v))) : 0;
}

fxp32
place_fractional(bool odd, short index, fxp32 factor) {
  fxp32 half_factor = (factor + 1) / 2;
  if (odd || half_factor == fxp32_half)
    half_factor += fxp32_half;
  fxp32 fl = half_factor & 0x7fff0000u;
  fxp32 ce = (half_factor & 0xffff) ? fl + fxp32_1 : half_factor;
  fxp32 frac = half_factor - fl;
  short num_half = ce >> 16;
  if (index >= num_half)
    return fxp32_half;
  short split = ce == fl ? num_half + 1
              : odd      ? (fl == fxp32_1 ? 0 : (short)(remove_msb((fl >> 16) - 1) << 1) + 1)
                         : (short)(remove_msb(fl >> 16) << 1) + 1;
  uint floor_segments = ((fl * 2) >> 16) - (odd ? 1 : 0);
  uint ceil_segments = ((ce * 2) >> 16) - (odd ? 1 : 0);
  fxp32 inv_floor = floor_segments ? (fxp32_1 + floor_segments / 2) / floor_segments : 0;
  fxp32 inv_ceil = ceil_segments ? (fxp32_1 + ceil_segments / 2) / ceil_segments : 0;
  uint on_floor = index > split ? index - 1 : index;
  fxp32 location = on_floor * inv_floor * (fxp32_1 - frac) + (uint)index * inv_ceil * frac;
  return (location + fxp32_half) >> 16;
}

template <>
fxp32
place_in_1d<partitioning::fractional_odd>(short index, fxp32 factor) {
  return place_fractional(true, index, factor);
}

template <>
fxp32
place_in_1d<partitioning::fractional_even>(short index, fxp32 factor) {
  return place_fractional(false, index, factor);
}

/* Integer and pow2 factors are integral; parity follows the factor. The D3D "inside factor 1 is even" rule never
 * applies here: the workload generators raise every inside factor of a non-trivial patch to at least 2. */
template <>
fxp32
place_in_1d<partitioning::integer>(short index, fxp32 factor) {
  return place_fractional((factor >> 16) & 1, index, factor);
}

template <>
fxp32
place_in_1d<partitioning::pow2>(short index, fxp32 factor) {
  return place_in_1d<partitioning::integer>(index, factor);
}

/* Point `index` of a whole integer-partitioned edge with `factor` segments (both halves), 16.16. */
fxp32
place_in_1d_full(short index, fxp32 factor) {
  const short segments = factor >> 16;
  const short mirrored = segments - index;
  const fxp32 lower = place_in_1d<partitioning::integer>(min(index, mirrored), factor);
  return mirrored < index ? fxp32_1 - lower : lower;
}

template <partitioning partition>
fxp32
regularize_factor(float factor) {
  // For integer partitioning, TessFactor range is [1 ... 64] (fractions rounded up).
  return ((uint)clamp(ceil(factor), 1.0f, 64.0f)) << 16;
}

template <>
fxp32
regularize_factor<partitioning::pow2>(float factor) {
  // The D3D11 reference tessellator (CHWTessellator) processes pow2 exactly like integer partitioning;
  // rounding to a power of two is HLSL helper code (CHLSLTessellator), not fixed-function behaviour.
  return ((uint)clamp(ceil(factor), 1.0f, 64.0f)) << 16;
}

template <>
fxp32
regularize_factor<partitioning::fractional_odd>(float factor) {
  // For fractional odd partitioning, TessFactor range is [1 ... 63].
  return clamp(factor, 1.0f, 63.0f) * 65536.0;
}

template <>
fxp32
regularize_factor<partitioning::fractional_even>(float factor) {
  // For fractional even tessellation, TessFactor range is [2 ... 64].
  return clamp(factor, 2.0f, 64.0f) * 65536.0;
}

struct TessMeshWorkload {
  fxp16v2 inner0;
  fxp16v2 inner1;
  fxp16v2 outer0;
  fxp16v2 outer1;
  fxp32 inner_factor;
  fxp32 outer_factor;
  char inner_factor_i;
  char outer_factor_i;
  bool has_complement;
  /* Point-output workloads (gen_workload_*_points): bit 0 drops the inner edge's last point, bit 1 the outer
   * edge's, because the next ring side starts at that point. Always 0 for triangle/line workloads. */
  uchar drop_last;
  fxp16v2 inner0_c;
  fxp16v2 inner1_c;
  fxp16v2 outer0_c;
  fxp16v2 outer1_c;
  fxp32 inner_factor_c;
  fxp32 outer_factor_c;
  char inner_factor_c_i;
  char outer_factor_c_i;
  short patch_index;
};

int
get_next_index(threadgroup int *out_count) {
  return atomic_fetch_add_explicit(reinterpret_cast<threadgroup atomic_int *>(out_count), 1, memory_order_relaxed);
}

template <partitioning partition>
char
ring_int_factor(fxp32 factor) {
  const short ring = factor >> 24;
  return (char)max(0, (int)get_int_factor<partition>(factor & 0xffffffu) - 2 * ring);
}

/* Position of point `tid` (of `segments`) on a workload edge, as a 16.16 fraction from its left end. */
template <partitioning partition>
fxp32
place_on_ring(ushort tid, ushort segments, fxp32 factor) {
  const ushort tid_ref = segments - tid;
  const short ring = factor >> 24;
  const fxp32 base = factor & 0xffffffu;
  if (!ring) {
    fxp32 ratio = place_in_1d<partition>(min(tid, tid_ref), base);
    return tid_ref < tid ? fxp32_1 - ratio : ratio;
  }
  fxp32 point = place_in_1d<partition>(ring + min(tid, tid_ref), base);
  point = tid_ref < tid ? fxp32_1 - point : point;
  const fxp32 start = place_in_1d<partition>(ring, base);
  const fxp32 span = fxp32_1 - 2 * start;
  return (fxp32)((((ulong)(point - start) << 16) + span / 2) / span);
}

template <partitioning partition>
void
emit_tess_mesh_workload(
    object_data TessMeshWorkload *workloads, threadgroup int *out_count, 
    fxp16v2 in0, fxp16v2 in1, fxp16v2 out0, fxp16v2 out1, fxp32 inner_factor, fxp32 outer_factor,
    fxp16v2 in0_c, fxp16v2 in1_c, fxp16v2 out0_c, fxp16v2 out1_c, fxp32 inner_factor_c, fxp32 outer_factor_c, 
    short patch_index, bool has_complement
) {
  workloads[get_next_index(out_count)] = {
      in0,
      in1,
      out0,
      out1,
      inner_factor,
      outer_factor,
      ring_int_factor<partition>(inner_factor),
      ring_int_factor<partition>(outer_factor),
      has_complement,
      0,
      in0_c,
      in1_c,
      out0_c,
      out1_c,
      inner_factor_c,
      outer_factor_c,
      has_complement ? ring_int_factor<partition>(inner_factor_c) : (char)0,
      has_complement ? ring_int_factor<partition>(outer_factor_c) : (char)0,
      patch_index
  };
}

/* Ring k of an inside factor. D3D places ring points with the full inside-factor context at indices shifted by k
 * (CHWTessellator::TriGeneratePoints/QuadGeneratePoints), so the ring keeps the base factor and carries k in
 * bits 24..30; ring_int_factor/place_on_ring decode it. 0 when the ring has no segments. */
fxp32 factor_offset(fxp32 base, short count) {
  fxp32 result = base - ((count * fxp32_1) << 1);
  if (result & 0x80000000)
    return 0;
  return count ? base | ((uint)count << 24) : base;
}

/* Unique D3D11 tessellator domain points: the outer ring, then inner rings of 3 * (n - 2k)
 * points, closed by a centre point when n is even (a triangle of 3 when odd). */
uint
domain_point_count_triangle(char n, char e0, char e1, char e2) {
  uint count = e0 + e1 + e2;
  for (short k = 1; n - 2 * k > 0; k++)
    count += 3 * (n - 2 * k);
  return count + ((n & 1) ? 0 : 1);
}

/* Quad: outer ring, then (nx - 2k) x (ny - 2k) rings, closed by a line of |nx - ny| + 1 points
 * when the smaller inner factor is even. */
uint
domain_point_count_quad(char nx, char ny, char e0, char e1, char e2, char e3) {
  uint count = e0 + e1 + e2 + e3;
  short k = 1;
  for (; nx - 2 * k > 0 && ny - 2 * k > 0; k++)
    count += 2 * (nx - 2 * k) + 2 * (ny - 2 * k);
  if (min(nx, ny) - 2 * k == 0)
    count += abs(nx - ny) + 1;
  return count;
}

/* A patch with any factor above 1 gets an inside factor of at least 2 (fractional odd: above 1). */
template <partitioning partition>
void
regularize_triangle_inside(thread fxp32 &in0f, fxp32 out0f, fxp32 out1f, fxp32 out2f) {
  if (partition == partitioning::fractional_odd) {
    if ((out0f | out1f | out2f | in0f) != fxp32_1)
      in0f = max(fxp32_1 + 1u, in0f);
  } else if (partition != partitioning::fractional_even) {
    if ((out0f | out1f | out2f) != fxp32_1)
      in0f = max(fxp32_2, in0f);
  }
}

template <partitioning partition>
void
regularize_quad_inside(thread fxp32 &in0f, thread fxp32 &in1f, fxp32 out0f, fxp32 out1f, fxp32 out2f, fxp32 out3f) {
  if (partition == partitioning::fractional_odd) {
    if ((out0f | out1f | out2f | out3f | in0f | in1f) != fxp32_1) {
      in0f = max(fxp32_1 + 1u, in0f);
      in1f = max(fxp32_1 + 1u, in1f);
    }
  } else if (partition != partitioning::fractional_even) {
    if ((out0f | out1f | out2f | out3f) != fxp32_1) {
      in0f = max(fxp32_2, in0f);
      in1f = max(fxp32_2, in1f);
    }
  }
}

template <partitioning partition>
uint
gen_workload_triangle_impl(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in, float out0, float out1,
    float out2
) {
  if (isnan(out0) || out0 <= 0) return 0;
  if (isnan(out1) || out1 <= 0) return 0;
  if (isnan(out2) || out2 <= 0) return 0;

  object_data TessMeshWorkload *workloads = (object_data TessMeshWorkload *)out_buffer;
  fxp32 in0f = regularize_factor<partition>(in);
  fxp32 out0f = regularize_factor<partition>(out0);
  fxp32 out1f = regularize_factor<partition>(out1);
  fxp32 out2f = regularize_factor<partition>(out2);

  regularize_triangle_inside<partition>(in0f, out0f, out1f, out2f);

  char in0 = get_int_factor<partition>(in0f);
  const uint points = domain_point_count_triangle(
      in0, get_int_factor<partition>(out0f), get_int_factor<partition>(out1f), get_int_factor<partition>(out2f));

  short rings = in0 / 2;
  fxp16v2 _100{fxp16_1, fxp16_0};
  fxp16v2 _010{fxp16_0, fxp16_1};
  fxp16v2 _001{fxp16_0, fxp16_0};
  fxp16v2 _C = fxp16_one_third;

  short u = rings;

  for (short r = 0; r < u; r++, u--) {

    fxp32 outer = place_in_1d<partition>(r, in0f) << 1;
    fxp32 inner = place_in_1d<partition>(r + 1, in0f) << 1;

    fxp16v2 _001x = mix(_001, _C, outer);
    fxp16v2 _001i = mix(_001, _C, inner);
    fxp16v2 _100x = mix(_100, _C, outer);
    fxp16v2 _100i = mix(_100, _C, inner);
    fxp16v2 _010x = mix(_010, _C, outer);
    fxp16v2 _010i = mix(_010, _C, inner);


    if (r + 1 == u) {
      emit_tess_mesh_workload<partition>(
        workloads, out_count,
        _001i, _100i, _001x, _100x, factor_offset(in0f, r + 1), (r == 0 ? out1f : factor_offset(in0f, r)), 
        _100i, _010i, _100x, _010x, factor_offset(in0f, r + 1), (r == 0 ? out2f : factor_offset(in0f, r)), 
        patch_index, true
      );
      if (in0 & 1) {
        fxp32 outer = place_in_1d<partition>(rings, in0f) << 1;
        fxp16v2 _001xp = mix(_001, _C, outer);
        fxp16v2 _100xp = mix(_100, _C, outer);
        fxp16v2 _010xp = mix(_010, _C, outer);

        emit_tess_mesh_workload<partition>(
          workloads, out_count,
          _010i, _001i, _010x, _001x, factor_offset(in0f, r + 1), (r == 0 ? out0f : factor_offset(in0f ,r)),
          _010xp, _010xp, _001xp, _100xp, fxp32_0, fxp32_1,
          patch_index, true
        );
      } else {
        emit_tess_mesh_workload<partition>(
          workloads, out_count,
          _010i, _001i, _010x, _001x, factor_offset(in0f, r + 1), (r == 0 ? out0f : factor_offset(in0f, r)), 
          _C, _C, _C, _C, 0, 0,
          patch_index, false
        );
      }
      return points;
    }

    fxp32 outer_c = place_in_1d<partition>(u - 1, in0f) << 1;
    fxp32 inner_c = place_in_1d<partition>(u, in0f) << 1;

    fxp16v2 _001x_c = mix(_001, _C, outer_c);
    fxp16v2 _001i_c = mix(_001, _C, inner_c);
    fxp16v2 _100x_c = mix(_100, _C, outer_c);
    fxp16v2 _100i_c = mix(_100, _C, inner_c);
    fxp16v2 _010x_c = mix(_010, _C, outer_c);
    fxp16v2 _010i_c = mix(_010, _C, inner_c);


    emit_tess_mesh_workload<partition>(
        workloads, out_count, _001i, _100i, _001x, _100x, factor_offset(in0f, r + 1),
        (r == 0 ? out1f : factor_offset(in0f, r)), 
        _001i_c, _100i_c, _001x_c, _100x_c, factor_offset(in0f, u), factor_offset(in0f, u - 1), 
        patch_index, true
    );
    emit_tess_mesh_workload<partition>(
        workloads, out_count, _100i, _010i, _100x, _010x, factor_offset(in0f, r + 1),
        (r == 0 ? out2f : factor_offset(in0f, r)),
        _100i_c, _010i_c, _100x_c, _010x_c, factor_offset(in0f, u), factor_offset(in0f, u - 1), 
        patch_index, true
    );
    emit_tess_mesh_workload<partition>(
        workloads, out_count, _010i, _001i, _010x, _001x, factor_offset(in0f, r + 1),
        (r == 0 ? out0f : factor_offset(in0f, r)),
        _010i_c, _001i_c, _010x_c, _001x_c, factor_offset(in0f, u), factor_offset(in0f, u - 1), 
        patch_index, true
    );
  }

  if (in0 & 1) {
    fxp32 outer = place_in_1d<partition>(rings, in0f) << 1;
    fxp16v2 _001x = mix(_001, _C, outer);
    fxp16v2 _100x = mix(_100, _C, outer);
    fxp16v2 _010x = mix(_010, _C, outer);
    emit_tess_mesh_workload<partition>(workloads, out_count,
      _010x, _010x, _001x, _100x, fxp32_0, fxp32_1,
      _C, _C, _C, _C, 0, 0, patch_index, false
    );
  }

  return points;
}

uint gen_workload_triangle_integer(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in, float out0, float out1,
    float out2
) asm("dxmt.generate_workload.triangle.integer");
uint
gen_workload_triangle_integer(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in, float out0, float out1,
    float out2
) {
  return gen_workload_triangle_impl<partitioning::integer>(
      patch_index, out_count, out_buffer, in, out0, out1, out2
  );
}

uint gen_workload_triangle_pow2(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in, float out0, float out1,
    float out2
) asm("dxmt.generate_workload.triangle.pow2");
uint
gen_workload_triangle_pow2(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in, float out0, float out1,
    float out2
) {
  return gen_workload_triangle_impl<partitioning::pow2>(patch_index, out_count, out_buffer, in, out0, out1, out2);
}

uint gen_workload_triangle_odd(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in, float out0, float out1,
    float out2
) asm("dxmt.generate_workload.triangle.odd");
uint
gen_workload_triangle_odd(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in, float out0, float out1,
    float out2
) {
  return gen_workload_triangle_impl<partitioning::fractional_odd>(
      patch_index, out_count, out_buffer, in, out0, out1, out2
  );
}

uint gen_workload_triangle_even(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in, float out0, float out1,
    float out2
) asm("dxmt.generate_workload.triangle.even");
uint
gen_workload_triangle_even(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in, float out0, float out1,
    float out2
) {
  return gen_workload_triangle_impl<partitioning::fractional_even>(
      patch_index, out_count, out_buffer, in, out0, out1, out2
  );
}

fxp16v2 pxmy(fxp32 x, fxp32 y) {
  return {(fxp16)(x >> 1), (fxp16)(fxp16_1 - (y >> 1))};
}

fxp16v2 pxpy(fxp32 x, fxp32 y) {
  return {(fxp16)(x >> 1), (fxp16)(y >> 1)};
}

fxp16v2 mxpy(fxp32 x, fxp32 y) {
  return {(fxp16)(fxp16_1 - (x >> 1)), (fxp16)(y >> 1)};
}

fxp16v2 mxmy(fxp32 x, fxp32 y) {
  return {(fxp16)(fxp16_1 - (x >> 1)), (fxp16)(fxp16_1 - (y >> 1))};
}

template <partitioning partition>
uint
gen_workload_quad_impl(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in0, float in1, float out0,
    float out1, float out2, float out3
) {
  if (isnan(out0) || out0 <= 0) return 0;
  if (isnan(out1) || out1 <= 0) return 0;
  if (isnan(out2) || out2 <= 0) return 0;
  if (isnan(out3) || out3 <= 0) return 0;

  object_data TessMeshWorkload *workloads = (object_data TessMeshWorkload *)out_buffer;
  fxp32 in0f = regularize_factor<partition>(in0);
  fxp32 in1f = regularize_factor<partition>(in1);
  fxp32 out0f = regularize_factor<partition>(out0);
  fxp32 out1f = regularize_factor<partition>(out1);
  fxp32 out2f = regularize_factor<partition>(out2);
  fxp32 out3f = regularize_factor<partition>(out3);

  regularize_quad_inside<partition>(in0f, in1f, out0f, out1f, out2f, out3f);

  char in0i = get_int_factor<partition>(in0f);
  char in1i = get_int_factor<partition>(in1f);
  const uint points = domain_point_count_quad(
      in0i, in1i, get_int_factor<partition>(out0f), get_int_factor<partition>(out1f),
      get_int_factor<partition>(out2f), get_int_factor<partition>(out3f));

  short min_in = min(in0i, in1i);
  short rings = min_in / 2;

  short u = max(in0i, in1i) / 2;

  for (short r = 0; r < u && r < rings; r++, u--) {

    fxp32 x_coord = place_in_1d<partition>(r, in0f);
    fxp32 y_coord = place_in_1d<partition>(r, in1f);
    fxp32 x_coordi = place_in_1d<partition>(r + 1, in0f);
    fxp32 y_coordi = place_in_1d<partition>(r + 1, in1f);

    fxp16v2 _01x = pxmy(x_coord, y_coord);
    fxp16v2 _11x = mxmy(x_coord, y_coord);
    fxp16v2 _00x = pxpy(x_coord, y_coord);
    fxp16v2 _10x = mxpy(x_coord, y_coord);
    fxp16v2 _01i = pxmy(x_coordi, y_coordi);
    fxp16v2 _11i = mxmy(x_coordi, y_coordi);
    fxp16v2 _00i = pxpy(x_coordi, y_coordi);
    fxp16v2 _10i = mxpy(x_coordi, y_coordi);

    if (r + 1 == u) {
      emit_tess_mesh_workload<partition>(
          workloads, out_count,
          _11i, _01i, _11x, _01x, factor_offset(in0f, r + 1), (r == 0 ? out3f : factor_offset(in0f, r)),
          _10i, _11i, _10x, _11x, factor_offset(in1f, r + 1), (r == 0 ? out2f : factor_offset(in1f, r)),
          patch_index, true
      );
      emit_tess_mesh_workload<partition>(
          workloads, out_count,
          _00i, _10i, _00x, _10x, factor_offset(in0f, r + 1), (r == 0 ? out1f : factor_offset(in0f, r)),
          _01i, _00i, _01x, _00x, factor_offset(in1f, r + 1), (r == 0 ? out0f : factor_offset(in1f, r)),
         patch_index, true
      );
      break;
    }

    fxp32 x_coord_c = place_in_1d<partition>(u - 1, in0f);
    fxp32 y_coord_c = place_in_1d<partition>(u - 1, in1f);
    fxp32 x_coordi_c = place_in_1d<partition>(u, in0f);
    fxp32 y_coordi_c = place_in_1d<partition>(u, in1f);

    fxp16v2 _01x_c = pxmy(x_coord_c, y_coord_c);
    fxp16v2 _11x_c = mxmy(x_coord_c, y_coord_c);
    fxp16v2 _00x_c = pxpy(x_coord_c, y_coord_c);
    fxp16v2 _10x_c = mxpy(x_coord_c, y_coord_c);
    fxp16v2 _01i_c = pxmy(x_coordi_c, y_coordi_c);
    fxp16v2 _11i_c = mxmy(x_coordi_c, y_coordi_c);
    fxp16v2 _00i_c = pxpy(x_coordi_c, y_coordi_c);
    fxp16v2 _10i_c = mxpy(x_coordi_c, y_coordi_c);
  
    emit_tess_mesh_workload<partition>(
        workloads, out_count,
        _11i, _01i, _11x, _01x, factor_offset(in0f, r + 1), (r == 0 ? out3f : factor_offset(in0f, r)),
        _11i_c, _01i_c, _11x_c, _01x_c, factor_offset(in0f, u), factor_offset(in0f, u - 1),
        patch_index, u - 1 < rings
    );
    emit_tess_mesh_workload<partition>(
        workloads, out_count,
        _10i, _11i, _10x, _11x, factor_offset(in1f, r + 1), (r == 0 ? out2f : factor_offset(in1f, r)),
        _10i_c, _11i_c, _10x_c, _11x_c, factor_offset(in1f, u), factor_offset(in1f, u - 1),
        patch_index, u - 1 < rings
    );

    emit_tess_mesh_workload<partition>(
        workloads, out_count,
        _00i, _10i, _00x, _10x, factor_offset(in0f, r + 1), (r == 0 ? out1f : factor_offset(in0f, r)),
        _00i_c, _10i_c, _00x_c, _10x_c, factor_offset(in0f, u), factor_offset(in0f, u - 1),
        patch_index, u - 1 < rings
    );
    emit_tess_mesh_workload<partition>(
        workloads, out_count,
        _01i, _00i, _01x, _00x, factor_offset(in1f, r + 1), (r == 0 ? out0f : factor_offset(in1f, r)),
        _01i_c, _00i_c, _01x_c, _00x_c, factor_offset(in1f, u), factor_offset(in1f, u - 1),
        patch_index, u - 1 < rings
    );
  }

  if (min_in & 1) {
    fxp32 x_coord = place_in_1d<partition>(rings, in0f);
    fxp32 y_coord = place_in_1d<partition>(rings, in1f);

    fxp16v2 _01x = pxmy(x_coord, y_coord);
    fxp16v2 _11x = mxmy(x_coord, y_coord);
    fxp16v2 _00x = pxpy(x_coord, y_coord);
    fxp16v2 _10x = mxpy(x_coord, y_coord);

    if (in0 > in1) {
      emit_tess_mesh_workload<partition>(
          workloads, out_count, _01x,_11x, _00x,  _10x, factor_offset(in0f, rings), factor_offset(in0f, rings),
          0, 0, 0, 0, 0, 0,
          patch_index, false
      );
    } else {
      emit_tess_mesh_workload<partition>(
          workloads, out_count, _11x, _10x, _01x, _00x, factor_offset(in1f, rings), factor_offset(in1f, rings),
          0, 0, 0, 0, 0, 0,
          patch_index, false
      );
    }
  }

  return points;
}

uint gen_workload_quad_integer(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in0, float in1, float out0,
    float out1, float out2, float out3
) asm("dxmt.generate_workload.quad.integer");
uint
gen_workload_quad_integer(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in0, float in1, float out0,
    float out1, float out2, float out3
) {
  return gen_workload_quad_impl<partitioning::integer>(
      patch_index, out_count, out_buffer, in0, in1, out0, out1, out2, out3
  );
}

uint gen_workload_quad_pow2(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in0, float in1, float out0,
    float out1, float out2, float out3
) asm("dxmt.generate_workload.quad.pow2");
uint
gen_workload_quad_pow2(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in0, float in1, float out0,
    float out1, float out2, float out3
) {
  return gen_workload_quad_impl<partitioning::pow2>(
      patch_index, out_count, out_buffer, in0, in1, out0, out1, out2, out3
  );
}

uint gen_workload_quad_odd(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in0, float in1, float out0,
    float out1, float out2, float out3
) asm("dxmt.generate_workload.quad.odd");
uint
gen_workload_quad_odd(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in0, float in1, float out0,
    float out1, float out2, float out3
) {
  return gen_workload_quad_impl<partitioning::fractional_odd>(
      patch_index, out_count, out_buffer, in0, in1, out0, out1, out2, out3
  );
}

uint gen_workload_quad_even(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in0, float in1, float out0,
    float out1, float out2, float out3
) asm("dxmt.generate_workload.quad.even");
uint
gen_workload_quad_even(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in0, float in1, float out0,
    float out1, float out2, float out3
) {
  return gen_workload_quad_impl<partitioning::fractional_even>(
      patch_index, out_count, out_buffer, in0, in1, out0, out1, out2, out3
  );
}

/* Point output (D3D11.3 11.7: only unique points within a patch). Instead of the triangle ring workloads, whose
 * edges repeat every inner ring and every corner, each ring side becomes one workload edge that drops its last point
 * (the next side's first point); two sides share a workload. Rings, factors and endpoints are exactly those of
 * gen_workload_triangle_impl / gen_workload_quad_impl, so point positions are identical. */
struct point_side {
  fxp16v2 left, right;
  fxp32 factor;
  bool drop;
};

struct point_emitter {
  object_data TessMeshWorkload *workloads;
  threadgroup int *out_count;
  short patch_index;
  bool pending = false;
  point_side first;

  template <partitioning partition>
  void push(point_side side) {
    if (!pending) {
      first = side;
      pending = true;
      return;
    }
    emit<partition>(first, side, true);
    pending = false;
  }

  template <partitioning partition>
  void flush() {
    if (pending)
      emit<partition>(first, first, false);
    pending = false;
  }

  template <partitioning partition>
  void emit(point_side a, point_side b, bool pair) {
    workloads[get_next_index(out_count)] = {
        a.left, a.right, b.left, b.right, a.factor, b.factor,
        ring_int_factor<partition>(a.factor), (char)(pair ? ring_int_factor<partition>(b.factor) : -1), false,
        (uchar)((a.drop ? 1 : 0) | (pair && b.drop ? 2 : 0)),
        {}, {}, {}, {}, 0, 0, 0, 0, patch_index
    };
  }
};

template <partitioning partition>
uint
gen_workload_triangle_points_impl(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in, float out0, float out1,
    float out2
) {
  if (isnan(out0) || out0 <= 0 || isnan(out1) || out1 <= 0 || isnan(out2) || out2 <= 0)
    return 0;
  fxp32 in0f = regularize_factor<partition>(in);
  fxp32 out0f = regularize_factor<partition>(out0);
  fxp32 out1f = regularize_factor<partition>(out1);
  fxp32 out2f = regularize_factor<partition>(out2);
  regularize_triangle_inside<partition>(in0f, out0f, out1f, out2f);
  const char in0 = get_int_factor<partition>(in0f);
  point_emitter emitter{(object_data TessMeshWorkload *)out_buffer, out_count, (short)patch_index};
  const fxp16v2 _100{fxp16_1, fxp16_0}, _010{fxp16_0, fxp16_1}, _001{fxp16_0, fxp16_0};
  const fxp16v2 _C = fxp16_one_third;
  const short rings = in0 / 2;
  for (short k = 0; k <= rings; k++) {
    const fxp32 at = place_in_1d<partition>(k, in0f) << 1;
    const fxp16v2 a = mix(_001, _C, at), b = mix(_100, _C, at), c = mix(_010, _C, at);
    if (k && in0 - 2 * k == 0) {
      emitter.push<partition>({a, a, 0, false}); // centre point of an even inside factor
      break;
    }
    emitter.push<partition>({a, b, k ? factor_offset(in0f, k) : out1f, true});
    emitter.push<partition>({b, c, k ? factor_offset(in0f, k) : out2f, true});
    emitter.push<partition>({c, a, k ? factor_offset(in0f, k) : out0f, true});
  }
  emitter.flush<partition>();
  return domain_point_count_triangle(in0, get_int_factor<partition>(out0f), get_int_factor<partition>(out1f),
                                     get_int_factor<partition>(out2f));
}

template <partitioning partition>
uint
gen_workload_quad_points_impl(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float in0, float in1, float out0,
    float out1, float out2, float out3
) {
  if (isnan(out0) || out0 <= 0 || isnan(out1) || out1 <= 0 || isnan(out2) || out2 <= 0 || isnan(out3) ||
      out3 <= 0)
    return 0;
  fxp32 in0f = regularize_factor<partition>(in0);
  fxp32 in1f = regularize_factor<partition>(in1);
  fxp32 out0f = regularize_factor<partition>(out0);
  fxp32 out1f = regularize_factor<partition>(out1);
  fxp32 out2f = regularize_factor<partition>(out2);
  fxp32 out3f = regularize_factor<partition>(out3);
  regularize_quad_inside<partition>(in0f, in1f, out0f, out1f, out2f, out3f);
  const char nx = get_int_factor<partition>(in0f), ny = get_int_factor<partition>(in1f);
  point_emitter emitter{(object_data TessMeshWorkload *)out_buffer, out_count, (short)patch_index};
  short k = 0;
  for (; k == 0 || (nx - 2 * k > 0 && ny - 2 * k > 0); k++) {
    const fxp32 x = place_in_1d<partition>(k, in0f), y = place_in_1d<partition>(k, in1f);
    const fxp16v2 _01 = pxmy(x, y), _11 = mxmy(x, y), _00 = pxpy(x, y), _10 = mxpy(x, y);
    emitter.push<partition>({_11, _01, k ? factor_offset(in0f, k) : out3f, true});
    emitter.push<partition>({_10, _11, k ? factor_offset(in1f, k) : out2f, true});
    emitter.push<partition>({_00, _10, k ? factor_offset(in0f, k) : out1f, true});
    emitter.push<partition>({_01, _00, k ? factor_offset(in1f, k) : out0f, true});
  }
  if (min(nx, ny) - 2 * k == 0) {
    // Centre line along the longer inside direction (a point when nx == ny).
    const fxp32 x = place_in_1d<partition>(k, in0f), y = place_in_1d<partition>(k, in1f);
    if (nx >= ny)
      emitter.push<partition>({pxpy(x, y), mxpy(x, y), factor_offset(in0f, k), false});
    else
      emitter.push<partition>({pxpy(x, y), pxmy(x, y), factor_offset(in1f, k), false});
  }
  emitter.flush<partition>();
  return domain_point_count_quad(nx, ny, get_int_factor<partition>(out0f), get_int_factor<partition>(out1f),
                                 get_int_factor<partition>(out2f), get_int_factor<partition>(out3f));
}

#define POINT_ENTRIES(suffix, part)                                                                                  \
  uint gen_workload_triangle_points_##suffix(int patch_index, threadgroup int *out_count,                          \
                                             object_data int *out_buffer, float in, float out0, float out1,        \
                                             float out2) asm("dxmt.generate_workload.triangle_points." #suffix);   \
  uint gen_workload_triangle_points_##suffix(int patch_index, threadgroup int *out_count,                          \
                                             object_data int *out_buffer, float in, float out0, float out1,        \
                                             float out2) {                                                         \
    return gen_workload_triangle_points_impl<part>(patch_index, out_count, out_buffer, in, out0, out1, out2);      \
  }                                                                                                                \
  uint gen_workload_quad_points_##suffix(int patch_index, threadgroup int *out_count, object_data int *out_buffer, \
                                         float in0, float in1, float out0, float out1, float out2, float out3)     \
      asm("dxmt.generate_workload.quad_points." #suffix);                                                          \
  uint gen_workload_quad_points_##suffix(int patch_index, threadgroup int *out_count, object_data int *out_buffer, \
                                         float in0, float in1, float out0, float out1, float out2, float out3) {   \
    return gen_workload_quad_points_impl<part>(patch_index, out_count, out_buffer, in0, in1, out0, out1, out2,     \
                                               out3);                                                              \
  }
POINT_ENTRIES(integer, partitioning::integer)
POINT_ENTRIES(pow2, partitioning::pow2)
POINT_ENTRIES(odd, partitioning::fractional_odd)
POINT_ENTRIES(even, partitioning::fractional_even)
#undef POINT_ENTRIES

/* Isoline domain (D3D11.3 11.6, CHWTessellator::TessellateIsoLineDomain): tess factor 0 is the line detail (U, any
 * partitioning), factor 1 the line density (V, always integer, [1, 64]). Lines sit at V = k / density for
 * k < density (the V = 1 line is not drawn); each has detail + 1 points. One workload carries up to two lines as its
 * inner (first) and outer (second) edge, so get_domain_point and the line connectivity below are unchanged; a
 * single-line workload has outer_factor_i = -1 (no points). */
template <partitioning partition>
uint
gen_workload_isoline_impl(
    int patch_index, threadgroup int *out_count, object_data int *out_buffer, float detail, float density
) {
  if (isnan(detail) || detail <= 0 || isnan(density) || density <= 0)
    return 0;
  object_data TessMeshWorkload *workloads = (object_data TessMeshWorkload *)out_buffer;
  const fxp32 detailf = regularize_factor<partition>(detail);
  const short lines = (short)clamp(ceil(density), 1.0f, 64.0f);
  const fxp32 densityf = (fxp32)lines << 16;
  const char detail_i = get_int_factor<partition>(detailf);
  for (short k = 0; k < lines; k += 2) {
    const fxp16 v0 = (fxp16)(place_in_1d_full(k, densityf) >> 1);
    const bool pair = k + 1 < lines;
    const fxp16 v1 = pair ? (fxp16)(place_in_1d_full(k + 1, densityf) >> 1) : v0;
    workloads[get_next_index(out_count)] = {
        fxp16v2(fxp16_0, v0), fxp16v2(fxp16_1, v0), fxp16v2(fxp16_0, v1), fxp16v2(fxp16_1, v1),
        detailf, detailf, detail_i, (char)(pair ? detail_i : -1), false, 0,
        {}, {}, {}, {}, 0, 0, 0, 0, (short)patch_index
    };
  }
  return (uint)lines * (uint)(detail_i + 1);
}

#define ISOLINE_ENTRY(suffix, part)                                                                                  \
  uint gen_workload_isoline_##suffix(int patch_index, threadgroup int *out_count, object_data int *out_buffer,     \
                                     float detail, float density) asm("dxmt.generate_workload.isoline." #suffix); \
  uint gen_workload_isoline_##suffix(int patch_index, threadgroup int *out_count, object_data int *out_buffer,     \
                                     float detail, float density) {                                               \
    return gen_workload_isoline_impl<part>(patch_index, out_count, out_buffer, detail, density);                    \
  }
ISOLINE_ENTRY(integer, partitioning::integer)
ISOLINE_ENTRY(pow2, partitioning::pow2)
ISOLINE_ENTRY(odd, partitioning::fractional_odd)
ISOLINE_ENTRY(even, partitioning::fractional_even)
#undef ISOLINE_ENTRY

struct domain_location {
  float2 uv;
  bool active;
  bool iterate;
};

template <partitioning partition>
float2
get_domain_point(object_data TessMeshWorkload &workload, ushort tid) {
  bool point_at_out = tid > workload.inner_factor_i;
  bool point_at_in_c = tid > workload.inner_factor_i + workload.outer_factor_i + 1;
  bool point_at_out_c = tid > workload.inner_factor_i + workload.outer_factor_i + workload.inner_factor_c_i + 2;

  char real_factor = workload.inner_factor_i;
  real_factor = select(real_factor, workload.outer_factor_i, point_at_out);
  real_factor = select(real_factor, workload.inner_factor_c_i, point_at_in_c);
  real_factor = select(real_factor, workload.outer_factor_c_i, point_at_out_c);

  tid = select(tid, (ushort)(tid - (workload.inner_factor_i + 1)), point_at_out);
  tid = select(tid, (ushort)(tid - (workload.outer_factor_i + 1)), point_at_in_c);
  tid = select(tid, (ushort)(tid - (workload.inner_factor_c_i + 1)), point_at_out_c);

  fxp16v2 left = workload.inner0;
  left = select(left, workload.outer0, point_at_out);
  left = select(left, workload.inner0_c, point_at_in_c);
  left = select(left, workload.outer0_c, point_at_out_c);

  fxp16v2 right = workload.inner1;
  right = select(right, workload.outer1, point_at_out);
  right = select(right, workload.inner1_c, point_at_in_c);
  right = select(right, workload.outer1_c, point_at_out_c);

  fxp32 factor = workload.inner_factor;
  factor = select(factor, workload.outer_factor, point_at_out);
  factor = select(factor, workload.inner_factor_c, point_at_in_c);
  factor = select(factor, workload.outer_factor_c, point_at_out_c);

  if (real_factor == 0)
    return to_float(min(left, right)); // very edge case...
  return to_float(mix(left, right, place_on_ring<partition>(tid, real_factor, factor)));
}

int get_domain_patch_index(int workload_index, object_data int *data) asm("dxmt.get_domain_patch_index");

int get_domain_patch_index(int workload_index, object_data int *data) {
  object_data TessMeshWorkload &workload = ((object_data TessMeshWorkload *)data)[workload_index];
  return workload.patch_index;  
}

/* Domain points a workload's DS invocations cover: both edges (and the complement), minus dropped points.
 * outer_factor_i is -1 for a single-line isoline workload. */
int
workload_point_count(object_data TessMeshWorkload &workload) {
  int count = workload.inner_factor_i + workload.outer_factor_i + 2;
  if (workload.has_complement)
    count += workload.inner_factor_c_i + workload.outer_factor_c_i + 2;
  return count - (workload.drop_last & 1) - ((workload.drop_last >> 1) & 1);
}

template <partitioning partition>
domain_location
get_domain_location_impl(int workload_index, int thread_index, object_data int *data) {
  simdgroup_barrier(mem_flags::mem_none);

  domain_location ret{float2(0), false, false};
  object_data TessMeshWorkload &workload = ((object_data TessMeshWorkload *)data)[workload_index];

  const int count = workload_point_count(workload);
  if (thread_index < count) {
    ret.active = true;
    // Skip the dropped last inner point: the outer edge starts right after it.
    const int dropped = (workload.drop_last & 1) && thread_index >= workload.inner_factor_i;
    ret.uv = (float2)get_domain_point<partition>(workload, thread_index + dropped);
  }

  simdgroup_barrier(mem_flags::mem_none);
  ret.iterate = simd_all(ret.active);

  return ret;
};

domain_location get_domain_location_integer(int workload_index, int thread_index, object_data int *data) asm(
    "dxmt.get_domain_location.integer"
);

domain_location
get_domain_location_integer(int workload_index, int thread_index, object_data int *data) {
  return get_domain_location_impl<partitioning::integer>(workload_index, thread_index, data);
};

domain_location get_domain_location_pow2(int workload_index, int thread_index, object_data int *data) asm(
    "dxmt.get_domain_location.pow2"
);

domain_location
get_domain_location_pow2(int workload_index, int thread_index, object_data int *data) {
  return get_domain_location_impl<partitioning::pow2>(workload_index, thread_index, data);
};

domain_location
get_domain_location_odd(int workload_index, int thread_index, object_data int *data) asm("dxmt.get_domain_location.odd"
);

domain_location
get_domain_location_odd(int workload_index, int thread_index, object_data int *data) {
  return get_domain_location_impl<partitioning::fractional_odd>(workload_index, thread_index, data);
};

domain_location get_domain_location_even(int workload_index, int thread_index, object_data int *data) asm(
    "dxmt.get_domain_location.even"
);

domain_location
get_domain_location_even(int workload_index, int thread_index, object_data int *data) {
  return get_domain_location_impl<partitioning::fractional_even>(workload_index, thread_index, data);
};

struct dummy_vertex {
  float4 pos [[position]];
};

using MeshTri = metal::mesh<dummy_vertex, void, 130, 128, topology::triangle>;
using MeshLine = metal::mesh<dummy_vertex, void, 130, 128, topology::line>;

/* Primitive index sinks: the mesh variants write the mesh index buffer, the capture variants
 * (TS->GS pass 1) write `indices[p * K + k]` into the capture scratch. Both share the generators
 * below, so order and winding are identical. */
void sink_set_index(MeshTri mesh, uint i, uint v) { mesh.set_index(i, v); }
void sink_set_index(MeshLine mesh, uint i, uint v) { mesh.set_index(i, v); }
void sink_set_index(device uint *indices, uint i, uint v) { indices[i] = v; }
void sink_finish(MeshTri mesh, uint count) { mesh.set_primitive_count(count); }
void sink_finish(MeshLine mesh, uint count) { mesh.set_primitive_count(count); }
void sink_finish(device uint *, uint) {}

template <typename Sink>
void
generate_triangle_for_edges(Sink sink, short base_inner, short base_outer, short inner, short outer, short provoke_offset, short ccw_xor) {
  for (short i = 0; i <= inner; i++) {
    half frac_left = half(i) / half(inner + 1);
    half frac_right = half(i + 1) / half(inner + 1);
    short left = frac_left <= 0.5 ? floor(frac_left * half(outer)) : ceil(frac_left * half(outer));
    short right = frac_right <= 0.5 ? floor(frac_right * half(outer)) : ceil(frac_right * half(outer));
    short count = right - left;
    if (count) {
      for (short j = 0; j < count; j++) {
        short primitive_index = base_outer + left + j - provoke_offset - 1;
        sink_set_index(sink, primitive_index * 3, base_outer + left + j);
        sink_set_index(sink, primitive_index * 3 + (1 ^ ccw_xor), base_outer + left + j + 1);
        sink_set_index(sink, primitive_index * 3 + (2 ^ ccw_xor), base_inner + i);
      }
    }
    if (i == inner) break;
    short primitive_index = base_inner + i - provoke_offset;
    sink_set_index(sink, primitive_index * 3, base_inner + i);
    sink_set_index(sink, primitive_index * 3 + (1 ^ ccw_xor), base_outer + right);
    sink_set_index(sink, primitive_index * 3 + (2 ^ ccw_xor), base_inner + i + 1);
  }
}

template <typename Sink>
int
generate_primitive_triangle_impl(int workload_index, object_data int *data, Sink sink, short ccw_xor) {
  object_data TessMeshWorkload &workload = ((object_data TessMeshWorkload *)data)[workload_index];

  short primitive_count = 0;
  {
    short max_in = workload.inner_factor_i;
    short max_out = workload.outer_factor_i;
    short base_outer = max_in + 1;

    primitive_count += max_in + max_out;
    generate_triangle_for_edges(sink, 0, base_outer, max_in, max_out, 0, ccw_xor);
  }

  if (workload.has_complement) {
    short max_in = workload.inner_factor_c_i;
    short max_out = workload.outer_factor_c_i;
    short base_inner = 2 + workload.inner_factor_i + workload.outer_factor_i;
    short base_outer = base_inner + max_in + 1;

    primitive_count += max_in + max_out;
    generate_triangle_for_edges(sink, base_inner, base_outer, max_in, max_out, 2, ccw_xor);
  }

  sink_finish(sink, primitive_count);
  return primitive_count;
}

/* Isoline connectivity: each line of detail + 1 points is detail segments (CHWTessellator::IsoLineGenerateConnectivity).
 * Workload points: line A at [0, inner_factor_i], line B at [inner_factor_i + 1, ...]. */
template <typename Sink>
int
generate_primitive_line_impl(int workload_index, object_data int *data, Sink sink) {
  object_data TessMeshWorkload &workload = ((object_data TessMeshWorkload *)data)[workload_index];
  short primitive_count = 0;
  const short lines = workload.outer_factor_i < 0 ? 1 : 2;
  for (short l = 0; l < lines; l++) {
    const short segments = l ? workload.outer_factor_i : workload.inner_factor_i;
    const short base = l ? workload.inner_factor_i + 1 : 0;
    for (short s = 0; s < segments; s++) {
      sink_set_index(sink, primitive_count * 2, base + s);
      sink_set_index(sink, primitive_count * 2 + 1, base + s + 1);
      primitive_count++;
    }
  }
  sink_finish(sink, primitive_count);
  return primitive_count;
}

template <typename Sink>
int
generate_primitive_point_impl(int workload_index, object_data int *data, Sink sink) {
  object_data TessMeshWorkload &workload = ((object_data TessMeshWorkload *)data)[workload_index];
  // Point workloads hold only unique points (gen_workload_*_points / isoline): one point per DS vertex.
  const short count = workload_point_count(workload);
  for (short i = 0; i < count; i++)
    sink_set_index(sink, i, i);
  sink_finish(sink, count);
  return count;
}

int generatePrimitiveTriangle(int workload_index, object_data int *data, MeshTri mesh) asm(
    "dxmt.domain_generate_primitives.triangle"
);
int
generatePrimitiveTriangle(int workload_index, object_data int *data, MeshTri mesh) {
  return generate_primitive_triangle_impl(workload_index, data, mesh, 0);
}

int generatePrimitiveTriangleCCW(int workload_index, object_data int *data, MeshTri mesh) asm(
    "dxmt.domain_generate_primitives.triangle_ccw"
);
int
generatePrimitiveTriangleCCW(int workload_index, object_data int *data, MeshTri mesh) {
  return generate_primitive_triangle_impl(workload_index, data, mesh, 3);
}

int generatePrimitiveLine(int workload_index, object_data int *data, MeshLine mesh) asm(
    "dxmt.domain_generate_primitives.line"
);
int
generatePrimitiveLine(int workload_index, object_data int *data, MeshLine mesh) {
  return generate_primitive_line_impl(workload_index, data, mesh);
}

int generatePrimitivePoint(int workload_index, object_data int *data, MeshTri mesh) asm(
    "dxmt.domain_generate_primitives.point"
);
int
generatePrimitivePoint(int workload_index, object_data int *data, MeshTri mesh) {
  return generate_primitive_point_impl(workload_index, data, mesh);
}

int generatePrimitiveTriangleCapture(int workload_index, object_data int *data, device uint *indices) asm(
    "dxmt.domain_generate_primitives.triangle.capture"
);
int
generatePrimitiveTriangleCapture(int workload_index, object_data int *data, device uint *indices) {
  return generate_primitive_triangle_impl(workload_index, data, indices, 0);
}

int generatePrimitiveTriangleCCWCapture(int workload_index, object_data int *data, device uint *indices) asm(
    "dxmt.domain_generate_primitives.triangle_ccw.capture"
);
int
generatePrimitiveTriangleCCWCapture(int workload_index, object_data int *data, device uint *indices) {
  return generate_primitive_triangle_impl(workload_index, data, indices, 3);
}

int generatePrimitiveLineCapture(int workload_index, object_data int *data, device uint *indices) asm(
    "dxmt.domain_generate_primitives.line.capture"
);
int
generatePrimitiveLineCapture(int workload_index, object_data int *data, device uint *indices) {
  return generate_primitive_line_impl(workload_index, data, indices);
}

int generatePrimitivePointCapture(int workload_index, object_data int *data, device uint *indices) asm(
    "dxmt.domain_generate_primitives.point.capture"
);
int
generatePrimitivePointCapture(int workload_index, object_data int *data, device uint *indices) {
  return generate_primitive_point_impl(workload_index, data, indices);
}

/* Rank of a workload among its patch's workloads (TS->GS capture slot). A patch's workloads are
 * emitted by one thread in order, so rank = #{ j < workload_index : same patch_index }. */
int get_domain_workload_rank(int workload_index, object_data int *data) asm("dxmt.get_domain_workload_rank");
int
get_domain_workload_rank(int workload_index, object_data int *data) {
  object_data TessMeshWorkload *workloads = (object_data TessMeshWorkload *)data;
  const short patch_index = workloads[workload_index].patch_index;
  int rank = 0;
  for (int j = 0; j < workload_index; j++)
    rank += workloads[j].patch_index == patch_index ? 1 : 0;
  return rank;
}
