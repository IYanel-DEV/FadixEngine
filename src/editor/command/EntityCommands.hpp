#pragma once

#include "engine/command/ICommand.hpp"
#include "engine/Uuid.hpp"
#include "runtime/Components.hpp"

#include <entt/entity/entity.hpp>

#include <optional>
#include <string>
#include <vector>

namespace fadix
{
class IWorld;

struct EntitySnapshot
{
    Uuid Id;
    std::optional<NameComponent> Name;
    std::optional<TransformComponent> Transform;
    std::optional<RelationshipComponent> Relationship;
    std::optional<MeshComponent> Mesh;
    std::optional<DirectionalLightComponent> DirectionalLight;
    std::optional<PointLightComponent> PointLight;
    std::optional<SpotLightComponent> SpotLight;
    std::optional<EnvironmentComponent> Environment;
    std::optional<VisibilityComponent> Visibility;
    std::optional<CameraComponent> Camera;
    std::optional<NetworkIdentityComponent> Network;
    std::optional<JoltBodyComponent> JoltBody;
    std::optional<CharacterControllerComponent> CharacterController;
    std::optional<Box2DBodyComponent> Box2DBody;
    std::optional<ScriptComponent> Script;
    std::optional<ParticleEmitterComponent> ParticleEmitter;
    std::optional<AudioSourceComponent> AudioSource;
    std::optional<AudioListenerComponent> AudioListener;
    std::optional<UICanvasComponent> UICanvas;
    std::optional<TerrainComponent> Terrain;
    std::optional<SkeletonComponent> Skeleton;
    std::optional<AnimatorComponent> Animator;
    std::optional<TransformAnimatorComponent> TransformAnimator;
    std::optional<Sprite2DComponent> Sprite2D;
    std::optional<RigidBody2DComponent> RigidBody2D;
    std::optional<Collider2DComponent> Collider2D;
    std::optional<SpriteFrameAnimatorComponent> SpriteFrameAnimator;
    std::optional<TileMapComponent> TileMap;

    [[nodiscard]] static std::optional<EntitySnapshot> Capture(const IWorld& world, const Uuid& id);
    [[nodiscard]] entt::entity Restore(IWorld& world) const;
};

class SnapshotEntityCommand final : public ICommand
{
public:
    SnapshotEntityCommand(IWorld& world, EntitySnapshot before, EntitySnapshot after, std::string name);
    void Execute() override;
    void Undo() override;
    [[nodiscard]] std::string_view Name() const noexcept override;

private:
    IWorld& m_World;
    EntitySnapshot m_Before;
    EntitySnapshot m_After;
    std::string m_Name;
};

// One undoable edit that touches several entities at once (for example "Make
// Primary" clearing the Primary flag on every other Environment component).
class MultiSnapshotCommand final : public ICommand
{
public:
    MultiSnapshotCommand(IWorld& world,
        std::vector<EntitySnapshot> before,
        std::vector<EntitySnapshot> after,
        std::string name);
    void Execute() override;
    void Undo() override;
    [[nodiscard]] std::string_view Name() const noexcept override;

private:
    IWorld& m_World;
    std::vector<EntitySnapshot> m_Before;
    std::vector<EntitySnapshot> m_After;
    std::string m_Name;
};

class TransformEntityCommand final : public ICommand
{
public:
    TransformEntityCommand(
        IWorld& world, Uuid entity, TransformComponent before, TransformComponent after);
    void Execute() override;
    void Undo() override;
    [[nodiscard]] std::string_view Name() const noexcept override;

private:
    void Apply(const TransformComponent& value);
    IWorld& m_World;
    Uuid m_Entity;
    TransformComponent m_Before;
    TransformComponent m_After;
};

class AddEntityCommand final : public ICommand
{
public:
    AddEntityCommand(IWorld& world, std::string name, Uuid parent = {});
    void Execute() override;
    void Undo() override;
    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] const Uuid& EntityId() const noexcept;
    void SetTransform(TransformComponent component);
    void SetMesh(MeshComponent component);
    void SetDirectionalLight(DirectionalLightComponent component);
    void SetPointLight(PointLightComponent component);
    void SetSpotLight(SpotLightComponent component);
    void SetEnvironment(EnvironmentComponent component);
    void SetCamera(CameraComponent component);
    void SetJoltBody(JoltBodyComponent component);
    void SetBox2DBody(Box2DBodyComponent component);
    void SetSprite2D(Sprite2DComponent component);
    void SetTileMap(TileMapComponent component);
    void SetRigidBody2D(RigidBody2DComponent component);
    void SetCollider2D(Collider2DComponent component);
    void SetSpriteFrameAnimator(SpriteFrameAnimatorComponent component);

private:
    IWorld& m_World;
    EntitySnapshot m_Entity;
};

class DeleteEntityCommand final : public ICommand
{
public:
    DeleteEntityCommand(IWorld& world, Uuid entity);
    void Execute() override;
    void Undo() override;
    [[nodiscard]] std::string_view Name() const noexcept override;

private:
    IWorld& m_World;
    Uuid m_Entity;
    std::vector<EntitySnapshot> m_Deleted;
};

class DuplicateEntityCommand final : public ICommand
{
public:
    DuplicateEntityCommand(IWorld& world, Uuid source);
    void Execute() override;
    void Undo() override;
    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] const Uuid& DuplicateId() const noexcept;

private:
    IWorld& m_World;
    Uuid m_Source;
    EntitySnapshot m_Duplicate;
    bool m_Captured{false};
};

class RenameEntityCommand final : public ICommand
{
public:
    RenameEntityCommand(IWorld& world, Uuid entity, std::string name);
    void Execute() override;
    void Undo() override;
    [[nodiscard]] std::string_view Name() const noexcept override;

private:
    IWorld& m_World;
    Uuid m_Entity;
    std::string m_OldName;
    std::string m_NewName;
};

class SelectEntityCommand final : public ICommand
{
public:
    SelectEntityCommand(std::optional<Uuid>& selection, std::optional<Uuid> entity);
    void Execute() override;
    void Undo() override;
    [[nodiscard]] std::string_view Name() const noexcept override;

private:
    std::optional<Uuid>& m_Selection;
    std::optional<Uuid> m_OldSelection;
    std::optional<Uuid> m_NewSelection;
};

class ReparentEntityCommand final : public ICommand
{
public:
    ReparentEntityCommand(IWorld& world, Uuid entity, Uuid parent);
    void Execute() override;
    void Undo() override;
    [[nodiscard]] std::string_view Name() const noexcept override;

private:
    void Apply(const Uuid& parent);
    IWorld& m_World;
    Uuid m_Entity;
    Uuid m_OldParent;
    Uuid m_NewParent;
};
}
