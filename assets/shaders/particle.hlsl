// CPU particle billboards. Vertices are world-space quads with current and
// previous center positions for motion vectors.
//
// Motion-vector convention (matches TAA reprojection):
//   velocity.xy = (currNDC.xy / currNDC.w) - (prevNDC.xy / prevNDC.w)

cbuffer ParticleVertexUniforms : register(b0, space1)
{
    float4x4 view_projection;
    float4x4 prev_view_projection;
};

struct VSInput
{
    float3 position : TEXCOORD0;
    float3 prev_position : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 color : TEXCOORD3;
};

struct PSInput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float2 velocity : TEXCOORD1;
};

struct FragmentOut
{
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
};

float2 NdcVelocity(float4 curr_clip, float4 prev_clip)
{
    const float2 curr_ndc = curr_clip.xy / max(abs(curr_clip.w), 1.0e-6);
    const float2 prev_ndc = prev_clip.xy / max(abs(prev_clip.w), 1.0e-6);
    return curr_ndc - prev_ndc;
}

PSInput VertexMain(VSInput input)
{
    PSInput output;
    output.position = mul(view_projection, float4(input.position, 1.0));
    const float4 prev_clip = mul(prev_view_projection, float4(input.prev_position, 1.0));
    output.velocity = NdcVelocity(output.position, prev_clip);
    output.color = input.color;
    output.uv = input.uv;
    return output;
}

FragmentOut FragmentMain(PSInput input)
{
    FragmentOut output;
    const float dist = length(input.uv * 2.0 - 1.0);
    const float alpha = saturate(1.0 - dist * dist);
    output.color = float4(input.color.rgb, input.color.a * alpha);
    output.velocity = input.velocity;
    return output;
}
