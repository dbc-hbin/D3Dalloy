#pragma once

#include "d3d12.h"

namespace dxmt {

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
  const bool depth_stencil = properties.depth || properties.stencil;

  if (properties.buffer_compatible)
    add_support1(D3D12_FORMAT_SUPPORT1_BUFFER);

  if (typed && properties.vertex_attribute)
    add_support1(D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER);

  if (typed && (format == DXGI_FORMAT_R16_UINT || format == DXGI_FORMAT_R32_UINT))
    add_support1(D3D12_FORMAT_SUPPORT1_IA_INDEX_BUFFER);

  if (properties.texture_compatible) {
    if (!properties.block_compressed && !depth_stencil)
      add_support1(D3D12_FORMAT_SUPPORT1_TEXTURE1D);

    add_support1(D3D12_FORMAT_SUPPORT1_TEXTURE2D);

    if (!properties.block_compressed && !depth_stencil)
      add_support1(D3D12_FORMAT_SUPPORT1_TEXTURE3D);

    if (!depth_stencil)
      add_support1(D3D12_FORMAT_SUPPORT1_TEXTURECUBE);

    if (properties.typeless)
      add_support1(D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT);

    if (typed) {
      add_support1(D3D12_FORMAT_SUPPORT1_SHADER_LOAD);

      if (properties.filter) {
        add_support1(D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE);
        add_support1(D3D12_FORMAT_SUPPORT1_SHADER_GATHER);
        add_support1(D3D12_FORMAT_SUPPORT1_MIP);
      }

      if (properties.depth) {
        add_support1(D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON);
        add_support1(D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON);
      }

      if (properties.color)
        add_support1(D3D12_FORMAT_SUPPORT1_RENDER_TARGET);

      if (properties.blend)
        add_support1(D3D12_FORMAT_SUPPORT1_BLENDABLE);

      if (depth_stencil)
        add_support1(D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL);

      if (properties.resolve && properties.color)
        add_support1(D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE);

      if (properties.backbuffer && properties.color)
        add_support1(D3D12_FORMAT_SUPPORT1_DISPLAY);

      if (properties.multisample) {
        add_support1(D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD);
        if (properties.color)
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
