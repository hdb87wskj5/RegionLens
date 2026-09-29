#pragma once
namespace RegionLens::native
{
    inline constexpr char QualityVertexShader[] = R"(
struct Output { float4 position:SV_POSITION; float2 uv:TEXCOORD0; };
Output main(uint id:SV_VertexID) {
    Output o; o.uv=float2((id<<1)&2,id&2);
    o.position=float4(o.uv.x*2-1,1-o.uv.y*2,0,1); return o;
})";
    inline constexpr char QualityResizeShader[] = R"(
Texture2D Source:register(t0);
cbuffer QualityConstants:register(b0) { float4 SourceRect; float2 OutputSize; float Strength; float Reserved; };
float4 Weights(float t) {
    float t2=t*t, t3=t2*t;
    return float4(-.5*t+t2-.5*t3,1-2.5*t2+1.5*t3,.5*t+2*t2-1.5*t3,-.5*t2+.5*t3);
}
float4 main(float4 position:SV_POSITION):SV_TARGET {
    float2 p=SourceRect.xy+position.xy/OutputSize*SourceRect.zw-.5;
    int2 base=int2(floor(p)); float2 f=frac(p);
    float4 wx=Weights(f.x), wy=Weights(f.y);
    int2 lo=int2(SourceRect.xy), hi=lo+int2(SourceRect.zw)-1;
    float3 sum=0, minimum=1, maximum=0;
    [unroll] for(int y=0;y<4;++y) [unroll] for(int x=0;x<4;++x) {
        float3 c=Source.Load(int3(clamp(base+int2(x-1,y-1),lo,hi),0)).rgb;
        sum+=c*wx[x]*wy[y];
        if(x>=1 && x<=2 && y>=1 && y<=2) { minimum=min(minimum,c); maximum=max(maximum,c); }
    }
    return float4(clamp(sum,minimum,maximum),1);
})";
    // The complete notice is part of the compiled EXE's shader string as well
    // as the source distribution. No dependency on the FidelityFX SDK runtime.
    inline constexpr char QualityCasShader[] = R"(
// Adapted from AMD FidelityFX CAS, sharpen-only, FP32, green-weight path.
// Upstream: GPUOpen-Effects/FidelityFX-CAS
// Commit: 9fabcc9a2c45f958aff55ddfda337e74ef894b7f, ffx-cas/ffx_cas.h
// Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
Texture2D LinearSource:register(t0); // SRGB view decodes before CAS arithmetic.
cbuffer QualityConstants:register(b0) { float4 SourceRect; float2 OutputSize; float Strength; float Reserved; };
float3 Read(int2 p) { return LinearSource.Load(int3(clamp(p,0,int2(OutputSize)-1),0)).rgb; }
float3 Encode(float3 c) {
    return float3(c.r<=.0031308 ? 12.92*c.r : 1.055*pow(c.r,1.0/2.4)-.055,
                  c.g<=.0031308 ? 12.92*c.g : 1.055*pow(c.g,1.0/2.4)-.055,
                  c.b<=.0031308 ? 12.92*c.b : 1.055*pow(c.b,1.0/2.4)-.055);
}
float4 main(float4 position:SV_POSITION):SV_TARGET {
    int2 p=int2(position.xy);
    float3 b=Read(p+int2(0,-1)), d=Read(p+int2(-1,0)), e=Read(p), f=Read(p+int2(1,0)), h=Read(p+int2(0,1));
    float mn=min(min(min(d.g,e.g),f.g),min(b.g,h.g));
    float mx=max(max(max(d.g,e.g),f.g),max(b.g,h.g));
    float amp=sqrt(saturate(min(mn,1-mx)/max(mx,1e-6)));
    float weight=amp*(-1.0/lerp(8.0,5.0,saturate(Strength)));
    float3 rgb=saturate(((b+d+f+h)*weight+e)/(1+4*weight));
    return float4(Encode(rgb),1);
})";
}
