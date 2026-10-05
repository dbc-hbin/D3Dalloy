// Focused 64-bit atomic consumer; uses existing public D3D12 fixture plumbing.
#define wmain sm6_fixture_wmain
#include "owned-d3d12-sm6.cpp"
#undef wmain

int wmain(int argc, wchar_t **argv) {
  const bool capacity_mode=argc==2&&std::wcscmp(argv[1],L"--capacity")==0;
  if(argc>2||(argc==2&&!capacity_mode)){std::fprintf(stderr,"usage: owned-d3d12-atomic64 [--capacity]\n");return 2;}
  setvbuf(stdout, nullptr, _IONBF, 0);
  HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  using BackendIdFn = const char *(WINAPI *)();
  auto backend = d3d12 ? reinterpret_cast<BackendIdFn>(GetProcAddress(d3d12, "OwnedGraphicsBackendId")) : nullptr;
  if (!backend || !backend() || std::strcmp(backend(), kExpectedBackendId) != 0)
    die("OwnedGraphicsBackendId mismatch");
  std::printf("OWNED_D3D12_ATOMIC64_BACKEND id=%s\n", backend());
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "EnumAdapters1");
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
  printCaps(device.Get());
  Gpu gpu(device.Get());
  Compute compute{gpu, computeRoot(device.Get()), nullptr};
  if(!capacity_mode) {
  begin();
  auto operations = compute.run("cs_atomic64_groupops", filled(24*8), filled(64), filled(64), {0,0,0,0}, 1);
  if (operations.empty()) return 1;
  const auto &o = operations[0];
  for (uint32_t op = 0; op < 9; ++op) {
    const uint64_t old = op == 4 || op == 5 ? 0xfffffff000000002ull : 0x200000002ull;
    const uint64_t value = op == 4 || op == 5 ? 0xfffffff100000003ull : 0x100000001ull;
    uint64_t expected = value;
    switch (op) {
    case 0: expected = old+value; break;
    case 1: expected = old&value; break;
    case 2: expected = old|value; break;
    case 3: expected = old^value; break;
    case 4: expected = uint64_t(std::min(int64_t(old), int64_t(value))); break;
    case 5: expected = uint64_t(std::max(int64_t(old), int64_t(value))); break;
    case 6: expected = std::min(old,value); break;
    case 7: expected = std::max(old,value); break;
    }
    EXPECT_EQ("groupops", "old", op, load<uint64_t>(o,op*16),old);
    EXPECT_EQ("groupops", "final", op, load<uint64_t>(o,op*16+8),expected);
  }
  const uint64_t expected[] = {0x200000002ull,0x300000003ull,0x300000003ull,
                             UINT64_MAX,0,0x8000000000000000ull};
  for (uint32_t i=0;i<6;++i) EXPECT_EQ("groupops","compare/wrap",i,load<uint64_t>(o,(18+i)*8),expected[i]);
  end("atomic64_groupshared_operations");
  begin();
  constexpr uint32_t groups = 64;
  auto stress = compute.run("cs_atomic64_groupstress", filled(groups*1025*8), filled(64),filled(64),{0,0,0,0},groups);
  if (stress.empty()) return 1;
  std::vector<uint64_t> returned(1024);
  for (uint32_t group=0; group<groups; ++group) {
    for (uint32_t i=0;i<1024;++i) returned[i] = load<uint64_t>(stress[0],(group*1025+i)*8);
    std::sort(returned.begin(),returned.end());
    for (uint32_t i=0;i<1024;++i) EXPECT_EQ("groupstress","permutation",group*1024+i,returned[i],uint64_t(i)*0x100000001ull);
    EXPECT_EQ("groupstress","final",group,load<uint64_t>(stress[0],(group*1025+1024)*8),1024ull*0x100000001ull);
  }
  end("atomic64_groupshared_stress");
  } else {
  begin();
  auto capacity = compute.run("cs_atomic64_groupcapacity",filled(65536),filled(64),filled(64),{0,0,0,0},1);
  if (capacity.empty()) return 1;
  for (uint32_t i=0;i<4096;++i) {
    EXPECT_EQ("groupcapacity","old",i,load<uint64_t>(capacity[0],i*8),uint64_t(i)*0x100000001ull);
    EXPECT_EQ("groupcapacity","final",i,load<uint64_t>(capacity[0],32768+i*8),uint64_t(i+1)*0x100000001ull);
  }
  end("atomic64_groupshared_32k_capacity");
  }
  std::printf("OWNED_D3D12_ATOMIC64_RESULT mode=%s failures=%d\n",capacity_mode?"capacity":"operations-stress",g_failures);
  std::printf("OWNED_D3D12_ATOMIC64_%s mode=%s\n",g_failures?"FAIL":"PASS",
              capacity_mode?"capacity":"operations-stress");
  return g_failures ? 1 : 0;
}
