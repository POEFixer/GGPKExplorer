#pragma once

namespace GGPKExp {

// Max bones for GPU skinning constant buffer (256 * 64 bytes = 16KB).
// POE2 rigs reach ~250 bones (breachbosshandmage); 256 matches the uint8
// BLENDINDEX domain and keeps cbuffer well under D3D11's 64KB per-cbuffer
// cap. Raised from 128 on 2026-04-15 to fix "fingertip spike" bug.
inline constexpr int kMaxSkinBones = 256;

// NOTE: both shaders share cbuffer Constants : register(b0). It MUST match
// MeshRenderer::CBData byte-for-byte (176 bytes, static_assert there):
//   float4x4 MVP
//   float4x4 World
//   float4   BaseColor
//   float4   LightDir         // xyz = the direction the light travels
//   float    Opaque           // 1 = alpha forced to 1, 0 = texture alpha
//   float    SkinEnabled      // 1 = blend by the bone palette (b1)
//   float2   _pad

inline const char* g_meshVertexShaderSrc = R"(
cbuffer Constants : register(b0) {
    float4x4 MVP;
    float4x4 World;
    float4   BaseColor;
    float4   LightDir;
    float    Opaque;
    float    SkinEnabled;
    float2   _pad;
};
cbuffer BoneBuffer : register(b1) {
    float4x4 Bones[256];
};
struct VS_INPUT {
    float3 Pos     : POSITION;
    float3 Normal  : NORMAL;
    float2 UV      : TEXCOORD0;
    uint4  BoneIdx : BLENDINDICES;
    float4 BoneWt  : BLENDWEIGHT;
};
struct VS_OUTPUT {
    float4 Pos    : SV_POSITION;
    float3 Normal : NORMAL;
    float2 UV     : TEXCOORD0;
};
VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT o;

    float3 localPos = input.Pos;
    float3 localNrm = input.Normal;

    // GPU skinning: blend position/normal by up to 4 bones
    if (SkinEnabled > 0.5) {
        float4 sp = float4(0,0,0,0);
        float3 sn = float3(0,0,0);
        float  wt = input.BoneWt.x + input.BoneWt.y + input.BoneWt.z + input.BoneWt.w;
        if (wt > 0.001) {
            [unroll] for (int i = 0; i < 4; i++) {
                float  w = i==0 ? input.BoneWt.x : i==1 ? input.BoneWt.y : i==2 ? input.BoneWt.z : input.BoneWt.w;
                uint idx = i==0 ? input.BoneIdx.x : i==1 ? input.BoneIdx.y : i==2 ? input.BoneIdx.z : input.BoneIdx.w;
                if (w > 0.001 && idx < 256) {
                    float4x4 bm = Bones[idx];
                    sp += mul(float4(input.Pos, 1.0), bm) * w;
                    sn += mul(float4(input.Normal, 0.0), bm).xyz * w;
                }
            }
            localPos = sp.xyz / wt;
            localNrm = sn / wt;
        }
    }

    o.Pos    = mul(float4(localPos, 1.0), MVP);
    o.Normal = normalize(mul(float4(localNrm, 0.0), World).xyz);
    o.UV     = input.UV;
    return o;
}
)";

// Diffuse texture lit by a hemisphere ambient and one directional light.
inline const char* g_meshPixelShaderSrc = R"(
cbuffer Constants : register(b0) {
    float4x4 MVP;
    float4x4 World;
    float4   BaseColor;
    float4   LightDir;
    float    Opaque;
    float    SkinEnabled;
    float2   _pad;
};

Texture2D    texDiffuse : register(t0);
SamplerState samp0      : register(s0);

struct PS_INPUT {
    float4 Pos    : SV_POSITION;
    float3 Normal : NORMAL;
    float2 UV     : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_TARGET {
    float3 N = normalize(input.Normal);

    float4 texColor = texDiffuse.Sample(samp0, input.UV);
    float3 baseCol = texColor.rgb * BaseColor.rgb;

    // Hemisphere ambient
    float skyFactor = abs(N.y) * 0.5 + 0.5;
    float3 ambient = lerp(float3(0.30, 0.30, 0.30), float3(0.45, 0.50, 0.55), skyFactor);

    // One directional light, lighting both sides of a face.
    float ndl = abs(dot(N, -normalize(LightDir.xyz)));
    float3 col = baseCol * (ambient + 0.85 * ndl);

    // Reinhard tone mapping — soft roll-off, preserves highlight detail.
    col = col / (1.0 + col);

    // Opaque mode: alpha=1; transparent mode: use texture alpha.
    float outAlpha = lerp(texColor.a * BaseColor.a, 1.0, Opaque);
    return float4(col, outAlpha);
}
)";

inline const char* g_wirePixelShaderSrc = R"(
struct PS_INPUT {
    float4 Pos    : SV_POSITION;
    float3 Normal : NORMAL;
    float2 UV     : TEXCOORD0;
};
float4 main(PS_INPUT input) : SV_TARGET {
    return float4(0.6, 0.65, 0.7, 1.0);
}
)";

} // namespace GGPKExp
