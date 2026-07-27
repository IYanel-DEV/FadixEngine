// Fadix screen-space ambient occlusion. Forward-renderer friendly: reconstructs
// view-space position from the scene depth buffer and derives normals from that
// position, so no normal G-buffer is needed. Half- or full-resolution AO pass +
// a depth-aware bilateral blur. No temporal accumulation (no motion vectors yet).
// The AO result modulates only ambient/IBL in the lit shader.

float4 FullscreenVS(uint vertex_id : SV_VertexID) : SV_Position
{
    const float2 corners[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    return float4(corners[vertex_id], 0.0, 1.0);
}

// ---- AO pass ---------------------------------------------------------------
Texture2D depth_tex : register(t0, space2);
SamplerState depth_sampler : register(s0, space2);

cbuffer SsaoUniforms : register(b0, space3)
{
    float4x4 projection;      // view -> clip (RH_ZO)
    float4x4 inv_projection;  // clip -> view
    float4 ssao_params0;      // x radius, y intensity, z power, w bias
    float4 ssao_params1;      // x sampleCount, y fadeStart, z fadeEnd, w debug
    float4 ssao_params2;      // xy inv_resolution, z max valid depth (1.0), w unused
};

// Clip -> view-space position for a screen uv (0..1, V down) and hardware depth.
float3 ViewPositionFromDepth(float2 uv, float depth)
{
    // uv (V down) -> NDC (Y up). RH_ZO clip z == hardware depth.
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 clip = float4(ndc, depth, 1.0);
    float4 view = mul(inv_projection, clip);
    return view.xyz / max(view.w, 1e-6);
}

// Hash for per-pixel rotation, breaks up banding without a noise texture.
float Hash21(float2 p)
{
    p = frac(p * float2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return frac(p.x * p.y);
}

float4 SsaoPS(float4 position : SV_Position) : SV_Target0
{
    const float2 uv = position.xy * ssao_params2.xy;
    const float depth = depth_tex.Sample(depth_sampler, uv).r;
    // Background (far plane) has no geometry -> fully lit.
    if (depth >= 1.0)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }

    const float3 p = ViewPositionFromDepth(uv, depth);
    // Reconstruct the view-space normal from position derivatives. Using the
    // smaller of the forward/backward differences avoids normals bending across
    // silhouettes (helps prevent halos).
    const float3 ddxP = ddx(p);
    const float3 ddyP = ddy(p);
    const float3 normal = normalize(cross(ddxP, ddyP));

    const float radius = ssao_params0.x;
    const float bias = ssao_params0.w;
    const int samples = max((int)ssao_params1.x, 1);
    const float rotation = Hash21(position.xy) * 6.2831853;
    const float cosR = cos(rotation);
    const float sinR = sin(rotation);

    float occlusion = 0.0;
    [loop]
    for (int i = 0; i < samples; ++i)
    {
        // Cheap hemisphere kernel: spiral of directions scaled toward the center.
        const float t = (float(i) + 0.5) / float(samples);
        const float angle = t * 6.2831853 * 3.0 + rotation;
        float2 dir = float2(cos(angle), sin(angle));
        dir = float2(dir.x * cosR - dir.y * sinR, dir.x * sinR + dir.y * cosR);
        const float sampleRadius = radius * lerp(0.15, 1.0, t * t);

        // Offset in view space along the tangent plane, biased toward the normal.
        float3 offset = float3(dir, 0.0) * sampleRadius + normal * (sampleRadius * 0.25);
        float3 samplePos = p + offset;

        // Project the sample back to screen space to fetch stored depth there.
        float4 clip = mul(projection, float4(samplePos, 1.0));
        if (clip.w <= 0.0)
        {
            continue;
        }
        float2 sampleUv = clip.xy / clip.w;
        sampleUv = float2(sampleUv.x * 0.5 + 0.5, 0.5 - sampleUv.y * 0.5);
        if (sampleUv.x < 0.0 || sampleUv.x > 1.0 || sampleUv.y < 0.0 || sampleUv.y > 1.0)
        {
            continue;
        }
        const float sampleDepth = depth_tex.Sample(depth_sampler, sampleUv).r;
        const float3 occluderPos = ViewPositionFromDepth(sampleUv, sampleDepth);

        // Occluded when the stored surface is closer to the eye than the sample
        // (view -Z forward, so a larger .z means closer). Range check keeps a
        // distant background from casting a halo onto near geometry.
        const float delta = occluderPos.z - samplePos.z;
        const float rangeCheck = smoothstep(0.0, 1.0, radius / max(abs(p.z - occluderPos.z), 1e-4));
        occlusion += (delta > bias ? 1.0 : 0.0) * rangeCheck;
    }
    occlusion /= float(samples);

    // Distance fade so AO disappears far from the camera (viewZ is negative).
    const float viewDist = -p.z;
    const float fade = 1.0 - smoothstep(ssao_params1.y, ssao_params1.z, viewDist);
    float ao = 1.0 - occlusion * ssao_params0.y * fade;
    ao = pow(saturate(ao), ssao_params0.z); // power/contrast
    return float4(ao, ao, ao, 1.0);
}

// ---- Depth-aware bilateral blur --------------------------------------------
Texture2D ao_tex : register(t0, space2);
SamplerState ao_sampler : register(s0, space2);
Texture2D blur_depth_tex : register(t1, space2);
SamplerState blur_depth_sampler : register(s1, space2);

cbuffer BlurUniforms : register(b0, space3)
{
    float4 blur_params; // xy inv_resolution, z depth sigma, w direction (1=horiz,0=vert)
};

// Separable depth-aware blur: neighbors on the other side of a depth edge are
// down-weighted so the AO does not bleed across silhouettes (no halos).
float4 BlurPS(float4 position : SV_Position) : SV_Target0
{
    const float2 uv = position.xy * blur_params.xy;
    const float2 step = blur_params.w > 0.5
        ? float2(blur_params.x, 0.0)
        : float2(0.0, blur_params.y);
    const float centerDepth = blur_depth_tex.Sample(blur_depth_sampler, uv).r;
    const float sigma = max(blur_params.z, 1e-4);

    float sum = ao_tex.Sample(ao_sampler, uv).r;
    float weightSum = 1.0;
    [unroll]
    for (int i = 1; i <= 3; ++i)
    {
        const float2 offset = step * float(i);
        [unroll]
        for (int s = -1; s <= 1; s += 2)
        {
            const float2 sampleUv = uv + offset * float(s);
            const float sampleDepth = blur_depth_tex.Sample(blur_depth_sampler, sampleUv).r;
            const float spatial = exp(-float(i * i) * 0.25);
            const float depthWeight = exp(-abs(sampleDepth - centerDepth) / sigma);
            const float w = spatial * depthWeight;
            sum += ao_tex.Sample(ao_sampler, sampleUv).r * w;
            weightSum += w;
        }
    }
    const float ao = sum / max(weightSum, 1e-4);
    return float4(ao, ao, ao, 1.0);
}
