#pragma once

#include "d3d12.h"
#include "d3d12_resource_contract.hpp"

namespace dxmt {

// The DXGI names for depth/stencil plane SRVs contain TYPELESS even though
// each selects a concrete shader-readable aspect of a two-plane resource.
inline bool IsD3D12DepthStencilPlaneView(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
  case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
  case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
  case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    return true;
  default:
    return false;
  }
}

inline bool IsD3D12DepthStencilStorageFormat(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_D16_UNORM || format == DXGI_FORMAT_D32_FLOAT ||
         format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

inline bool IsD3D12ShaderDepthViewFormat(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R16_UNORM || format == DXGI_FORMAT_R32_FLOAT ||
         format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
         format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
}

inline bool IsD3D12ShaderViewFormat(DXGI_FORMAT format, bool typeless) {
  if (IsD3D12DepthStencilPlaneView(format))
    return true;
  if (typeless)
    return false;
  return !IsD3D12DepthStencilStorageFormat(format);
}

inline bool IsD3D12UintColorFormat(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R32G32B32A32_UINT:
  case DXGI_FORMAT_R32G32B32_UINT:
  case DXGI_FORMAT_R16G16B16A16_UINT:
  case DXGI_FORMAT_R32G32_UINT:
  case DXGI_FORMAT_R10G10B10A2_UINT:
  case DXGI_FORMAT_R8G8B8A8_UINT:
  case DXGI_FORMAT_R16G16_UINT:
  case DXGI_FORMAT_R32_UINT:
  case DXGI_FORMAT_R8G8_UINT:
  case DXGI_FORMAT_R16_UINT:
  case DXGI_FORMAT_R8_UINT:
    return true;
  default:
    return false;
  }
}

struct D3D12FormatSupportProperties {
  bool mapped = false;
  bool buffer_compatible = false;
  bool texture_compatible = false;
  bool typeless = false;
  bool block_compressed = false;
  bool depth = false;
  bool stencil = false;
  bool backbuffer = false;
  bool vertex_attribute = false;
  bool filter = false;
  bool write = false;
  bool color = false;
  bool blend = false;
  bool multisample = false;
  bool resolve = false;
  bool atomic = false;
  bool typed_uav_load = false;
  bool typed_uav_store = false;
};

inline D3D12_FEATURE_DATA_FORMAT_SUPPORT
BuildD3D12FormatSupport(DXGI_FORMAT format, const D3D12FormatSupportProperties &properties) {
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
  support.Format = format;
  support.Support1 = D3D12_FORMAT_SUPPORT1_NONE;
  support.Support2 = D3D12_FORMAT_SUPPORT2_NONE;

  if (format == DXGI_FORMAT_UNKNOWN) {
    support.Support1 = D3D12_FORMAT_SUPPORT1_BUFFER;
    return support;
  }

  if (!properties.mapped)
    return support;

  auto add_support1 = [&](D3D12_FORMAT_SUPPORT1 bit) {
    support.Support1 = static_cast<D3D12_FORMAT_SUPPORT1>(
        static_cast<unsigned int>(support.Support1) | static_cast<unsigned int>(bit));
  };
  auto add_support2 = [&](D3D12_FORMAT_SUPPORT2 bit) {
    support.Support2 = static_cast<D3D12_FORMAT_SUPPORT2>(
        static_cast<unsigned int>(support.Support2) | static_cast<unsigned int>(bit));
  };

  const bool typed = !properties.typeless;
  const bool shader_view = IsD3D12ShaderViewFormat(format, properties.typeless);
  const bool depth_stencil = properties.depth || properties.stencil;

  if (properties.buffer_compatible)
    add_support1(D3D12_FORMAT_SUPPORT1_BUFFER);

  if (typed && properties.vertex_attribute)
    add_support1(D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER);

  if (typed && (format == DXGI_FORMAT_R16_UINT || format == DXGI_FORMAT_R32_UINT))
    add_support1(D3D12_FORMAT_SUPPORT1_IA_INDEX_BUFFER);

  // SO writes unconverted 32-bit components. It does not use a typed Metal
  // buffer texture, so three-component formats are supported here as well.
  switch (format) {
  case DXGI_FORMAT_R32_FLOAT:
  case DXGI_FORMAT_R32_UINT:
  case DXGI_FORMAT_R32_SINT:
  case DXGI_FORMAT_R32G32_FLOAT:
  case DXGI_FORMAT_R32G32_UINT:
  case DXGI_FORMAT_R32G32_SINT:
  case DXGI_FORMAT_R32G32B32_FLOAT:
  case DXGI_FORMAT_R32G32B32_UINT:
  case DXGI_FORMAT_R32G32B32_SINT:
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
  case DXGI_FORMAT_R32G32B32A32_UINT:
  case DXGI_FORMAT_R32G32B32A32_SINT:
    add_support1(D3D12_FORMAT_SUPPORT1_SO_BUFFER);
    break;
  default:
    break;
  }

  if (properties.texture_compatible) {
    if (!properties.block_compressed)
      add_support1(D3D12_FORMAT_SUPPORT1_TEXTURE1D);

    add_support1(D3D12_FORMAT_SUPPORT1_TEXTURE2D);

    if (!depth_stencil)
      add_support1(D3D12_FORMAT_SUPPORT1_TEXTURE3D);

    add_support1(D3D12_FORMAT_SUPPORT1_TEXTURECUBE);

    // Fully typed resources can also have another compatible view format.
    // Formats outside a DXGI bit-layout family have no other interpretation.
    if (resource_contract::GetFormatFamily(format) != resource_contract::FormatFamily::Unknown)
      add_support1(D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT);

    // Mip storage and per-level Load do not depend on filtering, or on whether
    // the allocation itself has a typed format. Its SRV may supply the type.
    add_support1(D3D12_FORMAT_SUPPORT1_MIP);

    if (shader_view)
      add_support1(D3D12_FORMAT_SUPPORT1_SHADER_LOAD);

    if (shader_view && properties.filter) {
      add_support1(D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE);
      add_support1(D3D12_FORMAT_SUPPORT1_SHADER_GATHER);
    }

    if (shader_view && IsD3D12ShaderDepthViewFormat(format)) {
      add_support1(D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON);
      add_support1(D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON);
    }

    if (shader_view && properties.multisample)
      add_support1(D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD);

    if (typed) {
      if (properties.color)
        add_support1(D3D12_FORMAT_SUPPORT1_RENDER_TARGET);

      // FS 17.7.1: logic ops are defined for (and must cover) every renderable UINT format.
      if (properties.color && IsD3D12UintColorFormat(format))
        add_support2(D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP);

      if (properties.blend)
        add_support1(D3D12_FORMAT_SUPPORT1_BLENDABLE);

      if (depth_stencil && IsD3D12DepthStencilStorageFormat(format))
        add_support1(D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL);

      if (properties.resolve && properties.color)
        add_support1(D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE);

      if (properties.backbuffer && properties.color)
        add_support1(D3D12_FORMAT_SUPPORT1_DISPLAY);

      if (properties.multisample) {
        if (properties.color || IsD3D12DepthStencilStorageFormat(format))
          add_support1(D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET);
      }

      const bool typed_uav =
          !properties.block_compressed && !depth_stencil &&
          ((properties.texture_compatible && properties.write) ||
           (properties.buffer_compatible && properties.typed_uav_store));
      if (typed_uav) {
        add_support1(D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW);

        if (properties.typed_uav_load)
          add_support2(D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD);
        if (properties.typed_uav_store)
          add_support2(D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);

        if (properties.atomic) {
          add_support2(D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD);
          add_support2(D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS);
          add_support2(D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE);
          add_support2(D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE);
          add_support2(D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX);
          add_support2(D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX);
        }
      }
    }
  }

  return support;
}

} // namespace dxmt
