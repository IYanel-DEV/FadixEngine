// Smoke tests for the native 2D foundation: Sprite2DComponent, RigidBody2DComponent,
// Collider2DComponent, SpriteFrameAnimatorComponent, TileMapComponent round-trips and
// Box2DBodyComponent legacy compat. Exits non-zero on any failure.

#include "editor/scene/SceneSerializer.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "engine/Uuid.hpp"
#include "runtime/Components.hpp"
#include "runtime/World.hpp"

#include <glm/glm.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
int g_Failures = 0;

void Check(const bool condition, const std::string& label)
{
    if (condition)
    {
        std::cout << "  ok   " << label << '\n';
    }
    else
    {
        std::cerr << "  FAIL " << label << '\n';
        ++g_Failures;
    }
}

std::filesystem::path TempPath(const char* name)
{
    return std::filesystem::temp_directory_path() / name;
}

void TestSprite2D()
{
    using namespace fadix;
    std::cout << "[2d-smoke] Sprite2DComponent round-trip\n";

    World source{false};
    const entt::entity ent = source.Create();
    source.Registry().emplace<NameComponent>(ent, "MySpr");
    source.Registry().emplace<TransformComponent>(ent);

    Sprite2DComponent spr;
    spr.Tint = {0.5F, 0.25F, 1.0F, 0.8F};
    spr.Size = {2.5F, 1.5F};
    spr.Pivot = {0.0F, 1.0F};
    spr.UvRect = {0.1F, 0.2F, 0.8F, 0.7F};
    spr.FlipX = true;
    spr.FlipY = false;
    spr.SortingLayer = 3;
    spr.OrderInLayer = -2;
    spr.PixelsPerUnit = 64.0F;
    spr.NearestFilter = true;
    spr.PixelSnap = true;
    source.Registry().emplace<Sprite2DComponent>(ent, spr);

    const std::filesystem::path path = TempPath("fadix_2d_sprite.fadix");
    SceneDocument doc{Uuid::Generate(), "SprTest", path, true};
    SceneService service;
    const auto saved = service.Save(doc, source);
    Check(saved.IsOk(), "Sprite2D scene saves");

    World loaded{false};
    SceneDocument loadedDoc;
    const auto loadedResult = service.Load(loadedDoc, loaded, path);
    Check(loadedResult.IsOk(), "Sprite2D scene loads");

    bool found = false;
    for (const auto [e, sp] : loaded.Registry().view<const Sprite2DComponent>().each())
    {
        found = true;
        Check(std::abs(sp.Tint.r - 0.5F) < 0.01F, "tint.r preserved");
        Check(std::abs(sp.Size.x - 2.5F) < 0.01F, "size.x preserved");
        Check(std::abs(sp.Pivot.y - 1.0F) < 0.01F, "pivot.y preserved");
        Check(std::abs(sp.UvRect.z - 0.8F) < 0.01F, "uvrect.z preserved");
        Check(sp.FlipX, "flipX preserved");
        Check(!sp.FlipY, "flipY preserved");
        Check(sp.SortingLayer == 3, "sortingLayer preserved");
        Check(sp.OrderInLayer == -2, "orderInLayer preserved");
        Check(std::abs(sp.PixelsPerUnit - 64.0F) < 0.01F, "pixelsPerUnit preserved");
        Check(sp.NearestFilter, "nearestFilter preserved");
        Check(sp.PixelSnap, "pixelSnap preserved");
        static_cast<void>(e);
    }
    Check(found, "Sprite2DComponent entity found after load");
}

void TestRigidBody2D()
{
    using namespace fadix;
    std::cout << "[2d-smoke] RigidBody2D + Collider2D round-trip\n";

    World source{false};
    const entt::entity ent = source.Create();
    source.Registry().emplace<NameComponent>(ent, "Body");
    source.Registry().emplace<TransformComponent>(ent);

    RigidBody2DComponent rb;
    rb.Type = Body2DType::Kinematic;
    rb.Mass = 5.0F;
    rb.GravityScale = 0.5F;
    rb.LinearDamping = 0.1F;
    rb.AngularDamping = 0.2F;
    rb.FixedRotation = true;
    rb.InitialLinearVelocity = {3.0F, -1.0F};
    rb.InitialAngularVelocity = 0.3F;
    source.Registry().emplace<RigidBody2DComponent>(ent, rb);

    Collider2DComponent col;
    col.Shape = Collider2DShape::Circle;
    col.Offset = {0.1F, 0.2F};
    col.Size = {0.75F, 0.75F};
    col.Friction = 0.4F;
    col.Restitution = 0.6F;
    col.Density = 2.5F;
    col.Sensor = true;
    source.Registry().emplace<Collider2DComponent>(ent, col);

    const std::filesystem::path path = TempPath("fadix_2d_rigidbody.fadix");
    SceneDocument doc{Uuid::Generate(), "RbTest", path, true};
    SceneService service;
    const auto saved = service.Save(doc, source);
    Check(saved.IsOk(), "RigidBody2D scene saves");

    World loaded{false};
    SceneDocument loadedDoc;
    const auto loadedResult = service.Load(loadedDoc, loaded, path);
    Check(loadedResult.IsOk(), "RigidBody2D scene loads");

    bool foundRb = false;
    for (const auto [e, r] : loaded.Registry().view<const RigidBody2DComponent>().each())
    {
        foundRb = true;
        Check(r.Type == Body2DType::Kinematic, "rb2d type preserved");
        Check(std::abs(r.Mass - 5.0F) < 0.01F, "rb2d mass preserved");
        Check(std::abs(r.GravityScale - 0.5F) < 0.01F, "rb2d gravityScale preserved");
        Check(r.FixedRotation, "rb2d fixedRotation preserved");
        Check(std::abs(r.InitialLinearVelocity.x - 3.0F) < 0.01F, "rb2d vel.x preserved");
        Check(r.Handle == InvalidPhysicsBody, "rb2d handle reset on load");
        static_cast<void>(e);
    }
    Check(foundRb, "RigidBody2DComponent found after load");

    bool foundCol = false;
    for (const auto [e, c] : loaded.Registry().view<const Collider2DComponent>().each())
    {
        foundCol = true;
        Check(c.Shape == Collider2DShape::Circle, "col shape preserved");
        Check(std::abs(c.Offset.x - 0.1F) < 0.01F, "col offset.x preserved");
        Check(std::abs(c.Size.x - 0.75F) < 0.01F, "col size.x preserved");
        Check(std::abs(c.Friction - 0.4F) < 0.01F, "col friction preserved");
        Check(std::abs(c.Restitution - 0.6F) < 0.01F, "col restitution preserved");
        Check(c.Sensor, "col sensor preserved");
        static_cast<void>(e);
    }
    Check(foundCol, "Collider2DComponent found after load");
}

void TestSpriteFrameAnimator()
{
    using namespace fadix;
    std::cout << "[2d-smoke] SpriteFrameAnimator round-trip\n";

    World source{false};
    const entt::entity ent = source.Create();
    source.Registry().emplace<NameComponent>(ent, "Anim");
    source.Registry().emplace<TransformComponent>(ent);

    SpriteFrameAnimatorComponent sfa;
    sfa.Autoplay = true;
    sfa.Speed = 1.5F;
    SpriteAnimationClip clip;
    clip.Name = "Walk";
    clip.Loop = true;
    SpriteFrame f1{{0.0F, 0.0F, 0.25F, 1.0F}, 0.1F};
    SpriteFrame f2{{0.25F, 0.0F, 0.25F, 1.0F}, 0.1F};
    clip.Frames.push_back(f1);
    clip.Frames.push_back(f2);
    sfa.Clips.push_back(clip);
    sfa.CurrentClip = "Walk";
    source.Registry().emplace<SpriteFrameAnimatorComponent>(ent, sfa);

    const std::filesystem::path path = TempPath("fadix_2d_animator.fadix");
    SceneDocument doc{Uuid::Generate(), "AnimTest", path, true};
    SceneService service;
    const auto saved = service.Save(doc, source);
    Check(saved.IsOk(), "SpriteFrameAnimator scene saves");

    World loaded{false};
    SceneDocument loadedDoc;
    const auto loadedResult = service.Load(loadedDoc, loaded, path);
    Check(loadedResult.IsOk(), "SpriteFrameAnimator scene loads");

    bool found = false;
    for (const auto [e, a] : loaded.Registry().view<const SpriteFrameAnimatorComponent>().each())
    {
        found = true;
        Check(a.Autoplay, "sfa autoplay preserved");
        Check(std::abs(a.Speed - 1.5F) < 0.01F, "sfa speed preserved");
        Check(a.Clips.size() == 1, "sfa clip count preserved");
        Check(!a.Clips.empty() && a.Clips[0].Name == "Walk", "sfa clip name preserved");
        Check(!a.Clips.empty() && a.Clips[0].Frames.size() == 2, "sfa frame count preserved");
        Check(!a.Clips.empty() && a.Clips[0].Loop, "sfa loop preserved");
        Check(a.CurrentClip == "Walk", "sfa currentClip preserved");
        static_cast<void>(e);
    }
    Check(found, "SpriteFrameAnimatorComponent found after load");
}

void TestTileMap()
{
    using namespace fadix;
    std::cout << "[2d-smoke] TileMapComponent round-trip\n";

    World source{false};
    const entt::entity ent = source.Create();
    source.Registry().emplace<NameComponent>(ent, "Map");
    source.Registry().emplace<TransformComponent>(ent);

    TileMapComponent tm = MakeDefaultTileMap();
    tm.TileWidth = 32;
    tm.TileHeight = 32;
    tm.GridWidth = 3;
    tm.GridHeight = 2;
    tm.SheetColumns = 8;
    tm.PixelsPerUnit = 32.0F;
    tm.LayerCount = 1;
    tm.TileData = {1, 2, 3, -1, 5, 6};
    source.Registry().emplace<TileMapComponent>(ent, tm);

    const std::filesystem::path path = TempPath("fadix_2d_tilemap.fadix");
    SceneDocument doc{Uuid::Generate(), "TileTest", path, true};
    SceneService service;
    const auto saved = service.Save(doc, source);
    Check(saved.IsOk(), "TileMap scene saves");

    World loaded{false};
    SceneDocument loadedDoc;
    const auto loadedResult = service.Load(loadedDoc, loaded, path);
    Check(loadedResult.IsOk(), "TileMap scene loads");

    bool found = false;
    for (const auto [e, t] : loaded.Registry().view<const TileMapComponent>().each())
    {
        found = true;
        Check(t.TileWidth == 32, "tilemap tileWidth preserved");
        Check(t.GridWidth == 3, "tilemap gridWidth preserved");
        Check(t.GridHeight == 2, "tilemap gridHeight preserved");
        Check(t.SheetColumns == 8, "tilemap sheetColumns preserved");
        Check(t.LayerCount == 1, "tilemap layerCount preserved");
        Check(t.TileData.size() == 6, "tilemap tileData count preserved");
        Check(!t.TileData.empty() && t.TileData[0] == 1, "tilemap tile[0] preserved");
        Check(t.TileData.size() >= 4 && t.TileData[3] == -1, "tilemap empty tile preserved");
        static_cast<void>(e);
    }
    Check(found, "TileMapComponent found after load");
}

void TestBox2DLegacy()
{
    using namespace fadix;
    std::cout << "[2d-smoke] Box2DBodyComponent legacy compat\n";

    World source{false};
    const entt::entity ent = source.Create();
    source.Registry().emplace<NameComponent>(ent, "Old");
    source.Registry().emplace<TransformComponent>(ent);

    Box2DBodyComponent legacy;
    legacy.HalfExtent = {0.7F, 0.4F};
    legacy.Dynamic = true;
    source.Registry().emplace<Box2DBodyComponent>(ent, legacy);

    const std::filesystem::path path = TempPath("fadix_2d_legacy_box2d.fadix");
    SceneDocument doc{Uuid::Generate(), "LegacyTest", path, true};
    SceneService service;
    const auto saved = service.Save(doc, source);
    Check(saved.IsOk(), "Legacy Box2D scene saves");

    World loaded{false};
    SceneDocument loadedDoc;
    const auto loadedResult = service.Load(loadedDoc, loaded, path);
    Check(loadedResult.IsOk(), "Legacy Box2D scene loads");

    bool found = false;
    for (const auto [e, b] : loaded.Registry().view<const Box2DBodyComponent>().each())
    {
        found = true;
        Check(std::abs(b.HalfExtent.x - 0.7F) < 0.01F, "legacy halfExtent.x preserved");
        Check(b.Dynamic, "legacy dynamic preserved");
        static_cast<void>(e);
    }
    Check(found, "Box2DBodyComponent (legacy) found after load");
}

} // namespace

int main()
{
    std::cout << "=== fadix_2d_smoke ===\n";
    TestSprite2D();
    TestRigidBody2D();
    TestSpriteFrameAnimator();
    TestTileMap();
    TestBox2DLegacy();
    if (g_Failures == 0)
    {
        std::cout << "all checks passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << g_Failures << " check(s) failed\n";
    return EXIT_FAILURE;
}
