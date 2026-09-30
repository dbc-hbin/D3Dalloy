// DXIL library whose only content is root signature subobjects, for
// ID3D12Device14::CreateRootSignatureFromSubobjectInLibrary (owned-d3d12-enhanced-barriers).
GlobalRootSignature GlobalRS = { "RootConstants(num32BitConstants=3, b0), UAV(u0)" };
LocalRootSignature LocalRS = { "RootFlags(LOCAL_ROOT_SIGNATURE), CBV(b1)" };
// A library needs at least one export; this one is never called.
[shader("compute")] [numthreads(1, 1, 1)] void unused() {}
