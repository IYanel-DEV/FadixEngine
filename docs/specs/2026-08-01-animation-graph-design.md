# Animation Graph — Design Spec

**Date:** 2026-08-01
**Sub-project:** 1 of 4 (AnimationGraph → Montage → Sequencer → Procedural)
**Branch target:** dev

---

## Goal

Replace the flat `AnimatorController` + `BlendTree1D` hack with a proper pull-based virtual node graph (`AnimationGraph`) that drives character poses. Existing `AnimatorController` scenes keep working unchanged. New content uses the graph.

---

## Architecture: Pull-Based Virtual Node Graph

Each frame, the runtime calls `OutputNode::Evaluate(ctx)`, which recursively pulls poses from child nodes. No push, no topological sort — just recursive virtual dispatch. Node count per character is 5–20; virtual call overhead is negligible.

Nodes own their per-instance playback state (e.g. `ClipNode::CurrentTime`). The graph asset itself is shared across entities; per-instance state lives on the component (`AnimatorComponent::GraphInstanceData`).

---

## Core Types

### `AnimGraphParameter`
```cpp
struct AnimGraphParameter {
    enum class Type { Float, Bool, Int, Trigger };
    std::string Name;
    Type ParamType{Type::Float};
    float FloatValue{0.0F};
    bool BoolValue{false};
    int IntValue{0};
};
```

### `AnimGraphContext`
Passed by reference down the recursive `Evaluate` chain each frame.

```cpp
struct AnimGraphContext {
    float DeltaTime{0.0F};
    const Skeleton* Skel{nullptr};
    std::span<AnimGraphParameter> Parameters;
    std::unordered_map<std::string, SkeletonPose> SavedPoses;

    float GetFloat(std::string_view name) const;
    bool  GetBool(std::string_view name) const;
    int   GetInt(std::string_view name) const;
    void  SetTrigger(std::string_view name, bool value);  // runtime resets triggers
};
```

### `AnimGraphNode` (base)
```cpp
struct AnimGraphNode {
    glm::vec2 EditorPosition{};

    virtual ~AnimGraphNode() = default;
    virtual SkeletonPose Evaluate(AnimGraphContext& ctx) = 0;
    virtual std::string_view TypeName() const = 0;
    virtual void Serialize(JsonWriter&) const = 0;
    virtual void Deserialize(const JsonValue&) = 0;
};
```

### `AnimationGraph` (asset)
```cpp
struct AnimationGraph {
    std::string Name;
    std::vector<std::unique_ptr<AnimGraphNode>> Nodes;
    std::vector<AnimGraphParameter> Parameters;  // schema + defaults
    int OutputNodeIndex{-1};

    SkeletonPose Evaluate(AnimGraphContext& ctx);
};
```

Node connections stored as `int` child indices into `Nodes[]` — no raw pointers, serialize cleanly.

### `AnimGraphNodeRegistry`
Static registry mapping `std::string_view TypeName → std::unique_ptr<AnimGraphNode>(*)()` factory.
New node types call `AnimGraphNodeRegistry::Register<T>()` at startup — no engine changes needed.

---

## Node Types (Standard Set)

### `ClipNode`
```
Fields: ClipName (string), Speed (float=1), Loop (bool=true), Mirror (bool=false)
State:  CurrentTime (float, per-instance)
Output: Sampled SkeletonPose at CurrentTime
```
Advances `CurrentTime` by `ctx.DeltaTime * Speed`, wraps if `Loop`. Samples all channels of the named clip.

### `BlendByFloatNode`
```
Fields: ParameterName (string)
        Entries: vector<{float Threshold, int ChildIndex}>
Output: Blended pose from two neighboring child nodes
```
Reads `ctx.GetFloat(ParameterName)`. Calls `ResolveBlendTree1D` (already implemented). Evaluates straddling pair, blends poses with `BlendSkeletonPoses`. Single-entry: returns that child directly.

### `BlendByConditionNode`
```
Fields: ParameterName (string), TrueChild (int), FalseChild (int), BlendDuration (float=0.15)
State:  float BlendElapsed, bool LastValue
Output: Crossfaded pose between TrueChild and FalseChild
```
On parameter change, starts crossfade. During fade, evaluates both children and blends by `BlendElapsed / BlendDuration`. Trigger type: fires TrueChild for one evaluation then returns to FalseChild; runtime clears trigger after consume.

### `StateMachineNode`
```
Fields: Controller (AnimatorController — existing type, embedded)
        StateNodes: map<string StateName, int ChildIndex>
State:  (delegates to AnimatorController runtime fields)
Output: Pose from whichever state is active
```
Uses existing `FindAnimatorTransition` / `BeginAnimatorTransition` / `UpdateWorldAnimations` logic. Each state maps to a child node index instead of a `ClipName` — states can now reference `ClipNode`, `BlendByFloatNode`, or any other node. **Backward compat path:** when `StateNodes` is empty, falls back to `ClipName`-based sampling (existing behavior).

### `LayeredBlendNode`
```
Fields: BaseChild (int), LayerChild (int)
        BoneMask: vector<int JointIndex>
        Weight (float=1.0), WeightParameter (string, optional)
Output: BaseChild pose with LayerChild blended over masked bones
```
Weight optionally driven by float parameter. For each joint in `BoneMask`, lerps between base and layer pose by weight. Classic use: upper body (layer) over locomotion (base).

### `AdditiveNode`
```
Fields: BaseChild (int), AdditiveChild (int)
        Weight (float=1.0), WeightParameter (string, optional)
Output: BaseChild + AdditiveChild * Weight
```
Adds delta pose on top of base. AdditiveChild should be authored as an additive clip (delta from reference pose). Used for breathing, aim sway, hit reactions.

### `SavedPoseNode` / `UseSavedPoseNode`
```
SavedPoseNode:   PoseKey (string), Child (int) — evaluates child, stores in ctx.SavedPoses[PoseKey]
UseSavedPoseNode: PoseKey (string)              — returns ctx.SavedPoses[PoseKey], zero pose if missing
```
Allows one subtree to feed two branches without double-evaluation.

### `OutputNode`
```
Fields: Child (int)
```
Root node. Fixed in graph — cannot be deleted. `Evaluate` forwards to child. Graph evaluates starting here.

---

## Component Integration

`AnimatorComponent` gains one new field:

```cpp
struct AnimatorComponent {
    // --- LEGACY (unchanged, still works) ---
    AnimatorController Controller;
    std::string ActiveState;
    float CurrentTime{0.0F};
    // ... all existing fields ...

    // --- NEW (takes priority when non-null) ---
    std::shared_ptr<AnimationGraph> Graph;
    std::vector<AnimGraphParameter> RuntimeParameters;  // per-instance values, copied from Graph::Parameters on init
};
```

Runtime tick in `AnimationRuntime.hpp`:
- If `animator.Graph != nullptr`: build `AnimGraphContext` from `RuntimeParameters`, call `Graph->Evaluate(ctx)`, apply resulting `SkeletonPose` to joints.
- Else: existing `UpdateWorldAnimations` path runs unchanged.

Triggers in `RuntimeParameters` are cleared (reset to false) after each evaluation.

---

## Editor (FdxAnimationPanel — new "Anim Graph" tab)

### Canvas
Same pan/zoom ImGui DrawList canvas as state machine graph. Nodes are rounded rectangles. Input ports on the left edge, output port on the right edge. Bezier connections between ports.

### Node Colors
| Node | Color |
|---|---|
| ClipNode | Blue `IM_COL32(52, 120, 210, 255)` |
| BlendByFloatNode | Teal `IM_COL32(30, 160, 140, 255)` |
| BlendByConditionNode | Purple `IM_COL32(130, 80, 200, 255)` |
| StateMachineNode | Dark grey `IM_COL32(60, 65, 75, 255)` |
| LayeredBlendNode | Orange `IM_COL32(200, 120, 40, 255)` |
| AdditiveNode | Green `IM_COL32(50, 175, 90, 255)` |
| SavedPoseNode / UseSavedPoseNode | Yellow `IM_COL32(200, 180, 40, 255)` |
| OutputNode | White `IM_COL32(230, 235, 245, 255)` |

### Interactions
- **Port drag** — drag output port → input port to connect. Same mechanic as transition port-drag.
- **Right-click canvas** → "Add Node" submenu (categories: Clips, Blending, Logic, Utilities).
- **Right-click node** → Delete (except OutputNode).
- **Right-click connection line** → Delete connection.
- **Click node** → shows node inspector in right panel.
- **Double-click StateMachineNode** → opens existing state graph editor for that controller.

### Parameters Panel
Left sidebar. Lists all `AnimationGraph::Parameters` with type badge + default value field. Add / rename / delete buttons. Parameter names show in dropdown pickers inside node inspectors.

### Node Inspector (right panel)
Per node type:
- `ClipNode`: clip asset picker, speed drag, loop toggle, mirror toggle.
- `BlendByFloatNode`: parameter dropdown, entry list with threshold + child-node-picker per entry, inline 1D ruler.
- `BlendByConditionNode`: parameter dropdown, blend duration drag.
- `StateMachineNode`: entry state dropdown, per-state child-node assignment.
- `LayeredBlendNode`: bone mask checkbox list (skeleton joints), weight drag, optional parameter dropdown.
- `AdditiveNode`: weight drag, optional parameter dropdown.
- `OutputNode`: no fields.

### Migration Button
When `AnimatorComponent` has a legacy `Controller` and no `Graph`, Inspector shows **"Upgrade to Anim Graph"** button. Creates a `StateMachineNode` wrapping the controller + `OutputNode`, sets `Graph`, leaves `Controller` in place as the embedded data.

---

## Serialization

`AnimationGraph` serializes to JSON embedded in `AnimatorComponent`'s scene entity data.

```json
{
  "graph": {
    "name": "PlayerGraph",
    "parameters": [
      {"name": "Speed", "type": "Float", "default": 0.0},
      {"name": "IsGrounded", "type": "Bool", "default": true}
    ],
    "outputNode": 2,
    "nodes": [
      {"type": "ClipNode",   "editorPos": [100, 200], "data": {"clip": "Idle", "loop": true}},
      {"type": "ClipNode",   "editorPos": [100, 350], "data": {"clip": "Run",  "loop": true}},
      {"type": "BlendByFloatNode", "editorPos": [350, 275], "data": {"param": "Speed", "entries": [{"t": 0.0, "child": 0}, {"t": 5.0, "child": 1}]}},
      {"type": "OutputNode", "editorPos": [600, 275], "data": {"child": 2}}
    ]
  }
}
```

`AnimGraphNodeRegistry` maps `"ClipNode"` → factory. Adding a new node: implement `TypeName()`, `Serialize()`, `Deserialize()`, call `AnimGraphNodeRegistry::Register<MyNode>()`.

---

## FXS / Lua Scripting API

Unchanged surface — same calls work for legacy controller and graph:

```lua
animator:setFloat("Speed", velocity:length())
animator:setBool("IsGrounded", grounded)
animator:setInt("WeaponType", 2)
animator:trigger("Attack")          -- auto-clears after next Evaluate()

-- Read-back
local state = animator:getActiveState()  -- from StateMachineNode if present
local time  = animator:getClipTime()     -- from active ClipNode if present
```

---

## Smoke Tests (`tools/AnimationSmoke.cpp`)

- `PASS AnimGraph ClipNode evaluates non-zero pose`
- `PASS AnimGraph BlendByFloat interpolates between two clip nodes`
- `PASS AnimGraph BlendByCondition crossfades on bool change`
- `PASS AnimGraph LayeredBlend merges upper/lower body bones`
- `PASS AnimGraph StateMachineNode backward compat (no StateNodes map)`
- `PASS AnimGraph SavedPose caches and retrieves pose`
- `PASS AnimGraph trigger parameter clears after one evaluation`
- `PASS AnimGraph serialization round-trip (write + read back)`
- `PASS AnimGraph legacy AnimatorComponent path still runs when Graph == null`

---

## What This Does NOT Include

- Sub-project 2 (Sequencer / multi-track timeline)
- Sub-project 3 (Montage system)
- Sub-project 4 (IK / procedural nodes)
- BlendSpace2D (2D parameter grid) — deferred to after core graph ships
- Network replication of graph parameters
- LOD / culling of distant character animation graphs
