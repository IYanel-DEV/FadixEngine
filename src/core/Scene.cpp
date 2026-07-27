#include "Scene.hpp"

#include <algorithm>
#include <fstream>
#include <memory>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "SceneSerializer.hpp"

// =============================================================================
// Fadix Engine — Scene Implementation
// =============================================================================

namespace {

using nlohmann::json;

} // namespace

// =============================================================================
// Registry deep-copy
// =============================================================================

std::unique_ptr<Scene> Scene::Clone(const std::unique_ptr<Scene>& source)
{
    auto copy = std::make_unique<Scene>();

    const entt::registry& srcReg = source->m_Registry;
    entt::registry&       dstReg = copy->m_Registry;

    // Collect handles first so we can create them in a stable order.
    std::vector<entt::entity> handles;
    for (auto e : srcReg.view<entt::entity>())
        handles.push_back(e);

    // Mirror every entity into the destination with the same identifier so
    // that m_SelectedEntity remains valid across OnPlay/OnStop cycles.
    dstReg.create(handles.begin(), handles.end());

    for (entt::entity e : handles)
    {
        if (const auto* c = srcReg.try_get<TagComponent>(e))
            dstReg.emplace<TagComponent>(e, *c);
        if (const auto* c = srcReg.try_get<TransformComponent>(e))
            dstReg.emplace<TransformComponent>(e, *c);
        if (const auto* c = srcReg.try_get<MeshComponent>(e))
            dstReg.emplace<MeshComponent>(e, *c);
        if (const auto* c = srcReg.try_get<LightComponent>(e))
            dstReg.emplace<LightComponent>(e, *c);
        if (const auto* c = srcReg.try_get<CameraComponent>(e))
            dstReg.emplace<CameraComponent>(e, *c);

        // RelationshipComponent: entity identifiers are preserved by the bulk
        // create above, so parent/children handles stay valid verbatim.
        if (const auto* c = srcReg.try_get<RelationshipComponent>(e))
            dstReg.emplace<RelationshipComponent>(e, *c);

        // Physics components: copy the authored data but RESET the runtime
        // handles — they reference worlds that will not exist when this clone
        // is used (the backup is restored after the Play world is torn down).
        // Leaving stale handles would make SyncBodies skip body creation on
        // the next Play, silently disabling physics.
        if (const auto* c = srcReg.try_get<RigidBody3DComponent>(e))
        {
            auto& rb  = dstReg.emplace<RigidBody3DComponent>(e, *c);
            rb.BodyId = 0xFFFFFFFFu;
        }
        if (const auto* c = srcReg.try_get<BoxCollider3DComponent>(e))
            dstReg.emplace<BoxCollider3DComponent>(e, *c);
        if (const auto* c = srcReg.try_get<SphereCollider3DComponent>(e))
            dstReg.emplace<SphereCollider3DComponent>(e, *c);
        if (const auto* c = srcReg.try_get<RigidBody2DComponent>(e))
        {
            auto& rb2  = dstReg.emplace<RigidBody2DComponent>(e, *c);
            rb2.BodyId = {};
        }
        if (const auto* c = srcReg.try_get<BoxCollider2DComponent>(e))
        {
            auto& bc2    = dstReg.emplace<BoxCollider2DComponent>(e, *c);
            bc2.ShapeId  = {};
        }

        // NativeScriptComponent: copy the Bind<> function pointers so the
        // restored editor backup can re-instantiate scripts on the next Play.
        // Instance is always null at clone time (scripts only run during Play),
        // so the plain struct copy is safe — no deep heap duplication needed.
        if (const auto* c = srcReg.try_get<NativeScriptComponent>(e))
        {
            auto& nsc    = dstReg.emplace<NativeScriptComponent>(e, *c);
            nsc.Instance = nullptr; // defensive: never share a live instance
        }

#ifdef FADIX_EDITOR
        // ScriptComponent: copy the editor-assigned asset UUID, path, and all
        // property overrides so the Play-mode backup keeps the inspector state.
        if (const auto* c = srcReg.try_get<ScriptComponent>(e))
            dstReg.emplace<ScriptComponent>(e, *c);
#endif
    }

    return copy;
}

// =============================================================================
// Entity lifecycle
// =============================================================================

Entity Scene::CreateEntity(const std::string& name)
{
    Entity entity(m_Registry.create(), &m_Registry);

    entity.AddComponent<TagComponent>(name);
    entity.AddComponent<TransformComponent>();

    return entity;
}

void Scene::DestroyEntity(Entity entity)
{
    if (!entity) return;

    const entt::entity handle = entity.GetHandle();
    if (!m_Registry.valid(handle)) return;

    // Detach from the parent's child list first.
    if (const auto* rel = m_Registry.try_get<RelationshipComponent>(handle))
    {
        if (rel->Parent != entt::null && m_Registry.valid(rel->Parent))
        {
            if (auto* parentRel = m_Registry.try_get<RelationshipComponent>(rel->Parent))
            {
                auto& kids = parentRel->Children;
                kids.erase(std::remove(kids.begin(), kids.end(), handle), kids.end());
            }
        }

        // Destroy the descendant subtree (copy the list — recursion mutates it).
        const std::vector<entt::entity> children = rel->Children;
        for (entt::entity child : children)
        {
            if (m_Registry.valid(child))
                DestroyEntity(Entity(child, &m_Registry));
        }
    }

    m_Registry.destroy(handle);
}

// =============================================================================
// Scene-graph parenting
// =============================================================================

void Scene::SetParent(entt::entity child, entt::entity parent,
                      bool keepWorldTransform)
{
    if (child == entt::null || !m_Registry.valid(child)) return;
    if (child == parent) return;
    if (parent != entt::null && !m_Registry.valid(parent)) return;

    // Cycle guard: an entity cannot be parented under its own descendant.
    if (parent != entt::null && IsDescendantOf(parent, child)) return;

    auto& rel = m_Registry.get_or_emplace<RelationshipComponent>(child);
    if (rel.Parent == parent) return;

    // Capture the child's world matrix BEFORE any link changes so its world
    // placement can be preserved across the re-parent.
    auto* tc = m_Registry.try_get<TransformComponent>(child);
    const glm::mat4 childWorld = (keepWorldTransform && tc)
        ? GetWorldMatrix(m_Registry, child)
        : glm::mat4(1.0f);

    // Detach from the current parent.
    if (rel.Parent != entt::null && m_Registry.valid(rel.Parent))
    {
        if (auto* oldRel = m_Registry.try_get<RelationshipComponent>(rel.Parent))
        {
            auto& kids = oldRel->Children;
            kids.erase(std::remove(kids.begin(), kids.end(), child), kids.end());
        }
    }

    rel.Parent = parent;

    if (parent != entt::null)
    {
        auto& parentRel = m_Registry.get_or_emplace<RelationshipComponent>(parent);
        parentRel.Children.push_back(child);
    }

    if (keepWorldTransform && tc)
    {
        // Convert the captured world matrix into the new parent's local space
        // (identity parent for root) and decompose back into TRS fields.
        const glm::mat4 parentWorld = (parent != entt::null)
            ? GetWorldMatrix(m_Registry, parent)
            : glm::mat4(1.0f);
        const glm::mat4 local = glm::inverse(parentWorld) * childWorld;

        // Decompose (assumes no shear — true for TRS-composed matrices).
        tc->Position = glm::vec3(local[3]);
        const glm::vec3 scale(
            glm::length(glm::vec3(local[0])),
            glm::length(glm::vec3(local[1])),
            glm::length(glm::vec3(local[2])));
        tc->Scale = scale;
        glm::mat3 rot(local);
        if (scale.x > 1e-6f) rot[0] /= scale.x;
        if (scale.y > 1e-6f) rot[1] /= scale.y;
        if (scale.z > 1e-6f) rot[2] /= scale.z;
        tc->Rotation = glm::degrees(glm::eulerAngles(glm::quat_cast(rot)));
    }
}

bool Scene::IsDescendantOf(entt::entity entity, entt::entity ancestor) const
{
    if (entity == entt::null || ancestor == entt::null) return false;

    entt::entity cursor = entity;
    // Depth guard against corrupt cyclic data.
    for (int depth = 0; depth < 1024; ++depth)
    {
        const auto* rel = m_Registry.try_get<RelationshipComponent>(cursor);
        if (!rel || rel->Parent == entt::null) return false;
        if (rel->Parent == ancestor) return true;
        cursor = rel->Parent;
    }
    return false;
}

entt::entity Scene::GetParent(entt::entity entity) const
{
    if (entity == entt::null || !m_Registry.valid(entity)) return entt::null;
    const auto* rel = m_Registry.try_get<RelationshipComponent>(entity);
    return rel ? rel->Parent : entt::null;
}

glm::mat4 Scene::ComposeLocal(const TransformComponent& transform)
{
    return glm::translate(glm::mat4(1.0f), transform.Position)
         * glm::mat4_cast(glm::quat(glm::radians(transform.Rotation)))
         * glm::scale(glm::mat4(1.0f), transform.Scale);
}

glm::mat4 Scene::GetWorldMatrix(const entt::registry& registry, entt::entity entity)
{
    glm::mat4 world(1.0f);

    entt::entity cursor = entity;
    // Compose child-to-root; the loop multiplies parents on the left.
    for (int depth = 0; depth < 1024 && cursor != entt::null; ++depth)
    {
        if (!registry.valid(cursor)) break;

        if (const auto* tc = registry.try_get<TransformComponent>(cursor))
            world = ComposeLocal(*tc) * world;

        const auto* rel = registry.try_get<RelationshipComponent>(cursor);
        cursor = rel ? rel->Parent : entt::null;
    }
    return world;
}

// ---------------------------------------------------------------------------
// Serialization — delegates to SceneSerializer so the engine has exactly one
// JSON scene format (component coverage lives in SceneSerializer.cpp).
// ---------------------------------------------------------------------------

bool Scene::SaveToFile(const std::filesystem::path& file)
{
    std::shared_ptr<Scene> self(this, [](Scene*) {});
    return SceneSerializer(self).Serialize(file.generic_string());
}

bool Scene::LoadFromFile(const std::filesystem::path& file)
{
    std::shared_ptr<Scene> self(this, [](Scene*) {});
    return SceneSerializer(self).Deserialize(file.generic_string());
}

#ifdef FADIX_EDITOR

// ---------------------------------------------------------------------------
// Hot-reload state preservation
// ---------------------------------------------------------------------------
// Writes every entity's ScriptComponent property overrides to a temporary JSON
// cache so they survive DLL unload / reload without being wiped.  Called by
// EngineContext::ReloadScriptLibrary immediately before FreeLibrary.
// ---------------------------------------------------------------------------
void Scene::SerializeHotReloadState(const std::string& path) const
{
    json root = json::array();

    for (auto handle : m_Registry.view<TagComponent>())
    {
        json je;
        je["id"] = static_cast<uint32_t>(entt::to_integral(handle));

        if (const auto* sc = m_Registry.try_get<ScriptComponent>(handle))
        {
            je["scriptPath"] = sc->ScriptAssetPath;
            je["scriptUUID"] = sc->ScriptAssetId.ToString();

            json jov = json::object();
            for (const auto& [name, val] : sc->PropertyOverrides)
            {
                json jv;
                jv["type"] = static_cast<int>(val.Type);
                jv["i"]    = val.IntVal;
                jv["f"]    = val.FloatVal;
                jv["v"]    = { val.Vec3Val.x, val.Vec3Val.y, val.Vec3Val.z };
                jv["s"]    = val.StringVal;
                jov[name]  = std::move(jv);
            }
            je["overrides"] = std::move(jov);
        }

        root.push_back(std::move(je));
    }

    std::error_code ec;
    std::filesystem::create_directories("bin", ec);

    std::ofstream out(path);
    if (out.is_open())
        out << root.dump(2) << '\n';
}

// Restores ScriptComponent data written by SerializeHotReloadState.
// Called by EngineContext::ReloadScriptLibrary immediately after LoadLibraryA.
// Entities are keyed by their raw entt integer — robust because the registry
// is untouched during a DLL swap (only function pointers are remapped).
void Scene::DeserializeHotReloadState(const std::string& path)
{
    std::ifstream in(path);
    if (!in.is_open()) return;

    const json root = json::parse(in, nullptr, /*exceptions=*/false);
    if (root.is_discarded() || !root.is_array()) return;

    for (const auto& je : root)
    {
        if (!je.is_object() || !je.contains("id")) continue;

        const entt::entity handle =
            static_cast<entt::entity>(je["id"].get<uint32_t>());

        if (!m_Registry.valid(handle)) continue;

        if (!je.contains("overrides")) continue;

        auto* sc = m_Registry.try_get<ScriptComponent>(handle);
        if (!sc)
            sc = &m_Registry.emplace<ScriptComponent>(handle);

        if (je.contains("scriptPath"))
            sc->ScriptAssetPath = je["scriptPath"].get<std::string>();
        if (je.contains("scriptUUID"))
            sc->ScriptAssetId = fadix::UUID::FromString(
                je["scriptUUID"].get<std::string>());

        for (const auto& [name, jv] : je["overrides"].items())
        {
            fadix::PropertyValue val;
            val.Type = static_cast<fadix::PropertyType>(
                jv.value("type", static_cast<int>(fadix::PropertyType::Float)));
            val.IntVal    = jv.value("i", 0);
            val.FloatVal  = jv.value("f", 0.0f);
            val.StringVal = jv.value("s", std::string{});
            if (jv.contains("v") && jv["v"].is_array() && jv["v"].size() >= 3)
                val.Vec3Val = glm::vec3(
                    jv["v"][0].get<float>(),
                    jv["v"][1].get<float>(),
                    jv["v"][2].get<float>());
            sc->PropertyOverrides[name] = std::move(val);
        }
    }
}

#endif // FADIX_EDITOR
