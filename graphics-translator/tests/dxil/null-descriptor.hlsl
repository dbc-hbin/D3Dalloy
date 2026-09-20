// D3D12 null / out-of-range descriptor contract fixture.
//
// Every resource is bound through a descriptor table, so the fixture can write
// production-valid null views into the CBV/SRV/UAV heap
// (Create*View(nullptr, pDesc, handle)) and a deliberately small view over a
// much larger allocation. D3D12 defines:
//
//   * a read through an unbound (null) descriptor yields zero,
//   * a store through an unbound descriptor writes nothing,
//   * a store outside the view bound writes nothing, even though the backing
//     allocation continues past the view,
//   * an atomic on either of those does nothing and reports the zero it read.
//
// u3 is deliberately a root descriptor instead of a table entry: it has no
// metadata, so it must keep performing its access unconditionally.
//
// Rebuild with upstream DXC:
//   dxc -T cs_6_0 -E main -HV 2021 -O0 -Fo null-descriptor.dxil null-descriptor.hlsl
// scripts/test-owned-null-descriptor.py compiles and runs the native GPU probe
// that binds this fixture's resources through the descriptor table below.
cbuffer NullCbv : register(b0)
{
    uint4 nullCbvValue;
};
ByteAddressBuffer NullRaw : register(t0);
StructuredBuffer<uint> NullStructured : register(t1);
RWByteAddressBuffer NullUav : register(u0);
// Backed by 64 KiB, but viewed as only 16 bytes: offset 4096 is out of range.
RWByteAddressBuffer OobUav : register(u1);
RWByteAddressBuffer Result : register(u2);
// Root descriptor: no view bound is communicated, access stays unconditional.
RWByteAddressBuffer RootUav : register(u3);

[numthreads(1, 1, 1)]
void main()
{
    Result.Store(0, nullCbvValue.x);
    Result.Store(4, NullRaw.Load<uint>(0));
    Result.Store(8, NullStructured.Load(3));
    Result.Store(12, 0x11223344u);

    NullUav.Store(0, 0xdeadbeefu);
    OobUav.Store(4096, 0x55667788u);

    uint nullOriginal = 0xffffffffu;
    uint oobOriginal = 0xffffffffu;
    NullUav.InterlockedAdd(0, 1u, nullOriginal);
    OobUav.InterlockedAdd(4096, 1u, oobOriginal);
    Result.Store(16, nullOriginal);
    Result.Store(20, oobOriginal);

    RootUav.Store(0, 0xcafef00du);
    Result.Store(28, RootUav.Load<uint>(0));
    Result.Store(24, 0xa5a5a5a5u);
}
