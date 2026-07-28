#include "runtime/World.hpp"

#include "runtime/Components.hpp"

#include <glm/gtc/constants.hpp>

#include <stdexcept>

namespace fadix
{
World::World(const bool createDefaultScene)
{
    if (!createDefaultScene)
    {
        return;
    }

    const entt::entity root = Create();
    m_Registry.emplace<NameComponent>(root, "Main Scene");
    m_Registry.emplace<TransformComponent>(root);

    const Uuid rootId = GetUuid(root).value();
    const entt::entity camera = Create();
    m_Registry.emplace<NameComponent>(camera, "Player Camera");
    m_Registry.emplace<TransformComponent>(camera, glm::vec3{0.0F, 2.0F, 6.0F});
    m_Registry.emplace<RelationshipComponent>(camera, rootId);
    m_Registry.emplace<CameraComponent>(camera, 60.0F, 0.05F, 5000.0F, true);
    m_Registry.emplace<NetworkIdentityComponent>(camera, std::uint64_t{1}, NetworkAuthority::Owner, true);

    const glm::quat sunRotation = glm::quat{glm::radians(glm::vec3{-45.0F, -30.0F, 0.0F})};
    const entt::entity sun = Create();
    m_Registry.emplace<NameComponent>(sun, "Sun Light");
    m_Registry.emplace<TransformComponent>(sun,
        TransformComponent{glm::vec3{2.0F, 4.0F, 1.0F}, sunRotation});
    m_Registry.emplace<RelationshipComponent>(sun, rootId);
    m_Registry.emplace<DirectionalLightComponent>(sun, MakeSunLight());

    const entt::entity moon = Create();
    m_Registry.emplace<NameComponent>(moon, "Moon Light");
    m_Registry.emplace<TransformComponent>(moon,
        TransformComponent{glm::vec3{-2.0F, 4.0F, -1.0F},
            sunRotation * glm::angleAxis(glm::pi<float>(), glm::vec3{0.0F, 1.0F, 0.0F})});
    m_Registry.emplace<RelationshipComponent>(moon, rootId);
    m_Registry.emplace<DirectionalLightComponent>(moon, MakeMoonLight());

    const entt::entity cube = Create();
    m_Registry.emplace<NameComponent>(cube, "Cube");
    m_Registry.emplace<TransformComponent>(cube, glm::vec3{0.0F, 0.5F, 0.0F});
    m_Registry.emplace<RelationshipComponent>(cube, rootId);
    m_Registry.emplace<MeshComponent>(cube);
}

entt::registry& World::Registry() noexcept
{
    return m_Registry;
}

const entt::registry& World::Registry() const noexcept
{
    return m_Registry;
}

entt::entity World::Create(Uuid id)
{
    if (!id.IsValid())
    {
        throw std::invalid_argument("World entity UUID must be valid and unique");
    }
    if (const auto existing = m_Entities.find(id); existing != m_Entities.end())
    {
        if (m_Registry.valid(existing->second))
        {
            throw std::invalid_argument("World entity UUID must be valid and unique");
        }
        m_Entities.erase(existing);
    }
    const entt::entity entity = m_Registry.create();
    m_Registry.emplace<UuidComponent>(entity, id);
    m_Entities.emplace(id, entity);
    return entity;
}

std::optional<entt::entity> World::Find(const Uuid& id) const
{
    const auto iterator = m_Entities.find(id);
    return iterator == m_Entities.end() || !m_Registry.valid(iterator->second)
        ? std::nullopt
        : std::optional{iterator->second};
}

std::optional<Uuid> World::GetUuid(const entt::entity entity) const
{
    if (!m_Registry.valid(entity))
    {
        return std::nullopt;
    }
    const UuidComponent* id = m_Registry.try_get<UuidComponent>(entity);
    return id == nullptr ? std::nullopt : std::optional{id->Id};
}

void World::Clear()
{
    m_Registry.clear();
    m_Entities.clear();
}

std::unique_ptr<IWorld> World::Clone() const
{
    auto result = std::make_unique<World>(false);
    for (const auto [entity, uuid] : m_Registry.view<UuidComponent>().each())
    {
        const entt::entity destination = result->Create(uuid.Id);
#define FADIX_CLONE_COMPONENT(Type) \
        if (const Type* value = m_Registry.try_get<Type>(entity)) result->m_Registry.emplace<Type>(destination, *value)
        FADIX_CLONE_COMPONENT(NameComponent);
        FADIX_CLONE_COMPONENT(TransformComponent);
        FADIX_CLONE_COMPONENT(RelationshipComponent);
        FADIX_CLONE_COMPONENT(MeshComponent);
        FADIX_CLONE_COMPONENT(DirectionalLightComponent);
        FADIX_CLONE_COMPONENT(PointLightComponent);
        FADIX_CLONE_COMPONENT(SpotLightComponent);
        FADIX_CLONE_COMPONENT(EnvironmentComponent);
        FADIX_CLONE_COMPONENT(VisibilityComponent);
        FADIX_CLONE_COMPONENT(CameraComponent);
        FADIX_CLONE_COMPONENT(NetworkIdentityComponent);
        FADIX_CLONE_COMPONENT(JoltBodyComponent);
        FADIX_CLONE_COMPONENT(CharacterControllerComponent);
        FADIX_CLONE_COMPONENT(Box2DBodyComponent);
        FADIX_CLONE_COMPONENT(ScriptComponent);
        FADIX_CLONE_COMPONENT(ParticleEmitterComponent);
        FADIX_CLONE_COMPONENT(AudioSourceComponent);
        FADIX_CLONE_COMPONENT(AudioListenerComponent);
        FADIX_CLONE_COMPONENT(UICanvasComponent);
        FADIX_CLONE_COMPONENT(TerrainComponent);
        FADIX_CLONE_COMPONENT(SkeletonComponent);
        FADIX_CLONE_COMPONENT(AnimatorComponent);
        FADIX_CLONE_COMPONENT(TransformAnimatorComponent);
#undef FADIX_CLONE_COMPONENT
        if (auto* body = result->m_Registry.try_get<JoltBodyComponent>(destination))
        {
            body->Handle = InvalidPhysicsBody;
        }
        if (auto* character = result->m_Registry.try_get<CharacterControllerComponent>(destination))
        {
            character->MoveInput = {0.0F, 0.0F};
            character->JumpRequested = false;
            character->Grounded = false;
        }
        if (auto* body = result->m_Registry.try_get<Box2DBodyComponent>(destination))
        {
            body->Handle = InvalidPhysicsBody;
        }
        if (auto* audio = result->m_Registry.try_get<AudioSourceComponent>(destination))
        {
            audio->ActiveTrack = -1;
        }
        if (auto* animator = result->m_Registry.try_get<AnimatorComponent>(destination))
        {
            animator->ClipIndex = -1;
            animator->CurrentTime = 0.0F;
        }
    }
    return result;
}

std::size_t World::EntityCount() const
{
    return m_Registry.view<UuidComponent>().size();
}

std::unique_ptr<IWorld> CreateEditorWorld()
{
    return std::make_unique<World>();
}
}
