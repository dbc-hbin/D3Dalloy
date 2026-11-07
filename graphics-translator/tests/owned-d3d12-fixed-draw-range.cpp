// SPDX-License-Identifier: LGPL-2.1-or-later
#define main retained_render_cases
#include "owned-d3d12-render-passes.cpp"
#undef main

int main() {
  Context c;
  constexpr auto fmt = DXGI_FORMAT_R32_FLOAT;
  auto target = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, texDesc(fmt, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                          D3D12_RESOURCE_STATE_RENDER_TARGET);
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtv_heap);
  c.dev->CreateRenderTargetView(target.Get(), nullptr, rtv);
  Draw normal(c.dev.Get(), fmt, DXGI_FORMAT_UNKNOWN, 1, "psMain", true);
  Draw seed(c.dev.Get(), fmt, DXGI_FORMAT_UNKNOWN, 1, "psMain", false);
  Draw writer(c.dev.Get(), fmt, DXGI_FORMAT_UNKNOWN, 1, "psUavAtomicAdd", true, false, 6, true);
  auto uav = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, bufDesc(4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto indices = committed(c.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, bufDesc(24), D3D12_RESOURCE_STATE_GENERIC_READ);
  UINT *data;
  check(indices->Map(0, nullptr, reinterpret_cast<void **>(&data)), "index map");
  const UINT index_data[] = {0x00010000u, 2, 0, 0, 1, 2};
  memcpy(data, index_data, sizeof(index_data)); indices->Unmap(0, nullptr);
  auto control = committed(c.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, bufDesc(8), D3D12_RESOURCE_STATE_GENERIC_READ);
  enum Mode { Direct, Index16, Index32, Below, Above, Aliased, UAV, Counted, Predicated, Cold, ShortPage, WrongKey, ColdPSO, ResetEmpty };
  for (auto mode : {ColdPSO, Cold, ResetEmpty, ShortPage, WrongKey, Direct, Index16, Index32, Below, Above, Aliased, UAV, Counted, Predicated}) {
    const bool indexed = mode == Index16 || mode == Index32;
    const UINT n = mode == Below ? 16383 : mode == Above ? 32769 : 16384;
    const auto desc = bufDesc(32ull * n + 16);
    auto upload = committed(c.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, desc, D3D12_RESOURCE_STATE_GENERIC_READ);
    ComPtr<ID3D12Resource> args;
    ComPtr<ID3D12Heap> placed;
    if (mode == Aliased) {
      D3D12_HEAP_DESC h{}; h.SizeInBytes = c.dev->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
      h.Properties = heap(D3D12_HEAP_TYPE_DEFAULT); h.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
      check(c.dev->CreateHeap(&h, IID_PPV_ARGS(&placed)), "aliasable heap");
      check(c.dev->CreatePlacedResource(placed.Get(), 0, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                       IID_PPV_ARGS(&args)), "aliasable arguments");
    } else args = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COPY_DEST);
    check(upload->Map(0, nullptr, reinterpret_cast<void **>(&data)), "argument map");
    memset(data, 0, desc.Width); data[4] = 3; data[5] = 1;
    const UINT last = 4 + 8 * (n - 1); data[last] = 3;
    D3D12_INDIRECT_ARGUMENT_DESC op{};
    op.Type = indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC sd{32, 1, &op, 0};
    ComPtr<ID3D12CommandSignature> sig;
    check(c.dev->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&sig)), "fixed signature");
    auto l = c.list();
    const float zero[4]{}, one[4]{1, 0, 0, 1};
    auto &draw = mode == UAV ? writer : normal;
    if (mode != Cold) {
      auto &preparation = mode == ColdPSO ? seed : draw;
      l.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      preparation.draw(l.list.Get(), 1, 1, one, .5f, 0, nullptr, nullptr, mode == UAV ? uav->GetGPUVirtualAddress() : 0);
      D3D12_INDEX_BUFFER_VIEW ib{indices->GetGPUVirtualAddress(), 8, DXGI_FORMAT_R16_UINT};
      l.list->IASetIndexBuffer(&ib);
      auto warm_sig = sig;
      if (mode == WrongKey) {
        op.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        warm_sig.Reset();
        check(c.dev->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&warm_sig)), "indexed warm-up signature");
      }
      uint64_t *count;
      check(control->Map(0,nullptr,reinterpret_cast<void **>(&count)), "warm-up control map");
      *count = 0; control->Unmap(0,nullptr);
      if (mode != ResetEmpty)
        l.list->ExecuteIndirect(warm_sig.Get(), mode == ShortPage ? 64u : n, upload.Get(), 16, control.Get(), 0);
      c.execute({l.list.Get()});
      auto retire = c.list(); retire.list->ClearRenderTargetView(rtv, zero, 0, nullptr); c.execute({retire.list.Get()});
      check(l.allocator->Reset(), "recycle ICB page"); check(l.list->Reset(l.allocator.Get(), nullptr), "reset list");
    }
    l.list->CopyResource(args.Get(), upload.Get());
    transition(l.list.Get(), args.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    l.list->ClearRenderTargetView(rtv, zero, 0, nullptr);
    l.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    draw.draw(l.list.Get(), 1, 1, one, .5f, 0, nullptr, nullptr, mode == UAV ? uav->GetGPUVirtualAddress() : 0);
    if (indexed) {
      D3D12_INDEX_BUFFER_VIEW ib{indices->GetGPUVirtualAddress() + (mode == Index32 ? 12 : 0), mode == Index32 ? 12u : 8u,
                                mode == Index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
      l.list->IASetIndexBuffer(&ib);
    }
    if (mode == Predicated) l.list->SetPredication(control.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    l.list->ExecuteIndirect(sig.Get(), n, args.Get(), 16, mode == Counted ? control.Get() : nullptr, 0);
    if (mode == Predicated) l.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    transition(l.list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto pixel = copyOut(c.dev.Get(), l.list.Get(), target.Get());
    transition(l.list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(l.list.Get(), args.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST);
    check(l.list->Close(), "close fixed range");
    for (UINT replay : {1u, 2u}) {
      data[last + 1] = replay;
      uint64_t *cond;
      check(control->Map(0,nullptr,reinterpret_cast<void **>(&cond)), "control map");
      *cond = mode == Counted ? (replay == 1 ? 1 : n) : (replay - 1); control->Unmap(0,nullptr);
      c.executeSealed({l.list.Get()});
      const float expected = replay == 1 && mode == Counted ? 2 : replay == 1 && mode == Predicated ? 1 : 2 + replay;
      const float actual = pixel.at<float>(0, 0);
      if (actual != expected) {
        std::fprintf(stderr, "FIXED_RANGE_FAIL mode=%d replay=%u pixel=%g expected=%g\n", int(mode), replay, actual, expected);
        return 1;
      }
      // The fence signal can precede allocator retirement. Wait for a subsequent submission
      // before reissuing this exact recording with different live GPU arguments.
      if (replay == 1) {
        auto retire = c.list();
        retire.list->ClearRenderTargetView(rtv, zero, 0, nullptr);
        c.execute({retire.list.Get()});
      }
    }
    upload->Unmap(0,nullptr);
    std::printf("FIXED_RANGE_PASS mode=%d slots=%u replays=2\n", int(mode), n);
  }
}
