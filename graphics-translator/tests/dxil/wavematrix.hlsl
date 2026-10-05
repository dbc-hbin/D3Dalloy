RWByteAddressBuffer outputBuffer : register(u0);
RWByteAddressBuffer inputBuffer : register(u1);
groupshared float sharedData[512];
groupshared int8_t4_packed signedData[64];
groupshared uint8_t4_packed unsignedData[64];
groupshared float largeData[2048];
[WaveSize(32)]
[numthreads(32, 1, 1)]
void cs_wavematrix(uint lane : SV_GroupIndex)
{
    WaveMatrixLeft<float32_t, 16, 16> a;
    WaveMatrixRight<float32_t, 16, 16> b;
    WaveMatrixAccumulator<float32_t, 16, 16> c;
    WaveMatrixLeftColAcc<float32_t, 16, 16> l;
    WaveMatrixRightRowAcc<float32_t, 16, 16> r;
    a.Load(inputBuffer, 0, 64, false, 4);
    b.Load(inputBuffer, 1024, 64, true, 4);
    a.Store(sharedData, 0, 16, false);
    b.Store(sharedData, 256, 16, true);
    GroupMemoryBarrierWithGroupSync();
    a.Load(sharedData, 0, 16, false);
    b.Load(sharedData, 256, 16, true);
    c.Fill(0.0f);
    c.Multiply(a,b);
    c.MultiplyAccumulate(a,b);
    c.ScalarAdd(3.0f);
    c.ScalarSubtract(1.0f);
    c.ScalarMultiply(2.0f);
    c.ScalarDivide(2.0f);
    l.Fill(0.0f);
    r.Fill(0.0f);
    l.SumAccumulate(a);
    r.SumAccumulate(b);
    c.Add(l);
    c.Add(r);
    c.Store(outputBuffer, 0, 64, false, 4);
    l.Store(outputBuffer, 1024, 4, 4);
    r.Store(outputBuffer, 1088, 4, 4);
    if (lane==0) outputBuffer.Store(1152,a.MatrixDepth());
    WaveMatrixLeft<float16_t, 16, 16> ah;
    WaveMatrixRight<float16_t, 16, 16> bh;
    WaveMatrixAccumulator<float32_t, 16, 16> ch;
    ah.Fill(float16_t(2.0f));
    bh.Fill(float16_t(3.0f));
    ch.Multiply(ah,bh);
    ch.Store(outputBuffer, 1280, 64, false, 4);
    WaveMatrixLeft<int8_t4_packed, 16, 16> ai;
    WaveMatrixRight<uint8_t4_packed, 16, 16> bi;
    WaveMatrixAccumulator<int32_t, 16, 16> ci;
    ai.Fill(int8_t4_packed(0xffffffff));
    bi.Fill(uint8_t4_packed(0x02020202));
    ci.Multiply(ai,bi);
    ci.Store(outputBuffer, 2304, 64, false, 4);
    WaveMatrixLeft<uint8_t4_packed, 16, 16> au;
    WaveMatrixRight<int8_t4_packed, 16, 16> bs;
    ai.Fill(int8_t4_packed(0xfe7f80ff));
    bi.Fill(uint8_t4_packed(0x0103ff80));
    au.Fill(uint8_t4_packed(0x0103ff80));
    bs.Fill(int8_t4_packed(0xfe7f80ff));
    ai.Store(signedData,0,4,false);
    bi.Store(unsignedData,0,4,true);
    GroupMemoryBarrierWithGroupSync();
    ai.Load(signedData,0,4,false);
    bi.Load(unsignedData,0,4,true);
    WaveMatrixLeftColAcc<int32_t,16,16> sl;
    WaveMatrixRightRowAcc<int32_t,16,16> ur;
    sl.Fill(0);ur.Fill(0);
    sl.SumAccumulate(ai);ur.SumAccumulate(bi);
    ci.Multiply(ai,bi);ci.Add(sl);ci.Add(ur);
    ci.Store(outputBuffer,3456,64,false,4);
    ci.Multiply(au,bs);ci.Store(outputBuffer,4480,64,false,4);
    ci.Multiply(ai,bs);ci.Store(outputBuffer,5504,64,false,4);
    ci.Multiply(au,bi);ci.Store(outputBuffer,6528,64,false,4);
    sl.Store(outputBuffer,7552,4,4);ur.Store(outputBuffer,7616,4,4);
    // Products remain subnormal until the final term; input and accumulation must not flush.
    WaveMatrixAccumulator<float16_t,16,16> hh;
    ah.Fill(float16_t(0.000030517578125f));bh.Fill(float16_t(0.125f));
    hh.Multiply(ah,bh);hh.Store(outputBuffer,7808,32,false,4);
    ch.Multiply(ah,bh);ch.Store(outputBuffer,8384,64,false,4);
}

template<uint M,uint N>
void largeWaveMatrix() {
    WaveMatrixLeft<float32_t,M,N> a;
    WaveMatrixRight<float32_t,M,N> b;
    WaveMatrixAccumulator<float32_t,M,N> c;
    WaveMatrixLeftColAcc<float32_t,M,N> l;
    WaveMatrixRightRowAcc<float32_t,M,N> r;
    a.Load(inputBuffer,32,80,false,4);
    b.Load(inputBuffer,8192,80,true,4);
    a.Store(largeData,0,16,false);
    b.Store(largeData,1024,16,true);
    GroupMemoryBarrierWithGroupSync();
    a.Load(largeData,0,16,false);
    b.Load(largeData,1024,16,true);
    c.Multiply(a,b);c.MultiplyAccumulate(a,b);
    l.Fill(0.0f);r.Fill(0.0f);l.SumAccumulate(a);r.SumAccumulate(b);
    c.Add(l);c.Add(r);
    c.Store(outputBuffer,32,N*4+16,false,4);
    l.Store(outputBuffer,32+M*(N*4+16)+32,8,4);
    r.Store(outputBuffer,32+M*(N*4+16)+32+M*8+32,8,4);
}
[WaveSize(32)] [numthreads(32,1,1)]
void cs_wavematrix16x64() { largeWaveMatrix<16,64>(); }
[WaveSize(32)] [numthreads(32,1,1)]
void cs_wavematrix64x16() { largeWaveMatrix<64,16>(); }
[WaveSize(32)] [numthreads(32,1,1)]
void cs_wavematrix64x64() { largeWaveMatrix<64,64>(); }
