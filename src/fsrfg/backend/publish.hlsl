// SPDX-License-Identifier: MIT
cbuffer Publication : register(b0) { uint valid, index, copyColor, writeFlag; };
ByteAddressBuffer ActualCount : register(t0);
Texture2D<float4> Color : register(t1);
RWTexture2D<float4> Output : register(u0);
RWByteAddressBuffer DisableInterpolation : register(u1);
[numthreads(8,8,1)]
void CS(uint3 pixel : SV_DispatchThreadID) {
    bool publish = valid != 0 && ActualCount.Load(8) > index;
    if (all(pixel == 0) && writeFlag != 0) DisableInterpolation.Store(0, publish ? 0 : 1);
    if (publish && copyColor != 0) {
        uint width, height; Output.GetDimensions(width, height);
        if (pixel.x < width && pixel.y < height)
            Output[pixel.xy] = Color.Load(int3(pixel.xy,0));
    }
}
