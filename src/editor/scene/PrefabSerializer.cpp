#include "editor/scene/PrefabSerializer.hpp"

#include "editor/scene/EntityTextIO.hpp"
#include "engine/scene/IWorld.hpp"
#include "runtime/Components.hpp"
#include "runtime/World.hpp"

#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fadix
{
namespace
{
std::vector<Uuid> CollectSubtree(const IWorld& world, Uuid root)
{
    std::vector<Uuid> collected;
    std::vector<Uuid> pending{root};
    while (!pending.empty())
    {
        const Uuid current = pending.back();
        pending.pop_back();
        if (!world.Find(current))
        {
            continue;
        }
        collected.push_back(current);
        for (const auto [entity, relationship, id] :
             world.Registry().view<const RelationshipComponent, const UuidComponent>().each())
        {
            if (relationship.Parent == current)
            {
                pending.push_back(id.Id);
            }
            static_cast<void>(entity);
        }
    }
    return collected;
}

void SanitizeRuntimeHandles(IWorld& world, entt::entity entity)
{
    entt::registry& registry = world.Registry();
    if (auto* network = registry.try_get<NetworkIdentityComponent>(entity))
    {
        network->NetworkId = 0;
    }
    if (auto* jolt = registry.try_get<JoltBodyComponent>(entity))
    {
        jolt->Handle = InvalidPhysicsBody;
    }
    if (auto* character = registry.try_get<CharacterControllerComponent>(entity))
    {
        character->MoveInput = {0.0F, 0.0F};
        character->JumpRequested = false;
        character->Grounded = false;
    }
    if (auto* box2d = registry.try_get<Box2DBodyComponent>(entity))
    {
        box2d->Handle = InvalidPhysicsBody;
    }
    if (auto* rb2d = registry.try_get<RigidBody2DComponent>(entity))
    {
        rb2d->Handle = InvalidPhysicsBody;
    }
    if (auto* audio = registry.try_get<AudioSourceComponent>(entity))
    {
        audio->ActiveTrack = -1;
    }
}
}

Result<void> PrefabSerializer::Save(
    const IWorld& world, Uuid root, const std::filesystem::path& outputPath)
{
    if (!root.IsValid() || !world.Find(root))
    {
        return Result<void>::Error("Prefab root entity not found");
    }

    std::error_code error;
    if (!outputPath.parent_path().empty())
    {
        std::filesystem::create_directories(outputPath.parent_path(), error);
        if (error)
        {
            return Result<void>::Error("Could not create prefab directory: " + error.message());
        }
    }

    const std::filesystem::path temporary = outputPath.string() + ".tmp";
    std::ofstream output{temporary, std::ios::trunc};
    if (!output)
    {
        return Result<void>::Error("Could not open prefab for writing: " + temporary.string());
    }

    output << "FADIX_PREFAB 1\n";
    for (const Uuid id : CollectSubtree(world, root))
    {
        if (const auto entity = world.Find(id))
        {
            WriteEntityLine(output, world, *entity, id == root ? std::optional<Uuid>{Uuid{}} : std::nullopt);
        }
    }
    output.flush();
    output.close();
    if (!output)
    {
        std::error_code cleanup;
        std::filesystem::remove(temporary, cleanup);
        return Result<void>::Error("Failed while writing prefab: " + outputPath.string());
    }
    return AtomicReplaceFile(temporary, outputPath);
}

Result<Uuid> PrefabSerializer::Instantiate(
    IWorld& world, const std::filesystem::path& path, std::optional<Uuid> parent)
{
    std::ifstream input{path};
    std::string header;
    unsigned version = 0;
    if (!(input >> header >> version) || header != "FADIX_PREFAB" || version != 1)
    {
        return Result<Uuid>::Error("Invalid or unsupported prefab: " + path.string());
    }

    World templateWorld{false};
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line))
    {
        if (line.empty())
        {
            continue;
        }
        if (const Result<void> parsed = ParseEntityLine(line, templateWorld); !parsed)
        {
            return Result<Uuid>::Error(parsed.ErrorMessage());
        }
    }

    std::unordered_map<std::string, Uuid> oldToNew;
    Uuid templateRoot{};
    for (const auto [entity, uuid] : templateWorld.Registry().view<const UuidComponent>().each())
    {
        oldToNew.emplace(uuid.Id.ToString(), Uuid::Generate());
        if (!templateWorld.Registry().all_of<RelationshipComponent>(entity))
        {
            templateRoot = uuid.Id;
        }
    }
    if (!templateRoot.IsValid())
    {
        return Result<Uuid>::Error("Prefab has no root entity: " + path.string());
    }

    if (parent)
    {
        if (!parent->IsValid() || !world.Find(*parent))
        {
            return Result<Uuid>::Error("Instantiate parent not found");
        }
    }

    World instance{false};
    for (const auto [entity, uuid] : templateWorld.Registry().view<const UuidComponent>().each())
    {
        const auto selfIt = oldToNew.find(uuid.Id.ToString());
        if (selfIt == oldToNew.end())
        {
            return Result<Uuid>::Error(
                "Prefab instantiate: entity id missing from remap map: " + uuid.Id.ToString());
        }
        const Uuid newId = selfIt->second;
        const entt::entity created = instance.Create(newId);
        CopyEntityComponents(instance, created, templateWorld, entity);
        if (instance.Registry().all_of<RelationshipComponent>(created))
        {
            instance.Registry().remove<RelationshipComponent>(created);
        }
        if (const auto* rel = templateWorld.Registry().try_get<RelationshipComponent>(entity))
        {
            const auto parentIt = oldToNew.find(rel->Parent.ToString());
            if (parentIt == oldToNew.end())
            {
                return Result<Uuid>::Error(
                    "Prefab instantiate: relationship parent missing from remap map: "
                    + rel->Parent.ToString());
            }
            instance.Registry().emplace<RelationshipComponent>(
                created, RelationshipComponent{parentIt->second});
        }
        else if (parent && parent->IsValid())
        {
            instance.Registry().emplace<RelationshipComponent>(
                created, RelationshipComponent{*parent});
        }
        SanitizeRuntimeHandles(instance, created);
    }
    const auto rootIt = oldToNew.find(templateRoot.ToString());
    if (rootIt == oldToNew.end())
    {
        return Result<Uuid>::Error(
            "Prefab instantiate: template root missing from remap map: " + templateRoot.ToString());
    }
    CopyWorldInto(instance, world);
    return Result<Uuid>::Ok(rootIt->second);
}
}
