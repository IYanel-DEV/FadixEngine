#include "editor/command/EntityCommands.hpp"

#include "engine/scene/IWorld.hpp"
#include "runtime/Components.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

namespace fadix
{
namespace
{
template <typename Component>
std::optional<Component> CopyComponent(const entt::registry& registry, const entt::entity entity)
{
    const Component* component = registry.try_get<Component>(entity);
    return component == nullptr ? std::nullopt : std::optional<Component>{*component};
}

template <typename Component>
void RestoreComponent(entt::registry& registry, const entt::entity entity, const std::optional<Component>& component)
{
    if (component)
    {
        registry.emplace_or_replace<Component>(entity, *component);
    }
    else
    {
        registry.remove<Component>(entity);
    }
}

void Destroy(IWorld& world, const Uuid& id)
{
    if (const auto entity = world.Find(id))
    {
        world.Registry().destroy(*entity);
        // World removes lookup entries lazily through Clear/Create, so command deletion
        // recreates via a fresh UUID only after World::Find no longer sees a valid entity.
    }
}

// "Point Light Copy Copy" / "Mesh (3)" -> "Point Light" / "Mesh" so the next
// duplicate becomes "Name (2)", "Name (3)", ... instead of endless " Copy".
[[nodiscard]] std::string StripDuplicateDecorations(std::string name)
{
    for (;;)
    {
        constexpr std::string_view kCopy = " Copy";
        if (name.size() >= kCopy.size() &&
            name.compare(name.size() - kCopy.size(), kCopy.size(), kCopy) == 0)
        {
            name.resize(name.size() - kCopy.size());
            continue;
        }
        break;
    }
    if (name.size() >= 4 && name.back() == ')')
    {
        const auto open = name.rfind(" (");
        if (open != std::string::npos && open + 2 < name.size())
        {
            bool digits = true;
            for (std::size_t i = open + 2; i + 1 < name.size(); ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(name[i])))
                {
                    digits = false;
                    break;
                }
            }
            if (digits)
            {
                name.resize(open);
            }
        }
    }
    return name;
}

[[nodiscard]] std::string MakeNumberedDuplicateName(IWorld& world, std::string sourceName)
{
    const std::string base = StripDuplicateDecorations(std::move(sourceName));
    std::unordered_set<std::string> used;
    for (const auto [entity, name] : world.Registry().view<const NameComponent>().each())
    {
        (void)entity;
        used.insert(name.Name);
    }
    for (int n = 2; n < 100000; ++n)
    {
        std::string candidate = base + " (" + std::to_string(n) + ")";
        if (!used.contains(candidate))
        {
            return candidate;
        }
    }
    return base + " (" + Uuid::Generate().ToString() + ")";
}
}

std::optional<EntitySnapshot> EntitySnapshot::Capture(const IWorld& world, const Uuid& id)
{
    const auto entity = world.Find(id);
    if (!entity || !world.Registry().valid(*entity))
    {
        return std::nullopt;
    }
    const entt::registry& registry = world.Registry();
    return EntitySnapshot{id,
        CopyComponent<NameComponent>(registry, *entity),
        CopyComponent<TransformComponent>(registry, *entity),
        CopyComponent<RelationshipComponent>(registry, *entity),
        CopyComponent<MeshComponent>(registry, *entity),
        CopyComponent<DirectionalLightComponent>(registry, *entity),
        CopyComponent<PointLightComponent>(registry, *entity),
        CopyComponent<SpotLightComponent>(registry, *entity),
        CopyComponent<EnvironmentComponent>(registry, *entity),
        CopyComponent<VisibilityComponent>(registry, *entity),
        CopyComponent<CameraComponent>(registry, *entity),
        CopyComponent<NetworkIdentityComponent>(registry, *entity),
        CopyComponent<JoltBodyComponent>(registry, *entity),
        CopyComponent<CharacterControllerComponent>(registry, *entity),
        CopyComponent<Box2DBodyComponent>(registry, *entity),
        CopyComponent<ScriptComponent>(registry, *entity),
        CopyComponent<ParticleEmitterComponent>(registry, *entity),
        CopyComponent<AudioSourceComponent>(registry, *entity),
        CopyComponent<AudioListenerComponent>(registry, *entity),
        CopyComponent<UICanvasComponent>(registry, *entity),
        CopyComponent<TerrainComponent>(registry, *entity),
        CopyComponent<SkeletonComponent>(registry, *entity),
        CopyComponent<AnimatorComponent>(registry, *entity),
        CopyComponent<TransformAnimatorComponent>(registry, *entity)};
}

entt::entity EntitySnapshot::Restore(IWorld& world) const
{
    entt::entity entity{};
    if (const auto existing = world.Find(Id); existing && world.Registry().valid(*existing))
    {
        entity = *existing;
    }
    else
    {
        entity = world.Create(Id);
    }
    entt::registry& registry = world.Registry();
    RestoreComponent(registry, entity, Name);
    RestoreComponent(registry, entity, Transform);
    RestoreComponent(registry, entity, Relationship);
    RestoreComponent(registry, entity, Mesh);
    RestoreComponent(registry, entity, DirectionalLight);
    RestoreComponent(registry, entity, PointLight);
    RestoreComponent(registry, entity, SpotLight);
    RestoreComponent(registry, entity, Environment);
    RestoreComponent(registry, entity, Visibility);
    RestoreComponent(registry, entity, Camera);
    RestoreComponent(registry, entity, Network);
    RestoreComponent(registry, entity, JoltBody);
    RestoreComponent(registry, entity, CharacterController);
    RestoreComponent(registry, entity, Box2DBody);
    RestoreComponent(registry, entity, Script);
    RestoreComponent(registry, entity, ParticleEmitter);
    RestoreComponent(registry, entity, AudioSource);
    RestoreComponent(registry, entity, AudioListener);
    RestoreComponent(registry, entity, UICanvas);
    RestoreComponent(registry, entity, Terrain);
    RestoreComponent(registry, entity, Skeleton);
    RestoreComponent(registry, entity, Animator);
    RestoreComponent(registry, entity, TransformAnimator);
    return entity;
}

SnapshotEntityCommand::SnapshotEntityCommand(
    IWorld& world, EntitySnapshot before, EntitySnapshot after, std::string name)
    : m_World(world), m_Before(std::move(before)), m_After(std::move(after)), m_Name(std::move(name))
{
}

void SnapshotEntityCommand::Execute() { static_cast<void>(m_After.Restore(m_World)); }
void SnapshotEntityCommand::Undo() { static_cast<void>(m_Before.Restore(m_World)); }
std::string_view SnapshotEntityCommand::Name() const noexcept { return m_Name; }

MultiSnapshotCommand::MultiSnapshotCommand(IWorld& world,
    std::vector<EntitySnapshot> before,
    std::vector<EntitySnapshot> after,
    std::string name)
    : m_World(world), m_Before(std::move(before)), m_After(std::move(after)), m_Name(std::move(name))
{
}

void MultiSnapshotCommand::Execute()
{
    for (const EntitySnapshot& snapshot : m_After)
    {
        static_cast<void>(snapshot.Restore(m_World));
    }
}

void MultiSnapshotCommand::Undo()
{
    for (const EntitySnapshot& snapshot : m_Before)
    {
        static_cast<void>(snapshot.Restore(m_World));
    }
}

std::string_view MultiSnapshotCommand::Name() const noexcept { return m_Name; }

TransformEntityCommand::TransformEntityCommand(
    IWorld& world,
    const Uuid entity,
    const TransformComponent before,
    const TransformComponent after)
    : m_World(world), m_Entity(entity), m_Before(before), m_After(after)
{
}

void TransformEntityCommand::Apply(const TransformComponent& value)
{
    if (const auto entity = m_World.Find(m_Entity))
    {
        m_World.Registry().emplace_or_replace<TransformComponent>(*entity, value);
    }
}

void TransformEntityCommand::Execute() { Apply(m_After); }
void TransformEntityCommand::Undo() { Apply(m_Before); }
std::string_view TransformEntityCommand::Name() const noexcept { return "Transform Entity"; }

AddEntityCommand::AddEntityCommand(IWorld& world, std::string name, const Uuid parent)
    : m_World(world)
{
    m_Entity.Id = Uuid::Generate();
    m_Entity.Name = NameComponent{std::move(name)};
    m_Entity.Transform = TransformComponent{};
    if (parent.IsValid())
    {
        m_Entity.Relationship = RelationshipComponent{parent};
    }
}

void AddEntityCommand::Execute() { static_cast<void>(m_Entity.Restore(m_World)); }
void AddEntityCommand::Undo() { Destroy(m_World, m_Entity.Id); }
std::string_view AddEntityCommand::Name() const noexcept { return "Add Entity"; }
const Uuid& AddEntityCommand::EntityId() const noexcept { return m_Entity.Id; }
void AddEntityCommand::SetTransform(TransformComponent component) { m_Entity.Transform = std::move(component); }
void AddEntityCommand::SetMesh(MeshComponent component) { m_Entity.Mesh = std::move(component); }
void AddEntityCommand::SetDirectionalLight(DirectionalLightComponent component) { m_Entity.DirectionalLight = std::move(component); }
void AddEntityCommand::SetPointLight(PointLightComponent component) { m_Entity.PointLight = std::move(component); }
void AddEntityCommand::SetSpotLight(SpotLightComponent component) { m_Entity.SpotLight = std::move(component); }
void AddEntityCommand::SetEnvironment(EnvironmentComponent component) { m_Entity.Environment = std::move(component); }
void AddEntityCommand::SetCamera(CameraComponent component) { m_Entity.Camera = std::move(component); }
void AddEntityCommand::SetJoltBody(JoltBodyComponent component) { m_Entity.JoltBody = std::move(component); }
void AddEntityCommand::SetBox2DBody(Box2DBodyComponent component) { m_Entity.Box2DBody = std::move(component); }

DeleteEntityCommand::DeleteEntityCommand(IWorld& world, const Uuid entity)
    : m_World(world), m_Entity(entity)
{
}

void DeleteEntityCommand::Execute()
{
    if (m_Deleted.empty())
    {
        std::vector<Uuid> pending{m_Entity};
        while (!pending.empty())
        {
            const Uuid current = pending.back();
            pending.pop_back();
            if (const auto snapshot = EntitySnapshot::Capture(m_World, current))
            {
                m_Deleted.push_back(*snapshot);
                for (const auto [entity, relationship, id] :
                     m_World.Registry().view<const RelationshipComponent, const UuidComponent>().each())
                {
                    if (relationship.Parent == current)
                    {
                        pending.push_back(id.Id);
                    }
                    static_cast<void>(entity);
                }
            }
        }
    }
    for (auto iterator = m_Deleted.rbegin(); iterator != m_Deleted.rend(); ++iterator)
    {
        Destroy(m_World, iterator->Id);
    }
}

void DeleteEntityCommand::Undo()
{
    for (const EntitySnapshot& snapshot : m_Deleted)
    {
        static_cast<void>(snapshot.Restore(m_World));
    }
}

std::string_view DeleteEntityCommand::Name() const noexcept { return "Delete Entity"; }

DuplicateEntityCommand::DuplicateEntityCommand(IWorld& world, const Uuid source)
    : m_World(world), m_Source(source)
{
    const auto snapshot = EntitySnapshot::Capture(m_World, m_Source);
    if (!snapshot)
    {
        return;
    }
    m_Duplicate = *snapshot;
    m_Duplicate.Id = Uuid::Generate();
    if (m_Duplicate.Name)
    {
        m_Duplicate.Name->Name = MakeNumberedDuplicateName(m_World, m_Duplicate.Name->Name);
    }
    if (m_Duplicate.Network)
    {
        m_Duplicate.Network->NetworkId = 0;
    }
    if (m_Duplicate.JoltBody)
    {
        m_Duplicate.JoltBody->Handle = InvalidPhysicsBody;
    }
    if (m_Duplicate.Box2DBody)
    {
        m_Duplicate.Box2DBody->Handle = InvalidPhysicsBody;
    }
    m_Captured = true;
}

void DuplicateEntityCommand::Execute()
{
    if (m_Captured)
    {
        static_cast<void>(m_Duplicate.Restore(m_World));
    }
}

void DuplicateEntityCommand::Undo() { Destroy(m_World, m_Duplicate.Id); }
std::string_view DuplicateEntityCommand::Name() const noexcept { return "Duplicate Entity"; }
const Uuid& DuplicateEntityCommand::DuplicateId() const noexcept { return m_Duplicate.Id; }

RenameEntityCommand::RenameEntityCommand(IWorld& world, const Uuid entity, std::string name)
    : m_World(world), m_Entity(entity), m_NewName(std::move(name))
{
    if (const auto found = m_World.Find(entity))
    {
        if (const NameComponent* component = m_World.Registry().try_get<NameComponent>(*found))
        {
            m_OldName = component->Name;
        }
    }
}

void RenameEntityCommand::Execute()
{
    if (const auto entity = m_World.Find(m_Entity); entity && m_World.Registry().valid(*entity))
    {
        m_World.Registry().emplace_or_replace<NameComponent>(*entity, m_NewName);
    }
}

void RenameEntityCommand::Undo()
{
    if (const auto entity = m_World.Find(m_Entity); entity && m_World.Registry().valid(*entity))
    {
        m_World.Registry().emplace_or_replace<NameComponent>(*entity, m_OldName);
    }
}

std::string_view RenameEntityCommand::Name() const noexcept { return "Rename Entity"; }

SelectEntityCommand::SelectEntityCommand(std::optional<Uuid>& selection, std::optional<Uuid> entity)
    : m_Selection(selection), m_OldSelection(selection), m_NewSelection(std::move(entity))
{
}

void SelectEntityCommand::Execute() { m_Selection = m_NewSelection; }
void SelectEntityCommand::Undo() { m_Selection = m_OldSelection; }
std::string_view SelectEntityCommand::Name() const noexcept { return "Select Entity"; }

ReparentEntityCommand::ReparentEntityCommand(IWorld& world, const Uuid entity, const Uuid parent)
    : m_World(world), m_Entity(entity), m_NewParent(parent)
{
    if (const auto found = world.Find(entity))
    {
        if (const RelationshipComponent* relationship = world.Registry().try_get<RelationshipComponent>(*found))
        {
            m_OldParent = relationship->Parent;
        }
    }
}

void ReparentEntityCommand::Apply(const Uuid& parent)
{
    if (parent == m_Entity)
    {
        return;
    }
    Uuid ancestor = parent;
    while (ancestor.IsValid())
    {
        if (ancestor == m_Entity)
        {
            return;
        }
        const auto entity = m_World.Find(ancestor);
        if (!entity)
        {
            break;
        }
        const RelationshipComponent* relationship =
            m_World.Registry().try_get<RelationshipComponent>(*entity);
        ancestor = relationship == nullptr ? Uuid{} : relationship->Parent;
    }
    const auto entity = m_World.Find(m_Entity);
    if (!entity || !m_World.Registry().valid(*entity))
    {
        return;
    }
    if (parent.IsValid())
    {
        m_World.Registry().emplace_or_replace<RelationshipComponent>(*entity, parent);
    }
    else
    {
        m_World.Registry().remove<RelationshipComponent>(*entity);
    }
}

void ReparentEntityCommand::Execute() { Apply(m_NewParent); }
void ReparentEntityCommand::Undo() { Apply(m_OldParent); }
std::string_view ReparentEntityCommand::Name() const noexcept { return "Reparent Entity"; }
}
