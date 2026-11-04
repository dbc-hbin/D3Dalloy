// Exact integer hierarchy: partial logical and physical waves, wraparound and i16.
RWByteAddressBuffer Out : register(u0);
void exact_values(uint gi) {
  uint lane=WaveGetLaneIndex();
  uint64_t value=(uint64_t(gi+1)<<32) | (uint64_t(gi)*0x9e3779b9u+17u);
  int64_t signedValue=(gi&1)?-int64_t(value):int64_t(value);
  uint64_t product=uint64_t((lane%3)*2+1);
  uint16_t small=uint16_t(gi*257+32761);
  int16_t signedSmall=int16_t(small);
  uint base=gi*256;
  Out.Store<uint64_t>(base+0,WaveActiveSum(value));
  Out.Store<uint64_t>(base+8,WavePrefixSum(value));
  Out.Store<uint64_t>(base+16,WaveActiveMin(value));
  Out.Store<uint64_t>(base+24,WaveActiveMax(value));
  Out.Store<uint64_t>(base+32,uint64_t(WaveActiveSum(signedValue)));
  Out.Store<uint64_t>(base+40,uint64_t(WavePrefixSum(signedValue)));
  Out.Store<uint64_t>(base+48,uint64_t(WaveActiveMin(signedValue)));
  Out.Store<uint64_t>(base+56,uint64_t(WaveActiveMax(signedValue)));
  Out.Store<uint64_t>(base+64,WaveActiveBitAnd(value));
  Out.Store<uint64_t>(base+72,WaveActiveBitOr(value));
  Out.Store<uint64_t>(base+80,WaveActiveBitXor(value));
  Out.Store<uint64_t>(base+88,WaveActiveProduct(product));
  Out.Store<uint64_t>(base+96,WavePrefixProduct(product));
  Out.Store(base+104,uint(WaveActiveSum(small)));
  Out.Store(base+108,uint(WavePrefixSum(small)));
  Out.Store(base+112,uint(WaveActiveMin(small)));
  Out.Store(base+116,uint(WaveActiveMax(small)));
  Out.Store(base+120,uint(int(WaveActiveMin(signedSmall))));
  Out.Store(base+124,uint(int(WaveActiveMax(signedSmall))));
  Out.Store(base+128,WaveActiveCountBits((lane%5)==0));
  Out.Store(base+132,WavePrefixCountBits((lane%5)==0));
  Out.Store(base+136,WaveActiveAnyTrue(lane==33));
  Out.Store(base+140,WaveActiveAllTrue(lane<WaveGetLaneCount()));
  uint64_t common=(uint64_t(0x80000000u | (gi-lane))<<32) | 0xffffffffu;
  Out.Store(base+144,WaveActiveAllEqual(common));
  Out.Store(base+148,WaveActiveAllEqual(common ^ (uint64_t(lane >= 32)<<63)));
  Out.Store(base+152,WaveActiveAllEqual(common ^ uint64_t(lane == 16)));
}
[WaveSize(64)] [numthreads(173,1,1)] void cs_wide_exact64(uint gi:SV_GroupIndex) {exact_values(gi);}
[WaveSize(128)] [numthreads(161,1,1)] void cs_wide_exact128(uint gi:SV_GroupIndex) {exact_values(gi);}
[WaveSize(64)] [numthreads(17,1,1)] void cs_wide_exact64_short(uint gi:SV_GroupIndex) {exact_values(gi);}
[WaveSize(128)] [numthreads(17,1,1)] void cs_wide_exact128_short(uint gi:SV_GroupIndex) {exact_values(gi);}
