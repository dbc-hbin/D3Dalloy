// Rebuild with upstream DXC: dxc -T vs_6_0 -E vsLine -HV 2021 -O0 -Fo msaa-lines-vsLine.dxil msaa-lines-input.hlsl
// Compile vsLineEdge with -T vs_6_0, and psEval, psSample,
// psUnusedSampleDerivative, psSampleDerivative with -T ps_6_0 and matching -Fo names.
RWByteAddressBuffer invocationCounter : register(u0);
struct LineOracle {
    float4 position : SV_Position;
    noperspective float value : TEXCOORD0;
    centroid noperspective float centroid_value : TEXCOORD1;
    sample noperspective float sample_value : TEXCOORD2;
};
LineOracle vsLine(uint id : SV_VertexID) {
    LineOracle v;
    float x = id ? 26.0 : 6.0;
    v.position = float4(x/16.0-1.0, 1.0-16.5/16.0, 0.3, 1);
    v.value = v.centroid_value = v.sample_value = (x-6.0)/20.0;
    return v;
}
LineOracle vsLineEdge(uint id : SV_VertexID) {
    LineOracle v = vsLine(id);
    v.position.y = 1.0-15.75/16.0;
    return v;
}
struct LineEval {
    float4 position : SV_Position;
    noperspective float value : TEXCOORD0;
    centroid noperspective float centroid_value : TEXCOORD1;
};
float4 psEval(LineEval v) : SV_Target {
    return float4(EvaluateAttributeAtSample(v.value, 0),
                  EvaluateAttributeAtSample(v.value, 3),
                  EvaluateAttributeSnapped(v.value, int2(0, 0)),
                  (ddx(v.value) + abs(ddy(v.value)))*4);
}
float4 psUnusedSampleDerivative(LineOracle v) : SV_Target {
    // Sample-qualified TEXCOORD2 is intentionally unused. Its mere presence
    // must not change pixel-frequency ddy of horizontal TEXCOORD0.
    return float4(v.value, v.centroid_value, 0, abs(ddy(v.value))*4);
}
float4 psSample(LineOracle v, uint sample_id : SV_SampleIndex) : SV_Target {
    float evaluated = EvaluateAttributeAtSample(v.value, sample_id);
    // Isolate and magnify two sample lanes before resolve, so their distinct
    // positions cannot disappear into the four-sample average.
    return float4(sample_id == 0 ? 0.5 + (v.sample_value - 0.525)*32 : 0,
                  sample_id == 3 ? 0.5 + (evaluated - 0.525)*32 : 0,
                  0.5 + (v.centroid_value - 0.525)*32, 1);
}
float4 psSampleDerivative(LineOracle v, uint sample_id : SV_SampleIndex) : SV_Target {
    uint old_count;
    invocationCounter.InterlockedAdd(0, 1, old_count);
    float evaluated = EvaluateAttributeAtSample(v.value, sample_id);
    return float4(sample_id == 0 ? 0.5 + (v.sample_value - 0.525)*32 : 0,
                  sample_id == 3 ? 0.5 + (evaluated - 0.525)*32 : 0,
                  0.5 + (v.centroid_value - 0.525)*32,
                  (ddx(v.value) + abs(ddy(v.value)))*4);
}
