#pragma once

// FDX Animation runtime tick. Header-only glue between AnimatorComponent and the
// skinning matrices the mesh draw path reads. Shared by the editor viewport and
// fadix_player so playback is identical in both.

#include "engine/animation/AnimationPlayer.hpp"
#include "engine/assets/AssetHandle.hpp"
#include "engine/assets/GltfMeshAsset.hpp"
#include "runtime/Components.hpp"

#include <entt/entt.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <utility>

namespace fadix
{
// --- Transform animation (any entity, no skinned mesh required) --------------
// Reuses AnimationChannel: JointIndex is ignored, Target picks which part of the
// entity's TransformComponent the channel drives. Kept header-only so the editor
// viewport, fadix_player and the smoke test all share one implementation.

// Locate the channel that drives `property`, or create an empty one.
[[nodiscard]] inline AnimationChannel& FindOrCreateTransformChannel(
    AnimationClipAsset& clip, const AnimationChannel::Property property)
{
    for (AnimationChannel& channel : clip.Channels)
    {
        if (channel.Target == property)
        {
            return channel;
        }
    }
    AnimationChannel channel;
    channel.JointIndex = -1;
    channel.Target = property;
    clip.Channels.push_back(std::move(channel));
    return clip.Channels.back();
}

[[nodiscard]] inline glm::vec4 TransformPropertyValue(
    const TransformComponent& transform, const AnimationChannel::Property property)
{
    switch (property)
    {
    case AnimationChannel::Property::Translation:
        return glm::vec4{transform.Position, 0.0F};
    case AnimationChannel::Property::Rotation:
        return glm::vec4{
            transform.Rotation.x, transform.Rotation.y, transform.Rotation.z, transform.Rotation.w};
    case AnimationChannel::Property::Scale:
        return glm::vec4{transform.Scale, 0.0F};
    }
    return glm::vec4{0.0F};
}

// Add or replace a key at `time` capturing the entity's current value for `property`.
inline void KeyTransformProperty(TransformAnimatorComponent& anim, const TransformComponent& transform,
    const AnimationChannel::Property property, const float time)
{
    AnimationChannel& channel = FindOrCreateTransformChannel(anim.Clip, property);
    const glm::vec4 value = TransformPropertyValue(transform, property);
    for (AnimationKeyframe& key : channel.Keyframes)
    {
        if (std::abs(key.Time - time) < 1.0e-4F)
        {
            key.Value = value;
            return;
        }
    }
    channel.Keyframes.push_back({time, value});
    std::sort(channel.Keyframes.begin(), channel.Keyframes.end(),
        [](const AnimationKeyframe& a, const AnimationKeyframe& b) { return a.Time < b.Time; });
    anim.Clip.Duration = std::max(anim.Clip.Duration, time);
}

[[nodiscard]] inline bool TransformKeyedAt(const AnimationClipAsset& clip,
    const AnimationChannel::Property property, const float time)
{
    for (const AnimationChannel& channel : clip.Channels)
    {
        if (channel.Target != property)
        {
            continue;
        }
        for (const AnimationKeyframe& key : channel.Keyframes)
        {
            if (std::abs(key.Time - time) < 1.0e-4F)
            {
                return true;
            }
        }
    }
    return false;
}

// Write the clip's pose at `time` into the entity's transform. Properties without a
// channel keep their current value.
inline void ApplyTransformClip(
    const AnimationClipAsset& clip, const float time, TransformComponent& transform)
{
    for (const AnimationChannel& channel : clip.Channels)
    {
        if (channel.Keyframes.empty())
        {
            continue;
        }
        glm::vec3 translation = transform.Position;
        glm::quat rotation = transform.Rotation;
        glm::vec3 scale = transform.Scale;
        channel.Sample(time, translation, rotation, scale);
        switch (channel.Target)
        {
        case AnimationChannel::Property::Translation: transform.Position = translation; break;
        case AnimationChannel::Property::Rotation: transform.Rotation = rotation; break;
        case AnimationChannel::Property::Scale: transform.Scale = scale; break;
        }
    }
}

// Per-frame: advance every TransformAnimatorComponent (when playing) and write its
// pose into the entity's TransformComponent. Runs before skeletal skinning so an
// entity may carry both: transform clip poses the entity, skeletal poses the skin.
inline void UpdateTransformAnimations(entt::registry& registry, const float dt)
{
    for (auto&& [entity, anim, transform] :
        registry.view<TransformAnimatorComponent, TransformComponent>().each())
    {
        // Only a playing animator drives the transform; a paused one leaves it
        // editable so the Inspector can re-pose and key (the panel one-shot-applies
        // for scrub preview). Mirrors how skeletal advance is gated on Playing.
        if (anim.Clip.Channels.empty() || !anim.Playing)
        {
            continue;
        }
        anim.CurrentTime += dt * anim.Speed;
        if (anim.Clip.Duration > 0.0F)
        {
            if (anim.Loop)
            {
                anim.CurrentTime = std::fmod(anim.CurrentTime, anim.Clip.Duration);
                if (anim.CurrentTime < 0.0F)
                {
                    anim.CurrentTime += anim.Clip.Duration;
                }
            }
            else
            {
                anim.CurrentTime = std::clamp(anim.CurrentTime, 0.0F, anim.Clip.Duration);
            }
        }
        ApplyTransformClip(anim.Clip, anim.CurrentTime, transform);
    }
}

// Locate a clip by name among an imported model's clips (imported glTF clips plus
// any authored .fdxanim clips merged into Animations). Empty name -> first clip.
[[nodiscard]] inline const AnimationClipAsset* FindAnimationClip(
    const GltfMeshAsset& gltf, const std::string& name)
{
    if (gltf.Animations.empty())
    {
        return nullptr;
    }
    if (name.empty())
    {
        return &gltf.Animations.front();
    }
    for (const AnimationClipAsset& clip : gltf.Animations)
    {
        if (clip.Name == name)
        {
            return &clip;
        }
    }
    return nullptr;
}

// Ensure a skinned+animated imported mesh has the Skeleton/Animator components the
// tick needs. Idempotent; logs the clip count once (only when it first attaches).
inline void AttachImportedAnimation(
    entt::registry& registry, const entt::entity entity, const GltfMeshAsset& gltf,
    const std::function<void(const std::string&)>& log = nullptr)
{
    if (!gltf.HasSkeleton || gltf.Animations.empty())
    {
        return;
    }
    if (!registry.all_of<SkeletonComponent>(entity))
    {
        SkeletonComponent skeleton;
        skeleton.HasSkeleton = true;
        skeleton.JointCount = static_cast<int>(gltf.Skeleton.Joints.size());
        registry.emplace<SkeletonComponent>(entity, std::move(skeleton));
    }
    if (!registry.all_of<AnimatorComponent>(entity))
    {
        AnimatorComponent animator;
        animator.ClipName = gltf.Animations.front().Name;
        registry.emplace<AnimatorComponent>(entity, std::move(animator));
        if (log)
        {
            log("[FDX Animation] '" + gltf.DebugName + "' - " +
                std::to_string(gltf.Animations.size()) + " clip(s), " +
                std::to_string(gltf.Skeleton.Joints.size()) + " joints");
        }
    }
}

// Per-frame: advance every AnimatorComponent and bake skinning matrices into its
// SkeletonComponent so the draw path shows the posed mesh instead of bind pose.
// ponytail: rebuilds a full SkeletonPose per animated entity each frame. Fine for
// a handful of actors; cache a persistent pose per entity if it ever shows up hot.
inline void UpdateWorldAnimations(
    entt::registry& registry,
    const std::function<const GltfMeshAsset*(const AssetHandle&)>& resolveMesh,
    const float dt)
{
    for (auto&& [entity, mesh] : registry.view<MeshComponent>().each())
    {
        if (!mesh.ImportedMesh.IsValid())
        {
            continue;
        }
        const GltfMeshAsset* gltf = resolveMesh(mesh.ImportedMesh);
        if (gltf == nullptr || !gltf->HasSkeleton || gltf->Animations.empty())
        {
            continue;
        }
        // ponytail: re-attaches every frame, so removing Animator/Skeleton from a
        // skinned+animated mesh doesn't stick. Acceptable (playback is the whole
        // point); gate on an explicit "no animation" opt-out flag if that matters.
        AttachImportedAnimation(registry, entity, *gltf);

        auto* animator = registry.try_get<AnimatorComponent>(entity);
        auto* skeleton = registry.try_get<SkeletonComponent>(entity);
        if (animator == nullptr || skeleton == nullptr)
        {
            continue;
        }
        const AnimationClipAsset* clip = FindAnimationClip(*gltf, animator->ClipName);
        if (clip == nullptr)
        {
            continue;
        }

        SkeletonPose pose;
        pose.Skeleton = gltf->Skeleton;
        AnimationPlayer player;
        player.SetClip(clip);
        player.SetTime(animator->CurrentTime);
        player.Update(animator->Playing ? dt : 0.0F, animator->Speed, animator->Loop, pose);
        animator->CurrentTime = player.GetTime();

        skeleton->HasSkeleton = true;
        skeleton->JointCount = static_cast<int>(pose.Skeleton.Joints.size());
        skeleton->JointMatrices = pose.Skeleton.GetJointMatrices();
    }
}
}
