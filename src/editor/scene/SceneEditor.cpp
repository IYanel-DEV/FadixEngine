#include "editor/scene/SceneEditor.hpp"

#include "assets/AssetDatabase.hpp"
#include "assets/GltfMeshCache.hpp"
#include "editor/command/EntityCommands.hpp"
#include "editor/scene/EntityTextIO.hpp"
#include "editor/scene/PrefabSerializer.hpp"
#include "engine/assets/MaterialAsset.hpp"
#include "engine/command/UndoStack.hpp"
#include "engine/scene/IWorld.hpp"
#include "runtime/AnimationRuntime.hpp"
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
    return FindSceneRootId(m_World);
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
    if (const auto created = m_World.Find(id))
    {
        AttachImportedAnimation(m_World.Registry(), *created, *gltf,
            [this](const std::string& message) { Report(message); });
    }
    MarkChanged();
    return id.IsValid() ? std::optional<Uuid>{id} : std::nullopt;
}

std::optional<Uuid> SceneEditor::InstantiatePrefab(
    const std::filesystem::path& path, const glm::vec3& gridPosition)
{
    // ponytail: prefab drop is not a single undo step (Instantiate builds a whole
    // subtree with no matching command). Delete the root to remove it. Upgrade
    // path: an InstantiatePrefabCommand capturing the created ids.
    const Result<Uuid> root = PrefabSerializer::Instantiate(m_World, path, SceneRootId());
    if (!root)
    {
        Report("Prefab drop failed: " + root.ErrorMessage());
        return std::nullopt;
    }
    if (const std::optional<entt::entity> entity = m_World.Find(root.Value()))
    {
        if (auto* transform = m_World.Registry().try_get<TransformComponent>(*entity))
        {
            transform->Position = gridPosition;
        }
    }
    MarkChanged();
    return root.Value();
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

} // namespace fadix
