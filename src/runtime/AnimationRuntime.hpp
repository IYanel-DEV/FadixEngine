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
#include <string_view>
#include <utility>

namespace fadix
{
template <typename Animator>
inline void ClearAnimationBlend(Animator& animator)
{
    animator.ClearBlend();
}

[[nodiscard]] inline float AdvanceClipTime(float time, const AnimationClipAsset& clip,
    const float dt, const float speed, const bool loop)
{
    time += dt * speed;
    if (clip.Duration <= 0.0F)
    {
        return time;
    }
    if (!loop)
    {
        return std::clamp(time, 0.0F, clip.Duration);
    }
    time = std::fmod(time, clip.Duration);
    return time < 0.0F ? time + clip.Duration : time;
}

inline void BlendTransformPoses(const TransformComponent& from, const TransformComponent& to,
    const float requestedWeight, TransformComponent& result)
{
    const float weight = std::clamp(requestedWeight, 0.0F, 1.0F);
    result.Position = glm::mix(from.Position, to.Position, weight);
    result.Rotation = glm::normalize(glm::slerp(from.Rotation, to.Rotation, weight));
    result.Scale = glm::mix(from.Scale, to.Scale, weight);
}

struct BlendTree1DResult
{
    std::string ClipA;
    std::string ClipB; // empty = single clip, no blending needed
    float Weight{0.0F};
};

// entries is taken by value so it can be sorted locally without touching the state.
// Returns a single-clip result (ClipB empty, Weight=0) when entries has one element
// or value is clamped to a boundary threshold.
[[nodiscard]] inline BlendTree1DResult ResolveBlendTree1D(
    std::vector<BlendTree1DEntry> entries, const float value)
{
    if (entries.empty())
    {
        return {};
    }
    std::sort(entries.begin(), entries.end(),
        [](const BlendTree1DEntry& a, const BlendTree1DEntry& b) {
            return a.Threshold < b.Threshold;
        });
    if (entries.size() == 1)
    {
        return {entries[0].ClipName, {}, 0.0F};
    }
    const float clamped =
        std::clamp(value, entries.front().Threshold, entries.back().Threshold);

    // Check if clamped value is exactly at a threshold (or very close)
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        if (std::abs(clamped - entries[i].Threshold) < 1.0e-6F)
        {
            return {entries[i].ClipName, {}, 0.0F};
        }
    }

    // Find the blend between two thresholds
    for (std::size_t i = 0; i + 1 < entries.size(); ++i)
    {
        const float lo = entries[i].Threshold;
        const float hi = entries[i + 1].Threshold;
        if (clamped > lo && clamped < hi)
        {
            const float span = hi - lo;
            const float weight = (clamped - lo) / span;
            return {entries[i].ClipName, entries[i + 1].ClipName, weight};
        }
    }
    return {entries.back().ClipName, {}, 0.0F};
}

template <typename Animator>
inline void QueueClipEvents(Animator& animator, const AnimationClipAsset& clip,
    const float previousTime, const float deltaTime)
{
    // Destination notifies remain deterministic during a crossfade. Source-clip
    // notify weighting belongs with the later layered animator/state-machine stage.
    ForEachAnimationEventCrossed(clip, previousTime, deltaTime, animator.Loop,
        animator.EmitStartEvents,
        [&](const AnimationEvent& event) { animator.PendingEvents.push_back(event); });
    animator.EmitStartEvents = false;
}

[[nodiscard]] inline const AnimatorState* FindAnimatorState(
    const AnimatorController& controller, const std::string_view name)
{
    const auto found = std::find_if(controller.States.begin(), controller.States.end(),
        [&](const AnimatorState& state) { return state.Name == name; });
    return found == controller.States.end() ? nullptr : &*found;
}

template <typename Animator>
[[nodiscard]] inline bool StartAnimatorController(Animator& animator)
{
    const AnimatorState* entry = FindAnimatorState(animator.Controller,
        animator.Controller.EntryState.empty() && !animator.Controller.States.empty()
            ? animator.Controller.States.front().Name
            : animator.Controller.EntryState);
    if (entry == nullptr || (!entry->UseBlendTree && entry->ClipName.empty()))
    {
        return false;
    }
    animator.ActiveState = entry->Name;
    animator.ClipName = entry->ClipName;
    animator.CurrentTime = 0.0F;
    animator.Playing = true;
    animator.Paused = false;
    animator.ClearBlend();
    animator.ClearEventState();
    animator.EmitStartEvents = true;
    return true;
}

template <typename Animator>
[[nodiscard]] inline bool SetAnimatorBool(Animator& animator, const std::string& name,
    const bool value)
{
    AnimatorParameter* parameter = FindAnimatorParameter(animator.Controller, name);
    if (parameter == nullptr || (parameter->Type != AnimatorParameterType::Bool &&
            parameter->Type != AnimatorParameterType::Trigger))
    {
        return false;
    }
    parameter->BoolValue = value;
    return true;
}

template <typename Animator>
[[nodiscard]] inline bool SetAnimatorFloat(Animator& animator, const std::string& name,
    const float value)
{
    AnimatorParameter* parameter = FindAnimatorParameter(animator.Controller, name);
    if (parameter == nullptr || parameter->Type != AnimatorParameterType::Float)
    {
        return false;
    }
    parameter->FloatValue = value;
    return true;
}

template <typename Animator>
[[nodiscard]] inline bool SetAnimatorInt(Animator& animator, const std::string& name,
    const int value)
{
    AnimatorParameter* parameter = FindAnimatorParameter(animator.Controller, name);
    if (parameter == nullptr || parameter->Type != AnimatorParameterType::Int)
    {
        return false;
    }
    parameter->IntValue = value;
    return true;
}

template <typename Animator>
[[nodiscard]] inline const AnimatorTransition* FindAnimatorTransition(
    const Animator& animator, const float clipDuration, const float lookAheadSeconds = 0.0F)
{
    if (animator.ActiveState.empty())
    {
        return nullptr;
    }
    // First pass: transitions from the active state.
    for (const AnimatorTransition& transition : animator.Controller.Transitions)
    {
        if (transition.From != animator.ActiveState || transition.To.empty())
        {
            continue;
        }
        if (transition.HasExitTime)
        {
            const float normalized = clipDuration > 1.0e-5F
                ? (animator.CurrentTime + std::max(lookAheadSeconds, 0.0F)) / clipDuration
                : 1.0F;
            if (normalized < transition.ExitTime)
            {
                continue;
            }
        }
        if (!std::all_of(transition.Conditions.begin(), transition.Conditions.end(),
                [&](const AnimatorCondition& condition) {
                    return AnimatorConditionPasses(animator.Controller, condition);
                }))
        {
            continue;
        }
        return &transition;
    }
    // Second pass: Any-State transitions. Skip if destination == current state.
    for (const AnimatorTransition& transition : animator.Controller.Transitions)
    {
        if (transition.From != "Any State" || transition.To.empty() ||
            transition.To == animator.ActiveState)
        {
            continue;
        }
        if (!std::all_of(transition.Conditions.begin(), transition.Conditions.end(),
                [&](const AnimatorCondition& condition) {
                    return AnimatorConditionPasses(animator.Controller, condition);
                }))
        {
            continue;
        }
        return &transition;
    }
    return nullptr;
}

template <typename Animator>
inline void BeginAnimatorTransition(Animator& animator, const AnimatorTransition& transition,
    const AnimatorState& destination)
{
    const std::string fromClip = animator.ClipName;
    const float fromTime = animator.CurrentTime;
    animator.ActiveState = destination.Name;
    if (destination.UseBlendTree && !destination.BlendEntries.empty())
    {
        // Primary clip = lowest-threshold entry; used for crossfade timing only.
        const auto primaryIt = std::min_element(destination.BlendEntries.begin(),
            destination.BlendEntries.end(),
            [](const BlendTree1DEntry& a, const BlendTree1DEntry& b) {
                return a.Threshold < b.Threshold;
            });
        animator.ClipName = primaryIt->ClipName;
    }
    else
    {
        animator.ClipName = destination.ClipName;
    }
    animator.CurrentTime = 0.0F;
    animator.ClearBlend();
    animator.ClearEventState();
    animator.EmitStartEvents = true;
    if (transition.Duration > 0.0F && !fromClip.empty())
    {
        animator.BlendFromClipName = fromClip;
        animator.BlendFromTime = fromTime;
        animator.BlendDuration = transition.Duration;
    }
    for (const AnimatorCondition& condition : transition.Conditions)
    {
        AnimatorParameter* parameter = FindAnimatorParameter(animator.Controller, condition.Parameter);
        if (parameter != nullptr && parameter->Type == AnimatorParameterType::Trigger)
        {
            parameter->BoolValue = false;
        }
    }
}

// --- Transform animation (any entity, no skinned mesh required) --------------
// Reuses AnimationChannel: JointIndex is ignored, Target picks which part of the
// entity's TransformComponent the channel drives. Kept header-only so the editor
// viewport, fadix_player and the smoke test all share one implementation.

[[nodiscard]] inline AnimationClipAsset* FindTransformClip(
    TransformAnimatorComponent& animator, const std::string_view requestedName = {})
{
    if (animator.Clips.empty())
    {
        return nullptr;
    }
    const std::string_view name = requestedName.empty()
        ? std::string_view{animator.ClipName}
        : requestedName;
    if (!name.empty())
    {
        for (AnimationClipAsset& clip : animator.Clips)
        {
            if (clip.Name == name)
            {
                animator.ClipName = clip.Name;
                return &clip;
            }
        }
        if (!requestedName.empty())
        {
            return nullptr;
        }
    }
    animator.ClipName = animator.Clips.front().Name;
    return &animator.Clips.front();
}

[[nodiscard]] inline const AnimationClipAsset* FindTransformClip(
    const TransformAnimatorComponent& animator, const std::string_view requestedName = {})
{
    if (animator.Clips.empty())
    {
        return nullptr;
    }
    const std::string_view name = requestedName.empty()
        ? std::string_view{animator.ClipName}
        : requestedName;
    for (const AnimationClipAsset& clip : animator.Clips)
    {
        if (!name.empty() && clip.Name == name)
        {
            return &clip;
        }
    }
    return requestedName.empty() ? &animator.Clips.front() : nullptr;
}

[[nodiscard]] inline AnimationClipAsset& EnsureTransformClip(
    TransformAnimatorComponent& animator, std::string name = "Transform")
{
    if (AnimationClipAsset* clip = FindTransformClip(animator))
    {
        return *clip;
    }
    if (name.empty())
    {
        name = "Transform";
    }
    AnimationClipAsset clip;
    clip.Name = std::move(name);
    animator.Clips.push_back(std::move(clip));
    animator.ClipName = animator.Clips.back().Name;
    return animator.Clips.back();
}

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
    AnimationClipAsset& clip = EnsureTransformClip(anim);
    AnimationChannel& channel = FindOrCreateTransformChannel(clip, property);
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
    clip.Duration = std::max(clip.Duration, time);
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

inline void ApplyTransformBlendTree1D(const TransformAnimatorComponent& anim,
    const BlendTree1DResult& blend, const float time, TransformComponent& transform)
{
    TransformComponent poseA = transform;
    const AnimationClipAsset* clipA = FindTransformClip(anim, blend.ClipA);
    if (clipA != nullptr)
    {
        ApplyTransformClip(*clipA, time, poseA);
    }
    if (blend.ClipB.empty() || blend.Weight <= 1.0e-6F)
    {
        transform = poseA;
        return;
    }
    TransformComponent poseB = transform;
    const AnimationClipAsset* clipB = FindTransformClip(anim, blend.ClipB);
    if (clipB != nullptr)
    {
        ApplyTransformClip(*clipB, time, poseB);
    }
    BlendTransformPoses(poseA, poseB, blend.Weight, transform);
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
        AnimationClipAsset* clip = FindTransformClip(anim);
        if (!anim.Playing)
        {
            continue;
        }
        if (clip != nullptr)
        {
            if (const AnimatorTransition* transition = FindAnimatorTransition(
                    anim, clip->Duration, std::abs(dt * anim.Speed)))
            {
                const AnimatorState* destination =
                    FindAnimatorState(anim.Controller, transition->To);
                const AnimationClipAsset* target = nullptr;
                if (destination != nullptr)
                {
                    const std::string_view targetClip =
                        destination->UseBlendTree && !destination->BlendEntries.empty()
                        ? std::string_view{std::min_element(destination->BlendEntries.begin(),
                              destination->BlendEntries.end(),
                              [](const BlendTree1DEntry& a, const BlendTree1DEntry& b) {
                                  return a.Threshold < b.Threshold; })->ClipName}
                        : std::string_view{destination->ClipName};
                    target = FindTransformClip(
                        static_cast<const TransformAnimatorComponent&>(anim), targetClip);
                }
                if (destination != nullptr && target != nullptr)
                {
                    BeginAnimatorTransition(anim, *transition, *destination);
                    clip = FindTransformClip(anim);
                }
            }
        }
        if (clip == nullptr)
        {
            continue;
        }
        const float previousTime = anim.CurrentTime;
        QueueClipEvents(anim, *clip, previousTime, dt * anim.Speed);
        anim.CurrentTime = AdvanceClipTime(anim.CurrentTime, *clip, dt, anim.Speed, anim.Loop);
        const AnimationClipAsset* fromClip = anim.BlendFromClipName.empty()
            ? nullptr
            : FindTransformClip(
                  static_cast<const TransformAnimatorComponent&>(anim), anim.BlendFromClipName);
        if (fromClip != nullptr && anim.BlendDuration > 0.0F)
        {
            anim.BlendFromTime =
                AdvanceClipTime(anim.BlendFromTime, *fromClip, dt, anim.Speed, anim.Loop);
            TransformComponent fromPose = transform;
            TransformComponent toPose = transform;
            ApplyTransformClip(*fromClip, anim.BlendFromTime, fromPose);
            ApplyTransformClip(*clip, anim.CurrentTime, toPose);
            anim.BlendElapsed += std::abs(dt);
            const float weight = anim.BlendElapsed / anim.BlendDuration;
            BlendTransformPoses(fromPose, toPose, weight, transform);
            if (weight >= 1.0F)
            {
                ClearAnimationBlend(anim);
            }
        }
        else
        {
            ClearAnimationBlend(anim);
            // Check if active state is a blend tree
            const AnimatorState* activeState =
                FindAnimatorState(anim.Controller, anim.ActiveState);
            if (activeState != nullptr && activeState->UseBlendTree &&
                !activeState->BlendEntries.empty())
            {
                const AnimatorParameter* param = FindAnimatorParameter(
                    anim.Controller, activeState->BlendParameter);
                const BlendTree1DResult blend = ResolveBlendTree1D(activeState->BlendEntries,
                    param != nullptr ? param->FloatValue : 0.0F);
                ApplyTransformBlendTree1D(anim, blend, anim.CurrentTime, transform);
            }
            else
            {
                ApplyTransformClip(*clip, anim.CurrentTime, transform);
            }
        }
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

// Sample a 1D blend-tree pose from a GltfMeshAsset.
// Both clips sample at the same time (synchronized playback).
// Returns the blended SkeletonPose; pose.Skeleton is pre-set to gltf.Skeleton.
[[nodiscard]] inline SkeletonPose SampleSkeletalBlendTree1D(const GltfMeshAsset& gltf,
    const BlendTree1DResult& blend, const float time, const float dt,
    const float speed, const bool loop)
{
    SkeletonPose poseA;
    poseA.Skeleton = gltf.Skeleton;
    const AnimationClipAsset* clipA = FindAnimationClip(gltf, blend.ClipA);
    if (clipA != nullptr)
    {
        AnimationPlayer player;
        player.SetClip(clipA);
        player.SetTime(time);
        player.Update(dt, speed, loop, poseA);
    }
    if (blend.ClipB.empty() || blend.Weight <= 1.0e-6F)
    {
        return poseA;
    }
    SkeletonPose poseB;
    poseB.Skeleton = gltf.Skeleton;
    const AnimationClipAsset* clipB = FindAnimationClip(gltf, blend.ClipB);
    if (clipB != nullptr)
    {
        AnimationPlayer player;
        player.SetClip(clipB);
        player.SetTime(time);
        player.Update(dt, speed, loop, poseB);
    }
    SkeletonPose result;
    result.Skeleton = gltf.Skeleton;
    BlendSkeletonPoses(poseA, poseB, blend.Weight, result);
    return result;
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
        if (!animator->Playing)
        {
            continue;
        }
        // --- clip / blend-tree resolution ---
        const AnimatorState* activeState = FindAnimatorState(
            animator->Controller, animator->ActiveState);
        const bool usingBlendTree = activeState != nullptr &&
            activeState->UseBlendTree && !activeState->BlendEntries.empty();
        BlendTree1DResult blend;
        const AnimationClipAsset* clip = nullptr;
        if (usingBlendTree)
        {
            const AnimatorParameter* param = FindAnimatorParameter(
                animator->Controller, activeState->BlendParameter);
            blend = ResolveBlendTree1D(activeState->BlendEntries,
                param != nullptr ? param->FloatValue : 0.0F);
            clip = FindAnimationClip(*gltf, blend.ClipA);
        }
        else
        {
            clip = FindAnimationClip(*gltf, animator->ClipName);
        }
        if (clip == nullptr)
        {
            continue;
        }

        // --- transition check ---
        if (const AnimatorTransition* transition = FindAnimatorTransition(
                *animator, clip->Duration, std::abs(dt * animator->Speed)))
        {
            const AnimatorState* destination =
                FindAnimatorState(animator->Controller, transition->To);
            const AnimationClipAsset* target = destination == nullptr
                ? nullptr
                : FindAnimationClip(*gltf, destination->UseBlendTree &&
                        !destination->BlendEntries.empty()
                    ? std::min_element(destination->BlendEntries.begin(),
                          destination->BlendEntries.end(),
                          [](const BlendTree1DEntry& a, const BlendTree1DEntry& b) {
                              return a.Threshold < b.Threshold; })->ClipName
                    : destination->ClipName);
            if (destination != nullptr && target != nullptr)
            {
                BeginAnimatorTransition(*animator, *transition, *destination);
                // Re-resolve after transition
                activeState = FindAnimatorState(
                    animator->Controller, animator->ActiveState);
                const bool nowBlendTree = activeState != nullptr &&
                    activeState->UseBlendTree && !activeState->BlendEntries.empty();
                if (nowBlendTree)
                {
                    const AnimatorParameter* param = FindAnimatorParameter(
                        animator->Controller, activeState->BlendParameter);
                    blend = ResolveBlendTree1D(activeState->BlendEntries,
                        param != nullptr ? param->FloatValue : 0.0F);
                    clip = FindAnimationClip(*gltf, blend.ClipA);
                }
                else
                {
                    blend = {};
                    clip = target;
                }
                if (clip == nullptr) { continue; }
            }
        }

        const float previousTime = animator->CurrentTime;
        QueueClipEvents(*animator, *clip, previousTime, dt * animator->Speed);

        // --- pose sampling ---
        SkeletonPose targetPose;
        if (usingBlendTree || !blend.ClipA.empty())
        {
            targetPose = SampleSkeletalBlendTree1D(
                *gltf, blend, animator->CurrentTime, dt, animator->Speed, animator->Loop);
            animator->CurrentTime = AdvanceClipTime(
                animator->CurrentTime, *clip, dt, animator->Speed, animator->Loop);
        }
        else
        {
            targetPose.Skeleton = gltf->Skeleton;
            AnimationPlayer targetPlayer;
            targetPlayer.SetClip(clip);
            targetPlayer.SetTime(animator->CurrentTime);
            targetPlayer.Update(dt, animator->Speed, animator->Loop, targetPose);
            animator->CurrentTime = targetPlayer.GetTime();
        }

        SkeletonPose pose = targetPose;
        const AnimationClipAsset* fromClip = animator->BlendFromClipName.empty()
            ? nullptr
            : FindAnimationClip(*gltf, animator->BlendFromClipName);
        if (fromClip != nullptr && animator->BlendDuration > 0.0F)
        {
            SkeletonPose fromPose;
            fromPose.Skeleton = gltf->Skeleton;
            AnimationPlayer fromPlayer;
            fromPlayer.SetClip(fromClip);
            fromPlayer.SetTime(animator->BlendFromTime);
            fromPlayer.Update(dt, animator->Speed, animator->Loop, fromPose);
            animator->BlendFromTime = fromPlayer.GetTime();
            animator->BlendElapsed += std::abs(dt);
            const float weight = animator->BlendElapsed / animator->BlendDuration;
            BlendSkeletonPoses(fromPose, targetPose, weight, pose);
            if (weight >= 1.0F)
            {
                ClearAnimationBlend(*animator);
            }
        }
        else
        {
            ClearAnimationBlend(*animator);
        }

        skeleton->HasSkeleton = true;
        skeleton->JointCount = static_cast<int>(pose.Skeleton.Joints.size());
        skeleton->JointMatrices = pose.Skeleton.GetJointMatrices();
    }
}
}
