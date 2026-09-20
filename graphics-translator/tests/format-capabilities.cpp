#include "d3d12_format_support.hpp"

#include <cassert>
#include <cstdint>

using namespace dxmt;

static bool
has1(const D3D12_FEATURE_DATA_FORMAT_SUPPORT &support, D3D12_FORMAT_SUPPORT1 bit) {
  return (static_cast<std::uint32_t>(support.Support1) & static_cast<std::uint32_t>(bit)) != 0;
}

static bool
has2(const D3D12_FEATURE_DATA_FORMAT_SUPPORT &support, D3D12_FORMAT_SUPPORT2 bit) {
  return (static_cast<std::uint32_t>(support.Support2) & static_cast<std::uint32_t>(bit)) != 0;
}

int
main() {
  {
    auto support = BuildD3D12FormatSupport(DXGI_FORMAT_UNKNOWN, {});
    assert(support.Support1 == D3D12_FORMAT_SUPPORT1_BUFFER);
    assert(support.Support2 == D3D12_FORMAT_SUPPORT2_NONE);
  }

  {
    D3D12FormatSupportProperties p{};
    auto support = BuildD3D12FormatSupport(DXGI_FORMAT_R8G8B8A8_UNORM, p);
    assert(support.Support1 == D3D12_FORMAT_SUPPORT1_NONE);
    assert(support.Support2 == D3D12_FORMAT_SUPPORT2_NONE);
  }

  {
    D3D12FormatSupportProperties p{
        .mapped = true,
        .buffer_compatible = true,
        .texture_compatible = true,
        .backbuffer = true,
        .vertex_attribute = true,
        .filter = true,
        .write = true,
        .color = true,
        .blend = true,
        .multisample = true,
        .resolve = true,
        .typed_uav_load = true,
        .typed_uav_store = true,
    };
    auto support = BuildD3D12FormatSupport(DXGI_FORMAT_R8G8B8A8_UNORM, p);
    assert(has1(support, D3D12_FORMAT_SUPPORT1_BUFFER));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_TEXTURE1D));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_TEXTURE2D));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_TEXTURE3D));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_TEXTURECUBE));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_SHADER_LOAD));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_SHADER_GATHER));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_MIP));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_RENDER_TARGET));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_BLENDABLE));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_DISPLAY));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW));
    assert(has2(support, D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD));
    assert(has2(support, D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_SO_BUFFER));
    assert(!has2(support, D3D12_FORMAT_SUPPORT2_TILED));
    assert(!has2(support, D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP));
  }

  {
    D3D12FormatSupportProperties p{
        .mapped = true,
        .buffer_compatible = true,
        .texture_compatible = true,
        .typeless = true,
        .filter = true,
        .write = true,
        .color = true,
        .blend = true,
        .multisample = true,
        .resolve = true,
        .typed_uav_load = true,
        .typed_uav_store = true,
    };
    auto support = BuildD3D12FormatSupport(DXGI_FORMAT_R8G8B8A8_TYPELESS, p);
    assert(has1(support, D3D12_FORMAT_SUPPORT1_BUFFER));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_TEXTURE2D));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_SHADER_LOAD));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_RENDER_TARGET));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW));
    assert(support.Support2 == D3D12_FORMAT_SUPPORT2_NONE);
  }

  {
    D3D12FormatSupportProperties p{
        .mapped = true,
        .texture_compatible = true,
        .block_compressed = true,
        .filter = true,
    };
    auto support = BuildD3D12FormatSupport(DXGI_FORMAT_BC1_UNORM, p);
    assert(has1(support, D3D12_FORMAT_SUPPORT1_TEXTURE2D));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_TEXTURECUBE));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_SHADER_LOAD));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_SHADER_GATHER));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_MIP));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_BUFFER));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_TEXTURE1D));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_TEXTURE3D));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_RENDER_TARGET));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW));
  }

  {
    D3D12FormatSupportProperties p{
        .mapped = true,
        .block_compressed = true,
        .filter = true,
    };
    auto support = BuildD3D12FormatSupport(DXGI_FORMAT_BC1_UNORM, p);
    assert(support.Support1 == D3D12_FORMAT_SUPPORT1_NONE);
    assert(support.Support2 == D3D12_FORMAT_SUPPORT2_NONE);
  }

  {
    D3D12FormatSupportProperties p{
        .mapped = true,
        .texture_compatible = true,
        .depth = true,
        .multisample = true,
        .resolve = true,
    };
    auto support = BuildD3D12FormatSupport(DXGI_FORMAT_D32_FLOAT, p);
    assert(has1(support, D3D12_FORMAT_SUPPORT1_TEXTURE2D));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_SHADER_LOAD));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_SHADER_GATHER));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_RENDER_TARGET));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW));
  }

  {
    D3D12FormatSupportProperties p{
        .mapped = true,
        .buffer_compatible = true,
        .texture_compatible = true,
        .vertex_attribute = true,
        .write = true,
        .color = true,
        .multisample = true,
        .atomic = true,
        .typed_uav_load = true,
        .typed_uav_store = true,
    };
    auto support = BuildD3D12FormatSupport(DXGI_FORMAT_R32_UINT, p);
    assert(has1(support, D3D12_FORMAT_SUPPORT1_IA_INDEX_BUFFER));
    assert(has1(support, D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW));
    assert(has2(support, D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD));
    assert(has2(support, D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE));
    assert(has2(support, D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD));
    assert(has2(support, D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS));
    assert(has2(support, D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE));
    assert(has2(support, D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE));
    assert(has2(support, D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX));
    assert(has2(support, D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE));
  }

  {
    D3D12FormatSupportProperties p{
        .mapped = true,
        .vertex_attribute = true,
    };
    auto support = BuildD3D12FormatSupport(DXGI_FORMAT_R32G32B32_FLOAT, p);
    assert(has1(support, D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_SO_BUFFER));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_BUFFER));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_TEXTURE1D));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_TEXTURE2D));
    assert(!has1(support, D3D12_FORMAT_SUPPORT1_TEXTURE3D));
  }

  return 0;
}
