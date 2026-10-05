// DXIL 1.9 vector ABI and long-vector GPU oracle (DXC 1.9.2609.5).
#define VECTOR_ROOT "RootConstants(num32BitConstants=4,b0),CBV(b1),DescriptorTable(UAV(u0,numDescriptors=3)),DescriptorTable(SRV(t0,numDescriptors=7))"
#define VECTOR_HEAP_ROOT "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," VECTOR_ROOT
cbuffer Params : register(b0) { uint index; uint unused1; uint unused2; uint unused3; };
RWByteAddressBuffer Output : register(u0);
RWByteAddressBuffer Input : register(u1);
groupshared vector<uint, 8> sharedValues[32];
static const vector<uint, 5> staticValues = {11, 13, 17, 19, 23};

[noinline] vector<uint, 5> helper(vector<uint, 5> value, uint at) {
  value[at] += 7;
  return value;
}

[numthreads(32, 1, 1)]
void cs_vectors(uint lane : SV_GroupIndex) {
  uint offset = (lane % 3 == 0) ? 0 : ((lane % 3 == 1) ? 16 : 0xfffffff0);
  uint status;
  vector<uint, 8> loaded = Input.Load<vector<uint, 8> >(offset, status);
  sharedValues[lane] = loaded;
  GroupMemoryBarrierWithGroupSync();
  vector<uint, 8> local = sharedValues[(lane + 1) & 31];
  uint at = index % 5;
  vector<uint, 5> values = helper(staticValues, at);
  values[(at + 1) % 5] = lane + 100;
  vector<float, 5> floating = (vector<float, 5>)values;
  precise float product = dot(floating, (vector<float, 5>)2.0);
  vector<uint, 8> wave = (vector<uint, 8>)7;
  wave[7] = lane & 1;
  uint4 match = WaveMatch(wave);
  uint base = lane * 128;
  Output.Store<vector<uint, 8> >(base, loaded);
  Output.Store<vector<uint, 8> >(base + 32, local);
  Output.Store(base + 64, values[at]);
  Output.Store(base + 68, values[(at + 1) % 5]);
  Output.Store(base + 72, asuint(product));
  Output.Store(base + 76, all(loaded != 0));
  Output.Store(base + 80, any(loaded != 0));
  Output.Store(base + 84, all(WaveActiveAllEqual(wave)));
  Output.Store(base + 88, match.x);
  Output.Store(base + 92, CheckAccessFullyMapped(status));
  Output.Store<vector<uint, 8> >(base + 96, countbits(loaded));
}

[numthreads(1, 1, 1)]
void cs_vectors1024() {
  vector<uint, 1024> values = (vector<uint, 1024>)0;
  uint at = index & 1023;
  values[at] = Input.Load(0) + at;
  Output.Store(0, values[at]);
  Output.Store(4, values[(at + 1) & 1023]);
  Output.Store(8, any(values != 0));
  Output.Store(12, all(values != 0));
}

[numthreads(1, 1, 1)]
void cs_vector_denorm() {
  vector<float, 5> value = Input.Load<vector<float, 5> >(0);
  precise vector<float, 5> doubled = value + value;
  Output.Store<vector<float, 5> >(0, doubled);
  Output.Store(20, asuint(dot(value, (vector<float, 5>)2.0)));
}

[numthreads(1, 1, 1)]
void cs_vector_double() {
  vector<double, 5> value = Input.Load<vector<double, 5> >(0);
  precise vector<double, 5> result = value * (vector<double, 5>)2.0 + (vector<double, 5>)1.0;
  Output.Store<vector<double, 5> >(0, result);
  Output.Store(40, all(WaveActiveAllEqual(value)));
}

[numthreads(1, 1, 1)]
void cs_vector_uint64() {
  vector<uint64_t, 5> value = Input.Load<vector<uint64_t, 5> >(0);
  vector<uint64_t, 5> result = (value << (vector<uint64_t, 5>)17) ^ (value + (vector<uint64_t, 5>)1);
  Output.Store<vector<uint64_t, 5> >(0, result);
  Output.Store(40, all(WaveActiveAllEqual(value)));
}

[numthreads(1, 1, 1)]
void cs_vector_half() {
  vector<float16_t, 8> value = Input.Load<vector<float16_t, 8> >(0);
  precise vector<float16_t, 8> result = value * (vector<float16_t, 8>)2.0h;
  Output.Store<vector<float16_t, 8> >(0, result);
  Output.Store(16, asuint((float)dot(value, (vector<float16_t, 8>)1.0h)));
}

RWStructuredBuffer<vector<uint, 8> > Structured : register(u2);
[numthreads(1, 1, 1)]
void cs_vector_structured() {
  vector<uint, 8> value = Structured[index];
  Structured[index + 1] = value + (vector<uint, 8>)10;
  Output.Store<vector<uint, 8> >(0, value);
}

[numthreads(1, 1, 1)]
void cs_vector_heap() {
  RWByteAddressBuffer source = ResourceDescriptorHeap[1];
  vector<uint, 8> value = source.Load<vector<uint, 8> >(index);
  Output.Store<vector<uint, 8> >(0, value);
  source.Store<vector<uint, 8> >(index + 16, value + (vector<uint, 8>)10);
}

[numthreads(1, 1, 1)]
void cs_vector_tiles() {
  uint status;
  vector<uint, 8> value = Input.Load<vector<uint, 8> >(index, status);
  Output.Store<vector<uint, 8> >(0, value);
  Output.Store(32, CheckAccessFullyMapped(status));
}

[numthreads(1, 1, 1)]
void cs_vector_ssa() {
  vector<uint, 5> value = Input.Load<vector<uint, 5> >(0);
  uint iterations = index % 4 + 1;
  [loop] for (uint at = 0; at < iterations; ++at) {
    value = (index & 1) ? value + (vector<uint, 5>)(at + 1) : value * (vector<uint, 5>)2;
    value[(index + at) % 5] ^= 0x55;
  }
  Output.Store<vector<uint, 5> >(0, value);
}

float4 vs_vectors(uint vertex : SV_VertexID) : SV_Position {
  vector<uint, 5> value = helper(staticValues, vertex);
  Output.Store<vector<uint, 5> >(1536 + vertex * 32, value);
  float2 positions[3] = {float2(-1,-1),float2(-1,3),float2(3,-1)};
  return float4(positions[vertex], 0, 1);
}

float4 ps_vectors(float4 position : SV_Position) : SV_Target {
  uint pixel = (uint)position.y * 8 + (uint)position.x;
  vector<uint, 5> value = helper(staticValues, pixel % 5);
  Output.Store<vector<uint, 5> >(pixel * 24, value);
  Output.Store(pixel * 24 + 20, any(value == 24));
  return float4(0,0,0,1);
}
