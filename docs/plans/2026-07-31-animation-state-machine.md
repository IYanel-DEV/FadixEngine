# Animation State Machine v2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add 1D blend trees, Any-State transitions, port-drag transition creation, and play-mode live parameter controls to the existing animator system.

**Architecture:** The existing `AnimatorController` / `AnimatorState` / `AnimatorTransition` data model is extended minimally — one new struct and three new fields on `AnimatorState`. Runtime helpers in `AnimationRuntime.hpp` grow a `ResolveBlendTree1D` function and a second-pass Any-State check in `FindAnimatorTransition`. All UI work lives in `FdxAnimationPanel.cpp` and `InspectorPanel.cpp` (`.cpp`-only, no cascade after the header work is done).

**Tech Stack:** C++20, Dear ImGui (docking), EnTT ECS, glm, SDL3. No new dependencies.

## Global Constraints

- C++20, MSVC `/W4 /permissive-`. No new files unless a task says "Create".
- Build Debug only: `cmake --build .build\debug-cmake --config Debug --target <target> --parallel 8`
- Run smoke: `.\bin\Debug\fadix_animation_smoke.exe`
- Run editor: `.\bin\Debug\fadix_editor.exe`
- Every commit message: imperative, under 72 chars.
- Do NOT edit `src/generated/EmbeddedAssets.hpp`.
- `"Any State"` is a reserved string — never a valid user state name. Enforce in the UI (disable adding a state with that exact name).

---

## File Map

| File | Role in this feature |
|------|----------------------|
| `src/engine/animation/AnimationClip.hpp` | Add `BlendTree1DEntry`; extend `AnimatorState` |
| `src/engine/animation/AnimatorControllerIO.hpp` | Version+blend-tree serialization |
| `src/runtime/AnimationRuntime.hpp` | `BlendTree1DResult`, `ResolveBlendTree1D`, Any-State pass, blend tree tick paths |
| `src/editor/imgui/panels/FdxAnimationPanel.hpp` | Two new panel members for port-drag |
| `src/editor/imgui/panels/FdxAnimationPanel.cpp` | Port-drag, right-click, Any-State node, blend tree inspector |
| `src/editor/imgui/panels/InspectorPanel.cpp` | Play-mode live parameter controls |
| `tools/AnimationSmoke.cpp` | Blend tree + Any-State smoke tests |

---

## Task 1: Data Model + Serializer

**Files:**
- Modify: `src/engine/animation/AnimationClip.hpp`
- Modify: `src/engine/animation/AnimatorControllerIO.hpp`

**Interfaces produced:**
- `struct BlendTree1DEntry { std::string ClipName; float Threshold{0.0F}; };`
- `AnimatorState::UseBlendTree` (bool), `AnimatorState::BlendParameter` (string), `AnimatorState::BlendEntries` (vector)
- File format version 2 (version 1 = original, missing blend tree fields = defaults)

- [ ] **Step 1: Add `BlendTree1DEntry` and extend `AnimatorState`**

In `src/engine/animation/AnimationClip.hpp`, find the existing `struct AnimatorState` (currently has `Name`, `ClipName`, `Position`). Add the new struct **before** `AnimatorState`, then add three fields to `AnimatorState`:

```cpp
// Insert before AnimatorState:
struct BlendTree1DEntry
{
    std::string ClipName;
    float Threshold{0.0F};
};

// Extend AnimatorState — add after `glm::vec2 Position{40.0F, 40.0F};`:
    bool UseBlendTree{false};
    std::string BlendParameter;
    std::vector<BlendTree1DEntry> BlendEntries;
```

`ClipName` is kept and still used when `UseBlendTree == false`. No existing code breaks.

- [ ] **Step 2: Add version+blend-tree fields to the serializer**

In `src/engine/animation/AnimatorControllerIO.hpp`, replace `WriteAnimatorControllerData` and `ReadAnimatorControllerData` with the versioned variants below.

The write function prepends `2` (version int) before the quoted name. The read function tries to read an int first; if it fails (v1 file starts with a quoted name), it clears the stream error and rewinds.

```cpp
inline void WriteAnimatorControllerData(std::ostream& out, const AnimatorController& controller)
{
    out << 2 << ' '; // format version
    out << std::quoted(controller.Name) << ' ' << std::quoted(controller.EntryState) << ' '
        << controller.Parameters.size() << ' ' << controller.States.size() << ' '
        << controller.Transitions.size() << ' ';
    for (const AnimatorParameter& parameter : controller.Parameters)
    {
        out << std::quoted(parameter.Name) << ' ' << static_cast<int>(parameter.Type) << ' '
            << parameter.BoolValue << ' ' << parameter.FloatValue << ' ' << parameter.IntValue
            << ' ';
    }
    for (const AnimatorState& state : controller.States)
    {
        out << std::quoted(state.Name) << ' ' << std::quoted(state.ClipName) << ' '
            << state.Position.x << ' ' << state.Position.y << ' '
            << state.UseBlendTree << ' ' << std::quoted(state.BlendParameter) << ' '
            << state.BlendEntries.size() << ' ';
        for (const BlendTree1DEntry& entry : state.BlendEntries)
        {
            out << std::quoted(entry.ClipName) << ' ' << entry.Threshold << ' ';
        }
    }
    for (const AnimatorTransition& transition : controller.Transitions)
    {
        out << std::quoted(transition.From) << ' ' << std::quoted(transition.To) << ' '
            << transition.Duration << ' ' << transition.HasExitTime << ' ' << transition.ExitTime
            << ' ' << transition.Conditions.size() << ' ';
        for (const AnimatorCondition& condition : transition.Conditions)
        {
            out << std::quoted(condition.Parameter) << ' '
                << static_cast<int>(condition.Comparison) << ' ' << condition.Threshold << ' ';
        }
    }
}

inline bool ReadAnimatorControllerData(std::istream& row, AnimatorController& controller)
{
    // Detect version: v2+ starts with an unquoted int; v1 starts with a quoted name.
    int version = 1;
    const std::streampos startPos = row.tellg();
    if (!(row >> version) || version < 1 || version > 100)
    {
        row.clear();
        row.seekg(startPos);
        version = 1;
    }

    std::size_t parameterCount = 0;
    std::size_t stateCount = 0;
    std::size_t transitionCount = 0;
    row >> std::quoted(controller.Name) >> std::quoted(controller.EntryState) >> parameterCount >>
        stateCount >> transitionCount;
    controller.Parameters.clear();
    controller.States.clear();
    controller.Transitions.clear();
    for (std::size_t i = 0; i < parameterCount && row; ++i)
    {
        AnimatorParameter parameter;
        int type = 0;
        row >> std::quoted(parameter.Name) >> type >> parameter.BoolValue >> parameter.FloatValue >>
            parameter.IntValue;
        parameter.Type = static_cast<AnimatorParameterType>(std::clamp(type, 0, 3));
        controller.Parameters.push_back(std::move(parameter));
    }
    for (std::size_t i = 0; i < stateCount && row; ++i)
    {
        AnimatorState state;
        row >> std::quoted(state.Name) >> std::quoted(state.ClipName) >> state.Position.x >>
            state.Position.y;
        if (version >= 2 && row)
        {
            int useBlendTree = 0;
            std::size_t entryCount = 0;
            row >> useBlendTree >> std::quoted(state.BlendParameter) >> entryCount;
            state.UseBlendTree = (useBlendTree != 0);
            for (std::size_t e = 0; e < entryCount && row; ++e)
            {
                BlendTree1DEntry entry;
                row >> std::quoted(entry.ClipName) >> entry.Threshold;
                state.BlendEntries.push_back(std::move(entry));
            }
        }
        controller.States.push_back(std::move(state));
    }
    for (std::size_t i = 0; i < transitionCount && row; ++i)
    {
        AnimatorTransition transition;
        std::size_t conditionCount = 0;
        row >> std::quoted(transition.From) >> std::quoted(transition.To) >> transition.Duration >>
            transition.HasExitTime >> transition.ExitTime >> conditionCount;
        transition.Duration = std::max(transition.Duration, 0.0F);
        transition.ExitTime = std::clamp(transition.ExitTime, 0.0F, 1.0F);
        for (std::size_t c = 0; c < conditionCount && row; ++c)
        {
            AnimatorCondition condition;
            int comparison = 0;
            row >> std::quoted(condition.Parameter) >> comparison >> condition.Threshold;
            condition.Comparison =
                static_cast<AnimatorComparison>(std::clamp(comparison, 0, 3));
            transition.Conditions.push_back(std::move(condition));
        }
        controller.Transitions.push_back(std::move(transition));
    }
    return static_cast<bool>(row);
}
```

- [ ] **Step 3: Build smoke to verify headers compile**

```bat
cmake --build .build\debug-cmake --config Debug --target fadix_animation_smoke --parallel 8
```

Expected: build succeeds. The smoke binary will not test the new code yet (that's Task 3).

- [ ] **Step 4: Commit**

```bat
git add src/engine/animation/AnimationClip.hpp src/engine/animation/AnimatorControllerIO.hpp
git commit -m "feat(anim): add BlendTree1DEntry, extend AnimatorState, version serializer"
```

---

## Task 2: Runtime — BlendTree1D + Any-State

**Files:**
- Modify: `src/runtime/AnimationRuntime.hpp`

**Interfaces produced:**
- `struct BlendTree1DResult { std::string ClipA; std::string ClipB; float Weight{0.0F}; };`
- `BlendTree1DResult ResolveBlendTree1D(std::vector<BlendTree1DEntry> entries, float value);`
- `FindAnimatorTransition` now checks `"Any State"` transitions as a second pass
- `BeginAnimatorTransition` sets `ClipName` to primary blend tree entry when entering a blend tree state
- `UpdateWorldAnimations` + `UpdateTransformAnimations` sample blend tree poses when active state has `UseBlendTree == true`

- [ ] **Step 1: Add `BlendTree1DResult` and `ResolveBlendTree1D`**

In `src/runtime/AnimationRuntime.hpp`, add `#include <algorithm>` is already present. Add these two after the existing `BlendTransformPoses` function:

```cpp
struct BlendTree1DResult
{
    std::string ClipA;
    std::string ClipB; // empty = single clip, no blending needed
    float Weight{0.0F};
};

// entries is taken by value so it can be sorted locally without touching the state.
// Returns a single-clip result (ClipB empty, Weight=0) when entries has one element
// or value is exactly at an edge threshold.
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
    for (std::size_t i = 0; i + 1 < entries.size(); ++i)
    {
        const float lo = entries[i].Threshold;
        const float hi = entries[i + 1].Threshold;
        if (clamped >= lo && clamped <= hi + 1.0e-6F)
        {
            const float span = hi - lo;
            const float weight =
                span > 1.0e-6F ? (clamped - lo) / span : 0.0F;
            return {entries[i].ClipName, entries[i + 1].ClipName, weight};
        }
    }
    return {entries.back().ClipName, {}, 0.0F};
}
```

- [ ] **Step 2: Add Any-State second pass to `FindAnimatorTransition`**

Replace the existing `FindAnimatorTransition` template (keep the signature identical — only add the second loop after the first `return nullptr`):

```cpp
template <typename Animator>
[[nodiscard]] inline const AnimatorTransition* FindAnimatorTransition(
    const Animator& animator, const float clipDuration, const float lookAheadSeconds = 0.0F)
{
    if (animator.ActiveState.empty())
    {
        return nullptr;
    }
    // First pass: transitions from the active state (unchanged logic).
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
```

- [ ] **Step 3: Fix `BeginAnimatorTransition` for blend tree destinations**

In `BeginAnimatorTransition`, the line `animator.ClipName = destination.ClipName;` sets `ClipName` to empty when the destination is a blend tree state (because `ClipName` isn't used for blend tree states). Fix so crossfades have a valid source clip name:

```cpp
// Replace:
    animator.ClipName = destination.ClipName;
// With:
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
```

- [ ] **Step 4: Add skeletal blend tree pose sampler**

Add after `BlendSkeletonPoses` (before `AnimationPlayer`):

```cpp
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
```

Note: `FindAnimationClip` is defined later in the file. Move `SampleSkeletalBlendTree1D` to **after** `FindAnimationClip` — place it just before `UpdateWorldAnimations`.

- [ ] **Step 5: Wire blend tree into `UpdateWorldAnimations`**

`UpdateWorldAnimations` has this pattern (near the top of its entity loop):

```cpp
const AnimationClipAsset* clip = FindAnimationClip(*gltf, animator->ClipName);
if (clip == nullptr) { continue; }
if (const AnimatorTransition* transition = FindAnimatorTransition(*animator, clip->Duration, ...))
{ ... }
const float previousTime = animator->CurrentTime;
QueueClipEvents(*animator, *clip, previousTime, dt * animator->Speed);
SkeletonPose targetPose;
targetPose.Skeleton = gltf->Skeleton;
AnimationPlayer targetPlayer;
targetPlayer.SetClip(clip);
...
animator->CurrentTime = targetPlayer.GetTime();
```

Replace the clip-lookup + pose-sampling block with this:

```cpp
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

        // --- transition check (unchanged call signature) ---
        if (const AnimatorTransition* transition = FindAnimatorTransition(
                *animator, clip->Duration, std::abs(dt * animator->Speed)))
        {
            const AnimatorState* destination =
                FindAnimatorState(animator->Controller, transition->To);
            const AnimationClipAsset* target = destination == nullptr
                ? nullptr
                : FindAnimationClip(*gltf, destination->UseBlendTree &&
                        !destination->BlendEntries.empty()
                    ? destination->BlendEntries.front().ClipName
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
```

Leave the crossfade (from-pose) section after this unchanged.

- [ ] **Step 6: Wire blend tree into `UpdateTransformAnimations`**

Add a transform blend tree applier (add just before `UpdateTransformAnimations`):

```cpp
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
```

In `UpdateTransformAnimations`, find the apply block:

```cpp
// existing:
ApplyTransformClip(*clip, anim.CurrentTime, transform);
```

and the surrounding blend section. Replace the final apply section (after crossfade ends):

```cpp
        // After the crossfade section, where the plain clip apply was:
        else
        {
            ClearAnimationBlend(anim);
            // Check if active state is a blend tree
            const AnimatorState* activeState = FindAnimatorState(anim.Controller, anim.ActiveState);
            if (activeState != nullptr && activeState->UseBlendTree && !activeState->BlendEntries.empty())
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
```

Also update the transition check in `UpdateTransformAnimations` — replace the `FindTransformClip` call for the destination target to handle blend tree states:

```cpp
// existing: const AnimationClipAsset* target = destination == nullptr ? nullptr
//     : FindTransformClip(static_cast<const TransformAnimatorComponent&>(anim), destination->ClipName);
// replace with:
const AnimationClipAsset* target = nullptr;
if (destination != nullptr)
{
    const std::string_view targetClip = destination->UseBlendTree && !destination->BlendEntries.empty()
        ? std::string_view{destination->BlendEntries.front().ClipName}
        : std::string_view{destination->ClipName};
    target = FindTransformClip(static_cast<const TransformAnimatorComponent&>(anim), targetClip);
}
```

- [ ] **Step 7: Build smoke to verify runtime compiles**

```bat
cmake --build .build\debug-cmake --config Debug --target fadix_animation_smoke --parallel 8
```

Expected: build succeeds.

- [ ] **Step 8: Commit**

```bat
git add src/runtime/AnimationRuntime.hpp
git commit -m "feat(anim): BlendTree1D resolution, Any-State transitions, blend tree tick"
```

---

## Task 3: Smoke Tests

**Files:**
- Modify: `tools/AnimationSmoke.cpp`

- [ ] **Step 1: Locate the existing test entry point**

Open `tools/AnimationSmoke.cpp`. Find `main()`. All new tests go at the **end**, before the final `return 0;` (or `std::cout << "All tests passed\n"`).

- [ ] **Step 2: Add blend tree resolution tests**

```cpp
    // --- BlendTree1D resolution ---
    {
        std::vector<fadix::BlendTree1DEntry> entries;
        entries.push_back({"Idle", 0.0F});
        entries.push_back({"Walk", 0.5F});
        entries.push_back({"Run",  1.0F});

        // Clamp below min
        {
            auto r = fadix::ResolveBlendTree1D(entries, -1.0F);
            assert(r.ClipA == "Idle" && r.ClipB.empty() && r.Weight == 0.0F
                && "Clamp below min failed");
        }
        // At min threshold
        {
            auto r = fadix::ResolveBlendTree1D(entries, 0.0F);
            assert(!r.ClipA.empty() && "At-min failed");
        }
        // Midpoint Idle->Walk
        {
            auto r = fadix::ResolveBlendTree1D(entries, 0.25F);
            assert(r.ClipA == "Idle" && r.ClipB == "Walk"
                && std::abs(r.Weight - 0.5F) < 1.0e-5F && "Idle->Walk midpoint failed");
        }
        // Midpoint Walk->Run
        {
            auto r = fadix::ResolveBlendTree1D(entries, 0.75F);
            assert(r.ClipA == "Walk" && r.ClipB == "Run"
                && std::abs(r.Weight - 0.5F) < 1.0e-5F && "Walk->Run midpoint failed");
        }
        // Clamp above max
        {
            auto r = fadix::ResolveBlendTree1D(entries, 2.0F);
            assert(r.ClipA == "Run" && r.ClipB.empty() && "Clamp above max failed");
        }
        // Single entry
        {
            std::vector<fadix::BlendTree1DEntry> one{{"Solo", 0.5F}};
            auto r = fadix::ResolveBlendTree1D(one, 0.7F);
            assert(r.ClipA == "Solo" && r.ClipB.empty() && r.Weight == 0.0F
                && "Single-entry failed");
        }
        // Unsorted input (entries passed out of order)
        {
            std::vector<fadix::BlendTree1DEntry> unsorted{{"Run", 1.0F}, {"Idle", 0.0F}};
            auto r = fadix::ResolveBlendTree1D(unsorted, 0.5F);
            assert(r.ClipA == "Idle" && r.ClipB == "Run"
                && std::abs(r.Weight - 0.5F) < 1.0e-5F && "Unsorted input failed");
        }
        std::cout << "PASS blend tree resolution\n";
    }
```

- [ ] **Step 3: Add Any-State transition test**

```cpp
    // --- Any-State transition ---
    {
        fadix::AnimatorController controller;
        fadix::AnimatorState stateA; stateA.Name = "A"; stateA.ClipName = "ClipA";
        fadix::AnimatorState stateB; stateB.Name = "B"; stateB.ClipName = "ClipB";
        controller.States = {stateA, stateB};
        controller.EntryState = "A";

        // Normal A->B transition with unsatisfied bool condition
        fadix::AnimatorTransition normal;
        normal.From = "A"; normal.To = "B";
        fadix::AnimatorCondition cond;
        cond.Parameter = "jump";
        cond.Comparison = fadix::AnimatorComparison::Equal;
        cond.Threshold = 1.0F;
        normal.Conditions = {cond};

        // Any-State -> B transition with no conditions (always fires)
        fadix::AnimatorTransition anyTrans;
        anyTrans.From = "Any State"; anyTrans.To = "B";

        controller.Transitions = {normal, anyTrans};
        controller.Parameters.push_back({"jump", fadix::AnimatorParameterType::Bool, false});

        fadix::AnimatorComponent animator;
        animator.Controller = controller;
        animator.ActiveState = "A";
        animator.CurrentTime = 0.0F;

        // Normal transition should NOT fire (condition unsatisfied)
        // Any-State transition SHOULD fire
        const fadix::AnimatorTransition* found =
            fadix::FindAnimatorTransition(animator, 1.0F);
        assert(found != nullptr && found->From == "Any State"
            && "Any-State transition not found");

        // Any-State must not re-enter current state
        anyTrans.To = "A"; // target == active state
        controller.Transitions = {anyTrans};
        animator.Controller = controller;
        const fadix::AnimatorTransition* blocked =
            fadix::FindAnimatorTransition(animator, 1.0F);
        assert(blocked == nullptr && "Any-State must not re-enter active state");

        std::cout << "PASS Any-State transition\n";
    }
```

- [ ] **Step 4: Build and run the smoke**

```bat
cmake --build .build\debug-cmake --config Debug --target fadix_animation_smoke --parallel 8
.\bin\Debug\fadix_animation_smoke.exe
```

Expected: all existing tests pass, plus:
```
PASS blend tree resolution
PASS Any-State transition
```

Fix any assertion failures before continuing.

- [ ] **Step 5: Commit**

```bat
git add tools/AnimationSmoke.cpp
git commit -m "test(anim): smoke tests for blend tree resolution and Any-State"
```

---

## Task 4: Editor — Port-Drag Transition Creation + Right-Click Menu

**Files:**
- Modify: `src/editor/imgui/panels/FdxAnimationPanel.hpp`
- Modify: `src/editor/imgui/panels/FdxAnimationPanel.cpp`

- [ ] **Step 1: Add panel members for port-drag state**

In `FdxAnimationPanel.hpp`, inside `class FdxAnimationPanel`, add two members after the existing `m_SkeletalGraphPan` / `m_TransformGraphPan` members:

```cpp
    int m_ConnectingFromState{-1};   // -1 = idle; index into controller.States
    ImVec2 m_ConnectingLineEnd{};
```

- [ ] **Step 2: Extend `DrawAnimatorController` signature**

`DrawAnimatorController` is a `template <typename Animator>` free function. Add two parameters at the end of the signature:

```cpp
template <typename Animator>
void DrawAnimatorController(SceneEditor& scene, const char* id, Animator& animator,
    const std::vector<std::string>& clipNames, const std::filesystem::path& projectRoot,
    int& selectedState, int& selectedTransition, glm::vec2& graphPan,
    int& connectingFrom, ImVec2& connectingEnd)          // NEW
```

Update both call sites:

```cpp
// Skeletal call site (~line 2550):
DrawAnimatorController(scene, "SkeletalController", *animator, skeletalClipNames,
    projectRoot, m_SelectedAnimatorState, m_SelectedAnimatorTransition,
    m_SkeletalGraphPan, m_ConnectingFromState, m_ConnectingLineEnd);

// Transform call site (~line 1948):
DrawAnimatorController(scene, "TransformController", anim, transformClipNames,
    projectRoot, selectedState, selectedTransition, graphPan,
    connectingFrom, connectingEnd);
```

For the transform call, `connectingFrom` and `connectingEnd` are new local refs in `DrawTransformSection`. Add them to `DrawTransformSection`'s signature and pass `m_TConnectingFromState` / `m_TConnectingLineEnd` (two new panel members — add them to the header alongside `m_ConnectingFromState`).

- [ ] **Step 3: Add port circles and drag detection in the node loop**

Inside `DrawAnimatorController`, in the node-drawing loop (after `draw->AddRect(...)` for the node border and before `ImGui::PopID()`), add:

```cpp
        // Port circle on right edge — shows when canvas hovered
        const ImVec2 portPos{topLeft.x + nodeSize.x, topLeft.y + nodeSize.y * 0.5F};
        const bool portHit = canvasHovered &&
            ImGui::IsMouseHoveringRect(
                ImVec2{portPos.x - 7.0F, portPos.y - 7.0F},
                ImVec2{portPos.x + 7.0F, portPos.y + 7.0F});
        if (canvasHovered || connectingFrom == index)
        {
            const ImU32 portCol = portHit || connectingFrom == index
                ? IM_COL32(255, 208, 76, 255)
                : IM_COL32(140, 152, 168, 210);
            draw->AddCircleFilled(portPos, 5.0F, portCol);
            draw->AddCircle(portPos, 5.5F, IM_COL32(220, 228, 238, 180), 12, 1.2F);
        }
        if (portHit && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            connectingFrom = index;
            connectingEnd = portPos;
            selectedState = -1;
            selectedTransition = -1;
        }
```

- [ ] **Step 4: Draw in-progress connection line**

After the node loop (after `draw->PopClipRect()`), add:

```cpp
    // Draw in-progress transition line
    if (connectingFrom >= 0 && connectingFrom < static_cast<int>(controller.States.size()))
    {
        const AnimatorState& srcState =
            controller.States[static_cast<std::size_t>(connectingFrom)];
        const ImVec2 srcPort{origin.x + graphPan.x + srcState.Position.x + nodeSize.x,
            origin.y + graphPan.y + srcState.Position.y + nodeSize.y * 0.5F};
        connectingEnd = ImGui::GetMousePos();
        draw->AddBezierCubic(srcPort,
            ImVec2{srcPort.x + 60.0F, srcPort.y},
            ImVec2{connectingEnd.x - 60.0F, connectingEnd.y},
            connectingEnd, IM_COL32(255, 208, 76, 200), 2.0F);
        draw->AddCircleFilled(connectingEnd, 4.0F, IM_COL32(255, 208, 76, 200));
    }
```

- [ ] **Step 5: Release-to-create and cancel logic**

Still inside `DrawAnimatorController`, after the canvas `InvisibleButton` section (where pan is handled), add:

```cpp
    // Finalize port-drag on mouse release
    if (connectingFrom >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        int releaseTarget = -1;
        for (int ti = 0; ti < static_cast<int>(controller.States.size()); ++ti)
        {
            if (ti == connectingFrom) continue;
            const AnimatorState& tgt = controller.States[static_cast<std::size_t>(ti)];
            const ImVec2 tl{origin.x + graphPan.x + tgt.Position.x,
                origin.y + graphPan.y + tgt.Position.y};
            if (ImGui::IsMouseHoveringRect(tl,
                    ImVec2{tl.x + nodeSize.x, tl.y + nodeSize.y}))
            {
                releaseTarget = ti;
                break;
            }
        }
        if (releaseTarget >= 0)
        {
            const std::string fromName =
                controller.States[static_cast<std::size_t>(connectingFrom)].Name;
            const std::string toName =
                controller.States[static_cast<std::size_t>(releaseTarget)].Name;
            CommitControllerEdit(scene, "Add Animator Transition", [&]() {
                AnimatorTransition t;
                t.From = fromName;
                t.To = toName;
                t.HasExitTime = true;
                controller.Transitions.push_back(std::move(t));
                selectedTransition =
                    static_cast<int>(controller.Transitions.size()) - 1;
                selectedState = -1;
            });
        }
        connectingFrom = -1;
    }
    // Cancel on Escape
    if (connectingFrom >= 0 && ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        connectingFrom = -1;
    }
```

- [ ] **Step 6: Add right-click context menu on state nodes**

In the node loop, after the `InvisibleButton` + drag block and before the port-circle block, add:

```cpp
        if (ImGui::BeginPopupContextItem("NodeCtx"))
        {
            if (ImGui::MenuItem("Set as Entry State"))
            {
                CommitControllerEdit(scene, "Set Animator Entry State",
                    [&]() { controller.EntryState = state.Name; });
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete State"))
            {
                CommitControllerEdit(scene, "Delete Animator State", [&]() {
                    const std::string removed = state.Name;
                    controller.States.erase(
                        controller.States.begin() + index);
                    std::erase_if(controller.Transitions,
                        [&](const AnimatorTransition& tr) {
                            return tr.From == removed || tr.To == removed;
                        });
                    if (controller.EntryState == removed)
                    {
                        controller.EntryState = controller.States.empty()
                            ? std::string{}
                            : controller.States.front().Name;
                    }
                    if (animator.ActiveState == removed)
                    {
                        animator.ClearControllerRuntime();
                    }
                    selectedState = -1;
                    selectedTransition = -1;
                });
            }
            ImGui::EndPopup();
        }
        ImGui::OpenPopupOnItemClick("NodeCtx", ImGuiPopupFlags_MouseButtonRight);
```

Note: `OpenPopupOnItemClick` must be called **after** `BeginPopupContextItem` for the same ID, and after the `InvisibleButton` is the active item.

- [ ] **Step 7: Prevent naming a state "Any State"**

In the state-name `InputText` change handler inside the state inspector section, add a guard:

```cpp
// After computing newName from the InputText buffer:
if (newName == "Any State") { /* skip — reserved */ }
else { /* apply rename as before */ }
```

- [ ] **Step 8: Build editor**

```bat
cmake --build .build\debug-cmake --config Debug --target fadix_editor --parallel 8
```

Expected: build succeeds.

- [ ] **Step 9: Smoke the interaction**

Launch `.\bin\Debug\fadix_editor.exe`. Open or create a project. Select an entity with an `AnimatorComponent`. Open FDX Animation panel. Hover a state node — port circle appears on the right edge. Drag from it to another node — line follows cursor. Release over a target — transition created. Right-click a node — menu appears with "Set as Entry State" and "Delete State".

- [ ] **Step 10: Commit**

```bat
git add src/editor/imgui/panels/FdxAnimationPanel.hpp src/editor/imgui/panels/FdxAnimationPanel.cpp
git commit -m "feat(anim): port-drag transition creation, right-click node context menu"
```

---

## Task 5: Editor — Any-State Pseudo-Node

**Files:**
- Modify: `src/editor/imgui/panels/FdxAnimationPanel.cpp`

The sentinel value `-2` for `selectedState` identifies the Any-State pseudo-node. All existing guards use `selectedState >= 0`, so they naturally skip the Any-State selection without changes.

- [ ] **Step 1: Render the Any-State node**

Inside `DrawAnimatorController`, in the graph section after the real state nodes loop (after `draw->PopClipRect()`), add the Any-State node rendering. Put this **before** the pop, so it clips correctly:

```cpp
    // Any-State pseudo-node (fixed position, top-left of graph)
    constexpr ImVec2 anyStateSize{120.0F, 38.0F};
    const ImVec2 anyTL{origin.x + graphPan.x + 14.0F, origin.y + graphPan.y + 14.0F};
    ImGui::SetCursorScreenPos(anyTL);
    ImGui::PushID("AnyState");
    ImGui::InvisibleButton("AnyState", anyStateSize);
    if (ImGui::IsItemClicked())
    {
        selectedState = -2;
        selectedTransition = -1;
        connectingFrom = -1;
    }
    const bool anySelected = selectedState == -2;
    draw->AddRectFilled(anyTL,
        ImVec2{anyTL.x + anyStateSize.x, anyTL.y + anyStateSize.y},
        IM_COL32(72, 45, 110, 255), 5.0F);
    draw->AddRect(anyTL,
        ImVec2{anyTL.x + anyStateSize.x, anyTL.y + anyStateSize.y},
        anySelected ? IM_COL32(200, 170, 255, 255) : IM_COL32(140, 110, 190, 255),
        5.0F, 0, anySelected ? 2.5F : 1.5F);
    draw->AddText(ImVec2{anyTL.x + 10.0F, anyTL.y + 11.0F},
        IM_COL32(220, 200, 255, 255), "Any State");
    // Port on Any-State right edge
    const ImVec2 anyPort{anyTL.x + anyStateSize.x, anyTL.y + anyStateSize.y * 0.5F};
    const bool anyPortHit = canvasHovered &&
        ImGui::IsMouseHoveringRect(
            ImVec2{anyPort.x - 7.0F, anyPort.y - 7.0F},
            ImVec2{anyPort.x + 7.0F, anyPort.y + 7.0F});
    if (canvasHovered)
    {
        draw->AddCircleFilled(anyPort, 5.0F,
            anyPortHit ? IM_COL32(255, 208, 76, 255) : IM_COL32(140, 110, 190, 210));
    }
    if (anyPortHit && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        connectingFrom = -3; // sentinel: dragging from Any-State
        connectingEnd = anyPort;
    }
    ImGui::PopID();
```

- [ ] **Step 2: Draw transition arrows from Any-State**

In the transition-drawing loop (where bezier curves are drawn), the existing loop already draws transitions where `From` matches a real state. Extend the `stateCenter` lambda so `"Any State"` resolves to the Any-State node's center:

```cpp
    const auto stateCenter = [&](const std::string& name) -> ImVec2 {
        if (name == "Any State")
        {
            return {anyTL.x + anyStateSize.x * 0.5F, anyTL.y + anyStateSize.y * 0.5F};
        }
        const AnimatorState* state = FindAnimatorState(controller, name);
        return state == nullptr ? origin
            : ImVec2{origin.x + graphPan.x + state->Position.x + 75.0F,
                origin.y + graphPan.y + state->Position.y + 27.0F};
    };
```

Also update the loop guard that skips transitions with missing states:

```cpp
    // Replace:
    if (FindAnimatorState(controller, transition.From) == nullptr ||
        FindAnimatorState(controller, transition.To) == nullptr)
    // With:
    if ((transition.From != "Any State" &&
            FindAnimatorState(controller, transition.From) == nullptr) ||
        FindAnimatorState(controller, transition.To) == nullptr)
```

- [ ] **Step 3: Handle `connectingFrom == -3` (drag from Any-State)**

In the release-to-create block (Task 4, Step 5), add handling for the Any-State source before the existing `if (connectingFrom >= 0 ...)` block:

```cpp
    if (connectingFrom == -3 && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        for (int ti = 0; ti < static_cast<int>(controller.States.size()); ++ti)
        {
            const AnimatorState& tgt = controller.States[static_cast<std::size_t>(ti)];
            const ImVec2 tl{origin.x + graphPan.x + tgt.Position.x,
                origin.y + graphPan.y + tgt.Position.y};
            if (ImGui::IsMouseHoveringRect(tl,
                    ImVec2{tl.x + nodeSize.x, tl.y + nodeSize.y}))
            {
                const std::string toName = tgt.Name;
                CommitControllerEdit(scene, "Add Any-State Transition", [&]() {
                    AnimatorTransition t;
                    t.From = "Any State";
                    t.To = toName;
                    controller.Transitions.push_back(std::move(t));
                    selectedTransition =
                        static_cast<int>(controller.Transitions.size()) - 1;
                    selectedState = -1;
                });
                break;
            }
        }
        connectingFrom = -1;
    }
```

Also extend the in-progress line drawing to handle `connectingFrom == -3`:

```cpp
    if ((connectingFrom >= 0 || connectingFrom == -3) && ...)
    {
        ImVec2 srcPort;
        if (connectingFrom == -3)
        {
            srcPort = {anyTL.x + anyStateSize.x, anyTL.y + anyStateSize.y * 0.5F};
        }
        else
        {
            const AnimatorState& srcState = controller.States[...];
            srcPort = {...};
        }
        // ... rest unchanged
    }
```

- [ ] **Step 4: Any-State inspector**

After the existing transition inspector (`if (selectedTransition >= 0) { ... }`), add:

```cpp
    if (selectedState == -2)
    {
        ImGui::BeginChild("AnyStateInspector", ImVec2{0.0F, 100.0F}, true);
        ImGui::TextUnformatted("ANY STATE");
        ImGui::SameLine();
        ImGui::TextDisabled("— transitions fire from any active state");
        ImGui::Separator();
        ImGui::TextUnformatted("Add transition to:");
        ImGui::SameLine();
        for (int ti = 0; ti < static_cast<int>(controller.States.size()); ++ti)
        {
            ImGui::PushID(ti + 30000);
            const std::string& name =
                controller.States[static_cast<std::size_t>(ti)].Name;
            if (ImGui::SmallButton((std::string{"+ "} + name).c_str()))
            {
                CommitControllerEdit(scene, "Add Any-State Transition", [&]() {
                    AnimatorTransition t;
                    t.From = "Any State";
                    t.To = name;
                    controller.Transitions.push_back(std::move(t));
                    selectedTransition =
                        static_cast<int>(controller.Transitions.size()) - 1;
                    selectedState = -1;
                });
            }
            ImGui::SameLine();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
```

- [ ] **Step 5: Build and smoke**

```bat
cmake --build .build\debug-cmake --config Debug --target fadix_editor --parallel 8
.\bin\Debug\fadix_editor.exe
```

Open FDX Animation. Verify the purple Any-State node appears at the top-left of the graph. Drag from its port to a state — transition created (line labeled "Any State → StateName" in the transition inspector). Clicking Any-State shows the add-transition UI.

- [ ] **Step 6: Commit**

```bat
git add src/editor/imgui/panels/FdxAnimationPanel.cpp
git commit -m "feat(anim): Any-State pseudo-node in animator graph editor"
```

---

## Task 6: Editor — Blend Tree Inspector

**Files:**
- Modify: `src/editor/imgui/panels/FdxAnimationPanel.cpp`

- [ ] **Step 1: Update node label for blend tree states**

In the node-drawing loop, replace:

```cpp
draw->AddText(ImVec2{topLeft.x + 10.0F, topLeft.y + 29.0F}, IM_COL32(174, 182, 195, 255),
    state.ClipName.empty() ? "No clip" : state.ClipName.c_str());
```

with:

```cpp
    char nodeSubLabel[128]{};
    if (state.UseBlendTree)
    {
        std::snprintf(nodeSubLabel, sizeof(nodeSubLabel),
            "Blend Tree (%zu clips)", state.BlendEntries.size());
    }
    else
    {
        std::snprintf(nodeSubLabel, sizeof(nodeSubLabel), "%s",
            state.ClipName.empty() ? "No clip" : state.ClipName.c_str());
    }
    draw->AddText(ImVec2{topLeft.x + 10.0F, topLeft.y + 29.0F},
        IM_COL32(174, 182, 195, 255), nodeSubLabel);
```

- [ ] **Step 2: Blend tree controls in the state inspector**

In the state inspector block (`if (selectedState >= 0) { ... }`), after the clip combo and before the "Set Entry" button row, add:

```cpp
        // Blend tree toggle
        bool useBlendTree = state.UseBlendTree;
        if (ImGui::Checkbox("Blend Tree", &useBlendTree))
        {
            CommitControllerEdit(scene, "Toggle Blend Tree", [&]() {
                state.UseBlendTree = useBlendTree;
            });
        }
        // Hide clip combo when blend tree is active
        // (move the existing clip combo inside an `if (!state.UseBlendTree)` block)
```

Wrap the existing clip combo `if (ImGui::BeginCombo(...))` inside:
```cpp
        if (!state.UseBlendTree)
        {
            // ... existing clip combo code ...
        }
```

- [ ] **Step 3: Blend parameter dropdown**

After the checkbox, when `state.UseBlendTree` is true, show the float-param picker:

```cpp
        if (state.UseBlendTree)
        {
            ImGui::SameLine();
            ImGui::TextUnformatted("Param:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(130.0F);
            // Collect float parameter names
            std::vector<const char*> floatParamNames;
            for (const AnimatorParameter& p : controller.Parameters)
            {
                if (p.Type == AnimatorParameterType::Float)
                {
                    floatParamNames.push_back(p.Name.c_str());
                }
            }
            if (floatParamNames.empty())
            {
                ImGui::TextColored(ImVec4{1.0F, 0.6F, 0.3F, 1.0F}, "No float params");
            }
            else
            {
                const char* preview = state.BlendParameter.empty()
                    ? floatParamNames[0]
                    : state.BlendParameter.c_str();
                if (ImGui::BeginCombo("##BlendParam", preview))
                {
                    for (const char* name : floatParamNames)
                    {
                        if (ImGui::Selectable(name, state.BlendParameter == name))
                        {
                            CommitControllerEdit(scene, "Set Blend Parameter",
                                [&]() { state.BlendParameter = name; });
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        }
```

- [ ] **Step 4: Blend tree entry list**

After the param dropdown block, when `state.UseBlendTree`, show the entry table:

```cpp
        if (state.UseBlendTree)
        {
            ImGui::Separator();
            int removeEntry = -1;
            for (int ei = 0; ei < static_cast<int>(state.BlendEntries.size()); ++ei)
            {
                BlendTree1DEntry& entry =
                    state.BlendEntries[static_cast<std::size_t>(ei)];
                ImGui::PushID(ei + 50000);
                ImGui::SetNextItemWidth(70.0F);
                ImGui::DragFloat("##Thresh", &entry.Threshold, 0.01F, -1000.0F, 1000.0F, "%.2f");
                TrackControllerEditItem(scene, "Set Blend Threshold");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140.0F);
                if (ImGui::BeginCombo("##EClip",
                        entry.ClipName.empty() ? "Select" : entry.ClipName.c_str()))
                {
                    for (const std::string& cn : clipNames)
                    {
                        if (ImGui::Selectable(cn.c_str(), cn == entry.ClipName))
                        {
                            CommitControllerEdit(scene, "Set Blend Entry Clip",
                                [&]() { entry.ClipName = cn; });
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(FADIX_ICON_TRASH "##re"))
                {
                    removeEntry = ei;
                }
                ImGui::PopID();
            }
            if (removeEntry >= 0)
            {
                CommitControllerEdit(scene, "Remove Blend Entry", [&]() {
                    state.BlendEntries.erase(
                        state.BlendEntries.begin() + removeEntry);
                });
            }
            if (ImGui::SmallButton(FADIX_ICON_PLUS "  Add Entry"))
            {
                CommitControllerEdit(scene, "Add Blend Entry", [&]() {
                    BlendTree1DEntry entry;
                    entry.ClipName = clipNames.empty() ? std::string{} : clipNames.front();
                    entry.Threshold = state.BlendEntries.empty()
                        ? 0.0F
                        : state.BlendEntries.back().Threshold + 1.0F;
                    state.BlendEntries.push_back(std::move(entry));
                });
            }

            // 1D ruler visualization
            if (!state.BlendEntries.empty())
            {
                ImGui::Spacing();
                const ImVec2 rulerStart = ImGui::GetCursorScreenPos();
                const float rulerW = ImGui::GetContentRegionAvail().x - 4.0F;
                constexpr float rulerH = 6.0F;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(rulerStart,
                    ImVec2{rulerStart.x + rulerW, rulerStart.y + rulerH},
                    IM_COL32(50, 55, 66, 255), 3.0F);
                // Find sorted min/max threshold
                float minT = state.BlendEntries[0].Threshold;
                float maxT = state.BlendEntries[0].Threshold;
                for (const BlendTree1DEntry& e : state.BlendEntries)
                {
                    minT = std::min(minT, e.Threshold);
                    maxT = std::max(maxT, e.Threshold);
                }
                const float range = maxT - minT;
                for (const BlendTree1DEntry& e : state.BlendEntries)
                {
                    const float t = range > 1.0e-5F
                        ? (e.Threshold - minT) / range
                        : 0.5F;
                    const float x = rulerStart.x + t * rulerW;
                    dl->AddLine(
                        ImVec2{x, rulerStart.y - 2.0F},
                        ImVec2{x, rulerStart.y + rulerH + 2.0F},
                        IM_COL32(255, 208, 76, 220), 2.0F);
                }
                ImGui::Dummy(ImVec2{rulerW, rulerH + 4.0F});
            }
        }
```

- [ ] **Step 5: Build editor**

```bat
cmake --build .build\debug-cmake --config Debug --target fadix_editor --parallel 8
.\bin\Debug\fadix_editor.exe
```

Open FDX Animation. Select a state node → inspector shows "Blend Tree" checkbox. Check it — clip combo hides, param dropdown and entry list appear. Add entries, adjust thresholds — ruler markers update. Node label changes to "Blend Tree (N clips)". Save the scene; reload — blend tree data persists.

- [ ] **Step 6: Commit**

```bat
git add src/editor/imgui/panels/FdxAnimationPanel.cpp
git commit -m "feat(anim): blend tree inspector — entries, threshold ruler, node label"
```

---

## Task 7: Play-Mode Live Parameter Controls

**Files:**
- Modify: `src/editor/imgui/panels/InspectorPanel.cpp`

- [ ] **Step 1: Add live param controls in the Animator component section**

In `InspectorPanel.cpp`, find the `AnimatorComponent` `CollapsingHeader` block (around line 807). After the existing "Open FDX Animation" button and before `RemoveButton(...)`, add:

```cpp
            // Play-mode: live parameter editing
            const bool inPlayMode = ui.PlayModeLabel != "Edit";
            if (inPlayMode && !animator->Controller.Parameters.empty())
            {
                ImGui::Separator();
                if (!animator->ActiveState.empty())
                {
                    ImGui::TextDisabled("State: %s", animator->ActiveState.c_str());
                }
                ImGui::TextUnformatted("Parameters:");
                for (AnimatorParameter& param : animator->Controller.Parameters)
                {
                    ImGui::PushID(param.Name.c_str());
                    ImGui::SetNextItemWidth(110.0F);
                    if (param.Type == AnimatorParameterType::Float)
                    {
                        ImGui::DragFloat(param.Name.c_str(), &param.FloatValue, 0.02F);
                    }
                    else if (param.Type == AnimatorParameterType::Int)
                    {
                        ImGui::DragInt(param.Name.c_str(), &param.IntValue, 1.0F);
                    }
                    else if (param.Type == AnimatorParameterType::Bool)
                    {
                        ImGui::Checkbox(param.Name.c_str(), &param.BoolValue);
                    }
                    else // Trigger
                    {
                        if (ImGui::Button(param.Name.c_str()))
                        {
                            param.BoolValue = true;
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("(trigger)");
                    }
                    ImGui::PopID();
                }
            }
```

No undo wrapping — play-mode state is transient. The next `UpdateWorldAnimations` tick picks up changes automatically.

- [ ] **Step 2: Build editor**

```bat
cmake --build .build\debug-cmake --config Debug --target fadix_editor --parallel 8
```

- [ ] **Step 3: Smoke the interaction**

Launch `.\bin\Debug\fadix_editor.exe`. Open a project with a skinned animated entity. Enter Play mode. Select the entity — Inspector shows "Parameters:" section with live controls. Drag a float — watch the active state transition when conditions are met. Trigger button fires and resets. Stop play.

- [ ] **Step 4: Commit**

```bat
git add src/editor/imgui/panels/InspectorPanel.cpp
git commit -m "feat(anim): live parameter controls in Inspector during play mode"
```

---

## Self-Review Notes

- Serializer version detection uses `seekg` on `istream` — valid because `AnimatorControllerIO` is called with `std::istringstream`, which supports `seekg`. Confirmed in existing `ReadAnimatorControllerData` usage.
- `connectingFrom == -3` is an Any-State drag sentinel. Not -1 (idle) or >= 0 (real node). All `>= 0` guards skip it cleanly.
- `selectedState == -2` is the Any-State selection sentinel. All `>= 0` guards skip it.
- `BlendTree1DEntry` must be included before `AnimatorState` in `AnimationClip.hpp` — the struct is defined in order.
- `SampleSkeletalBlendTree1D` must appear after `FindAnimationClip` in `AnimationRuntime.hpp`. The function ordering in that header is: helpers, `FindAnimationClip`, then `UpdateWorldAnimations`. Place it between `FindAnimationClip` and `UpdateWorldAnimations`.
- `ApplyTransformBlendTree1D` must appear after `ApplyTransformClip` and `FindTransformClip` in `AnimationRuntime.hpp`. Place it just before `UpdateTransformAnimations`.
