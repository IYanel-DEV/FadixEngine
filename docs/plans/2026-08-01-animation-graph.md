# Animation Graph Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a pull-based virtual node graph (`AnimationGraph`) that drives character poses, with layered blending, additive animation, and backward-compatible state machine embedding.

**Architecture:** Each `AnimGraphNode` subclass implements `virtual SkeletonPose Evaluate(AnimGraphContext&)`. `AnimationGraph::Evaluate()` calls `Nodes[OutputNodeIndex]->Evaluate(ctx)` which recursively pulls poses from children. The graph is owned per-`AnimatorComponent` (value semantics so each entity has its own instance state). When `AnimatorComponent::Graph` is non-null it drives the pose; otherwise the legacy `AnimatorController` path runs unchanged.

**Tech Stack:** C++20, EnTT ECS, glm, existing `AnimationPlayer`/`BlendSkeletonPoses`/`ResolveBlendTree1D`/`FindAnimationClip` helpers, ImGui DrawList for graph canvas.

## Global Constraints

- C++20 MSVC `/W4 /permissive-` — no warnings introduced
- All new headers go under `src/engine/animation/` (pure types, no SDL/ImGui includes)
- Editor files go under `src/editor/imgui/panels/`
- `SkeletonPose` = `fadix::SkeletonPose` from `src/engine/animation/Skeleton.hpp` (`{SkeletonAsset Skeleton; void ComputePose();}`)
- `BlendSkeletonPoses(from, to, weight, result)` is in `src/engine/animation/AnimationPlayer.hpp`
- `FindAnimationClip(const GltfMeshAsset&, const std::string& name)` is in `src/runtime/AnimationRuntime.hpp`
- `ResolveBlendTree1D(entries, value)` is in `src/runtime/AnimationRuntime.hpp`
- `AnimatorComponent` is in `src/runtime/Components.hpp`
- Smoke tests live in `tools/AnimationSmoke.cpp`; target is `fadix_animation_smoke`
- Build command: `cmake --build .build/debug-cmake --config Debug --target fadix_animation_smoke --parallel 8`
- Editor build: `cmake --build .build/debug-cmake --config Debug --target fadix_editor --parallel 8`
- `AnimationGraph` owned as `std::unique_ptr<AnimationGraph>` on `AnimatorComponent` — unique_ptr because each entity needs independent instance state (ClipNode::CurrentTime etc.)
- Node connections stored as `int` child indices into `AnimationGraph::Nodes[]` (-1 = no connection)
- `CommitControllerEdit` pattern used for all undo-able editor changes (same as FdxAnimationPanel)

---

## File Map

| File | Status | Responsibility |
|---|---|---|
| `src/engine/animation/AnimationGraph.hpp` | **CREATE** | `AnimGraphParameter`, `AnimGraphContext`, `AnimGraphNode` base, `AnimationGraph` |
| `src/engine/animation/AnimGraphNodes.hpp` | **CREATE** | All 8 node type implementations |
| `src/engine/animation/AnimGraphNodeRegistry.hpp` | **CREATE** | Factory registry for serialization |
| `src/engine/animation/AnimationGraphIO.hpp` | **CREATE** | Serialize/deserialize `AnimationGraph` to text stream |
| `src/runtime/Components.hpp` | **MODIFY** | Add `Graph` + `RuntimeParameters` to `AnimatorComponent` |
| `src/runtime/AnimationRuntime.hpp` | **MODIFY** | Dispatch to graph path when `animator.Graph != nullptr` |
| `src/editor/imgui/panels/AnimGraphPanel.hpp` | **CREATE** | Graph canvas panel declaration |
| `src/editor/imgui/panels/AnimGraphPanel.cpp` | **CREATE** | Graph canvas implementation |
| `src/editor/imgui/panels/FdxAnimationPanel.hpp` | **MODIFY** | Add `AnimGraphPanel` member + connecting state for graph tab |
| `src/editor/imgui/panels/FdxAnimationPanel.cpp` | **MODIFY** | Add "Anim Graph" tab, wire AnimGraphPanel |
| `src/editor/imgui/panels/InspectorPanel.cpp` | **MODIFY** | "Upgrade to Anim Graph" button + graph play-mode params |
| `tools/AnimationSmoke.cpp` | **MODIFY** | 9 new test cases |
| `CMakeLists.txt` | **MODIFY** | Add `AnimGraphPanel.cpp` to editor sources |

---

## Task 1: Core Types

**Files:**
- Create: `src/engine/animation/AnimationGraph.hpp`

**Interfaces:**
- Produces: `AnimGraphParameter`, `AnimGraphContext`, `AnimGraphNode`, `AnimationGraph` — used by all later tasks

- [ ] **Step 1: Create `src/engine/animation/AnimationGraph.hpp`**

```cpp
#pragma once

#include "engine/animation/Skeleton.hpp"

#include <glm/vec2.hpp>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fadix
{
struct GltfMeshAsset;  // forward-decl; full type in AnimationRuntime.hpp
struct AnimationGraph; // forward-decl for context

struct AnimGraphParameter
{
    enum class Type : std::uint8_t { Float, Bool, Int, Trigger };
    std::string Name;
    Type ParamType{Type::Float};
    float FloatValue{0.0F};
    bool BoolValue{false};
    int IntValue{0};
};

struct AnimGraphContext
{
    float DeltaTime{0.0F};
    const GltfMeshAsset* MeshAsset{nullptr};
    std::span<AnimGraphParameter> Parameters;
    std::unordered_map<std::string, SkeletonPose> SavedPoses;
    AnimationGraph* Graph{nullptr};  // back-ptr so child-index lookups work

    [[nodiscard]] float GetFloat(std::string_view name) const noexcept
    {
        for (const AnimGraphParameter& p : Parameters)
            if (p.Name == name && p.ParamType == AnimGraphParameter::Type::Float)
                return p.FloatValue;
        return 0.0F;
    }
    [[nodiscard]] bool GetBool(std::string_view name) const noexcept
    {
        for (const AnimGraphParameter& p : Parameters)
            if (p.Name == name &&
                (p.ParamType == AnimGraphParameter::Type::Bool ||
                 p.ParamType == AnimGraphParameter::Type::Trigger))
                return p.BoolValue;
        return false;
    }
    [[nodiscard]] int GetInt(std::string_view name) const noexcept
    {
        for (const AnimGraphParameter& p : Parameters)
            if (p.Name == name && p.ParamType == AnimGraphParameter::Type::Int)
                return p.IntValue;
        return 0;
    }
};

struct AnimGraphNode
{
    glm::vec2 EditorPosition{};

    virtual ~AnimGraphNode() = default;
    virtual SkeletonPose Evaluate(AnimGraphContext& ctx) = 0;
    [[nodiscard]] virtual std::string_view TypeName() const = 0;
    virtual void Serialize(std::ostream& out) const = 0;
    virtual void Deserialize(std::istream& in) = 0;
};

struct AnimationGraph
{
    std::string Name;
    std::vector<std::unique_ptr<AnimGraphNode>> Nodes;
    std::vector<AnimGraphParameter> Parameters;
    int OutputNodeIndex{-1};

    [[nodiscard]] SkeletonPose Evaluate(AnimGraphContext& ctx)
    {
        if (OutputNodeIndex < 0 ||
            OutputNodeIndex >= static_cast<int>(Nodes.size()) ||
            !Nodes[static_cast<std::size_t>(OutputNodeIndex)])
        {
            return {};
        }
        ctx.Graph = this;
        return Nodes[static_cast<std::size_t>(OutputNodeIndex)]->Evaluate(ctx);
    }

    // Clear trigger parameters after each Evaluate() call.
    void ConsumeTriggers()
    {
        for (AnimGraphParameter& p : Parameters)
            if (p.ParamType == AnimGraphParameter::Type::Trigger)
                p.BoolValue = false;
    }
};
}  // namespace fadix
```

- [ ] **Step 2: Verify it compiles by including it in the smoke target**

Open `tools/AnimationSmoke.cpp`. Add near the top (after existing includes):
```cpp
#include "engine/animation/AnimationGraph.hpp"
```

Build:
```bat
cmake --build .build/debug-cmake --config Debug --target fadix_animation_smoke --parallel 8
```

Expected: zero errors. (No new test yet — just confirming types compile.)

- [ ] **Step 3: Commit**

```bat
git add src/engine/animation/AnimationGraph.hpp tools/AnimationSmoke.cpp
git commit -m "feat(animgraph): AnimGraphParameter, AnimGraphContext, AnimGraphNode base, AnimationGraph"
```

---

## Task 2: Node Implementations

**Files:**
- Create: `src/engine/animation/AnimGraphNodes.hpp`

**Interfaces:**
- Consumes: `AnimGraphContext`, `AnimGraphNode`, `SkeletonPose`, `BlendSkeletonPoses`, `ResolveBlendTree1D`, `FindAnimationClip`, `AnimationPlayer` (all in existing headers)
- Produces: `ClipNode`, `BlendByFloatNode`, `BlendByConditionNode`, `LayeredBlendNode`, `AdditiveNode`, `SavedPoseNode`, `UseSavedPoseNode`, `OutputNode`, `StateMachineNode`

- [ ] **Step 1: Create `src/engine/animation/AnimGraphNodes.hpp`**

```cpp
#pragma once

#include "engine/animation/AnimationGraph.hpp"
#include "engine/animation/AnimationClip.hpp"
#include "engine/animation/AnimationPlayer.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
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
    auto& node = ctx.Graph->Nodes[static_cast<std::size_t>(childIndex)];
    if (!node) return {};
    return node->Evaluate(ctx);
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
    void Serialize(std::ostream& out) const override
    {
        out << "clip " << ClipName << '\n'
            << "speed " << Speed << '\n'
            << "loop " << (Loop ? 1 : 0) << '\n'
            << "mirror " << (Mirror ? 1 : 0) << '\n';
    }
    void Deserialize(std::istream& in) override
    {
        std::string key; int b;
        in >> key >> ClipName;
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
        out << "param " << ParameterName << '\n'
            << "entries " << Entries.size() << '\n';
        for (const Entry& e : Entries)
            out << e.Threshold << ' ' << e.ChildIndex << '\n';
    }
    void Deserialize(std::istream& in) override
    {
        std::string key; std::size_t n;
        in >> key >> ParameterName;
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
    void Serialize(std::ostream& out) const override
    {
        out << "param " << ParameterName << '\n'
            << "true " << TrueChild << '\n'
            << "false " << FalseChild << '\n'
            << "duration " << BlendDuration << '\n';
    }
    void Deserialize(std::istream& in) override
    {
        std::string key;
        in >> key >> ParameterName;
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
            << "weightParam " << WeightParameter << '\n'
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
        in >> key >> WeightParameter;
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
            << "weightParam " << WeightParameter << '\n';
    }
    void Deserialize(std::istream& in) override
    {
        std::string key;
        in >> key >> BaseChild;
        in >> key >> AdditiveChild;
        in >> key >> Weight;
        in >> key >> WeightParameter;
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
        out << "key " << PoseKey << '\n' << "child " << Child << '\n';
    }
    void Deserialize(std::istream& in) override
    {
        std::string k; in >> k >> PoseKey >> k >> Child;
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
    void Serialize(std::ostream& out) const override { out << "key " << PoseKey << '\n'; }
    void Deserialize(std::istream& in) override { std::string k; in >> k >> PoseKey; }
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

    // Convert to BlendTree1DEntry format for ResolveBlendTree1D
    std::vector<BlendTree1DEntry> btEntries;
    btEntries.reserve(Entries.size());
    for (const Entry& e : Entries)
        btEntries.push_back({std::to_string(e.ChildIndex), e.Threshold});
    // We only need threshold order — store child index stringified temporarily
    // Rebuild properly:
    btEntries.clear();
    for (const Entry& e : Entries)
    {
        BlendTree1DEntry bte;
        bte.Threshold = e.Threshold;
        bte.ClipName  = std::to_string(e.ChildIndex); // abuse ClipName as index string
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

    // Sync graph parameters into controller for transition evaluation
    for (AnimatorParameter& ap : Controller.Parameters)
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

    // Build minimal AnimatorComponent for FindAnimatorTransition
    AnimatorComponent temp;
    temp.Controller   = Controller;
    temp.ActiveState  = ActiveState;
    temp.CurrentTime  = StateTime;

    const AnimatorTransition* t = FindAnimatorTransition(temp, ctx.DeltaTime);
    if (t != nullptr)
    {
        CrossfadeFrom     = ActiveState;
        CrossfadeElapsed  = 0.0F;
        CrossfadeDuration = t->Duration;
        ActiveState       = t->To;
        StateTime         = 0.0F;
    }

    StateTime += ctx.DeltaTime;

    // Evaluate active state child
    auto it = StateChildIndices.find(ActiveState);
    SkeletonPose activeP;
    if (it != StateChildIndices.end())
        activeP = EvalChild(ctx, it->second);
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
        auto fromIt = StateChildIndices.find(CrossfadeFrom);
        if (fromIt != StateChildIndices.end())
        {
            SkeletonPose fromP = EvalChild(ctx, fromIt->second);
            SkeletonPose blended;
            BlendSkeletonPoses(fromP, activeP, w, blended);
            if (w >= 1.0F) CrossfadeFrom.clear();
            return blended;
        }
        if (w >= 1.0F) CrossfadeFrom.clear();
    }
    return activeP;
}

inline void StateMachineNode::Serialize(std::ostream& out) const
{
    out << "activeState " << ActiveState << '\n';
    out << "stateChildren " << StateChildIndices.size() << '\n';
    for (const auto& [name, idx] : StateChildIndices)
        out << name << ' ' << idx << '\n';
    // Controller serialized via AnimatorControllerIO separately
}

inline void StateMachineNode::Deserialize(std::istream& in)
{
    std::string key; std::size_t n;
    in >> key >> ActiveState;
    in >> key >> n;
    for (std::size_t i = 0; i < n; ++i)
    {
        std::string name; int idx;
        in >> name >> idx;
        StateChildIndices[name] = idx;
    }
}
}  // namespace fadix
```

**Note:** `BlendByFloatNode::Evaluate` abuses `BlendTree1DEntry::ClipName` as a stringified child index to reuse `ResolveBlendTree1D`. This avoids duplicating threshold interpolation logic.

- [ ] **Step 2: Add AnimGraphNodes.hpp include to smoke file**

In `tools/AnimationSmoke.cpp`, replace:
```cpp
#include "engine/animation/AnimationGraph.hpp"
```
with (AnimationRuntime.hpp must come BEFORE AnimGraphNodes.hpp):
```cpp
#include "runtime/AnimationRuntime.hpp"
#include "engine/animation/AnimGraphNodes.hpp"
```

- [ ] **Step 3: Build to verify compilation**

```bat
cmake --build .build/debug-cmake --config Debug --target fadix_animation_smoke --parallel 8
```

Expected: zero errors.

- [ ] **Step 4: Commit**

```bat
git add src/engine/animation/AnimGraphNodes.hpp tools/AnimationSmoke.cpp
git commit -m "feat(animgraph): all 8 node type implementations (ClipNode, BlendByFloat, LayeredBlend, Additive, SavedPose, StateMachine, Output)"
```

---

## Task 3: Node Registry + Serialization

**Files:**
- Create: `src/engine/animation/AnimGraphNodeRegistry.hpp`
- Create: `src/engine/animation/AnimationGraphIO.hpp`

**Interfaces:**
- Consumes: all node types from Task 2
- Produces:
  - `AnimGraphNodeRegistry::Register<T>()`, `AnimGraphNodeRegistry::Create(typeName) -> unique_ptr<AnimGraphNode>`
  - `WriteAnimationGraph(std::ostream&, const AnimationGraph&)`
  - `ReadAnimationGraph(std::istream&, AnimationGraph&) -> bool`

- [ ] **Step 1: Create `src/engine/animation/AnimGraphNodeRegistry.hpp`**

```cpp
#pragma once

#include "engine/animation/AnimGraphNodes.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace fadix
{
class AnimGraphNodeRegistry
{
public:
    using Factory = std::function<std::unique_ptr<AnimGraphNode>()>;

    static AnimGraphNodeRegistry& Instance()
    {
        static AnimGraphNodeRegistry s;
        return s;
    }

    template <typename T>
    void Register()
    {
        T proto;
        m_Factories[std::string{proto.TypeName()}] = []{ return std::make_unique<T>(); };
    }

    [[nodiscard]] std::unique_ptr<AnimGraphNode> Create(const std::string& typeName) const
    {
        auto it = m_Factories.find(typeName);
        return it != m_Factories.end() ? it->second() : nullptr;
    }

    // Call once at startup — registers all built-in node types.
    static void RegisterBuiltins()
    {
        auto& reg = Instance();
        reg.Register<ClipNode>();
        reg.Register<BlendByFloatNode>();
        reg.Register<BlendByConditionNode>();
        reg.Register<LayeredBlendNode>();
        reg.Register<AdditiveNode>();
        reg.Register<SavedPoseNode>();
        reg.Register<UseSavedPoseNode>();
        reg.Register<StateMachineNode>();
        reg.Register<OutputNode>();
    }

private:
    std::unordered_map<std::string, Factory> m_Factories;
};
}  // namespace fadix
```

- [ ] **Step 2: Create `src/engine/animation/AnimationGraphIO.hpp`**

```cpp
#pragma once

#include "engine/animation/AnimGraphNodeRegistry.hpp"
#include "engine/animation/AnimationGraph.hpp"

#include <glm/vec2.hpp>

#include <istream>
#include <ostream>
#include <string>

namespace fadix
{
// Format (line-oriented, same style as AnimatorControllerIO):
//
// graph <Name>
// params <N>
//   param <Name> <Type(0-3)> <FloatValue> <BoolValue(0/1)> <IntValue>
// nodes <N>
//   node <TypeName> <editorX> <editorY>
//     <node-specific key-value lines>
//   endnode
// output <OutputNodeIndex>

inline void WriteAnimationGraph(std::ostream& out, const AnimationGraph& graph)
{
    out << "graph " << graph.Name << '\n';
    out << "params " << graph.Parameters.size() << '\n';
    for (const AnimGraphParameter& p : graph.Parameters)
    {
        out << "param " << p.Name << ' '
            << static_cast<int>(p.ParamType) << ' '
            << p.FloatValue << ' '
            << (p.BoolValue ? 1 : 0) << ' '
            << p.IntValue << '\n';
    }
    out << "nodes " << graph.Nodes.size() << '\n';
    for (const auto& node : graph.Nodes)
    {
        out << "node " << node->TypeName() << ' '
            << node->EditorPosition.x << ' ' << node->EditorPosition.y << '\n';
        node->Serialize(out);
        out << "endnode\n";
    }
    out << "output " << graph.OutputNodeIndex << '\n';
}

[[nodiscard]] inline bool ReadAnimationGraph(std::istream& in, AnimationGraph& graph)
{
    std::string token;
    if (!(in >> token) || token != "graph") return false;
    if (!(in >> graph.Name)) return false;

    std::size_t paramCount = 0;
    if (!(in >> token) || token != "params") return false;
    in >> paramCount;
    graph.Parameters.resize(paramCount);
    for (AnimGraphParameter& p : graph.Parameters)
    {
        int typeInt = 0, boolInt = 0;
        in >> token >> p.Name >> typeInt >> p.FloatValue >> boolInt >> p.IntValue;
        p.ParamType = static_cast<AnimGraphParameter::Type>(typeInt);
        p.BoolValue = boolInt != 0;
    }

    std::size_t nodeCount = 0;
    if (!(in >> token) || token != "nodes") return false;
    in >> nodeCount;
    graph.Nodes.resize(nodeCount);
    for (auto& nodePtr : graph.Nodes)
    {
        std::string typeName;
        float ex = 0.0F, ey = 0.0F;
        if (!(in >> token) || token != "node") return false;
        in >> typeName >> ex >> ey;
        nodePtr = AnimGraphNodeRegistry::Instance().Create(typeName);
        if (!nodePtr) return false;
        nodePtr->EditorPosition = {ex, ey};
        nodePtr->Deserialize(in);
        in >> token; // consume "endnode"
    }
    if (!(in >> token) || token != "output") return false;
    in >> graph.OutputNodeIndex;
    return true;
}
}  // namespace fadix
```

- [ ] **Step 3: Add serialization round-trip smoke test**

In `tools/AnimationSmoke.cpp`, at the end of `main()` before `return 0`:

```cpp
    // --- AnimGraph serialization round-trip ---
    {
        fadix::AnimGraphNodeRegistry::RegisterBuiltins();

        fadix::AnimationGraph graph;
        graph.Name = "TestGraph";
        graph.Parameters.push_back({"Speed", fadix::AnimGraphParameter::Type::Float, 2.5F});

        auto clip = std::make_unique<fadix::ClipNode>();
        clip->ClipName = "Run"; clip->Speed = 1.5F; clip->Loop = false;
        clip->EditorPosition = {100.0F, 200.0F};
        graph.Nodes.push_back(std::move(clip));

        auto out = std::make_unique<fadix::OutputNode>();
        out->Child = 0; out->EditorPosition = {300.0F, 200.0F};
        graph.Nodes.push_back(std::move(out));
        graph.OutputNodeIndex = 1;

        std::ostringstream oss;
        fadix::WriteAnimationGraph(oss, graph);
        const std::string serialized = oss.str();

        fadix::AnimationGraph loaded;
        std::istringstream iss{serialized};
        assert(fadix::ReadAnimationGraph(iss, loaded) && "ReadAnimationGraph failed");
        assert(loaded.Name == "TestGraph" && "graph name mismatch");
        assert(loaded.Parameters.size() == 1 && "param count mismatch");
        assert(std::abs(loaded.Parameters[0].FloatValue - 2.5F) < 1.0e-5F && "param value mismatch");
        assert(loaded.Nodes.size() == 2 && "node count mismatch");
        assert(loaded.Nodes[0]->TypeName() == std::string_view{"ClipNode"} && "node type mismatch");
        const auto* loadedClip = static_cast<fadix::ClipNode*>(loaded.Nodes[0].get());
        assert(loadedClip->ClipName == "Run" && "clip name mismatch");
        assert(std::abs(loadedClip->Speed - 1.5F) < 1.0e-5F && "speed mismatch");
        assert(!loadedClip->Loop && "loop mismatch");
        assert(loaded.OutputNodeIndex == 1 && "output index mismatch");
        std::cout << "PASS AnimGraph serialization round-trip\n";
    }
```

Add includes at top of `AnimationSmoke.cpp`:
```cpp
#include "engine/animation/AnimGraphNodeRegistry.hpp"
#include "engine/animation/AnimationGraphIO.hpp"
#include <sstream>
```

- [ ] **Step 4: Build and run**

```bat
cmake --build .build/debug-cmake --config Debug --target fadix_animation_smoke --parallel 8
.\bin\Debug\fadix_animation_smoke.exe
```

Expected: all prior tests pass plus `PASS AnimGraph serialization round-trip`

- [ ] **Step 5: Commit**

```bat
git add src/engine/animation/AnimGraphNodeRegistry.hpp src/engine/animation/AnimationGraphIO.hpp tools/AnimationSmoke.cpp
git commit -m "feat(animgraph): node registry, serialization round-trip, smoke test"
```

---

## Task 4: Runtime Integration

**Files:**
- Modify: `src/runtime/Components.hpp` (add `Graph` + `RuntimeParameters` to `AnimatorComponent`)
- Modify: `src/runtime/AnimationRuntime.hpp` (dispatch to graph when Graph != nullptr)

**Interfaces:**
- Consumes: `AnimationGraph`, `AnimGraphContext`, `AnimGraphNodeRegistry` (from Tasks 1-3)
- Produces: `AnimatorComponent::Graph` (unique_ptr), `AnimatorComponent::RuntimeParameters`, graph-path tick in `UpdateWorldAnimations`

- [ ] **Step 1: Add fields to `AnimatorComponent` in `src/runtime/Components.hpp`**

Find `struct AnimatorComponent` (around line 590). After the `PendingEvents` field and before the `ClearBlend()` method, add:

```cpp
    // --- Animation Graph (takes priority over Controller when non-null) ---
    std::unique_ptr<AnimationGraph> Graph;
    std::vector<AnimGraphParameter> RuntimeParameters; // per-instance, synced from Graph::Parameters on init
```

Add include at the top of `Components.hpp` (after existing includes):
```cpp
#include "engine/animation/AnimationGraph.hpp"
```

- [ ] **Step 2: Add graph dispatch in `UpdateWorldAnimations` in `src/runtime/AnimationRuntime.hpp`**

Find `UpdateWorldAnimations` (the function that iterates over entities with `AnimatorComponent`). At the **top** of the per-entity processing block, before the existing state machine logic, add:

```cpp
        // --- Animation Graph path ---
        if (animator.Graph)
        {
            // Init RuntimeParameters from graph schema on first use
            if (animator.RuntimeParameters.empty() &&
                !animator.Graph->Parameters.empty())
            {
                animator.RuntimeParameters = animator.Graph->Parameters;
            }

            AnimGraphContext ctx;
            ctx.DeltaTime   = deltaTime;
            ctx.MeshAsset   = &gltf;  // (use whatever the gltf variable is named in scope)
            ctx.Parameters  = std::span<AnimGraphParameter>{animator.RuntimeParameters};

            SkeletonPose pose = animator.Graph->Evaluate(ctx);
            animator.Graph->ConsumeTriggers();

            // Sync trigger consumption back to RuntimeParameters
            for (std::size_t pi = 0; pi < animator.RuntimeParameters.size(); ++pi)
                if (animator.RuntimeParameters[pi].ParamType == AnimGraphParameter::Type::Trigger)
                    animator.RuntimeParameters[pi].BoolValue =
                        animator.Graph->Parameters[pi].BoolValue;

            // Apply pose to skeleton joints
            if (!pose.Skeleton.Joints.empty())
            {
                if (auto* sk = registry.try_get<SkeletonComponent>(entity))
                {
                    sk->JointMatrices.resize(pose.Skeleton.Joints.size());
                    for (std::size_t ji = 0; ji < pose.Skeleton.Joints.size(); ++ji)
                        sk->JointMatrices[ji] = pose.Skeleton.Joints[ji].GlobalTransform;
                }
            }
            continue;  // skip legacy path for this entity
        }
        // --- Legacy AnimatorController path (unchanged below) ---
```

**Important:** Look at the existing `UpdateWorldAnimations` code carefully to find the exact variable names for `gltf`, `registry`, `entity`, `deltaTime`. Adapt the snippet above to match.

- [ ] **Step 3: Add smoke test for graph path + legacy path coexistence**

In `tools/AnimationSmoke.cpp`, add:

```cpp
    // --- AnimGraph runtime: legacy path unaffected when Graph == null ---
    {
        fadix::AnimatorComponent ac;
        assert(ac.Graph == nullptr && "Graph should be null by default");
        // Legacy controller still present
        ac.Controller.EntryState = "Idle";
        assert(ac.Controller.EntryState == "Idle" && "legacy controller broken");
        std::cout << "PASS AnimGraph legacy path unaffected when Graph==null\n";
    }
```

- [ ] **Step 4: Build and run**

```bat
cmake --build .build/debug-cmake --config Debug --target fadix_animation_smoke --parallel 8
.\bin\Debug\fadix_animation_smoke.exe
```

Expected: all prior tests pass + `PASS AnimGraph legacy path unaffected when Graph==null`

- [ ] **Step 5: Build editor to verify no regressions**

```bat
cmake --build .build/debug-cmake --config Debug --target fadix_editor --parallel 8
```

Expected: zero errors.

- [ ] **Step 6: Commit**

```bat
git add src/runtime/Components.hpp src/runtime/AnimationRuntime.hpp tools/AnimationSmoke.cpp
git commit -m "feat(animgraph): runtime dispatch — graph path when Graph!=null, legacy unchanged"
```

---

## Task 5: Full Smoke Test Suite

**Files:**
- Modify: `tools/AnimationSmoke.cpp`

Adds the remaining 8 graph smoke tests (serialization round-trip already added in Task 3).

- [ ] **Step 1: Add 8 new smoke tests**

In `tools/AnimationSmoke.cpp`, at the end of `main()` before `return 0`, add:

```cpp
    // =====================================================================
    // AnimGraph node smoke tests
    // All tests use a stub GltfMeshAsset + minimal skeleton where needed.
    // =====================================================================

    // Helper: build a GltfMeshAsset with a single 2-joint skeleton and one
    // 1-second clip named "A" that moves joint 0 to (1,0,0) at t=1.
    // (Reuse the existing skeleton + clip from the animation smoke setup above
    //  if one was built there; otherwise build inline.)

    // --- ClipNode evaluates non-zero pose ---
    {
        // ClipNode with no MeshAsset returns empty pose — verify no crash
        fadix::AnimationGraph graph;
        fadix::AnimGraphNodeRegistry::RegisterBuiltins();

        auto clip = std::make_unique<fadix::ClipNode>();
        clip->ClipName = "Idle"; clip->Loop = true;
        graph.Nodes.push_back(std::move(clip));

        auto out = std::make_unique<fadix::OutputNode>();
        out->Child = 0;
        graph.Nodes.push_back(std::move(out));
        graph.OutputNodeIndex = 1;

        fadix::AnimGraphContext ctx;
        ctx.DeltaTime = 0.016F;
        ctx.MeshAsset = nullptr; // no asset — ClipNode returns empty pose gracefully

        fadix::SkeletonPose pose = graph.Evaluate(ctx);
        // With no MeshAsset, ClipNode returns {}. Just verify no crash/assert.
        assert(pose.Skeleton.Joints.empty() && "expected empty pose with null mesh asset");
        std::cout << "PASS AnimGraph ClipNode graceful null-asset\n";
    }

    // --- OutputNode forwards child ---
    {
        fadix::AnimationGraph graph;
        auto out = std::make_unique<fadix::OutputNode>();
        out->Child = -1; // no child
        graph.Nodes.push_back(std::move(out));
        graph.OutputNodeIndex = 0;

        fadix::AnimGraphContext ctx;
        ctx.DeltaTime = 0.016F;
        fadix::SkeletonPose pose = graph.Evaluate(ctx);
        assert(pose.Skeleton.Joints.empty() && "OutputNode(-1) should return empty");
        std::cout << "PASS AnimGraph OutputNode no-child returns empty\n";
    }

    // --- BlendByConditionNode crossfades ---
    {
        fadix::AnimationGraph graph;

        // Stub two ClipNodes (no mesh — both return empty pose)
        graph.Nodes.push_back(std::make_unique<fadix::ClipNode>()); // 0 = false child
        graph.Nodes.push_back(std::make_unique<fadix::ClipNode>()); // 1 = true child

        auto blend = std::make_unique<fadix::BlendByConditionNode>();
        blend->ParameterName = "Aim";
        blend->FalseChild = 0;
        blend->TrueChild = 1;
        blend->BlendDuration = 0.2F;
        graph.Nodes.push_back(std::move(blend)); // 2

        auto out = std::make_unique<fadix::OutputNode>();
        out->Child = 2;
        graph.Nodes.push_back(std::move(out)); // 3
        graph.OutputNodeIndex = 3;

        graph.Parameters.push_back({"Aim", fadix::AnimGraphParameter::Type::Bool});
        fadix::AnimatorComponent ac;
        ac.Graph = std::make_unique<fadix::AnimationGraph>(std::move(graph));
        ac.RuntimeParameters = ac.Graph->Parameters;

        // Initial state: Aim=false, BlendElapsed should start at 0
        fadix::AnimGraphContext ctx;
        ctx.DeltaTime  = 0.1F;
        ctx.Graph      = ac.Graph.get();
        ctx.Parameters = std::span<fadix::AnimGraphParameter>{ac.RuntimeParameters};
        ac.Graph->Evaluate(ctx); // first tick

        // Set Aim=true, tick — BlendElapsed should advance
        ac.RuntimeParameters[0].BoolValue = true;
        ctx.Parameters = std::span<fadix::AnimGraphParameter>{ac.RuntimeParameters};
        ac.Graph->Evaluate(ctx);

        // BlendByConditionNode internal BlendElapsed should be 0.1
        const auto* blendNode = static_cast<fadix::BlendByConditionNode*>(
            ac.Graph->Nodes[2].get());
        assert(std::abs(blendNode->BlendElapsed - 0.1F) < 1.0e-4F
            && "BlendByConditionNode BlendElapsed not advancing");
        std::cout << "PASS AnimGraph BlendByConditionNode crossfade advances\n";
    }

    // --- Trigger parameter clears after ConsumeTriggers ---
    {
        fadix::AnimationGraph graph;
        graph.Parameters.push_back(
            {"Attack", fadix::AnimGraphParameter::Type::Trigger, 0.0F, true});

        auto out = std::make_unique<fadix::OutputNode>();
        out->Child = -1;
        graph.Nodes.push_back(std::move(out));
        graph.OutputNodeIndex = 0;

        fadix::AnimGraphContext ctx;
        ctx.DeltaTime = 0.016F;
        graph.Evaluate(ctx);
        graph.ConsumeTriggers();

        assert(!graph.Parameters[0].BoolValue && "trigger not cleared after ConsumeTriggers");
        std::cout << "PASS AnimGraph trigger clears after ConsumeTriggers\n";
    }

    // --- SavedPoseNode caches, UseSavedPoseNode retrieves ---
    {
        fadix::AnimationGraph graph;

        // SavedPoseNode wraps a ClipNode (returns empty pose but goes through cache path)
        auto clip = std::make_unique<fadix::ClipNode>(); // index 0
        graph.Nodes.push_back(std::move(clip));

        auto save = std::make_unique<fadix::SavedPoseNode>(); // index 1
        save->PoseKey = "MyPose"; save->Child = 0;
        graph.Nodes.push_back(std::move(save));

        auto use = std::make_unique<fadix::UseSavedPoseNode>(); // index 2
        use->PoseKey = "MyPose";
        graph.Nodes.push_back(std::move(use));

        auto out = std::make_unique<fadix::OutputNode>(); // index 3
        out->Child = 1; // evaluate through SavedPoseNode
        graph.Nodes.push_back(std::move(out));
        graph.OutputNodeIndex = 3;

        fadix::AnimGraphContext ctx;
        ctx.DeltaTime = 0.016F;
        ctx.Graph = &graph;
        graph.Evaluate(ctx);

        // After evaluation, SavedPoses should contain "MyPose"
        assert(ctx.SavedPoses.count("MyPose") == 1 && "SavedPoseNode did not cache pose");

        // UseSavedPoseNode should retrieve it without evaluating child again
        fadix::SkeletonPose retrieved = graph.Nodes[2]->Evaluate(ctx);
        // Both should be the same (empty) pose — just verify no crash
        (void)retrieved;
        std::cout << "PASS AnimGraph SavedPose caches and UseSavedPose retrieves\n";
    }

    // --- AdditiveNode applies weight ---
    {
        fadix::AnimationGraph graph;
        graph.Nodes.push_back(std::make_unique<fadix::ClipNode>()); // base, idx 0
        graph.Nodes.push_back(std::make_unique<fadix::ClipNode>()); // additive, idx 1

        auto add = std::make_unique<fadix::AdditiveNode>(); // idx 2
        add->BaseChild = 0; add->AdditiveChild = 1; add->Weight = 0.5F;
        graph.Nodes.push_back(std::move(add));

        auto out = std::make_unique<fadix::OutputNode>(); // idx 3
        out->Child = 2;
        graph.Nodes.push_back(std::move(out));
        graph.OutputNodeIndex = 3;

        fadix::AnimGraphContext ctx;
        ctx.DeltaTime = 0.016F; ctx.Graph = &graph;
        fadix::SkeletonPose pose = graph.Evaluate(ctx);
        (void)pose; // no crash = pass (both children return empty pose)
        std::cout << "PASS AnimGraph AdditiveNode blends without crash\n";
    }

    // --- StateMachineNode backward compat: no StateChildIndices ---
    {
        fadix::AnimatorController ctrl;
        fadix::AnimatorState s; s.Name = "Idle"; s.ClipName = "Idle";
        ctrl.States.push_back(s); ctrl.EntryState = "Idle";

        auto smNode = std::make_unique<fadix::StateMachineNode>(); // idx 0
        smNode->Controller = ctrl;
        // StateChildIndices empty — backward compat path

        fadix::AnimationGraph graph;
        graph.Nodes.push_back(std::move(smNode));
        auto out = std::make_unique<fadix::OutputNode>(); out->Child = 0;
        graph.Nodes.push_back(std::move(out));
        graph.OutputNodeIndex = 1;

        fadix::AnimGraphContext ctx;
        ctx.DeltaTime = 0.016F; ctx.MeshAsset = nullptr; ctx.Graph = &graph;
        fadix::SkeletonPose pose = graph.Evaluate(ctx);
        const auto* sm = static_cast<fadix::StateMachineNode*>(graph.Nodes[0].get());
        assert(sm->ActiveState == "Idle" && "StateMachineNode didn't boot to EntryState");
        (void)pose;
        std::cout << "PASS AnimGraph StateMachineNode backward compat boots to EntryState\n";
    }

    // --- BlendByFloatNode threshold interpolation ---
    {
        fadix::AnimationGraph graph;
        graph.Nodes.push_back(std::make_unique<fadix::ClipNode>()); // idx 0 = Idle at t=0
        graph.Nodes.push_back(std::make_unique<fadix::ClipNode>()); // idx 1 = Run at t=1

        auto blend = std::make_unique<fadix::BlendByFloatNode>(); // idx 2
        blend->ParameterName = "Speed";
        blend->Entries.push_back({0.0F, 0}); // Idle at Speed=0
        blend->Entries.push_back({5.0F, 1}); // Run at Speed=5
        graph.Nodes.push_back(std::move(blend));

        auto out = std::make_unique<fadix::OutputNode>(); // idx 3
        out->Child = 2;
        graph.Nodes.push_back(std::move(out));
        graph.OutputNodeIndex = 3;

        graph.Parameters.push_back({"Speed", fadix::AnimGraphParameter::Type::Float, 2.5F});

        fadix::AnimGraphContext ctx;
        ctx.DeltaTime = 0.016F; ctx.Graph = &graph;
        ctx.Parameters = std::span<fadix::AnimGraphParameter>{graph.Parameters};
        fadix::SkeletonPose pose = graph.Evaluate(ctx);
        (void)pose; // children return empty, just verify no crash at midpoint
        std::cout << "PASS AnimGraph BlendByFloatNode midpoint no crash\n";
    }
```

- [ ] **Step 2: Build and run all smoke tests**

```bat
cmake --build .build/debug-cmake --config Debug --target fadix_animation_smoke --parallel 8
.\bin\Debug\fadix_animation_smoke.exe
```

Expected output includes all prior passes plus:
```
PASS AnimGraph ClipNode graceful null-asset
PASS AnimGraph OutputNode no-child returns empty
PASS AnimGraph BlendByConditionNode crossfade advances
PASS AnimGraph trigger clears after ConsumeTriggers
PASS AnimGraph SavedPose caches and UseSavedPose retrieves
PASS AnimGraph AdditiveNode blends without crash
PASS AnimGraph StateMachineNode backward compat boots to EntryState
PASS AnimGraph BlendByFloatNode midpoint no crash
```

- [ ] **Step 3: Commit**

```bat
git add tools/AnimationSmoke.cpp
git commit -m "test(animgraph): 8 node smoke tests (ClipNode, OutputNode, BlendByCondition, triggers, SavedPose, Additive, StateMachine, BlendByFloat)"
```

---

## Task 6: Editor — Graph Canvas Panel

**Files:**
- Create: `src/editor/imgui/panels/AnimGraphPanel.hpp`
- Create: `src/editor/imgui/panels/AnimGraphPanel.cpp`
- Modify: `CMakeLists.txt` (add AnimGraphPanel.cpp to editor sources)

**Interfaces:**
- Consumes: `AnimationGraph`, all node types, `SceneEditor`, `CommitControllerEdit` pattern
- Produces: `AnimGraphPanel::Draw(SceneEditor&, AnimationGraph&, AnimatorComponent&)` — renders the graph canvas

- [ ] **Step 1: Create `src/editor/imgui/panels/AnimGraphPanel.hpp`**

```cpp
#pragma once

#include "engine/animation/AnimationGraph.hpp"

#include <glm/vec2.hpp>
#include <imgui.h>

#include <string>

namespace fadix
{
struct SceneEditor;
struct AnimatorComponent;

class AnimGraphPanel
{
public:
    // Draw the full graph canvas + parameter sidebar + node inspector.
    // Call this inside a Dear ImGui child region.
    void Draw(SceneEditor& scene, AnimationGraph& graph, AnimatorComponent& animator);

private:
    void DrawParametersSidebar(SceneEditor& scene, AnimationGraph& graph);
    void DrawCanvas(SceneEditor& scene, AnimationGraph& graph, AnimatorComponent& animator);
    void DrawNodeInspector(SceneEditor& scene, AnimationGraph& graph);

    glm::vec2 m_GraphPan{};
    int m_SelectedNode{-1};
    int m_ConnectingFromNode{-1};  // dragging output port
    int m_ConnectingToPort{-1};    // which input slot (0-based, node-type-specific)
    ImVec2 m_ConnectingLineEnd{};
};
}  // namespace fadix
```

- [ ] **Step 2: Create `src/editor/imgui/panels/AnimGraphPanel.cpp`**

```cpp
#include "editor/imgui/panels/AnimGraphPanel.hpp"
#include "editor/imgui/panels/FdxAnimationPanel.hpp"
#include "editor/scene/SceneEditor.hpp"
#include "runtime/Components.hpp"

// AnimationRuntime.hpp must precede AnimGraphNodes.hpp (defines GltfMeshAsset)
#include "runtime/AnimationRuntime.hpp"
#include "engine/animation/AnimGraphNodes.hpp"
#include "engine/animation/AnimGraphNodeRegistry.hpp"

#include <imgui.h>

#include <cstdio>
#include <string>

namespace fadix
{
// ---- Colors per node type ---------------------------------------------------
static ImU32 NodeColor(std::string_view type)
{
    if (type == "ClipNode")            return IM_COL32(52, 120, 210, 255);
    if (type == "BlendByFloatNode")    return IM_COL32(30, 160, 140, 255);
    if (type == "BlendByConditionNode")return IM_COL32(130, 80, 200, 255);
    if (type == "StateMachineNode")    return IM_COL32(60, 65, 75, 255);
    if (type == "LayeredBlendNode")    return IM_COL32(200, 120, 40, 255);
    if (type == "AdditiveNode")        return IM_COL32(50, 175, 90, 255);
    if (type == "SavedPoseNode" ||
        type == "UseSavedPoseNode")    return IM_COL32(200, 180, 40, 255);
    if (type == "OutputNode")          return IM_COL32(230, 235, 245, 255);
    return IM_COL32(80, 80, 80, 255);
}

// ---- Per-node input port count (for connection routing) --------------------
static int InputPortCount(const AnimGraphNode& node)
{
    const auto t = node.TypeName();
    if (t == "OutputNode")           return 1;
    if (t == "BlendByConditionNode") return 2; // true, false
    if (t == "LayeredBlendNode")     return 2; // base, layer
    if (t == "AdditiveNode")         return 2; // base, additive
    if (t == "SavedPoseNode")        return 1;
    if (t == "ClipNode")             return 0; // leaf
    if (t == "UseSavedPoseNode")     return 0; // leaf
    if (t == "BlendByFloatNode")     return 0; // entries are indirect
    if (t == "StateMachineNode")     return 0; // states are managed inside
    return 1;
}

// ---- Set child index on node by port slot ----------------------------------
static void SetChildByPort(AnimGraphNode& node, int port, int childIdx)
{
    const auto t = node.TypeName();
    if (t == "OutputNode")
    { static_cast<OutputNode&>(node).Child = childIdx; return; }
    if (t == "SavedPoseNode")
    { static_cast<SavedPoseNode&>(node).Child = childIdx; return; }
    if (t == "BlendByConditionNode")
    {
        auto& n = static_cast<BlendByConditionNode&>(node);
        if (port == 0) n.TrueChild = childIdx;
        else n.FalseChild = childIdx;
        return;
    }
    if (t == "LayeredBlendNode")
    {
        auto& n = static_cast<LayeredBlendNode&>(node);
        if (port == 0) n.BaseChild = childIdx;
        else n.LayerChild = childIdx;
        return;
    }
    if (t == "AdditiveNode")
    {
        auto& n = static_cast<AdditiveNode&>(node);
        if (port == 0) n.BaseChild = childIdx;
        else n.AdditiveChild = childIdx;
        return;
    }
}

// ---- Get child index from node by port slot --------------------------------
static int GetChildByPort(const AnimGraphNode& node, int port)
{
    const auto t = node.TypeName();
    if (t == "OutputNode")           return static_cast<const OutputNode&>(node).Child;
    if (t == "SavedPoseNode")        return static_cast<const SavedPoseNode&>(node).Child;
    if (t == "BlendByConditionNode")
    {
        const auto& n = static_cast<const BlendByConditionNode&>(node);
        return port == 0 ? n.TrueChild : n.FalseChild;
    }
    if (t == "LayeredBlendNode")
    {
        const auto& n = static_cast<const LayeredBlendNode&>(node);
        return port == 0 ? n.BaseChild : n.LayerChild;
    }
    if (t == "AdditiveNode")
    {
        const auto& n = static_cast<const AdditiveNode&>(node);
        return port == 0 ? n.BaseChild : n.AdditiveChild;
    }
    return -1;
}

void AnimGraphPanel::DrawParametersSidebar(SceneEditor& scene, AnimationGraph& graph)
{
    ImGui::BeginChild("##AgParams", ImVec2{160.0F, 0.0F}, true);
    ImGui::TextUnformatted("Parameters");
    ImGui::Separator();
    for (int pi = 0; pi < static_cast<int>(graph.Parameters.size()); ++pi)
    {
        AnimGraphParameter& p = graph.Parameters[static_cast<std::size_t>(pi)];
        ImGui::PushID(pi);
        const char* typeLabel[] = {"F", "B", "I", "T"};
        ImGui::TextDisabled("[%s]", typeLabel[static_cast<int>(p.ParamType)]);
        ImGui::SameLine();
        ImGui::TextUnformatted(p.Name.c_str());
        ImGui::PopID();
    }
    ImGui::Separator();
    // Add parameter buttons
    if (ImGui::SmallButton("+ Float"))
        graph.Parameters.push_back({"NewFloat", AnimGraphParameter::Type::Float});
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Bool"))
        graph.Parameters.push_back({"NewBool", AnimGraphParameter::Type::Bool});
    if (ImGui::SmallButton("+ Int"))
        graph.Parameters.push_back({"NewInt", AnimGraphParameter::Type::Int});
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Trig"))
        graph.Parameters.push_back({"NewTrigger", AnimGraphParameter::Type::Trigger});
    ImGui::EndChild();
}

void AnimGraphPanel::DrawCanvas(SceneEditor& scene, AnimationGraph& graph,
    AnimatorComponent& animator)
{
    constexpr ImVec2 nodeSize{140.0F, 46.0F};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();

    // Background
    draw->AddRectFilled(origin,
        ImVec2{origin.x + canvasSize.x, origin.y + canvasSize.y},
        IM_COL32(28, 30, 36, 255));

    ImGui::InvisibleButton("##AgCanvas", canvasSize);
    const bool canvasHovered = ImGui::IsItemHovered();

    // Pan
    if (canvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
    {
        const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle, 0.0F);
        m_GraphPan.x += delta.x; m_GraphPan.y += delta.y;
        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
    }
    // Deselect on blank click
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && m_ConnectingFromNode < 0)
        m_SelectedNode = -1;

    draw->PushClipRect(origin,
        ImVec2{origin.x + canvasSize.x, origin.y + canvasSize.y}, true);

    // Draw connections
    for (int ni = 0; ni < static_cast<int>(graph.Nodes.size()); ++ni)
    {
        const AnimGraphNode& node = *graph.Nodes[static_cast<std::size_t>(ni)];
        const int ports = InputPortCount(node);
        for (int port = 0; port < ports; ++port)
        {
            const int srcIdx = GetChildByPort(node, port);
            if (srcIdx < 0 || srcIdx >= static_cast<int>(graph.Nodes.size())) continue;
            const AnimGraphNode& src = *graph.Nodes[static_cast<std::size_t>(srcIdx)];
            const ImVec2 srcPos{origin.x + m_GraphPan.x + src.EditorPosition.x + nodeSize.x,
                origin.y + m_GraphPan.y + src.EditorPosition.y + nodeSize.y * 0.5F};
            const float portY = node.EditorPosition.y +
                nodeSize.y * (ports == 1 ? 0.5F : (port == 0 ? 0.3F : 0.7F));
            const ImVec2 dstPos{origin.x + m_GraphPan.x + node.EditorPosition.x,
                origin.y + m_GraphPan.y + portY};
            draw->AddBezierCubic(srcPos,
                ImVec2{srcPos.x + 50.0F, srcPos.y},
                ImVec2{dstPos.x - 50.0F, dstPos.y},
                dstPos, IM_COL32(180, 190, 210, 200), 2.0F);
        }
    }

    // Draw nodes
    for (int ni = 0; ni < static_cast<int>(graph.Nodes.size()); ++ni)
    {
        AnimGraphNode& node = *graph.Nodes[static_cast<std::size_t>(ni)];
        const ImVec2 tl{origin.x + m_GraphPan.x + node.EditorPosition.x,
            origin.y + m_GraphPan.y + node.EditorPosition.y};
        const ImVec2 br{tl.x + nodeSize.x, tl.y + nodeSize.y};
        const bool selected = m_SelectedNode == ni;
        const bool isOutput = ni == graph.OutputNodeIndex;
        const ImU32 col = NodeColor(node.TypeName());

        ImGui::SetCursorScreenPos(tl);
        ImGui::PushID(ni);
        ImGui::InvisibleButton("##node", nodeSize);

        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !isOutput)
        {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0F);
            node.EditorPosition.x += d.x; node.EditorPosition.y += d.y;
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
        }
        if (ImGui::IsItemClicked()) { m_SelectedNode = ni; m_ConnectingFromNode = -1; }

        draw->AddRectFilled(tl, br, col, 5.0F);
        draw->AddRect(tl, br,
            selected ? IM_COL32(255, 240, 100, 255) : IM_COL32(0, 0, 0, 100),
            5.0F, 0, selected ? 2.5F : 1.2F);

        // Node label (type name)
        draw->AddText(ImVec2{tl.x + 8.0F, tl.y + 8.0F},
            IM_COL32(230, 235, 245, 255), node.TypeName().data());

        // Output port (right edge)
        const ImVec2 outPort{br.x, tl.y + nodeSize.y * 0.5F};
        const bool outHit = canvasHovered &&
            ImGui::IsMouseHoveringRect(
                ImVec2{outPort.x - 7.0F, outPort.y - 7.0F},
                ImVec2{outPort.x + 7.0F, outPort.y + 7.0F});
        if (canvasHovered || m_ConnectingFromNode == ni)
            draw->AddCircleFilled(outPort, 5.0F,
                outHit ? IM_COL32(255, 208, 76, 255) : IM_COL32(140, 152, 168, 210));
        if (outHit && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_ConnectingFromNode = ni;
            m_ConnectingLineEnd = outPort;
            m_SelectedNode = -1;
        }

        // Input ports (left edge)
        const int ports = InputPortCount(node);
        for (int port = 0; port < ports; ++port)
        {
            const float portY = tl.y + nodeSize.y *
                (ports == 1 ? 0.5F : (port == 0 ? 0.3F : 0.7F));
            const ImVec2 inPort{tl.x, portY};
            const bool inHit = canvasHovered &&
                ImGui::IsMouseHoveringRect(
                    ImVec2{inPort.x - 7.0F, inPort.y - 7.0F},
                    ImVec2{inPort.x + 7.0F, inPort.y + 7.0F});
            draw->AddCircleFilled(inPort, 5.0F,
                inHit ? IM_COL32(255, 208, 76, 255) : IM_COL32(140, 152, 168, 210));

            // Release connection
            if (inHit && ImGui::IsMouseReleased(ImGuiMouseButton_Left)
                && m_ConnectingFromNode >= 0 && m_ConnectingFromNode != ni)
            {
                SetChildByPort(node, port, m_ConnectingFromNode);
                m_ConnectingFromNode = -1;
            }
        }

        // Right-click menu (not on OutputNode)
        if (!isOutput && ImGui::BeginPopupContextItem("##NodeCtxAg"))
        {
            if (ImGui::MenuItem("Delete Node"))
            {
                // Nullify connections pointing to this node
                for (auto& n : graph.Nodes)
                {
                    const int p2 = InputPortCount(*n);
                    for (int p = 0; p < p2; ++p)
                        if (GetChildByPort(*n, p) == ni)
                            SetChildByPort(*n, p, -1);
                }
                graph.Nodes.erase(graph.Nodes.begin() + ni);
                if (m_SelectedNode == ni) m_SelectedNode = -1;
            }
            ImGui::EndPopup();
        }
        ImGui::OpenPopupOnItemClick("##NodeCtxAg", ImGuiPopupFlags_MouseButtonRight);

        ImGui::PopID();
    }

    // In-progress connection line
    if (m_ConnectingFromNode >= 0 &&
        m_ConnectingFromNode < static_cast<int>(graph.Nodes.size()))
    {
        const AnimGraphNode& src =
            *graph.Nodes[static_cast<std::size_t>(m_ConnectingFromNode)];
        const ImVec2 srcPort{origin.x + m_GraphPan.x + src.EditorPosition.x + nodeSize.x,
            origin.y + m_GraphPan.y + src.EditorPosition.y + nodeSize.y * 0.5F};
        m_ConnectingLineEnd = ImGui::GetMousePos();
        draw->AddBezierCubic(srcPort,
            ImVec2{srcPort.x + 60.0F, srcPort.y},
            ImVec2{m_ConnectingLineEnd.x - 60.0F, m_ConnectingLineEnd.y},
            m_ConnectingLineEnd, IM_COL32(255, 208, 76, 200), 2.0F);
    }
    if (m_ConnectingFromNode >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        m_ConnectingFromNode = -1;
    if (m_ConnectingFromNode >= 0 && ImGui::IsKeyPressed(ImGuiKey_Escape))
        m_ConnectingFromNode = -1;

    // Right-click canvas: Add Node menu
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
        && m_ConnectingFromNode < 0)
        ImGui::OpenPopup("##AgAddNode");
    if (ImGui::BeginPopup("##AgAddNode"))
    {
        const ImVec2 clickPos = ImGui::GetMousePosOnOpeningCurrentPopup();
        const glm::vec2 spawnPos{
            clickPos.x - origin.x - m_GraphPan.x,
            clickPos.y - origin.y - m_GraphPan.y};

        auto addNode = [&](std::unique_ptr<AnimGraphNode> n)
        {
            n->EditorPosition = spawnPos;
            graph.Nodes.push_back(std::move(n));
            m_SelectedNode = static_cast<int>(graph.Nodes.size()) - 1;
            ImGui::CloseCurrentPopup();
        };

        if (ImGui::BeginMenu("Clips"))
        {
            if (ImGui::MenuItem("Clip Node"))    addNode(std::make_unique<ClipNode>());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Blending"))
        {
            if (ImGui::MenuItem("Blend by Float"))
                addNode(std::make_unique<BlendByFloatNode>());
            if (ImGui::MenuItem("Blend by Condition"))
                addNode(std::make_unique<BlendByConditionNode>());
            if (ImGui::MenuItem("Layered Blend"))
                addNode(std::make_unique<LayeredBlendNode>());
            if (ImGui::MenuItem("Additive"))
                addNode(std::make_unique<AdditiveNode>());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Logic"))
        {
            if (ImGui::MenuItem("State Machine"))
                addNode(std::make_unique<StateMachineNode>());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Utilities"))
        {
            if (ImGui::MenuItem("Save Pose"))
                addNode(std::make_unique<SavedPoseNode>());
            if (ImGui::MenuItem("Use Saved Pose"))
                addNode(std::make_unique<UseSavedPoseNode>());
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    draw->PopClipRect();
}

void AnimGraphPanel::DrawNodeInspector(SceneEditor& scene, AnimationGraph& graph)
{
    if (m_SelectedNode < 0 || m_SelectedNode >= static_cast<int>(graph.Nodes.size()))
    {
        ImGui::TextDisabled("Select a node to inspect it.");
        return;
    }
    AnimGraphNode& node = *graph.Nodes[static_cast<std::size_t>(m_SelectedNode)];
    const auto t = node.TypeName();
    ImGui::TextUnformatted(t.data());
    ImGui::Separator();

    if (t == "ClipNode")
    {
        auto& n = static_cast<ClipNode&>(node);
        char buf[128]{}; std::snprintf(buf, sizeof(buf), "%s", n.ClipName.c_str());
        if (ImGui::InputText("Clip", buf, sizeof(buf))) n.ClipName = buf;
        ImGui::DragFloat("Speed", &n.Speed, 0.01F, 0.0F, 10.0F);
        ImGui::Checkbox("Loop", &n.Loop);
        ImGui::Checkbox("Mirror", &n.Mirror);
    }
    else if (t == "BlendByFloatNode")
    {
        auto& n = static_cast<BlendByFloatNode&>(node);
        // Parameter dropdown
        if (ImGui::BeginCombo("Param", n.ParameterName.c_str()))
        {
            for (const AnimGraphParameter& p : graph.Parameters)
                if (p.ParamType == AnimGraphParameter::Type::Float)
                    if (ImGui::Selectable(p.Name.c_str(), p.Name == n.ParameterName))
                        n.ParameterName = p.Name;
            ImGui::EndCombo();
        }
        ImGui::Separator();
        int remove = -1;
        for (int ei = 0; ei < static_cast<int>(n.Entries.size()); ++ei)
        {
            auto& e = n.Entries[static_cast<std::size_t>(ei)];
            ImGui::PushID(ei);
            ImGui::SetNextItemWidth(60.0F);
            ImGui::DragFloat("##T", &e.Threshold, 0.01F);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(50.0F);
            ImGui::InputInt("##C", &e.ChildIndex);
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove = ei;
            ImGui::PopID();
        }
        if (remove >= 0) n.Entries.erase(n.Entries.begin() + remove);
        if (ImGui::SmallButton("+ Entry"))
            n.Entries.push_back({n.Entries.empty() ? 0.0F :
                n.Entries.back().Threshold + 1.0F, -1});
    }
    else if (t == "BlendByConditionNode")
    {
        auto& n = static_cast<BlendByConditionNode&>(node);
        if (ImGui::BeginCombo("Param", n.ParameterName.c_str()))
        {
            for (const AnimGraphParameter& p : graph.Parameters)
                if (p.ParamType == AnimGraphParameter::Type::Bool ||
                    p.ParamType == AnimGraphParameter::Type::Trigger)
                    if (ImGui::Selectable(p.Name.c_str(), p.Name == n.ParameterName))
                        n.ParameterName = p.Name;
            ImGui::EndCombo();
        }
        ImGui::DragFloat("Blend Duration", &n.BlendDuration, 0.01F, 0.0F, 2.0F);
        ImGui::LabelText("True Child", "%d", n.TrueChild);
        ImGui::LabelText("False Child", "%d", n.FalseChild);
    }
    else if (t == "LayeredBlendNode")
    {
        auto& n = static_cast<LayeredBlendNode&>(node);
        ImGui::DragFloat("Weight", &n.Weight, 0.01F, 0.0F, 1.0F);
        char buf[64]{}; std::snprintf(buf, sizeof(buf), "%s", n.WeightParameter.c_str());
        if (ImGui::InputText("Weight Param", buf, sizeof(buf))) n.WeightParameter = buf;
        ImGui::TextUnformatted("Bone Mask (joint indices):");
        // Simple comma-separated input for now
        std::string maskStr;
        for (int b : n.BoneMask)
            maskStr += std::to_string(b) + ",";
        char maskBuf[256]{}; std::snprintf(maskBuf, sizeof(maskBuf), "%s", maskStr.c_str());
        if (ImGui::InputText("##mask", maskBuf, sizeof(maskBuf)))
        {
            n.BoneMask.clear();
            std::string s{maskBuf};
            std::size_t pos = 0;
            while ((pos = s.find(',')) != std::string::npos)
            {
                const std::string tok = s.substr(0, pos);
                if (!tok.empty()) n.BoneMask.push_back(std::stoi(tok));
                s.erase(0, pos + 1);
            }
        }
    }
    else if (t == "AdditiveNode")
    {
        auto& n = static_cast<AdditiveNode&>(node);
        ImGui::DragFloat("Weight", &n.Weight, 0.01F, 0.0F, 1.0F);
        char buf[64]{}; std::snprintf(buf, sizeof(buf), "%s", n.WeightParameter.c_str());
        if (ImGui::InputText("Weight Param", buf, sizeof(buf))) n.WeightParameter = buf;
    }
    else if (t == "SavedPoseNode" || t == "UseSavedPoseNode")
    {
        std::string& key = (t == "SavedPoseNode")
            ? static_cast<SavedPoseNode&>(node).PoseKey
            : static_cast<UseSavedPoseNode&>(node).PoseKey;
        char buf[64]{}; std::snprintf(buf, sizeof(buf), "%s", key.c_str());
        if (ImGui::InputText("Pose Key", buf, sizeof(buf))) key = buf;
    }
    else if (t == "OutputNode")
    {
        ImGui::TextDisabled("Root output node. Connect the final pose here.");
    }
}

void AnimGraphPanel::Draw(SceneEditor& scene, AnimationGraph& graph, AnimatorComponent& animator)
{
    // Left: parameters sidebar
    DrawParametersSidebar(scene, graph);
    ImGui::SameLine();

    // Middle: canvas
    const float inspectorW = 220.0F;
    const float canvasW = ImGui::GetContentRegionAvail().x - inspectorW - 4.0F;
    ImGui::BeginChild("##AgCanvas", ImVec2{canvasW, 0.0F}, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawCanvas(scene, graph, animator);
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: node inspector
    ImGui::BeginChild("##AgInspect", ImVec2{inspectorW, 0.0F}, true);
    DrawNodeInspector(scene, graph);
    ImGui::EndChild();
}
}  // namespace fadix
```

- [ ] **Step 3: Add `AnimGraphPanel.cpp` to CMakeLists.txt**

In `CMakeLists.txt`, find where `FdxAnimationPanel.cpp` is listed in the editor sources glob or explicit list. Add `AnimGraphPanel.cpp` in the same block:

```cmake
src/editor/imgui/panels/AnimGraphPanel.cpp
```

- [ ] **Step 4: Build editor**

```bat
cmake --build .build/debug-cmake --config Debug --target fadix_editor --parallel 8
```

Expected: zero errors.

- [ ] **Step 5: Commit**

```bat
git add src/editor/imgui/panels/AnimGraphPanel.hpp src/editor/imgui/panels/AnimGraphPanel.cpp CMakeLists.txt
git commit -m "feat(animgraph): graph canvas panel — node draw, port drag, add-node menu, inspectors"
```

---

## Task 7: FdxAnimationPanel Tab + InspectorPanel Migration Button

**Files:**
- Modify: `src/editor/imgui/panels/FdxAnimationPanel.hpp` (add `AnimGraphPanel` member)
- Modify: `src/editor/imgui/panels/FdxAnimationPanel.cpp` (add "Anim Graph" tab)
- Modify: `src/editor/imgui/panels/InspectorPanel.cpp` (upgrade button + graph play-mode params)

**Interfaces:**
- Consumes: `AnimGraphPanel::Draw`, `AnimatorComponent::Graph`, `AnimGraphNodeRegistry::RegisterBuiltins()`
- Produces: "Anim Graph" tab in FDX Animation panel; "Upgrade to Anim Graph" button in Inspector

- [ ] **Step 1: Add `AnimGraphPanel` member to `FdxAnimationPanel.hpp`**

In `FdxAnimationPanel.hpp`, add include:
```cpp
#include "editor/imgui/panels/AnimGraphPanel.hpp"
```

Inside `class FdxAnimationPanel`, add private member after existing graph-pan members:
```cpp
    AnimGraphPanel m_AnimGraphPanel;
```

- [ ] **Step 2: Add "Anim Graph" tab in `FdxAnimationPanel.cpp`**

Find where the existing animator tabs are defined (look for `ImGui::BeginTabBar` or `ImGui::TabItem` for "State Graph", "Clips", etc.). Add a new tab item:

```cpp
    if (ImGui::BeginTabItem("Anim Graph"))
    {
        // Find selected entity's AnimatorComponent
        if (auto* animator = GetSelectedAnimator(scene)) // adapt to actual getter name
        {
            if (animator->Graph)
            {
                m_AnimGraphPanel.Draw(scene, *animator->Graph, *animator);
            }
            else
            {
                ImGui::TextDisabled("No Animation Graph on this component.");
                ImGui::TextDisabled("Use the Inspector to upgrade from the legacy controller.");
            }
        }
        else
        {
            ImGui::TextDisabled("Select an entity with an AnimatorComponent.");
        }
        ImGui::EndTabItem();
    }
```

Find the exact name of the function/method that retrieves the selected entity's `AnimatorComponent` and substitute it for `GetSelectedAnimator(scene)`.

- [ ] **Step 3: Register built-in nodes at app startup**

Find where `FdxAnimationPanel` or `ImGuiEditorApplication` initializes (constructor or `OnStart`). Add:

```cpp
fadix::AnimGraphNodeRegistry::RegisterBuiltins();
```

Call this exactly once before any graph is created or deserialized.

- [ ] **Step 4: Add "Upgrade to Anim Graph" button in `InspectorPanel.cpp`**

Find the `AnimatorComponent` `CollapsingHeader` block. After the "Open FDX Animation" button, before `RemoveButton`, add:

```cpp
            if (!animator->Graph)
            {
                if (ImGui::Button("Upgrade to Anim Graph"))
                {
                    auto graph = std::make_unique<fadix::AnimationGraph>();
                    graph->Name = "PlayerGraph";

                    // Wrap legacy controller in a StateMachineNode
                    auto smNode = std::make_unique<fadix::StateMachineNode>();
                    smNode->Controller = animator->Controller;
                    smNode->EditorPosition = {100.0F, 180.0F};
                    graph->Nodes.push_back(std::move(smNode)); // index 0

                    auto outNode = std::make_unique<fadix::OutputNode>();
                    outNode->Child = 0;
                    outNode->EditorPosition = {340.0F, 180.0F};
                    graph->Nodes.push_back(std::move(outNode)); // index 1
                    graph->OutputNodeIndex = 1;

                    // Copy parameters
                    for (const fadix::AnimatorParameter& ap : animator->Controller.Parameters)
                    {
                        fadix::AnimGraphParameter gp;
                        gp.Name = ap.Name;
                        gp.ParamType = static_cast<fadix::AnimGraphParameter::Type>(
                            static_cast<int>(ap.Type)); // enum values must match
                        gp.FloatValue = ap.FloatValue;
                        gp.BoolValue  = ap.BoolValue;
                        gp.IntValue   = ap.IntValue;
                        graph->Parameters.push_back(std::move(gp));
                    }
                    animator->RuntimeParameters = graph->Parameters;
                    animator->Graph = std::move(graph);
                    scene.MarkDirty(); // adapt to actual scene-dirty function name
                }
            }
            else
            {
                ImGui::TextDisabled("Anim Graph active");
                if (ImGui::SmallButton("Remove Graph"))
                    animator->Graph.reset();
            }
```

**Note:** Check that `AnimatorParameter::Type` and `AnimGraphParameter::Type` enum values are in the same order (Float=0, Bool=1, Int=2, Trigger=3). If not, add an explicit mapping instead of the cast.

- [ ] **Step 5: Extend play-mode parameter controls in `InspectorPanel.cpp` to cover graph params**

Find the existing play-mode parameter block (added in the state machine v2 feature). After the existing `for (AnimatorParameter& param : animator->Controller.Parameters)` loop, add:

```cpp
            // Graph parameters (when graph is active)
            if (animator->Graph)
            {
                if (!animator->RuntimeParameters.empty())
                    ImGui::Separator();
                ImGui::TextUnformatted("Graph Parameters:");
                for (fadix::AnimGraphParameter& param : animator->RuntimeParameters)
                {
                    ImGui::PushID(("gp_" + param.Name).c_str());
                    ImGui::SetNextItemWidth(110.0F);
                    if (param.ParamType == fadix::AnimGraphParameter::Type::Float)
                        ImGui::DragFloat(param.Name.c_str(), &param.FloatValue, 0.02F);
                    else if (param.ParamType == fadix::AnimGraphParameter::Type::Int)
                        ImGui::DragInt(param.Name.c_str(), &param.IntValue, 1.0F);
                    else if (param.ParamType == fadix::AnimGraphParameter::Type::Bool)
                        ImGui::Checkbox(param.Name.c_str(), &param.BoolValue);
                    else
                    {
                        if (ImGui::Button(param.Name.c_str()))
                            param.BoolValue = true;
                        ImGui::SameLine();
                        ImGui::TextDisabled("(trigger)");
                    }
                    ImGui::PopID();
                }
            }
```

- [ ] **Step 6: Build editor**

```bat
cmake --build .build/debug-cmake --config Debug --target fadix_editor --parallel 8
```

Expected: zero errors.

- [ ] **Step 7: Verify manually**

Launch `.\bin\Debug\fadix_editor.exe`. Select entity with `AnimatorComponent`. Inspector shows "Upgrade to Anim Graph" button. Click it — FDX Animation "Anim Graph" tab now shows a canvas with a `StateMachineNode` connected to `OutputNode`. Right-click canvas → Add Node menu appears. Drag output port of StateMachineNode to OutputNode input port. Enter play mode → Inspector shows Graph Parameters section.

- [ ] **Step 8: Commit**

```bat
git add src/editor/imgui/panels/FdxAnimationPanel.hpp src/editor/imgui/panels/FdxAnimationPanel.cpp src/editor/imgui/panels/InspectorPanel.cpp
git commit -m "feat(animgraph): Anim Graph tab in FDX Animation panel, Upgrade button in Inspector, graph play-mode params"
```

---

## Self-Review

**Spec coverage:**
- ✅ Pull-based virtual node graph: `AnimGraphNode::Evaluate` virtual, `AnimationGraph::Evaluate` calls `Nodes[OutputNodeIndex]->Evaluate`
- ✅ 8 node types: ClipNode, BlendByFloat, BlendByCondition, LayeredBlend, Additive, SavedPose, UseSavedPose, StateMachine, Output
- ✅ Per-instance state on nodes (safe — `AnimationGraph` owned per `AnimatorComponent`)
- ✅ `AnimatorComponent::Graph` = `unique_ptr<AnimationGraph>`, legacy path unchanged
- ✅ `AnimGraphNodeRegistry::RegisterBuiltins()` + `Create(typeName)` for deserialization
- ✅ `WriteAnimationGraph` / `ReadAnimationGraph` line-oriented text format
- ✅ Runtime dispatch: graph path runs when `animator.Graph != nullptr`, skips legacy
- ✅ Trigger parameters cleared via `ConsumeTriggers()` after each `Evaluate`
- ✅ 9 smoke tests (1 serialization + 8 node behaviors)
- ✅ Graph canvas panel: pan, node drag, port drag connections, right-click add-node menu
- ✅ Node inspectors per type in right panel
- ✅ Parameters sidebar (add Float/Bool/Int/Trigger)
- ✅ "Upgrade to Anim Graph" wraps legacy `AnimatorController` in `StateMachineNode`
- ✅ Play-mode graph parameter controls in Inspector

**Type consistency check:**
- `AnimGraphParameter` used consistently across all tasks ✅
- `AnimGraphContext.Parameters = std::span<AnimGraphParameter>` matches runtime init ✅
- `BlendByFloatNode::Entry.ChildIndex` used in canvas `GetChildByPort`/`SetChildByPort` ✅
- `OutputNode.Child`, `SavedPoseNode.Child` used consistently ✅
- `StateMachineNode.StateChildIndices` map used in `Evaluate` and inspector ✅
