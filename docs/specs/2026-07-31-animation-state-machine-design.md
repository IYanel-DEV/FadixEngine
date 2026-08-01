# Animation State Machine v2 — Design Spec
**Date:** 2026-07-31  
**Status:** Approved

## Goal

Complete the existing animator system with four missing pieces:
1. **1D Blend Trees** — a state blends N clips by a float parameter instead of playing one clip
2. **Any-State transitions** — transitions that fire from any active state
3. **Port-drag transition creation** — drag from a node edge to create a transition (replaces the flat button list)
4. **Play-mode live parameter controls** — edit animator parameters in the Inspector while the game runs

The system already has: state graph editor, bezier transition lines, parameter/condition inspector, crossfade blending, Lua API, serialization.

---

## 1. Data Model

**File:** `src/engine/animation/AnimationClip.hpp`

### New struct

```cpp
struct BlendTree1DEntry {
    std::string ClipName;
    float Threshold{0.0F};
};
```

### Extended `AnimatorState`

```cpp
struct AnimatorState {
    std::string Name{"State"};
    std::string ClipName;           // used when UseBlendTree == false
    glm::vec2 Position{40.0F, 40.0F};

    bool UseBlendTree{false};
    std::string BlendParameter;     // must name a Float parameter
    std::vector<BlendTree1DEntry> BlendEntries; // caller keeps sorted by Threshold
};
```

`ClipName` is kept for backward compatibility and is the only field used when `UseBlendTree` is false. Existing controllers load unchanged.

### Any-State convention

No struct change. The reserved string `"Any State"` in `AnimatorTransition::From` identifies an Any-State transition. The graph renders a special pseudo-node for it; the runtime checks it via a second pass.

---

## 2. Serialization

**File:** `src/engine/animation/AnimatorControllerIO.hpp`

For each state, after the existing `Name ClipName Position.x Position.y` fields, append:

```
UseBlendTree BlendParameter entryCount [ClipName Threshold]...
```

Old files that end before these fields parse successfully — missing fields fall back to `UseBlendTree=false`, `BlendParameter=""`, `entryCount=0`. This is backward compatible.

---

## 3. Runtime

**File:** `src/runtime/AnimationRuntime.hpp`

### Any-State support in `FindAnimatorTransition`

After the current pass (searching `From == ActiveState`), if no transition was found, do a second pass searching `From == "Any State"`. The second pass must not fire if the destination is already the active state (prevents self-loop flicker).

### Blend tree clip resolution

New free function:

```cpp
struct BlendTree1DResult {
    std::string ClipA;         // always set
    std::string ClipB;         // empty when only one entry or clamped to edge
    float Weight{0.0F};        // blend weight toward ClipB [0,1]
};

// entries need not be pre-sorted; the function sorts a local copy.
// Returns a single-clip result (ClipB empty, Weight=0) when entries has one element
// or when value is exactly at an edge threshold.
BlendTree1DResult ResolveBlendTree1D(
    std::vector<BlendTree1DEntry> entries, float value);
```

Logic:
- Sort entries by `Threshold` (local copy, N ≤ ~8 so linear is fine)
- Clamp `value` to `[entries.front().Threshold, entries.back().Threshold]`
- Find the two straddling entries with a linear scan
- `Weight = (value - lo.Threshold) / (hi.Threshold - lo.Threshold)`
- If only one entry: `ClipA = entries[0].ClipName`, `ClipB = ""`, `Weight = 0`

In `UpdateWorldAnimations` and `UpdateTransformAnimations`, when the active state has `UseBlendTree == true`:
1. Look up the float parameter value from `animator.Controller`
2. Call `ResolveBlendTree1D`
3. Sample both clips, call existing `BlendSkeletonPoses` / `BlendTransformPoses` with the returned weight
4. The resulting pose feeds the existing crossfade logic unchanged

The crossfade (transition blend) continues to work: it blends the full output pose of the blend tree against the full output pose of the destination state, same as today.

---

## 4. Editor UI

**File:** `src/editor/imgui/panels/FdxAnimationPanel.cpp` (`.cpp` only, no header cascade)

### 4a. Port-drag transition creation

New panel state members:
```cpp
int m_ConnectingFromState{-1};   // index, -1 = idle
ImVec2 m_ConnectingLineEnd{};
```

In the node-drawing loop:
- When `canvasHovered` and the mouse is within ~16px of a node's right edge, draw a small circle (radius 5) on the edge in accent colour.
- If that circle is left-click-dragged, set `m_ConnectingFromState = index`.
- While `m_ConnectingFromState >= 0`, draw a bezier line from the source node center to `ImGui::GetMousePos()`.
- On mouse release: if cursor is inside another node's bounding rect, `CommitControllerEdit("Add Animator Transition", ...)` and reset `m_ConnectingFromState = -1`. If released over empty space, just reset (no transition created).

### 4b. Right-click context menu on nodes

In the node loop, after the existing `InvisibleButton`, add `ImGui::OpenPopupOnItemClick("NodeCtx", ImGuiPopupFlags_MouseButtonRight)`. Popup contains:
- **Set as Entry** → `CommitControllerEdit(...)`
- **Delete State** → same as current delete button

This surfaces existing actions without removing them from the inspector.

### 4c. Any-State pseudo-node

Always rendered at `origin + graphPan + (20, 20)` (fixed canvas-space position). Size: `{120, 40}`. Colour: purple (`IM_COL32(90, 60, 140, 255)`), border white. Label: `"Any State"`.

Interaction:
- Left-click: set `selectedState = AnyStateIndex` (use sentinel value `-2` to distinguish from real state indices).
- When `selectedState == -2`, the state inspector below shows only a row of "→ StateName" transition-add buttons for every real state, and a list of existing Any-State transitions with a delete button.
- Any-State node is not draggable (position is fixed).
- Port-drag from Any-State node works the same as from real nodes.

### 4d. Blend tree inspector (state inspector expansion)

When `selectedState >= 0` (a real state is selected), the state inspector shows a new row beneath the existing clip dropdown:

```
[x] Blend Tree   Parameter: [SpeedDropdown▼]
```

Checking "Blend Tree" sets `state.UseBlendTree = true` and hides the clip dropdown. Unchecking restores the clip dropdown.

When `UseBlendTree` is true, below the checkbox row:

| Threshold | Clip | |
|-----------|------|---|
| [0.0 drag] | [Idle▼] | [x] |
| [0.5 drag] | [Walk▼] | [x] |
| [1.0 drag] | [Run▼]  | [x] |
| [+ Add Entry] | | |

Entries are shown sorted by threshold. "Add Entry" appends a new entry with threshold = last + 1.0 and the first available clip. A thin 1D ruler (a horizontal line with threshold markers) is drawn below the list as a visual aid.

The state graph node label changes from the clip name to `"Blend Tree (N clips)"` when `UseBlendTree` is true.

---

## 5. Play-mode Parameter Controls

**File:** `src/editor/imgui/panels/InspectorPanel.cpp` — inside the `AnimatorComponent` `CollapsingHeader` block at line ~805.

During play mode (when `EditorPlayMode != Edit`), render each parameter in `animator.Controller.Parameters` as a live control:
- `Bool` / `Trigger`: `ImGui::Checkbox`
- `Float`: `ImGui::DragFloat`
- `Int`: `ImGui::DragInt`

Changes write directly to `animator.Controller.Parameters[i]` — the next `UpdateWorldAnimations` tick picks them up automatically. No undo wrapping (play-mode state is transient).

Show the active state name as read-only text above the parameter list.

---

## 6. Smoke Test Extension

**File:** `tools/AnimationSmoke.cpp`

Add two test cases to the existing smoke:

1. **Blend tree resolution**: Build an `AnimatorState` with `UseBlendTree=true` and three entries at thresholds 0/0.5/1.0. Call `ResolveBlendTree1D` with values -0.1, 0.0, 0.25, 0.5, 0.75, 1.0, 1.1. Assert correct clip pair and weight for each. Edge clamp must work.

2. **Any-State transition**: Build an `AnimatorController` with states A and B, a normal transition A→B (with unsatisfied condition), and an Any-State transition `"Any State"→B` (no conditions). Set `ActiveState = "A"`. Call `FindAnimatorTransition`. Assert it returns the Any-State transition, not null.

---

## 7. File Touch Summary

| File | Change type | Recompile impact |
|------|-------------|-----------------|
| `src/engine/animation/AnimationClip.hpp` | Add struct + state fields | Cascades (full editor rebuild, once) |
| `src/engine/animation/AnimatorControllerIO.hpp` | Serializer extension | Cascades (included by panel + smoke only) |
| `src/runtime/AnimationRuntime.hpp` | Any-State pass + blend tree eval | Cascades (included by editor + player) |
| `src/editor/imgui/panels/FdxAnimationPanel.cpp` | All UI changes | Local only |
| `tools/AnimationSmoke.cpp` | Test extension | Local only |
| Inspector AnimatorComponent `.cpp` | Play-mode params | Local only |

The header changes happen once. Subsequent iteration on UI is `.cpp`-only.

---

## 8. Out of Scope

- 2D blend spaces
- Sub-state machines / hierarchical states  
- AvatarMask / layered blending
- Transition interrupt / re-entry modes
- Retargeting between skeletons
