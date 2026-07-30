#include "editor/scene/SceneSerializer.hpp"
#include "runtime/Components.hpp"
#include "runtime/World.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
int failures = 0;

void Check(const bool condition, const std::string& message)
{
    std::cout << (condition ? "  ok   " : "  FAIL ") << message << '\n';
    failures += condition ? 0 : 1;
}

bool Near(const float a, const float b)
{
    return std::abs(a - b) < 0.0001F;
}
}

int main()
{
    using namespace fadix;
    std::cout << "Fadix scene persistence smoke\n";

    World defaultWorld;
    const TransformComponent* defaultSunTransform = nullptr;
    const TransformComponent* defaultMoonTransform = nullptr;
    const DirectionalLightComponent* defaultMoon = nullptr;
    for (const auto [entity, transform, light] :
         defaultWorld.Registry()
             .view<const TransformComponent, const DirectionalLightComponent>()
             .each())
    {
        if (light.CelestialBody == CelestialLightType::Sun)
        {
            defaultSunTransform = &transform;
        }
        else if (light.CelestialBody == CelestialLightType::Moon)
        {
            defaultMoonTransform = &transform;
            defaultMoon = &light;
        }
        static_cast<void>(entity);
    }
    Check(defaultSunTransform != nullptr && defaultMoonTransform != nullptr &&
            defaultMoon != nullptr && defaultMoon->LinkOppositeSun,
        "Default world contains a linked Sun and Moon");
    if (defaultSunTransform != nullptr && defaultMoonTransform != nullptr)
    {
        const glm::vec3 sunDirection =
            glm::normalize(defaultSunTransform->Rotation * glm::vec3{0.0F, 0.0F, -1.0F});
        const glm::vec3 moonDirection =
            glm::normalize(defaultMoonTransform->Rotation * glm::vec3{0.0F, 0.0F, -1.0F});
        Check(glm::dot(sunDirection, moonDirection) < -0.999F,
            "Default Moon starts on the opposite side of the Sun");
    }

    World source{false};
    const entt::entity target = source.Create();
    const Uuid targetId = source.GetUuid(target).value();
    source.Registry().emplace<NameComponent>(target, "Target");

    const entt::entity entity = source.Create();
    const Uuid entityId = source.GetUuid(entity).value();
    source.Registry().emplace<NameComponent>(entity, "Persistent Entity");
    source.Registry().emplace<TransformComponent>(entity, glm::vec3{1.0F, 2.0F, 3.0F});
    source.Registry().emplace<MeshComponent>(entity, MeshComponent{MeshKind::Cube});

    DirectionalLightComponent moon = MakeMoonLight();
    source.Registry().emplace<DirectionalLightComponent>(entity, moon);

    JoltBodyComponent body;
    body.Shape = ColliderShape3D::Box;
    body.AutoFit = false;
    body.HalfExtent = {2.0F, 3.0F, 4.0F};
    body.Mass = 125.5F;
    body.Friction = 1.25F;
    source.Registry().emplace<JoltBodyComponent>(entity, body);

    ScriptComponent script;
    script.ScriptNames = {"PlayerController", "Health"};
    script.Target = targetId;
    source.Registry().emplace<ScriptComponent>(entity, script);

    UICanvasComponent canvas;
    canvas.UIAsset = Uuid::Generate();
    canvas.StyleAsset = Uuid::Generate();
    canvas.RenderInEditor = true;
    canvas.Order = 7;
    canvas.Scale = 1.25F;
    source.Registry().emplace<UICanvasComponent>(entity, canvas);

    TerrainComponent terrain;
    terrain.Heightmap = Uuid::Generate();
    terrain.Width = 240.0F;
    terrain.Depth = 180.0F;
    terrain.HeightScale = 42.0F;
    terrain.LayerCount = 2;
    terrain.Layers[0].Texture = Uuid::Generate();
    terrain.Layers[1].Texture = Uuid::Generate();
    terrain.Layers[1].Tiling = 24.0F;
    source.Registry().emplace<TerrainComponent>(entity, terrain);

    AnimatorComponent animator;
    animator.ClipName = "Run Forward";
    animator.Speed = 1.5F;
    animator.Loop = false;
    animator.Playing = true; // Preview/runtime state must never become authored autoplay.
    animator.Controller.Name = "Character Controller";
    animator.Controller.EntryState = "Run";
    animator.Controller.States.push_back({"Run", "Run Forward", {32.0F, 48.0F}});
    animator.ActiveState = "Run";
    animator.BlendFromClipName = "Idle";
    animator.BlendDuration = 0.4F;
    source.Registry().emplace<AnimatorComponent>(entity, animator);

    TransformAnimatorComponent transformAnimator;
    transformAnimator.ClipName = "Dash";
    transformAnimator.Speed = 0.75F;
    transformAnimator.Loop = true;
    transformAnimator.Playing = true;
    transformAnimator.BlendFromClipName = "Idle";
    transformAnimator.BlendDuration = 0.4F;
    transformAnimator.Controller.Name = "Movement Controller";
    transformAnimator.Controller.EntryState = "Idle";
    transformAnimator.Controller.Parameters.push_back(
        {"Moving", AnimatorParameterType::Bool, false, 0.0F, 0});
    transformAnimator.Controller.States = {
        {"Idle", "Idle", {30.0F, 40.0F}}, {"Dash", "Dash", {230.0F, 40.0F}}};
    AnimatorTransition dashTransition;
    dashTransition.From = "Idle";
    dashTransition.To = "Dash";
    dashTransition.Duration = 0.3F;
    dashTransition.Conditions.push_back({"Moving", AnimatorComparison::Equal, 1.0F});
    transformAnimator.Controller.Transitions.push_back(std::move(dashTransition));
    transformAnimator.ActiveState = "Dash";
    AnimationClipAsset idleClip;
    idleClip.Name = "Idle";
    AnimationClipAsset dashClip;
    dashClip.Name = "Dash";
    AnimationChannel dashChannel;
    dashChannel.JointIndex = -1;
    dashChannel.Target = AnimationChannel::Property::Translation;
    dashChannel.InterpolationMode = AnimationChannel::Interpolation::Smooth;
    dashChannel.Keyframes.push_back({0.0F, glm::vec4{0.0F}});
    dashChannel.Keyframes.push_back({1.25F, glm::vec4{2.0F, 0.0F, 0.0F, 0.0F}});
    dashClip.Duration = 1.25F;
    dashClip.Channels.push_back(std::move(dashChannel));
    dashClip.Events.push_back({0.5F, "Footstep", "left"});
    transformAnimator.Clips.push_back(std::move(idleClip));
    transformAnimator.Clips.push_back(std::move(dashClip));
    source.Registry().emplace<TransformAnimatorComponent>(entity, transformAnimator);

    const std::unique_ptr<IWorld> runtimeClone = source.Clone();
    const auto runtimeEntity = runtimeClone->Find(entityId);
    const AnimatorComponent* runtimeAnimator = runtimeEntity
        ? runtimeClone->Registry().try_get<AnimatorComponent>(*runtimeEntity)
        : nullptr;
    Check(runtimeAnimator != nullptr && !runtimeAnimator->Playing &&
            Near(runtimeAnimator->CurrentTime, 0.0F) && runtimeAnimator->BlendFromClipName.empty() &&
            runtimeAnimator->ActiveState.empty(),
        "Runtime clone waits for code to start animation");
    const TransformAnimatorComponent* runtimeTransformAnimator = runtimeEntity
        ? runtimeClone->Registry().try_get<TransformAnimatorComponent>(*runtimeEntity)
        : nullptr;
    Check(runtimeTransformAnimator != nullptr && !runtimeTransformAnimator->Playing &&
            !runtimeTransformAnimator->Paused && runtimeTransformAnimator->Clips.size() == 2 &&
            runtimeTransformAnimator->BlendFromClipName.empty() &&
            runtimeTransformAnimator->ActiveState.empty(),
        "Runtime clone keeps the transform clip library without autoplay");

    const std::filesystem::path path =
        std::filesystem::current_path() / ".build" / "scene-persistence-smoke.fadixscene";
    SceneDocument document{Uuid::Generate(), "Persistence", path, true};
    SceneService service;
    const Result<void> saved = service.Save(document, source);
    Check(saved.IsOk(), saved ? "Scene saves" : saved.ErrorMessage());
    Check(!document.Dirty, "Successful save clears the dirty flag");

    World loaded{false};
    SceneDocument loadedDocument;
    const Result<void> loadedResult = service.Load(loadedDocument, loaded, path);
    Check(loadedResult.IsOk(), loadedResult ? "Scene loads" : loadedResult.ErrorMessage());

    const auto loadedEntity = loaded.Find(entityId);
    Check(loadedEntity.has_value(), "Saved entity is restored");
    if (loadedEntity)
    {
        const entt::registry& registry = loaded.Registry();
        const auto* loadedBody = registry.try_get<JoltBodyComponent>(*loadedEntity);
        Check(loadedBody != nullptr && Near(loadedBody->Mass, 125.5F) &&
                Near(loadedBody->Friction, 1.25F) &&
                loadedBody->Shape == ColliderShape3D::Box && !loadedBody->AutoFit,
            "Collider shape, fitting, mass and friction survive reload");

        const auto* loadedMoon = registry.try_get<DirectionalLightComponent>(*loadedEntity);
        Check(loadedMoon != nullptr && loadedMoon->CelestialBody == CelestialLightType::Moon &&
                loadedMoon->LinkOppositeSun,
            "Moon role and opposite-Sun link survive reload");

        const auto* loadedScript = registry.try_get<ScriptComponent>(*loadedEntity);
        Check(loadedScript != nullptr && loadedScript->ScriptNames == script.ScriptNames &&
                loadedScript->Target == targetId,
            "Scripts and entity target survive reload");

        const auto* loadedCanvas = registry.try_get<UICanvasComponent>(*loadedEntity);
        Check(loadedCanvas != nullptr && loadedCanvas->UIAsset == canvas.UIAsset &&
                loadedCanvas->StyleAsset == canvas.StyleAsset && loadedCanvas->Order == 7,
            "UI Canvas settings survive reload");

        const auto* loadedTerrain = registry.try_get<TerrainComponent>(*loadedEntity);
        Check(loadedTerrain != nullptr && loadedTerrain->Heightmap == terrain.Heightmap &&
                loadedTerrain->LayerCount == 2 && Near(loadedTerrain->Layers[1].Tiling, 24.0F),
            "Terrain and layer settings survive reload");

        const auto* loadedAnimator = registry.try_get<AnimatorComponent>(*loadedEntity);
        Check(loadedAnimator != nullptr && loadedAnimator->ClipName == animator.ClipName &&
                Near(loadedAnimator->Speed, 1.5F) && !loadedAnimator->Loop &&
                !loadedAnimator->Playing && loadedAnimator->BlendFromClipName.empty() &&
                loadedAnimator->ActiveState.empty() &&
                loadedAnimator->Controller.EntryState == "Run" &&
                loadedAnimator->Controller.States.size() == 1,
            "Animator controller survives reload without autoplay");

        const auto* loadedTransformAnimator =
            registry.try_get<TransformAnimatorComponent>(*loadedEntity);
        Check(loadedTransformAnimator != nullptr && loadedTransformAnimator->ClipName == "Dash" &&
                loadedTransformAnimator->Clips.size() == 2 &&
                loadedTransformAnimator->Clips[1].Name == "Dash" &&
                loadedTransformAnimator->Clips[1].Channels.size() == 1 &&
                loadedTransformAnimator->Clips[1].Events.size() == 1 &&
                loadedTransformAnimator->Clips[1].Events[0].Name == "Footstep" &&
                loadedTransformAnimator->Clips[1].Events[0].Payload == "left" &&
                loadedTransformAnimator->Clips[1].Channels[0].InterpolationMode ==
                    AnimationChannel::Interpolation::Smooth &&
                Near(loadedTransformAnimator->Clips[1].Duration, 1.25F) &&
                !loadedTransformAnimator->Playing &&
                loadedTransformAnimator->BlendFromClipName.empty() &&
                loadedTransformAnimator->ActiveState.empty() &&
                loadedTransformAnimator->Controller.Parameters.size() == 1 &&
                loadedTransformAnimator->Controller.States.size() == 2 &&
                loadedTransformAnimator->Controller.Transitions.size() == 1 &&
                loadedTransformAnimator->Controller.Transitions[0].Conditions.size() == 1,
            "Named transform clips and controller survive reload without autoplay");
    }

    const Uuid legacyId = Uuid::Generate();
    {
        std::ofstream legacy{path, std::ios::trunc};
        legacy << "FADIX_SCENE 1\nENTITY " << legacyId.ToString()
               << " \"Legacy Entity\" 1 0 0 0 1 0 0 0 1 1 1 - "
                  "J 0.5 0.5 0.5 1 K 0 1 Y 1 \"LegacyScript\" 1 AL 0 "
                  "TA \"Legacy Move\" 1 1 0 1 T Linear 1 0 0 0 0 0 END\n";
    }
    World legacyWorld{false};
    SceneDocument legacyDocument;
    const Result<void> legacyResult = service.Load(legacyDocument, legacyWorld, path);
    Check(legacyResult.IsOk(), "Older scene format still loads");
    if (const auto legacyEntity = legacyWorld.Find(legacyId))
    {
        const entt::registry& registry = legacyWorld.Registry();
        const auto* legacyBody = registry.try_get<JoltBodyComponent>(*legacyEntity);
        const auto* legacyTransformAnimator =
            registry.try_get<TransformAnimatorComponent>(*legacyEntity);
        Check(legacyTransformAnimator != nullptr && legacyTransformAnimator->Clips.size() == 1 &&
                legacyTransformAnimator->ClipName == "Legacy Move",
            "Legacy single transform clip migrates into the named library");
        Check(legacyBody != nullptr && Near(legacyBody->Mass, 1.0F) &&
                Near(legacyBody->Friction, 0.8F),
            "Older colliders receive safe mass and friction defaults");
        Check(registry.all_of<ScriptComponent, AudioListenerComponent>(*legacyEntity),
            "Older script records do not consume the following component");
    }
    else
    {
        Check(false, "Older scene entity is restored");
    }

    // Scene.load-by-display-name: the root can be renamed away from "Main Scene" and
    // is still identified structurally (parentless entity that others hang under).
    {
        const Uuid rootId = Uuid::Generate();
        const Uuid childId = Uuid::Generate();
        std::ofstream named{path, std::ios::trunc};
        named << "FADIX_SCENE 1\n"
              << "ENTITY " << rootId.ToString()
              << " \"Lobby Scene\" 1 0 0 0 1 0 0 0 1 1 1 - END\n"
              << "ENTITY " << childId.ToString() << " \"Camera\" 1 0 0 0 1 0 0 0 1 1 1 "
              << rootId.ToString() << " END\n";
    }
    const auto display = SceneSerializer::PeekDisplayName(path);
    Check(display.has_value() && *display == "Lobby Scene",
        "PeekDisplayName returns the renamed scene root display name");

    std::error_code cleanup;
    std::filesystem::remove(path, cleanup);
    return failures == 0 ? 0 : 1;
}
