#pragma once

#include "airconv_public.h"
#include <cstddef>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include <utility>

/*
 * D3D12 variable-rate (coarse) pixel shading on the captured MSAA pixel path (SM50_SHADER_D3D12_COARSE_SHADING).
 *
 * Coarse pixels are aligned to the 8x8 MSAA tile, which is also the VRS tile, so a primitive has one rate per
 * tile. Each fragment input of one primitive is affine in screen space (A = v * q with q = 1 / w for perspective
 * inputs, v otherwise). The coverage pass records the primitive's attribute planes once per tile in input record
 * kPlaneRecord, evaluated by native interpolate_at_offset at the tile origin and 8 pixels right and down (Metal
 * extrapolates linearly, probe build/local/p3/vrs/probe/probe.m). The pixel pass runs on a virtual coarse grid in
 * the tile's top-left corner: fragment (x, y) of the tile is coarse pixel (x, y), so native quads are coarse
 * quads and screen derivatives span coarse pixels. Every pull-model evaluation (center, centroid, sample,
 * snapped offset) evaluates the planes at its D3D location in the coarse pixel. The replay broadcasts the coarse
 * output record to each covered fine sample; depth, stencil and coverage stay per fine sample. Coarse coverage
 * masks use the D3D order: fine pixels left to right then top to bottom, each fine pixel's samples packed.
 */
namespace d3dalloy::msaa::coarse {

/* Shared by captured position, line/distance interpolation and coarse pull-model evaluation. */
inline llvm::Value *programmableSampleOffset(llvm::IRBuilder<> &b, llvm::Value *control, llvm::Value *sample,
                                            unsigned count, unsigned axis, llvm::Value *fallback) {
  if (count > 8)
    return fallback;
  auto *n = b.CreateLoad(b.getInt32Ty(), b.CreateConstGEP1_32(b.getInt32Ty(), control,
      offsetof(SM50_MSAA_TILE_CONTROL, programmable_sample_count) / 4));
  auto *index = b.CreateAdd(b.CreateMul(b.CreateAnd(sample, b.getInt32(7)), b.getInt32(2)),
      b.getInt32(offsetof(SM50_MSAA_TILE_CONTROL, programmable_sample_positions) / 4 + axis));
  auto *bits = b.CreateLoad(b.getInt32Ty(), b.CreateGEP(b.getInt32Ty(), control, index));
  return b.CreateSelect(b.CreateICmpEQ(n, b.getInt32(count)), b.CreateBitCast(bits, b.getFloatTy()), fallback);
}

/* Tile-control dword (SM50_MSAA_TILE_CONTROL.shading_rate) holding the draw's rate state. */
constexpr unsigned kRateDword = 14;
constexpr unsigned kRateBaseMask = 0xf;        /* RSSetShadingRate base D3D12_SHADING_RATE */
constexpr unsigned kRateCombiner0Shift = 4;    /* D3D12_SHADING_RATE_COMBINER of (base, per-primitive) */
constexpr unsigned kRateCombiner1Shift = 8;    /* D3D12_SHADING_RATE_COMBINER of (first result, image) */
constexpr unsigned kRateAdditional = 1u << 12; /* AdditionalShadingRatesSupported: 2x4, 4x2 and 4x4 */

/* Input records 0-63 are fine pixels and 64-127 helper records; the planes occupy one more record. */
constexpr unsigned kPlaneRecord = 128;
constexpr unsigned kRecordCount = 129;
/* Dword of record kPlaneRecord holding the tile's resolved D3D12_SHADING_RATE. */
constexpr unsigned kRateRecordDword = 0;

/* Sample counts with a coarse rate other than 1x1 (D3D12 support table). */
constexpr bool supported(unsigned sampleCount) { return sampleCount == 1 || sampleCount == 2 || sampleCount == 4; }

struct PlaneInput {
  unsigned argument; /* original fragment argument: a native pull interpolant */
  bool perspective;
  unsigned offset = ~0u; /* {A(origin), dA/dx, dA/dy}: three float4 */
};

struct Planes {
  unsigned rate = ~0u;  /* the tile's resolved D3D12_SHADING_RATE: dword kRateRecordDword */
  unsigned claim = ~0u; /* nonzero once a fragment of the primitive stored the record */
  unsigned q = ~0u;     /* {q(origin), dq/dx, dq/dy}: q = 1 / clip w */
  unsigned z = ~0u;     /* {z(origin), dz/dx, dz/dy}: window depth including the gathered depth bias */
  llvm::SmallVector<PlaneInput, 8> inputs;
};

/* Allocates the plane words from record dword `words` on; returns the new record size. */
unsigned allocate(Planes &planes, unsigned words);

/* The tile's D3D12_SHADING_RATE: base, per-primitive and image rates through the two combiners, each combiner
 * input and output sanitized to a rate supported at `sampleCount`. */
llvm::Value *resolveRate(llvm::IRBuilder<> &b, llvm::Value *control, unsigned sampleCount,
                         llvm::Value *primitive, llvm::Value *image);

/* Coverage variant: the first real fragment of the primitive in the tile stores the rate, the planes and the
 * {dword, value} per-primitive inputs into record kPlaneRecord. `interpolants` parallels planes.inputs; null
 * slots use `capturedInput` at window-space XY to reconstruct spilled vertex fields. Native inputs retain
 * their interpolation functions. The reciprocal-W plane comes from interpolated clip positions. The depth plane is the gather
 * pass's center window depth of input record `pixel` (dword `centerZ`, including depth bias) with the slopes of
 * native position.z. */
void capture(llvm::IRBuilder<> &b, llvm::Module &module, llvm::Function &entry, llvm::Value *scratch,
             unsigned stride, llvm::Value *position, llvm::Value *control, llvm::Value *pixel, unsigned centerZ,
             const Planes &planes, llvm::ArrayRef<llvm::Value *> interpolants,
             llvm::Value *clipPosition, llvm::Value *rate,
             llvm::ArrayRef<std::pair<unsigned, llvm::Value *>> constants,
             llvm::function_ref<llvm::Value *(unsigned, llvm::Value *)> capturedInput);

struct Grid {
  llvm::Value *log2x, *log2y; /* log2 coarse pixel size */
  llvm::Value *valid;         /* the fragment is a coarse pixel of the tile */
  llvm::Value *fineX, *fineY; /* tile-local coordinates of the coarse pixel's top-left fine pixel */
  llvm::Value *center;        /* float2 coarse pixel center in render-target pixels */
};

/* Pixel variant: the coarse pixel of this fragment of the virtual grid at D3D12_SHADING_RATE `rate`. */
Grid grid(llvm::IRBuilder<> &b, llvm::Value *position, llvm::Value *control, llvm::Value *rate);

/* Coverage of the coarse pixel in coarse order: `geometric` is the pre-test primitive coverage of the input records,
 * `post` the post-test sample mask `fineMask` returns at a float4 fine-pixel position for each fine pixel the
 * primitive covers (uncovered fine pixels, possibly outside the render target, are never read). */
struct Coverage {
  llvm::Value *geometric, *post;
};
Coverage coverage(llvm::IRBuilder<> &b, const Grid &grid, llvm::Value *control, llvm::Value *scratch,
                  unsigned stride, unsigned sampleCount, llvm::function_ref<llvm::Value *(llvm::Value *)> fineMask);

/* Typed load from record kPlaneRecord. */
llvm::Value *load(llvm::IRBuilder<> &b, llvm::Value *scratch, unsigned stride, unsigned offset, llvm::Type *type);

/* SV_Position of the coarse pixel: center xy, the z plane and q (the signature prologue reciprocates w). */
llvm::Value *position(llvm::IRBuilder<> &b, llvm::Value *scratch, unsigned stride, const Planes &planes,
                      llvm::Value *control, llvm::Value *center);

/* Trailing body arguments rewritePulls consumes: input records, tile control, the float2 coarse center,
 * <4 x i32> {fine x, fine y, log2 width, log2 height} and the i32 geometric coverage. */
constexpr unsigned kBodyArguments = 5;
llvm::SmallVector<llvm::Type *, kBodyArguments> bodyTypes(llvm::Type *scratch, llvm::Type *control);
llvm::SmallVector<llvm::Value *, kBodyArguments> bodyArguments(llvm::IRBuilder<> &b, llvm::Value *scratch,
    llvm::Value *control, const Grid &grid, llvm::Value *geometric);

/* Replaces each pull-model call on a plane input of `body` (original arguments, then the kBodyArguments trailing
 * arguments from `extra` on) by the planes evaluated at its D3D location in the coarse pixel. */
llvm::Error rewritePulls(llvm::Function &body, unsigned extra, const Planes &planes, unsigned stride,
                         unsigned sampleCount, bool centerPattern, bool conservative);

struct Replay {
  llvm::Value *record; /* output record of the fine pixel's coarse pixel */
  llvm::Value *shift;  /* coarse coverage bit of the fine pixel's sample 0 */
};

/* Replay variant: the coarse output record and coverage bit of this fine pixel. */
Replay replay(llvm::IRBuilder<> &b, llvm::Value *position, llvm::Value *control, llvm::Value *rate,
              unsigned sampleCount);

} // namespace d3dalloy::msaa::coarse
