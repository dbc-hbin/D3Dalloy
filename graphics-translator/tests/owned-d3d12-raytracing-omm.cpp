// Independent OMM fixture. Reuses fence/readback/SBT plumbing, not the runtime classifier.
#define wmain raytracingBaseMain
#include "owned-d3d12-raytracing.cpp"
#undef wmain
#include "../../src/d3d12/d3d12_interfaces_ext.hpp"
#include <span>

namespace {
namespace omm {
using namespace contract;
struct Cell { float x[3], y[3]; };
ComPtr<ID3D12Resource> detachedUpload(ID3D12Device *device, const void *data, UINT64 bytes) {
  auto resource = createBuffer(device, std::max<UINT64>(bytes, 256), D3D12_HEAP_TYPE_UPLOAD,
      D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  std::memcpy(mapUpload(resource.Get()), data, bytes);
  resource->Unmap(0, nullptr);
  return resource;
}
std::vector<uint8_t> readBytes(Runner &geometry, ID3D12Resource *source, UINT64 bytes) {
  auto readback = createBuffer(geometry.device(), bytes, D3D12_HEAP_TYPE_READBACK,
      D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  transition(geometry.ctx().list.Get(), source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  geometry.ctx().list->CopyBufferRegion(readback.Get(), 0, source, 0, bytes);
  transition(geometry.ctx().list.Get(), source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  geometry.submit("omm: byte readback");
  void *mapped; D3D12_RANGE range{0, SIZE_T(bytes)};
  check(readback->Map(0, &range, &mapped), "omm: map readback");
  std::vector<uint8_t> result(bytes); std::memcpy(result.data(), mapped, bytes);
  D3D12_RANGE none{}; readback->Unmap(0, &none);
  return result;
}

// Decode bird-curve digits into a physical microtriangle. The runtime performs
// the opposite operation (barycentrics -> packed index); this oracle builds real
// ordinary geometry from these vertices and compares fence-completed dispatches.
Cell cell(uint32_t index, uint32_t level) {
  uint32_t x = 0, y = 0, z = 0;
  bool flip = false, turn = false;
  for (int bit = int(level) - 1; bit >= 0; --bit) {
    const bool e = (index >> (2 * bit)) & 1, o = (index >> (2 * bit + 1)) & 1;
    flip = flip != e;
    turn = turn != (e && !o);
    const bool t = turn != o;
    x |= uint32_t(t ? !e && !flip : flip || e) << bit;
    y |= uint32_t(turn != e) << bit;
    z |= uint32_t(t ? !e && flip : !flip || e) << bit;
  }
  const float scale = float(1u << level);
  const bool inverted = x + y + z + 2 == (1u << level);
  Cell c{{float(x),float(x + 1),float(x)}, {float(y),float(y),float(y + 1)}};
  if (inverted) c = {{float(x + 1),float(x),float(x + 1)}, {float(y + 1),float(y + 1),float(y)}};
  for (UINT i = 0; i < 3; ++i) { c.x[i] /= scale; c.y[i] /= scale; }
  return c;
}
D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS abi(const dxmt::OmmBuildInputs &in) {
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS result;
  std::memcpy(&result, &in, sizeof(result));
  return result;
}
ComPtr<ID3D12Resource> scene(Runner &geometry, ID3D12Resource *blas, UINT flags = 0) {
  const auto xf = translateScale(1, 0, 0, 0);
  auto instance = instanceDesc(xf.m, 0, 255, 0, flags, blas->GetGPUVirtualAddress());
  auto upload = geometry.upload(&instance, sizeof(instance));
  return geometry.build(tlasInputs(upload->GetGPUVirtualAddress(), 1), "omm: TLAS");
}
int execute(ID3D12Device5 *device, Context &ctx, const std::vector<uint8_t> &contractDxil,
            const std::vector<uint8_t> &dxil, bool legacy = false) {
  const UINT resultWords[] = {0,2,3,6,4,5};
  const std::span<const UINT> checkedWords(resultWords, legacy ? 4 : 6);
  Runner geometry(device, ctx, contractDxil);
  stateobj::Runner dispatch(device, ctx, dxil, geometry);
  auto library = dispatch.library();
  D3D12_RAYTRACING_PIPELINE_CONFIG1 config{1, D3D12_RAYTRACING_PIPELINE_FLAGS(D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS)};
  ComPtr<ID3D12StateObject> object;
  check(dispatch.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
                        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1, &config}}, object), "omm: pipeline");
  dispatch.raygen(object.Get(), L"OmmRayGen");
  dispatch.miss(0, object.Get(), L"OmmMiss", 0);
  dispatch.hitGroup(0, object.Get(), L"OmmGroup", 0);
  dispatch.hitGroup(1, object.Get(), L"OmmGroup", 0); // Independent oracle has opaque and unknown geometries.
  const float macro[] = {0,0,0, 1,0,0, 0,1,0};
  auto vertices = geometry.upload(macro, sizeof(macro));
  D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC triangles{};
  triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
  triangles.VertexCount = 3;
  triangles.VertexBuffer = {vertices->GetGPUVirtualAddress(), 12};
  uint64_t comparisons = 0;
  int bad = 0;
  for (UINT format = 1; format <= 2; ++format) for (UINT level = 0; level <= 12; ++level) {
    if (legacy && (format != 2 || level != 1)) continue;
    const UINT count = 1u << (2 * level), states = 1u << format;
    std::vector<uint8_t> packed(std::max(1u, (count * format + 7) / 8), 0);
    for (UINT i = 0; i < count; ++i) {
      const UINT state = ((i * 2654435761u) ^ (i >> 7) ^ (i >> 13)) >> 28 & (states - 1);
      packed[i * format / 8] |= uint8_t(state << (i * format % 8));
    }
    // Padding and nonzero ByteOffset verify the input descriptor is not treated as a packed stream.
    packed.insert(packed.begin(), 37, 0xa5);
    auto data = detachedUpload(device, packed.data(), packed.size());
    D3D12_RAYTRACING_OPACITY_MICROMAP_DESC desc{};
    desc.ByteOffset = 37; desc.SubdivisionLevel = level;
    desc.Format = D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT(format);
    auto descs = detachedUpload(device, &desc, sizeof(desc));
    ComPtr<ID3D12Resource> stagingData, stagingDescs;
    if (format == 2 && level == 1) {
      // No host wait: descriptor readback must follow these GPU producers in
      // command-list order, before the subsequently recorded array build.
      stagingData = data; stagingDescs = descs;
      data = createBuffer(device, stagingData->GetDesc().Width, D3D12_HEAP_TYPE_DEFAULT,
          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
      descs = createBuffer(device, stagingDescs->GetDesc().Width, D3D12_HEAP_TYPE_DEFAULT,
          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
      ctx.list->CopyBufferRegion(data.Get(), 0, stagingData.Get(), 0, packed.size());
      ctx.list->CopyBufferRegion(descs.Get(), 0, stagingDescs.Get(), 0, sizeof(desc));
      transition(ctx.list.Get(), data.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      transition(ctx.list.Get(), descs.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY histogram{1, level, desc.Format};
    D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC arrayDesc{1, &histogram, data->GetGPUVirtualAddress(),
                                                        {descs->GetGPUVirtualAddress(), sizeof(desc)}};
    dxmt::OmmBuildInputs arrayInputs{};
    arrayInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY);
    arrayInputs.NumDescs = 1;
    arrayInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
    arrayInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    arrayInputs.pOpacityMicromapArrayDesc = &arrayDesc;
    auto array = geometry.build(abi(arrayInputs), "omm: array build");
    D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC link{};
    link.OpacityMicromapArray = array->GetGPUVirtualAddress();
    dxmt::OmmGeometryDesc g{};
    g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE(D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES);
    g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE; // OMM replaces this flag.
    g.OmmTriangles = {&triangles, &link};
    dxmt::OmmBuildInputs blas{};
    blas.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blas.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(1 | 2 | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_OMM_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS);
    blas.NumDescs = 1; blas.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; blas.pGeometryDescs = &g;
    auto bottom = geometry.build(abi(blas), "omm: linked BLAS");
    auto top = scene(geometry, bottom.Get());
    geometry.submit("omm: build before oracle");
    // Unlike Runner::upload, these have no fixture-owned keepalive. Once the
    // build fence retires, every later operation must consume the AS shadow.
    data.Reset(); descs.Reset(); stagingData.Reset(); stagingDescs.Reset();
    const UINT samples = level <= 5 ? count : 32;
    for (UINT start = 0; start < samples; start += 16) {
      std::vector<stateobj::LaneRay> rays;
      std::vector<float> opaqueVertices, unknownVertices;
      for (UINT j = start; j < std::min(start + 16, samples); ++j) {
        const UINT index = level <= 5 ? j : ((j * 2654435761u) & (count - 1));
        const auto c = cell(index, level);
        const UINT state = ((index * 2654435761u) ^ (index >> 7) ^ (index >> 13)) >> 28 & (states - 1);
        auto r = stateobj::ray((c.x[0]+c.x[1]+c.x[2])/3, (c.y[0]+c.y[1]+c.y[2])/3, -1, 0,0,1);
        rays.push_back(r);
        auto &out = state == 1 ? opaqueVertices : unknownVertices;
        if (state != 0) for (UINT k = 0; k < 3; ++k) out.insert(out.end(), {c.x[k], c.y[k], 0});
      }
      std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> explicitGeometries;
      std::vector<ComPtr<ID3D12Resource>> explicitBuffers;
      for (UINT kind = 0; kind < 2; ++kind) {
        const auto &v = kind == 0 ? opaqueVertices : unknownVertices;
        if (v.empty()) continue;
        auto vb = geometry.upload(v.data(), v.size()*sizeof(float));
        D3D12_RAYTRACING_GEOMETRY_DESC e{};
        e.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        e.Flags = kind == 0 ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        e.Triangles = triangles; e.Triangles.VertexCount = UINT(v.size()/3);
        e.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
        explicitBuffers.push_back(vb); explicitGeometries.push_back(e);
      }
      auto oracleBottom = geometry.build(blasInputs(explicitGeometries.data(), UINT(explicitGeometries.size())), "omm: physical microtriangles");
      auto oracleTop = scene(geometry, oracleBottom.Get());
      // Two dispatches exercise accepting/rejecting unknown candidates against real geometry.
      for (UINT reject = 0; reject < 2; ++reject) {
        for (auto &r : rays) r.pad[0] = reject;
        auto actual = dispatch.trace("omm: classified dispatch", object.Get(), top->GetGPUVirtualAddress(), rays);
        auto expected = dispatch.trace("omm: independent geometry dispatch", object.Get(), oracleTop->GetGPUVirtualAddress(), rays);
        for (UINT lane = 0; lane < rays.size(); ++lane) for (UINT word : checkedWords) {
          const UINT offset = lane*32 + word;
          ++comparisons;
          if (actual[offset] != expected[offset]) {
            std::printf("OMM_ORACLE_FAIL format=%u level=%u sample=%u reject=%u word=%u actual=%u expected=%u\n",
                        format,level,start+lane,reject,word,actual[offset],expected[offset]); ++bad;
          }
        }
        if (!legacy) for (UINT lane = 0; lane < rays.size(); ++lane) {
          comparisons += 2;
          if (actual[lane*32+8] != 1 || actual[lane*32+9] != 0) {
            std::printf("OMM_QUERY_OPTOUT_FAIL format=%u level=%u sample=%u reject=%u hit=%u candidates=%u\n",
                format,level,start+lane,reject,actual[lane*32+8],actual[lane*32+9]); ++bad;
          }
        }
      }
    }
    if (format == 2 && level == 1) {
      const uint8_t mapStates[] = {0,1,2,3};
      auto multiData = detachedUpload(device, mapStates, sizeof(mapStates));
      D3D12_RAYTRACING_OPACITY_MICROMAP_DESC mapDescs[4]{};
      for (UINT i = 0; i < 4; ++i) {
        mapDescs[i].ByteOffset = i;
        mapDescs[i].Format = D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE;
      }
      auto multiDescs = detachedUpload(device, mapDescs, sizeof(mapDescs));
      D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY multiHistogram{4, 0, D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE};
      D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC multiDesc{1, &multiHistogram, multiData->GetGPUVirtualAddress(),
          {multiDescs->GetGPUVirtualAddress(), sizeof(mapDescs[0])}};
      auto multiInputs = arrayInputs; multiInputs.pOpacityMicromapArrayDesc = &multiDesc;
      auto multiArray = geometry.build(abi(multiInputs), "omm: multiple-map array");
      for (UINT width : {0u,1u,2u,4u}) for (UINT index = 0; index < (width ? 3u : 1u); ++index) {
        auto indices = geometry.upload(&index, std::max(width, 1u));
        link.OpacityMicromapArray = multiArray->GetGPUVirtualAddress();
        link.OpacityMicromapBaseLocation = 1;
        link.OpacityMicromapIndexBuffer = width ? D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE{indices->GetGPUVirtualAddress(), width} : D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE{};
        link.OpacityMicromapIndexFormat = width == 1 ? DXGI_FORMAT_R8_UINT : width == 2 ? DXGI_FORMAT_R16_UINT : width == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN;
        auto indexedBottom = geometry.build(abi(blas), "omm: indexed linkage");
        auto indexedTop = scene(geometry, indexedBottom.Get());
        std::vector<stateobj::LaneRay> rays;
        for (UINT reject = 0; reject < 2; ++reject) {
          auto r = stateobj::ray(0.25f,0.25f,-1,0,0,1); r.pad[0] = reject; rays.push_back(r);
        }
        auto words = dispatch.trace("omm: nonuniform index and base", object.Get(), indexedTop->GetGPUVirtualAddress(), rays);
        for (UINT lane = 0; lane < 2; ++lane) for (UINT word : checkedWords) {
          const UINT expected = word == 3 || word == 5 || word == 6 ? index != 0 : index == 0 || lane == 0;
          ++comparisons;
          if (words[lane*32+word] != expected) {
            std::printf("OMM_INDEX_FAIL width=%u index=%u reject=%u word=%u actual=%u expected=%u\n",
                width,index,lane,word,words[lane*32+word],expected); ++bad;
          }
        }
      }
      multiData.Reset(); multiDescs.Reset();
      // Uniform states require no array, and the reserved indices retain their
      // signed meaning independently of element width or BaseLocation.
      for (UINT width : {1u,2u,4u}) for (UINT state = 0; state < 4; ++state) {
        const UINT index = 0xffffffffu - state;
        auto indices = geometry.upload(&index, width);
        link.OpacityMicromapArray = 0;
        link.OpacityMicromapIndexBuffer = {indices->GetGPUVirtualAddress(), width};
        link.OpacityMicromapIndexFormat = width == 1 ? DXGI_FORMAT_R8_UINT : width == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        link.OpacityMicromapBaseLocation = 17;
        auto uniformBottom = geometry.build(abi(blas), "omm: uniform-state BLAS");
        if (width == 1 && state == 1) {
          const auto uniformAddress = uniformBottom->GetGPUVirtualAddress();
          auto sizeBuffer = geometry.uavBuffer(16);
          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC sizeInfo{sizeBuffer->GetGPUVirtualAddress(),
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION};
          ctx.list->EmitRaytracingAccelerationStructurePostbuildInfo(&sizeInfo, 1, &uniformAddress);
          uavBarrier(ctx.list.Get(), sizeBuffer.Get());
          auto sizeBytes = readBytes(geometry, sizeBuffer.Get(), 16);
          UINT64 serializedSize; std::memcpy(&serializedSize, sizeBytes.data(), 8);
          auto serialized = geometry.uavBuffer(serializedSize);
          ctx.list->CopyRaytracingAccelerationStructure(serialized->GetGPUVirtualAddress(), uniformAddress,
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
          uavBarrier(ctx.list.Get(), serialized.Get());
          auto headerBytes = readBytes(geometry, serialized.Get(), 56);
          UINT64 pointerCount; std::memcpy(&pointerCount, headerBytes.data()+48, 8);
          require(pointerCount == 0, "omm: null array creates no serialized resource pointer");
          auto restored = geometry.asBuffer(uniformBottom->GetDesc().Width);
          ctx.list->CopyRaytracingAccelerationStructure(restored->GetGPUVirtualAddress(), serialized->GetGPUVirtualAddress(),
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE);
          uavBarrier(ctx.list.Get(), restored.Get()); geometry.submit("omm: uniform BLAS deserialize");
          auto restoredTop = scene(geometry, restored.Get());
          std::vector rays{stateobj::ray(0.25f,0.25f,-1,0,0,1)};
          auto words = dispatch.trace("omm: uniform BLAS relocation", object.Get(), restoredTop->GetGPUVirtualAddress(), rays);
          for (UINT word : checkedWords) {
            ++comparisons;
            require(words[word] == (word == 3 || word == 5 || word == 6 ? 0u : 1u),
                "omm: uniform BLAS retains opaque special index");
          }
        }
        for (UINT instanceFlags : {0u,4u,8u,16u,32u}) {
          auto uniformTop = scene(geometry, uniformBottom.Get(), instanceFlags);
          std::vector<stateobj::LaneRay> rays;
          for (UINT flags : {0u,1u,2u,0x40u,0x80u,0x400u,0x401u}) for (UINT reject = 0; reject < 2; ++reject) {
            auto r = stateobj::ray(0.25f,0.25f,-1,0,0,1,flags); r.pad[0] = reject; rays.push_back(r);
          }
          auto words = dispatch.trace("omm: uniform flag precedence", object.Get(), uniformTop->GetGPUVirtualAddress(), rays);
          for (UINT lane = 0; lane < rays.size(); ++lane) {
            const auto &r = rays[lane];
            const UINT resolved = (instanceFlags & 32) ? 1 : (instanceFlags & 16) || (r.flags & 0x400) ? state & 1 : state;
            const UINT forced = (r.flags & 3) ? r.flags & 3 : (instanceFlags >> 2) & 3;
            const bool opaque = forced ? forced == 1 : resolved == 1;
            const bool culled = r.flags & (opaque ? 0x40 : 0x80);
            const bool candidate = resolved != 0 && !opaque && !culled;
            const bool hit = resolved != 0 && !culled && (opaque || r.pad[0] == 0);
            for (UINT word : checkedWords) {
              const UINT expected = word == 3 || word == 5 || word == 6 ? candidate : hit;
              ++comparisons;
              if (words[lane*32+word] != expected) {
                std::printf("OMM_FLAGS_FAIL width=%u state=%u instance=%u ray=%u reject=%u word=%u actual=%u expected=%u\n",
                    width,state,instanceFlags,r.flags,r.pad[0],word,words[lane*32+word],expected); ++bad;
              }
            }
          }
        }
      }
      link.OpacityMicromapArray = array->GetGPUVirtualAddress();
      link.OpacityMicromapIndexBuffer = {}; link.OpacityMicromapIndexFormat = DXGI_FORMAT_UNKNOWN;
      link.OpacityMicromapBaseLocation = 0;
      link.OpacityMicromapArray = 0;
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO invalidLinkInfo{};
      auto invalidLinkInputs = abi(blas);
      std::puts("OMM_EXPECTED_INVALID_LINKAGE_BEGIN");
      device->GetRaytracingAccelerationStructurePrebuildInfo(&invalidLinkInputs, &invalidLinkInfo);
      require(invalidLinkInfo.ResultDataMaxSizeInBytes == 0, "omm: implicit linkage requires an array");
      std::puts("OMM_EXPECTED_INVALID_LINKAGE_END");
      link.OpacityMicromapArray = array->GetGPUVirtualAddress();
      auto infoBuffer = geometry.uavBuffer(16);
      const auto arrayAddress = array->GetGPUVirtualAddress();
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC info{infoBuffer->GetGPUVirtualAddress(),
          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE(0)};
      ctx.list->EmitRaytracingAccelerationStructurePostbuildInfo(&info, 1, &arrayAddress);
      uavBarrier(ctx.list.Get(), infoBuffer.Get());
      auto infoBytes = readBytes(geometry, infoBuffer.Get(), 8);
      UINT64 currentSize; std::memcpy(&currentSize, infoBytes.data(), 8);
      require(currentSize >= 64 && currentSize <= array->GetDesc().Width, "omm: postbuild current size");
      info.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TOOLS_VISUALIZATION;
      ctx.list->EmitRaytracingAccelerationStructurePostbuildInfo(&info, 1, &arrayAddress);
      uavBarrier(ctx.list.Get(), infoBuffer.Get());
      infoBytes = readBytes(geometry, infoBuffer.Get(), 8);
      UINT64 visualSize; std::memcpy(&visualSize, infoBytes.data(), 8);
      auto visual = geometry.uavBuffer(visualSize);
      ctx.list->CopyRaytracingAccelerationStructure(visual->GetGPUVirtualAddress(), arrayAddress,
          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS);
      uavBarrier(ctx.list.Get(), visual.Get());
      auto visualBytes = readBytes(geometry, visual.Get(), visualSize);
      UINT visualHeader[2]; std::memcpy(visualHeader, visualBytes.data(), 8);
      require(visualHeader[0] == 2 && visualHeader[1] == 1, "omm: decoded array header");
      UINT64 decodedPointers[4]; std::memcpy(decodedPointers, visualBytes.data()+16, sizeof(decodedPointers));
      const auto visualAddress = visual->GetGPUVirtualAddress();
      auto decodedOffset = [&](UINT64 address, UINT64 bytes) {
        require(address >= visualAddress && address-visualAddress <= visualSize &&
            bytes <= visualSize-(address-visualAddress), "omm: decoded input is self-contained");
        return SIZE_T(address-visualAddress);
      };
      const SIZE_T decodedDescOffset = decodedOffset(decodedPointers[2], sizeof(desc));
      D3D12_RAYTRACING_OPACITY_MICROMAP_DESC decodedDesc;
      std::memcpy(&decodedDesc, visualBytes.data()+decodedDescOffset, sizeof(decodedDesc));
      require(decodedDesc.SubdivisionLevel == 1 && decodedDesc.Format == 2 && decodedPointers[3] == 8,
          "omm: decoded descriptor layout");
      const SIZE_T decodedDataOffset = decodedOffset(decodedPointers[1]+decodedDesc.ByteOffset, 1);
      require(visualBytes[decodedDataOffset] == packed[37], "omm: decoded packed states");
      const SIZE_T histogramOffset = decodedOffset(decodedPointers[0], 26*12);
      D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY decodedHistogram;
      std::memcpy(&decodedHistogram, visualBytes.data()+histogramOffset+3*12, sizeof(decodedHistogram));
      require(decodedHistogram.Count == 1 && decodedHistogram.SubdivisionLevel == 1 && decodedHistogram.Format == 2,
          "omm: decoded histogram");
      const auto bottomAddress = bottom->GetGPUVirtualAddress();
      ctx.list->EmitRaytracingAccelerationStructurePostbuildInfo(&info, 1, &bottomAddress);
      uavBarrier(ctx.list.Get(), infoBuffer.Get());
      infoBytes = readBytes(geometry, infoBuffer.Get(), 8);
      UINT64 bottomVisualSize; std::memcpy(&bottomVisualSize, infoBytes.data(), 8);
      auto bottomVisual = geometry.uavBuffer(bottomVisualSize);
      ctx.list->CopyRaytracingAccelerationStructure(bottomVisual->GetGPUVirtualAddress(), bottomAddress,
          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS);
      uavBarrier(ctx.list.Get(), bottomVisual.Get());
      auto bottomVisualBytes = readBytes(geometry, bottomVisual.Get(), bottomVisualSize);
      UINT bottomVisualHeader[4]; std::memcpy(bottomVisualHeader, bottomVisualBytes.data(), 16);
      require(bottomVisualHeader[0] == 1 && bottomVisualHeader[1] == 1 && bottomVisualHeader[2] == 2,
          "omm: decoded BLAS geometry header");
      UINT64 decodedLinkAddress; std::memcpy(&decodedLinkAddress, bottomVisualBytes.data()+24, 8);
      const auto bottomVisualAddress = bottomVisual->GetGPUVirtualAddress();
      require(decodedLinkAddress >= bottomVisualAddress && decodedLinkAddress-bottomVisualAddress <= bottomVisualSize-32,
          "omm: decoded BLAS linkage in result");
      D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC decodedLink;
      std::memcpy(&decodedLink, bottomVisualBytes.data()+SIZE_T(decodedLinkAddress-bottomVisualAddress), sizeof(decodedLink));
      require(decodedLink.OpacityMicromapArray == arrayAddress && decodedLink.OpacityMicromapIndexBuffer.StartAddress == 0 &&
          decodedLink.OpacityMicromapIndexFormat == DXGI_FORMAT_UNKNOWN, "omm: decoded BLAS preserves external array reference");
      std::vector<stateobj::LaneRay> copyRays;
      for (UINT i = 0; i < 4; ++i) {
        auto c = cell(i, 1); copyRays.push_back(stateobj::ray((c.x[0]+c.x[1]+c.x[2])/3,
            (c.y[0]+c.y[1]+c.y[2])/3,-1,0,0,1));
      }
      auto baseline = dispatch.trace("omm: persistence baseline", object.Get(), top->GetGPUVirtualAddress(), copyRays);
      auto compare = [&](ID3D12Resource *copiedTop, const char *name) {
        auto words = dispatch.trace(name, object.Get(), copiedTop->GetGPUVirtualAddress(), copyRays);
        for (UINT lane = 0; lane < 4; ++lane) for (UINT word : checkedWords) {
          ++comparisons;
          if (words[lane*32+word] != baseline[lane*32+word]) {
            std::printf("OMM_PERSISTENCE_FAIL case=%s lane=%u word=%u actual=%u expected=%u\n",name,lane,word,
                words[lane*32+word],baseline[lane*32+word]); ++bad;
          }
        }
      };
      ComPtr<ID3D12Resource> relocatedArray;
      for (UINT mode : {0u,1u,3u}) {
        relocatedArray = geometry.asBuffer(array->GetDesc().Width);
        if (mode != 3) {
          ctx.list->CopyRaytracingAccelerationStructure(relocatedArray->GetGPUVirtualAddress(), arrayAddress,
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE(mode));
        } else {
          auto serialized = geometry.uavBuffer(array->GetDesc().Width + 2048);
          ctx.list->CopyRaytracingAccelerationStructure(serialized->GetGPUVirtualAddress(), arrayAddress,
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
          uavBarrier(ctx.list.Get(), serialized.Get());
          auto bytes = readBytes(geometry, serialized.Get(), 56);
          D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER1 header; std::memcpy(&header, bytes.data(), 56);
          require(header.HeaderPostambleType == 0 && header.NumBottomLevelAccelerationStructurePointersAfterHeader == 0, "omm: serialized array header");
          require(device->CheckDriverMatchingIdentifier(D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE,
              &header.DriverMatchingIdentifier) == D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE,
              "omm: serialized array RTAS0 driver admission");
          require(device->CheckDriverMatchingIdentifier(D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE, nullptr)
              == D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED, "omm: null driver identifier");
          require(device->CheckDriverMatchingIdentifier(D3D12_SERIALIZED_DATA_TYPE(1), &header.DriverMatchingIdentifier)
              == D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED, "omm: unsupported serialized-data type");
          auto invalidVersion = header.DriverMatchingIdentifier; invalidVersion.DriverOpaqueVersioningData[8] ^= 1;
          require(device->CheckDriverMatchingIdentifier(D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE, &invalidVersion)
              == D3D12_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION, "omm: incompatible serialization version");
          ctx.list->CopyRaytracingAccelerationStructure(relocatedArray->GetGPUVirtualAddress(), serialized->GetGPUVirtualAddress(),
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE);
        }
        uavBarrier(ctx.list.Get(), relocatedArray.Get());
        link.OpacityMicromapArray = relocatedArray->GetGPUVirtualAddress();
        auto b = geometry.build(abi(blas), "omm: relocated array BLAS");
        auto t = scene(geometry, b.Get()); compare(t.Get(), "omm: array clone/compact/serialize");
      }
      link.OpacityMicromapArray = arrayAddress;
      float distantMacro[9]; std::memcpy(distantMacro, macro, sizeof(macro));
      for (UINT i = 0; i < 3; ++i) distantMacro[i*3] += 2;
      auto distantVertices = geometry.upload(distantMacro, sizeof(distantMacro));
      dxmt::OmmGeometryDesc mixedGeometries[2]{};
      mixedGeometries[0].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
      mixedGeometries[0].Triangles = triangles;
      mixedGeometries[0].Triangles.VertexBuffer.StartAddress = distantVertices->GetGPUVirtualAddress();
      mixedGeometries[1] = g;
      auto mixedInputs = blas; mixedInputs.NumDescs = 2; mixedInputs.pGeometryDescs = mixedGeometries;
      auto mixedBottom = geometry.build(abi(mixedInputs), "omm: mixed ordinary and linked geometry");
      for (UINT mode : {0u,1u,3u}) {
        auto copiedBottom = geometry.asBuffer(mixedBottom->GetDesc().Width);
        if (mode != 3) {
          ctx.list->CopyRaytracingAccelerationStructure(copiedBottom->GetGPUVirtualAddress(), mixedBottom->GetGPUVirtualAddress(),
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE(mode));
        } else {
          const auto mixedAddress = mixedBottom->GetGPUVirtualAddress();
          info.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION;
          ctx.list->EmitRaytracingAccelerationStructurePostbuildInfo(&info, 1, &mixedAddress);
          uavBarrier(ctx.list.Get(), infoBuffer.Get());
          auto sizeBytes = readBytes(geometry, infoBuffer.Get(), 16);
          UINT64 serializedSize; std::memcpy(&serializedSize, sizeBytes.data(), 8);
          auto serialized = geometry.uavBuffer(serializedSize);
          ctx.list->CopyRaytracingAccelerationStructure(serialized->GetGPUVirtualAddress(), mixedAddress,
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
          uavBarrier(ctx.list.Get(), serialized.Get());
          auto bytes = readBytes(geometry, serialized.Get(), 80);
          D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER1 header; std::memcpy(&header, bytes.data(), 56);
          D3D12_RAYTRACING_SERIALIZED_BLOCK block; std::memcpy(&block, bytes.data()+56, 16);
          std::printf("OMM_SERIALIZED_BLAS_HEADER postamble=%u blocks=%u blocktype=%u pointers=%llu\n",
              UINT(header.HeaderPostambleType),header.NumBlocks,UINT(block.Type),
              static_cast<unsigned long long>(block.NumBlockPointersAfterHeader));
          require(header.HeaderPostambleType == 0xffffffffu && header.NumBlocks == 1 && block.Type == 0 && block.NumBlockPointersAfterHeader == 1, "omm: serialized BLAS block header");
          require(device->CheckDriverMatchingIdentifier(D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE,
              &header.DriverMatchingIdentifier) == D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE,
              "omm: serialized mixed BLAS RTAS0 driver admission");
          std::puts("OMM_DRIVER_IDENTIFIER_PASS type=0 array=1 blas=1 rejects=3");
          const auto newAddress = relocatedArray->GetGPUVirtualAddress();
          auto pointer = geometry.upload(&newAddress, sizeof(newAddress));
          transition(ctx.list.Get(), serialized.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
          ctx.list->CopyBufferRegion(serialized.Get(), 72, pointer.Get(), 0, 8);
          transition(ctx.list.Get(), serialized.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          ctx.list->CopyRaytracingAccelerationStructure(copiedBottom->GetGPUVirtualAddress(), serialized->GetGPUVirtualAddress(),
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE);
        }
        uavBarrier(ctx.list.Get(), copiedBottom.Get());
        auto t = scene(geometry, copiedBottom.Get()); compare(t.Get(), "omm: BLAS clone/compact/serialize relocation");
      }
      link.OpacityMicromapArray = relocatedArray->GetGPUVirtualAddress();
      auto updatedInputs = abi(blas);
      updatedInputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO updateInfo{};
      device->GetRaytracingAccelerationStructurePrebuildInfo(&updatedInputs, &updateInfo);
      auto scratch = geometry.uavBuffer(updateInfo.UpdateScratchDataSizeInBytes);
      D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC update{};
      update.Inputs = updatedInputs; update.SourceAccelerationStructureData = bottom->GetGPUVirtualAddress();
      update.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
      auto updatedBottom = geometry.asBuffer(updateInfo.ResultDataMaxSizeInBytes);
      update.DestAccelerationStructureData = updatedBottom->GetGPUVirtualAddress();
      ctx.list->BuildRaytracingAccelerationStructure(&update, 0, nullptr); uavBarrier(ctx.list.Get(), updatedBottom.Get());
      auto outOfPlaceTop = scene(geometry, updatedBottom.Get()); compare(outOfPlaceTop.Get(), "omm: out-of-place linkage update");
      update.DestAccelerationStructureData = bottom->GetGPUVirtualAddress();
      ctx.list->BuildRaytracingAccelerationStructure(&update, 0, nullptr); uavBarrier(ctx.list.Get(), bottom.Get());
      auto updatedTop = scene(geometry, bottom.Get()); compare(updatedTop.Get(), "omm: in-place linkage update");
    }
    std::printf("OMM_SUBDIVISION format=%u level=%u samples=%u cumulative=%llu failures=%d\n",format,level,samples,
                static_cast<unsigned long long>(comparisons),bad);
    std::fflush(stdout);
  }
  std::printf("OWNED_D3D12_OMM_RESULT comparisons=%llu failures=%d legacy=%u\n",static_cast<unsigned long long>(comparisons),bad,legacy);
  return bad != 0;
}
}
}
int wmain(int argc, wchar_t **argv) {
  if (argc != 3 && argc != 4) return fail("usage: owned-d3d12-raytracing-omm.exe contract.dxil omm.dxil [legacy-omm.dxil]");
  HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  if (!d3d12) return fail("omm: d3d12 module", HRESULT_FROM_WIN32(GetLastError()));
  using BackendIdFn = const char *(WINAPI *)();
  auto backendId = reinterpret_cast<BackendIdFn>(GetProcAddress(d3d12, "OwnedGraphicsBackendId"));
  const char *id = backendId ? backendId() : nullptr;
  require(id && std::strcmp(id, kExpectedBackendId) == 0, "omm: owned backend identity");
  std::printf("OWNED_D3D12_OMM_BACKEND id=%s\n", id);
  std::fflush(stdout);
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "omm: factory");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "omm: adapter");
  ComPtr<ID3D12Device5> device;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "omm: device");
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)), "omm: OPTIONS5 query");
  require(options5.RaytracingTier == D3D12_RAYTRACING_TIER_1_2, "omm: public RaytracingTier1.2");
  D3D12_FEATURE_DATA_D3D12_OPTIONS22 options22{};
  const auto options22Feature = D3D12_FEATURE(D3D12_FEATURE_D3D12_OPTIONS22);
  check(device->CheckFeatureSupport(options22Feature, &options22, sizeof(options22)), "omm: OPTIONS22 query");
  require(options22.ShaderExecutionReorderingActuallyReorders == FALSE && options22.CreateByteOffsetViewsSupported == FALSE &&
      options22.Max1DDispatchSize == 65535 && options22.Max1DDispatchMeshSize == 65535, "omm: truthful SER and dispatch limits");
  for (UINT feature : {UINT(D3D12_FEATURE_D3D12_OPTIONS5), UINT(D3D12_FEATURE_D3D12_OPTIONS22)}) {
    const UINT exactSize = feature == UINT(D3D12_FEATURE_D3D12_OPTIONS5) ? sizeof(options5) : sizeof(options22);
    for (UINT bytes : {0u, exactSize-1, exactSize+1}) {
      std::array<uint8_t,32> canary; canary.fill(0xa5);
      require(device->CheckFeatureSupport(D3D12_FEATURE(feature), canary.data(), bytes) == E_INVALIDARG,
          "omm: capability query rejects incorrect byte count");
      require(std::ranges::all_of(canary, [](uint8_t byte) { return byte == 0xa5; }), "omm: invalid query leaves output untouched");
    }
    require(device->CheckFeatureSupport(D3D12_FEATURE(feature), nullptr, exactSize) == E_INVALIDARG,
        "omm: capability query rejects null output");
  }
  std::puts("OWNED_D3D12_OMM_CAPS tier=12 actually_reorders=0 byte_offset_views=0 max_dispatch=65535 max_mesh=65535 query_abi=16 errors=8");
  Context ctx(device.Get());
  const auto contractDxil = readFile(argv[1]);
  const int modern = omm::execute(device.Get(), ctx, contractDxil, readFile(argv[2]));
  const int legacy = argc == 4 ? omm::execute(device.Get(), ctx, contractDxil, readFile(argv[3]), true) : 0;
  if (!modern && !legacy && argc == 4) std::puts("OWNED_D3D12_OMM_PASS cases=27");
  return modern | legacy;
}
