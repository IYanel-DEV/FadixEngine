// Fadix post-processing shaders. All passes share FullscreenVS (SV_VertexID
// full-screen triangle, no vertex buffer). Each PS derives uv from SV_Position:
//   uv = position.xy * InvResolution
// Fragment uniforms bind at b0,space3; source textures at t0/s0 (and t1/s1 for
// composite / upsample), space2.

float4 FullscreenVS(uint vertex_id : SV_VertexID) : SV_Position
{
    const float2 corners[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    return float4(corners[vertex_id], 0.0, 1.0);
}

Texture2D source_tex : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

Texture2D bloom_tex : register(t1, space2);
SamplerState bloom_sampler : register(s1, space2);

static const float3 kLumaWeights = float3(0.2126, 0.7152, 0.0722);

float Luma(float3 color)
{
    return dot(color, kLumaWeights);
}

// Bloom extract weight in [0, ~1]. Hard zero when luma <= threshold so ordinary
// white albedo (luma ~1, default threshold 1.0) never blooms. For excess = luma -
// threshold > 0, a quadratic soft knee eases in over (0, knee]; above knee the
// linear excess dominates. At luma=1, t=1: weight=0. At luma=1+eps: ~eps/t.
float SoftKnee(float luma, float threshold, float knee)
{
    if (luma <= threshold)
    {
        return 0.0;
    }

    const float excess = luma - threshold;
    const float soft = min(excess, knee);
    const float soft2 = soft * soft / (knee + 1e-4);
    return max(soft2, excess) / max(luma, 1e-4);
}

float3 KarisAverage(float3 c0, float3 c1, float3 c2, float3 c3)
{
    const float w0 = 1.0 / (1.0 + Luma(c0));
    const float w1 = 1.0 / (1.0 + Luma(c1));
    const float w2 = 1.0 / (1.0 + Luma(c2));
    const float w3 = 1.0 / (1.0 + Luma(c3));
    return (c0 * w0 + c1 * w1 + c2 * w2 + c3 * w3) / (w0 + w1 + w2 + w3);
}

// ---- Bright extract + half-res downsample ----------------------------------
cbuffer BrightExtractUniforms : register(b0, space3)
{
    float2 extract_inv_resolution;
    float extract_threshold;
    float extract_knee;
};

float4 BrightExtractPS(float4 position : SV_Position) : SV_Target0
{
    const float2 uv = position.xy * extract_inv_resolution;
    const float2 srcTexel = extract_inv_resolution * 2.0;
    const float3 s00 = source_tex.Sample(source_sampler, uv + float2(-0.5, -0.5) * srcTexel).rgb;
    const float3 s10 = source_tex.Sample(source_sampler, uv + float2(0.5, -0.5) * srcTexel).rgb;
    const float3 s01 = source_tex.Sample(source_sampler, uv + float2(-0.5, 0.5) * srcTexel).rgb;
    const float3 s11 = source_tex.Sample(source_sampler, uv + float2(0.5, 0.5) * srcTexel).rgb;
    const float3 color = KarisAverage(s00, s10, s01, s11);
    const float luma = Luma(color);
    const float weight = SoftKnee(luma, extract_threshold, extract_knee);
    return float4(color * weight, 1.0);
}

// ---- Bloom downsample (13-tap dual filter) ---------------------------------
cbuffer BloomDownsampleUniforms : register(b0, space3)
{
    float2 down_inv_resolution;
    float down_pad0;
    float down_pad1;
};

float4 BloomDownsamplePS(float4 position : SV_Position) : SV_Target0
{
    const float2 uv = position.xy * down_inv_resolution;
    const float2 halfPixel = down_inv_resolution * 0.5;
    float3 color = source_tex.Sample(source_sampler, uv).rgb * 4.0;
    color += source_tex.Sample(source_sampler, uv - halfPixel).rgb;
    color += source_tex.Sample(source_sampler, uv + float2(halfPixel.x, -halfPixel.y)).rgb;
    color += source_tex.Sample(source_sampler, uv - float2(halfPixel.x, -halfPixel.y)).rgb;
    color += source_tex.Sample(source_sampler, uv + halfPixel).rgb;
    color += source_tex.Sample(source_sampler, uv + float2(-halfPixel.x * 2.0, 0.0)).rgb;
    color += source_tex.Sample(source_sampler, uv + float2(-halfPixel.x, halfPixel.y * 2.0)).rgb;
    color += source_tex.Sample(source_sampler, uv + float2(0.0, halfPixel.y * 2.0)).rgb;
    color += source_tex.Sample(source_sampler, uv + float2(halfPixel.x, halfPixel.y * 2.0)).rgb;
    color += source_tex.Sample(source_sampler, uv + float2(halfPixel.x * 2.0, 0.0)).rgb;
    color += source_tex.Sample(source_sampler, uv + float2(-halfPixel.x, -halfPixel.y * 2.0)).rgb;
    color += source_tex.Sample(source_sampler, uv + float2(0.0, -halfPixel.y * 2.0)).rgb;
    color += source_tex.Sample(source_sampler, uv + float2(halfPixel.x, -halfPixel.y * 2.0)).rgb;
    return float4(color / 16.0, 1.0);
}

// ---- Bloom upsample (9-tap tent + additive fine mip) -----------------------
cbuffer BloomUpsampleUniforms : register(b0, space3)
{
    float2 up_inv_resolution;
    float up_pad0;
    float up_pad1;
};

float3 UpsampleTent(Texture2D tex, SamplerState samp, float2 uv, float2 coarseInvResolution)
{
    const float2 offset = coarseInvResolution;
    float3 result = tex.Sample(samp, uv + float2(-offset.x, 0.0)).rgb;
    result += tex.Sample(samp, uv + float2(offset.x, 0.0)).rgb;
    result += tex.Sample(samp, uv + float2(0.0, -offset.y)).rgb;
    result += tex.Sample(samp, uv + float2(0.0, offset.y)).rgb;
    result *= 2.0;
    result += tex.Sample(samp, uv + float2(-offset.x, -offset.y)).rgb * 2.0;
    result += tex.Sample(samp, uv + float2(offset.x, -offset.y)).rgb * 2.0;
    result += tex.Sample(samp, uv + float2(-offset.x, offset.y)).rgb * 2.0;
    result += tex.Sample(samp, uv + float2(offset.x, offset.y)).rgb * 2.0;
    result += tex.Sample(samp, uv).rgb * 4.0;
    return result / 16.0;
}

float4 BloomUpsamplePS(float4 position : SV_Position) : SV_Target0
{
    const float2 uv = position.xy * up_inv_resolution;
    const float3 fine = source_tex.Sample(source_sampler, uv).rgb;
    const float3 coarseUpsampled = UpsampleTent(bloom_tex, bloom_sampler, uv, up_inv_resolution * 2.0);
    return float4(fine + coarseUpsampled, 1.0);
}

// ---- Bloom composite -------------------------------------------------------
cbuffer CompositeUniforms : register(b0, space3)
{
    float2 composite_inv_resolution;
    float composite_intensity;
    float composite_pad0;
};

float4 CompositePS(float4 position : SV_Position) : SV_Target0
{
    const float2 uv = position.xy * composite_inv_resolution;
    const float3 scene = source_tex.Sample(source_sampler, uv).rgb;
    const float3 bloom = bloom_tex.Sample(bloom_sampler, uv).rgb;
    return float4(scene + bloom * composite_intensity, 1.0);
}

// ---- Tonemap ---------------------------------------------------------------
cbuffer TonemapUniforms : register(b0, space3)
{
    float2 tonemap_inv_resolution;
    float tonemap_exposure;
    float tonemap_apply_aces;
};

float3 TonemapAces(float3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 TonemapPS(float4 position : SV_Position) : SV_Target0
{
    const float2 uv = position.xy * tonemap_inv_resolution;
    float3 hdr = source_tex.Sample(source_sampler, uv).rgb * tonemap_exposure;
    float3 ldr = tonemap_apply_aces > 0.5 ? TonemapAces(hdr) : saturate(hdr);
    ldr = pow(max(ldr, 0.0), 1.0 / 2.2);
    return float4(ldr, 1.0);
}

// ---- Color grade -----------------------------------------------------------
cbuffer ColorGradeUniforms : register(b0, space3)
{
    float2 grade_inv_resolution;
    float grade_pad0;
    float grade_pad1;
    float4 grade_lift;
    float4 grade_gamma;
    float4 grade_gain;
};

float4 ColorGradePS(float4 position : SV_Position) : SV_Target0
{
    const float2 uv = position.xy * grade_inv_resolution;
    float3 color = source_tex.Sample(source_sampler, uv).rgb;
    color = color * grade_gain.rgb + grade_lift.rgb * (1.0 - color);
    color = pow(max(color, 0.0), 1.0 / max(grade_gamma.rgb, 0.0001));
    return float4(saturate(color), 1.0);
}

// ---- FXAA ------------------------------------------------------------------
cbuffer FxaaUniforms : register(b0, space3)
{
    float2 fxaa_inv_resolution;
    float fxaa_strength;
    float fxaa_enabled;
};

float FxaaLuma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

float4 FxaaPS(float4 position : SV_Position) : SV_Target0
{
    const float2 uv = position.xy * fxaa_inv_resolution;
    const float3 rgbM = source_tex.Sample(source_sampler, uv).rgb;
    if (fxaa_enabled < 0.5)
    {
        return float4(rgbM, 1.0);
    }

    const float2 px = fxaa_inv_resolution;
    const float lumaM = FxaaLuma(rgbM);
    const float lumaNW = FxaaLuma(source_tex.Sample(source_sampler, uv + float2(-px.x, -px.y)).rgb);
    const float lumaNE = FxaaLuma(source_tex.Sample(source_sampler, uv + float2(px.x, -px.y)).rgb);
    const float lumaSW = FxaaLuma(source_tex.Sample(source_sampler, uv + float2(-px.x, px.y)).rgb);
    const float lumaSE = FxaaLuma(source_tex.Sample(source_sampler, uv + float2(px.x, px.y)).rgb);

    const float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    const float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    const float range = lumaMax - lumaMin;
    if (range < max(0.0312, lumaMax * 0.125))
    {
        return float4(rgbM, 1.0);
    }

    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y = ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    const float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.03125, 0.0078125);
    const float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -8.0, 8.0) * px * fxaa_strength;

    const float3 rgbA = 0.5 * (
        source_tex.Sample(source_sampler, uv + dir * (1.0 / 3.0 - 0.5)).rgb +
        source_tex.Sample(source_sampler, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    const float3 rgbB = rgbA * 0.5 + 0.25 * (
        source_tex.Sample(source_sampler, uv + dir * -0.5).rgb +
        source_tex.Sample(source_sampler, uv + dir * 0.5).rgb);

    const float lumaB = FxaaLuma(rgbB);
    return float4((lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB, 1.0);
}

// ---- Copy + ordered dither (AA off) ----------------------------------------
cbuffer CopyDitherUniforms : register(b0, space3)
{
    float2 copy_inv_resolution;
    float copy_pad0;
    float copy_pad1;
};

float CopyDither4x4(float2 position)
{
    const float4x4 bayer = float4x4(
        0.0, 8.0, 2.0, 10.0,
        12.0, 4.0, 14.0, 6.0,
        3.0, 11.0, 1.0, 9.0,
        15.0, 7.0, 13.0, 5.0) / 16.0;
    const uint2 cell = uint2(position) & 3;
    return bayer[cell.x][cell.y];
}

float4 CopyDitherPS(float4 position : SV_Position) : SV_Target0
{
    const float2 uv = position.xy * copy_inv_resolution;
    float3 color = source_tex.Sample(source_sampler, uv).rgb;
    color += (CopyDither4x4(position.xy) - 0.5) / 255.0;
    return float4(saturate(color), 1.0);
}

float4 CopyPS(float4 position : SV_Position) : SV_Target0
{
    const float2 uv = position.xy * copy_inv_resolution;
    return float4(source_tex.Sample(source_sampler, uv).rgb, 1.0);
}

// ---- TAA -------------------------------------------------------------------
// Proper temporal resolve (not a blur):
//   1. histUV = uv - velocity * 0.5  (velocity is NDC xy delta from geometry)
//   2. Sample history; reject when histUV leaves the screen
//   3. Neighborhood AABB clip (3x3) in LDR color space
//   4. Optional depth rejection at histUV vs current depth
//   5. out = lerp(current, clippedHistory, weight * acceptance)
//   6. On reset / first frame: out = current
Texture2D velocity_tex : register(t2, space2);
SamplerState velocity_sampler : register(s2, space2);

Texture2D depth_tex : register(t3, space2);
SamplerState depth_sampler : register(s3, space2);

cbuffer TaaUniforms : register(b0, space3)
{
    float2 taa_inv_resolution;
    float taa_history_weight;
    float taa_reset;
    float taa_sharpness;
    float taa_depth_reject;
    float taa_debug_rejection;
    float taa_pad0;
};

float4 TaaPS(float4 position : SV_Position) : SV_Target0
{
    const float2 uv = position.xy * taa_inv_resolution;
    const float2 px = taa_inv_resolution;
    const float3 current = source_tex.Sample(source_sampler, uv).rgb;

    if (taa_reset > 0.5)
    {
        return float4(current, 1.0);
    }

    const float2 velocity = velocity_tex.Sample(velocity_sampler, uv).xy;
    const float2 histUV = uv - velocity * 0.5;
    float acceptance = 1.0;
    if (any(histUV < 0.0) || any(histUV > 1.0))
    {
        acceptance = 0.0;
    }

    float3 history = current;
    if (acceptance > 0.0)
    {
        history = bloom_tex.Sample(bloom_sampler, histUV).rgb;
        if (taa_depth_reject > 0.5)
        {
            const float curDepth = depth_tex.Sample(depth_sampler, uv).r;
            const float histDepth = depth_tex.Sample(depth_sampler, histUV).r;
            if (abs(curDepth - histDepth) > 0.002)
            {
                acceptance = 0.0;
            }
        }
    }

    float3 minC = current;
    float3 maxC = current;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float3 sampleC = source_tex.Sample(source_sampler, uv + float2(x, y) * px).rgb;
            minC = min(minC, sampleC);
            maxC = max(maxC, sampleC);
        }
    }
    const float3 clippedHistory = clamp(history, minC, maxC);
    const float weight = taa_history_weight * acceptance;
    float3 resolved = lerp(current, clippedHistory, weight);

    if (taa_sharpness > 0.0)
    {
        const float3 blur = (
            source_tex.Sample(source_sampler, uv + float2(px.x, 0.0)).rgb +
            source_tex.Sample(source_sampler, uv - float2(px.x, 0.0)).rgb +
            source_tex.Sample(source_sampler, uv + float2(0.0, px.y)).rgb +
            source_tex.Sample(source_sampler, uv - float2(0.0, px.y)).rgb) * 0.25;
        resolved = lerp(resolved, resolved + (resolved - blur) * taa_sharpness, weight);
    }

    if (taa_debug_rejection > 0.5)
    {
        return float4(1.0 - acceptance, 0.0, 0.0, 1.0);
    }
    return float4(resolved, 1.0);
}

// ---- Motion vectors debug --------------------------------------------------
cbuffer MotionVectorsUniforms : register(b0, space3)
{
    float2 mv_inv_resolution;
    float mv_scale;
    float mv_pad0;
};

float4 MotionVectorsPS(float4 position : SV_Position) : SV_Target0
{
    const float2 uv = position.xy * mv_inv_resolution;
    const float2 velocity = velocity_tex.Sample(velocity_sampler, uv).xy;
    const float2 vis = velocity * mv_scale + 0.5;
    return float4(saturate(vis), 0.0, 1.0);
}
