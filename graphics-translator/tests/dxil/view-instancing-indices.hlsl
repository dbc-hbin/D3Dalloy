// Adds authored viewport/layer indices to the shared view contract without changing its frozen shader entries.
#include "view-instancing.hlsl"

struct IndexedVOut {
  float4 pos : SV_Position;
  nointerpolation uint up : VIEWUP;
  uint viewport : SV_ViewportArrayIndex;
  uint layer : SV_RenderTargetArrayIndex;
};

IndexedVOut vs_indices(uint vid : SV_VertexID, uint view : SV_ViewID) {
  IndexedVOut o;
  o.pos = corner(vid);
  o.up = 0x100u | view;
  o.viewport = authored_viewport;
  o.layer = authored_layer;
  return o;
}
