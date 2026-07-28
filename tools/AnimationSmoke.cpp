// FDX Animation self-check: proves the AnimationPlayer advances time and that a
// joint's skinning matrix actually changes as a clip plays (not frozen bind pose).
#include "engine/animation/AnimationClip.hpp"
#include "engine/animation/AnimationPlayer.hpp"
#include "engine/animation/Skeleton.hpp"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>

using namespace fadix;

namespace
{
int g_fail = 0;

void Check(const bool cond, const char* msg)
{
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond)
    {
        ++g_fail;
    }
}

bool MatricesDiffer(const glm::mat4& a, const glm::mat4& b, const float eps)
{
    for (int c = 0; c < 4; ++c)
    {
        for (int r = 0; r < 4; ++r)
        {
            if (std::abs(a[c][r] - b[c][r]) > eps)
            {
                return true;
            }
        }
    }
    return false;
}
}

int main()
{
    SkeletonPose pose;
    pose.Skeleton.Joints.resize(1);
    Joint& joint = pose.Skeleton.Joints[0];
    joint.Parent = -1;
    joint.RestTranslation = glm::vec3{0.0F};
    joint.RestRotation = glm::quat{1.0F, 0.0F, 0.0F, 0.0F};
    joint.RestScale = glm::vec3{1.0F};
    pose.Skeleton.ResetToRestPose();

    const glm::quat q0{1.0F, 0.0F, 0.0F, 0.0F};
    const glm::quat q1 = glm::angleAxis(glm::radians(90.0F), glm::vec3{0.0F, 1.0F, 0.0F});

    AnimationClipAsset clip;
    clip.Name = "spin";
    clip.Duration = 1.0F;
    AnimationChannel channel;
    channel.JointIndex = 0;
    channel.Target = AnimationChannel::Property::Rotation;
    channel.Keyframes.push_back({0.0F, glm::vec4{q0.x, q0.y, q0.z, q0.w}});
    channel.Keyframes.push_back({1.0F, glm::vec4{q1.x, q1.y, q1.z, q1.w}});
    clip.Channels.push_back(channel);

    AnimationPlayer player;
    player.SetClip(&clip);
    player.SetTime(0.0F);
    player.Update(0.0F, 1.0F, false, pose);
    const glm::mat4 atStart = pose.Skeleton.GetJointMatrices()[0];

    player.Update(0.5F, 1.0F, false, pose);
    Check(std::abs(player.GetTime() - 0.5F) < 1e-4F, "player time advanced to 0.5s");
    const glm::mat4 atMid = pose.Skeleton.GetJointMatrices()[0];
    Check(MatricesDiffer(atStart, atMid, 1e-4F), "joint matrix changed after advancing time");

    player.SetTime(1.0F);
    player.Update(0.0F, 1.0F, false, pose);
    const glm::mat4 atEnd = pose.Skeleton.GetJointMatrices()[0];
    const glm::mat4 expected = glm::mat4_cast(glm::normalize(q1));
    Check(!MatricesDiffer(atEnd, expected, 1e-3F), "end pose matches final keyframe rotation");

    // Looping wraps time back into range.
    player.SetTime(0.0F);
    player.Update(1.5F, 1.0F, true, pose);
    Check(player.GetTime() < 1.0F, "looping clip wraps time within duration");

    // Transform-animation core (P1): a Translation channel with JointIndex ignored
    // must interpolate the same way ApplyTransformClip reads it for a bare entity.
    {
        AnimationChannel move;
        move.JointIndex = -1; // entity transform, not a joint
        move.Target = AnimationChannel::Property::Translation;
        move.Keyframes.push_back({0.0F, glm::vec4{0.0F, 0.0F, 0.0F, 0.0F}});
        move.Keyframes.push_back({2.0F, glm::vec4{10.0F, 0.0F, 0.0F, 0.0F}});

        glm::vec3 t{0.0F};
        glm::quat r{1.0F, 0.0F, 0.0F, 0.0F};
        glm::vec3 s{1.0F};
        move.Sample(0.0F, t, r, s);
        Check(std::abs(t.x - 0.0F) < 1e-4F, "transform translation at t=0 is start key");
        move.Sample(1.0F, t, r, s);
        Check(std::abs(t.x - 5.0F) < 1e-4F, "transform translation at t=1 interpolates to midpoint");
        move.Sample(2.0F, t, r, s);
        Check(std::abs(t.x - 10.0F) < 1e-4F, "transform translation at t=2 is end key");
    }

    std::printf(g_fail == 0 ? "ALL PASS\n" : "%d CHECK(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
