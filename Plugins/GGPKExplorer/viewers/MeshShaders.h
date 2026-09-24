#pragma once

namespace GGPKExp {

// Max bones for GPU skinning constant buffer (256 * 64 bytes = 16KB).
// POE2 rigs reach ~250 bones (breachbosshandmage); 256 matches the uint8
// BLENDINDEX domain and keeps cbuffer well under D3D11's 64KB per-cbuffer
// cap. Raised from 128 on 2026-04-15 to fix "fingertip spike" bug.
inline constexpr int kMaxSkinBones = 256;

// NOTE: all three shaders below share the same cbuffer Constants : register(b0)
// layout. It MUST match MeshRenderer::CBData byte-for-byte:
//   float4x4 MVP
//   float4x4 World
//   float4   BaseColor
//   float4   CameraPosOpaque        // xyz = cam pos, w = opaque flag
//   float4x4 LightVP
//   ShaderLight Lights[8]           // 8 * 4 float4 = 512 bytes
//   int      LightCount
//   float    SkinEnabled            // replaces Phase-1 "Padding"; used by VS
//   int      _pad1, _pad2
//   float4   SunDir                 // xyz = sun-fallback direction

inline const char* g_meshVertexShaderSrc = R"(
struct ShaderLight {
    float4 posRadius;
    float4 dirCone;
    float4 colorInner;
    float4 params;
};

static const int kMaxShaderLights = 8;

cbuffer Constants : register(b0) {
    float4x4     MVP;
    float4x4     World;
    float4       BaseColor;
    float4       CameraPosOpaque;
    float4x4     LightVP;
    ShaderLight  Lights[kMaxShaderLights];
    int          LightCount;
    float        SkinEnabled;
    int          _pad1;
    int          _pad2;
    float4       SunDir;
};
cbuffer BoneBuffer : register(b1) {
    float4x4 Bones[256];
};
struct VS_INPUT {
    float3 Pos     : POSITION;
    float3 Normal  : NORMAL;
    float4 Tangent : TANGENT;
    float2 UV      : TEXCOORD0;
    uint4  BoneIdx : BLENDINDICES;
    float4 BoneWt  : BLENDWEIGHT;
};
struct VS_OUTPUT {
    float4 Pos      : SV_POSITION;
    float3 Normal   : NORMAL;
    float3 Tangent  : TANGENT;
    float3 Binormal : BINORMAL;
    float3 WorldPos : TEXCOORD0;
    float2 UV       : TEXCOORD1;
};
VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT o;

    float3 localPos = input.Pos;
    float3 localNrm = input.Normal;
    float3 localTan = input.Tangent.xyz;

    // GPU skinning: blend position/normal/tangent by up to 4 bones
    if (SkinEnabled > 0.5) {
        float4 sp = float4(0,0,0,0);
        float3 sn = float3(0,0,0);
        float3 st = float3(0,0,0);
        float  wt = input.BoneWt.x + input.BoneWt.y + input.BoneWt.z + input.BoneWt.w;
        if (wt > 0.001) {
            [unroll] for (int i = 0; i < 4; i++) {
                float  w = i==0 ? input.BoneWt.x : i==1 ? input.BoneWt.y : i==2 ? input.BoneWt.z : input.BoneWt.w;
                uint idx = i==0 ? input.BoneIdx.x : i==1 ? input.BoneIdx.y : i==2 ? input.BoneIdx.z : input.BoneIdx.w;
                if (w > 0.001 && idx < 256) {
                    float4x4 bm = Bones[idx];
                    sp += mul(float4(input.Pos, 1.0), bm) * w;
                    sn += mul(float4(input.Normal, 0.0), bm).xyz * w;
                    st += mul(float4(input.Tangent.xyz, 0.0), bm).xyz * w;
                }
            }
            localPos = sp.xyz / wt;
            localNrm = sn / wt;
            localTan = st / wt;
        }
    }

    o.Pos = mul(float4(localPos, 1.0), MVP);
    o.Normal   = normalize(mul(float4(localNrm, 0.0), World).xyz);
    o.Tangent  = normalize(mul(float4(localTan, 0.0), World).xyz);
    o.Binormal = cross(o.Normal, o.Tangent) * input.Tangent.w;
    o.WorldPos = mul(float4(localPos, 1.0), World).xyz;
    o.UV = input.UV;
    return o;
}
)";

inline const char* g_meshPixelShaderSrc = R"(
struct ShaderLight {
    float4 posRadius;
    float4 dirCone;
    float4 colorInner;
    float4 params;
};

static const int kMaxShaderLights = 8;

cbuffer Constants : register(b0) {
    float4x4     MVP;
    float4x4     World;
    float4       BaseColor;
    float4       CameraPosOpaque;
    float4x4     LightVP;
    ShaderLight  Lights[kMaxShaderLights];
    int          LightCount;
    float        SkinEnabled;
    int          _pad1;
    int          _pad2;
    float4       SunDir;
};

Texture2D      texDiffuse : register(t0);
Texture2D      texNormal  : register(t1);
Texture2D      texShadow  : register(t2);
SamplerState   samp0      : register(s0);
SamplerComparisonState shadowSamp : register(s1);

struct PS_INPUT {
    float4 Pos      : SV_POSITION;
    float3 Normal   : NORMAL;
    float3 Tangent  : TANGENT;
    float3 Binormal : BINORMAL;
    float3 WorldPos : TEXCOORD0;
    float2 UV       : TEXCOORD1;
};

float4 main(PS_INPUT input) : SV_TARGET {
    // Sample normal map: RGB = tangent-space normal, A = gloss
    float4 nSample = texNormal.Sample(samp0, input.UV);
    float3 tN = nSample.rgb * 2.0 - 1.0;
    float gloss = nSample.a; // 0 = rough, 1 = smooth

    float3 Nraw = normalize(input.Normal);
    float3 Traw = normalize(input.Tangent);
    float3 Braw = normalize(input.Binormal);
    float3x3 TBN = float3x3(Traw, Braw, Nraw);
    float3 N = normalize(mul(tN, TBN));

    float3 V = normalize(CameraPosOpaque.xyz - input.WorldPos);
    float shininess = lerp(4.0, 128.0, gloss * gloss);

    // Sample diffuse texture
    float4 texColor = texDiffuse.Sample(samp0, input.UV);
    float3 baseCol = texColor.rgb * BaseColor.rgb;

    // Hemisphere ambient
    float skyFactor = abs(N.y) * 0.5 + 0.5;
    float3 ambient = lerp(float3(0.30, 0.30, 0.30), float3(0.45, 0.50, 0.55), skyFactor);
    float3 col = ambient * baseCol;

    // Phase 2 — scene lights
    int count = min(LightCount, kMaxShaderLights);
    for (int i = 0; i < count; ++i) {
        ShaderLight lt = Lights[i];
        float3 toL = lt.posRadius.xyz - input.WorldPos;
        float dist = length(toL);
        float3 Ldir = dist > 1e-4 ? toL / dist : float3(0, 1, 0);

        // Radius-based attenuation. posRadius.w is the light's reach in world
        // units; params.y (penumbra_radius) is a 0..1 fraction of radius over
        // which the edge softens — tiny values produce a near-hard cutoff.
        float lradius = lt.posRadius.w;
        float penFrac = saturate(lt.params.y);
        float inner   = lradius * (1.0 - penFrac);
        float outer   = max(lradius, inner + 1.0);
        float attenD  = 1.0 - smoothstep(inner, outer, dist);
        if (attenD <= 0.001) continue;

        float attenC = 1.0;
        if (lt.dirCone.w >= 0.0) {
            float3 spotDir = normalize(-lt.dirCone.xyz);
            float  cosTheta = dot(Ldir, spotDir);
            float  cosPen   = lt.dirCone.w;
            float  cosUmb   = lt.colorInner.w;
            attenC = smoothstep(cosPen, cosUmb, cosTheta);
            if (attenC <= 0.001) continue;
        }

        float ndl = saturate(dot(N, Ldir));
        float3 H = normalize(Ldir + V);
        float ndh = saturate(dot(N, H));
        float spec = pow(ndh, shininess) * gloss * 0.5;

        // Shadow map applies to slot 0 only (dominant caster).
        float shadow = 1.0;
        if (i == 0 && lt.params.z > 0.5) {
            float4 lightClip = mul(float4(input.WorldPos, 1.0), LightVP);
            if (lightClip.w > 0.001) {
                float3 lndc = lightClip.xyz / lightClip.w;
                float2 shadowUV = float2(lndc.x * 0.5 + 0.5, -lndc.y * 0.5 + 0.5);
                if (shadowUV.x >= 0 && shadowUV.x <= 1 && shadowUV.y >= 0 && shadowUV.y <= 1) {
                    float bias = 0.002;
                    shadow = texShadow.SampleCmpLevelZero(shadowSamp, shadowUV, lndc.z - bias);
                    shadow = lerp(0.35, 1.0, shadow); // soften shadow (not fully black)
                }
            }
        }

        col += (lt.colorInner.rgb * (ndl + spec)) * (attenD * attenC) * shadow * baseCol;
    }

    // Sun fallback when no scene lights — preserves Phase-1 look.
    if (count == 0) {
        float3 L = -normalize(SunDir.xyz);
        float3 H = normalize(L + V);
        float ndl = abs(dot(N, L));
        float ndh = max(dot(N, H), 0.0);
        float spec = pow(ndh, shininess) * gloss * 0.5;

        // Shadow mapping (sun-aligned LightVP).
        float4 lightClip = mul(float4(input.WorldPos, 1.0), LightVP);
        float shadow = 1.0;
        if (lightClip.w > 0.001) {
            float3 lndc = lightClip.xyz / lightClip.w;
            float2 shadowUV = float2(lndc.x * 0.5 + 0.5, -lndc.y * 0.5 + 0.5);
            if (shadowUV.x >= 0 && shadowUV.x <= 1 && shadowUV.y >= 0 && shadowUV.y <= 1) {
                float bias = 0.002;
                shadow = texShadow.SampleCmpLevelZero(shadowSamp, shadowUV, lndc.z - bias);
                shadow = lerp(0.35, 1.0, shadow);
            }
        }

        // Fill light
        float3 fillDir = normalize(float3(0.5, 0.3, -0.7));
        float fill = abs(dot(N, fillDir)) * 0.15;
        float3 specColor = float3(1.0, 0.98, 0.95) * spec * shadow;
        col += baseCol * (0.85 * ndl * shadow + fill) + specColor;
    }

    // Fresnel rim
    float fresnel = pow(1.0 - saturate(dot(N, V)), 3.0);
    col += fresnel * 0.15 * baseCol;

    // Reinhard tone mapping — soft roll-off, preserves highlight detail.
    col = col / (1.0 + col);

    // Opaque mode: alpha=1; transparent mode: use texture alpha.
    float outAlpha = lerp(texColor.a * BaseColor.a, 1.0, CameraPosOpaque.w);
    return float4(col, outAlpha);
}
)";

inline const char* g_wirePixelShaderSrc = R"(
struct PS_INPUT {
    float4 Pos      : SV_POSITION;
    float3 Normal   : NORMAL;
    float3 Tangent  : TANGENT;
    float3 Binormal : BINORMAL;
    float3 WorldPos : TEXCOORD0;
    float2 UV       : TEXCOORD1;
};
float4 main(PS_INPUT input) : SV_TARGET {
    return float4(0.6, 0.65, 0.7, 1.0);
}
)";

// Shadow map depth-only vertex shader — must share the same cbuffer layout
// as the main VS/PS so register(b0) binding stays valid.
inline const char* g_shadowVertexShaderSrc = R"(
struct ShaderLight {
    float4 posRadius;
    float4 dirCone;
    float4 colorInner;
    float4 params;
};

static const int kMaxShaderLights = 8;

cbuffer Constants : register(b0) {
    float4x4     MVP;
    float4x4     World;
    float4       BaseColor;
    float4       CameraPosOpaque;
    float4x4     LightVP;
    ShaderLight  Lights[kMaxShaderLights];
    int          LightCount;
    float        SkinEnabled;
    int          _pad1;
    int          _pad2;
    float4       SunDir;
};
struct VS_INPUT {
    float3 Pos     : POSITION;
    float3 Normal  : NORMAL;
    float4 Tangent : TANGENT;
    float2 UV      : TEXCOORD0;
    uint4  BoneIdx : BLENDINDICES;
    float4 BoneWt  : BLENDWEIGHT;
};
struct VS_OUTPUT {
    float4 Pos : SV_POSITION;
};
VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT o;
    o.Pos = mul(float4(input.Pos, 1.0), LightVP);
    return o;
}
)";

} // namespace GGPKExp
