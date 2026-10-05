#pragma once

#include <cstdint>

namespace dxmt::dxil {

// DXIL opcode numbers are a stable bytecode ABI, independent of the SDK headers.
// Explicit levels/gradients do not require an implicit derivative or quad remap.
constexpr bool
RequiresQuadDerivatives(uint32_t opcode) {
  switch (opcode) {
  case 60:  // Sample
  case 61:  // SampleBias
  case 64:  // SampleCmp
  case 81:  // CalculateLOD
  case 83:  // DerivCoarseX
  case 84:  // DerivCoarseY
  case 85:  // DerivFineX
  case 86:  // DerivFineY
  case 174: // WriteSamplerFeedback
  case 175: // WriteSamplerFeedbackBias
  case 255: // SampleCmpBias (SM 6.8)
    return true;
  default:
    return false;
  }
}

constexpr bool
UsesQuadOperations(uint32_t opcode) {
  return RequiresQuadDerivatives(opcode) || opcode == 122 || opcode == 123 || opcode == 222;
}

enum class QuadLayout { Unavailable, Linear, Tiled2D };

// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_Derivatives.html
// Group size limits are validated separately; this classifies only the quad shape.
constexpr QuadLayout
ClassifyQuadLayout(uint32_t x, uint32_t y, uint32_t z) {
  if (!x || !y || !z)
    return QuadLayout::Unavailable;
  if (x % 4 == 0 && y == 1 && z == 1)
    return QuadLayout::Linear;
  if (x % 2 == 0 && y % 2 == 0)
    return QuadLayout::Tiled2D;
  return QuadLayout::Unavailable;
}

} // namespace dxmt::dxil
