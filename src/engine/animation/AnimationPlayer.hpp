#pragma once

#include "engine/animation/AnimationClip.hpp"
#include "engine/animation/Skeleton.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fadix
{
class AnimationPlayer
{
public:
    void SetClip(const AnimationClipAsset* clip)
    {
        m_Clip = clip;
    }

    void SetTime(const float time)
    {
        m_Time = time;
    }

    [[nodiscard]] float GetTime() const noexcept
    {
        return m_Time;
    }

    void Update(const float deltaTime, const float speed, const bool loop, SkeletonPose& pose)
    {
        if (m_Clip == nullptr || m_Clip->Channels.empty() || pose.Skeleton.Joints.empty())
        {
            return;
        }

        m_Time += deltaTime * speed;
        if (m_Clip->Duration > 0.0F)
        {
            if (loop)
            {
                m_Time = std::fmod(m_Time, m_Clip->Duration);
                if (m_Time < 0.0F)
                {
                    m_Time += m_Clip->Duration;
                }
            }
            else
            {
                m_Time = std::clamp(m_Time, 0.0F, m_Clip->Duration);
            }
        }

        pose.Skeleton.ResetToRestPose();

        struct LocalTrs
        {
            glm::vec3 Translation{0.0F};
            glm::quat Rotation{1.0F, 0.0F, 0.0F, 0.0F};
            glm::vec3 Scale{1.0F};
            bool HasTranslation{false};
            bool HasRotation{false};
            bool HasScale{false};
        };
        std::vector<LocalTrs> locals(pose.Skeleton.Joints.size());
        for (std::size_t i = 0; i < pose.Skeleton.Joints.size(); ++i)
        {
            locals[i].Translation = pose.Skeleton.Joints[i].RestTranslation;
            locals[i].Rotation = pose.Skeleton.Joints[i].RestRotation;
            locals[i].Scale = pose.Skeleton.Joints[i].RestScale;
        }

        for (const AnimationChannel& channel : m_Clip->Channels)
        {
            if (channel.JointIndex < 0 ||
                channel.JointIndex >= static_cast<int>(locals.size()))
            {
                continue;
            }
            LocalTrs& local = locals[static_cast<std::size_t>(channel.JointIndex)];
            glm::vec3 translation = local.Translation;
            glm::quat rotation = local.Rotation;
            glm::vec3 scale = local.Scale;
            channel.Sample(m_Time, translation, rotation, scale);
            if (channel.Target == AnimationChannel::Property::Translation)
            {
                local.Translation = translation;
                local.HasTranslation = true;
            }
            else if (channel.Target == AnimationChannel::Property::Rotation)
            {
                local.Rotation = rotation;
                local.HasRotation = true;
            }
            else
            {
                local.Scale = scale;
                local.HasScale = true;
            }
        }

        for (std::size_t i = 0; i < pose.Skeleton.Joints.size(); ++i)
        {
            const LocalTrs& local = locals[i];
            pose.Skeleton.Joints[i].LocalTransform =
                glm::translate(glm::mat4{1.0F}, local.Translation) * glm::mat4_cast(local.Rotation) *
                glm::scale(glm::mat4{1.0F}, local.Scale);
        }
        pose.ComputePose();
    }

private:
    const AnimationClipAsset* m_Clip{nullptr};
    float m_Time{0.0F};
};
}
