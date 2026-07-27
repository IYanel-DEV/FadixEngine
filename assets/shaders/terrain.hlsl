// Terrain layers. Static mesh per chunk; object motion comes from Model/prev_model.
//
// Motion-vector convention (matches TAA reprojection):
//   velocity.xy = (currNDC.xy / currNDC.w) - (prevNDC.xy / prevNDC.w)

cbuffer TerrainVertexUniforms : register(b0, space1)
{
    float4x4 ViewProjection;
    float4x4 PrevViewProjection;
    float4x4 Model;
    float4x4 PrevModel;
};

cbuffer TerrainFragmentUniforms : register(b0, space3)
{
    float3 CameraPos;
    float HeightScale;
    float4 LightDir;
    float4 LightColor;
    float4 AmbientColor;
    float4 Params;
    float4 LayerTiling;
    float4 LayerMinHeight;
    float4 LayerMaxHeight;
    float4 LayerMinSlope;
    float4 LayerMaxSlope;
};

Texture2D layerTex0 : register(t0, space2);
SamplerState terrainSampler0 : register(s0, space2);
Texture2D layerTex1 : register(t1, space2);
SamplerState terrainSampler1 : register(s1, space2);
Texture2D layerTex2 : register(t2, space2);
SamplerState terrainSampler2 : register(s2, space2);
Texture2D layerTex3 : register(t3, space2);
SamplerState terrainSampler3 : register(s3, space2);

struct FragmentOut
{
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
};

struct VertexInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float4 color : TEXCOORD4;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float height : TEXCOORD3;
    float2 velocity : TEXCOORD4;
};

float2 NdcVelocity(float4 curr_clip, float4 prev_clip)
{
    const float2 curr_ndc = curr_clip.xy / max(abs(curr_clip.w), 1.0e-6);
    const float2 prev_ndc = prev_clip.xy / max(abs(prev_clip.w), 1.0e-6);
    return curr_ndc - prev_ndc;
}

VertexOutput VertexMain(VertexInput input)
{
    VertexOutput output;
    float4 worldPos = mul(Model, float4(input.position, 1.0));
    float4 prevWorldPos = mul(PrevModel, float4(input.position, 1.0));
    output.position = mul(ViewProjection, worldPos);
    output.velocity = NdcVelocity(output.position, mul(PrevViewProjection, prevWorldPos));
    output.worldPos = worldPos.xyz;
    output.normal = normalize(mul((float3x3)Model, input.normal));
    output.uv = input.uv;
    output.height = input.position.y;
    return output;
}

float ComputeSlope(float3 normal)
{
    return 1.0 - saturate(normalize(normal).y);
}

FragmentOut FragmentMain(VertexOutput input)
{
    FragmentOut output;
    float normalizedHeight = saturate(input.height / max(HeightScale, 0.001));
    float slope = ComputeSlope(input.normal);
    int layerCount = clamp(int(Params.x), 1, 4);

    float weights[4] = {0.0, 0.0, 0.0, 0.0};
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        if (i >= layerCount)
        {
            break;
        }
        float minH = LayerMinHeight[i];
        float maxH = LayerMaxHeight[i];
        float minS = LayerMinSlope[i];
        float maxS = LayerMaxSlope[i];
        float heightWeight = smoothstep(minH, minH + 0.05, normalizedHeight) *
            (1.0 - smoothstep(maxH - 0.05, maxH, normalizedHeight));
        float slopeWeight = smoothstep(minS, minS + 0.1, slope) *
            (1.0 - smoothstep(maxS - 0.1, maxS, slope));
        weights[i] = max(heightWeight * slopeWeight, 0.0);
    }

    float totalWeight = weights[0] + weights[1] + weights[2] + weights[3];
    if (totalWeight > 0.001)
    {
        weights[0] /= totalWeight;
        weights[1] /= totalWeight;
        weights[2] /= totalWeight;
        weights[3] /= totalWeight;
    }
    else
    {
        weights[0] = 1.0;
    }

    float4 color = layerTex0.Sample(terrainSampler0, input.uv * LayerTiling.x) * weights[0] +
        layerTex1.Sample(terrainSampler1, input.uv * LayerTiling.y) * weights[1] +
        layerTex2.Sample(terrainSampler2, input.uv * LayerTiling.z) * weights[2] +
        layerTex3.Sample(terrainSampler3, input.uv * LayerTiling.w) * weights[3];

    float3 N = normalize(input.normal);
    float3 L = normalize(-LightDir.xyz);
    float3 V = normalize(CameraPos - input.worldPos);
    float3 H = normalize(L + V);
    float diff = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), 32.0);
    float3 ambient = AmbientColor.rgb * AmbientColor.a;
    float3 lit = color.rgb * (ambient + LightColor.rgb * LightColor.a * diff) +
        LightColor.rgb * LightColor.a * spec * 0.1;
    output.color = float4(lit, 1.0);
    output.velocity = input.velocity;
    return output;
}
