#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/common.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace fadix
{
struct AnimationKeyframe
{
    float Time{0.0F};
    glm::vec4 Value{0.0F};
};

struct AnimationEvent
{
    float Time{0.0F};
    std::string Name{"Event"};
    std::string Payload;
};

struct AnimationChannel
{
    enum class Property : std::uint8_t
    {
        Translation,
        Rotation,
        Scale
    };

    enum class Interpolation : std::uint8_t
    {
        Step,
        Linear,
        Smooth
    };

    int JointIndex{-1};
    Property Target{Property::Translation};
    Interpolation InterpolationMode{Interpolation::Linear};
    std::vector<AnimationKeyframe> Keyframes;

    void Sample(const float time, glm::vec3& translation, glm::quat& rotation, glm::vec3& scale) const
    {
        if (Keyframes.empty())
        {
            return;
        }
        const float t = std::clamp(time, Keyframes.front().Time, Keyframes.back().Time);
        for (std::size_t i = 0; i + 1 < Keyframes.size(); ++i)
        {
            if (t < Keyframes[i].Time || t > Keyframes[i + 1].Time)
            {
                continue;
            }
            const float span = Keyframes[i + 1].Time - Keyframes[i].Time;
            float alpha = span > 0.0F ? (t - Keyframes[i].Time) / span : 0.0F;
            if (InterpolationMode == Interpolation::Step)
            {
                alpha = 0.0F;
            }
            else if (InterpolationMode == Interpolation::Smooth)
            {
                alpha = alpha * alpha * (3.0F - 2.0F * alpha);
            }
            if (Target == Property::Translation)
            {
                translation = glm::mix(
                    glm::vec3{Keyframes[i].Value}, glm::vec3{Keyframes[i + 1].Value}, alpha);
            }
            else if (Target == Property::Rotation)
            {
                const glm::quat a{
                    Keyframes[i].Value.w, Keyframes[i].Value.x, Keyframes[i].Value.y,
                    Keyframes[i].Value.z};
                const glm::quat b{
                    Keyframes[i + 1].Value.w, Keyframes[i + 1].Value.x, Keyframes[i + 1].Value.y,
                    Keyframes[i + 1].Value.z};
                rotation = glm::normalize(glm::slerp(a, b, alpha));
            }
            else
            {
                scale = glm::mix(
                    glm::vec3{Keyframes[i].Value}, glm::vec3{Keyframes[i + 1].Value}, alpha);
            }
            return;
        }
        if (Target == Property::Translation)
        {
            translation = glm::vec3{Keyframes.back().Value};
        }
        else if (Target == Property::Rotation)
        {
            rotation = glm::normalize(glm::quat{
                Keyframes.back().Value.w, Keyframes.back().Value.x, Keyframes.back().Value.y,
                Keyframes.back().Value.z});
        }
        else
        {
            scale = glm::vec3{Keyframes.back().Value};
        }
    }
};

struct AnimationClipAsset
{
    std::string Name;
    float Duration{0.0F};
    std::vector<AnimationChannel> Channels;
    std::vector<AnimationEvent> Events;
};

enum class AnimatorParameterType : std::uint8_t
{
    Bool,
    Float,
    Int,
    Trigger
};

struct AnimatorParameter
{
    std::string Name{"Parameter"};
    AnimatorParameterType Type{AnimatorParameterType::Bool};
    bool BoolValue{false};
    float FloatValue{0.0F};
    int IntValue{0};
};

enum class AnimatorComparison : std::uint8_t
{
    Equal,
    NotEqual,
    Greater,
    Less
};

struct AnimatorCondition
{
    std::string Parameter;
    AnimatorComparison Comparison{AnimatorComparison::Equal};
    float Threshold{1.0F};
};

struct AnimatorState
{
    std::string Name{"State"};
    std::string ClipName;
    glm::vec2 Position{40.0F, 40.0F};
};

struct AnimatorTransition
{
    std::string From;
    std::string To;
    float Duration{0.25F};
    bool HasExitTime{false};
    float ExitTime{1.0F};
    std::vector<AnimatorCondition> Conditions;
};

struct AnimatorController
{
    std::string Name{"Animator"};
    std::string EntryState;
    std::vector<AnimatorParameter> Parameters;
    std::vector<AnimatorState> States;
    std::vector<AnimatorTransition> Transitions;
};

[[nodiscard]] inline AnimatorParameter* FindAnimatorParameter(
    AnimatorController& controller, const std::string& name)
{
    const auto found = std::find_if(controller.Parameters.begin(), controller.Parameters.end(),
        [&](const AnimatorParameter& parameter) { return parameter.Name == name; });
    return found == controller.Parameters.end() ? nullptr : &*found;
}

[[nodiscard]] inline const AnimatorParameter* FindAnimatorParameter(
    const AnimatorController& controller, const std::string& name)
{
    const auto found = std::find_if(controller.Parameters.begin(), controller.Parameters.end(),
        [&](const AnimatorParameter& parameter) { return parameter.Name == name; });
    return found == controller.Parameters.end() ? nullptr : &*found;
}

[[nodiscard]] inline bool AnimatorConditionPasses(
    const AnimatorController& controller, const AnimatorCondition& condition)
{
    const AnimatorParameter* parameter = FindAnimatorParameter(controller, condition.Parameter);
    if (parameter == nullptr)
    {
        return false;
    }
    const float value = parameter->Type == AnimatorParameterType::Float ? parameter->FloatValue
        : parameter->Type == AnimatorParameterType::Int                 ? static_cast<float>(parameter->IntValue)
                                                                        : parameter->BoolValue ? 1.0F : 0.0F;
    switch (condition.Comparison)
    {
    case AnimatorComparison::Equal: return std::abs(value - condition.Threshold) <= 1.0e-5F;
    case AnimatorComparison::NotEqual: return std::abs(value - condition.Threshold) > 1.0e-5F;
    case AnimatorComparison::Greater: return value > condition.Threshold;
    case AnimatorComparison::Less: return value < condition.Threshold;
    }
    return false;
}

// Visit destination-clip events crossed by one playback step, in playback order.
// The interval excludes its starting time unless includeStart is requested, which
// makes events at t=0 fire when play/crossfade starts without double-firing later.
template <typename Emit>
void ForEachAnimationEventCrossed(const AnimationClipAsset& clip, float startTime,
    const float deltaTime, const bool loop, const bool includeStart, Emit&& emit)
{
    if (clip.Events.empty())
    {
        return;
    }
    constexpr float epsilon = 1.0e-5F;
    if (loop && clip.Duration > epsilon)
    {
        startTime = std::fmod(startTime, clip.Duration);
        if (startTime < 0.0F)
        {
            startTime += clip.Duration;
        }
    }
    if (includeStart)
    {
        for (const AnimationEvent& event : clip.Events)
        {
            if (std::abs(event.Time - startTime) <= epsilon)
            {
                emit(event);
            }
        }
    }
    if (std::abs(deltaTime) <= epsilon)
    {
        return;
    }

    struct Occurrence
    {
        double Position;
        const AnimationEvent* Event;
    };
    std::vector<Occurrence> crossed;
    // ponytail: protects a frame from pathological script speeds. A future event
    // bus can retain overflow if games genuinely need >1024 notifies in one tick.
    constexpr std::size_t maxEventsPerTick = 1024;
    const double start = static_cast<double>(startTime);
    double end = start + static_cast<double>(deltaTime);
    if (!loop || clip.Duration <= epsilon)
    {
        end = std::clamp(end, 0.0, static_cast<double>(clip.Duration));
        for (const AnimationEvent& event : clip.Events)
        {
            const double time = static_cast<double>(event.Time);
            const bool hit = deltaTime > 0.0F
                ? time > start + epsilon && time <= end + epsilon
                : time < start - epsilon && time >= end - epsilon;
            if (hit)
            {
                crossed.push_back({time, &event});
            }
        }
    }
    else
    {
        const double duration = static_cast<double>(clip.Duration);
        for (const AnimationEvent& event : clip.Events)
        {
            const double eventTime =
                std::clamp(static_cast<double>(event.Time), 0.0, duration);
            if (deltaTime > 0.0F)
            {
                const long long first =
                    static_cast<long long>(std::floor((start - eventTime) / duration)) + 1;
                const long long last = static_cast<long long>(
                    std::floor((end - eventTime + epsilon) / duration));
                for (long long cycle = first;
                     cycle <= last && crossed.size() < maxEventsPerTick; ++cycle)
                {
                    crossed.push_back({eventTime + static_cast<double>(cycle) * duration, &event});
                }
            }
            else
            {
                const long long first = static_cast<long long>(
                    std::ceil((end - eventTime - epsilon) / duration));
                const long long last =
                    static_cast<long long>(std::ceil((start - eventTime) / duration)) - 1;
                for (long long cycle = first;
                     cycle <= last && crossed.size() < maxEventsPerTick; ++cycle)
                {
                    crossed.push_back({eventTime + static_cast<double>(cycle) * duration, &event});
                }
            }
            if (crossed.size() >= maxEventsPerTick)
            {
                break;
            }
        }
    }
    std::sort(crossed.begin(), crossed.end(), [deltaTime](const Occurrence& a, const Occurrence& b) {
        return deltaTime > 0.0F ? a.Position < b.Position : a.Position > b.Position;
    });
    for (const Occurrence& occurrence : crossed)
    {
        emit(*occurrence.Event);
    }
}
}
