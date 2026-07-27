#include "editor/scene/SceneEditor.hpp"

#include "assets/AssetDatabase.hpp"
#include "assets/GltfMeshCache.hpp"
#include "editor/command/EntityCommands.hpp"
#include "engine/assets/MaterialAsset.hpp"
#include "engine/command/UndoStack.hpp"
#include "engine/scene/IWorld.hpp"
#include "runtime/Components.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>

namespace fadix
{
namespace
{
std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

const char* EntityIcon(const entt::registry& registry, entt::entity entity, bool isRoot)
{
    if (registry.all_of<CameraComponent>(entity)) return "camera";
    if (registry.all_of<DirectionalLightComponent>(entity)) return "light";
    if (registry.all_of<PointLightComponent>(entity)) return "point-light";
    if (registry.all_of<SpotLightComponent>(entity)) return "spot-light";
    if (registry.all_of<EnvironmentComponent>(entity)) return "environment";
    if (registry.all_of<MeshComponent>(entity)) return "mesh";
    if (registry.any_of<JoltBodyComponent, CharacterControllerComponent>(entity)) return "physics3d";
    if (registry.all_of<Box2DBodyComponent>(entity)) return "physics2d";
    if (registry.all_of<NetworkIdentityComponent>(entity)) return "network";
    return isRoot ? "scene" : "entity";
}
} // namespace

SceneEditor::SceneEditor(IWorld& world, UndoStack& history) : m_World(world), m_History(history) {}

IWorld& SceneEditor::World() noexcept { return m_World; }
UndoStack& SceneEditor::History() noexcept { return m_History; }
std::optional<Uuid> SceneEditor::Selection() const { return m_Selection; }
const std::string& SceneEditor::Filter() const noexcept { return m_Filter; }
AssetDatabase* SceneEditor::Assets() const noexcept { return m_Assets; }
GltfMeshCache* SceneEditor::GltfMeshes() const noexcept { return m_GltfMeshes; }

void SceneEditor::SetSelection(std::optional<Uuid> selection, const bool recordUndo)
{
    if (recordUndo && selection)
    {
        m_History.Push(std::make_unique<SelectEntityCommand>(m_Selection, std::move(*selection)));
    }
    else
    {
        m_Selection = std::move(selection);
    }
}

void SceneEditor::SetFilter(std::string filter)
{
    m_Filter = Lower(std::move(filter));
}

void SceneEditor::SetChangedCallback(std::function<void()> callback)
{
    m_Changed = std::move(callback);
}

void SceneEditor::SetAssetDatabase(AssetDatabase* database) { m_Assets = database; }
void SceneEditor::SetGltfMeshCache(GltfMeshCache* cache) { m_GltfMeshes = cache; }

void SceneEditor::SetSelectedMaterialProvider(std::function<std::optional<AssetHandle>()> provider)
{
    m_SelectedMaterial = std::move(provider);
}

void SceneEditor::SetSelectedMeshProvider(std::function<std::optional<AssetHandle>()> provider)
{
    m_SelectedMesh = std::move(provider);
}

void SceneEditor::SetStatusReporter(std::function<void(std::string_view)> reporter)
{
    m_ReportStatus = std::move(reporter);
}

void SceneEditor::MarkChanged()
{
    if (m_Changed)
    {
        m_Changed();
    }
}

void SceneEditor::Report(const std::string_view message) const
{
    if (m_ReportStatus)
    {
        m_ReportStatus(message);
    }
}

std::optional<Uuid> SceneEditor::SceneRootId() const
{
    std::optional<Uuid> fallback;
    for (const auto [entity, id] : m_World.Registry().view<const UuidComponent>().each())
    {
        const NameComponent* name = m_World.Registry().try_get<NameComponent>(entity);
        if (name != nullptr && name->Name == "Main Scene")
        {
            return id.Id;
        }
        if (!fallback && !m_World.Registry().all_of<RelationshipComponent>(entity))
        {
            fallback = id.Id;
        }
    }
    return fallback;
}

bool SceneEditor::IsSceneRoot(const Uuid& id) const
{
    const std::optional<Uuid> root = SceneRootId();
    return root.has_value() && *root == id;
}

void SceneEditor::EnsureOrphansAdopted()
{
    const std::optional<Uuid> rootId = SceneRootId();
    if (!rootId)
    {
        return;
    }
    bool adopted = false;
    for (const auto [entity, id] : m_World.Registry().view<const UuidComponent>().each())
    {
        if (id.Id == *rootId)
        {
            continue;
        }
        if (RelationshipComponent* rel =
                m_World.Registry().try_get<RelationshipComponent>(entity))
        {
            // Empty parent = orphan; treat as child of Main Scene for hierarchy.
            if (!rel->Parent.IsValid() || rel->Parent == id.Id)
            {
                rel->Parent = *rootId;
                adopted = true;
            }
        }
        else
        {
            m_World.Registry().emplace<RelationshipComponent>(entity, *rootId);
            adopted = true;
        }
    }
    if (adopted)
    {
        MarkChanged();
    }
}

std::vector<SceneEditor::HierarchyNode> SceneEditor::BuildHierarchy() const
{
    const std::optional<Uuid> rootId = SceneRootId();

    struct Info
    {
        std::string Name;
        Uuid ParentId;
        const char* Icon;
        bool IsRoot;
    };
    std::vector<std::pair<Uuid, Info>> all;
    all.reserve(64);

    for (const auto [entity, id] : m_World.Registry().view<const UuidComponent>().each())
    {
        const NameComponent* name = m_World.Registry().try_get<NameComponent>(entity);
        const std::string displayName = name ? name->Name : "Entity";
        const bool isRoot = rootId && id.Id == *rootId;
        const char* icon = EntityIcon(m_World.Registry(), entity, isRoot);
        const RelationshipComponent* rel =
            m_World.Registry().try_get<RelationshipComponent>(entity);
        const Uuid parentId = rel ? rel->Parent : Uuid{};
        all.push_back({id.Id, Info{displayName, parentId, icon, isRoot}});
    }

    std::vector<HierarchyNode> result;
    result.reserve(all.size());

    // DFS from a given node, collecting children at each step.
    std::function<void(const Uuid&, int)> visit = [&](const Uuid& nodeId, int depth) {
        auto it =
            std::find_if(all.begin(), all.end(), [&](const auto& p) { return p.first == nodeId; });
        if (it == all.end())
        {
            return;
        }
        const Info& info = it->second;
        result.push_back({nodeId, info.Name, info.ParentId, info.Icon, depth, info.IsRoot});

        // Collect and sort children alphabetically.
        std::vector<std::pair<Uuid, std::string>> kids;
        for (const auto& [childId, childInfo] : all)
        {
            if (childInfo.ParentId == nodeId)
            {
                kids.push_back({childId, childInfo.Name});
            }
        }
        std::sort(kids.begin(), kids.end(), [](const auto& a, const auto& b) {
            return Lower(a.second) < Lower(b.second);
        });
        for (const auto& [childId, _] : kids)
        {
            visit(childId, depth + 1);
        }
    };

    if (rootId)
    {
        visit(*rootId, 0);
    }
    // Orphans (should be empty after EnsureOrphansAdopted, but be defensive).
    for (const auto& [id, info] : all)
    {
        if (!info.ParentId.IsValid() && !(rootId && id == *rootId))
        {
            visit(id, 0);
        }
    }

    // Apply filter: keep nodes whose name matches or that are an ancestor of a match.
    if (!m_Filter.empty())
    {
        std::vector<bool> keep(result.size(), false);
        for (std::size_t i = 0; i < result.size(); ++i)
        {
            if (Lower(result[i].Name).find(m_Filter) != std::string::npos)
            {
                keep[i] = true;
            }
        }
        // Mark ancestors of kept nodes.
        for (std::size_t i = 0; i < result.size(); ++i)
        {
            if (!keep[i])
            {
                continue;
            }
            Uuid parentId = result[i].Parent;
            while (parentId.IsValid())
            {
                bool found = false;
                for (std::size_t j = 0; j < result.size(); ++j)
                {
                    if (result[j].Id == parentId)
                    {
                        keep[j] = true;
                        parentId = result[j].Parent;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    break;
                }
            }
        }
        std::vector<HierarchyNode> filtered;
        filtered.reserve(result.size());
        for (std::size_t i = 0; i < result.size(); ++i)
        {
            if (keep[i])
            {
                filtered.push_back(std::move(result[i]));
            }
        }
        result = std::move(filtered);
    }

    return result;
}

std::optional<Uuid> SceneEditor::CreateEntity(std::string name)
{
    auto cmd = std::make_unique<AddEntityCommand>(
        m_World, std::move(name), SceneRootId().value_or(Uuid{}));
    const Uuid id = cmd->EntityId();
    m_History.Push(std::move(cmd));
    MarkChanged();
    return id.IsValid() ? std::optional<Uuid>{id} : std::nullopt;
}

std::optional<Uuid> SceneEditor::CreateImportedMeshEntity(
    std::string name, const AssetHandle handle, const glm::vec3& gridPosition)
{
    if (m_Assets == nullptr || m_GltfMeshes == nullptr)
    {
        Report("Mesh importer is not available");
        return std::nullopt;
    }
    const AssetMetadata* metadata = m_Assets->Meta(handle);
    if (metadata == nullptr || metadata->Type != "Mesh")
    {
        Report("Selected asset is not an imported mesh");
        return std::nullopt;
    }
    const std::filesystem::path& path = !metadata->SourcePath.empty()
        ? metadata->SourcePath
        : metadata->ImportedPath;
    const GltfMeshAsset* gltf =
        m_GltfMeshes->Load(handle, path.string(), [this](const std::string& message) {
            Report(message);
        });
    if (gltf == nullptr)
    {
        Report("Could not load imported mesh " + path.filename().string());
        return std::nullopt;
    }

    TransformComponent transform;
    transform.Position = gridPosition;
    transform.Position.y -= gltf->BoundingBoxMin.y;
    MeshComponent mesh;
    mesh.ImportedMesh = handle;

    auto command = std::make_unique<AddEntityCommand>(
        m_World, std::move(name), SceneRootId().value_or(Uuid{}));
    command->SetTransform(transform);
    command->SetMesh(mesh);
    const Uuid id = command->EntityId();
    m_History.Push(std::move(command));
    MarkChanged();
    return id.IsValid() ? std::optional<Uuid>{id} : std::nullopt;
}

bool SceneEditor::DuplicateSelection()
{
    if (!m_Selection)
    {
        return false;
    }
    auto command = std::make_unique<DuplicateEntityCommand>(m_World, *m_Selection);
    const Uuid duplicate = command->DuplicateId();
    m_History.Push(std::move(command));
    if (duplicate.IsValid())
    {
        m_Selection = duplicate;
    }
    MarkChanged();
    return true;
}

bool SceneEditor::DeleteSelection()
{
    if (!m_Selection)
    {
        return false;
    }
    if (IsSceneRoot(*m_Selection))
    {
        Report("Cannot delete the scene root");
        return false;
    }
    m_History.Push(std::make_unique<DeleteEntityCommand>(m_World, *m_Selection));
    m_Selection.reset();
    MarkChanged();
    return true;
}

bool SceneEditor::RenameSelection(std::string name)
{
    if (!m_Selection)
    {
        return false;
    }
    m_History.Push(std::make_unique<RenameEntityCommand>(m_World, *m_Selection, std::move(name)));
    MarkChanged();
    return true;
}

bool SceneEditor::Reparent(const Uuid entity, const Uuid newParent)
{
    if (!entity.IsValid() || !newParent.IsValid() || entity == newParent)
    {
        return false;
    }
    if (IsSceneRoot(entity))
    {
        Report("Cannot reparent the scene root");
        return false;
    }
    // ReparentEntityCommand::Apply handles the ancestor-cycle guard internally.
    m_History.Push(std::make_unique<ReparentEntityCommand>(m_World, entity, newParent));
    MarkChanged();
    return true;
}

bool SceneEditor::AddComponent(const std::string_view id)
{
    if (!m_Selection || id.rfind("add-component-", 0) != 0)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    entt::registry& registry = m_World.Registry();
    if (id == "add-component-mesh") registry.emplace_or_replace<MeshComponent>(*entity);
    else if (id == "add-component-light")
        registry.emplace_or_replace<DirectionalLightComponent>(*entity);
    else if (id == "add-component-pointlight")
        registry.emplace_or_replace<PointLightComponent>(*entity);
    else if (id == "add-component-spotlight")
        registry.emplace_or_replace<SpotLightComponent>(*entity);
    else if (id == "add-component-environment")
        registry.emplace_or_replace<EnvironmentComponent>(*entity);
    else if (id == "add-component-visibility")
        registry.emplace_or_replace<VisibilityComponent>(*entity);
    else if (id == "add-component-camera") registry.emplace_or_replace<CameraComponent>(*entity);
    else if (id == "add-component-particle")
        registry.emplace_or_replace<ParticleEmitterComponent>(*entity);
    else if (id == "add-component-audio-source")
        registry.emplace_or_replace<AudioSourceComponent>(*entity);
    else if (id == "add-component-audio-listener")
        registry.emplace_or_replace<AudioListenerComponent>(*entity);
    else if (id == "add-component-ui-canvas")
        registry.emplace_or_replace<UICanvasComponent>(*entity);
    else if (id == "add-component-terrain")
        registry.emplace_or_replace<TerrainComponent>(*entity);
    else if (id == "add-component-skeleton")
        registry.emplace_or_replace<SkeletonComponent>(*entity);
    else if (id == "add-component-animator")
        registry.emplace_or_replace<AnimatorComponent>(*entity);
    else if (id == "add-component-jolt")
    {
        JoltBodyComponent body;
        if (const MeshComponent* mesh = registry.try_get<MeshComponent>(*entity);
            mesh != nullptr && !mesh->ImportedMesh.IsValid() && mesh->Kind == MeshKind::Plane)
        {
            body.Dynamic = false;
        }
        registry.remove<CharacterControllerComponent>(*entity);
        registry.emplace_or_replace<JoltBodyComponent>(*entity, body);
    }
    else if (id == "add-component-character")
    {
        registry.remove<JoltBodyComponent>(*entity);
        registry.emplace_or_replace<CharacterControllerComponent>(*entity);
    }
    else if (id == "add-component-box2d")
        registry.emplace_or_replace<Box2DBodyComponent>(*entity);
    else if (id == "add-component-network")
        registry.emplace_or_replace<NetworkIdentityComponent>(*entity);
    else if (id == "add-component-script") registry.emplace_or_replace<ScriptComponent>(*entity);
    else return false;

    if (before)
    {
        if (auto after = EntitySnapshot::Capture(m_World, *m_Selection))
        {
            m_History.Push(std::make_unique<SnapshotEntityCommand>(
                m_World, std::move(*before), std::move(*after), "Add Component"));
        }
    }
    MarkChanged();
    return true;
}

bool SceneEditor::RemoveComponent(const std::string_view removeId)
{
    if (!m_Selection || removeId.rfind("remove-", 0) != 0)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    entt::registry& registry = m_World.Registry();
    if (removeId == "remove-mesh") registry.remove<MeshComponent>(*entity);
    else if (removeId == "remove-dirlight") registry.remove<DirectionalLightComponent>(*entity);
    else if (removeId == "remove-pointlight") registry.remove<PointLightComponent>(*entity);
    else if (removeId == "remove-spotlight") registry.remove<SpotLightComponent>(*entity);
    else if (removeId == "remove-environment") registry.remove<EnvironmentComponent>(*entity);
    else if (removeId == "remove-visibility") registry.remove<VisibilityComponent>(*entity);
    else if (removeId == "remove-camera") registry.remove<CameraComponent>(*entity);
    else if (removeId == "remove-jolt") registry.remove<JoltBodyComponent>(*entity);
    else if (removeId == "remove-character") registry.remove<CharacterControllerComponent>(*entity);
    else if (removeId == "remove-box2d") registry.remove<Box2DBodyComponent>(*entity);
    else if (removeId == "remove-script") registry.remove<ScriptComponent>(*entity);
    else if (removeId == "remove-particle") registry.remove<ParticleEmitterComponent>(*entity);
    else if (removeId == "remove-audio-source") registry.remove<AudioSourceComponent>(*entity);
    else if (removeId == "remove-audio-listener") registry.remove<AudioListenerComponent>(*entity);
    else if (removeId == "remove-ui-canvas") registry.remove<UICanvasComponent>(*entity);
    else if (removeId == "remove-terrain") registry.remove<TerrainComponent>(*entity);
    else if (removeId == "remove-skeleton") registry.remove<SkeletonComponent>(*entity);
    else if (removeId == "remove-animator") registry.remove<AnimatorComponent>(*entity);
    else return false;

    if (before)
    {
        if (auto after = EntitySnapshot::Capture(m_World, *m_Selection))
        {
            m_History.Push(std::make_unique<SnapshotEntityCommand>(
                m_World, std::move(*before), std::move(*after), "Remove Component"));
        }
    }
    MarkChanged();
    return true;
}

float SceneEditor::NumericDragSensitivity(const std::string_view id, const float startValue)
{
    if (id == "network-id" || id == "env-priority") return 1.0F;
    if (id == "env-wb-temp") return 25.0F;
    if (id == "env-exposure-ev" || id == "env-wb-tint" || id == "env-contrast" ||
        id == "env-saturation" || id == "env-highlight-compression" ||
        id == "env-environment-intensity" || id == "env-ao-intensity" ||
        id == "env-ao-power")
        return 0.01F;
    if (id == "env-environment-rotation") return 0.5F;
    if (id == "env-ao-radius") return 0.01F;
    if (id == "env-ao-bias") return 0.001F;
    if (id.rfind("rotation-", 0) == 0 || id == "camera-fov") return 0.25F;
    if (id == "camera-far") return std::max(std::abs(startValue) * 0.005F, 1.0F);
    if (id == "camera-near") return std::max(std::abs(startValue) * 0.01F, 0.001F);
    if (id == "light-intensity") return 0.05F;
    if (id == "light-shadow-bias") return 0.0005F;
    if (id == "light-shadow-strength" || id == "light-cascade-lambda" ||
        id == "light-shadow-softness")
        return 0.01F;
    if (id == "light-shadow-distance") return std::max(std::abs(startValue) * 0.01F, 0.5F);
    if (id == "pointlight-intensity" || id == "spotlight-intensity") return 0.1F;
    if (id == "pointlight-shadow-bias" || id == "spotlight-shadow-bias") return 0.0005F;
    if (id == "pointlight-shadow-resolution" || id == "spotlight-shadow-resolution") return 256.0F;
    if (id == "pointlight-shadow-strength" || id == "pointlight-softness" ||
        id == "spotlight-shadow-strength" || id == "spotlight-softness")
        return 0.01F;
    if (id == "pointlight-range" || id == "spotlight-range" ||
        id == "env-fog-start" || id == "env-fog-end")
        return std::max(std::abs(startValue) * 0.01F, 0.05F);
    if (id == "spotlight-inner" || id == "spotlight-outer") return 0.25F;
    if (id == "pointlight-falloff" || id == "spotlight-falloff" ||
        id == "env-ambient-intensity" || id == "env-exposure")
        return 0.02F;
    if (id == "env-fog-density") return 0.0005F;
    if (id == "env-bloom-passes") return 1.0F;
    if (id == "env-bloom-threshold" || id == "env-bloom-intensity" || id == "env-fxaa-strength" ||
        id == "env-gamma-r" || id == "env-gamma-g" || id == "env-gamma-b" ||
        id == "env-gain-r" || id == "env-gain-g" || id == "env-gain-b")
    {
        return 0.05F;
    }
    if (id == "env-lift-r" || id == "env-lift-g" || id == "env-lift-b")
    {
        return 0.01F;
    }
    if (id.rfind("mesh-color-", 0) == 0 || id.rfind("light-color-", 0) == 0 ||
        id.rfind("pointlight-color-", 0) == 0 || id.rfind("spotlight-color-", 0) == 0 ||
        id.rfind("env-zenith-", 0) == 0 || id.rfind("env-horizon-", 0) == 0 ||
        id.rfind("env-ground-", 0) == 0 || id.rfind("env-ambient-", 0) == 0 ||
        id.rfind("env-fog-color-", 0) == 0 ||
        id == "mesh-metallic" || id == "mesh-roughness")
        return 0.005F;
    return std::max(std::abs(startValue) * 0.005F, 0.01F);
}

bool SceneEditor::ApplyNumericField(const std::string_view id, const float value)
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    entt::registry& registry = m_World.Registry();
    if (TransformComponent* transform = registry.try_get<TransformComponent>(*entity))
    {
        if (id.rfind("position-", 0) == 0 || id.rfind("scale-", 0) == 0)
        {
            const int axis = id.back() == 'x' ? 0 : id.back() == 'y' ? 1 : 2;
            (id.rfind("position-", 0) == 0 ? transform->Position : transform->Scale)[axis] = value;
            return true;
        }
        if (id.rfind("rotation-", 0) == 0)
        {
            glm::vec3 euler = glm::degrees(glm::eulerAngles(transform->Rotation));
            const int axis = id.back() == 'x' ? 0 : id.back() == 'y' ? 1 : 2;
            euler[axis] = value;
            transform->Rotation = glm::quat{glm::radians(euler)};
            return true;
        }
    }
    if (MeshComponent* mesh = registry.try_get<MeshComponent>(*entity))
    {
        if (id.rfind("mesh-color-", 0) == 0)
        {
            const int channel = id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : 2;
            mesh->BaseColor[channel] = std::clamp(value, 0.0F, 1.0F);
            return true;
        }
        if (id == "mesh-metallic")
        {
            mesh->Metallic = std::clamp(value, 0.0F, 1.0F);
            return true;
        }
        if (id == "mesh-roughness")
        {
            mesh->Roughness = std::clamp(value, 0.0F, 1.0F);
            return true;
        }
    }
    if (DirectionalLightComponent* light = registry.try_get<DirectionalLightComponent>(*entity))
    {
        if (id.rfind("light-color-", 0) == 0)
        {
            const int channel = id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : 2;
            light->Color[channel] = std::clamp(value, 0.0F, 1.0F);
            return true;
        }
        if (id == "light-intensity")
        {
            light->Intensity = std::max(value, 0.0F);
            return true;
        }
        if (id == "light-shadow-bias")
        {
            light->ShadowBias = std::clamp(value, 0.0F, 0.05F);
            return true;
        }
        if (id == "light-shadow-strength")
        {
            light->ShadowStrength = std::clamp(value, 0.0F, 1.0F);
            return true;
        }
        if (id == "light-shadow-distance")
        {
            light->ShadowDistance = std::clamp(value, 5.0F, 2000.0F);
            return true;
        }
        if (id == "light-cascade-lambda")
        {
            light->CascadeSplitLambda = std::clamp(value, 0.0F, 1.0F);
            return true;
        }
        if (id == "light-shadow-softness")
        {
            light->ShadowSoftness = std::clamp(value, 0.0F, 4.0F);
            return true;
        }
    }
    if (CameraComponent* camera = registry.try_get<CameraComponent>(*entity))
    {
        if (id == "camera-fov") camera->FieldOfView = std::clamp(value, 1.0F, 179.0F);
        else if (id == "camera-near") camera->NearPlane = std::max(value, 0.001F);
        else if (id == "camera-far")
            camera->FarPlane = std::max(value, camera->NearPlane + 0.001F);
        else return false;
        return true;
    }
    if (NetworkIdentityComponent* network = registry.try_get<NetworkIdentityComponent>(*entity);
        network != nullptr && id == "network-id")
    {
        network->NetworkId =
            static_cast<std::uint64_t>(std::max(std::round(value), 0.0F));
        return true;
    }
    if (JoltBodyComponent* body = registry.try_get<JoltBodyComponent>(*entity);
        body != nullptr && id == "jolt-mass")
    {
        body->Mass = std::clamp(value, 0.001F, 1000000.0F);
        return true;
    }
    if (JoltBodyComponent* body = registry.try_get<JoltBodyComponent>(*entity);
        body != nullptr && id == "jolt-friction")
    {
        body->Friction = std::clamp(value, 0.0F, 2.0F);
        return true;
    }
    if (JoltBodyComponent* body = registry.try_get<JoltBodyComponent>(*entity);
        body != nullptr && id.rfind("jolt-extent-", 0) == 0)
    {
        const int axis = id.back() == 'x' ? 0 : id.back() == 'y' ? 1 : 2;
        body->HalfExtent[axis] = std::max(value, 0.001F);
        return true;
    }
    if (Box2DBodyComponent* body = registry.try_get<Box2DBodyComponent>(*entity);
        body != nullptr && id.rfind("box2d-extent-", 0) == 0)
    {
        body->HalfExtent[id.back() == 'x' ? 0 : 1] = std::max(value, 0.001F);
        return true;
    }
    if (PointLightComponent* light = registry.try_get<PointLightComponent>(*entity))
    {
        if (id.rfind("pointlight-color-", 0) == 0)
        {
            const int channel = id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : 2;
            light->Color[channel] = std::clamp(value, 0.0F, 1.0F);
            return true;
        }
        if (id == "pointlight-intensity")
        {
            light->Intensity = std::max(value, 0.0F);
            return true;
        }
        if (id == "pointlight-range")
        {
            light->Range = std::clamp(value, 0.01F, 10000.0F);
            return true;
        }
        if (id == "pointlight-falloff")
        {
            light->FalloffExponent = std::clamp(value, 0.1F, 16.0F);
            return true;
        }
        if (id == "pointlight-shadow-bias") { light->ShadowBias = std::clamp(value, 0.0F, 1.0F); return true; }
        if (id == "pointlight-shadow-resolution")
        {
            light->ShadowResolution = SanitizedShadowResolution(static_cast<int>(std::round(value)));
            return true;
        }
        if (id == "pointlight-shadow-strength") { light->ShadowStrength = std::clamp(value, 0.0F, 1.0F); return true; }
        if (id == "pointlight-softness") { light->Softness = std::clamp(value, 0.0F, 8.0F); return true; }
    }
    if (SpotLightComponent* light = registry.try_get<SpotLightComponent>(*entity))
    {
        if (id.rfind("spotlight-color-", 0) == 0)
        {
            const int channel = id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : 2;
            light->Color[channel] = std::clamp(value, 0.0F, 1.0F);
            return true;
        }
        if (id == "spotlight-intensity")
        {
            light->Intensity = std::max(value, 0.0F);
            return true;
        }
        if (id == "spotlight-range")
        {
            light->Range = std::clamp(value, 0.01F, 10000.0F);
            return true;
        }
        if (id == "spotlight-inner")
        {
            light->InnerConeDegrees = std::clamp(value, 0.0F, light->OuterConeDegrees - 0.1F);
            return true;
        }
        if (id == "spotlight-outer")
        {
            light->OuterConeDegrees =
                std::clamp(value, light->InnerConeDegrees + 0.1F, 89.9F);
            return true;
        }
        if (id == "spotlight-falloff")
        {
            light->FalloffExponent = std::clamp(value, 0.1F, 16.0F);
            return true;
        }
        if (id == "spotlight-shadow-bias") { light->ShadowBias = std::clamp(value, 0.0F, 1.0F); return true; }
        if (id == "spotlight-shadow-resolution")
        {
            light->ShadowResolution = SanitizedShadowResolution(static_cast<int>(std::round(value)));
            return true;
        }
        if (id == "spotlight-shadow-strength") { light->ShadowStrength = std::clamp(value, 0.0F, 1.0F); return true; }
        if (id == "spotlight-softness") { light->Softness = std::clamp(value, 0.0F, 8.0F); return true; }
    }
    if (EnvironmentComponent* env = registry.try_get<EnvironmentComponent>(*entity))
    {
        const auto channel = [&id]() { return id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : 2; };
        const float color = std::clamp(value, 0.0F, 1.0F);
        if (id.rfind("env-zenith-", 0) == 0)
        {
            env->SkyZenithColor[channel()] = color;
            return true;
        }
        if (id.rfind("env-horizon-", 0) == 0)
        {
            env->SkyHorizonColor[channel()] = color;
            return true;
        }
        if (id.rfind("env-ground-", 0) == 0)
        {
            env->GroundColor[channel()] = color;
            return true;
        }
        if (id.rfind("env-fog-color-", 0) == 0)
        {
            env->FogColor[channel()] = color;
            return true;
        }
        if (id == "env-ambient-intensity")
        {
            env->AmbientIntensity = std::clamp(value, 0.0F, 16.0F);
            return true;
        }
        if (id.rfind("env-ambient-", 0) == 0)
        {
            env->AmbientColor[channel()] = color;
            return true;
        }
        if (id == "env-exposure")
        {
            env->Exposure = std::clamp(value, 0.05F, 16.0F);
            return true;
        }
        if (id == "env-fog-density")
        {
            env->FogDensity = std::clamp(value, 0.0F, 1.0F);
            return true;
        }
        if (id == "env-fog-start")
        {
            env->FogStart = std::max(value, 0.0F);
            env->FogEnd = std::max(env->FogEnd, env->FogStart + 0.1F);
            return true;
        }
        if (id == "env-fog-end")
        {
            env->FogEnd = std::max(value, env->FogStart + 0.1F);
            return true;
        }
        if (id == "env-bloom-threshold")
        {
            env->BloomThreshold = std::clamp(value, 0.0F, 10.0F);
            return true;
        }
        if (id == "env-bloom-intensity")
        {
            env->BloomIntensity = std::clamp(value, 0.0F, 4.0F);
            return true;
        }
        if (id == "env-bloom-passes")
        {
            env->BloomPasses = std::clamp(static_cast<int>(std::round(value)), 1, 8);
            return true;
        }
        if (id == "env-lift-r") { env->Lift.x = std::clamp(value, -1.0F, 1.0F); return true; }
        if (id == "env-lift-g") { env->Lift.y = std::clamp(value, -1.0F, 1.0F); return true; }
        if (id == "env-lift-b") { env->Lift.z = std::clamp(value, -1.0F, 1.0F); return true; }
        if (id == "env-gamma-r") { env->Gamma.x = std::clamp(value, 0.1F, 3.0F); return true; }
        if (id == "env-gamma-g") { env->Gamma.y = std::clamp(value, 0.1F, 3.0F); return true; }
        if (id == "env-gamma-b") { env->Gamma.z = std::clamp(value, 0.1F, 3.0F); return true; }
        if (id == "env-gain-r") { env->Gain.x = std::clamp(value, 0.0F, 3.0F); return true; }
        if (id == "env-gain-g") { env->Gain.y = std::clamp(value, 0.0F, 3.0F); return true; }
        if (id == "env-gain-b") { env->Gain.z = std::clamp(value, 0.0F, 3.0F); return true; }
        if (id == "env-fxaa-strength")
        {
            env->FxaaStrength = std::clamp(value, 0.0F, 2.0F);
            return true;
        }
        if (id == "env-exposure-ev")
        {
            env->ExposureCompensationEV = std::clamp(value, -8.0F, 8.0F);
            return true;
        }
        if (id == "env-wb-temp")
        {
            env->WhiteBalanceTemperature = std::clamp(value, 1500.0F, 15000.0F);
            return true;
        }
        if (id == "env-wb-tint")
        {
            env->WhiteBalanceTint = std::clamp(value, -1.0F, 1.0F);
            return true;
        }
        if (id == "env-contrast")
        {
            env->Contrast = std::clamp(value, 0.0F, 2.0F);
            return true;
        }
        if (id == "env-saturation")
        {
            env->Saturation = std::clamp(value, 0.0F, 2.0F);
            return true;
        }
        if (id == "env-highlight-compression")
        {
            env->HighlightCompression = std::clamp(value, 0.0F, 1.0F);
            return true;
        }
        if (id == "env-environment-rotation")
        {
            float wrapped = value - 360.0F * std::floor(value / 360.0F);
            env->EnvironmentRotation = wrapped;
            return true;
        }
        if (id == "env-environment-intensity")
        {
            env->EnvironmentIntensity = std::clamp(value, 0.0F, 16.0F);
            return true;
        }
        if (id == "env-ao-radius") { env->AoRadius = std::clamp(value, 0.05F, 5.0F); return true; }
        if (id == "env-ao-intensity") { env->AoIntensity = std::clamp(value, 0.0F, 4.0F); return true; }
        if (id == "env-ao-power") { env->AoPower = std::clamp(value, 0.1F, 8.0F); return true; }
        if (id == "env-ao-bias") { env->AoBias = std::clamp(value, 0.0F, 0.5F); return true; }
        if (id == "env-priority")
        {
            env->Priority =
                std::clamp(static_cast<int>(std::round(value)), -1000000, 1000000);
            return true;
        }
    }
    if (ParticleEmitterComponent* emitter = registry.try_get<ParticleEmitterComponent>(*entity))
    {
        if (id == "particle-max")
        {
            emitter->MaxParticles = std::clamp(static_cast<int>(std::round(value)), 1, 4096);
            return true;
        }
        if (id == "particle-emit-rate") { emitter->EmitRate = std::max(value, 0.0F); return true; }
        if (id == "particle-life-min")
        {
            emitter->LifetimeMin = std::max(value, 0.01F);
            emitter->LifetimeMax = std::max(emitter->LifetimeMax, emitter->LifetimeMin);
            return true;
        }
        if (id == "particle-life-max")
        {
            emitter->LifetimeMax = std::max(value, emitter->LifetimeMin);
            return true;
        }
        if (id == "particle-speed-min")
        {
            emitter->SpeedMin = std::max(value, 0.0F);
            emitter->SpeedMax = std::max(emitter->SpeedMax, emitter->SpeedMin);
            return true;
        }
        if (id == "particle-speed-max")
        {
            emitter->SpeedMax = std::max(value, emitter->SpeedMin);
            return true;
        }
        if (id == "particle-shape-radius")
        {
            emitter->ShapeRadius = std::max(value, 0.0F);
            return true;
        }
        if (id == "particle-cone-angle")
        {
            emitter->ConeAngle = std::clamp(value, 0.0F, 90.0F);
            return true;
        }
        if (id == "particle-size-start")
        {
            emitter->SizeStart = std::max(value, 0.0F);
            return true;
        }
        if (id == "particle-size-end")
        {
            emitter->SizeEnd = std::max(value, 0.0F);
            return true;
        }
        if (id.rfind("particle-dir-", 0) == 0)
        {
            emitter->Direction[id.back() == 'x' ? 0 : id.back() == 'y' ? 1 : 2] = value;
            return true;
        }
        if (id.rfind("particle-gravity-", 0) == 0)
        {
            emitter->Gravity[id.back() == 'x' ? 0 : id.back() == 'y' ? 1 : 2] = value;
            return true;
        }
        if (id.rfind("particle-color-start-", 0) == 0)
        {
            const int ch =
                id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : id.back() == 'b' ? 2 : 3;
            emitter->ColorStart[ch] = std::clamp(value, 0.0F, 1.0F);
            return true;
        }
        if (id.rfind("particle-color-end-", 0) == 0)
        {
            const int ch =
                id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : id.back() == 'b' ? 2 : 3;
            emitter->ColorEnd[ch] = std::clamp(value, 0.0F, 1.0F);
            return true;
        }
    }
    if (AudioSourceComponent* audio = registry.try_get<AudioSourceComponent>(*entity))
    {
        if (id == "audio-volume")
        {
            audio->Volume = std::clamp(value, 0.0F, 1.0F);
            return true;
        }
        if (id == "audio-min-distance")
        {
            audio->MinDistance = std::max(value, 0.0F);
            audio->MaxDistance = std::max(audio->MaxDistance, audio->MinDistance + 0.01F);
            return true;
        }
        if (id == "audio-max-distance")
        {
            audio->MaxDistance = std::max(value, audio->MinDistance + 0.01F);
            return true;
        }
    }
    if (UICanvasComponent* ui = registry.try_get<UICanvasComponent>(*entity))
    {
        if (id == "ui-order") { ui->Order = static_cast<int>(std::lround(value)); return true; }
        if (id == "ui-scale") { ui->Scale = std::clamp(value, 0.1F, 5.0F); return true; }
    }
    if (AnimatorComponent* animator = registry.try_get<AnimatorComponent>(*entity))
    {
        if (id == "animator-speed") { animator->Speed = std::max(value, 0.0F); return true; }
    }
    if (TerrainComponent* terrain = registry.try_get<TerrainComponent>(*entity))
    {
        if (id == "terrain-width") { terrain->Width = std::max(value, 1.0F); return true; }
        if (id == "terrain-depth") { terrain->Depth = std::max(value, 1.0F); return true; }
        if (id == "terrain-height") { terrain->HeightScale = std::max(value, 0.1F); return true; }
        if (id == "terrain-res-x")
        {
            terrain->ResolutionX = std::clamp(static_cast<int>(std::lround(value)), 2, 513);
            return true;
        }
        if (id == "terrain-res-z")
        {
            terrain->ResolutionZ = std::clamp(static_cast<int>(std::lround(value)), 2, 513);
            return true;
        }
        if (id == "terrain-layer-count")
        {
            terrain->LayerCount = std::clamp(static_cast<int>(std::lround(value)), 1, 4);
            return true;
        }
        if (id.rfind("terrain-layer-", 0) == 0)
        {
            const std::size_t dash = id.find('-', 14);
            if (dash == std::string_view::npos) return false;
            const int index = std::atoi(std::string{id.substr(14, dash - 14)}.c_str());
            if (index < 0 || index >= 4) return false;
            const std::string_view field = id.substr(dash + 1);
            if (field == "tiling")
            {
                terrain->Layers[index].Tiling = std::max(value, 0.1F);
                return true;
            }
            if (field == "min-h")
            {
                terrain->Layers[index].MinHeight = std::clamp(value, 0.0F, 1.0F);
                return true;
            }
            if (field == "max-h")
            {
                terrain->Layers[index].MaxHeight = std::clamp(value, 0.0F, 1.0F);
                return true;
            }
            if (field == "min-s")
            {
                terrain->Layers[index].MinSlope = std::clamp(value, 0.0F, 1.0F);
                return true;
            }
            if (field == "max-s")
            {
                terrain->Layers[index].MaxSlope = std::clamp(value, 0.0F, 1.0F);
                return true;
            }
        }
    }
    return false;
}

std::optional<float> SceneEditor::NumericFieldValue(const std::string_view id) const
{
    if (!m_Selection) return std::nullopt;
    const auto entity = m_World.Find(*m_Selection);
    if (!entity) return std::nullopt;
    const entt::registry& registry = m_World.Registry();
    if (const TransformComponent* transform = registry.try_get<TransformComponent>(*entity))
    {
        if (id.rfind("position-", 0) == 0 || id.rfind("scale-", 0) == 0)
        {
            const int axis = id.back() == 'x' ? 0 : id.back() == 'y' ? 1 : 2;
            return (id.rfind("position-", 0) == 0 ? transform->Position
                                                   : transform->Scale)[axis];
        }
        if (id.rfind("rotation-", 0) == 0)
        {
            const glm::vec3 euler = glm::degrees(glm::eulerAngles(transform->Rotation));
            return euler[id.back() == 'x' ? 0 : id.back() == 'y' ? 1 : 2];
        }
    }
    if (const MeshComponent* mesh = registry.try_get<MeshComponent>(*entity))
    {
        if (id.rfind("mesh-color-", 0) == 0)
            return mesh->BaseColor[id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : 2];
        if (id == "mesh-metallic") return mesh->Metallic;
        if (id == "mesh-roughness") return mesh->Roughness;
    }
    if (const DirectionalLightComponent* light =
            registry.try_get<DirectionalLightComponent>(*entity))
    {
        if (id.rfind("light-color-", 0) == 0)
            return light->Color[id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : 2];
        if (id == "light-intensity") return light->Intensity;
        if (id == "light-shadow-bias") return light->ShadowBias;
        if (id == "light-shadow-strength") return light->ShadowStrength;
        if (id == "light-shadow-distance") return light->ShadowDistance;
        if (id == "light-cascade-lambda") return light->CascadeSplitLambda;
        if (id == "light-shadow-softness") return light->ShadowSoftness;
    }
    if (const CameraComponent* camera = registry.try_get<CameraComponent>(*entity))
    {
        if (id == "camera-fov") return camera->FieldOfView;
        if (id == "camera-near") return camera->NearPlane;
        if (id == "camera-far") return camera->FarPlane;
    }
    if (const NetworkIdentityComponent* network =
            registry.try_get<NetworkIdentityComponent>(*entity);
        network != nullptr && id == "network-id")
    {
        return static_cast<float>(network->NetworkId);
    }
    if (const JoltBodyComponent* body = registry.try_get<JoltBodyComponent>(*entity);
        body != nullptr && id == "jolt-mass")
    {
        return body->Mass;
    }
    if (const JoltBodyComponent* body = registry.try_get<JoltBodyComponent>(*entity);
        body != nullptr && id == "jolt-friction")
    {
        return body->Friction;
    }
    if (const JoltBodyComponent* body = registry.try_get<JoltBodyComponent>(*entity);
        body != nullptr && id.rfind("jolt-extent-", 0) == 0)
    {
        return body->HalfExtent[id.back() == 'x' ? 0 : id.back() == 'y' ? 1 : 2];
    }
    if (const Box2DBodyComponent* body = registry.try_get<Box2DBodyComponent>(*entity);
        body != nullptr && id.rfind("box2d-extent-", 0) == 0)
    {
        return body->HalfExtent[id.back() == 'x' ? 0 : 1];
    }
    if (const PointLightComponent* light = registry.try_get<PointLightComponent>(*entity))
    {
        if (id.rfind("pointlight-color-", 0) == 0)
            return light->Color[id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : 2];
        if (id == "pointlight-intensity") return light->Intensity;
        if (id == "pointlight-range") return light->Range;
        if (id == "pointlight-falloff") return light->FalloffExponent;
        if (id == "pointlight-shadow-bias") return light->ShadowBias;
        if (id == "pointlight-shadow-resolution") return static_cast<float>(light->ShadowResolution);
        if (id == "pointlight-shadow-strength") return light->ShadowStrength;
        if (id == "pointlight-softness") return light->Softness;
    }
    if (const SpotLightComponent* light = registry.try_get<SpotLightComponent>(*entity))
    {
        if (id.rfind("spotlight-color-", 0) == 0)
            return light->Color[id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : 2];
        if (id == "spotlight-intensity") return light->Intensity;
        if (id == "spotlight-range") return light->Range;
        if (id == "spotlight-inner") return light->InnerConeDegrees;
        if (id == "spotlight-outer") return light->OuterConeDegrees;
        if (id == "spotlight-falloff") return light->FalloffExponent;
        if (id == "spotlight-shadow-bias") return light->ShadowBias;
        if (id == "spotlight-shadow-resolution") return static_cast<float>(light->ShadowResolution);
        if (id == "spotlight-shadow-strength") return light->ShadowStrength;
        if (id == "spotlight-softness") return light->Softness;
    }
    if (const EnvironmentComponent* env = registry.try_get<EnvironmentComponent>(*entity))
    {
        const auto channel = [&id]() { return id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : 2; };
        if (id.rfind("env-zenith-", 0) == 0) return env->SkyZenithColor[channel()];
        if (id.rfind("env-horizon-", 0) == 0) return env->SkyHorizonColor[channel()];
        if (id.rfind("env-ground-", 0) == 0) return env->GroundColor[channel()];
        if (id.rfind("env-fog-color-", 0) == 0) return env->FogColor[channel()];
        if (id == "env-ambient-intensity") return env->AmbientIntensity;
        if (id.rfind("env-ambient-", 0) == 0) return env->AmbientColor[channel()];
        if (id == "env-exposure") return env->Exposure;
        if (id == "env-fog-density") return env->FogDensity;
        if (id == "env-fog-start") return env->FogStart;
        if (id == "env-fog-end") return env->FogEnd;
        if (id == "env-bloom-threshold") return env->BloomThreshold;
        if (id == "env-bloom-intensity") return env->BloomIntensity;
        if (id == "env-bloom-passes") return static_cast<float>(env->BloomPasses);
        if (id == "env-lift-r") return env->Lift.x;
        if (id == "env-lift-g") return env->Lift.y;
        if (id == "env-lift-b") return env->Lift.z;
        if (id == "env-gamma-r") return env->Gamma.x;
        if (id == "env-gamma-g") return env->Gamma.y;
        if (id == "env-gamma-b") return env->Gamma.z;
        if (id == "env-gain-r") return env->Gain.x;
        if (id == "env-gain-g") return env->Gain.y;
        if (id == "env-gain-b") return env->Gain.z;
        if (id == "env-fxaa-strength") return env->FxaaStrength;
        if (id == "env-exposure-ev") return env->ExposureCompensationEV;
        if (id == "env-wb-temp") return env->WhiteBalanceTemperature;
        if (id == "env-wb-tint") return env->WhiteBalanceTint;
        if (id == "env-contrast") return env->Contrast;
        if (id == "env-saturation") return env->Saturation;
        if (id == "env-highlight-compression") return env->HighlightCompression;
        if (id == "env-environment-rotation") return env->EnvironmentRotation;
        if (id == "env-environment-intensity") return env->EnvironmentIntensity;
        if (id == "env-ao-radius") return env->AoRadius;
        if (id == "env-ao-intensity") return env->AoIntensity;
        if (id == "env-ao-power") return env->AoPower;
        if (id == "env-ao-bias") return env->AoBias;
        if (id == "env-priority") return static_cast<float>(env->Priority);
    }
    if (const ParticleEmitterComponent* emitter =
            registry.try_get<ParticleEmitterComponent>(*entity))
    {
        if (id == "particle-max") return static_cast<float>(emitter->MaxParticles);
        if (id == "particle-emit-rate") return emitter->EmitRate;
        if (id == "particle-life-min") return emitter->LifetimeMin;
        if (id == "particle-life-max") return emitter->LifetimeMax;
        if (id == "particle-speed-min") return emitter->SpeedMin;
        if (id == "particle-speed-max") return emitter->SpeedMax;
        if (id == "particle-shape-radius") return emitter->ShapeRadius;
        if (id == "particle-cone-angle") return emitter->ConeAngle;
        if (id == "particle-size-start") return emitter->SizeStart;
        if (id == "particle-size-end") return emitter->SizeEnd;
        if (id.rfind("particle-dir-", 0) == 0)
            return emitter->Direction[id.back() == 'x' ? 0 : id.back() == 'y' ? 1 : 2];
        if (id.rfind("particle-gravity-", 0) == 0)
            return emitter->Gravity[id.back() == 'x' ? 0 : id.back() == 'y' ? 1 : 2];
        if (id.rfind("particle-color-start-", 0) == 0)
        {
            const int ch =
                id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : id.back() == 'b' ? 2 : 3;
            return emitter->ColorStart[ch];
        }
        if (id.rfind("particle-color-end-", 0) == 0)
        {
            const int ch =
                id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : id.back() == 'b' ? 2 : 3;
            return emitter->ColorEnd[ch];
        }
    }
    if (const AudioSourceComponent* audio = registry.try_get<AudioSourceComponent>(*entity))
    {
        if (id == "audio-volume") return audio->Volume;
        if (id == "audio-min-distance") return audio->MinDistance;
        if (id == "audio-max-distance") return audio->MaxDistance;
    }
    if (const UICanvasComponent* ui = registry.try_get<UICanvasComponent>(*entity))
    {
        if (id == "ui-order") return static_cast<float>(ui->Order);
        if (id == "ui-scale") return ui->Scale;
    }
    if (const AnimatorComponent* animator = registry.try_get<AnimatorComponent>(*entity))
    {
        if (id == "animator-speed") return animator->Speed;
    }
    if (const TerrainComponent* terrain = registry.try_get<TerrainComponent>(*entity))
    {
        if (id == "terrain-width") return terrain->Width;
        if (id == "terrain-depth") return terrain->Depth;
        if (id == "terrain-height") return terrain->HeightScale;
        if (id == "terrain-res-x") return static_cast<float>(terrain->ResolutionX);
        if (id == "terrain-res-z") return static_cast<float>(terrain->ResolutionZ);
        if (id == "terrain-layer-count")
            return static_cast<float>(std::clamp(terrain->LayerCount, 1, 4));
        if (id.rfind("terrain-layer-", 0) == 0)
        {
            const std::size_t dash = id.find('-', 14);
            if (dash == std::string_view::npos) return std::nullopt;
            const int index = std::atoi(std::string{id.substr(14, dash - 14)}.c_str());
            if (index < 0 || index >= 4) return std::nullopt;
            const std::string_view field = id.substr(dash + 1);
            if (field == "tiling") return terrain->Layers[index].Tiling;
            if (field == "min-h") return terrain->Layers[index].MinHeight;
            if (field == "max-h") return terrain->Layers[index].MaxHeight;
            if (field == "min-s") return terrain->Layers[index].MinSlope;
            if (field == "max-s") return terrain->Layers[index].MaxSlope;
        }
    }
    return std::nullopt;
}

bool SceneEditor::BeginEditTransaction()
{
    m_EditTx.reset();
    if (!m_Selection)
    {
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    if (!before)
    {
        return false;
    }
    m_EditTx = EditTransaction{std::move(*before)};
    return true;
}

bool SceneEditor::EndEditTransaction(const char* commandName)
{
    if (!m_EditTx || !m_Selection)
    {
        m_EditTx.reset();
        return false;
    }
    auto after = EntitySnapshot::Capture(m_World, *m_Selection);
    if (!after)
    {
        m_EditTx.reset();
        return false;
    }
    m_History.Push(std::make_unique<SnapshotEntityCommand>(
        m_World, std::move(m_EditTx->Before), std::move(*after), commandName));
    m_EditTx.reset();
    MarkChanged();
    return true;
}

bool SceneEditor::CancelEditTransaction()
{
    if (!m_EditTx)
    {
        return false;
    }
    m_EditTx.reset();
    return true;
}

bool SceneEditor::AssignMaterial(const AssetHandle handle)
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    MeshComponent* mesh = m_World.Registry().try_get<MeshComponent>(*entity);
    if (mesh == nullptr)
    {
        Report("Material can only be assigned to mesh entities");
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    mesh->Material = handle;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Assign Material"));
    }
    MarkChanged();
    Report("Assigned material to mesh");
    return true;
}

bool SceneEditor::ClearMaterial()
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    MeshComponent* mesh = m_World.Registry().try_get<MeshComponent>(*entity);
    if (mesh == nullptr || !mesh->Material.IsValid())
    {
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    mesh->Material = AssetHandle{};
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Clear Material"));
    }
    MarkChanged();
    Report("Cleared mesh material");
    return true;
}

bool SceneEditor::AssignImportedMesh(const AssetHandle handle)
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    MeshComponent* mesh = m_World.Registry().try_get<MeshComponent>(*entity);
    if (mesh == nullptr)
    {
        Report("Mesh can only be assigned to mesh entities");
        return false;
    }
    if (m_Assets == nullptr || m_GltfMeshes == nullptr)
    {
        Report("Mesh importer is not available");
        return false;
    }
    const AssetMetadata* metadata = m_Assets->Meta(handle);
    if (metadata == nullptr || metadata->Type != "Mesh")
    {
        Report("Selected asset is not an imported mesh");
        return false;
    }
    const std::filesystem::path& path = !metadata->SourcePath.empty()
        ? metadata->SourcePath
        : metadata->ImportedPath;
    if (m_GltfMeshes->Load(handle, path.string(), [this](const std::string& message) {
            Report(message);
        }) == nullptr)
    {
        Report("Could not load imported mesh " + path.filename().string());
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    mesh->ImportedMesh = handle;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Assign Imported Mesh"));
    }
    MarkChanged();
    Report("Assigned imported mesh");
    return true;
}

bool SceneEditor::ClearImportedMesh()
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    MeshComponent* mesh = m_World.Registry().try_get<MeshComponent>(*entity);
    if (mesh == nullptr || !mesh->ImportedMesh.IsValid())
    {
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    mesh->ImportedMesh = AssetHandle{};
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Clear Imported Mesh"));
    }
    MarkChanged();
    Report("Cleared imported mesh");
    return true;
}

bool SceneEditor::AssignSound(const AssetHandle handle)
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    AudioSourceComponent* audio = m_World.Registry().try_get<AudioSourceComponent>(*entity);
    if (audio == nullptr)
    {
        Report("Audio can only be assigned to Audio Source entities");
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    audio->Sound = handle;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Assign Sound"));
    }
    MarkChanged();
    Report("Assigned sound to Audio Source");
    return true;
}

bool SceneEditor::AssignScript(const AssetHandle handle)
{
    if (!m_Selection)
    {
        Report("Select an entity before attaching a script");
        return false;
    }
    if (m_Assets == nullptr)
    {
        Report("Asset database is unavailable");
        return false;
    }
    const AssetMetadata* metadata = m_Assets->Meta(handle);
    if (metadata == nullptr || metadata->Type != "Script")
    {
        Report("Dropped asset is not a script");
        return false;
    }
    const std::string scriptName = metadata->SourcePath.stem().string();
    if (scriptName.empty())
    {
        Report("Script has no valid name");
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }

    entt::registry& registry = m_World.Registry();
    if (const ScriptComponent* scripts = registry.try_get<ScriptComponent>(*entity);
        scripts != nullptr &&
        std::find(scripts->ScriptNames.begin(), scripts->ScriptNames.end(), scriptName) !=
            scripts->ScriptNames.end())
    {
        Report("Script is already attached");
        return false;
    }

    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    registry.get_or_emplace<ScriptComponent>(*entity).ScriptNames.push_back(scriptName);
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Attach Script"));
    }
    MarkChanged();
    Report("Attached script '" + scriptName + "'");
    return true;
}

bool SceneEditor::AssignUIAsset(const AssetHandle handle)
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    UICanvasComponent* ui = m_World.Registry().try_get<UICanvasComponent>(*entity);
    if (ui == nullptr)
    {
        Report("UI can only be assigned to UI Canvas entities");
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    ui->UIAsset = handle;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Assign UI"));
    }
    MarkChanged();
    Report("Assigned UI document");
    return true;
}

bool SceneEditor::AssignUIStyle(const AssetHandle handle)
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    UICanvasComponent* ui = m_World.Registry().try_get<UICanvasComponent>(*entity);
    if (ui == nullptr)
    {
        Report("UIStyle can only be assigned to UI Canvas entities");
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    ui->StyleAsset = handle;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Assign UI Style"));
    }
    MarkChanged();
    Report("Assigned UI style");
    return true;
}

bool SceneEditor::AssignTerrainHeightmap(const AssetHandle handle)
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    TerrainComponent* terrain = m_World.Registry().try_get<TerrainComponent>(*entity);
    if (terrain == nullptr)
    {
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    terrain->Heightmap = handle;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Assign Heightmap"));
    }
    MarkChanged();
    Report("Assigned heightmap");
    return true;
}

bool SceneEditor::AssignTerrainAlbedo(AssetHandle /*handle*/)
{
    // ponytail: per-layer albedo slot requires a layer index; not yet wired.
    return false;
}

bool SceneEditor::AssignScriptTarget(const Uuid entityId)
{
    if (!m_Selection || !entityId.IsValid())
    {
        return false;
    }
    if (entityId == *m_Selection)
    {
        Report("Script Target cannot be the same entity");
        return false;
    }
    if (!m_World.Find(entityId))
    {
        Report("Dropped entity no longer exists");
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    ScriptComponent* scripts = m_World.Registry().try_get<ScriptComponent>(*entity);
    if (scripts == nullptr)
    {
        Report("Add a Script component before setting Target");
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    scripts->Target = entityId;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Set Script Target"));
    }
    MarkChanged();
    if (const auto targetEntity = m_World.Find(entityId))
    {
        if (const NameComponent* name =
                m_World.Registry().try_get<NameComponent>(*targetEntity))
        {
            Report("Script Target set to " + name->Name);
            return true;
        }
    }
    Report("Script Target set");
    return true;
}

bool SceneEditor::ClearScriptTarget()
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    ScriptComponent* scripts = m_World.Registry().try_get<ScriptComponent>(*entity);
    if (scripts == nullptr)
    {
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    scripts->Target = {};
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Clear Script Target"));
    }
    MarkChanged();
    return true;
}

bool SceneEditor::AssignMaterialFromSelection()
{
    if (!m_Selection || m_SelectedMaterial == nullptr)
    {
        Report("Select a Material asset in the Asset Browser, then click Assign");
        return false;
    }
    const std::optional<AssetHandle> handle = m_SelectedMaterial();
    if (!handle || !handle->IsValid())
    {
        Report("Select a Material asset in the Asset Browser, then click Assign");
        return false;
    }
    if (m_Assets != nullptr)
    {
        if (const AssetMetadata* meta = m_Assets->Meta(*handle);
            meta == nullptr || meta->Type != "Material")
        {
            Report("Selected asset is not a Material");
            return false;
        }
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    MeshComponent* mesh = m_World.Registry().try_get<MeshComponent>(*entity);
    if (mesh == nullptr)
    {
        Report("Selected entity has no Mesh component");
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    mesh->Material = *handle;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Assign Material"));
    }
    Report("Assigned material to mesh");
    return true;
}

bool SceneEditor::AssignMeshFromSelection()
{
    if (!m_Selection || m_SelectedMesh == nullptr)
    {
        Report("Select a Mesh asset in the Asset Browser, then click Assign");
        return false;
    }
    const std::optional<AssetHandle> handle = m_SelectedMesh();
    if (!handle || !handle->IsValid())
    {
        Report("Select a Mesh asset in the Asset Browser, then click Assign");
        return false;
    }
    if (m_Assets != nullptr)
    {
        if (const AssetMetadata* meta = m_Assets->Meta(*handle);
            meta == nullptr || meta->Type != "Mesh")
        {
            Report("Selected asset is not a Mesh");
            return false;
        }
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    MeshComponent* mesh = m_World.Registry().try_get<MeshComponent>(*entity);
    if (mesh == nullptr)
    {
        Report("Selected entity has no Mesh component");
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    mesh->ImportedMesh = *handle;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Assign Imported Mesh"));
    }
    Report("Assigned imported mesh");
    return true;
}
} // namespace fadix
