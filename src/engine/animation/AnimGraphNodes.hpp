#pragma once

#include "engine/animation/AnimationGraph.hpp"
#include "engine/animation/AnimationClip.hpp"
#include "engine/animation/AnimationPlayer.hpp"
#include "engine/animation/AnimatorControllerIO.hpp"

#include <algorithm>
#include <istream>
#include <iomanip>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// AnimatorController + runtime helpers are in AnimationRuntime.hpp.
// AnimGraphNodes.hpp is a pure-engine header — it includes AnimationPlayer.hpp
// (already engine-only) but NOT AnimationRuntime.hpp to avoid the GltfMeshCache
// circular dep. FindAnimationClip and ResolveBlendTree1D are re-declared here as
// forward-uses; callers must include AnimationRuntime.hpp before AnimGraphNodes.hpp
// in .cpp files. (Header-only nodes that need them carry the definition via the
// include chain through AnimationGraph.hpp -> AnimationPlayer.hpp.)
// NOTE: AnimGraphNodes.hpp MUST be included AFTER AnimationRuntime.hpp in any .cpp
// that uses StateMachineNode or BlendByFloatNode, so the helpers are visible.

namespace fadix
{
struct GltfMeshAsset;

// ---------------------------------------------------------------------------
// Helper — safe child evaluation (returns empty pose if index bad)
// ---------------------------------------------------------------------------
[[nodiscard]] inline SkeletonPose EvalChild(AnimGraphContext& ctx, int childIndex)
{
    if (childIndex < 0 || childIndex >= static_cast<int>(ctx.Graph->Nodes.size()))
        return {};
    if (std::find(ctx.EvaluationStack.begin(), ctx.EvaluationStack.end(), childIndex) !=
        ctx.EvaluationStack.end())
        return {};
    auto& node = ctx.Graph->Nodes[static_cast<std::size_t>(childIndex)];
    if (!node) return {};
    ctx.EvaluationStack.push_back(childIndex);
    SkeletonPose pose = node->Evaluate(ctx);
    ctx.EvaluationStack.pop_back();
    return pose;
}

// ---------------------------------------------------------------------------
// OutputNode — root, forwards to single child
// ---------------------------------------------------------------------------
struct OutputNode : AnimGraphNode
{
    int Child{-1};

    SkeletonPose Evaluate(AnimGraphContext& ctx) override { return EvalChild(ctx, Child); }
    std::string_view TypeName() const override { return "OutputNode"; }

    void Serialize(std::ostream& out) const override
    {
        out << "child " << Child << '\n';
    }
    void Deserialize(std::istream& in) override
    {
        std::string key; in >> key >> Child;
    }
};

// ---------------------------------------------------------------------------
// ClipNode — plays one AnimationClipAsset by name
// ---------------------------------------------------------------------------
struct ClipNode : AnimGraphNode
{
    std::string ClipName;
    float Speed{1.0F};
    bool Loop{true};
    bool Mirror{false};

    // Per-instance state (safe because AnimationGraph is owned per-entity).
    float CurrentTime{0.0F};

    SkeletonPose Evaluate(AnimGraphContext& ctx) override;

    std::string_view TypeName() const override { return "ClipNode"; }
    void ResetRuntime() noexcept override { CurrentTime = 0.0F; }
    void SetRuntimeTime(const float seconds) noexcept override
    {
        CurrentTime = std::max(seconds, 0.0F);
    }
    void Serialize(std::ostream& out) const override
    {
        out << "clip " << std::quoted(ClipName) << '\n'
            << "speed " << Speed << '\n'
            << "loop " << (Loop ? 1 : 0) << '\n'
            << "mirror " << (Mirror ? 1 : 0) << '\n';
    }
    void Deserialize(std::istream& in) override
    {
        std::string key; int b;
        in >> key >> std::quoted(ClipName);
        in >> key >> Speed;
        in >> key >> b; Loop = b != 0;
        in >> key >> b; Mirror = b != 0;
    }
};

// ClipNode::Evaluate defined after GltfMeshAsset is fully visible.
// Placed at bottom of this header after the forward-include chain resolves.

// ---------------------------------------------------------------------------
// BlendByFloatNode — 1D blend space driven by float parameter
// ---------------------------------------------------------------------------
struct BlendByFloatNode : AnimGraphNode
{
    std::string ParameterName;
    struct Entry { float Threshold{0.0F}; int ChildIndex{-1}; };
    std::vector<Entry> Entries;

    SkeletonPose Evaluate(AnimGraphContext& ctx) override;

    std::string_view TypeName() const override { return "BlendByFloatNode"; }
    void Serialize(std::ostream& out) const override
    {
        out << "param " << std::quoted(ParameterName) << '\n'
            << "entries " << Entries.size() << '\n';
        for (const Entry& e : Entries)
            out << e.Threshold << ' ' << e.ChildIndex << '\n';
    }
    void Deserialize(std::istream& in) override
    {
        std::string key; std::size_t n;
        in >> key >> std::quoted(ParameterName);
        in >> key >> n;
        Entries.resize(n);
        for (Entry& e : Entries) in >> e.Threshold >> e.ChildIndex;
    }
};

// ---------------------------------------------------------------------------
// BlendByConditionNode — crossfades between two children on bool/trigger
// ---------------------------------------------------------------------------
struct BlendByConditionNode : AnimGraphNode
{
    std::string ParameterName;
    int TrueChild{-1};
    int FalseChild{-1};
    float BlendDuration{0.15F};

    // Per-instance state
    float BlendElapsed{0.0F};
    bool ActiveValue{false};  // current target

    SkeletonPose Evaluate(AnimGraphContext& ctx) override
    {
        const bool newVal = ctx.GetBool(ParameterName);
        if (newVal != ActiveValue)
        {
            ActiveValue = newVal;
            BlendElapsed = 0.0F;
        }
        BlendElapsed = std::min(BlendElapsed + ctx.DeltaTime, BlendDuration);
        const float t = BlendDuration > 1.0e-5F ? BlendElapsed / BlendDuration : 1.0F;
        const float weight = ActiveValue ? t : 1.0F - t;

        SkeletonPose trueP  = EvalChild(ctx, TrueChild);
        SkeletonPose falseP = EvalChild(ctx, FalseChild);
        SkeletonPose result;
        BlendSkeletonPoses(falseP, trueP, weight, result);
        return result;
    }

    std::string_view TypeName() const override { return "BlendByConditionNode"; }
    void ResetRuntime() noexcept override
    {
        BlendElapsed = 0.0F;
        ActiveValue = false;
    }
    void Serialize(std::ostream& out) const override
    {
        out << "param " << std::quoted(ParameterName) << '\n'
            << "true " << TrueChild << '\n'
            << "false " << FalseChild << '\n'
            << "duration " << BlendDuration << '\n';
    }
    void Deserialize(std::istream& in) override
    {
        std::string key;
        in >> key >> std::quoted(ParameterName);
        in >> key >> TrueChild;
        in >> key >> FalseChild;
        in >> key >> BlendDuration;
    }
};

// ---------------------------------------------------------------------------
// LayeredBlendNode — blends LayerChild over BaseChild for specific bones
// ---------------------------------------------------------------------------
struct LayeredBlendNode : AnimGraphNode
{
    int BaseChild{-1};
    int LayerChild{-1};
    std::vector<int> BoneMask;   // joint indices to apply the layer to
    float Weight{1.0F};
    std::string WeightParameter; // optional — overrides Weight if non-empty

    SkeletonPose Evaluate(AnimGraphContext& ctx) override
    {
        SkeletonPose base  = EvalChild(ctx, BaseChild);
        SkeletonPose layer = EvalChild(ctx, LayerChild);
        const float w = WeightParameter.empty()
            ? Weight
            : ctx.GetFloat(WeightParameter);
        const float clamped = std::clamp(w, 0.0F, 1.0F);

        SkeletonPose result = base;
        for (int boneIdx : BoneMask)
        {
            if (boneIdx < 0 || boneIdx >= static_cast<int>(result.Skeleton.Joints.size()))
                continue;
            const std::size_t ji = static_cast<std::size_t>(boneIdx);
            if (ji >= layer.Skeleton.Joints.size()) continue;

            const glm::mat4& bm = base.Skeleton.Joints[ji].LocalTransform;
            const glm::mat4& lm = layer.Skeleton.Joints[ji].LocalTransform;
            // Extract TRS, lerp/slerp, recombine — same pattern as BlendSkeletonPoses
            const glm::vec3 bs{glm::length(glm::vec3{bm[0]}),
                glm::length(glm::vec3{bm[1]}), glm::length(glm::vec3{bm[2]})};
            const glm::vec3 ls{glm::length(glm::vec3{lm[0]}),
                glm::length(glm::vec3{lm[1]}), glm::length(glm::vec3{lm[2]})};
            if (bs.x <= 1.0e-6F || ls.x <= 1.0e-6F) continue;
            const glm::quat bq = glm::normalize(glm::quat_cast(
                glm::mat3{glm::vec3{bm[0]}/bs.x, glm::vec3{bm[1]}/bs.y, glm::vec3{bm[2]}/bs.z}));
            const glm::quat lq = glm::normalize(glm::quat_cast(
                glm::mat3{glm::vec3{lm[0]}/ls.x, glm::vec3{lm[1]}/ls.y, glm::vec3{lm[2]}/ls.z}));
            const glm::quat rq = glm::normalize(glm::slerp(bq, lq, clamped));
            const glm::vec3 rt = glm::mix(glm::vec3{bm[3]}, glm::vec3{lm[3]}, clamped);
            const glm::vec3 rs = glm::mix(bs, ls, clamped);
            result.Skeleton.Joints[ji].LocalTransform =
                glm::translate(glm::mat4{1.0F}, rt) *
                glm::mat4_cast(rq) *
                glm::scale(glm::mat4{1.0F}, rs);
        }
        result.ComputePose();
        return result;
    }

    std::string_view TypeName() const override { return "LayeredBlendNode"; }
    void Serialize(std::ostream& out) const override
    {
        out << "base " << BaseChild << '\n'
            << "layer " << LayerChild << '\n'
            << "weight " << Weight << '\n'
            << "weightParam " << std::quoted(WeightParameter) << '\n'
            << "bones " << BoneMask.size() << '\n';
        for (int b : BoneMask) out << b << ' ';
        out << '\n';
    }
    void Deserialize(std::istream& in) override
    {
        std::string key; std::size_t n;
        in >> key >> BaseChild;
        in >> key >> LayerChild;
        in >> key >> Weight;
        in >> key >> std::quoted(WeightParameter);
        in >> key >> n;
        BoneMask.resize(n);
        for (int& b : BoneMask) in >> b;
    }
};

// ---------------------------------------------------------------------------
// AdditiveNode — adds AdditiveChild * Weight on top of BaseChild
// ---------------------------------------------------------------------------
struct AdditiveNode : AnimGraphNode
{
    int BaseChild{-1};
    int AdditiveChild{-1};
    float Weight{1.0F};
    std::string WeightParameter;

    SkeletonPose Evaluate(AnimGraphContext& ctx) override
    {
        SkeletonPose base = EvalChild(ctx, BaseChild);
        SkeletonPose add  = EvalChild(ctx, AdditiveChild);
        const float w = WeightParameter.empty()
            ? Weight : ctx.GetFloat(WeightParameter);
        SkeletonPose result;
        BlendSkeletonPoses(base, add, std::clamp(w, 0.0F, 1.0F), result);
        return result;
    }

    std::string_view TypeName() const override { return "AdditiveNode"; }
    void Serialize(std::ostream& out) const override
    {
        out << "base " << BaseChild << '\n'
            << "additive " << AdditiveChild << '\n'
            << "weight " << Weight << '\n'
            << "weightParam " << std::quoted(WeightParameter) << '\n';
    }
    void Deserialize(std::istream& in) override
    {
        std::string key;
        in >> key >> BaseChild;
        in >> key >> AdditiveChild;
        in >> key >> Weight;
        in >> key >> std::quoted(WeightParameter);
    }
};

// ---------------------------------------------------------------------------
// SavedPoseNode — evaluates child and caches result under PoseKey
// ---------------------------------------------------------------------------
struct SavedPoseNode : AnimGraphNode
{
    std::string PoseKey;
    int Child{-1};

    SkeletonPose Evaluate(AnimGraphContext& ctx) override
    {
        SkeletonPose p = EvalChild(ctx, Child);
        ctx.SavedPoses[PoseKey] = p;
        return p;
    }

    std::string_view TypeName() const override { return "SavedPoseNode"; }
    void Serialize(std::ostream& out) const override
    {
        out << "key " << std::quoted(PoseKey) << '\n' << "child " << Child << '\n';
    }
    void Deserialize(std::istream& in) override
    {
        std::string k; in >> k >> std::quoted(PoseKey) >> k >> Child;
    }
};

// ---------------------------------------------------------------------------
// UseSavedPoseNode — retrieves a pose cached by SavedPoseNode
// ---------------------------------------------------------------------------
struct UseSavedPoseNode : AnimGraphNode
{
    std::string PoseKey;

    SkeletonPose Evaluate(AnimGraphContext& ctx) override
    {
        auto it = ctx.SavedPoses.find(PoseKey);
        return it != ctx.SavedPoses.end() ? it->second : SkeletonPose{};
    }

    std::string_view TypeName() const override { return "UseSavedPoseNode"; }
    void Serialize(std::ostream& out) const override
    {
        out << "key " << std::quoted(PoseKey) << '\n';
    }
    void Deserialize(std::istream& in) override
    {
        std::string k;
        in >> k >> std::quoted(PoseKey);
    }
};

// ---------------------------------------------------------------------------
// StateMachineNode — wraps existing AnimatorController; each state can
// reference a child node index (or fall back to ClipName sampling).
// ---------------------------------------------------------------------------
struct StateMachineNode : AnimGraphNode
{
    AnimatorController Controller;
    std::unordered_map<std::string, int> StateChildIndices;

    // Per-instance runtime (safe — graph is per-entity)
    std::string ActiveState;
    float StateTime{0.0F};
    std::string CrossfadeFrom;
    float CrossfadeElapsed{0.0F};
    float CrossfadeDuration{0.0F};

    SkeletonPose Evaluate(AnimGraphContext& ctx) override;

    std::string_view TypeName() const override { return "StateMachineNode"; }
    void ResetRuntime() noexcept override
    {
        ActiveState.clear();
        StateTime = 0.0F;
        CrossfadeFrom.clear();
        CrossfadeElapsed = 0.0F;
        CrossfadeDuration = 0.0F;
    }
    void SetRuntimeTime(const float seconds) noexcept override
    {
        StateTime = std::max(seconds, 0.0F);
    }
    void Serialize(std::ostream& out) const override;
    void Deserialize(std::istream& in) override;
};
}  // namespace fadix

// ---------------------------------------------------------------------------
// Implementations that need GltfMeshAsset fully defined — include order:
// callers must include AnimationRuntime.hpp BEFORE AnimGraphNodes.hpp.
// ---------------------------------------------------------------------------
#include "runtime/AnimationRuntime.hpp"  // brings in GltfMeshAsset, FindAnimationClip, ResolveBlendTree1D

namespace fadix
{
inline SkeletonPose ClipNode::Evaluate(AnimGraphContext& ctx)
{
    if (!ctx.MeshAsset) return {};
    const AnimationClipAsset* clip = FindAnimationClip(*ctx.MeshAsset, ClipName);
    if (clip == nullptr) return {};
    SkeletonPose pose;
    pose.Skeleton = ctx.MeshAsset->Skeleton;
    AnimationPlayer player;
    player.SetClip(clip);
    player.SetTime(CurrentTime);
    player.Update(ctx.DeltaTime, Speed, Loop, pose);
    CurrentTime = player.GetTime();
    return pose;
}

inline SkeletonPose BlendByFloatNode::Evaluate(AnimGraphContext& ctx)
{
    if (Entries.empty()) return {};
    if (Entries.size() == 1) return EvalChild(ctx, Entries[0].ChildIndex);

    // Reuse ResolveBlendTree1D by abusing ClipName as a stringified child index.
    // ponytail: stoi abuse; replace with direct threshold binary-search if perf matters.
    std::vector<BlendTree1DEntry> btEntries;
    btEntries.reserve(Entries.size());
    for (const Entry& e : Entries)
    {
        BlendTree1DEntry bte;
        bte.Threshold = e.Threshold;
        bte.ClipName  = std::to_string(e.ChildIndex); // child index as string
        btEntries.push_back(std::move(bte));
    }
    const float value = ctx.GetFloat(ParameterName);
    BlendTree1DResult res = ResolveBlendTree1D(btEntries, value);

    const int idxA = std::stoi(res.ClipA);
    SkeletonPose poseA = EvalChild(ctx, idxA);
    if (res.ClipB.empty()) return poseA;
    const int idxB = std::stoi(res.ClipB);
    SkeletonPose poseB = EvalChild(ctx, idxB);
    SkeletonPose result;
    BlendSkeletonPoses(poseA, poseB, res.Weight, result);
    return result;
}

inline SkeletonPose StateMachineNode::Evaluate(AnimGraphContext& ctx)
{
    // Boot: set entry state
    if (ActiveState.empty() && !Controller.EntryState.empty())
        ActiveState = Controller.EntryState;

    // Build minimal AnimatorComponent for FindAnimatorTransition.
    // Sync graph parameters into the copy so transition conditions see current values.
    AnimatorComponent temp;
    temp.Controller  = Controller;
    temp.ActiveState = ActiveState;
    temp.CurrentTime = StateTime;

    for (AnimatorParameter& ap : temp.Controller.Parameters)
    {
        for (const AnimGraphParameter& gp : ctx.Parameters)
        {
            if (ap.Name != gp.Name) continue;
            ap.FloatValue = gp.FloatValue;
            ap.BoolValue  = gp.BoolValue;
            ap.IntValue   = gp.IntValue;
            break;
        }
    }

    // ponytail: clipDuration=0 makes HasExitTime transitions always eligible (normalized=1.0F);
    // wire StateTime/clip length here if HasExitTime matters for this state machine node.
    const AnimatorTransition* t = FindAnimatorTransition(temp, 0.0F);
    if (t != nullptr)
    {
        CrossfadeFrom     = ActiveState;
        CrossfadeElapsed  = 0.0F;
        CrossfadeDuration = t->Duration;
        ActiveState       = t->To;
        StateTime         = 0.0F;
    }

    // Propagate consumed triggers back so they don't re-fire next frame
    for (AnimatorParameter& cp : Controller.Parameters)
    {
        if (cp.Type != AnimatorParameterType::Trigger) continue;
        for (const AnimatorParameter& tp : temp.Controller.Parameters)
        {
            if (tp.Name == cp.Name)
            {
                cp.BoolValue = tp.BoolValue;  // temp cleared it if consumed
                break;
            }
        }
    }

    // Sync consumed triggers back into graph parameters
    for (AnimGraphParameter& gp : ctx.Parameters)
    {
        if (gp.ParamType != AnimGraphParameter::Type::Trigger) continue;
        for (const AnimatorParameter& cp : Controller.Parameters)
        {
            if (cp.Name == gp.Name)
            {
                gp.BoolValue = cp.BoolValue;
                break;
            }
        }
    }

    StateTime += ctx.DeltaTime;

    // Evaluate active state child
    auto it = StateChildIndices.find(ActiveState);
    SkeletonPose activeP;
    if (it != StateChildIndices.end())
    {
        activeP = EvalChild(ctx, it->second);
    }
    else if (ctx.MeshAsset)
    {
        // Backward compat: find state's ClipName
        for (const AnimatorState& s : Controller.States)
        {
            if (s.Name != ActiveState) continue;
            SkeletonPose p; p.Skeleton = ctx.MeshAsset->Skeleton;
            AnimationPlayer player;
            const AnimationClipAsset* clip = FindAnimationClip(*ctx.MeshAsset, s.ClipName);
            if (clip) { player.SetClip(clip); player.Update(StateTime, 1.0F, true, p); }
            activeP = p;
            break;
        }
    }

    // Crossfade
    if (!CrossfadeFrom.empty() && CrossfadeDuration > 1.0e-5F)
    {
        CrossfadeElapsed += ctx.DeltaTime;
        const float w = std::min(CrossfadeElapsed / CrossfadeDuration, 1.0F);

        SkeletonPose fromP;
        auto fromIt = StateChildIndices.find(CrossfadeFrom);
        if (fromIt != StateChildIndices.end())
        {
            fromP = EvalChild(ctx, fromIt->second);
        }
        else if (ctx.MeshAsset)
        {
            // Fallback: sample ClipName from the previous state
            for (const AnimatorState& s : Controller.States)
            {
                if (s.Name != CrossfadeFrom) continue;
                fromP.Skeleton = ctx.MeshAsset->Skeleton;
                AnimationPlayer player;
                const AnimationClipAsset* clip = FindAnimationClip(*ctx.MeshAsset, s.ClipName);
                if (clip) { player.SetClip(clip); player.Update(0.0F, 1.0F, true, fromP); }
                break;
            }
        }

        SkeletonPose blended;
        BlendSkeletonPoses(fromP, activeP, w, blended);
        if (w >= 1.0F) CrossfadeFrom.clear();
        return blended;
    }
    return activeP;
}

inline void StateMachineNode::Serialize(std::ostream& out) const
{
    out << "activeState " << std::quoted(ActiveState) << '\n';
    out << "stateChildren " << StateChildIndices.size() << '\n';
    std::vector<std::pair<std::string, int>> sortedChildren{
        StateChildIndices.begin(), StateChildIndices.end()};
    std::sort(sortedChildren.begin(), sortedChildren.end());
    for (const auto& [name, idx] : sortedChildren)
        out << std::quoted(name) << ' ' << idx << '\n';
    out << "controller ";
    WriteAnimatorControllerData(out, Controller);
    out << '\n';
}

inline void StateMachineNode::Deserialize(std::istream& in)
{
    std::string key; std::size_t n;
    in >> key >> std::quoted(ActiveState);
    in >> key >> n;
    for (std::size_t i = 0; i < n; ++i)
    {
        std::string name; int idx;
        in >> std::quoted(name) >> idx;
        StateChildIndices[name] = idx;
    }
    const std::streampos controllerPosition = in.tellg();
    if (in >> key && key == "controller")
    {
        static_cast<void>(ReadAnimatorControllerData(in, Controller));
    }
    else
    {
        in.clear();
        in.seekg(controllerPosition);
    }
}
}  // namespace fadix
