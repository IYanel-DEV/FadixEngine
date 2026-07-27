#include "core/EditorHistory.hpp"
#ifdef FADIX_EDITOR

#include <algorithm>

#include "core/Scene.hpp"

// =============================================================================
// Fadix Engine — Editor Undo/Redo History Implementation
// =============================================================================

namespace fadix {

// ---------------------------------------------------------------------------
// EntitySnapshot
// ---------------------------------------------------------------------------

EntitySnapshot EntitySnapshot::Capture(const entt::registry& registry,
                                       entt::entity e)
{
    EntitySnapshot s;
    s.Handle = e;

    if (const auto* c = registry.try_get<TagComponent>(e))            s.Tag = *c;
    if (const auto* c = registry.try_get<TransformComponent>(e))      s.Transform = *c;
    if (const auto* c = registry.try_get<RelationshipComponent>(e))   s.Relationship = *c;
    if (const auto* c = registry.try_get<MeshComponent>(e))           s.Mesh = *c;
    if (const auto* c = registry.try_get<LightComponent>(e))          s.Light = *c;
    if (const auto* c = registry.try_get<CameraComponent>(e))         s.Camera = *c;
    if (const auto* c = registry.try_get<RigidBody3DComponent>(e))      s.RigidBody3D = *c;
    if (const auto* c = registry.try_get<BoxCollider3DComponent>(e))    s.BoxCollider3D = *c;
    if (const auto* c = registry.try_get<SphereCollider3DComponent>(e)) s.SphereCollider3D = *c;
    if (const auto* c = registry.try_get<RigidBody2DComponent>(e))      s.RigidBody2D = *c;
    if (const auto* c = registry.try_get<BoxCollider2DComponent>(e))    s.BoxCollider2D = *c;
    if (const auto* c = registry.try_get<ScriptComponent>(e))           s.Script = *c;
    if (const auto* c = registry.try_get<NativeScriptComponent>(e))     s.NativeScript = *c;

    // Runtime-only handles must never round-trip through history.
    if (s.RigidBody3D)   s.RigidBody3D->BodyId  = 0xFFFFFFFFu;
    if (s.RigidBody2D)   s.RigidBody2D->BodyId  = {};
    if (s.BoxCollider2D) s.BoxCollider2D->ShapeId = {};
    if (s.NativeScript)  s.NativeScript->Instance = nullptr;

    return s;
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

void EditorHistory::PushTransformEdit(entt::entity entity,
                                      const TransformComponent& before,
                                      const TransformComponent& after,
                                      const std::string& label)
{
    EditorCommand cmd;
    cmd.Type            = EditorCommandType::TransformEdit;
    cmd.Label           = label;
    cmd.Entity          = entity;
    cmd.TransformBefore = before;
    cmd.TransformAfter  = after;
    Push(std::move(cmd));
}

void EditorHistory::PushEntityCreated(Scene& scene, entt::entity entity,
                                      const std::string& label)
{
    EditorCommand cmd;
    cmd.Type   = EditorCommandType::EntityCreate;
    cmd.Label  = label;
    cmd.Entity = entity;
    CaptureSubtree(scene.GetRegistry(), entity, cmd.Snapshots);
    Push(std::move(cmd));
}

void EditorHistory::PushEntityDeleted(Scene& scene, entt::entity entity,
                                      const std::string& label)
{
    EditorCommand cmd;
    cmd.Type   = EditorCommandType::EntityDelete;
    cmd.Label  = label;
    cmd.Entity = entity;
    CaptureSubtree(scene.GetRegistry(), entity, cmd.Snapshots);
    Push(std::move(cmd));
}

void EditorHistory::PushScriptAssign(entt::entity entity,
                                     bool hadBefore, const ScriptComponent& before,
                                     bool hasAfter,  const ScriptComponent& after,
                                     const std::string& label)
{
    EditorCommand cmd;
    cmd.Type            = EditorCommandType::ScriptAssign;
    cmd.Label           = label;
    cmd.Entity          = entity;
    cmd.HadScriptBefore = hadBefore;
    cmd.HasScriptAfter  = hasAfter;
    cmd.ScriptBefore    = before;
    cmd.ScriptAfter     = after;
    Push(std::move(cmd));
}

void EditorHistory::PushReparent(entt::entity entity,
                                 entt::entity parentBefore,
                                 entt::entity parentAfter)
{
    EditorCommand cmd;
    cmd.Type         = EditorCommandType::Reparent;
    cmd.Label        = "Reparent";
    cmd.Entity       = entity;
    cmd.ParentBefore = parentBefore;
    cmd.ParentAfter  = parentAfter;
    Push(std::move(cmd));
}

void EditorHistory::Push(EditorCommand&& cmd)
{
    m_UndoStack.push_back(std::move(cmd));
    if (m_UndoStack.size() > kMaxDepth)
        m_UndoStack.erase(m_UndoStack.begin());

    // Any fresh edit invalidates the redo branch.
    m_RedoStack.clear();
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

std::string EditorHistory::UndoLabel() const
{
    return m_UndoStack.empty() ? std::string{} : m_UndoStack.back().Label;
}

std::string EditorHistory::RedoLabel() const
{
    return m_RedoStack.empty() ? std::string{} : m_RedoStack.back().Label;
}

void EditorHistory::Undo(Scene& scene)
{
    if (m_UndoStack.empty()) return;

    EditorCommand cmd = std::move(m_UndoStack.back());
    m_UndoStack.pop_back();

    Apply(scene, cmd, /*undo=*/true);
    m_RedoStack.push_back(std::move(cmd));
}

void EditorHistory::Redo(Scene& scene)
{
    if (m_RedoStack.empty()) return;

    EditorCommand cmd = std::move(m_RedoStack.back());
    m_RedoStack.pop_back();

    Apply(scene, cmd, /*undo=*/false);
    m_UndoStack.push_back(std::move(cmd));
}

void EditorHistory::Clear()
{
    m_UndoStack.clear();
    m_RedoStack.clear();
}

void EditorHistory::Apply(Scene& scene, const EditorCommand& cmd, bool undo)
{
    entt::registry& registry = scene.GetRegistry();

    switch (cmd.Type)
    {
    case EditorCommandType::TransformEdit:
    {
        if (!registry.valid(cmd.Entity)) return;
        if (auto* tc = registry.try_get<TransformComponent>(cmd.Entity))
        {
            *tc = undo ? cmd.TransformBefore : cmd.TransformAfter;
            // Physics bodies (if any live) must pick up the reverted pose.
            if (auto* rb = registry.try_get<RigidBody3DComponent>(cmd.Entity))
                rb->TransformDirty = true;
            if (auto* rb2 = registry.try_get<RigidBody2DComponent>(cmd.Entity))
                rb2->TransformDirty = true;
        }
        break;
    }

    case EditorCommandType::EntityCreate:
    {
        if (undo)
        {
            if (registry.valid(cmd.Entity))
                scene.DestroyEntity(Entity(cmd.Entity, &registry));
        }
        else
        {
            RestoreSnapshots(scene, cmd.Snapshots);
        }
        break;
    }

    case EditorCommandType::EntityDelete:
    {
        if (undo)
        {
            RestoreSnapshots(scene, cmd.Snapshots);
        }
        else
        {
            if (registry.valid(cmd.Entity))
                scene.DestroyEntity(Entity(cmd.Entity, &registry));
        }
        break;
    }

    case EditorCommandType::ScriptAssign:
    {
        if (!registry.valid(cmd.Entity)) return;

        const bool           shouldExist = undo ? cmd.HadScriptBefore : cmd.HasScriptAfter;
        const ScriptComponent& value     = undo ? cmd.ScriptBefore    : cmd.ScriptAfter;

        if (shouldExist)
        {
            registry.get_or_emplace<ScriptComponent>(cmd.Entity) = value;
        }
        else
        {
            registry.remove<ScriptComponent>(cmd.Entity);
            // The runtime binding follows the editor assignment.
            if (auto* nsc = registry.try_get<NativeScriptComponent>(cmd.Entity);
                nsc && !nsc->Instance)
                registry.remove<NativeScriptComponent>(cmd.Entity);
        }
        break;
    }

    case EditorCommandType::Reparent:
    {
        if (!registry.valid(cmd.Entity)) return;
        const entt::entity target = undo ? cmd.ParentBefore : cmd.ParentAfter;
        if (target == entt::null || registry.valid(target))
            scene.SetParent(cmd.Entity, target);
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Subtree snapshot / restore
// ---------------------------------------------------------------------------

void EditorHistory::CaptureSubtree(const entt::registry& registry,
                                   entt::entity root,
                                   std::vector<EntitySnapshot>& out)
{
    if (root == entt::null || !registry.valid(root)) return;

    out.push_back(EntitySnapshot::Capture(registry, root));

    if (const auto* rel = registry.try_get<RelationshipComponent>(root))
    {
        for (entt::entity child : rel->Children)
            CaptureSubtree(registry, child, out);
    }
}

void EditorHistory::RestoreSnapshots(Scene& scene,
                                     const std::vector<EntitySnapshot>& snapshots)
{
    entt::registry& registry = scene.GetRegistry();

    // Pass 1 — revive every entity handle so cross-references (relationship
    // lists) resolve during component restore.
    for (const EntitySnapshot& s : snapshots)
    {
        if (s.Handle == entt::null || registry.valid(s.Handle)) continue;
        static_cast<void>(registry.create(s.Handle));
    }

    // Pass 2 — restore component values verbatim.
    for (const EntitySnapshot& s : snapshots)
    {
        if (!registry.valid(s.Handle)) continue;

        if (s.Tag)            registry.emplace_or_replace<TagComponent>(s.Handle, *s.Tag);
        if (s.Transform)      registry.emplace_or_replace<TransformComponent>(s.Handle, *s.Transform);
        if (s.Relationship)   registry.emplace_or_replace<RelationshipComponent>(s.Handle, *s.Relationship);
        if (s.Mesh)           registry.emplace_or_replace<MeshComponent>(s.Handle, *s.Mesh);
        if (s.Light)          registry.emplace_or_replace<LightComponent>(s.Handle, *s.Light);
        if (s.Camera)         registry.emplace_or_replace<CameraComponent>(s.Handle, *s.Camera);
        if (s.RigidBody3D)    registry.emplace_or_replace<RigidBody3DComponent>(s.Handle, *s.RigidBody3D);
        if (s.BoxCollider3D)  registry.emplace_or_replace<BoxCollider3DComponent>(s.Handle, *s.BoxCollider3D);
        if (s.SphereCollider3D) registry.emplace_or_replace<SphereCollider3DComponent>(s.Handle, *s.SphereCollider3D);
        if (s.RigidBody2D)    registry.emplace_or_replace<RigidBody2DComponent>(s.Handle, *s.RigidBody2D);
        if (s.BoxCollider2D)  registry.emplace_or_replace<BoxCollider2DComponent>(s.Handle, *s.BoxCollider2D);
        if (s.Script)         registry.emplace_or_replace<ScriptComponent>(s.Handle, *s.Script);
        if (s.NativeScript)   registry.emplace_or_replace<NativeScriptComponent>(s.Handle, *s.NativeScript);
    }

    // Pass 3 — re-link the subtree root into its outer parent's child list
    // (the parent survived the deletion, but our handle was removed from it).
    if (!snapshots.empty())
    {
        const EntitySnapshot& root = snapshots.front();
        if (root.Relationship && root.Relationship->Parent != entt::null &&
            registry.valid(root.Relationship->Parent))
        {
            auto& parentRel = registry.get_or_emplace<RelationshipComponent>(
                root.Relationship->Parent);
            auto& kids = parentRel.Children;
            if (std::find(kids.begin(), kids.end(), root.Handle) == kids.end())
                kids.push_back(root.Handle);
        }
    }
}

} // namespace fadix

#endif // FADIX_EDITOR
