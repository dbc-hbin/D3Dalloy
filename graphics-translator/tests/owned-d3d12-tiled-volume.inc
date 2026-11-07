// Included inside the tiled fixture's anonymous namespace. Expected pixels are computed
// from independent CPU tile bytes and logical 3D address/filter math, never from GPU output.
struct VolumeColor {
  double lane[4] = {};
  double lower[4] = {}, upper[4] = {};
  bool bounded = false;
  bool mapped = true;
};
VolumeColor volumeDecodeBC1(UINT endpoints, UINT selectors, UINT pixelIndex, bool srgb) {
  VolumeColor result; result.bounded = true;
  const UINT a = endpoints & 0xffff, b = endpoints >> 16, selector = (selectors >> (2 * pixelIndex)) & 3;
  const UINT raw[2][3] = {{a >> 11, (a >> 5) & 63, a & 31}, {b >> 11, (b >> 5) & 63, b & 31}};
  result.lane[3] = result.lower[3] = result.upper[3] = a <= b && selector == 3 ? 0 : 1;
  // D3D11.3 functional spec §§19.5.2–19.5.6: MSB extension, integer reference
  // division, and per-channel error < 1/255 + .03*max(original,promoted span).
  // Reference 0/1 must be exact. sRGB bounds are linearized BEFORE filtering.
  for (UINT channel = 0; channel < 3; ++channel) {
    const UINT bits = channel == 1 ? 6 : 5, scale = (1u << bits) - 1;
    const UINT c0 = (raw[0][channel] << (8 - bits)) | (raw[0][channel] >> (2 * bits - 8));
    const UINT c1 = (raw[1][channel] << (8 - bits)) | (raw[1][channel] >> (2 * bits - 8));
    const UINT value8 = selector < 2 ? (selector ? c1 : c0)
        : a > b ? ((selector == 2 ? 2 : 1) * c0 + (selector == 2 ? 1 : 2) * c1) / 3
        : selector == 2 ? (c0 + c1) / 2 : 0;
    const double value = value8 / 255.0;
    const double span = std::fmax(std::fabs(double(raw[0][channel]) - raw[1][channel]) / scale,
                                std::fabs(double(c0) - c1) / 255.0);
    // §19.5.6 allows 1 UNORM8 ULP on input to sRGB linearization;
    // §3.2.3.7 adds .5 UNORM8 ULP conversion error, measured on the sRGB side.
    const double error = value8 == 0 || value8 == 255 ? 0 : 1.0 / 255 + .03 * span + (srgb ? 1.5 / 255 : 0);
    const double low = std::fmax(0, value - error), high = std::fmin(1, value + error);
    result.lane[channel] = srgb ? (value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4)) : value;
    result.lower[channel] = srgb ? (low <= .04045 ? low / 12.92 : std::pow((low + .055) / 1.055, 2.4)) : low;
    result.upper[channel] = srgb ? (high <= .04045 ? high / 12.92 : std::pow((high + .055) / 1.055, 2.4)) : high;
  }
  return result;
}
struct VolumeSampler {
  UINT address = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  bool minLinear = true, magLinear = true, mipLinear = true;
  UINT reduction = 0, anisotropy = 1;
  double bias = 0, minimum = 0, maximum = 16;
  double border[4] = {0.125, 0.25, 0.5, 0.75};
};
struct VolumeCase {
  float words[12] = {};
  UINT integers[4] = {};
  VolumeSampler sampler;
  UINT firstMip = 0, mipCount = 4;
  float resourceMin = 0;
  bool srgb = false, signedDecode = false, nullView = false;
};
struct VolumeResource {
  ComPtr<ID3D12Resource> texture, upload;
  ComPtr<ID3D12Heap> heap;
  D3D12_TILE_SHAPE shape{};
  D3D12_SUBRESOURCE_TILING tiling[4]{};
  UINT tileCount = 0, mipCount = 0;
  UINT width = 64, height = 64, depth = 32, block = 1, unitBytes = 4;
  DXGI_FORMAT format;
  std::vector<BYTE> bytes;
  std::vector<int> mapping;

  VolumeResource() = default; // CPU-only oracle regression; no device or GPU resources.
  VolumeResource(Context &c, DXGI_FORMAT format, UINT mips, D3D12_RESOURCE_FLAGS flags,
                 UINT w = 64, UINT h = 64, UINT d = 32) : mipCount(mips), width(w), height(h), depth(d), format(format) {
    if (format == DXGI_FORMAT_BC1_UNORM || format == DXGI_FORMAT_BC4_UNORM) { block = 4; unitBytes = 8; }
    if (format == DXGI_FORMAT_BC5_UNORM || format == DXGI_FORMAT_BC6H_UF16) { block = 4; unitBytes = 16; }
    auto desc = textureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE3D, width, height, UINT16(depth), UINT16(mips), format,
                            D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE);
    desc.Flags = flags;
    if (block == 4) {
      ComPtr<ID3D12Device10> device10; check(c.device.As(&device10), "volume BC cast device10");
      DXGI_FORMAT casts[] = {format, format == DXGI_FORMAT_BC1_UNORM ? DXGI_FORMAT_BC1_UNORM_SRGB
          : format == DXGI_FORMAT_BC4_UNORM ? DXGI_FORMAT_BC4_SNORM
          : format == DXGI_FORMAT_BC5_UNORM ? DXGI_FORMAT_BC5_SNORM : DXGI_FORMAT_BC6H_SF16,
          unitBytes == 8 ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_R16G16B16A16_UINT};
      const UINT castCount = flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS ? (unitBytes == 8 ? 4 : 3) : 2;
      check(device10->CreateReservedResource2(&desc, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr,
          castCount, casts, IID_PPV_ARGS(&texture)), "volume BC/plain reserved cast list");
    } else {
      check(c.device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&texture)), "volume reserved create");
    }
    D3D12_PACKED_MIP_INFO packed{};
    UINT count = mips;
    c.device->GetResourceTiling(texture.Get(), &tileCount, &packed, &shape, &count, 0, tiling);
    require(count == mips && packed.NumPackedMips == 0 && tileCount > 0, "volume standard tiling");
    require(UINT64(shape.WidthInTexels / block) * (shape.HeightInTexels / block) * shape.DepthInTexels * unitBytes == kTile,
            "volume official 64KB tile shape");
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = UINT64(tileCount) * kTile;
    hd.Properties = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    check(c.device->CreateHeap(&hd, IID_PPV_ARGS(&heap)), "volume physical heap");
    D3D12_TILED_RESOURCE_COORDINATE origin{};
    D3D12_TILE_REGION_SIZE region{tileCount, FALSE, 0, 0, 0};
    D3D12_TILE_RANGE_FLAGS range = D3D12_TILE_RANGE_FLAG_NONE;
    UINT offset = 0;
    c.queue->UpdateTileMappings(texture.Get(), 1, &origin, &region, heap.Get(), 1, &range, &offset, &tileCount,
                                D3D12_TILE_MAPPING_FLAG_NONE);
    mapping.resize(tileCount);
    bytes.resize(size_t(tileCount) * kTile);
    for (UINT tile = 0; tile < tileCount; ++tile) {
      mapping[tile] = int(tile);
      for (UINT z = 0; z < shape.DepthInTexels; ++z)
        for (UINT y = 0; y < shape.HeightInTexels; y += block)
          for (UINT x = 0; x < shape.WidthInTexels; x += block) {
            const UINT packedColor = ((tile * 17 + x * 3) & 255) | (((tile * 29 + y * 5) & 255) << 8) |
                                     (((tile * 11 + z * 7) & 255) << 16) | 0xff000000u;
            const UINT value = format == DXGI_FORMAT_R32_UINT ? 0xe1000000u + tile * 0x10000 + x + 64 * y + 4096 * z
                                                             : packedColor;
            const size_t address = size_t(tile) * kTile + unitBytes * (x / block + (shape.WidthInTexels / block) *
                (y / block + (shape.HeightInTexels / block) * z));
            if (format == DXGI_FORMAT_BC4_UNORM || format == DXGI_FORMAT_BC5_UNORM) {
              const UINT indices[] = {0, 1, 6, 7};
              for (UINT channel = 0; channel < unitBytes / 8; ++channel) {
                UINT64 encoded = (0x80u + channel) | (UINT64(0xc0 + (tile + x / 4 + y / 4 + z + channel * 7) % 48) << 8);
                for (UINT py = 0; py < 4; ++py) for (UINT px = 0; px < 4; ++px)
                  encoded |= UINT64(indices[(tile + x / 4 + y / 4 + z + px + 2 * py + channel) & 3]) << (16 + 3 * (px + 4 * py));
                std::memcpy(bytes.data() + address + channel * 8, &encoded, 8);
              }
            } else if (format == DXGI_FORMAT_BC6H_UF16) {
              // Mode 11, explicit equal RGB endpoints, all indices zero: varying nonzero signed/unsigned controls.
              bytes[address] = 3;
              for (UINT field = 0; field < 6; ++field) {
                const UINT endpoint = 770 + (field % 3) * 15 + (tile + x / 4 + 3 * y / 4 + z) % 24;
                for (UINT bit = 0; bit < 10; ++bit) {
                  const UINT destination = 5 + field * 10 + bit;
                  bytes[address + destination / 8] |= BYTE(((endpoint >> bit) & 1) << (destination % 8));
                }
              }
            } else if (block == 4) {
              const UINT endpoints = (((tile + x / block + z) & 1) ? 0x001fu : 0xf800u) | (0x07e0u << 16);
              UINT selectors = 0;
              for (UINT py = 0; py < 4; ++py) for (UINT px = 0; px < 4; ++px)
                selectors |= ((tile + x / block + 3 * y / block + 7 * z + px + 2 * py) & 3) << (2 * (px + 4 * py));
              std::memcpy(bytes.data() + address, &endpoints, 4); std::memcpy(bytes.data() + address + 4, &selectors, 4);
            } else std::memcpy(bytes.data() + address, &value, 4);
          }
    }
    upload = c.buffer(D3D12_HEAP_TYPE_UPLOAD, bytes.size(), "volume tile upload");
    void *mapped = nullptr;
    check(upload->Map(0, nullptr, &mapped), "volume upload map");
    std::memcpy(mapped, bytes.data(), bytes.size());
    upload->Unmap(0, nullptr);
    c.reset();
    if (block == 4)
      transition(c.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT mip = 0; mip < mipCount; ++mip)
      for (UINT z = 0; z < tiling[mip].DepthInTiles; ++z)
        for (UINT y = 0; y < tiling[mip].HeightInTiles; ++y)
          for (UINT x = 0; x < tiling[mip].WidthInTiles; ++x) {
            D3D12_TILED_RESOURCE_COORDINATE p{x, y, z, mip};
            D3D12_TILE_REGION_SIZE one{1, TRUE, 1, 1, 1};
            const UINT tile = tiling[mip].StartTileIndexInOverallResource +
                              x + tiling[mip].WidthInTiles * (y + tiling[mip].HeightInTiles * z);
            c.list->CopyTiles(texture.Get(), &p, &one, upload.Get(), UINT64(tile) * kTile,
                               D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
          }
    transition(c.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    c.execute("volume tile upload close");
    c.finish("volume tile upload");
  }

  UINT tileAt(UINT mip, UINT x, UINT y, UINT z) const {
    const auto &t = tiling[mip];
    return t.StartTileIndexInOverallResource + x / shape.WidthInTexels +
           t.WidthInTiles * (y / shape.HeightInTexels + t.HeightInTiles * (z / shape.DepthInTexels));
  }
  size_t address(UINT mip, UINT x, UINT y, UINT z) const {
    const int physical = mapping[tileAt(mip, x, y, z)];
    require(physical >= 0, "CPU mapped volume address");
    return size_t(physical) * kTile + unitBytes * ((x % shape.WidthInTexels) / block +
        (shape.WidthInTexels / block) * ((y % shape.HeightInTexels) / block + (shape.HeightInTexels / block) * (z % shape.DepthInTexels)));
  }
  VolumeColor texel(UINT mip, int x, int y, int z, bool srgb = false, bool signedDecode = false) const {
    VolumeColor result;
    result.bounded = format == DXGI_FORMAT_BC1_UNORM;
    if (format == DXGI_FORMAT_BC4_UNORM || format == DXGI_FORMAT_BC5_UNORM || format == DXGI_FORMAT_BC6H_UF16) result.lane[3] = 1;
    if (mip >= mipCount || x < 0 || y < 0 || z < 0 || x >= int(width >> mip) || y >= int(height >> mip) || z >= int(depth >> mip))
      return result;
    if (mapping[tileAt(mip, UINT(x), UINT(y), UINT(z))] < 0) {
      result.mapped = false;
      return result;
    }
    const auto *pixel = bytes.data() + address(mip, UINT(x), UINT(y), UINT(z));
    if (format == DXGI_FORMAT_BC4_UNORM || format == DXGI_FORMAT_BC5_UNORM) {
      for (UINT channel = 0; channel < unitBytes / 8; ++channel) {
        UINT64 encoded; std::memcpy(&encoded, pixel + channel * 8, 8);
        int a = int(encoded & 255), b = int((encoded >> 8) & 255);
        if (signedDecode) { if (a >= 128) a -= 256; if (b >= 128) b -= 256; a = int(std::fmax(-127, a)); b = int(std::fmax(-127, b)); }
        const UINT selector = UINT((encoded >> (16 + 3 * ((x & 3) + 4 * (y & 3)))) & 7);
        const double scale = signedDecode ? 127.0 : 255.0;
        result.lane[channel] = selector == 0 ? a / scale : selector == 1 ? b / scale
            : a > b ? ((8 - selector) * double(a) + (selector - 1) * double(b)) / (7 * scale)
            : selector < 6 ? ((6 - selector) * double(a) + (selector - 1) * double(b)) / (5 * scale)
            : selector == 6 ? (signedDecode ? -1 : 0) : 1;
      }
      return result;
    }
    if (format == DXGI_FORMAT_BC6H_UF16) {
      // Independent mode-11 constant-block equations from Microsoft's BC6H format specification:
      // https://learn.microsoft.com/en-us/windows/win32/direct3d11/bc6h-format
      require((pixel[0] & 31) == 3, "CPU BC6 mode11 fixture");
      for (UINT channel = 0; channel < 3; ++channel) {
        UINT raw = 0;
        for (UINT bit = 0; bit < 10; ++bit) { const UINT source = 5 + channel * 10 + bit; raw |= UINT((pixel[source / 8] >> (source % 8)) & 1) << bit; }
        const int component = signedDecode && raw >= 512 ? int(raw) - 1024 : int(raw);
        const UINT magnitude = UINT(component < 0 ? -component : component);
        const UINT unquantized = !magnitude ? 0 : signedDecode
            ? magnitude >= 511 ? 32767 : ((magnitude << 15) + 16384) >> 9
            : magnitude == 1023 ? 65535 : ((magnitude << 16) + 32768) >> 10;
        const UINT halfBits = (unquantized * 31) >> (signedDecode ? 5 : 6);
        const UINT exponent = (halfBits >> 10) & 31, mantissa = halfBits & 1023;
        const double value = exponent ? std::ldexp(1024.0 + mantissa, int(exponent) - 25) : std::ldexp(double(mantissa), -24);
        result.lane[channel] = component < 0 ? -value : value;
      }
      return result;
    }
    if (block == 4) {
      UINT endpoints, selectors; std::memcpy(&endpoints, pixel, 4); std::memcpy(&selectors, pixel + 4, 4);
      return volumeDecodeBC1(endpoints, selectors, UINT((x & 3) + 4 * (y & 3)), srgb);
    }
    for (UINT channel = 0; channel < 4; ++channel) {
      const double value = pixel[channel] / 255.0;
      result.lane[channel] = srgb && channel < 3 ? (value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4)) : value;
    }
    return result;
  }
  void remap(Context &c, UINT x, UINT y, UINT z, int physical) {
    D3D12_TILED_RESOURCE_COORDINATE p{x, y, z, 0};
    D3D12_TILE_REGION_SIZE one{1, TRUE, 1, 1, 1};
    D3D12_TILE_RANGE_FLAGS flag = physical < 0 ? D3D12_TILE_RANGE_FLAG_NULL : D3D12_TILE_RANGE_FLAG_NONE;
    UINT start = physical < 0 ? 0 : UINT(physical), count = 1;
    c.queue->UpdateTileMappings(texture.Get(), 1, &p, &one, heap.Get(), 1, &flag, &start, &count,
                                D3D12_TILE_MAPPING_FLAG_NONE);
    mapping[tileAt(0, x * shape.WidthInTexels, y * shape.HeightInTexels, z * shape.DepthInTexels)] = physical;
  }
};

void volumeCheckPhysicalBytes(Context &c, const VolumeResource &volume, const char *marker) {
  const auto tileReadback = c.buffer(D3D12_HEAP_TYPE_READBACK, volume.bytes.size(), "volume tiles readback");
  const auto shaderState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  c.reset(); transition(c.list.Get(), volume.texture.Get(), shaderState, D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (UINT mip = 0; mip < volume.mipCount; ++mip)
    for (UINT z = 0; z < volume.tiling[mip].DepthInTiles; ++z)
      for (UINT y = 0; y < volume.tiling[mip].HeightInTiles; ++y)
        for (UINT x = 0; x < volume.tiling[mip].WidthInTiles; ++x) {
          D3D12_TILED_RESOURCE_COORDINATE p{x, y, z, mip}; D3D12_TILE_REGION_SIZE one{1, TRUE, 1, 1, 1};
          const UINT tile = volume.tileAt(mip, x * volume.shape.WidthInTexels, y * volume.shape.HeightInTexels, z * volume.shape.DepthInTexels);
          c.list->CopyTiles(volume.texture.Get(), &p, &one, tileReadback.Get(), UINT64(tile) * kTile,
                             D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
        }
  transition(c.list.Get(), volume.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, shaderState);
  c.execute("volume physical bytes close"); c.finish("volume physical bytes");
  void *mappedTiles = nullptr; check(tileReadback->Map(0, nullptr, &mappedTiles), "volume tile bytes map");
  const auto *actualBytes = static_cast<const BYTE *>(mappedTiles);
  for (UINT tile = 0; tile < volume.tileCount; ++tile) if (volume.mapping[tile] >= 0) {
    const auto *expected = volume.bytes.data() + size_t(volume.mapping[tile]) * kTile;
    for (UINT byte = 0; byte < kTile; ++byte) if (actualBytes[size_t(tile) * kTile + byte] != expected[byte]) {
      std::fprintf(stderr, "VOLUME_COPYTILES case=%s tile=%u physical=%d byte=%u actual=%02x expected=%02x\n",
          marker, tile, volume.mapping[tile], byte, actualBytes[size_t(tile) * kTile + byte], expected[byte]);
      fail("volume complete 64KB physical alias/copy/clear/RTV oracle");
    }
  }
  tileReadback->Unmap(0, nullptr);
  pass(marker);
}

VolumeColor volumeLevel(const VolumeResource &resource, const VolumeCase &test, const double uv[3], UINT mip, bool linear) {
  const UINT actualMip = test.firstMip + mip;
  const int size[3] = {int(resource.width >> actualMip), int(resource.height >> actualMip), int(resource.depth >> actualMip)};
  int base[3];
  double fraction[3];
  for (UINT axis = 0; axis < 3; ++axis) {
    double u = uv[axis];
    if (test.sampler.address == D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE) u = std::fabs(u);
    const double p = u * size[axis] - (linear ? 0.5 : 0);
    base[axis] = int(std::floor(p));
    fraction[axis] = p - std::floor(p);
  }
  VolumeColor result;
  bool seen = false;
  for (UINT i = 0; i < (linear ? 8u : 1u); ++i) {
    int p[3];
    double weight = 1;
    bool border = false;
    for (UINT axis = 0; axis < 3; ++axis) {
      const bool upper = (i >> axis) & 1;
      p[axis] = base[axis] + (linear && upper ? 1 : 0);
      if (linear) weight *= upper ? fraction[axis] : 1 - fraction[axis];
      switch (test.sampler.address) {
      case D3D12_TEXTURE_ADDRESS_MODE_WRAP:
        p[axis] = (p[axis] % size[axis] + size[axis]) % size[axis]; break;
      case D3D12_TEXTURE_ADDRESS_MODE_MIRROR: {
        const int period = 2 * size[axis];
        const int q = (p[axis] % period + period) % period;
        p[axis] = q < size[axis] ? q : period - q - 1; break;
      }
      case D3D12_TEXTURE_ADDRESS_MODE_BORDER:
        border |= p[axis] < 0 || p[axis] >= size[axis]; break;
      default:
        p[axis] = p[axis] < 0 ? 0 : p[axis] >= size[axis] ? size[axis] - 1 : p[axis]; break;
      }
    }
    if (weight <= 0) continue;
    VolumeColor tap;
    if (border) std::memcpy(tap.lane, test.sampler.border, sizeof(tap.lane));
    else tap = resource.texel(actualMip, p[0], p[1], p[2], test.srgb, test.signedDecode);
    result.mapped &= tap.mapped;
    result.bounded |= tap.bounded;
    for (UINT channel = 0; channel < 4; ++channel) {
      for (UINT bound = 0; bound < 2; ++bound) {
        double &out = bound ? result.upper[channel] : result.lower[channel];
        const double value = tap.bounded ? (bound ? tap.upper[channel] : tap.lower[channel]) : tap.lane[channel];
        if (test.sampler.reduction == 2) out = !seen ? value : std::fmin(out, value);
        else if (test.sampler.reduction == 3) out = !seen ? value : std::fmax(out, value);
        else out += weight * value;
      }
      if (test.sampler.reduction == 2)
        result.lane[channel] = !seen || tap.lane[channel] < result.lane[channel] ? tap.lane[channel] : result.lane[channel];
      else if (test.sampler.reduction == 3)
        result.lane[channel] = !seen || tap.lane[channel] > result.lane[channel] ? tap.lane[channel] : result.lane[channel];
      else result.lane[channel] += weight * tap.lane[channel];
    }
    seen = true;
  }
  return result;
}
VolumeColor volumeSample(const VolumeResource &resource, const VolumeCase &test, const double uv[3], double lod, double shaderMin = 0) {
  VolumeColor result;
  result.bounded = resource.format == DXGI_FORMAT_BC1_UNORM;
  const double minimum = std::fmax(test.resourceMin, std::fmax(shaderMin, test.sampler.minimum));
  if (test.nullView || std::fmax(test.resourceMin, shaderMin) > test.mipCount - 1) return result;
  lod = std::fmax(minimum, std::fmin(test.sampler.maximum, lod));
  const bool linear = lod <= 0 ? test.sampler.magLinear : test.sampler.minLinear;
  lod = std::fmax(0, std::fmin(test.mipCount - 1, lod));
  const UINT mip = UINT(std::floor(lod + (test.sampler.mipLinear ? 0 : 0.5)));
  result = volumeLevel(resource, test, uv, mip, linear);
  const double fraction = test.sampler.mipLinear ? lod - std::floor(lod) : 0;
  if (fraction == 0) return result;
  const auto other = volumeLevel(resource, test, uv, mip + 1, linear);
  result.mapped &= other.mapped;
  result.bounded |= other.bounded;
  for (UINT channel = 0; channel < 4; ++channel) {
    for (UINT bound = 0; bound < 2; ++bound) {
      double &out = bound ? result.upper[channel] : result.lower[channel];
      const double value = other.bounded ? (bound ? other.upper[channel] : other.lower[channel]) : other.lane[channel];
      if (test.sampler.reduction == 2) out = std::fmin(out, value);
      else if (test.sampler.reduction == 3) out = std::fmax(out, value);
      else out = (1 - fraction) * out + fraction * value;
    }
    if (test.sampler.reduction == 2) result.lane[channel] = std::fmin(result.lane[channel], other.lane[channel]);
    else if (test.sampler.reduction == 3) result.lane[channel] = std::fmax(result.lane[channel], other.lane[channel]);
    else result.lane[channel] = (1 - fraction) * result.lane[channel] + fraction * other.lane[channel];
  }
  return result;
}
VolumeColor volumeGrad(const VolumeResource &resource, const VolumeCase &test, const double uv[3], const double dx[3], const double dy[3]) {
  double a = 0, b = 0, c = 0;
  for (UINT axis = 0; axis < 3; ++axis) {
    const double size = (axis == 0 ? resource.width : axis == 1 ? resource.height : resource.depth) >> test.firstMip;
    a += dx[axis] * dx[axis] * size * size;
    b += dy[axis] * dy[axis] * size * size;
    c += dx[axis] * dy[axis] * size * size;
  }
  double lod = 0.5 * std::log2(std::fmax(a, b)) + test.sampler.bias;
  if (test.sampler.anisotropy == 1) return volumeSample(resource, test, uv, lod, test.words[7]);
  // Independently obtain the principal direction by rotating the gradient basis.
  const double angle = 0.5 * std::atan2(2 * c, a - b);
  const double ex = std::cos(angle), ey = std::sin(angle);
  const double major = std::sqrt(a * ex * ex + 2 * c * ex * ey + b * ey * ey);
  const double minor = std::fmax(major / test.sampler.anisotropy,
      std::sqrt(std::fmax(0, a * ey * ey - 2 * c * ex * ey + b * ex * ex)));
  lod = std::log2(minor) + test.sampler.bias;
  const UINT taps = UINT(std::ceil(std::fmax(1, std::fmin(double(test.sampler.anisotropy), major / minor))));
  VolumeColor result;
  for (UINT tap = 0; tap < taps; ++tap) {
    const double distance = (tap + 0.5) / taps - 0.5;
    double coordinate[3];
    for (UINT axis = 0; axis < 3; ++axis) coordinate[axis] = uv[axis] + distance * (ex * dx[axis] + ey * dy[axis]);
    const auto value = volumeSample(resource, test, coordinate, lod, test.words[7]);
    result.mapped &= value.mapped;
    result.bounded |= value.bounded;
    for (UINT channel = 0; channel < 4; ++channel) {
      for (UINT bound = 0; bound < 2; ++bound) {
        double &out = bound ? result.upper[channel] : result.lower[channel];
        const double limit = value.bounded ? (bound ? value.upper[channel] : value.lower[channel]) : value.lane[channel];
        if (test.sampler.reduction == 2) out = tap ? std::fmin(out, limit) : limit;
        else if (test.sampler.reduction == 3) out = tap ? std::fmax(out, limit) : limit;
        else out += limit / taps;
      }
      if (test.sampler.reduction == 2) result.lane[channel] = tap ? std::fmin(result.lane[channel], value.lane[channel]) : value.lane[channel];
      else if (test.sampler.reduction == 3) result.lane[channel] = tap ? std::fmax(result.lane[channel], value.lane[channel]) : value.lane[channel];
      else result.lane[channel] += value.lane[channel] / taps;
    }
  }
  return result;
}
bool volumeColorMatches(float actual, const VolumeColor &expected, UINT channel) {
  if (!std::isfinite(actual)) return false;
  if (!expected.bounded) return std::fabs(actual - expected.lane[channel]) <= .003;
  if (expected.lower[channel] == expected.upper[channel]) return actual == float(expected.lower[channel]);
  // Compare in double: the spec uses a strict error bound, not a rounded float epsilon.
  return (actual > expected.lower[channel] || (actual == 0 && expected.lower[channel] == 0)) &&
         (actual < expected.upper[channel] || (actual == 1 && expected.upper[channel] == 1));
}
void volumeExpectColor(const Words &words, UINT offset, const VolumeColor &expected, const char *step) {
  for (UINT channel = 0; channel < 4; ++channel) {
    float actual;
    std::memcpy(&actual, &words.data[offset + channel], 4);
    if (!volumeColorMatches(actual, expected, channel)) {
      std::fprintf(stderr, "VOLUME_ORACLE step=%s channel=%u actual=%.9g expected=%.9g lower=%.9g upper=%.9g bounded=%u\n",
          step, channel, actual, expected.lane[channel], expected.lower[channel], expected.upper[channel], UINT(expected.bounded));
      fail(step);
    }
  }
}
void volumeOracleSelfTest() {
  const UINT redGreen = 0x07e0f800;
  const auto value = volumeDecodeBC1(redGreen, 3, 0, false);
  require(value.lane[0] == 85.0 / 255 && std::fabs(value.lower[0] - (85.0 / 255 - (1.0 / 255 + .03))) < 1e-15,
          "BC1 spec reference and endpoint-span bound");
  require(volumeColorMatches(84.0f / 255, value, 0), "BC1 legal 84 versus 85");
  for (UINT channel = 0; channel < 2; ++channel) {
    require(volumeColorMatches(float(value.lane[channel]), value, channel), "BC1 reference accepted");
    require(volumeColorMatches(std::nextafter(float(value.lower[channel]), INFINITY), value, channel) &&
            volumeColorMatches(std::nextafter(float(value.upper[channel]), -INFINITY), value, channel), "BC1 inside bounds accepted");
    require(!volumeColorMatches(std::nextafter(float(value.lower[channel]), -INFINITY), value, channel) &&
            !volumeColorMatches(std::nextafter(float(value.upper[channel]), INFINITY), value, channel), "BC1 outside bounds rejected");
  }
  require(!volumeColorMatches(170.0f / 255, value, 0) && !volumeColorMatches(1, value, 0), "BC1 wrong selector/block rejected");
  require(!volumeColorMatches(85.0f / 255, value, 1) && !volumeColorMatches(1.0f / 255, value, 2), "BC1 wrong channel/exact zero rejected");
  require(!volumeColorMatches(std::nextafter(1.0f, 0.0f), value, 3) && !volumeColorMatches(NAN, value, 0), "BC1 exact alpha and finite output");
  VolumeColor boundary; boundary.bounded = true; boundary.lower[0] = .25; boundary.upper[0] = .75;
  require(!volumeColorMatches(.25f, boundary, 0) && !volumeColorMatches(.75f, boundary, 0) &&
          volumeColorMatches(std::nextafter(.25f, 1.0f), boundary, 0) &&
          volumeColorMatches(std::nextafter(.75f, 0.0f), boundary, 0), "BC1 strict error boundary");
  const auto transparent = volumeDecodeBC1(0x07e0001f, 3, 0, true);
  for (UINT channel = 0; channel < 4; ++channel)
    require(volumeColorMatches(0, transparent, channel) && !volumeColorMatches(1.0f / 255, transparent, channel), "BC1 transparent black exact");
  const auto promoted = volumeDecodeBC1((3u << 11) | (17u << 5) | 3u, 0, 0, false);
  require(promoted.lane[0] == 24.0 / 255 && promoted.lane[1] == 69.0 / 255, "BC1 MSB endpoint promotion");
  const auto half = volumeDecodeBC1(0x07e0001f, 2, 0, false);
  require(half.lane[1] == 127.0 / 255 && volumeColorMatches(128.0f / 255, half, 1), "BC1 integer half reference and legal rounding");
  const auto srgb = volumeDecodeBC1(redGreen, 3, 0, true);
  require(volumeColorMatches(float(std::pow((84.0 / 255 + .055) / 1.055, 2.4)), srgb, 0), "BC1 legal 84 after sRGB conversion");
  require(volumeColorMatches(float(srgb.lane[0]), srgb, 0) && !volumeColorMatches(84.0f / 255, srgb, 0), "BC1 missing sRGB conversion rejected");
  require(!volumeColorMatches(float(srgb.lane[0]), value, 0), "BC1 spurious sRGB conversion rejected");
  require(!volumeColorMatches(std::nextafter(float(srgb.lower[0]), -INFINITY), srgb, 0) &&
          !volumeColorMatches(std::nextafter(float(srgb.upper[0]), INFINITY), srgb, 0), "BC1 sRGB outside transformed bounds rejected");
  const double srgbLow = 85.0 / 255 - (1.0 / 255 + .03 + 1.5 / 255);
  require(std::fabs(srgb.lower[0] - std::pow((srgbLow + .055) / 1.055, 2.4)) < 1e-15, "BC1 sRGB input-side precision bound");

  VolumeResource resource; resource.format = DXGI_FORMAT_BC1_UNORM; resource.mipCount = 2;
  resource.width = resource.height = 4; resource.depth = 2; resource.block = 4; resource.unitBytes = 8;
  resource.shape = {4, 4, 1}; resource.tiling[0] = {1, 1, 2, 0}; resource.tiling[1] = {1, 1, 1, 2};
  resource.mapping = {0, 1, 2}; resource.bytes.resize(3 * kTile);
  const UINT selectors[3] = {0xffffffff, 0xaaaaaaaa, 0};
  for (UINT tile = 0; tile < 3; ++tile) {
    std::memcpy(resource.bytes.data() + tile * kTile, &redGreen, 4);
    std::memcpy(resource.bytes.data() + tile * kTile + 4, &selectors[tile], 4);
  }
  VolumeCase test; test.srgb = true; test.mipCount = 2;
  const double uv[3] = {.125, .125, .5};
  const auto other = volumeDecodeBC1(redGreen, 2, 0, true);
  const auto filtered = volumeLevel(resource, test, uv, 0, true);
  require(filtered.bounded && std::fabs(filtered.lower[0] - .5 * (srgb.lower[0] + other.lower[0])) < 1e-15 &&
          std::fabs(filtered.upper[0] - .5 * (srgb.upper[0] + other.upper[0])) < 1e-15, "BC1 sRGB bounds linearized before Z interpolation");
  test.sampler.reduction = 2;
  const auto minimum = volumeLevel(resource, test, uv, 0, true);
  require(minimum.lower[0] == srgb.lower[0] && minimum.upper[0] == srgb.upper[0], "BC1 minimum interval");
  test.sampler.reduction = 3;
  const auto maximum = volumeLevel(resource, test, uv, 0, true);
  require(maximum.lower[0] == other.lower[0] && maximum.upper[0] == other.upper[0], "BC1 maximum interval");
  test.sampler.reduction = 0;
  const auto trilinear = volumeSample(resource, test, uv, .5);
  require(std::fabs(trilinear.lower[0] - .5 * (filtered.lower[0] + 1)) < 1e-15 &&
          std::fabs(trilinear.upper[0] - .5 * (filtered.upper[0] + 1)) < 1e-15, "BC1 mip interval");
  test.sampler.anisotropy = 2; const double dx[3] = {.5, 0, 0}, dy[3] = {0, .25, 0};
  const auto anisotropic = volumeGrad(resource, test, uv, dx, dy);
  require(anisotropic.bounded && std::fabs(anisotropic.lower[0] - filtered.lower[0]) < 1e-15 &&
          std::fabs(anisotropic.upper[0] - filtered.upper[0]) < 1e-15, "BC1 anisotropic interval");
  resource.mapping[0] = -1;
  const auto unmapped = resource.texel(0, 0, 0, 0);
  require(!unmapped.mapped && unmapped.bounded && !volumeColorMatches(1.0f / 255, unmapped, 0), "BC1 NULL mapping exact zero");
  pass("volume-BC1-spec-bounds-CPU-self-test");
}

struct VolumeSamplerDesc2 {
  D3D12_FILTER Filter;
  D3D12_TEXTURE_ADDRESS_MODE AddressU, AddressV, AddressW;
  FLOAT MipLODBias;
  UINT MaxAnisotropy;
  D3D12_COMPARISON_FUNC ComparisonFunc;
  union { FLOAT FloatBorderColor[4]; UINT UintBorderColor[4]; };
  FLOAT MinLOD, MaxLOD;
  UINT Flags;
};
struct VolumeSamplerDevice11 : public ID3D12Device10 {
  virtual void STDMETHODCALLTYPE CreateSampler2(const VolumeSamplerDesc2 *, D3D12_CPU_DESCRIPTOR_HANDLE) = 0;
};

void runVolumeCases(Context &c, int shaderCount, const char *const *paths) {
  require(shaderCount == 10, "volume shaders: DXIL read,uint,vs,sampleps,colorps; DXBC same order");
  auto *device = c.device.Get();
  const auto anyShader = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  VolumeResource volume(c, DXGI_FORMAT_R8G8B8A8_TYPELESS, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  VolumeResource integer(c, DXGI_FORMAT_R32_UINT, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  // Real ordered NULL and physical aliases across independently mapped logical pages.
  volume.remap(c, 0, 0, 0, -1);
  volume.remap(c, 1, 0, 0, 3);
  c.finish("volume ordered null and alias mappings");

  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ComPtr<ID3D12DescriptorHeap> views, samplers;
  check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&views)), "volume views");
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
  check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&samplers)), "volume samplers");
  const UINT viewStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT samplerStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  auto cpuView = [&](UINT i) { auto h = views->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(i) * viewStride; return h; };
  auto gpuView = [&](UINT i) { auto h = views->GetGPUDescriptorHandleForHeapStart(); h.ptr += UINT64(i) * viewStride; return h; };
  auto cpuSampler = [&](UINT i) { auto h = samplers->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(i) * samplerStride; return h; };
  auto gpuSampler = [&](UINT i) { auto h = samplers->GetGPUDescriptorHandleForHeapStart(); h.ptr += UINT64(i) * samplerStride; return h; };
  D3D12_DESCRIPTOR_RANGE ranges[5] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                    {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1},
                                    {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 2},
                                    {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2, 0, 3},
                                    {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3, 0, 4}};
  D3D12_DESCRIPTOR_RANGE samplerRange{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[4]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants = {0, 0, 16};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable = {5, ranges};
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[2].Descriptor = {1, 0};
  params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[3].DescriptorTable = {1, &samplerRange};
  const auto root = rootSignature(device, {4, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
  const auto output = c.buffer(D3D12_HEAP_TYPE_DEFAULT, 65536, "volume output");
  const auto readback = c.buffer(D3D12_HEAP_TYPE_READBACK, 65536, "volume readback");
  ID3D12DescriptorHeap *heaps[] = {views.Get(), samplers.Get()};
  std::vector<VolumeCase> tests;
  for (UINT filter = 0; filter < 8; ++filter)
    for (UINT address = 1; address <= 5; ++address)
      for (UINT position = 0; position < 3; ++position) {
        VolumeCase test;
        test.sampler.minLinear = filter & 1;
        test.sampler.magLinear = filter & 2;
        test.sampler.mipLinear = filter & 4;
        test.sampler.address = address;
        test.words[0] = position == 0 ? 0.5f : position == 1 ? -0.00390625f : 1.00390625f;
        test.words[1] = 0.5f;
        test.words[2] = 0.5f;
        test.words[3] = 0.375f;
        test.words[4] = 0.03125f;
        test.words[9] = 0.015625f;
        test.integers[0] = 31; test.integers[1] = 31; test.integers[2] = 15;
        tests.push_back(test);
      }
  for (UINT reduction = 2; reduction <= 3; ++reduction) {
    auto test = tests[105];
    test.sampler.reduction = reduction;
    test.words[0] = test.words[1] = 0.5f;
    test.words[2] = 0.5f;
    tests.push_back(test);
  }
  for (UINT cap : {2u, 8u}) {
    auto test = tests[105];
    test.sampler.anisotropy = cap;
    test.words[4] = 0.125f; test.words[5] = 0.0625f; test.words[6] = 0.03125f;
    test.words[8] = 0.015625f; test.words[9] = 0.03125f; test.words[10] = 0.0078125f;
    tests.push_back(test);
  }
  for (UINT linear = 0; linear < 2; ++linear) {
    auto test = tests[105]; test.sampler.magLinear = linear != 0;
    test.words[3] = -1; test.words[4] = test.words[9] = 0.00390625f;
    tests.push_back(test); // both explicit LOD and gradients take the magnification filter
  }
  for (float bias : {-1.25f, 1.25f}) {
    auto test = tests[105]; test.sampler.bias = bias; tests.push_back(test);
  }
  {
    auto test = tests[105]; test.sampler.minimum = 1.25; test.sampler.maximum = 1.75; tests.push_back(test);
    auto clampedMag = test; clampedMag.sampler.magLinear = false; clampedMag.words[3] = -1;
    clampedMag.words[4] = clampedMag.words[9] = 0.00390625f; tests.push_back(clampedMag);
    clampedMag.sampler.minimum = 0; clampedMag.sampler.maximum = 16; clampedMag.resourceMin = 0.5f;
    tests.push_back(clampedMag); // a resource clamp also turns the magnifying footprint into MIN filtering
    test.sampler.minimum = 0; test.sampler.maximum = 0.25; tests.push_back(test);
    test.sampler.maximum = 16; test.words[7] = 2.25f; tests.push_back(test);
    test.words[7] = 6; tests.push_back(test); // per-sample clamp excludes every view mip: empty, unlike sampler MinLOD
    test.words[7] = 0; test.sampler.minimum = 6; tests.push_back(test);
    for (UINT mip = 1; mip < 4; ++mip) {
      test = tests[105]; test.words[3] = test.words[11] = float(mip);
      test.integers[0] = test.integers[1] = 15 >> mip; test.integers[2] = 7 >> mip;
      tests.push_back(test);
    }
    test = tests[105]; test.firstMip = 2; test.mipCount = 1;
    test.integers[0] = test.integers[1] = 7; test.integers[2] = 3; tests.push_back(test);
  }
  {
    auto test = tests[105]; test.srgb = true; tests.push_back(test);
    test.srgb = false; test.firstMip = 1; test.mipCount = 2; test.resourceMin = 0.5f;
    test.words[3] = 0; test.words[7] = 0.75f; test.integers[0] = test.integers[1] = 15; test.integers[2] = 7;
    tests.push_back(test);
    test.resourceMin = 1.1f; tests.push_back(test); // exceeds the last view mip: empty even though floor is 1
    test.resourceMin = 0; test.nullView = true; tests.push_back(test);
    test.nullView = false; test.firstMip = 0; test.mipCount = 4; test.integers[0] = 64; tests.push_back(test);
    test.integers[0] = 32; test.words[0] = (32.5f / 64); test.words[1] = 0.5f / 64; test.words[2] = 0.5f / 32;
    tests.push_back(test); // zero-weight neighboring NULL texels must not report a residency miss
    for (UINT reduction = 2; reduction <= 3; ++reduction) {
      test.sampler.reduction = reduction; tests.push_back(test);
    }
  }
  require(tests.size() + 4 < 256, "volume fixture descriptor capacity");
  for (UINT i = 0; i < tests.size(); ++i) {
    auto &test = tests[i];
    test.integers[3] = i * 80;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv.Format = test.srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.Texture3D = {test.firstMip, test.mipCount, test.resourceMin + test.firstMip};
    device->CreateShaderResourceView(test.nullView ? nullptr : volume.texture.Get(), &srv, cpuView(i));
    D3D12_SAMPLER_DESC sd{};
    sd.Filter = D3D12_FILTER((test.sampler.minLinear ? 0x10 : 0) | (test.sampler.magLinear ? 4 : 0) |
                              (test.sampler.mipLinear ? 1 : 0) | (test.sampler.reduction << 7));
    if (test.sampler.anisotropy > 1) sd.Filter = D3D12_FILTER_ANISOTROPIC;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE(test.sampler.address);
    sd.MaxAnisotropy = test.sampler.anisotropy;
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sd.MipLODBias = float(test.sampler.bias);
    sd.MinLOD = float(test.sampler.minimum); sd.MaxLOD = float(test.sampler.maximum);
    for (UINT channel = 0; channel < 4; ++channel) sd.BorderColor[channel] = float(test.sampler.border[channel]);
    device->CreateSampler(&sd, cpuSampler(i));
  }

  for (UINT api = 0; api < 2; ++api) {
    const auto readCS = readFile(paths[api * 5]);
    const auto readPSO = computePipeline(device, root.Get(), readCS);
    c.reset();
    c.list->SetDescriptorHeaps(2, heaps);
    c.list->SetComputeRootSignature(root.Get());
    c.list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
    c.list->SetPipelineState(readPSO.Get());
    for (UINT i = 0; i < tests.size(); ++i) {
      UINT constants[16];
      std::memcpy(constants, tests[i].words, 48); std::memcpy(constants + 12, tests[i].integers, 16);
      c.list->SetComputeRoot32BitConstants(0, 16, constants, 0);
      c.list->SetComputeRootDescriptorTable(1, gpuView(i));
      c.list->SetComputeRootDescriptorTable(3, gpuSampler(i));
      c.list->Dispatch(1, 1, 1);
    }
    uavBarrier(c.list.Get());
    transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    c.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, tests.size() * 80);
    transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.execute("volume sampling close"); c.finish("volume sampling");
    Words data(readback);
    for (UINT i = 0; i < tests.size(); ++i) {
      const auto &test = tests[i];
      double uv[3], dx[3], dy[3];
      for (UINT axis = 0; axis < 3; ++axis) { uv[axis] = test.words[axis]; dx[axis] = test.words[4 + axis]; dy[axis] = test.words[8 + axis]; }
      const auto level = volumeSample(volume, test, uv, test.words[3] + test.sampler.bias);
      const auto grad = volumeGrad(volume, test, uv, dx, dy);
      VolumeColor load;
      if (!test.nullView && test.resourceMin <= test.mipCount - 1 && test.words[11] >= std::floor(test.resourceMin))
        load = volume.texel(test.firstMip + UINT(test.words[11]), int(test.integers[0]), int(test.integers[1]), int(test.integers[2]), test.srgb);
      volumeExpectColor(data, i * 20, level, "volume SampleLevel CPU oracle");
      volumeExpectColor(data, i * 20 + 4, grad, "volume SampleGrad CPU oracle");
      volumeExpectColor(data, i * 20 + 8, load, "volume Load CPU oracle");
      expect("volume width", data[i * 20 + 12], test.nullView ? 0 : 64u >> (test.firstMip + UINT(test.words[11])));
      expect("volume height", data[i * 20 + 13], test.nullView ? 0 : 64u >> (test.firstMip + UINT(test.words[11])));
      expect("volume depth", data[i * 20 + 14], test.nullView ? 0 : 32u >> (test.firstMip + UINT(test.words[11])));
      expect("volume mips", data[i * 20 + 15], test.nullView ? 0 : test.mipCount);
      if (data[i * 20 + 16] != UINT(level.mapped) || data[i * 20 + 17] != UINT(grad.mapped) ||
          data[i * 20 + 18] != UINT(load.mapped)) {
        std::fprintf(stderr, "OWNED_D3D12_TILED_VOLUME_STATUS compiler=%s case=%u actual=%08x,%08x,%08x expected=%u,%u,%u "
            "uv=%g,%g,%g lod=%g filter=%u,%u,%u address=%u firstMip=%u mipCount=%u resourceMin=%g null=%u\n",
            api ? "DXBC" : "DXIL", i, data[i * 20 + 16], data[i * 20 + 17], data[i * 20 + 18],
            UINT(level.mapped), UINT(grad.mapped), UINT(load.mapped), uv[0], uv[1], uv[2], double(test.words[3]),
            UINT(test.sampler.minLinear), UINT(test.sampler.magLinear), UINT(test.sampler.mipLinear), test.sampler.address,
            test.firstMip, test.mipCount, double(test.resourceMin), UINT(test.nullView));
      }
      expect("volume level mapped", data[i * 20 + 16], level.mapped);
      expect("volume grad mapped", data[i * 20 + 17], grad.mapped);
      expect("volume load mapped", data[i * 20 + 18], load.mapped);
    }
    std::printf("OWNED_D3D12_TILED_VOLUME_SAMPLING compiler=%s cases=%zu oracle=independent_CPU\n", api ? "DXBC" : "DXIL", tests.size());
    pass(api ? "volume-dxbc-filter-lod-address-residency" : "volume-dxil-filter-lod-address-residency");
  }

  // Pixel-stage implicit gradients, sample bias and both CalculateLOD forms.
  auto targetDesc = textureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, 1, 1,
      DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_TEXTURE_LAYOUT_UNKNOWN);
  targetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  const auto target = c.committed(D3D12_HEAP_TYPE_DEFAULT, targetDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, "volume sample target");
  hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ComPtr<ID3D12DescriptorHeap> rtvs;
  check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)), "volume RT views");
  const auto sampleRTV = rtvs->GetCPUDescriptorHandleForHeapStart();
  auto volumeRTV = sampleRTV;
  volumeRTV.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  device->CreateRenderTargetView(target.Get(), nullptr, sampleRTV);
  auto graphics = [&](const std::vector<BYTE> &vs, const std::vector<BYTE> &ps, DXGI_FORMAT format) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd{};
    gd.pRootSignature = root.Get(); gd.VS = {vs.data(), vs.size()}; gd.PS = {ps.data(), ps.size()};
    gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gd.SampleMask = UINT_MAX; gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gd.NumRenderTargets = 1; gd.RTVFormats[0] = format; gd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    check(device->CreateGraphicsPipelineState(&gd, IID_PPV_ARGS(&pso)), "volume graphics pipeline");
    return pso;
  };
  for (UINT api = 0; api < 2; ++api) {
    const auto vs = readFile(paths[api * 5 + 2]);
    const auto ps = readFile(paths[api * 5 + 3]);
    const auto pso = graphics(vs, ps, DXGI_FORMAT_R32G32B32A32_FLOAT);
    VolumeCase test = tests[105];
    test.words[0] = test.words[1] = test.words[2] = 0.45f;
    test.words[4] = test.words[9] = 0.015625f;
    test.words[5] = test.words[6] = test.words[8] = test.words[10] = 0;
    test.integers[3] = 0;
    UINT constants[16]; std::memcpy(constants, test.words, 48); std::memcpy(constants + 12, test.integers, 16);
    c.reset(); c.list->SetDescriptorHeaps(2, heaps); c.list->SetGraphicsRootSignature(root.Get());
    c.list->SetGraphicsRoot32BitConstants(0, 16, constants, 0);
    c.list->SetGraphicsRootDescriptorTable(1, gpuView(105));
    c.list->SetGraphicsRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
    c.list->SetGraphicsRootDescriptorTable(3, gpuSampler(105));
    c.list->SetPipelineState(pso.Get()); c.list->OMSetRenderTargets(1, &sampleRTV, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, 4, 4, 0, 1}; const D3D12_RECT scissor{0, 0, 4, 4};
    c.list->RSSetViewports(1, &viewport); c.list->RSSetScissorRects(1, &scissor);
    c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); c.list->DrawInstanced(3, 1, 0, 0);
    uavBarrier(c.list.Get());
    transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    c.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 16 * 48);
    transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.execute("volume implicit sampling close"); c.finish("volume implicit sampling");
    Words data(readback);
    for (UINT y = 0; y < 4; ++y) for (UINT x = 0; x < 4; ++x) {
      double uv[3];
      for (UINT axis = 0; axis < 3; ++axis)
        uv[axis] = test.words[axis] + (x + 0.5) * test.words[4 + axis] + (3.5 - y) * test.words[8 + axis];
      const auto implicit = volumeSample(volume, test, uv, 0);
      const auto bias = volumeSample(volume, test, uv, 1);
      const UINT at = 12 * (x + 4 * y);
      volumeExpectColor(data, at, implicit, "volume implicit sample");
      volumeExpectColor(data, at + 4, bias, "volume SampleBias");
      float clamped, unclamped; std::memcpy(&clamped, &data.data[at + 8], 4); std::memcpy(&unclamped, &data.data[at + 9], 4);
      require(std::fabs(clamped) < 0.0001 && std::fabs(unclamped) < 0.0001, "volume CalculateLOD");
      expect("volume implicit mapped", data[at + 10], implicit.mapped);
      expect("volume bias mapped", data[at + 11], bias.mapped);
    }
    pass(api ? "volume-dxbc-implicit-bias-calculateLOD" : "volume-dxil-implicit-bias-calculateLOD");
  }

  // Exact UINT border metadata, partial-W UAV addressing, stores, readback and both atomic forms.
  const GUID device11IID{0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};
  ComPtr<VolumeSamplerDevice11> device11;
  check(device->QueryInterface(device11IID, reinterpret_cast<void **>(device11.GetAddressOf())), "volume sampler Device11");
  VolumeSamplerDesc2 integerSampler{};
  integerSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  integerSampler.AddressU = integerSampler.AddressV = integerSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
  integerSampler.MaxAnisotropy = 1; integerSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  integerSampler.MaxLOD = D3D12_FLOAT32_MAX; integerSampler.Flags = 1;
  for (auto &value : integerSampler.UintBorderColor) value = 0xf1234567u;
  device11->CreateSampler2(&integerSampler, cpuSampler(200));
  D3D12_SHADER_RESOURCE_VIEW_DESC integerSRV{};
  integerSRV.Format = DXGI_FORMAT_R32_UINT; integerSRV.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
  integerSRV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; integerSRV.Texture3D.MipLevels = 1;
  device->CreateShaderResourceView(integer.texture.Get(), &integerSRV, cpuView(200));
  D3D12_UNORDERED_ACCESS_VIEW_DESC integerUAV{};
  integerUAV.Format = DXGI_FORMAT_R32_UINT; integerUAV.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
  integerUAV.Texture3D = {0, integer.shape.DepthInTexels, integer.shape.DepthInTexels};
  device->CreateUnorderedAccessView(integer.texture.Get(), nullptr, &integerUAV, cpuView(201));
  D3D12_SHADER_RESOURCE_VIEW_DESC nullBlockSRV{};
  nullBlockSRV.Format = DXGI_FORMAT_R32G32_UINT; nullBlockSRV.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
  nullBlockSRV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; nullBlockSRV.Texture3D.MipLevels = 1;
  device->CreateShaderResourceView(nullptr, &nullBlockSRV, cpuView(202));
  auto nullBlockUAV = integerUAV; nullBlockUAV.Format = DXGI_FORMAT_R32G32_UINT;
  device->CreateUnorderedAccessView(nullptr, nullptr, &nullBlockUAV, cpuView(203));
  auto nullWideUAV = nullBlockUAV; nullWideUAV.Format = DXGI_FORMAT_R16G16B16A16_UINT;
  device->CreateUnorderedAccessView(nullptr, nullptr, &nullWideUAV, cpuView(204));
  const UINT px = integer.shape.WidthInTexels + 3, py = integer.shape.HeightInTexels + 5, pz = integer.shape.DepthInTexels + 7;
  UINT currentValue; std::memcpy(&currentValue, integer.bytes.data() + integer.address(0, px, py, pz), 4);
  for (UINT api = 0; api < 2; ++api) {
    const auto shader = readFile(paths[api * 5 + 1]); const auto pso = computePipeline(device, root.Get(), shader);
    UINT constants[16]{};
    const float negative = -0.25f; std::memcpy(constants, &negative, 4); std::memcpy(constants + 1, &negative, 4); std::memcpy(constants + 2, &negative, 4);
    constants[4] = 0xd1234000u + api * 256;
    const float firstW = float(integer.shape.DepthInTexels); std::memcpy(constants + 11, &firstW, 4);
    constants[12] = px; constants[13] = py; constants[14] = pz - integer.shape.DepthInTexels;
    constants[15] = 0x80000000u;
    c.reset(); c.list->SetDescriptorHeaps(2, heaps); c.list->SetComputeRootSignature(root.Get()); c.list->SetPipelineState(pso.Get());
    c.list->SetComputeRoot32BitConstants(0, 16, constants, 0); c.list->SetComputeRootDescriptorTable(1, gpuView(200));
    c.list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress()); c.list->SetComputeRootDescriptorTable(3, gpuSampler(200));
    c.list->Dispatch(1, 1, 1);
    transition(c.list.Get(), integer.texture.Get(), anyShader, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    constants[15] = 0;
    c.list->SetComputeRoot32BitConstants(0, 16, constants, 0);
    c.list->Dispatch(1, 1, 1); uavBarrier(c.list.Get());
    transition(c.list.Get(), integer.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, anyShader);
    transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    c.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 48);
    transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.execute("volume integer close"); c.finish("volume integer");
    Words data(readback);
    expect("volume integer SRV load", data[0], currentValue);
    expect(api ? "volume DXBC integer load" : "volume exact UINT border", data[1], api ? currentValue : 0xf1234567u);
    expect("volume uint load mapped", data[2], 1); expect("volume uint border mapped", data[3], 1);
    expect("volume partial-W UAV read", data[4], currentValue); expect("volume atomic add prior", data[5], constants[4]);
    expect("volume cmpxchg prior", data[6], constants[4] + 7); expect("volume UAV final", data[7], constants[4] + 11);
    expect("volume UAV width", data[8], 64); expect("volume UAV height", data[9], 64);
    expect("volume UAV WSize", data[10], integer.shape.DepthInTexels);
    currentValue = constants[4] + 11;
    std::memcpy(integer.bytes.data() + integer.address(0, px, py, pz), &currentValue, 4);
    pass(api ? "volume-dxbc-uint-load-store-atomic-partialW" : "volume-dxil-uint-border-store-atomic-partialW");
  }
  for (UINT api = 0; api < 2; ++api) for (UINT invalidZ : {integer.shape.DepthInTexels, ~0u}) {
    const auto shader = readFile(paths[api * 5 + 1]); const auto pso = computePipeline(device, root.Get(), shader);
    UINT constants[16]{}; constants[4] = 0xfeed1234u;
    constants[12] = px; constants[13] = py; constants[14] = invalidZ;
    c.reset(); c.list->SetDescriptorHeaps(2, heaps); c.list->SetComputeRootSignature(root.Get()); c.list->SetPipelineState(pso.Get());
    c.list->SetComputeRoot32BitConstants(0, 16, constants, 0); c.list->SetComputeRootDescriptorTable(1, gpuView(200));
    c.list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress()); c.list->SetComputeRootDescriptorTable(3, gpuSampler(200));
    transition(c.list.Get(), integer.texture.Get(), anyShader, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.list->Dispatch(1, 1, 1); uavBarrier(c.list.Get());
    transition(c.list.Get(), integer.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, anyShader);
    transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    c.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 48);
    transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.execute("volume partial W OOB close"); c.finish("volume partial W OOB");
    Words data(readback);
    for (UINT word = 4; word < 8; ++word) expect("volume partial W OOB read/store/atomic", data[word], 0);
    expect("volume OOB query WSize", data[10], integer.shape.DepthInTexels);
  }
  volumeCheckPhysicalBytes(c, integer, "volume-dxil-dxbc-partialW-OOB-read-write-atomic-preserves-64KB");

  integer.remap(c, 0, 0, 0, -1); integer.remap(c, 1, 0, 0, 3);
  integerUAV.Texture3D.FirstWSlice = 0;
  device->CreateUnorderedAccessView(integer.texture.Get(), nullptr, &integerUAV, cpuView(201));
  const UINT uintClear[4] = {0xf1234567u, 0xabcdef01u, 0x12345678u, 0x87654321u};
  c.reset(); c.list->SetDescriptorHeaps(2, heaps);
  transition(c.list.Get(), integer.texture.Get(), anyShader, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  c.list->ClearUnorderedAccessViewUint(gpuView(201), cpuView(201), integer.texture.Get(), uintClear, 0, nullptr);
  transition(c.list.Get(), integer.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, anyShader);
  c.execute("volume UINT UAV clear close"); c.finish("volume UINT UAV clear");
  for (UINT z = 0; z < integer.shape.DepthInTexels; ++z) for (UINT y = 0; y < 64; ++y) for (UINT x = 0; x < 64; ++x)
    if (integer.mapping[integer.tileAt(0, x, y, z)] >= 0)
      std::memcpy(integer.bytes.data() + integer.address(0, x, y, z), uintClear, 4);
  volumeCheckPhysicalBytes(c, integer, "volume-UAV-UINT-clear-partial-W-null-alias-64KB");

  VolumeResource floatingUAV(c, DXGI_FORMAT_R32_FLOAT, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  floatingUAV.remap(c, 0, 0, 0, -1); floatingUAV.remap(c, 1, 0, 0, 3);
  auto floatUAV = integerUAV; floatUAV.Format = DXGI_FORMAT_R32_FLOAT;
  floatUAV.Texture3D = {0, floatingUAV.shape.DepthInTexels - 1, 2};
  device->CreateUnorderedAccessView(floatingUAV.texture.Get(), nullptr, &floatUAV, cpuView(203));
  const float floatClear[4] = {0.625f, 1.25f, -3.5f, 17};
  c.reset(); c.list->SetDescriptorHeaps(2, heaps);
  transition(c.list.Get(), floatingUAV.texture.Get(), anyShader, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  c.list->ClearUnorderedAccessViewFloat(gpuView(203), cpuView(203), floatingUAV.texture.Get(), floatClear, 0, nullptr);
  transition(c.list.Get(), floatingUAV.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, anyShader);
  c.execute("volume float UAV clear close"); c.finish("volume float UAV clear");
  for (UINT z = floatingUAV.shape.DepthInTexels - 1; z <= floatingUAV.shape.DepthInTexels; ++z)
    for (UINT y = 0; y < 64; ++y) for (UINT x = 0; x < 64; ++x)
      if (floatingUAV.mapping[floatingUAV.tileAt(0, x, y, z)] >= 0)
        std::memcpy(floatingUAV.bytes.data() + floatingUAV.address(0, x, y, z), floatClear, 4);
  volumeCheckPhysicalBytes(c, floatingUAV, "volume-UAV-float-clear-cross-Z-null-alias-64KB");

  VolumeResource bc(c, DXGI_FORMAT_BC1_UNORM, 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 256, 256, 64);
  bc.remap(c, 0, 0, 0, -1); bc.remap(c, 1, 0, 0, 3);
  std::vector<VolumeCase> bcTests;
  for (UINT srgb = 0; srgb < 2; ++srgb) for (UINT mip = 0; mip < bc.mipCount; ++mip) for (UINT i = 0; i < 12; ++i) {
    VolumeCase test = tests[105]; test.srgb = srgb != 0; test.firstMip = mip; test.mipCount = bc.mipCount - mip;
    const UINT w = bc.width >> mip, h = bc.height >> mip, d = bc.depth >> mip;
    const UINT x = UINT(std::fmin(i < 8 ? bc.shape.WidthInTexels - 1 + (i & 1) : bc.shape.WidthInTexels + 4 + (i & 3), w - 1));
    const UINT y = UINT(std::fmin(i < 8 ? bc.shape.HeightInTexels - 1 + ((i >> 1) & 1) : bc.shape.HeightInTexels + 5, h - 1));
    const UINT z = UINT(std::fmin(bc.shape.DepthInTexels - 1 + ((i >> 2) & 1), d - 1));
    test.words[0] = (x + 0.375f) / w; test.words[1] = (y + 0.375f) / h; test.words[2] = (z + 0.375f) / d;
    test.words[4] = 1.0f / w; test.words[9] = 1.0f / h;
    test.integers[0] = x; test.integers[1] = y; test.integers[2] = z; test.integers[3] = UINT(bcTests.size()) * 80;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv.Texture3D = {mip, test.mipCount, float(mip)};
    device->CreateShaderResourceView(bc.texture.Get(), &srv, cpuView(UINT(bcTests.size())));
    bcTests.push_back(test);
  }
  auto checkBCSamples = [&](const VolumeResource &sampled, const std::vector<VolumeCase> &sampledTests, UINT api, const char *marker) {
    const auto &bc = sampled; const auto &bcTests = sampledTests;
    const auto shader = readFile(paths[api * 5]); const auto pso = computePipeline(device, root.Get(), shader);
    c.reset(); c.list->SetDescriptorHeaps(2, heaps); c.list->SetComputeRootSignature(root.Get()); c.list->SetPipelineState(pso.Get());
    c.list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress()); c.list->SetComputeRootDescriptorTable(3, gpuSampler(105));
    for (UINT i = 0; i < bcTests.size(); ++i) {
      UINT constants[16]; std::memcpy(constants, bcTests[i].words, 48); std::memcpy(constants + 12, bcTests[i].integers, 16);
      c.list->SetComputeRoot32BitConstants(0, 16, constants, 0); c.list->SetComputeRootDescriptorTable(1, gpuView(i)); c.list->Dispatch(1, 1, 1);
    }
    uavBarrier(c.list.Get()); transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    c.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, bcTests.size() * 80);
    transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.execute("volume BC decode close"); c.finish("volume BC decode");
    Words data(readback);
    for (UINT i = 0; i < bcTests.size(); ++i) {
      const auto &test = bcTests[i]; const double uv[3] = {test.words[0], test.words[1], test.words[2]};
      const double dx[3] = {test.words[4], test.words[5], test.words[6]}, dy[3] = {test.words[8], test.words[9], test.words[10]};
      const auto level = volumeSample(bc, test, uv, test.words[3] + test.sampler.bias), grad = volumeGrad(bc, test, uv, dx, dy);
      const auto load = bc.texel(test.firstMip, int(test.integers[0]), int(test.integers[1]), int(test.integers[2]), test.srgb, test.signedDecode);
      volumeExpectColor(data, i * 20, level, "compressed SampleLevel independent block decode");
      volumeExpectColor(data, i * 20 + 4, grad, "compressed SampleGrad independent block decode");
      volumeExpectColor(data, i * 20 + 8, load, "compressed Load independent block decode");
      expect("compressed width", data[i * 20 + 12], bc.width >> test.firstMip); expect("compressed height", data[i * 20 + 13], bc.height >> test.firstMip);
      expect("compressed depth", data[i * 20 + 14], bc.depth >> test.firstMip); expect("compressed mip count", data[i * 20 + 15], test.mipCount);
      expect("compressed level mapped", data[i * 20 + 16], level.mapped); expect("compressed grad mapped", data[i * 20 + 17], grad.mapped);
      expect("compressed load mapped", data[i * 20 + 18], load.mapped);
    }
    pass(marker);
  };
  checkBCSamples(bc, bcTests, 0, "volume-dxil-BC1-sRGB-independent-decode-xyz-mips-null-alias");
  checkBCSamples(bc, bcTests, 1, "volume-dxbc-BC1-sRGB-independent-decode-xyz-mips-null-alias");
  device->CreateShaderResourceView(integer.texture.Get(), &integerSRV, cpuView(210));
  device->CreateUnorderedAccessView(integer.texture.Get(), nullptr, &integerUAV, cpuView(211));
  auto runDualUAV = [&](ID3D12Resource *texture, UINT mip, UINT firstW, UINT countW, UINT width, UINT height,
                        const UINT position[3], bool resident, UINT api, UINT ordinal, UINT finalWords[2]) {
    auto uav = nullBlockUAV; uav.Texture3D = {mip, firstW, countW};
    device->CreateUnorderedAccessView(texture, nullptr, &uav, cpuView(213));
    uav.Format = DXGI_FORMAT_R16G16B16A16_UINT;
    device->CreateUnorderedAccessView(texture, nullptr, &uav, cpuView(214));
    const auto shader = readFile(paths[api * 5 + 1]); const auto pso = computePipeline(device, root.Get(), shader);
    UINT constants[16]{};
    constants[4] = api ? 0xf80007e0u : 0x07e0f800u;
    constants[5] = 0xc9631be4u ^ (ordinal * 0x01010101u);
    // Typed UINT stores clamp (§3.2.3.13), unlike ClearUnorderedAccessViewUint's low-bit copy.
    // Rotate one overflow lane while keeping the others distinct to detect channel swaps.
    constants[8] = 0x001fu; constants[9] = 0xf800u;
    constants[10] = (0x018du + ordinal * 0x111u) & 0xffffu;
    constants[11] = (0xe4e4u ^ ordinal) & 0xffffu;
    constants[8 + (ordinal & 3)] |= 0x10000u;
    UINT stored[4];
    for (UINT lane = 0; lane < 4; ++lane) stored[lane] = std::min(constants[8 + lane], 0xffffu);
    std::memcpy(constants + 12, position, 12); constants[15] = 0x20000000u;
    finalWords[0] = stored[0] | (stored[1] << 16);
    finalWords[1] = stored[2] | (stored[3] << 16);
    c.reset(); c.list->SetDescriptorHeaps(2, heaps); c.list->SetComputeRootSignature(root.Get()); c.list->SetPipelineState(pso.Get());
    c.list->SetComputeRootDescriptorTable(1, gpuView(210)); c.list->SetComputeRootDescriptorTable(3, gpuSampler(105));
    c.list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress()); c.list->SetComputeRoot32BitConstants(0, 16, constants, 0);
    transition(c.list.Get(), texture, anyShader, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.list->Dispatch(1, 1, 1); uavBarrier(c.list.Get());
    transition(c.list.Get(), texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, anyShader);
    transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    c.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 64);
    transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.execute("volume dual UAV single dispatch close"); c.finish("volume dual UAV single dispatch");
    Words data(readback);
    for (UINT lane = 0; lane < 4; ++lane) {
      const UINT before = (constants[4 + lane / 2] >> ((lane & 1) * 16)) & 0xffffu;
      expect("same-dispatch RG32 store -> RGBA16 read", data[lane], resident ? before : 0);
      expect("same-dispatch RGBA16 stored lane saturation", data[4 + lane], resident ? stored[lane] : 0);
    }
    expect("same-dispatch RGBA16 store -> RG32 read low", data[8], resident ? finalWords[0] : 0);
    expect("same-dispatch RGBA16 store -> RG32 read high", data[9], resident ? finalWords[1] : 0);
    expect("dual RG32 width", data[10], width); expect("dual RG32 height", data[11], height);
    expect("dual RG32 WSize", data[12], countW); expect("dual RGBA16 width", data[13], width);
    expect("dual RGBA16 height", data[14], height); expect("dual RGBA16 WSize", data[15], countW);
  };
  // A real ordinary 3D parent control: it must support the same two views coherently,
  // rather than assuming that equal byte size makes Metal format views compatible.
  ComPtr<ID3D12Device10> castDevice; check(c.device.As(&castDevice), "ordinary dual UAV device10");
  ComPtr<ID3D12Resource> nativeParent;
  D3D12_RESOURCE_DESC1 nativeDesc{}; nativeDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  nativeDesc.Width = 4; nativeDesc.Height = 3; nativeDesc.DepthOrArraySize = 4; nativeDesc.MipLevels = 1;
  nativeDesc.Format = DXGI_FORMAT_R32G32_UINT; nativeDesc.SampleDesc.Count = 1;
  nativeDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const auto nativeHeap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  DXGI_FORMAT nativeCasts[] = {DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R16G16B16A16_UINT};
  check(castDevice->CreateCommittedResource3(&nativeHeap, D3D12_HEAP_FLAG_NONE, &nativeDesc,
      D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 2, nativeCasts, IID_PPV_ARGS(&nativeParent)), "ordinary dual UAV native parent create");
  std::vector<BYTE> nativeBytes(4 * 3 * 4 * 8);
  const UINT guardWords[2] = {0xdeadbeefu, 0xab12cd34u};
  const auto nativeUpload = c.buffer(D3D12_HEAP_TYPE_UPLOAD, 256 * 3 * 4, "ordinary dual UAV upload");
  BYTE *nativeMapped = nullptr; check(nativeUpload->Map(0, nullptr, reinterpret_cast<void **>(&nativeMapped)), "ordinary dual upload map");
  std::memset(nativeMapped, 0xcc, 256 * 3 * 4);
  for (UINT z = 0; z < 4; ++z) for (UINT y = 0; y < 3; ++y) for (UINT x = 0; x < 4; ++x) {
    std::memcpy(nativeBytes.data() + 8 * (x + 4 * (y + 3 * z)), guardWords, 8);
    std::memcpy(nativeMapped + 8 * x + 256 * (y + 3 * z), guardWords, 8);
  }
  nativeUpload->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION nativeDestination{}; nativeDestination.pResource = nativeParent.Get();
  nativeDestination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION nativeSource{}; nativeSource.pResource = nativeUpload.Get();
  nativeSource.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  nativeSource.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32_UINT, 4, 3, 4, 256};
  c.reset(); transition(c.list.Get(), nativeParent.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
  c.list->CopyTextureRegion(&nativeDestination, 0, 0, 0, &nativeSource, nullptr);
  transition(c.list.Get(), nativeParent.Get(), D3D12_RESOURCE_STATE_COPY_DEST, anyShader);
  c.execute("ordinary dual UAV initialize close"); c.finish("ordinary dual UAV initialize");
  for (UINT api = 0; api < 2; ++api) {
    for (UINT i = 0; i < 6; ++i) {
      const UINT position[3] = {i == 4 ? 4u : 2u, i == 5 ? ~0u : 1u, i < 4 ? (i == 3 ? ~0u : i) : 0u};
      const bool resident = i < 2;
      UINT finalWords[2]; runDualUAV(nativeParent.Get(), 0, 1, 2, 4, 3, position, resident, api, i, finalWords);
      if (resident) std::memcpy(nativeBytes.data() + 8 * (position[0] + 4 * (position[1] + 3 * (position[2] + 1))), finalWords, 8);
    }
    nativeDestination = nativeSource; nativeDestination.pResource = readback.Get();
    nativeSource.pResource = nativeParent.Get(); nativeSource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    nativeSource.SubresourceIndex = 0;
    c.reset(); transition(c.list.Get(), nativeParent.Get(), anyShader, D3D12_RESOURCE_STATE_COPY_SOURCE);
    c.list->CopyTextureRegion(&nativeDestination, 0, 0, 0, &nativeSource, nullptr);
    transition(c.list.Get(), nativeParent.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, anyShader);
    c.execute("ordinary dual UAV parent bytes close"); c.finish("ordinary dual UAV parent bytes");
    Words data(readback);
    for (UINT z = 0; z < 4; ++z) for (UINT y = 0; y < 3; ++y) for (UINT x = 0; x < 4; ++x) for (UINT lane = 0; lane < 2; ++lane) {
      UINT expected; std::memcpy(&expected, nativeBytes.data() + 8 * (x + 4 * (y + 3 * z)) + lane * 4, 4);
      expect("ordinary dual UAV exact parent bytes and guards", data[2 * x + 64 * (y + 3 * z) + lane], expected);
    }
    pass(api ? "volume-dxbc-native-parent-RG32-RGBA16-same-dispatch-partialW-guards"
             : "volume-dxil-native-parent-RG32-RGBA16-same-dispatch-partialW-guards");
    nativeSource = nativeDestination; nativeSource.pResource = nativeUpload.Get();
  }
  const auto bcParentTileReadback = c.buffer(D3D12_HEAP_TYPE_READBACK, kTile, "BC parent exact tile bytes");
  auto checkPlainParent = [&](const VolumeResource &parent, UINT mip, UINT x, UINT y, UINT z, UINT api) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = parent.format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture3D = {mip, 1, float(mip)};
    device->CreateShaderResourceView(parent.texture.Get(), &srv, cpuView(215));
    const auto shader = readFile(paths[api * 5]); const auto pso = computePipeline(device, root.Get(), shader);
    VolumeCase test{}; test.words[0] = x < (parent.width >> mip) ? (x + 0.5f) / (parent.width >> mip) : 0.5f;
    test.words[1] = y < (parent.height >> mip) ? (y + 0.5f) / (parent.height >> mip) : 0.5f;
    test.words[2] = z < (parent.depth >> mip) ? (z + 0.5f) / (parent.depth >> mip) : 0.5f;
    test.integers[0] = x; test.integers[1] = y; test.integers[2] = z;
    c.reset(); c.list->SetDescriptorHeaps(2, heaps); c.list->SetComputeRootSignature(root.Get()); c.list->SetPipelineState(pso.Get());
    c.list->SetComputeRoot32BitConstants(0, 16, &test, 0); c.list->SetComputeRootDescriptorTable(1, gpuView(215));
    c.list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress()); c.list->SetComputeRootDescriptorTable(3, gpuSampler(105));
    c.list->Dispatch(1, 1, 1); uavBarrier(c.list.Get());
    transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    c.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 80);
    transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // Keep the native shader result and opaque 8/16-byte block proof in the
    // same submission. Clamp OOB probes to a real edge page to prove its
    // unchanged bytes too; NULL pages must copy as zeros.
    const UINT copyX = std::min(x, (parent.width >> mip) - 1);
    const UINT copyY = std::min(y, (parent.height >> mip) - 1);
    const UINT copyZ = std::min(z, (parent.depth >> mip) - 1);
    D3D12_TILED_RESOURCE_COORDINATE tile{copyX / parent.shape.WidthInTexels,
        copyY / parent.shape.HeightInTexels, copyZ / parent.shape.DepthInTexels, mip};
    D3D12_TILE_REGION_SIZE one{1, TRUE, 1, 1, 1};
    transition(c.list.Get(), parent.texture.Get(), anyShader, D3D12_RESOURCE_STATE_COPY_SOURCE);
    c.list->CopyTiles(parent.texture.Get(), &tile, &one, bcParentTileReadback.Get(), 0,
        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    transition(c.list.Get(), parent.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, anyShader);
    c.execute("volume native compressed parent decode close"); c.finish("volume native compressed parent decode");
    Words data(readback); const auto expected = parent.texel(mip, int(x), int(y), int(z));
    Words copied(bcParentTileReadback);
    const UINT logical = parent.tileAt(mip, copyX, copyY, copyZ);
    const int physical = parent.mapping[logical];
    for (UINT word = 0; word < kTile / 4; ++word) {
      UINT expectedWord = 0;
      if (physical >= 0) std::memcpy(&expectedWord, parent.bytes.data() + size_t(physical) * kTile + word * 4, 4);
      if (copied[word] != expectedWord) {
        std::fprintf(stderr, "VOLUME_BC_COPYTILES api=%u blockBytes=%u mip=%u xyz=%u,%u,%u tile=%u physical=%d word=%u actual=%08x expected=%08x\n",
            api, parent.unitBytes, mip, x, y, z, logical, physical, word, copied[word], expectedWord);
        fail("native BC parent exact 64KB bytes after typed plain store / NULL / OOB guard");
      }
    }
    volumeExpectColor(data, 8, expected, "native BC parent decode after typed plain store");
    expect("native BC parent residency after typed store", data[18], expected.mapped);
  };
  for (UINT api = 0; api < 2; ++api) {
    for (UINT mip = 0; mip < 2; ++mip) {
      const UINT w = (bc.width >> mip) / 4, h = (bc.height >> mip) / 4, d = bc.depth >> mip;
      const UINT firstW = mip == 1 ? 1u : 0u, countW = d - firstW - 1;
      for (UINT i = 0; i < 15; ++i) {
        UINT position[3] = {std::min(bc.shape.WidthInTexels / 4 - 1 + (i & 1), w - 1),
                            std::min(bc.shape.HeightInTexels / 4 - 1 + ((i >> 1) & 1), h - 1),
                            std::min(bc.shape.DepthInTexels - 1 + ((i >> 2) & 1), d - 2) - firstW};
        if (i == 8) { position[0] = position[1] = position[2] = 0; }
        if (i == 9) { position[0] = std::min(bc.shape.WidthInTexels / 4, w - 1); position[1] = position[2] = 0; }
        if (i == 10) position[2] = countW;
        if (i == 11) position[2] = ~0u;
        if (i == 12) position[0] = w;
        if (i == 13) position[1] = h;
        if (i == 14) position[0] = ~0u;
        const UINT x = position[0] * 4, y = position[1] * 4, z = position[2] + firstW;
        const bool inside = position[0] < w && position[1] < h && position[2] < countW;
        const bool resident = inside && bc.mapping[bc.tileAt(mip, x, y, z)] >= 0;
        UINT finalWords[2]; runDualUAV(bc.texture.Get(), mip, firstW, countW, w, h, position, resident, api, i + 16 * mip, finalWords);
        if (resident) std::memcpy(bc.bytes.data() + bc.address(mip, x, y, z), finalWords, 8);
        checkPlainParent(bc, mip, x, y, z, api);
        if (mip == 0 && i == 9) checkPlainParent(bc, mip, x, y + bc.shape.HeightInTexels, z, api);
      }
    }
    pass(api ? "volume-dxbc-BC1-RG32-RGBA16-same-dispatch-xyz-null-alias-mip1-partialW-guards-native-decode"
             : "volume-dxil-BC1-RG32-RGBA16-same-dispatch-xyz-null-alias-mip1-partialW-guards-native-decode");
  }
  bc.remap(c, 0, 0, 0, 0); c.finish("volume dual UAV NULL page remap proof");
  volumeCheckPhysicalBytes(c, bc, "volume-BC1-RG32-RGBA16-dual-UAV-same-dispatch-exact-64KB-NULL-remap");
  bc.remap(c, 0, 0, 0, -1); c.finish("volume dual UAV restore NULL page");
  for (UINT api = 0; api < 2; ++api) for (UINT mip = 0; mip < bc.mipCount; ++mip) {
    const auto shader = readFile(paths[api * 5 + 1]); const auto pso = computePipeline(device, root.Get(), shader);
    auto srv = nullBlockSRV; srv.Texture3D = {mip, 1, float(mip)};
    device->CreateShaderResourceView(bc.texture.Get(), &srv, cpuView(212));
    auto uav = nullBlockUAV; uav.Texture3D = {mip, 1, (bc.depth >> mip) - 1};
    device->CreateUnorderedAccessView(bc.texture.Get(), nullptr, &uav, cpuView(213));
    const UINT x = UINT(std::fmin(bc.shape.WidthInTexels / 4 - 1 + api, (bc.width >> mip) / 4 - 1));
    const UINT y = UINT(std::fmin(bc.shape.HeightInTexels / 4, (bc.height >> mip) / 4 - 1));
    const UINT z = UINT(std::fmin(bc.shape.DepthInTexels, (bc.depth >> mip) - 1));
    const size_t address = bc.address(mip, x * 4, y * 4, z);
    UINT before[2]; std::memcpy(before, bc.bytes.data() + address, 8);
    UINT constants[16]{}; constants[4] = api ? 0x07e0f800u : 0xf80007e0u; constants[5] = api ? 0x55555555u : 0xaaaaaaaau;
    const float firstW = 1; std::memcpy(constants + 11, &firstW, 4);
    constants[12] = x; constants[13] = y; constants[14] = z - 1; constants[15] = 0xc0000000u;
    c.reset(); c.list->SetDescriptorHeaps(2, heaps); c.list->SetComputeRootSignature(root.Get()); c.list->SetPipelineState(pso.Get());
    c.list->SetComputeRootDescriptorTable(1, gpuView(210)); c.list->SetComputeRootDescriptorTable(3, gpuSampler(105));
    c.list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress()); c.list->SetComputeRoot32BitConstants(0, 16, constants, 0);
    c.list->Dispatch(1, 1, 1); transition(c.list.Get(), bc.texture.Get(), anyShader, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    constants[15] = 0x40000000u; c.list->SetComputeRoot32BitConstants(0, 16, constants, 0); c.list->Dispatch(1, 1, 1);
    uavBarrier(c.list.Get()); transition(c.list.Get(), bc.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, anyShader);
    transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    c.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 48);
    transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.execute("volume BC/plain store close"); c.finish("volume BC/plain store");
    Words data(readback);
    for (UINT lane = 0; lane < 2; ++lane) {
      expect("BC/plain SRV exact block bytes", data[lane], before[lane]); expect("BC/plain UAV prior", data[4 + lane], before[lane]);
      expect("BC/plain UAV stored bytes", data[6 + lane], constants[4 + lane]);
    }
    expect("BC/plain residency", data[2], 1); expect("BC/plain width", data[8], (bc.width >> mip) / 4);
    expect("BC/plain height", data[9], (bc.height >> mip) / 4); expect("BC/plain partial WSize", data[10], (bc.depth >> mip) - 1);
    std::memcpy(bc.bytes.data() + address, constants + 4, 8);
  }
  pass("volume-dxil-dxbc-BC1-plain-RG32-load-store-partialW-all-mips");
  auto bcClearUAV = nullBlockUAV; bcClearUAV.Texture3D = {2, 1, (bc.depth >> 2) - 1};
  device->CreateUnorderedAccessView(bc.texture.Get(), nullptr, &bcClearUAV, cpuView(213));
  const UINT bcClear[4] = {0x07e0001fu, 0xe4e4e4e4u, 0x12345678u, 0x87654321u};
  c.reset(); c.list->SetDescriptorHeaps(2, heaps);
  transition(c.list.Get(), bc.texture.Get(), anyShader, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  c.list->ClearUnorderedAccessViewUint(gpuView(213), cpuView(213), bc.texture.Get(), bcClear, 0, nullptr);
  transition(c.list.Get(), bc.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, anyShader);
  c.execute("volume BC/plain clear close"); c.finish("volume BC/plain clear");
  for (UINT z = 1; z < (bc.depth >> 2); ++z) for (UINT y = 0; y < (bc.height >> 2); y += 4)
    for (UINT x = 0; x < (bc.width >> 2); x += 4)
      std::memcpy(bc.bytes.data() + bc.address(2, x, y, z), bcClear, 8);
  pass("volume-BC1-plain-RG32-UAV-clear-mip2-partialW");
  checkBCSamples(bc, bcTests, 0, "volume-dxil-BC1-decode-after-plain-store-clear-same-64KB");
  checkBCSamples(bc, bcTests, 1, "volume-dxbc-BC1-decode-after-plain-store-clear-same-64KB");
  volumeCheckPhysicalBytes(c, bc, "volume-BC1-plain-cast-exact-64KB-alias-byte-oracle");

  for (DXGI_FORMAT original : {DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC6H_UF16}) {
    VolumeResource signedBC(c, original, 4, original == DXGI_FORMAT_BC5_UNORM
        ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE, 256, 256, 64);
    signedBC.remap(c, 0, 0, 0, -1); signedBC.remap(c, 1, 0, 0, 3);
    std::vector<VolumeCase> signedTests;
    for (UINT sign = 0; sign < 2; ++sign) for (UINT mip = 0; mip < signedBC.mipCount; ++mip) for (UINT i = 0; i < 12; ++i) {
      VolumeCase test = tests[105]; test.signedDecode = sign != 0; test.firstMip = mip; test.mipCount = signedBC.mipCount - mip;
      const UINT w = signedBC.width >> mip, h = signedBC.height >> mip, d = signedBC.depth >> mip;
      const UINT x = UINT(std::fmin(i < 8 ? signedBC.shape.WidthInTexels - 1 + (i & 1) : signedBC.shape.WidthInTexels + 4 + (i & 3), w - 1));
      const UINT y = UINT(std::fmin(i < 8 ? signedBC.shape.HeightInTexels - 1 + ((i >> 1) & 1) : signedBC.shape.HeightInTexels + 5, h - 1));
      const UINT z = UINT(std::fmin(signedBC.shape.DepthInTexels - 1 + ((i >> 2) & 1), d - 1));
      test.words[0] = (x + 0.375f) / w; test.words[1] = (y + 0.375f) / h; test.words[2] = (z + 0.375f) / d;
      test.words[4] = 1.0f / w; test.words[9] = 1.0f / h;
      test.integers[0] = x; test.integers[1] = y; test.integers[2] = z; test.integers[3] = UINT(signedTests.size()) * 80;
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Format = !sign ? original : original == DXGI_FORMAT_BC4_UNORM ? DXGI_FORMAT_BC4_SNORM
          : original == DXGI_FORMAT_BC5_UNORM ? DXGI_FORMAT_BC5_SNORM : DXGI_FORMAT_BC6H_SF16;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; srv.Texture3D = {mip, test.mipCount, float(mip)};
      device->CreateShaderResourceView(signedBC.texture.Get(), &srv, cpuView(UINT(signedTests.size())));
      signedTests.push_back(test);
    }
    checkBCSamples(signedBC, signedTests, 0, original == DXGI_FORMAT_BC4_UNORM ? "volume-dxil-BC4-UNORM-SNORM-independent-decode-xyz-mips-null-alias"
        : original == DXGI_FORMAT_BC5_UNORM ? "volume-dxil-BC5-UNORM-SNORM-independent-decode-xyz-mips-null-alias"
        : "volume-dxil-BC6H-UF16-SF16-nonzero-mode11-independent-decode-xyz-mips-null-alias");
    checkBCSamples(signedBC, signedTests, 1, original == DXGI_FORMAT_BC4_UNORM ? "volume-dxbc-BC4-UNORM-SNORM-independent-decode-xyz-mips-null-alias"
        : original == DXGI_FORMAT_BC5_UNORM ? "volume-dxbc-BC5-UNORM-SNORM-independent-decode-xyz-mips-null-alias"
        : "volume-dxbc-BC6H-UF16-SF16-nonzero-mode11-independent-decode-xyz-mips-null-alias");
    if (original == DXGI_FORMAT_BC5_UNORM) {
      for (UINT api = 0; api < 2; ++api) {
        const auto shader = readFile(paths[api * 5 + 1]); const auto pso = computePipeline(device, root.Get(), shader);
        for (UINT mip = 0; mip < signedBC.mipCount; ++mip) {
          const UINT w = (signedBC.width >> mip) / 4, h = (signedBC.height >> mip) / 4, d = signedBC.depth >> mip;
          const UINT firstW = mip == 1 ? 1u : 0u, countW = d - firstW - 1;
          auto uav = nullWideUAV; uav.Format = DXGI_FORMAT_R32G32B32A32_UINT; uav.Texture3D = {mip, firstW, countW};
          device->CreateUnorderedAccessView(signedBC.texture.Get(), nullptr, &uav, cpuView(214));
          for (UINT i = 0; i < 15; ++i) {
            UINT position[3] = {std::min(signedBC.shape.WidthInTexels / 4 - 1 + (i & 1), w - 1),
                                std::min(signedBC.shape.HeightInTexels / 4 - 1 + ((i >> 1) & 1), h - 1),
                                std::min(signedBC.shape.DepthInTexels - 1 + ((i >> 2) & 1), d - 2) - firstW};
            if (i == 8) { position[0] = position[1] = position[2] = 0; }
            if (i == 9) { position[0] = std::min(signedBC.shape.WidthInTexels / 4, w - 1); position[1] = position[2] = 0; }
            if (i == 10) position[2] = countW;
            if (i == 11) position[2] = ~0u;
            if (i == 12) position[0] = w;
            if (i == 13) position[1] = h;
            if (i == 14) position[0] = ~0u;
            const UINT x = position[0] * 4, y = position[1] * 4, z = position[2] + firstW;
            const bool inside = position[0] < w && position[1] < h && position[2] < countW;
            const bool resident = inside && signedBC.mapping[signedBC.tileAt(mip, x, y, z)] >= 0;
            UINT before[4]{}; if (resident) std::memcpy(before, signedBC.bytes.data() + signedBC.address(mip, x, y, z), 16);
            UINT constants[16]{};
            for (UINT channel = 0; channel < 2; ++channel) {
              UINT64 encoded = (0x80u + api + channel) | (UINT64(0xc0u + (i + 7 * mip + channel) % 48) << 8);
              const UINT indices[4] = {0, 1, 6, 7};
              for (UINT pixel = 0; pixel < 16; ++pixel) encoded |= UINT64(indices[(pixel + i + channel) & 3]) << (16 + 3 * pixel);
              std::memcpy(constants + 4 + 2 * channel, &encoded, 8);
            }
            std::memcpy(constants + 12, position, 12); constants[15] = 0x10000000u;
            c.reset(); c.list->SetDescriptorHeaps(2, heaps); c.list->SetComputeRootSignature(root.Get()); c.list->SetPipelineState(pso.Get());
            c.list->SetComputeRootDescriptorTable(1, gpuView(210)); c.list->SetComputeRootDescriptorTable(3, gpuSampler(105));
            c.list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress()); c.list->SetComputeRoot32BitConstants(0, 16, constants, 0);
            transition(c.list.Get(), signedBC.texture.Get(), anyShader, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            c.list->Dispatch(1, 1, 1); uavBarrier(c.list.Get());
            transition(c.list.Get(), signedBC.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, anyShader);
            transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            c.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 48);
            transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            c.execute("volume BC5 RGBA32 plain store close"); c.finish("volume BC5 RGBA32 plain store");
            {
              Words data(readback);
              for (UINT lane = 0; lane < 4; ++lane) {
                expect("BC5/plain16 prior exact bytes", data[lane], before[lane]);
                expect("BC5/plain16 stored exact bytes", data[4 + lane], resident ? constants[4 + lane] : 0);
              }
              expect("BC5/plain16 block width", data[8], w); expect("BC5/plain16 block height", data[9], h);
              expect("BC5/plain16 partial WSize", data[10], countW);
            }
            if (resident) std::memcpy(signedBC.bytes.data() + signedBC.address(mip, x, y, z), constants + 4, 16);
            checkPlainParent(signedBC, mip, x, y, z, api);
            if (mip == 0 && i == 9) checkPlainParent(signedBC, mip, x, y + signedBC.shape.HeightInTexels, z, api);
          }
        }
        pass(api ? "volume-dxbc-BC5-plain16-RGBA32-load-store-xyz-null-alias-all-mips-partialW-guards-native-decode"
                 : "volume-dxil-BC5-plain16-RGBA32-load-store-xyz-null-alias-all-mips-partialW-guards-native-decode");
      }
      signedBC.remap(c, 0, 0, 0, 0); c.finish("volume BC5 plain16 NULL page remap proof");
      volumeCheckPhysicalBytes(c, signedBC, "volume-BC5-plain16-RGBA32-store-exact-64KB-NULL-remap-alias-guards");
      signedBC.remap(c, 0, 0, 0, -1); c.finish("volume BC5 plain16 restore NULL page");
      auto clearUAV = nullWideUAV; clearUAV.Format = DXGI_FORMAT_R32G32B32A32_UINT;
      clearUAV.Texture3D = {2, 1, (signedBC.depth >> 2) - 2};
      device->CreateUnorderedAccessView(signedBC.texture.Get(), nullptr, &clearUAV, cpuView(214));
      UINT clearWords[4]{};
      for (UINT channel = 0; channel < 2; ++channel) {
        UINT64 encoded = (0x81u + channel) | (UINT64(0xd7u + channel) << 8);
        const UINT indices[4] = {0, 1, 6, 7};
        for (UINT pixel = 0; pixel < 16; ++pixel) encoded |= UINT64(indices[(pixel + channel) & 3]) << (16 + 3 * pixel);
        std::memcpy(clearWords + 2 * channel, &encoded, 8);
      }
      c.reset(); c.list->SetDescriptorHeaps(2, heaps);
      transition(c.list.Get(), signedBC.texture.Get(), anyShader, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      c.list->ClearUnorderedAccessViewUint(gpuView(214), cpuView(214), signedBC.texture.Get(), clearWords, 0, nullptr);
      transition(c.list.Get(), signedBC.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, anyShader);
      c.execute("volume BC5 plain16 clear close"); c.finish("volume BC5 plain16 clear");
      for (UINT z = 1; z < (signedBC.depth >> 2) - 1; ++z) for (UINT y = 0; y < (signedBC.height >> 2); y += 4)
        for (UINT x = 0; x < (signedBC.width >> 2); x += 4)
          if (signedBC.mapping[signedBC.tileAt(2, x, y, z)] >= 0)
            std::memcpy(signedBC.bytes.data() + signedBC.address(2, x, y, z), clearWords, 16);
      checkBCSamples(signedBC, signedTests, 0, "volume-dxil-BC5-UNORM-SNORM-decode-after-plain16-store-clear");
      checkBCSamples(signedBC, signedTests, 1, "volume-dxbc-BC5-UNORM-SNORM-decode-after-plain16-store-clear");
      volumeCheckPhysicalBytes(c, signedBC, "volume-BC5-plain16-RGBA32-UAV-clear-mip2-partialW-exact-64KB");
    }
    volumeCheckPhysicalBytes(c, signedBC, original == DXGI_FORMAT_BC4_UNORM ? "volume-BC4-signed-cast-identical-64KB"
        : original == DXGI_FORMAT_BC5_UNORM ? "volume-BC5-signed-cast-identical-64KB" : "volume-BC6H-signed-cast-identical-64KB");
  }

  // CopyTextureRegion across all three logical tile boundaries, then clear/draw the 3D RTV shadow.
  D3D12_SHADER_RESOURCE_VIEW_DESC originalSRV{}; originalSRV.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  originalSRV.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; originalSRV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  originalSRV.Texture3D.MipLevels = 4;
  device->CreateShaderResourceView(volume.texture.Get(), &originalSRV, cpuView(105));
  const auto regionUpload = c.buffer(D3D12_HEAP_TYPE_UPLOAD, 1024, "volume region upload");
  BYTE *regionData = nullptr; check(regionUpload->Map(0, nullptr, reinterpret_cast<void **>(&regionData)), "volume region map");
  std::memset(regionData, 0, 1024);
  const UINT regionPixel = 0xff806020u;
  for (UINT z = 0; z < 2; ++z) for (UINT y = 0; y < 2; ++y) for (UINT x = 0; x < 2; ++x)
    std::memcpy(regionData + z * 512 + y * 256 + x * 4, &regionPixel, 4);
  regionUpload->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = regionUpload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  source.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, 2, 256};
  D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = volume.texture.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  c.reset(); transition(c.list.Get(), volume.texture.Get(), anyShader, D3D12_RESOURCE_STATE_COPY_DEST);
  c.list->CopyTextureRegion(&destination, volume.shape.WidthInTexels - 1, volume.shape.HeightInTexels - 1,
      volume.shape.DepthInTexels - 1, &source, nullptr);
  transition(c.list.Get(), volume.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, anyShader);
  c.execute("volume region close"); c.finish("volume region");
  for (UINT z = 0; z < 2; ++z) for (UINT y = 0; y < 2; ++y) for (UINT x = 0; x < 2; ++x) {
    const UINT gx = volume.shape.WidthInTexels - 1 + x, gy = volume.shape.HeightInTexels - 1 + y, gz = volume.shape.DepthInTexels - 1 + z;
    if (volume.mapping[volume.tileAt(0, gx, gy, gz)] >= 0) std::memcpy(volume.bytes.data() + volume.address(0, gx, gy, gz), &regionPixel, 4);
  }
  {
    const auto shader = readFile(paths[0]); const auto pso = computePipeline(device, root.Get(), shader);
    c.reset(); c.list->SetDescriptorHeaps(2, heaps); c.list->SetComputeRootSignature(root.Get()); c.list->SetPipelineState(pso.Get());
    c.list->SetComputeRootDescriptorTable(1, gpuView(105)); c.list->SetComputeRootDescriptorTable(3, gpuSampler(105));
    c.list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
    for (UINT i = 0; i < 8; ++i) {
      UINT constants[16]{}; std::memcpy(constants, tests[105].words, 48);
      constants[12] = volume.shape.WidthInTexels - 1 + (i & 1);
      constants[13] = volume.shape.HeightInTexels - 1 + ((i >> 1) & 1);
      constants[14] = volume.shape.DepthInTexels - 1 + ((i >> 2) & 1); constants[15] = i * 80;
      c.list->SetComputeRoot32BitConstants(0, 16, constants, 0); c.list->Dispatch(1, 1, 1);
    }
    uavBarrier(c.list.Get()); transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    c.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 8 * 80);
    transition(c.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.execute("volume region oracle close"); c.finish("volume region oracle");
    Words data(readback);
    for (UINT i = 0; i < 8; ++i) {
      const auto value = volume.texel(0, int(volume.shape.WidthInTexels - 1 + (i & 1)),
          int(volume.shape.HeightInTexels - 1 + ((i >> 1) & 1)), int(volume.shape.DepthInTexels - 1 + ((i >> 2) & 1)));
      volumeExpectColor(data, i * 20 + 8, value, "volume CopyTextureRegion xyz boundary");
      expect("volume copied pixel residency", data[i * 20 + 18], value.mapped);
    }
  }
  pass("volume-copyregion-three-axis-boundary-null-alias");
  c.reset(); transition(c.list.Get(), volume.texture.Get(), anyShader, D3D12_RESOURCE_STATE_RENDER_TARGET);
  D3D12_RENDER_TARGET_VIEW_DESC volumeRTVDesc{};
  volumeRTVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; volumeRTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
  volumeRTVDesc.Texture3D = {0, volume.shape.DepthInTexels - 1, 2};
  device->CreateRenderTargetView(volume.texture.Get(), &volumeRTVDesc, volumeRTV);
  const float clear[4] = {32.0f / 255, 64.0f / 255, 128.0f / 255, 1};
  c.list->ClearRenderTargetView(volumeRTV, clear, 0, nullptr);
  transition(c.list.Get(), volume.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, anyShader);
  c.execute("volume RTV clear close"); c.finish("volume RTV clear");
  const UINT clearPixel = 0xff804020u;
  for (UINT z = volume.shape.DepthInTexels - 1; z <= volume.shape.DepthInTexels; ++z)
    for (UINT y = 0; y < 64; ++y) for (UINT x = 0; x < 64; ++x)
      if (volume.mapping[volume.tileAt(0, x, y, z)] >= 0) std::memcpy(volume.bytes.data() + volume.address(0, x, y, z), &clearPixel, 4);

  volumeRTVDesc.Texture3D = {0, volume.shape.DepthInTexels + 1, 1};
  device->CreateRenderTargetView(volume.texture.Get(), &volumeRTVDesc, volumeRTV);
  for (UINT api = 0; api < 2; ++api) {
    const auto vs = readFile(paths[api * 5 + 2]), ps = readFile(paths[api * 5 + 4]);
    const auto pso = graphics(vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM);
    const float color[4] = {(48.0f + api * 16) / 255, 96.0f / 255, 160.0f / 255, 1};
    UINT constants[16]{}; std::memcpy(constants, color, 16);
    c.reset(); transition(c.list.Get(), volume.texture.Get(), anyShader, D3D12_RESOURCE_STATE_RENDER_TARGET);
    c.list->SetDescriptorHeaps(2, heaps); c.list->SetGraphicsRootSignature(root.Get()); c.list->SetPipelineState(pso.Get());
    c.list->SetGraphicsRoot32BitConstants(0, 16, constants, 0); c.list->SetGraphicsRootDescriptorTable(1, gpuView(105));
    c.list->SetGraphicsRootUnorderedAccessView(2, output->GetGPUVirtualAddress()); c.list->SetGraphicsRootDescriptorTable(3, gpuSampler(105));
    c.list->OMSetRenderTargets(1, &volumeRTV, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, 64, 64, 0, 1}; const D3D12_RECT scissor{0, 0, 64, 64};
    c.list->RSSetViewports(1, &viewport); c.list->RSSetScissorRects(1, &scissor); c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c.list->DrawInstanced(3, 1, 0, 0);
    transition(c.list.Get(), volume.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, anyShader);
    c.execute("volume RTV draw close"); c.finish("volume RTV draw");
    const UINT packedColor = (48u + api * 16) | (96u << 8) | (160u << 16) | 0xff000000u;
    for (UINT y = 0; y < 64; ++y) for (UINT x = 0; x < 64; ++x) {
      const UINT z = volume.shape.DepthInTexels + 1;
      if (volume.mapping[volume.tileAt(0, x, y, z)] >= 0) std::memcpy(volume.bytes.data() + volume.address(0, x, y, z), &packedColor, 4);
    }
  }

  // Raw CopyTiles readback checks complete 64KB physical pages, including padding and aliases.
  volumeCheckPhysicalBytes(c, volume, "volume-copyregion-RTV-clear-draw-64KB-alias-byte-oracle");

  // Reuse the same two-slice RTV through a partial unmap, a second queue
  // consuming that mapping, and a remap. Compare full physical pages, including
  // aliases and bytes outside the W range.
  {
    const auto original = volume.mapping;
    auto primary = c.queue;
    ComPtr<ID3D12CommandQueue> other;
    D3D12_COMMAND_QUEUE_DESC desc{};
    check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&other)), "auxiliary sparse direct queue");
    volumeRTVDesc.Texture3D = {0, volume.shape.DepthInTexels - 1, 2};
    device->CreateRenderTargetView(volume.texture.Get(), &volumeRTVDesc, volumeRTV);
    const auto &tiling = volume.tiling[0];
    for (unsigned phase = 0; phase != 3; ++phase) {
      if (phase != 1) {
        for (UINT y = 0; y < tiling.HeightInTiles; ++y)
          for (UINT x = 0; x < tiling.WidthInTiles; ++x) {
            const auto tile = volume.tileAt(0, x * volume.shape.WidthInTexels, y * volume.shape.HeightInTexels,
                                            volume.shape.DepthInTexels);
            volume.remap(c, x, y, 1, phase == 0 ? -1 : original[tile]);
          }
      }
      const auto producer = c.queue;
      check(producer->Signal(c.fence.Get(), ++c.value), "auxiliary sparse producer");
      c.queue = producer.Get() == primary.Get() ? other : primary;
      check(c.queue->Wait(c.fence.Get(), c.value), "auxiliary sparse consumer");
      const UINT red = 35 + phase * 23;
      const float color[] = {float(red) / 255.0f, 0, 1, 1};
      c.reset();
      transition(c.list.Get(), volume.texture.Get(), anyShader, D3D12_RESOURCE_STATE_RENDER_TARGET);
      c.list->ClearRenderTargetView(volumeRTV, color, 0, nullptr);
      transition(c.list.Get(), volume.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, anyShader);
      c.execute("auxiliary sparse clear");
      const UINT pixel = 0xffff0000u | red;
      for (UINT z = volume.shape.DepthInTexels - 1; z <= volume.shape.DepthInTexels; ++z)
        for (UINT y = 0; y < 64; ++y) for (UINT x = 0; x < 64; ++x)
          if (volume.mapping[volume.tileAt(0, x, y, z)] >= 0)
            std::memcpy(volume.bytes.data() + volume.address(0, x, y, z), &pixel, sizeof(pixel));
    }
    check(primary->Wait(c.fence.Get(), c.value), "auxiliary sparse return to primary");
    c.queue = primary;
    volumeCheckPhysicalBytes(c, volume, "volume-auxiliary-remap-cross-queue-64KB");
  }

  VolumeResource mappings(c, DXGI_FORMAT_R8G8B8A8_TYPELESS, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  require(mappings.tiling[0].WidthInTiles == 2 && mappings.tiling[0].HeightInTiles == 2 && mappings.tiling[0].DepthInTiles == 2,
      "volume official RGBA8 32x32x16 mapping fixture");
  D3D12_COMMAND_QUEUE_DESC computeDesc{}; computeDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
  ComPtr<ID3D12CommandQueue> mappingQueue; check(device->CreateCommandQueue(&computeDesc, IID_PPV_ARGS(&mappingQueue)), "volume compute mapping queue");
  ComPtr<ID3D12Fence> mappingFence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mappingFence)), "volume mapping timeline");
  check(c.queue->Signal(mappingFence.Get(), 1), "volume producer signal"); check(mappingQueue->Wait(mappingFence.Get(), 1), "volume mapping producer wait");
  D3D12_TILED_RESOURCE_COORDINATE linearStart{1, 1, 1, 0}; D3D12_TILE_REGION_SIZE linearRegion{2, FALSE, 0, 0, 0};
  D3D12_TILE_RANGE_FLAGS regular = D3D12_TILE_RANGE_FLAG_NONE; UINT offset = 5, count = 2;
  mappingQueue->UpdateTileMappings(mappings.texture.Get(), 1, &linearStart, &linearRegion, mappings.heap.Get(),
      1, &regular, &offset, &count, D3D12_TILE_MAPPING_FLAG_NONE);
  const UINT finalBaseTile = mappings.tileAt(0, mappings.shape.WidthInTexels, mappings.shape.HeightInTexels, mappings.shape.DepthInTexels);
  require(finalBaseTile + 1 == mappings.tiling[1].StartTileIndexInOverallResource, "volume linear mip boundary");
  mappings.mapping[finalBaseTile] = 5; mappings.mapping[finalBaseTile + 1] = 6;
  D3D12_TILED_RESOURCE_COORDINATE boxStart{}; D3D12_TILE_REGION_SIZE boxRegion{4, TRUE, 2, 2, 1}; offset = 2; count = 4;
  mappingQueue->UpdateTileMappings(mappings.texture.Get(), 1, &boxStart, &boxRegion, mappings.heap.Get(),
      1, &regular, &offset, &count, D3D12_TILE_MAPPING_FLAG_NONE);
  for (UINT tile = 0; tile < 4; ++tile) mappings.mapping[tile] = int(2 + tile);
  boxStart.Z = 1;
  D3D12_TILE_RANGE_FLAGS special[] = {D3D12_TILE_RANGE_FLAG_SKIP, D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE, D3D12_TILE_RANGE_FLAG_NULL};
  UINT specialOffsets[] = {0, 2, 0}, specialCounts[] = {1, 2, 1};
  mappingQueue->UpdateTileMappings(mappings.texture.Get(), 1, &boxStart, &boxRegion, mappings.heap.Get(),
      3, special, specialOffsets, specialCounts, D3D12_TILE_MAPPING_FLAG_NONE);
  mappings.mapping[5] = mappings.mapping[6] = 2; mappings.mapping[7] = -1;
  D3D12_TILED_RESOURCE_COORDINATE aliasDestination{1, 0, 0, 0}, aliasSource{1, 1, 0, 0}; D3D12_TILE_REGION_SIZE one{1, TRUE, 1, 1, 1};
  mappingQueue->CopyTileMappings(mappings.texture.Get(), &aliasDestination, mappings.texture.Get(), &aliasSource, &one, D3D12_TILE_MAPPING_FLAG_NONE);
  mappings.mapping[1] = mappings.mapping[3];
  check(mappingQueue->Signal(mappingFence.Get(), 2), "volume mapping queue signal"); check(c.queue->Wait(mappingFence.Get(), 2), "volume mapping consumer wait");
  volumeCheckPhysicalBytes(c, mappings, "volume-xyz-box-linear-mip-SKIP-REUSE-CopyTileMappings-cross-queue");

  const auto nullWrite = c.buffer(D3D12_HEAP_TYPE_UPLOAD, kTile, "volume NULL page upload");
  void *nullBytes = nullptr; check(nullWrite->Map(0, nullptr, &nullBytes), "volume NULL upload map");
  std::memset(nullBytes, 0x5a, kTile); nullWrite->Unmap(0, nullptr);
  c.reset(); transition(c.list.Get(), mappings.texture.Get(), anyShader, D3D12_RESOURCE_STATE_COPY_DEST);
  c.list->CopyTiles(mappings.texture.Get(), &linearStart, &one, nullWrite.Get(), 0, D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
  transition(c.list.Get(), mappings.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, anyShader);
  c.execute("volume NULL write close"); c.finish("volume NULL write");
  offset = 5; count = 1;
  mappingQueue->UpdateTileMappings(mappings.texture.Get(), 1, &linearStart, &one, mappings.heap.Get(),
      1, &regular, &offset, &count, D3D12_TILE_MAPPING_FLAG_NONE);
  mappings.mapping[7] = 5;
  check(mappingQueue->Signal(mappingFence.Get(), 3), "volume remap signal"); check(c.queue->Wait(mappingFence.Get(), 3), "volume remap consumer wait");
  volumeCheckPhysicalBytes(c, mappings, "volume-NULL-write-remap-preserves-physical-64KB");

  VolumeResource copied(c, DXGI_FORMAT_R8G8B8A8_TYPELESS, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  const auto predicate = c.buffer(D3D12_HEAP_TYPE_UPLOAD, 8, "volume copy predicate");
  UINT64 *predicateWord = nullptr; check(predicate->Map(0, nullptr, reinterpret_cast<void **>(&predicateWord)), "volume predicate map");
  *predicateWord = 1; predicate->Unmap(0, nullptr);
  // D3D12 skips when the predicate matches the operation: nonzero skips
  // NOT_EQUAL_ZERO and executes EQUAL_ZERO. Preserve both full-byte oracles.
  c.reset(); transition(c.list.Get(), mappings.texture.Get(), anyShader, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(c.list.Get(), copied.texture.Get(), anyShader, D3D12_RESOURCE_STATE_COPY_DEST);
  c.list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
  c.list->CopyResource(copied.texture.Get(), mappings.texture.Get());
  c.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  transition(c.list.Get(), mappings.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, anyShader);
  transition(c.list.Get(), copied.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, anyShader);
  c.execute("volume predicated skipped copy close"); c.finish("volume predicated skipped copy");
  volumeCheckPhysicalBytes(c, copied, "volume-predicated-CopyResource-skip-all-mips");
  c.reset(); transition(c.list.Get(), mappings.texture.Get(), anyShader, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(c.list.Get(), copied.texture.Get(), anyShader, D3D12_RESOURCE_STATE_COPY_DEST);
  c.list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  c.list->CopyResource(copied.texture.Get(), mappings.texture.Get());
  c.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  transition(c.list.Get(), mappings.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, anyShader);
  transition(c.list.Get(), copied.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, anyShader);
  c.execute("volume predicated copy close"); c.finish("volume predicated copy");
  for (UINT mip = 0; mip < mappings.mipCount; ++mip) for (UINT z = 0; z < (mappings.depth >> mip); ++z)
    for (UINT y = 0; y < (mappings.height >> mip); ++y) for (UINT x = 0; x < (mappings.width >> mip); ++x) {
      BYTE pixel[4]{};
      if (mappings.mapping[mappings.tileAt(mip, x, y, z)] >= 0) std::memcpy(pixel, mappings.bytes.data() + mappings.address(mip, x, y, z), 4);
      std::memcpy(copied.bytes.data() + copied.address(mip, x, y, z), pixel, 4);
    }
  volumeCheckPhysicalBytes(c, copied, "volume-predicated-CopyResource-execute-all-mips-64KB-padding");
  for (UINT execute = 0; execute < 2; ++execute) {
    destination.pResource = copied.texture.Get();
    c.reset(); transition(c.list.Get(), copied.texture.Get(), anyShader, D3D12_RESOURCE_STATE_COPY_DEST);
    c.list->SetPredication(predicate.Get(), 0, execute ? D3D12_PREDICATION_OP_EQUAL_ZERO : D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
    c.list->CopyTextureRegion(&destination, copied.shape.WidthInTexels - 1, copied.shape.HeightInTexels - 1, copied.shape.DepthInTexels - 1, &source, nullptr);
    c.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    transition(c.list.Get(), copied.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, anyShader);
    c.execute("volume predicated region close"); c.finish("volume predicated region");
    if (execute) for (UINT z = 0; z < 2; ++z) for (UINT y = 0; y < 2; ++y) for (UINT x = 0; x < 2; ++x)
      std::memcpy(copied.bytes.data() + copied.address(0, copied.shape.WidthInTexels - 1 + x,
          copied.shape.HeightInTexels - 1 + y, copied.shape.DepthInTexels - 1 + z), &regionPixel, 4);
    volumeCheckPhysicalBytes(c, copied, execute ? "volume-predicated-CopyTextureRegion-execute-xyz" : "volume-predicated-CopyTextureRegion-skip-xyz");
  }

  auto mipRTV = volumeRTVDesc; mipRTV.Texture3D = {1, 1, 2};
  device->CreateRenderTargetView(mappings.texture.Get(), &mipRTV, volumeRTV);
  c.reset(); transition(c.list.Get(), mappings.texture.Get(), anyShader, D3D12_RESOURCE_STATE_RENDER_TARGET);
  c.list->ClearRenderTargetView(volumeRTV, clear, 0, nullptr);
  transition(c.list.Get(), mappings.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, anyShader);
  c.execute("volume mip1 RTV clear close"); c.finish("volume mip1 RTV clear");
  for (UINT z = 1; z < 3; ++z) for (UINT y = 0; y < 32; ++y) for (UINT x = 0; x < 32; ++x)
    std::memcpy(mappings.bytes.data() + mappings.address(1, x, y, z), &clearPixel, 4);
  mipRTV.Texture3D = {1, 3, 1}; device->CreateRenderTargetView(mappings.texture.Get(), &mipRTV, volumeRTV);
  for (UINT api = 0; api < 2; ++api) {
    const auto vs = readFile(paths[api * 5 + 2]), ps = readFile(paths[api * 5 + 4]);
    const auto pso = graphics(vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM);
    const float color[4] = {(96.0f + api * 16) / 255, 32.0f / 255, 224.0f / 255, 1}; UINT constants[16]{};
    std::memcpy(constants, color, 16);
    c.reset(); transition(c.list.Get(), mappings.texture.Get(), anyShader, D3D12_RESOURCE_STATE_RENDER_TARGET);
    c.list->SetDescriptorHeaps(2, heaps); c.list->SetGraphicsRootSignature(root.Get()); c.list->SetPipelineState(pso.Get());
    c.list->SetGraphicsRoot32BitConstants(0, 16, constants, 0); c.list->OMSetRenderTargets(1, &volumeRTV, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, 32, 32, 0, 1}; const D3D12_RECT scissor{0, 0, 32, 32};
    c.list->RSSetViewports(1, &viewport); c.list->RSSetScissorRects(1, &scissor); c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c.list->DrawInstanced(3, 1, 0, 0); transition(c.list.Get(), mappings.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, anyShader);
    c.execute("volume mip1 RTV draw close"); c.finish("volume mip1 RTV draw");
    const UINT pixel = (96u + api * 16) | (32u << 8) | (224u << 16) | 0xff000000u;
    for (UINT y = 0; y < 32; ++y) for (UINT x = 0; x < 32; ++x) std::memcpy(mappings.bytes.data() + mappings.address(1, x, y, 3), &pixel, 4);
  }
  volumeCheckPhysicalBytes(c, mappings, "volume-mip1-partialW-RTV-clear-dxbc-dxil-draw-64KB-alias");

  // A pipeline-statistics query selects the VS capture frontier without a
  // shader change. Sixty-five instances require resumed native submissions
  // which keep the same sparse partial-W render view and its backing heaps.
  D3D12_QUERY_HEAP_DESC queryDesc{};
  queryDesc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
  queryDesc.Count = 1;
  ComPtr<ID3D12QueryHeap> queryHeap;
  check(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queryHeap)), "volume frontier query heap");
  const auto queryReadback = c.buffer(D3D12_HEAP_TYPE_READBACK,
      sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS), "volume frontier query readback");
  D3D12_INDIRECT_ARGUMENT_DESC argumentDesc{};
  argumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
  D3D12_COMMAND_SIGNATURE_DESC signatureDesc{};
  signatureDesc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
  signatureDesc.NumArgumentDescs = 1;
  signatureDesc.pArgumentDescs = &argumentDesc;
  ComPtr<ID3D12CommandSignature> signature;
  check(device->CreateCommandSignature(&signatureDesc, nullptr, IID_PPV_ARGS(&signature)),
        "volume frontier command signature");
  const D3D12_DRAW_ARGUMENTS arguments{3, 65, 0, 0};
  const auto argumentUpload = c.buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof(arguments), "volume frontier argument upload");
  const auto argumentBuffer = c.committed(D3D12_HEAP_TYPE_DEFAULT, bufferDesc(sizeof(arguments)),
      D3D12_RESOURCE_STATE_COPY_DEST, "volume frontier GPU arguments");
  void *argumentBytes = nullptr;
  check(argumentUpload->Map(0, nullptr, &argumentBytes), "volume frontier argument map");
  std::memcpy(argumentBytes, &arguments, sizeof(arguments));
  argumentUpload->Unmap(0, nullptr);
  c.reset();
  c.list->CopyBufferRegion(argumentBuffer.Get(), 0, argumentUpload.Get(), 0, sizeof(arguments));
  transition(c.list.Get(), argumentBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
  c.execute("volume frontier argument copy close"); c.finish("volume frontier argument copy");
  for (UINT api = 0; api < 2; ++api) {
    const char *shader = api ? "dxbc" : "dxil";
    const auto vs = readFile(paths[api * 5 + 2]), ps = readFile(paths[api * 5 + 4]);
    const auto pso = graphics(vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM);
    const float color[4] = {(160.0f + api * 16) / 255, 64.0f / 255, 192.0f / 255, 1};
    UINT constants[16]{};
    std::memcpy(constants, color, sizeof(color));
    std::fprintf(stderr, "OWNED_D3D12_CAPTURE_FRONTIER_PHASE suite=tiled shader=%s "
        "case=volume-frontier-partial-w-residency begin\n", shader);
    std::fflush(stderr);
    c.reset(); transition(c.list.Get(), mappings.texture.Get(), anyShader, D3D12_RESOURCE_STATE_RENDER_TARGET);
    c.list->SetDescriptorHeaps(2, heaps); c.list->SetGraphicsRootSignature(root.Get()); c.list->SetPipelineState(pso.Get());
    c.list->SetGraphicsRoot32BitConstants(0, 16, constants, 0); c.list->OMSetRenderTargets(1, &volumeRTV, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, 32, 32, 0, 1}; const D3D12_RECT scissor{0, 0, 32, 32};
    c.list->RSSetViewports(1, &viewport); c.list->RSSetScissorRects(1, &scissor);
    c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c.list->BeginQuery(queryHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    c.list->ExecuteIndirect(signature.Get(), 1, argumentBuffer.Get(), 0, nullptr, 0);
    c.list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    c.list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, queryReadback.Get(), 0);
    transition(c.list.Get(), mappings.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, anyShader);
    c.execute("volume frontier partial-W RTV close"); c.finish("volume frontier partial-W RTV");
    void *queryBytes = nullptr;
    const D3D12_RANGE queryRange{0, sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)};
    check(queryReadback->Map(0, &queryRange, &queryBytes), "volume frontier query map");
    D3D12_QUERY_DATA_PIPELINE_STATISTICS statistics{};
    std::memcpy(&statistics, queryBytes, sizeof(statistics));
    const D3D12_RANGE noWrites{0, 0};
    queryReadback->Unmap(0, &noWrites);
    std::printf("OWNED_D3D12_TILED_FRONTIER_STATS shader=%s ia_vertices=%llu ia_primitives=%llu "
        "vs_invocations=%llu instances=65\n", shader,
        static_cast<unsigned long long>(statistics.IAVertices),
        static_cast<unsigned long long>(statistics.IAPrimitives),
        static_cast<unsigned long long>(statistics.VSInvocations));
    std::fflush(stdout);
    require(statistics.IAVertices == 195 && statistics.IAPrimitives == 65 && statistics.VSInvocations == 195,
        "volume frontier exact IA/VS invocations");
    std::fprintf(stderr, "OWNED_D3D12_CAPTURE_FRONTIER_PHASE suite=tiled shader=%s "
        "case=volume-frontier-partial-w-residency end\n", shader);
    std::fflush(stderr);
    const UINT pixel = (160u + api * 16) | (64u << 8) | (192u << 16) | 0xff000000u;
    for (UINT y = 0; y < 32; ++y) for (UINT x = 0; x < 32; ++x)
      std::memcpy(mappings.bytes.data() + mappings.address(1, x, y, 3), &pixel, sizeof(pixel));
    volumeCheckPhysicalBytes(c, mappings, api ? "volume-dxbc-frontier-partial-w-RTV-residency"
                                            : "volume-dxil-frontier-partial-w-RTV-residency");
  }
}
