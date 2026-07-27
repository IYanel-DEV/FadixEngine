#pragma once

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace fadix
{
inline constexpr int kMaxSkinJoints = 128;

struct Joint
{
    int Parent{-1};
    std::string Name;
    glm::mat4 InverseBindMatrix{1.0F};
    glm::vec3 RestTranslation{0.0F};
    glm::quat RestRotation{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 RestScale{1.0F};
    glm::mat4 LocalTransform{1.0F};
    glm::mat4 GlobalTransform{1.0F};
};

struct SkeletonAsset
{
    std::vector<Joint> Joints;

    void ResetToRestPose()
    {
        for (Joint& joint : Joints)
        {
            joint.LocalTransform = glm::translate(glm::mat4{1.0F}, joint.RestTranslation) *
                glm::mat4_cast(joint.RestRotation) * glm::scale(glm::mat4{1.0F}, joint.RestScale);
        }
    }

    void ComputeGlobalTransforms()
    {
        for (Joint& joint : Joints)
        {
            if (joint.Parent >= 0 && joint.Parent < static_cast<int>(Joints.size()))
            {
                joint.GlobalTransform = Joints[static_cast<std::size_t>(joint.Parent)].GlobalTransform *
                    joint.LocalTransform;
            }
            else
            {
                joint.GlobalTransform = joint.LocalTransform;
            }
        }
    }

    [[nodiscard]] std::vector<glm::mat4> GetJointMatrices() const
    {
        std::vector<glm::mat4> result(Joints.size(), glm::mat4{1.0F});
        for (std::size_t i = 0; i < Joints.size(); ++i)
        {
            result[i] = Joints[i].GlobalTransform * Joints[i].InverseBindMatrix;
        }
        return result;
    }
};

struct SkeletonPose
{
    SkeletonAsset Skeleton;

    void ComputePose()
    {
        Skeleton.ComputeGlobalTransforms();
    }
};
}
