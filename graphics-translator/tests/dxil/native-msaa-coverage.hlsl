// Exact consumed-coverage control for owned-d3d12-multisample-native.inc.
struct Output { float4 color : SV_Target0; float depth : SV_Depth; };
Output main(uint mask : SV_Coverage) {
  Output o;
  o.color = mask ? float4(.0625, .25, .5, 1) : 0;
  o.depth = .375;
  return o;
}
