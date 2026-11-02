#include <metal_stdlib>
using namespace metal;

// AMD signal conversion and corrections around MetalFX neural radiance denoising.
struct RrParams {
    uint width, height, signal, checkerboard;
    uint origin, flags, hasHistory, legacyMode;
    float normalStrength, stability, maxRadiance, clipK;
    float relaxation, depthThreshold, depthMin, depthMax;
    float motionScale[3], jitter[2], previousJitter[2], cameraDelta[3];
    float view[16], inverseProjection[16], inverseView[16];
    float lightDirection[3], lightEmission[3], lightRadius;
    float historyStrength;
    uint apiVersion;
    float projection[16];
    float previousView[16];
};

float4x4 loadMatrix(constant float* m) {
    return float4x4(float4(m[0],m[1],m[2],m[3]), float4(m[4],m[5],m[6],m[7]),
                    float4(m[8],m[9],m[10],m[11]), float4(m[12],m[13],m[14],m[15]));
}
kernel void rr_zero_albedo(texture2d<float,access::write> output [[texture(0)]],
    constant RrParams& p [[buffer(0)]], uint2 tid [[thread_position_in_grid]]) {
    if (tid.x < p.width && tid.y < p.height) output.write(float4(0),tid);
}
bool radiance(uint s) { return s == 1 || s == 2 || s == 4 || s == 5; }
bool specular(uint s) { return s == 2 || s == 5 || s == 6; }
float3 octNormal(float2 e) {
    float2 f = e * 2.0f - 1.0f;
    float3 n = float3(f, 1.0f - abs(f.x) - abs(f.y));
    if (n.z < 0.0f) n.xy = (1.0f - abs(n.yx)) * select(float2(-1), float2(1), n.xy >= 0);
    return normalize(n);
}
float3 viewPosition(uint2 p, float z, constant RrParams& c) {
    float2 uv = (float2(p) + 0.5f - float2(c.jitter[0], c.jitter[1])) / float2(c.width,c.height);
    float2 ndc = uv * float2(2,-2) + float2(-1,1);
    float4x4 inv = loadMatrix(c.inverseProjection);
    // Eliminate the homogeneous interpolation parameter before dividing:
    // neither an infinite far endpoint nor view-Z zero needs a singular t.
    float4 a = inv * float4(ndc,0,1), delta = inv[2];
    float numerator = z*a.w-a.z, denominator = delta.z-z*delta.w;
    return (a.xyz*denominator+delta.xyz*numerator)/(a.w*delta.z-delta.w*a.z);
}
float3 albedo(float4 t, constant RrParams& c) {
    float3 a = max(t.xyz, 0.0f);
    return (c.flags & 2) ? a : a*a;
}
float3 albedo(texture2d<float,access::read> t, uint2 p, constant RrParams& c) { return albedo(t.read(p),c); }
float3 diffuseAlbedo(float4 t, constant RrParams& c) {
    float3 value = albedo(t,c);
    if (c.apiVersion == 0x00400000) {
        float metallic = t.a;
        if (!(c.flags&2)) metallic *= metallic;
        value *= 1-clamp(metallic,0.0f,1.0f);
    }
    return value;
}
float3 diffuseAlbedo(texture2d<float,access::read> t, uint2 p, constant RrParams& c) {
    return diffuseAlbedo(t.read(p),c);
}
bool traced(uint2 p, constant RrParams& c) {
    return !c.checkerboard || ((p.x&1) == ((p.y&1)^c.origin));
}
bool activeSample(float4 value, uint2 p, constant RrParams& c) {
    // Direct radiance alpha is undefined; only indirect hit distances and
    // unpacked scalar validity use its sign.
    return traced(p,c) && ((c.signal == 1 || c.signal == 2) || value.a >= 0);
}
bool inside(int2 p, constant RrParams& c) {
    return all(p >= 0) && all(p < int2(c.width,c.height));
}
bool bounded(float z, constant RrParams& c) {
    return isfinite(z) && abs(z) >= c.depthMin && abs(z) <= c.depthMax;
}
float deviceDepth(uint2 p, float z, constant RrParams& c) {
    float4 clip = loadMatrix(c.projection)*float4(viewPosition(p,z,c),1);
    return clip.w != 0 ? clip.z/clip.w : INFINITY;
}
struct Neighborhood {
    float3 mean, variance;
    float alpha;
    bool valid, materialBoundary;
};
bool usesSpecularAlbedo(constant RrParams& c) { return c.signal == 2 || c.signal == 3 || c.signal == 5; }
bool usesDiffuseAlbedo(constant RrParams& c) {
    return c.signal == 1 || c.signal == 3 || c.signal == 4 || (c.legacyMode == 3 && c.signal == 5);
}
// The 7x7 neighborhood of an 8x8 threadgroup, staged once in threadgroup
// memory. Callers dispatch complete RrTileSize x RrTileSize groups covering
// the image; threads past its edge help fill the tile and then return. Only
// the channels neighborhood() reads are kept (15680 bytes), so the tile stays
// within 32 KiB even when Metal shader validation doubles threadgroup memory.
constant constexpr uint RrTileSize = 8, RrTileSpan = RrTileSize+6;
struct RrTile {
    float2 normals[RrTileSpan][RrTileSpan]; // packed normal (z, material a)
    float4 source[RrTileSpan][RrTileSpan], guide[RrTileSpan][RrTileSpan];
    packed_float3 decoded[RrTileSpan][RrTileSpan], spec[RrTileSpan][RrTileSpan];
    float4 diff[RrTileSpan][RrTileSpan];
};
// Every thread of the group must call this before any early return. Taps
// outside the image are never stored; neighborhood() skips them unread.
void loadTile(threadgroup RrTile& t, texture2d<float,access::read> source,
              texture2d<float,access::read> guide, texture2d<float,access::read> normals,
              texture2d<float,access::read> decoded, texture2d<float,access::read> specAlbedo,
              texture2d<float,access::read> diffAlbedo, uint index, uint2 group, constant RrParams& c) {
    bool useSpec = usesSpecularAlbedo(c), useDiff = usesDiffuseAlbedo(c);
    int2 origin = int2(group*RrTileSize)-3;
    for (uint i=index;i<RrTileSpan*RrTileSpan;i+=RrTileSize*RrTileSize) {
        uint2 l = uint2(i%RrTileSpan,i/RrTileSpan);
        int2 q = origin+int2(l);
        if (!inside(q,c)) continue;
        uint2 u = uint2(q);
        t.normals[l.y][l.x] = normals.read(u).zw;
        t.source[l.y][l.x] = source.read(u);
        t.guide[l.y][l.x] = guide.read(u);
        t.decoded[l.y][l.x] = packed_float3(decoded.read(u).xyz);
        if (useSpec) t.spec[l.y][l.x] = packed_float3(specAlbedo.read(u).xyz);
        if (useDiff) t.diff[l.y][l.x] = diffAlbedo.read(u);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
}
// Signal-independent geometry is decoded once per frame by rr_geometry:
// guide = (view position, raw depth), decoded = (octahedral normal, 0).
Neighborhood neighborhood(texture2d<float,access::read> source,
                          texture2d<float,access::read> guide,
                          texture2d<float,access::read> normals,
                          texture2d<float,access::read> decoded,
                          texture2d<float,access::read> specAlbedo,
                          texture2d<float,access::read> diffAlbedo,
                          texture2d<float,access::read> fusedAlbedo,
                          threadgroup const RrTile& tile, uint2 local,
                          uint2 p, constant RrParams& c) {
    float4 center = guide.read(p);
    float z = center.w;
    float4 raw = source.read(p), packed = normals.read(p);
    float3 n = decoded.read(p).xyz;
    float3 vn = normalize((loadMatrix(c.view)*float4(n,0)).xyz);
    float3 position = center.xyz;
    bool useSpec = usesSpecularAlbedo(c), useDiff = usesDiffuseAlbedo(c);
    bool composite = c.legacyMode == 3 && c.signal == 5;
    float3 sa = useSpec ? albedo(specAlbedo,p,c) : float3(0);
    float3 da = useDiff ? diffuseAlbedo(diffAlbedo,p,c) : float3(0);
    float3 fa = composite ? albedo(fusedAlbedo,p,c) : float3(0);
    float specWeight = specular(c.signal) ? 1.0f : 0.0f;
    if (composite) specWeight = dot(sa,float3(1))/max(dot(sa+da,float3(1)),1e-6f);
    float sigma = 0.65f+1.85f*c.relaxation;
    if (c.signal == 3 && activeSample(raw,p,c) && raw.x < 65504.0f) {
        uint neighbor = p.x+1 < c.width ? p.x+1 : (p.x ? p.x-1 : 0);
        float pixelWorld = length(viewPosition(uint2(neighbor,p.y),z,c)-position);
        float facing = max(abs(dot(n,-normalize(float3(c.lightDirection[0],c.lightDirection[1],c.lightDirection[2])))),0.1f);
        sigma = clamp(sigma+raw.x*tan(c.lightRadius)/max(pixelWorld*facing,1e-6f),0.65f,4.0f);
    }
    float3 sum = 0, squareSum = 0;
    float weights = 0, alpha = -1, nearestDistance = INFINITY;
    bool boundary = false, inBounds = bounded(z,c);
    float tolerance = max(abs(z)*c.depthThreshold,1e-5f);
    for (int y=-3;y<=3;++y) for (int x=-3;x<=3;++x) {
        int2 q = int2(p)+int2(x,y);
        if (!inside(q,c)) continue;
        uint2 t = local+uint2(x+3,y+3);
        float2 np = tile.normals[t.y][t.x];
        if (round(np.y*3) != round(packed.a*3)) { boundary = true; continue; }
        float4 v = tile.source[t.y][t.x];
        if (!activeSample(v,uint2(q),c)) continue;
        float4 qg = tile.guide[t.y][t.x];
        if (inBounds && !bounded(qg.w,c)) continue;
        float3 delta = qg.xyz-position;
        float exponent = -float(x*x+y*y)/(2*sigma*sigma)
            -abs(dot(vn,delta))/tolerance
            -(1-clamp(dot(n,float3(tile.decoded[t.y][t.x])),0.0f,1.0f))*64*c.normalStrength;
        float3 qs = useSpec ? albedo(float4(float3(tile.spec[t.y][t.x]),0),c) : float3(0);
        float3 qd = useDiff ? diffuseAlbedo(tile.diff[t.y][t.x],c) : float3(0);
        float edge = specular(c.signal) ? length(sa-qs) : length(da-qd);
        if (c.signal == 3) edge = length(sa-qs)+length(da-qd);
        if (composite) edge = length(fa-albedo(fusedAlbedo,uint2(q),c));
        float weight = exp(exponent-edge*8-specWeight*abs(np.x-packed.z)*16);
        float3 value = radiance(c.signal) ? clamp(v.xyz,0.0f,c.maxRadiance) :
            c.signal == 3 ? float3(v.x >= 65504.0f ? 1 : 0) : float3(clamp(v.x,0.0f,1.0f));
        sum += value*weight; squareSum += value*value*weight; weights += weight;
        float distance = float(x*x+y*y);
        if (weight > 1e-6f && distance < nearestDistance) {
            nearestDistance = distance; alpha = v.a;
        }
    }
    float3 mean = weights > 1e-8f ? sum/weights : float3(0);
    return {mean,weights > 1e-8f ? max(squareSum/weights-mean*mean,0.0f) : float3(0),
            alpha,weights > 1e-8f,boundary};
}

// History geometry and inputs requiring unpack or alias protection are copied
// before any output write. Float32 intermediates preserve caller precision.
kernel void rr_snapshot(texture2d<float,access::read> source [[texture(0)]],
                        texture2d<float,access::write> destination [[texture(1)]],
                        constant RrParams& c [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
    if (p.x >= c.width || p.y >= c.height) return;
    destination.write(source.read(p), p);
}

// Missing checkerboard samples and negative-alpha radiance remain explicitly
// inactive until the filter reconstructs them from compatible traced samples.
// Depth and packed-normal snapshots plus the per-frame neighborhood guides.
kernel void rr_geometry(texture2d<float,access::read> depthSource [[texture(0)]],
                        texture2d<float,access::read> normalSource [[texture(1)]],
                        texture2d<float,access::write> depth [[texture(2)]],
                        texture2d<float,access::write> normals [[texture(3)]],
                        texture2d<float,access::write> guide [[texture(4)]],
                        texture2d<float,access::write> decoded [[texture(5)]],
                        constant RrParams& c [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
    if (p.x >= c.width || p.y >= c.height) return;
    float4 d = depthSource.read(p), packed = normalSource.read(p);
    depth.write(d,p);
    normals.write(packed,p);
    guide.write(float4(viewPosition(p,d.x,c),d.x),p);
    decoded.write(float4(octNormal(packed.xy),0),p);
}

kernel void rr_unpack(texture2d<float,access::read> source [[texture(0)]],
                      texture2d<float,access::write> destination [[texture(1)]],
                      constant RrParams& c [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
    if (p.x >= c.width || p.y >= c.height) return;
    bool traced = !c.checkerboard || ((p.x & 1) == ((p.y & 1) ^ c.origin));
    float4 v = source.read(uint2(c.checkerboard ? p.x/2 : p.x,p.y));
    if (!traced) v = float4(0,0,0,-1);
    else if (!radiance(c.signal)) v.a = 1;
    destination.write(v,p);
}

kernel void rr_filter(texture2d<float,access::read> source [[texture(0)]],
                      texture2d<float,access::write> destination [[texture(1)]],
                      texture2d<float,access::read> depth [[texture(2)]],
                      texture2d<float,access::read> motion [[texture(3)]],
                      texture2d<float,access::read> normals [[texture(4)]],
                      texture2d<float,access::read> specAlbedo [[texture(5)]],
                      texture2d<float,access::read> diffAlbedo [[texture(6)]],
                      texture2d<float,access::read> history [[texture(7)]],
                      texture2d<float,access::read> previousDepth [[texture(8)]],
                      texture2d<float,access::read> previousNormals [[texture(9)]],
                      texture2d<float,access::read> fusedAlbedo [[texture(10)]],
                      texture2d<float,access::read> guide [[texture(11)]],
                      texture2d<float,access::read> decoded [[texture(12)]],
                      constant RrParams& c [[buffer(0)]], uint2 p [[thread_position_in_grid]],
                      uint2 lp [[thread_position_in_threadgroup]], uint li [[thread_index_in_threadgroup]],
                      uint2 group [[threadgroup_position_in_grid]]) {
    threadgroup RrTile tile;
    loadTile(tile,source,guide,normals,decoded,specAlbedo,diffAlbedo,li,group,c);
    if (p.x >= c.width || p.y >= c.height) return;
    float z = depth.read(p).x;
    float4 raw = source.read(p), packed = normals.read(p);
    bool active = raw.a >= 0;
    bool inBounds = bounded(z,c);
    // Full-resolution inactive pixels outside bounds are literal passthrough.
    if (!inBounds && (!c.checkerboard || active)) {
        destination.write(raw,p); return;
    }
    float3 n = octNormal(packed.xy);
    float3 position = viewPosition(p,z,c);
    float specWeight = specular(c.signal) ? 1.0f : 0.0f;
    Neighborhood local = neighborhood(source,guide,normals,decoded,specAlbedo,diffAlbedo,fusedAlbedo,tile,lp,p,c);
    float3 mean = local.mean;
    float4 result = float4(mean,active ? raw.a : local.alpha);
    bool valid = local.valid;
    if (c.hasHistory && inBounds) {
        float3 mv = motion.read(p).xyz * float3(c.motionScale[0],c.motionScale[1],c.motionScale[2]);
        float2 uv = (float2(p)+0.5f)/float2(c.width,c.height) + mv.xy;
        // Vectors are dejittered. Add the raster-grid displacement exactly once.
        uv += (float2(c.previousJitter[0],c.previousJitter[1])-float2(c.jitter[0],c.jitter[1]))/float2(c.width,c.height);
        float2 hp = uv*float2(c.width,c.height)-0.5f;
        int2 base = int2(floor(hp));
        float2 f = fract(hp);
        float3 hsum = 0; float hw = 0, ha = 0;
        for (int y=0;y<2;++y) for (int x=0;x<2;++x) {
            int2 q = base+int2(x,y);
            if (!inside(q,c)) continue;
            float pz = previousDepth.read(uint2(q)).x;
            float4 pn = previousNormals.read(uint2(q)), h = history.read(uint2(q));
            if (h.a < 0 || !bounded(pz,c) || abs(pz-(z+mv.z)) > max(abs(z+mv.z)*c.depthThreshold,1e-5f) || round(pn.a*3)!=round(packed.a*3)) continue;
            float w = (x ? f.x : 1-f.x)*(y ? f.y : 1-f.y);
            w *= exp(-(1-clamp(dot(n,octNormal(pn.xy)),0.0f,1.0f))*64*c.normalStrength);
            w *= exp(-specWeight*abs(pn.z-packed.z)*16);
            // RR 1.0 strength is an additional confidence rejection, not a
            // disocclusion threshold. Hard invalid-history checks stay above.
            float residual = abs(pz-(z+mv.z))/max(abs(z+mv.z)*c.depthThreshold,1e-5f);
            w *= 1.0f-c.historyStrength*clamp(residual,0.0f,1.0f);
            hsum += h.xyz*w; ha += h.a*w; hw += w;
        }
        if (hw > 1e-6f) {
            float3 h = hsum/hw;
            float confidence = 1;
            if (specular(c.signal)) {
                // Camera translation changes the reflected view even when the
                // primary surface reprojects exactly. Reduce glossy history by
                // the view-angle change, amplified for long secondary rays.
                float3 relative = (loadMatrix(c.inverseView)*float4(position,0)).xyz;
                float3 camera = float3(c.cameraDelta[0],c.cameraDelta[1],c.cameraDelta[2]);
                float angular = 1-clamp(dot(normalize(relative),normalize(relative-camera)),-1.0f,1.0f);
                confidence = exp(-specWeight*angular*32/max(packed.z*packed.z,0.02f));
            }
            float blend = valid ? (0.5f+0.48f*c.stability)*min(hw,1.0f)*confidence : 1;
            if (c.signal == 3 && valid) {
                // Bright emitters favor responsiveness at shadow boundaries.
                float energy = dot(float3(c.lightEmission[0],c.lightEmission[1],c.lightEmission[2]),float3(0.2126f,0.7152f,0.0722f));
                blend *= 1/(1+abs(h.x-mean.x)*log2(1+energy)*0.1f);
            }
            result.xyz = mix(mean,h,blend);
            if (!active && !valid) result.a = ha/hw;
            valid = true;
        }
    }
    if (!valid) result = active ? raw : float4(0,0,0,-1);
    result = float4(clamp(result.x,0.0f,1.0f),0,0,valid ? 1.0f : -1.0f);
    destination.write(result,p);
}

kernel void rr_neural_common(texture2d<float,access::read> depth [[texture(0)]],
                             texture2d<float,access::read> motion [[texture(1)]],
                             texture2d<float,access::read> packedNormals [[texture(2)]],
                             texture2d<float,access::read> specAlbedo [[texture(3)]],
                             texture2d<float,access::read> diffAlbedo [[texture(4)]],
                             texture2d<float,access::write> projectedDepth [[texture(5)]],
                             texture2d<float,access::write> pixelMotion [[texture(6)]],
                             texture2d<float,access::write> roughness [[texture(8)]],
                             texture2d<float,access::write> diffuse [[texture(9)]],
                             texture2d<float,access::write> specular [[texture(10)]],
                             constant RrParams& c [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
    if (p.x >= c.width || p.y >= c.height) return;
    float d = deviceDepth(p,depth.read(p).x,c);
    float4x4 inv = loadMatrix(c.inverseProjection);
    float4 a = inv[3], b = a+inv[2];
    bool reversed = abs(a.z*b.w) > abs(b.z*a.w);
    // Invalid projection pixels are handled by the correction pass. Never
    // inject infinities into the network's neighboring receptive fields.
    projectedDepth.write(float4(isfinite(d) ? clamp(d,0.0f,1.0f) : reversed ? 0.0f : 1.0f),p);
    float2 mv = motion.read(p).xy*float2(c.motionScale[0]*c.width,c.motionScale[1]*c.height);
    pixelMotion.write(float4(mv,0,0),p);
    // rr_geometry's decoded guide is the scaler's world normal input.
    float4 packed = packedNormals.read(p);
    roughness.write(float4(packed.z),p);
    diffuse.write(float4(diffuseAlbedo(diffAlbedo,p,c),0),p);
    specular.write(float4(albedo(specAlbedo,p,c),0),p);
}

kernel void rr_neural_prepare(texture2d<float,access::read> source [[texture(0)]],
                              texture2d<float,access::read> depth [[texture(1)]],
                              texture2d<float,access::read> motion [[texture(2)]],
                              texture2d<float,access::read> normals [[texture(3)]],
                              texture2d<float,access::read> specAlbedo [[texture(4)]],
                              texture2d<float,access::read> diffAlbedo [[texture(5)]],
                              texture2d<float,access::read> fusedAlbedo [[texture(6)]],
                              texture2d<float,access::read> previousDepth [[texture(7)]],
                              texture2d<float,access::read> previousNormals [[texture(8)]],
                              texture2d<float,access::read> history [[texture(9)]],
                              texture2d<float,access::write> color [[texture(10)]],
                              texture2d<float,access::write> reactive [[texture(11)]],
                              texture2d<float,access::write> strength [[texture(12)]],
                              texture2d<float,access::write> lower [[texture(13)]],
                              texture2d<float,access::write> upper [[texture(14)]],
                              texture2d<float,access::write> correction [[texture(15)]],
                              texture2d<float,access::write> hitDistance [[texture(16)]],
                              texture2d<float,access::read> previousAlpha [[texture(17)]],
                              texture2d<float,access::read> guide [[texture(18)]],
                              texture2d<float,access::read> decoded [[texture(19)]],
                              constant RrParams& c [[buffer(0)]], uint2 p [[thread_position_in_grid]],
                              uint2 lp [[thread_position_in_threadgroup]], uint li [[thread_index_in_threadgroup]],
                              uint2 group [[threadgroup_position_in_grid]]) {
    threadgroup RrTile tile;
    loadTile(tile,source,guide,normals,decoded,specAlbedo,diffAlbedo,li,group,c);
    if (p.x >= c.width || p.y >= c.height) return;
    float4 raw = source.read(p), packed = normals.read(p);
    float z = depth.read(p).x;
    bool active = activeSample(raw,p,c), inBounds = bounded(z,c);
    bool passthrough = !inBounds && (!c.checkerboard || active);
    Neighborhood local = {raw.xyz,float3(0),raw.a,true,false};
    if (!passthrough) local = neighborhood(source,guide,normals,decoded,specAlbedo,diffAlbedo,fusedAlbedo,tile,lp,p,c);
    float3 sa = albedo(specAlbedo,p,c), da = diffuseAlbedo(diffAlbedo,p,c);
    bool composite = c.legacyMode == 3;
    float3 modulator = composite ? albedo(fusedAlbedo,p,c) :
        specular(c.signal) ? sa : da;
    float specWeight = specular(c.signal) ? 1.0f : 0.0f;
    if (composite) specWeight = dot(sa,float3(1))/max(dot(sa+da,float3(1)),1e-6f);
    float alpha = active ? raw.a : local.alpha;
    float3 fallback = local.valid ? local.mean : active ? clamp(raw.xyz,0.0f,c.maxRadiance) : float3(0);
    float3 deviation = sqrt(local.variance)*c.clipK;
    float3 hsum = 0; float hw = 0, ha = 0, historyDepth = 0, predictedDepth = z;
    float3 n = octNormal(packed.xy);
    bool depthMotionCorrection = false;
    if (c.hasHistory && inBounds) {
        float3 mv = motion.read(p).xyz*float3(c.motionScale[0],c.motionScale[1],c.motionScale[2]);
        predictedDepth = z+mv.z;
        float3 relative = (loadMatrix(c.inverseView)*float4(viewPosition(p,z,c),0)).xyz;
        float3 cameraDelta = float3(c.cameraDelta[0],c.cameraDelta[1],c.cameraDelta[2]);
        float cameraDepth = (loadMatrix(c.previousView)*float4(relative-cameraDelta,0)).z;
        // MetalFX can reconstruct camera motion from its matrices, but its
        // two-channel motion input cannot convey independent surface Z motion.
        depthMotionCorrection = abs(predictedDepth-cameraDepth) > 1e-5f*max(abs(predictedDepth),1.0f);
        float2 hp = float2(p)+mv.xy*float2(c.width,c.height)
            +float2(c.previousJitter[0]-c.jitter[0],c.previousJitter[1]-c.jitter[1]);
        int2 base = int2(floor(hp)); float2 f = fract(hp);
        for (int y=0;y<2;++y) for (int x=0;x<2;++x) {
            int2 q = base+int2(x,y);
            if (!inside(q,c)) continue;
            float pz = previousDepth.read(uint2(q)).x;
            float4 pn = previousNormals.read(uint2(q)), h = history.read(uint2(q));
            float tolerance = max(abs(z+mv.z)*c.depthThreshold,1e-5f);
            if (h.a < 0 || !bounded(pz,c) || abs(pz-(z+mv.z)) > tolerance ||
                round(pn.a*3) != round(packed.a*3)) continue;
            float w = (x ? f.x : 1-f.x)*(y ? f.y : 1-f.y);
            w *= exp(-(1-clamp(dot(n,octNormal(pn.xy)),0.0f,1.0f))*64*c.normalStrength
                -specWeight*abs(pn.z-packed.z)*16);
            w *= 1-c.historyStrength*clamp(abs(pz-(z+mv.z))/tolerance,0.0f,1.0f);
            hsum += h.xyz*w;
            ha += ((c.signal == 1 || c.signal == 2) && c.checkerboard ? previousAlpha.read(uint2(q)).a : h.a)*w;
            hw += w; historyDepth += pz*w;
        }
    }
    float confidence = 1;
    float3 camera = float3(c.cameraDelta[0],c.cameraDelta[1],c.cameraDelta[2]);
    if (specWeight > 0 && any(camera != 0)) {
        float3 relative = (loadMatrix(c.inverseView)*float4(viewPosition(p,z,c),0)).xyz;
        if (dot(relative,relative) > 0 && dot(relative-camera,relative-camera) > 0) {
            float angular = 1-clamp(dot(normalize(relative),normalize(relative-camera)),-1.0f,1.0f);
            float hit = c.signal == 5 ? max(alpha,0.0f) : 0;
            confidence = exp(-specWeight*angular*(1+hit/max(length(relative),1e-4f))*32/max(packed.z*packed.z,0.02f));
        }
    }
    float blend = (0.5f+0.48f*c.stability)*min(hw,1.0f)*confidence;
    if (hw > 1e-6f) {
        float3 h = hsum/hw;
        if (local.valid) h = clamp(h,local.mean-deviation,local.mean+deviation);
        fallback = mix(fallback,h,local.valid ? blend : 1.0f);
        if (!active && !local.valid) alpha = ha/hw;
    }
    float d = deviceDepth(p,z,c);
    // A reactive hint can reject MetalFX history, but cannot force its private
    // depth test to accept a sample under AMD's configurable tolerance.
    bool historyToleranceCorrection = hw > 1e-6f &&
        abs(historyDepth/hw-predictedDepth) > 1e-5f*max(abs(predictedDepth),1.0f);
    bool needsCorrection = passthrough || local.materialBoundary || depthMotionCorrection ||
        historyToleranceCorrection ||
        !isfinite(d) || d < 0 || d > 1 || (!active && !local.valid && hw <= 1e-6f);
    if (passthrough) fallback = raw.xyz;
    else fallback = clamp(fallback,0.0f,c.maxRadiance);
    float3 input = active ? clamp(raw.xyz,0.0f,c.maxRadiance) : fallback;
    color.write(float4(input*modulator,1),p);
    reactive.write(float4(needsCorrection ? 1.0f : 1-clamp(blend,0.0f,1.0f)),p);
    // Apple's API contract uses one to bypass denoising. Corrections are also
    // explicitly selected after the network, rather than relying on a hint.
    strength.write(float4(needsCorrection ? 1.0f : 0.0f),p);
    lower.write(float4(local.valid ? clamp(local.mean-deviation,0.0f,c.maxRadiance) : float3(0),needsCorrection ? 1.0f : 0.0f),p);
    upper.write(float4(local.valid ? min(local.mean+deviation,c.maxRadiance) : float3(c.maxRadiance),alpha),p);
    float historyAlpha = c.signal == 1 || c.signal == 2 ?
        (active || local.valid || hw > 1e-6f ? 1.0f : -1.0f) : alpha;
    correction.write(float4(fallback,historyAlpha),p);
    if (c.signal == 5) hitDistance.write(float4(max(alpha,0.0f)),p);
}

// Neural correction for one radiance pixel. Returns RGB with the internal
// history validity alpha carried by the prepared fallback.
float4 neuralFinish(float4 fallback, texture2d<float,access::read> neural,
                    texture2d<float,access::read> lower, texture2d<float,access::read> upper,
                    texture2d<float,access::read> specAlbedo, texture2d<float,access::read> diffAlbedo,
                    texture2d<float,access::read> fusedAlbedo, uint2 p, constant RrParams& c) {
    float4 lo = lower.read(p), hi = upper.read(p);
    float3 value = fallback.xyz;
    if (lo.a == 0) {
        // MetalFX returns an unjittered image; AMD denoised signals remain on
        // the caller's jittered raster grid for its later composition/upscale.
        float2 q = float2(p)-float2(c.jitter[0],c.jitter[1]);
        int2 base = int2(floor(q)); float2 f = fract(q);
        float3 color = 0;
        for (int y=0;y<2;++y) for (int x=0;x<2;++x) {
            int2 sample = clamp(base+int2(x,y),int2(0),int2(c.width-1,c.height-1));
            color += neural.read(uint2(sample)).xyz*(x ? f.x : 1-f.x)*(y ? f.y : 1-f.y);
        }
        float3 modulator = c.legacyMode == 3 ? albedo(fusedAlbedo,p,c) :
            specular(c.signal) ? albedo(specAlbedo,p,c) : diffuseAlbedo(diffAlbedo,p,c);
        // Zero-albedo channels lose their lighting information when modulated.
        // Preserve those through the analytic correction, not division by zero.
        value = select(fallback.xyz,color/select(float3(1),modulator,modulator > 0),modulator > 0);
        value = clamp(value,lo.xyz,hi.xyz);
    }
    return float4(value,fallback.a);
}

// Neural finish and caller resolve in one pass. Every read is at p except the
// private MetalFX output, so no cross-pixel barrier is needed. History keeps
// its validity alpha; the caller output receives the RR 1.2 alpha below.
kernel void rr_neural_resolve(texture2d<float,access::read_write> working [[texture(0)]],
                              texture2d<float,access::read> neural [[texture(1)]],
                              texture2d<float,access::read> lower [[texture(2)]],
                              texture2d<float,access::read> upper [[texture(3)]],
                              texture2d<float,access::read> specAlbedo [[texture(4)]],
                              texture2d<float,access::read> diffAlbedo [[texture(5)]],
                              texture2d<float,access::read> fusedAlbedo [[texture(6)]],
                              texture2d<float,access::read_write> output [[texture(7)]],
                              texture2d<float,access::read> input [[texture(8)]],
                              texture2d<float,access::read> depth [[texture(9)]],
                              constant RrParams& c [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
    if (p.x >= c.width || p.y >= c.height) return;
    float4 v = neuralFinish(working.read(p),neural,lower,upper,specAlbedo,diffAlbedo,fusedAlbedo,p,c);
    working.write(v,p);
    // RR 1.2 Atrous3 preserves the destination UAV alpha; passthrough copies input RGBA.
    v.a = bounded(depth.read(p).x,c) ? output.read(p).a : input.read(p).a;
    output.write(v,p);
}

kernel void rr_neural_resolve_write(texture2d<float,access::read_write> working [[texture(0)]],
                                    texture2d<float,access::read> neural [[texture(1)]],
                                    texture2d<float,access::read> lower [[texture(2)]],
                                    texture2d<float,access::read> upper [[texture(3)]],
                                    texture2d<float,access::read> specAlbedo [[texture(4)]],
                                    texture2d<float,access::read> diffAlbedo [[texture(5)]],
                                    texture2d<float,access::read> fusedAlbedo [[texture(6)]],
                                    texture2d<float,access::write> output [[texture(7)]],
                                    texture2d<float,access::read> input [[texture(8)]],
                                    constant RrParams& c [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
    if (p.x >= c.width || p.y >= c.height) return;
    float4 v = neuralFinish(working.read(p),neural,lower,upper,specAlbedo,diffAlbedo,fusedAlbedo,p,c);
    working.write(v,p);
    // Checkerboard and legacy radiance: traced pixels keep input alpha, holes
    // take the reconstructed alpha retained in the prepare pass's upper bound.
    bool traced = !c.checkerboard || ((p.x & 1) == ((p.y & 1) ^ c.origin));
    v.a = traced ? input.read(p).a : upper.read(p).a;
    output.write(v,p);
}

kernel void rr_resolve(texture2d<float,access::read> filtered [[texture(0)]],
                       texture2d<float,access::read_write> output [[texture(1)]],
                       texture2d<float,access::read> input [[texture(2)]],
                       texture2d<float,access::read> depth [[texture(3)]],
                       constant RrParams& c [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
    if (p.x >= c.width || p.y >= c.height) return;
    float4 v = filtered.read(p);
    // RR 1.2 Atrous3 preserves the destination UAV alpha. ComposeRaw first
    // writes checkerboard reconstruction there; passthrough copies input RGBA.
    // Internal history validity remains separate from this caller-owned alpha.
    v.a = bounded(depth.read(p).x,c) ? output.read(p).a : input.read(p).a;
    output.write(v,p);
}

kernel void rr_resolve_write(texture2d<float,access::read> filtered [[texture(0)]],
                             texture2d<float,access::write> output [[texture(1)]],
                             texture2d<float,access::read> input [[texture(2)]],
                             texture2d<float,access::read> reconstructedAlpha [[texture(3)]],
                             constant RrParams& c [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
    if (p.x >= c.width || p.y >= c.height) return;
    // Scalar, checkerboard and legacy outputs need no destination read.
    float4 v = filtered.read(p);
    bool traced = !c.checkerboard || ((p.x & 1) == ((p.y & 1) ^ c.origin));
    if (radiance(c.signal)) v.a = traced ? input.read(p).a : reconstructedAlpha.read(p).a;
    output.write(v,p);
}
