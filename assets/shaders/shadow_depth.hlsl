cbuffer ShadowVertexUniforms : register(b0, space1)
{
    float4x4 light_space_matrix;
    float4x4 model;
    float4 skin_params;
};

cbuffer BoneUniforms : register(b1, space1)
{
    float4x4 bones[128];
};

// Per-draw alpha-mask parameters. x = mask enabled (0/1), y = alpha cutoff.
// uv_params scales/offsets the sampled UV to match the lit pass.
cbuffer ShadowFragmentUniforms : register(b0, space3)
{
    float4 uv_params;    // xy scale, zw offset
    float4 alpha_params; // x mask enabled, y cutoff
};

Texture2D base_color_tex : register(t0, space2);
SamplerState base_color_sampler : register(s0, space2);

struct VSInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float4 color : TEXCOORD4;
    float4 joint_indices : TEXCOORD5;
    float4 joint_weights : TEXCOORD6;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

PSInput VertexMain(VSInput input)
{
    PSInput output;
    float4 local_position = float4(input.position, 1.0);
    if (skin_params.x > 0.5)
    {
        const float4x4 skin =
            input.joint_weights.x * bones[int(input.joint_indices.x)] +
            input.joint_weights.y * bones[int(input.joint_indices.y)] +
            input.joint_weights.z * bones[int(input.joint_indices.z)] +
            input.joint_weights.w * bones[int(input.joint_indices.w)];
        local_position = mul(skin, local_position);
    }
    output.position = mul(mul(light_space_matrix, model), local_position);
    output.uv = input.uv;
    return output;
}

float4 FragmentMain(PSInput input) : SV_Target0
{
    // Alpha-mask casters (AlphaMode::Mask) punch holes in their shadow so
    // foliage/fences cast their cut-out silhouette instead of a solid block.
    // Opaque casters pass mask=0 and skip the sample entirely.
    if (alpha_params.x > 0.5)
    {
        const float2 uv = input.uv * uv_params.xy + uv_params.zw;
        const float alpha = base_color_tex.Sample(base_color_sampler, uv).a;
        if (alpha < alpha_params.y)
        {
            clip(-1.0);
        }
    }
    return float4(input.position.z, input.position.z, input.position.z, 1.0);
}
