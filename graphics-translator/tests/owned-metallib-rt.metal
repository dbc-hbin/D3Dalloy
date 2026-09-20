#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;
struct Payload { uint calls; float bary; };
struct Result { uint type; uint calls; float bary; float distance; uint primitive; uint geometry; uint instance; uint userID; };
[[intersection(triangle, triangle_data, instancing)]]
bool accept_triangle(float2 bary [[barycentric_coord]], ray_data Payload &p [[payload]]) {
  ++p.calls; p.bary=bary.x;
  return bary.x > 0.3f;
}
[[visible]] uint marker(uint v) { return v + 7; }
kernel void trace_contract(instance_acceleration_structure as [[buffer(0)]],
 intersection_function_table<triangle_data, instancing> ift [[buffer(1)]],
 device Result *output [[buffer(2)]], uint index [[thread_position_in_grid]]) {
 ray r; r.origin=float3(index==0 ? 0.5f : 0.0f,0,0); r.direction=float3(0,0,1);
 r.min_distance=0; r.max_distance=10;
 Payload p={0,0}; intersector<triangle_data, instancing> traversal;
 auto hit=traversal.intersect(r,as,ift,p);
 Result result={uint(hit.type),p.calls,p.bary,hit.distance,hit.primitive_id,hit.geometry_id,hit.instance_id,hit.user_instance_id};
 output[index]=result;
}
