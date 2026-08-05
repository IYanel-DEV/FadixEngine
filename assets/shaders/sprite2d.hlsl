// Fadix sprite2d.hlsl — native 2D sprite rendering pass
// Register layout matches viewport.hlsl conventions:
//   vertex uniforms   -> b0, space1
//   fragment uniforms -> b0, space3
//   textures/samplers -> t0, s0 in space2

cbuffer VertexUniforms : register(b0, space1)
{
    float4x4 view_projection;
    float4x4 model;
};

cbuffer FragmentUniforms : register(b0, space3)
{
    float4 tint;
    float4 uv_rect; // xy=UV offset, zw=UV scale
};

Texture2D sprite_texture : register(t0, space2);
SamplerState sprite_sampler : register(s0, space2);

struct VertexInput
{
    float3 position : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float4 tangent  : TEXCOORD2;
    float2 uv       : TEXCOORD3;
    float4 color    : TEXCOORD4;
    float4 joints   : TEXCOORD5;
    float4 weights  : TEXCOORD6;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

struct FragmentOut
{
    float4 color    : SV_Target0;
    float2 velocity : SV_Target1;
};

VertexOutput VertexMain(VertexInput input)
{
    VertexOutput output;
    float4 world_pos = mul(model, float4(input.position, 1.0));
    output.position  = mul(view_projection, world_pos);
    output.uv        = uv_rect.xy + input.uv * uv_rect.zw;
    return output;
}

FragmentOut FragmentMain(VertexOutput input)
{
    float4 sampled = sprite_texture.Sample(sprite_sampler, input.uv);
    FragmentOut output;
    output.color    = sampled * tint;
    output.velocity = float2(0.0, 0.0);
    return output;
}
