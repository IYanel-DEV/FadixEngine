// Fadix editor viewport shaders (SDL GPU HLSL register layout):
//   vertex uniforms   -> b0, space1
//   fragment uniforms -> b0, space3
// Entry points:
//   VertexMain / FragmentMain  : lit world geometry + procedural ground grid
//   VertexMain / UnlitFragmentMain : editor overlay + line geometry (HDR scene)
//   VertexMain / UnlitLdrFragmentMain : post-TAA LDR overlay + line + gizmo
//   SkyVertexMain / SkyFragmentMain : full-screen procedural sky
//
// Motion-vector convention (matches TAA reprojection):
//   velocity.xy = (currNDC.xy / currNDC.w) - (prevNDC.xy / prevNDC.w)

cbuffer VertexUniforms : register(b0, space1)
{
    float4x4 view_projection;
    float4x4 prev_view_projection;
    float4x4 model;
    float4x4 prev_model;
    float4 skin_params; // x = has_skeleton
    float4x4 normal_matrix;
};

cbuffer BoneUniforms : register(b1, space1)
{
    float4x4 bones[128];
};

cbuffer PrevBoneUniforms : register(b2, space1)
{
    float4x4 prev_bones[128];
};

#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS 8

cbuffer FragmentUniforms : register(b0, space3)
{
    float4 base_color;      // rgb albedo tint, a alpha
    float4 material;        // x metallic, y roughness, z grid blend, w ortho flag
    float4 emissive;        // rgb emissive color, a intensity
    float4 uv_params;       // x uv scale x, y uv scale y, z uv offset x, w uv offset y
    float4 alpha_params;    // x alpha mode (0=opaque, 1=mask, 2=blend), y cutoff, z reserved, w reserved
    float4 light_direction; // xyz direction light travels, w unused
    float4 light_color;     // rgb color, a intensity
    float4 camera_position; // xyz world-space eye
    float4 ambient_sky;     // rgb hemisphere sky ambient, a intensity
    float4 ambient_ground;  // rgb hemisphere ground ambient, a unused
    float4 env_params;      // x exposure, y fog enabled
    float4 fog_color_density; // rgb fog color, a density
    float4 fog_range;       // x fog start, y fog end
    float4 light_counts;    // x point count, y spot count
    float4 point_position_range[MAX_POINT_LIGHTS];   // xyz position, w range
    float4 point_color_intensity[MAX_POINT_LIGHTS];  // rgb color, a intensity
    float4 point_params[MAX_POINT_LIGHTS];           // x falloff exponent
    float4 spot_position_range[MAX_SPOT_LIGHTS];     // xyz position, w range
    float4 spot_direction_falloff[MAX_SPOT_LIGHTS];  // xyz direction, w falloff
    float4 spot_color_intensity[MAX_SPOT_LIGHTS];    // rgb color, a intensity
    float4 spot_cone[MAX_SPOT_LIGHTS];               // x cos(inner), y cos(outer)
    float4 shadow_params;      // x filter radius (texels), y bias, z strength, w enabled
    float4 cascade_splits;     // xyzw view-space far distance of cascade 0..3
    float4 cascade_texel;      // xyzw UV texel size (1/resolution) of cascade 0..3
    float4 cascade_count;      // x active cascade count
    float4 camera_forward;     // xyz unit camera view direction (view-space depth axis)
    float4x4 cascade_light_space[4];
    float4 debug_params;       // x mode (ViewportDebugView), y cascade count (diagnostic)
    float4 debug_splits;       // x split2, y split3, z shadow distance, w unused
};

Texture2D base_color_tex : register(t0, space2);
SamplerState base_color_sampler : register(s0, space2);

Texture2D normal_tex : register(t1, space2);
SamplerState normal_sampler : register(s1, space2);

Texture2D metallic_roughness_tex : register(t2, space2);
SamplerState metallic_roughness_sampler : register(s2, space2);

Texture2D emissive_tex : register(t3, space2);
SamplerState emissive_sampler : register(s3, space2);

Texture2D shadow_map : register(t4, space2);   // cascade 0
SamplerState shadow_sampler : register(s4, space2);
Texture2D shadow_map1 : register(t5, space2);  // cascade 1
SamplerState shadow_sampler1 : register(s5, space2);
Texture2D shadow_map2 : register(t6, space2);  // cascade 2
SamplerState shadow_sampler2 : register(s6, space2);
Texture2D shadow_map3 : register(t7, space2);  // cascade 3
SamplerState shadow_sampler3 : register(s7, space2);

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
    float4 joint_indices : TEXCOORD5;
    float4 joint_weights : TEXCOORD6;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 world_position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float4 color : TEXCOORD4;
    float2 velocity : TEXCOORD5;
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
    float4 local_position = float4(input.position, 1.0);
    float4 prev_local_position = local_position;
    float3 local_normal = input.normal;
    float3 local_tangent = input.tangent.xyz;
    if (skin_params.x > 0.5)
    {
        const float4x4 skin =
            input.joint_weights.x * bones[int(input.joint_indices.x)] +
            input.joint_weights.y * bones[int(input.joint_indices.y)] +
            input.joint_weights.z * bones[int(input.joint_indices.z)] +
            input.joint_weights.w * bones[int(input.joint_indices.w)];
        const float4x4 prev_skin =
            input.joint_weights.x * prev_bones[int(input.joint_indices.x)] +
            input.joint_weights.y * prev_bones[int(input.joint_indices.y)] +
            input.joint_weights.z * prev_bones[int(input.joint_indices.z)] +
            input.joint_weights.w * prev_bones[int(input.joint_indices.w)];
        local_position = mul(skin, local_position);
        prev_local_position = mul(prev_skin, float4(input.position, 1.0));
        local_normal = mul((float3x3)skin, local_normal);
        local_tangent = mul((float3x3)skin, local_tangent);
    }
    const float4 world_position = mul(model, local_position);
    const float4 prev_world_position = mul(prev_model, prev_local_position);
    output.position = mul(view_projection, world_position);
    output.world_position = world_position.xyz;
    output.normal = normalize(mul((float3x3)normal_matrix, local_normal));
    output.tangent = float4(mul((float3x3)model, local_tangent), input.tangent.w);
    output.uv = input.uv;
    output.color = input.color;
    output.velocity = NdcVelocity(
        output.position, mul(prev_view_projection, prev_world_position));
    return output;
}

float3 TonemapAces(float3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 LinearToSrgb(float3 x)
{
    return pow(max(x, 0.0), 1.0 / 2.2);
}

float3 SrgbToLinear(float3 x)
{
    return pow(max(x, 0.0), 2.2);
}

float GridLine(float coordinate, float scale)
{
    const float value = coordinate * scale;
    const float width = max(fwidth(value), 0.0001);
    const float subpixel_fade = 1.0 - smoothstep(0.25, 0.6, width);
    return (1.0 - smoothstep(0.0, width * 1.35, abs(frac(value - 0.5) - 0.5))) * subpixel_fade;
}

static const float PI = 3.14159265359;

float DistributionGGX(float3 normal, float3 half_vector, float roughness)
{
    const float alpha = roughness * roughness;
    const float alpha2 = alpha * alpha;
    const float n_dot_h = saturate(dot(normal, half_vector));
    const float denominator = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(PI * denominator * denominator, 1.0e-5);
}

float GeometrySchlickGGX(float n_dot_direction, float roughness)
{
    const float r = roughness + 1.0;
    const float k = (r * r) / 8.0;
    return n_dot_direction / max(n_dot_direction * (1.0 - k) + k, 1.0e-5);
}

float GeometrySmith(float3 normal, float3 to_eye, float3 to_light, float roughness)
{
    return GeometrySchlickGGX(saturate(dot(normal, to_eye)), roughness) *
        GeometrySchlickGGX(saturate(dot(normal, to_light)), roughness);
}

float3 FresnelSchlick(float cos_theta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cos_theta), 5.0);
}

float DistanceAttenuation(float distance_to_light, float range, float falloff)
{
    const float normalized_distance = distance_to_light / max(range, 0.001);
    const float range_window = saturate(1.0 - normalized_distance * normalized_distance *
        normalized_distance * normalized_distance);
    return pow(range_window, max(falloff, 0.01)) /
        max(distance_to_light * distance_to_light, 1.0);
}

float3 ShadeLight(
    float3 normal,
    float3 to_eye,
    float3 to_light,
    float3 albedo,
    float metallic,
    float roughness,
    float3 radiance)
{
    const float n_dot_l = saturate(dot(normal, to_light));
    const float n_dot_v = saturate(dot(normal, to_eye));
    if (n_dot_l <= 0.0 || n_dot_v <= 0.0)
    {
        return 0.0;
    }

    const float3 half_vector = normalize(to_light + to_eye);
    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    const float3 fresnel = FresnelSchlick(saturate(dot(half_vector, to_eye)), f0);
    const float distribution = DistributionGGX(normal, half_vector, roughness);
    const float geometry = GeometrySmith(normal, to_eye, to_light, roughness);
    const float3 specular =
        (distribution * geometry * fresnel) / max(4.0 * n_dot_v * n_dot_l, 1.0e-4);
    const float3 diffuse_weight = (1.0 - fresnel) * (1.0 - metallic);
    const float3 diffuse = diffuse_weight * albedo / PI;
    return (diffuse + specular) * radiance * n_dot_l;
}

// View-space depth (positive in front of the camera). Used to pick a cascade so
// selection matches how the CPU computed the split distances.
float ViewDepth(float3 world_position)
{
    return dot(world_position - camera_position.xyz, camera_forward.xyz);
}

// The near cascade whose far split still covers this depth (clamped to the last).
int SelectCascade(float view_depth, int count)
{
    int index = count - 1;
    [unroll]
    for (int c = 0; c < 4; ++c)
    {
        if (c < count && view_depth <= cascade_splits[c])
        {
            index = c;
            break;
        }
    }
    return index;
}

// PCF-samples one cascade's depth map. Returns 1 (lit) when the point falls
// outside this cascade's light frustum. `radius` is the half-kernel in texels.
float SampleCascadeShadow(int index, float3 world_position, int radius)
{
    const float4 light_space_pos = mul(cascade_light_space[index], float4(world_position, 1.0));
    const float3 proj = light_space_pos.xyz / max(light_space_pos.w, 1e-6);
    // SDL GPU NDC is Y-up while texture V is Y-down. Projected shadow
    // coordinates therefore need one vertical flip (backend-independent).
    const float2 uv = float2(proj.x * 0.5 + 0.5, 0.5 - proj.y * 0.5);
    if (proj.z < 0.0 || proj.z > 1.0 || uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    {
        return 1.0;
    }
    const float current_depth = proj.z;
    const float bias = shadow_params.y;
    const float texel = cascade_texel[index];
    float shadow = 0.0;
    float samples = 0.0;
    for (int x = -radius; x <= radius; ++x)
    {
        for (int y = -radius; y <= radius; ++y)
        {
            const float2 o = uv + float2(x, y) * texel;
            // Shadow maps have no mips; SampleLevel(...,0) is gradient-free so the
            // PCF kernel can use a dynamic (quality-driven) radius without fxc
            // trying to unroll a Sample() gradient instruction.
            float closest;
            if (index == 0)
            {
                closest = shadow_map.SampleLevel(shadow_sampler, o, 0.0).r;
            }
            else if (index == 1)
            {
                closest = shadow_map1.SampleLevel(shadow_sampler1, o, 0.0).r;
            }
            else if (index == 2)
            {
                closest = shadow_map2.SampleLevel(shadow_sampler2, o, 0.0).r;
            }
            else
            {
                closest = shadow_map3.SampleLevel(shadow_sampler3, o, 0.0).r;
            }
            shadow += (current_depth - bias > closest) ? 0.0 : 1.0;
            samples += 1.0;
        }
    }
    return shadow / max(samples, 1.0);
}

float ComputeShadow(float3 world_position)
{
    if (shadow_params.w < 0.5)
    {
        return 1.0;
    }
    const int count = max((int)cascade_count.x, 1);
    const int radius = max((int)shadow_params.x, 0);
    const float view_depth = ViewDepth(world_position);
    const int index = SelectCascade(view_depth, count);

    float shadow = SampleCascadeShadow(index, world_position, radius);

    // Blend across the split boundary so cascade seams are not visible.
    const float near_split = (index == 0) ? 0.0 : cascade_splits[index - 1];
    const float far_split = cascade_splits[index];
    const float band = max((far_split - near_split) * 0.1, 0.001);
    if (index + 1 < count)
    {
        const float t = saturate((view_depth - (far_split - band)) / band);
        if (t > 0.0)
        {
            shadow = lerp(shadow, SampleCascadeShadow(index + 1, world_position, radius), t);
        }
    }
    return lerp(1.0 - shadow_params.z, 1.0, shadow);
}

// Debug color for the cascade the sampler above actually selects (same splits).
float3 CascadeDebugColor(float3 world_position)
{
    static const float3 colors[4] = {
        float3(1.0, 0.15, 0.15),
        float3(0.15, 0.85, 0.25),
        float3(0.20, 0.45, 1.0),
        float3(1.0, 0.85, 0.15)
    };
    const int count = max((int)cascade_count.x, 1);
    const int index = SelectCascade(ViewDepth(world_position), count);
    return colors[clamp(index, 0, 3)];
}

FragmentOut FragmentMain(VertexOutput input)
{
    FragmentOut output;
    const float2 uv = input.uv * uv_params.xy + uv_params.zw;
    float4 base_color_sample = base_color_tex.Sample(base_color_sampler, uv);
    float3 albedo = base_color.rgb * input.color.rgb * base_color_sample.rgb;
    float output_alpha = base_color.a * input.color.a * base_color_sample.a;

    if (alpha_params.x == 1.0 && output_alpha < alpha_params.y)
    {
        clip(-1.0);
    }

    float3 normal = normalize(input.normal);
    const float3 normal_sample = normal_tex.Sample(normal_sampler, uv).xyz * 2.0 - 1.0;
    if (abs(input.tangent.w) > 0.5 && dot(input.tangent.xyz, input.tangent.xyz) > 1.0e-6)
    {
        const float3 tangent = normalize(input.tangent.xyz - normal * dot(input.tangent.xyz, normal));
        const float3 bitangent = normalize(cross(normal, tangent)) * input.tangent.w;
        const float3x3 tbn = float3x3(tangent, bitangent, normal);
        normal = normalize(mul(normal_sample, tbn));
    }

    float4 mr_sample = metallic_roughness_tex.Sample(metallic_roughness_sampler, uv);
    float metallic = saturate(material.x * mr_sample.b);
    float roughness = clamp(material.y * mr_sample.g, 0.04, 1.0);

    float3 emissive_sample = emissive_tex.Sample(emissive_sampler, uv).rgb;
    float3 emissive_light = emissive.rgb * emissive.a * emissive_sample;

    const float3 to_light = normalize(-light_direction.xyz);
    const float3 to_eye = normalize(camera_position.xyz - input.world_position);

    float3 sun = ShadeLight(normal, to_eye, to_light, albedo, metallic, roughness, light_color.rgb * light_color.a);
    sun *= ComputeShadow(input.world_position);
    float3 direct = sun;

    const int point_count = (int)light_counts.x;
    for (int point_index = 0; point_index < point_count; ++point_index)
    {
        const float3 to_light_vector = point_position_range[point_index].xyz - input.world_position;
        const float light_distance = length(to_light_vector);
        const float range = max(point_position_range[point_index].w, 0.001);
        if (light_distance >= range) continue;
        const float falloff = max(point_params[point_index].x, 0.01);
        const float attenuation = DistanceAttenuation(light_distance, range, falloff);
        const float3 radiance = point_color_intensity[point_index].rgb * point_color_intensity[point_index].a * attenuation;
        direct += ShadeLight(normal, to_eye, to_light_vector / max(light_distance, 0.001), albedo, metallic, roughness, radiance);
    }

    const int spot_count = (int)light_counts.y;
    for (int spot_index = 0; spot_index < spot_count; ++spot_index)
    {
        const float3 to_light_vector = spot_position_range[spot_index].xyz - input.world_position;
        const float light_distance = length(to_light_vector);
        const float range = max(spot_position_range[spot_index].w, 0.001);
        if (light_distance >= range) continue;
        const float3 to_light_direction = to_light_vector / max(light_distance, 0.001);
        const float cos_angle = dot(-to_light_direction, normalize(spot_direction_falloff[spot_index].xyz));
        const float cos_inner = spot_cone[spot_index].x;
        const float cos_outer = spot_cone[spot_index].y;
        if (cos_angle <= cos_outer) continue;
        const float cone = smoothstep(cos_outer, max(cos_inner, cos_outer + 0.0001), cos_angle);
        const float falloff = max(spot_direction_falloff[spot_index].w, 0.01);
        const float attenuation = DistanceAttenuation(light_distance, range, falloff) * cone;
        const float3 radiance = spot_color_intensity[spot_index].rgb * spot_color_intensity[spot_index].a * attenuation;
        direct += ShadeLight(normal, to_eye, to_light_direction, albedo, metallic, roughness, radiance);
    }

    const float hemisphere = normal.y * 0.5 + 0.5;
    const float3 ambient_color =
        lerp(ambient_ground.rgb, ambient_sky.rgb, hemisphere) * ambient_sky.a;
    const float3 ambient_f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    const float3 ambient =
        ambient_color * (albedo * (1.0 - metallic) + ambient_f0 * metallic);

    float3 shaded = ambient + direct + emissive_light;

    if (material.z > 0.5)
    {
        // The editor grid is a reference aid, not a scene surface. Keeping its
        // base unlit prevents scene lights, exposure and shadows from turning it
        // into a moving bright/dark plane around the camera.
        shaded = float3(0.055, 0.065, 0.080);
        const float eye_distance = length(input.world_position.xz - camera_position.xz);
        const float minor_distance_fade = saturate(1.0 - eye_distance / 45.0);
        const float major_distance_fade = saturate(1.0 - eye_distance / 180.0);
        const float minor = max(GridLine(input.world_position.x, 1.0), GridLine(input.world_position.z, 1.0)) * minor_distance_fade;
        const float major = max(GridLine(input.world_position.x, 0.1), GridLine(input.world_position.z, 0.1)) * major_distance_fade;
        const float x_axis = 1.0 - smoothstep(0.0, max(fwidth(input.world_position.z), 0.001) * 1.5, abs(input.world_position.z));
        const float z_axis = 1.0 - smoothstep(0.0, max(fwidth(input.world_position.x), 0.001) * 1.5, abs(input.world_position.x));
        const float3 minor_color = float3(0.16, 0.19, 0.23);
        const float3 major_color = float3(0.30, 0.36, 0.43);
        shaded = lerp(shaded, minor_color, minor * 0.45);
        shaded = lerp(shaded, major_color, major * 0.8);
        shaded = lerp(shaded, float3(0.75, 0.16, 0.14), x_axis * 0.9);
        shaded = lerp(shaded, float3(0.14, 0.36, 0.80), z_axis * 0.9);
        if (base_color.a < 0.999)
        {
            output_alpha = saturate(max(max(minor * 0.45, major * 0.8), max(x_axis, z_axis) * 0.9));
        }
    }

    if (env_params.y > 0.5)
    {
        const float eye_distance = length(input.world_position - camera_position.xyz);
        const float fog_span = clamp(eye_distance - fog_range.x, 0.0, max(fog_range.y - fog_range.x, 0.001));
        const float fog_amount = 1.0 - exp(-fog_color_density.a * fog_span);
        shaded = lerp(shaded, fog_color_density.rgb, saturate(fog_amount));
    }

    output.color = float4(shaded, output_alpha);
    output.velocity = input.velocity;

    const int debug_mode = (int)debug_params.x;
    if (debug_mode == 1)
    {
        output.color = float4(albedo, 1.0);
    }
    else if (debug_mode == 2)
    {
        output.color = float4(normal * 0.5 + 0.5, 1.0);
    }
    else if (debug_mode == 3)
    {
        output.color = float4(roughness, roughness, roughness, 1.0);
    }
    else if (debug_mode == 4)
    {
        output.color = float4(metallic, metallic, metallic, 1.0);
    }
    else if (debug_mode == 5)
    {
        const float occlusion = ComputeShadow(input.world_position);
        output.color = float4(occlusion, occlusion, occlusion, 1.0);
    }
    else if (debug_mode == 6)
    {
        const float depth = input.position.z / max(input.position.w, 1.0e-6);
        output.color = float4(depth, depth, depth, 1.0);
    }
    else if (debug_mode == 7)
    {
        output.color = float4(CascadeDebugColor(input.world_position), 1.0);
    }

    return output;
}

FragmentOut UnlitFragmentMain(VertexOutput input)
{
    FragmentOut output;
    output.color = float4(SrgbToLinear(base_color.rgb * input.color.rgb), base_color.a * input.color.a);
    output.velocity = float2(0.0, 0.0);
    return output;
}

FragmentOut UnlitLdrFragmentMain(VertexOutput input)
{
    FragmentOut output;
    output.color = float4(base_color.rgb * input.color.rgb, base_color.a * input.color.a);
    output.velocity = float2(0.0, 0.0);
    return output;
}

cbuffer SkyUniforms : register(b0, space3)
{
    float4x4 inverse_view_projection;
    float4x4 prev_inverse_view_projection;
    float4x4 sky_view_projection;
    float4x4 sky_prev_view_projection;
    float4 sun_direction;
    float4 sun_color;
    float4 moon_direction;
    float4 moon_color;
    float4 sky_params;
    float4 sky_zenith;
    float4 sky_horizon;
    float4 sky_ground;
};

float CelestialHash(float3 value)
{
    value = frac(value * 0.1031);
    value += dot(value, value.yzx + 33.33);
    return frac((value.x + value.y) * value.z);
}

struct SkyVertexOutput
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

SkyVertexOutput SkyVertexMain(uint vertex_id : SV_VertexID)
{
    SkyVertexOutput output;
    const float2 corners[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    output.ndc = corners[vertex_id];
    output.position = float4(output.ndc, 0.99999, 1.0);
    return output;
}

FragmentOut SkyFragmentMain(SkyVertexOutput input)
{
    FragmentOut output;
    float4 world = mul(inverse_view_projection, float4(input.ndc, 1.0, 1.0));
    const float3 direction = normalize(world.xyz / max(abs(world.w), 1.0e-6));
    const float3 to_sun = normalize(-sun_direction.xyz);
    const float3 to_moon = normalize(-moon_direction.xyz);

    const float height = direction.y;
    const float daylight = saturate(sky_params.z);
    const float3 zenith = lerp(float3(0.004, 0.009, 0.030), sky_zenith.rgb, daylight);
    const float3 horizon = lerp(float3(0.018, 0.030, 0.065), sky_horizon.rgb, daylight);
    const float3 ground = lerp(float3(0.003, 0.006, 0.014), sky_ground.rgb, daylight);

    float3 sky;
    if (height >= 0.0)
    {
        sky = lerp(horizon, zenith, pow(saturate(height), 0.55));
    }
    else
    {
        sky = lerp(horizon, ground, saturate(-height * 5.0));
    }

    const float sun_amount = saturate(dot(direction, to_sun));
    const float disc = smoothstep(0.9995, 0.99985, sun_amount);
    const float glow = pow(sun_amount, 96.0) * 0.55 + pow(sun_amount, 8.0) * 0.10;
    const float horizon_tint = pow(1.0 - abs(height), 6.0) * saturate(to_sun.y + 0.4);
    const float sun_strength = saturate(sun_color.a / 3.0) * sun_direction.w;
    sky += sun_color.rgb * (disc * 12.0 + glow) * sun_strength;
    sky += float3(0.9, 0.62, 0.35) * horizon_tint * 0.15 * sun_direction.w;

    // A full moon is always opposite the linked Sun. It remains a clean disc
    // instead of reusing the Sun glow, and supplies a subtle cool halo.
    const float moon_amount = saturate(dot(direction, to_moon));
    const float moon_disc = smoothstep(0.99935, 0.99972, moon_amount);
    const float moon_halo = pow(moon_amount, 128.0) * 0.18 + pow(moon_amount, 24.0) * 0.035;
    const float moon_strength = saturate(moon_color.a / 0.35) * moon_direction.w;
    sky += moon_color.rgb * (moon_disc * 3.2 + moon_halo) * moon_strength;

    // Sparse direction-locked stars fade naturally through dawn and below the horizon.
    const float star_noise = CelestialHash(floor(direction * 700.0));
    const float stars = smoothstep(0.9985, 1.0, star_noise) *
        pow(saturate(height), 0.45) * (1.0 - daylight);
    sky += float3(0.72, 0.82, 1.0) * stars * 1.6;

    output.color = float4(sky, 1.0);

    const float4 far_point = mul(inverse_view_projection, float4(input.ndc, 1.0, 1.0));
    const float3 world_far = far_point.xyz / max(abs(far_point.w), 1.0e-6) * 10000.0;
    const float4 prev_clip = mul(sky_prev_view_projection, float4(world_far, 1.0));
    output.velocity = NdcVelocity(float4(input.ndc, 0.99999, 1.0), prev_clip);
    return output;
}
