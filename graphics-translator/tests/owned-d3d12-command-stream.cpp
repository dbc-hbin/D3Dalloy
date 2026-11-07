// SPDX-License-Identifier: LGPL-2.1-or-later
// Focused public D3D12 regression. Reuses the render fixture's device, PSO and readback helpers.
#define main retained_render_cases
#include "owned-d3d12-render-passes.cpp"
#undef main

static void CheckDrawRuns() {
  Context c;
  constexpr auto format = DXGI_FORMAT_R8G8B8A8_UNORM;
  auto rt = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, texDesc(format, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                      D3D12_RESOURCE_STATE_RENDER_TARGET);
  const uint32_t indices[] = {0x00010000, 2, 0, 1, 2, 0, 0, 0};
  auto upload = committed(c.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, bufDesc(sizeof(indices)), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto ib = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, bufDesc(sizeof(indices)), D3D12_RESOURCE_STATE_COPY_DEST);
  void *mapped;
  check(upload->Map(0, nullptr, &mapped), "index-map");
  memcpy(mapped, indices, sizeof(indices)); upload->Unmap(0, nullptr);
  auto prepare = c.list();
  prepare.list->CopyBufferRegion(ib.Get(), 0, upload.Get(), 0, sizeof(indices));
  transition(prepare.list.Get(), ib.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
  c.execute({prepare.list.Get()});
  ComPtr<ID3D12DescriptorHeap> heap;
  const auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, heap);
  c.dev->CreateRenderTargetView(rt.Get(), nullptr, rtv);
  Draw draw(c.dev.Get(), format, DXGI_FORMAT_UNKNOWN, 1, "psMain", true);
  auto list = c.list();
  const float black[] = {0, 0, 0, 1}, red[] = {1 / 255.f, 0, 0, 0}, green[] = {0, 1 / 255.f, 0, 0};
  list.list->ClearRenderTargetView(rtv, black, 0, nullptr);
  list.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  draw.draw(list.list.Get(), 4, 4, red);
  list.list->DrawInstanced(3, 1, 0, 0);
  list.list->DrawInstanced(3, 1, 0, 0);
  for (UINT i=2; i<=4; ++i) list.list->DrawInstanced(3, i, 0, UINT32_MAX-i);
  list.list->SetGraphicsRoot32BitConstants(0, 4, green, 0);
  D3D12_INDEX_BUFFER_VIEW view{ib->GetGPUVirtualAddress(), 8, DXGI_FORMAT_R16_UINT};
  list.list->IASetIndexBuffer(&view);
  list.list->DrawIndexedInstanced(3, 2, 0, 1, 17);
  list.list->DrawIndexedInstanced(3, 3, 0, 1, 81);
  // Both states permit an index read. This records a boundary without ending the scheduling pass.
  transition(list.list.Get(), ib.Get(), D3D12_RESOURCE_STATE_INDEX_BUFFER,
      D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  list.list->DrawIndexedInstanced(3, 4, 0, 1, 128);
  const float blue[] = {0, 0, 1 / 255.f, 0};
  list.list->SetGraphicsRoot32BitConstants(0, 4, blue, 0);
  view = {ib->GetGPUVirtualAddress() + 20, 12, DXGI_FORMAT_R32_UINT};
  list.list->IASetIndexBuffer(&view);
  list.list->DrawIndexedInstanced(3, 3, 0, 0, UINT32_MAX - 3); // degenerate
  view.BufferLocation = ib->GetGPUVirtualAddress() + 8;
  list.list->IASetIndexBuffer(&view);
  list.list->DrawIndexedInstanced(3, 2, 0, 0, UINT32_MAX - 2);
  list.list->DrawIndexedInstanced(3, 1, 0, 0, 0);
  list.list->DrawIndexedInstanced(3, 1, 0, 0, 0);
  list.list->DrawIndexedInstanced(3, 1, 0, 0, 0);
  c.execute({list.list.Get()});
  auto read = c.list();
  transition(read.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto color = copyOut(c.dev.Get(), read.list.Get(), rt.Get());
  c.execute({read.list.Get()});
  for (UINT y=0; y<4; ++y) for (UINT x=0; x<4; ++x)
    if (color.at<UINT>(x,y) != 0xff05090cu) fail("draw-run-pixel");
  std::puts("DRAW_RUNS_PASS red=12 green=9 blue=5 pixels=16 read_only_boundary=1 single_runs=2");
}

int main() {
  CheckDrawRuns();
  Context c;
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.Num32BitValues = 1;
  auto root = rootSignature(c.dev.Get(), {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
  auto *file = std::fopen("predication-indirect-cs.dxil", "rb");
  if (!file || std::fseek(file, 0, SEEK_END)) fail("read predication-indirect-cs.dxil");
  const auto length = std::ftell(file);
  if (length <= 0 || std::fseek(file, 0, SEEK_SET)) fail("shader size");
  std::vector<unsigned char> cs(size_t(length), 0);
  if (std::fread(cs.data(), 1, cs.size(), file) != cs.size()) fail("shader read");
  std::fclose(file);
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root.Get(); desc.CS = {cs.data(), cs.size()};
  ComPtr<ID3D12PipelineState> pso;
  check(c.dev->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)), "slice-pso");
  D3D12_INDIRECT_ARGUMENT_DESC dispatch{};
  dispatch.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
  D3D12_COMMAND_SIGNATURE_DESC sigDesc{12, 1, &dispatch, 0};
  ComPtr<ID3D12CommandSignature> sig;
  check(c.dev->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&sig)), "slice-signature");
  constexpr UINT n = 65, bytes = (n + 2) * 4;
  auto args = committed(c.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, bufDesc(n * 12), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto counts = committed(c.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, bufDesc(n * 4), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto zero = committed(c.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, bufDesc(bytes), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, bufDesc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_COPY_DEST);
  auto result = committed(c.dev.Get(), D3D12_HEAP_TYPE_READBACK, bufDesc(bytes), D3D12_RESOURCE_STATE_COPY_DEST);
  void *mapped;
  check(args->Map(0, nullptr, &mapped), "slice-args");
  auto *grid = static_cast<UINT *>(mapped);
  for (UINT i = 0; i < n; ++i) { grid[3*i]=i%3+1; grid[3*i+1]=grid[3*i+2]=1; }
  args->Unmap(0, nullptr);
  check(zero->Map(0, nullptr, &mapped), "slice-initialize"); memset(mapped, 0, bytes); zero->Unmap(0,nullptr);
  auto list = c.list();
  list.list->CopyBufferRegion(output.Get(), 0, zero.Get(), 0, bytes);
  transition(list.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list.list->SetComputeRootSignature(root.Get()); list.list->SetPipelineState(pso.Get());
  for (UINT i = 0; i < n; ++i) {
    list.list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress() + (i+1)*4);
    list.list->SetComputeRoot32BitConstant(1, i+1, 0);
    list.list->ExecuteIndirect(sig.Get(), 1, args.Get(), 12*i, counts.Get(), 4*i);
  }
  transition(list.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list.list->CopyBufferRegion(result.Get(),0,output.Get(),0,bytes);
  transition(list.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  check(list.list->Close(),"slice-close");
  for (UINT replay = 0; replay < 2; ++replay) {
    check(counts->Map(0, nullptr, &mapped), "slice-counts");
    for (UINT i = 0; i < n; ++i) static_cast<UINT *>(mapped)[i] = (i+replay)%4 != 0;
    counts->Unmap(0,nullptr);
    c.executeSealed({list.list.Get()});
    check(result->Map(0,nullptr,&mapped),"slice-read");
    const auto *words = static_cast<const UINT *>(mapped);
    if (words[0] || words[n+1]) fail("slice-guard");
    for (UINT i=0;i<n;++i)
      if (words[i+1] != ((i+replay)%4 ? (i+1)*(i%3+1) : 0)) fail("slice-value");
    result->Unmap(0,nullptr);
  }
  std::puts("COMMAND_STREAM_PASS slots=65 replays=2 guards=2");
}
