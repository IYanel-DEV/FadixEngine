#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/common.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
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

struct AnimationChannel
{
    enum class Property : std::uint8_t
    {
        Translation,
        Rotation,
        Scale
    };

    int JointIndex{-1};
    Property Target{Property::Translation};
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
            const float alpha = span > 0.0F ? (t - Keyframes[i].Time) / span : 0.0F;
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
};
}
