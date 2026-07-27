#pragma once
#ifdef FADIX_EDITOR

#include <optional>
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "Components.hpp" // all engine components + ScriptComponent (editor)

class Scene;

// =============================================================================
// Fadix Engine — Editor Undo/Redo History  (editor-only)
// =============================================================================
//
// A classic command/transaction stack.  Commands store value snapshots (not
// callbacks) so they stay valid however the registry mutates in between.
//
// Tracked operations:
//   - Transform edits    (gizmo drags and Inspector field edits, coalesced
//                         into one command per drag)
//   - Entity creation    (undo destroys; redo restores the full snapshot)
//   - Entity deletion    (whole subtree snapshot, original handles revived)
//   - Script assignment  (attach / rebind / unbind on ScriptComponent)
//   - Reparenting        (hierarchy drag-and-drop)
//
// Entity identity: entt revives an exact identifier through create(hint) as
// long as its index is free, which holds for LIFO undo usage.  The history is
// cleared whenever the active scene object changes (Play/Stop swap, load).
//
// =============================================================================

namespace fadix {

// Full value snapshot of one entity — everything needed to resurrect it.
struct EntitySnapshot
{
    entt::entity Handle = entt::null;

    std::optional<TagComponent>            Tag;
    std::optional<TransformComponent>      Transform;
    std::optional<RelationshipComponent>   Relationship;
    std::optional<MeshComponent>           Mesh;
    std::optional<LightComponent>          Light;
    std::optional<CameraComponent>         Camera;
    std::optional<RigidBody3DComponent>      RigidBody3D;
    std::optional<BoxCollider3DComponent>    BoxCollider3D;
    std::optional<SphereCollider3DComponent> SphereCollider3D;
    std::optional<RigidBody2DComponent>      RigidBody2D;
    std::optional<BoxCollider2DComponent>    BoxCollider2D;
    std::optional<ScriptComponent>         Script;
    std::optional<NativeScriptComponent>   NativeScript; // Instance always null in Edit mode

    static EntitySnapshot Capture(const entt::registry& registry, entt::entity e);
};

enum class EditorCommandType
{
    TransformEdit,
    EntityCreate,
    EntityDelete,
    ScriptAssign,
    Reparent,
};

struct EditorCommand
{
    EditorCommandType Type = EditorCommandType::TransformEdit;
    std::string       Label;

    // TransformEdit / ScriptAssign / Reparent target
    entt::entity Entity = entt::null;

    // TransformEdit payload
    TransformComponent TransformBefore{};
    TransformComponent TransformAfter{};

    // EntityCreate / EntityDelete payload — root first, then descendants.
    std::vector<EntitySnapshot> Snapshots;

    // ScriptAssign payload
    bool            HadScriptBefore = false;
    bool            HasScriptAfter  = false;
    ScriptComponent ScriptBefore{};
    ScriptComponent ScriptAfter{};

    // Reparent payload
    entt::entity ParentBefore = entt::null;
    entt::entity ParentAfter  = entt::null;
};

class EditorHistory
{
public:
    // ---- Recording (call at the moment the edit is committed) ---------------
    void PushTransformEdit(entt::entity entity,
                           const TransformComponent& before,
                           const TransformComponent& after,
                           const std::string& label = "Transform");

    // Call AFTER the entity (and any children) exists.
    void PushEntityCreated(Scene& scene, entt::entity entity,
                           const std::string& label = "Create Entity");

    // Call BEFORE Scene::DestroyEntity — captures the live subtree.
    void PushEntityDeleted(Scene& scene, entt::entity entity,
                           const std::string& label = "Delete Entity");

    void PushScriptAssign(entt::entity entity,
                          bool hadBefore, const ScriptComponent& before,
                          bool hasAfter,  const ScriptComponent& after,
                          const std::string& label = "Assign Script");

    // Call AFTER Scene::SetParent succeeded.
    void PushReparent(entt::entity entity,
                      entt::entity parentBefore, entt::entity parentAfter);

    // ---- Execution -----------------------------------------------------------
    bool CanUndo() const { return !m_UndoStack.empty(); }
    bool CanRedo() const { return !m_RedoStack.empty(); }

    // Labels for the Edit menu ("Undo Delete Entity" etc.); empty when n/a.
    std::string UndoLabel() const;
    std::string RedoLabel() const;

    void Undo(Scene& scene);
    void Redo(Scene& scene);

    // Drop everything — required when the scene object is swapped (Play/Stop,
    // scene load) because stored entity handles refer to the old registry.
    void Clear();

private:
    void Push(EditorCommand&& cmd);
    void Apply(Scene& scene, const EditorCommand& cmd, bool undo);

    static void CaptureSubtree(const entt::registry& registry, entt::entity root,
                               std::vector<EntitySnapshot>& out);
    static void RestoreSnapshots(Scene& scene,
                                 const std::vector<EntitySnapshot>& snapshots);

    static constexpr std::size_t kMaxDepth = 128;

    std::vector<EditorCommand> m_UndoStack;
    std::vector<EditorCommand> m_RedoStack;
};

} // namespace fadix

#endif // FADIX_EDITOR
