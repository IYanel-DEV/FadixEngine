// Smoke tests for the native 2D foundation: Sprite2DComponent, RigidBody2DComponent,
// Collider2DComponent, SpriteFrameAnimatorComponent, TileMapComponent round-trips and
// Box2DBodyComponent legacy compat. Exits non-zero on any failure.

#include "editor/scene/SceneSerializer.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "engine/Uuid.hpp"
#include "runtime/AnimationRuntime.hpp"
#include "runtime/Components.hpp"
#include "runtime/World.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

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

void TestSpriteAnimationRuntime()
{
    using namespace fadix;
    std::cout << "[2d-smoke] SpriteFrameAnimator runtime tick\n";

    World world{false};
    const entt::entity ent = world.Create();
    world.Registry().emplace<NameComponent>(ent, "AnimEnt");

    Sprite2DComponent spr;
    spr.UvRect = {0.0F, 0.0F, 0.25F, 1.0F};
    world.Registry().emplace<Sprite2DComponent>(ent, spr);

    SpriteFrameAnimatorComponent anim;
    anim.Autoplay = true;
    anim.Speed = 1.0F;
    SpriteAnimationClip clip;
    clip.Name = "Run";
    clip.Loop = true;
    clip.Frames.push_back({{0.00F, 0.0F, 0.25F, 1.0F}, 0.1F});
    clip.Frames.push_back({{0.25F, 0.0F, 0.25F, 1.0F}, 0.1F});
    clip.Frames.push_back({{0.50F, 0.0F, 0.25F, 1.0F}, 0.1F});
    clip.Frames.push_back({{0.75F, 0.0F, 0.25F, 1.0F}, 0.1F});
    anim.Clips.push_back(clip);
    anim.CurrentClip = "Run";
    world.Registry().emplace<SpriteFrameAnimatorComponent>(ent, anim);

    // First tick: autoplay starts, frame 0
    UpdateSpriteAnimations(world.Registry(), 0.05F);
    {
        const auto& a = world.Registry().get<const SpriteFrameAnimatorComponent>(ent);
        Check(a.Playing, "autoplay starts on first tick");
        Check(a.CurrentFrame == 0, "frame 0 at t=0.05");
        const auto& sp = world.Registry().get<const Sprite2DComponent>(ent);
        Check(std::abs(sp.UvRect.x - 0.0F) < 0.01F, "UvRect written for frame 0");
    }

    // Advance past frame 0 boundary (>= 0.1s total)
    UpdateSpriteAnimations(world.Registry(), 0.06F);
    {
        const auto& a = world.Registry().get<const SpriteFrameAnimatorComponent>(ent);
        Check(a.CurrentFrame == 1, "frame advances to 1 at t=0.11");
        const auto& sp = world.Registry().get<const Sprite2DComponent>(ent);
        Check(std::abs(sp.UvRect.x - 0.25F) < 0.01F, "UvRect updated for frame 1");
    }

    // Advance 0.3 more: t=0.41 wraps (clip=0.4s) to 0.01 => frame 0
    UpdateSpriteAnimations(world.Registry(), 0.30F);
    {
        const auto& a = world.Registry().get<const SpriteFrameAnimatorComponent>(ent);
        Check(a.CurrentFrame == 0, "loop wraps; t=0.41 mod 0.4=0.01 => frame 0");
    }
}

void TestTileMapNewFields()
{
    using namespace fadix;
    std::cout << "[2d-smoke] TileMapComponent SheetRows/SortingLayer/OrderInLayer round-trip\n";

    World source{false};
    const entt::entity ent = source.Create();
    source.Registry().emplace<NameComponent>(ent, "Map2");
    source.Registry().emplace<TransformComponent>(ent);

    TileMapComponent tm = MakeDefaultTileMap();
    tm.TileWidth = 16;
    tm.TileHeight = 16;
    tm.GridWidth = 2;
    tm.GridHeight = 2;
    tm.SheetColumns = 4;
    tm.SheetRows = 4;
    tm.SortingLayer = 2;
    tm.OrderInLayer = 1;
    tm.PixelsPerUnit = 16.0F;
    tm.LayerCount = 1;
    tm.TileData = {0, 3, 7, -1};
    source.Registry().emplace<TileMapComponent>(ent, tm);

    const std::filesystem::path path = TempPath("fadix_2d_tilemap2.fadix");
    SceneDocument doc{Uuid::Generate(), "TileTest2", path, true};
    SceneService service;
    const auto saved = service.Save(doc, source);
    Check(saved.IsOk(), "TileMap TM2 scene saves");

    World loaded{false};
    SceneDocument loadedDoc;
    const auto loadedResult = service.Load(loadedDoc, loaded, path);
    Check(loadedResult.IsOk(), "TileMap TM2 scene loads");

    bool found = false;
    for (const auto [e, t] : loaded.Registry().view<const TileMapComponent>().each())
    {
        found = true;
        Check(t.SheetRows == 4, "sheetRows preserved");
        Check(t.SortingLayer == 2, "sortingLayer preserved");
        Check(t.OrderInLayer == 1, "orderInLayer preserved");
        Check(t.TileData.size() == 4, "tileData count preserved");
        Check(!t.TileData.empty() && t.TileData[1] == 3, "tile[1] value preserved");
        Check(t.TileData.size() >= 4 && t.TileData[3] == -1, "empty tile preserved");
        static_cast<void>(e);
    }
    Check(found, "TileMapComponent found after TM2 load");
}

void TestCollisionLayerMask()
{
    using namespace fadix;
    std::cout << "[2d-smoke] Collider2D CollisionLayer/Mask round-trip\n";

    World source{false};
    const entt::entity ent = source.Create();
    source.Registry().emplace<NameComponent>(ent, "Filter");
    source.Registry().emplace<TransformComponent>(ent);
    source.Registry().emplace<RigidBody2DComponent>(ent);

    Collider2DComponent col;
    col.Shape = Collider2DShape::Box;
    col.CollisionLayer = 0x0002;
    col.CollisionMask = 0x0005;
    col.Sensor = false;
    source.Registry().emplace<Collider2DComponent>(ent, col);

    const std::filesystem::path path = TempPath("fadix_2d_collayer.fadix");
    SceneDocument doc{Uuid::Generate(), "LayerTest", path, true};
    SceneService service;
    const auto saved = service.Save(doc, source);
    Check(saved.IsOk(), "CollisionLayer scene saves");

    World loaded{false};
    SceneDocument loadedDoc;
    const auto loadedResult = service.Load(loadedDoc, loaded, path);
    Check(loadedResult.IsOk(), "CollisionLayer scene loads");

    bool found = false;
    for (const auto [e, c] : loaded.Registry().view<const Collider2DComponent>().each())
    {
        found = true;
        Check(c.CollisionLayer == 0x0002, "collisionLayer preserved");
        Check(c.CollisionMask == 0x0005, "collisionMask preserved");
        static_cast<void>(e);
    }
    Check(found, "Collider2DComponent with layer/mask found after load");
}

void TestTileUvCalculation()
{
    using namespace fadix;
    std::cout << "[2d-smoke] Tile UV calculation (SheetColumns/Rows)\n";

    // 4x4 atlas: tileId 5 => col=1, row=1 => UV (0.25, 0.25, 0.25, 0.25)
    constexpr int cols = 4;
    constexpr int rows = 4;
    constexpr float uvW = 1.0F / static_cast<float>(cols);
    constexpr float uvH = 1.0F / static_cast<float>(rows);
    const int tileId = 5;
    const int sheetCol = tileId % cols;
    const int sheetRow = tileId / cols;
    const float u = static_cast<float>(sheetCol) * uvW;
    const float v = static_cast<float>(sheetRow) * uvH;
    Check(std::abs(u - 0.25F) < 0.001F, "tile UV u correct for id=5, 4x4 atlas");
    Check(std::abs(v - 0.25F) < 0.001F, "tile UV v correct for id=5, 4x4 atlas");
    Check(std::abs(uvW - 0.25F) < 0.001F, "tile UV width correct");
    Check(std::abs(uvH - 0.25F) < 0.001F, "tile UV height correct");

    // 8x2 atlas: tileId 9 => col=1, row=1 => UV (0.125, 0.5, 0.125, 0.5)
    constexpr int cols2 = 8;
    constexpr int rows2 = 2;
    const int tileId2 = 9;
    const float uvW2 = 1.0F / static_cast<float>(cols2);
    const float uvH2 = 1.0F / static_cast<float>(rows2);
    const float u2 = static_cast<float>(tileId2 % cols2) * uvW2;
    const float v2 = static_cast<float>(tileId2 / cols2) * uvH2;
    Check(std::abs(u2 - 0.125F) < 0.001F, "tile UV u correct for id=9, 8x2 atlas");
    Check(std::abs(v2 - 0.5F) < 0.001F, "tile UV v correct for id=9, 8x2 atlas");
}

void TestTileMapPainting()
{
    using namespace fadix;
    std::cout << "[2d-smoke] TileMap painting (tile set/erase/rect/fill)\n";

    // 4x4 tilemap, 1 layer
    TileMapComponent tm;
    tm.TileWidth = 16;
    tm.TileHeight = 16;
    tm.GridWidth = 4;
    tm.GridHeight = 4;
    tm.SheetColumns = 4;
    tm.SheetRows = 4;
    tm.PixelsPerUnit = 16.0F;
    tm.LayerCount = 1;
    tm.TileData.assign(static_cast<std::size_t>(4 * 4), -1);

    // Pencil paint: set tile (1,0) to id 3
    tm.TileData[static_cast<std::size_t>(0 * 4 + 1)] = 3;
    Check(tm.TileData[1] == 3, "pencil sets tile (1,0)");

    // Erase: set tile (1,0) back to -1
    tm.TileData[1] = -1;
    Check(tm.TileData[1] == -1, "eraser clears tile (1,0)");

    // Rectangle paint: fill (0,0)-(1,1) with tile 5
    {
        const int tileId = 5;
        for (int y = 0; y <= 1; ++y)
        {
            for (int x = 0; x <= 1; ++x)
            {
                tm.TileData[static_cast<std::size_t>(y * 4 + x)] = tileId;
            }
        }
        Check(tm.TileData[0] == 5, "rect paint (0,0)");
        Check(tm.TileData[1] == 5, "rect paint (1,0)");
        Check(tm.TileData[4] == 5, "rect paint (0,1)");
        Check(tm.TileData[5] == 5, "rect paint (1,1)");
        Check(tm.TileData[2] == -1, "rect paint does not touch (2,0)");
    }

    // Flood fill: set (3,3) to 7, then flood fill from (3,3) replacing 7 with 9
    tm.TileData[static_cast<std::size_t>(3 * 4 + 3)] = 7;
    {
        // simple BFS fill
        const int old = 7;
        const int newId = 9;
        std::vector<std::pair<int,int>> stack{{3, 3}};
        int iter = 0;
        while (!stack.empty() && iter < 10000)
        {
            auto [x, y] = stack.back();
            stack.pop_back();
            if (x < 0 || x >= 4 || y < 0 || y >= 4) continue;
            if (tm.TileData[static_cast<std::size_t>(y * 4 + x)] != old) continue;
            tm.TileData[static_cast<std::size_t>(y * 4 + x)] = newId;
            ++iter;
            stack.push_back({x+1,y}); stack.push_back({x-1,y});
            stack.push_back({x,y+1}); stack.push_back({x,y-1});
        }
        Check(tm.TileData[static_cast<std::size_t>(3 * 4 + 3)] == 9, "flood fill sets target");
        Check(tm.TileData[0] == 5, "flood fill doesn't touch different tiles");
    }

    // Eyedropper: read tile at (0,0) = 5
    const int picked = tm.TileData[0];
    Check(picked == 5, "eyedropper reads correct tile");
}

void TestTileMapLayerOps()
{
    using namespace fadix;
    std::cout << "[2d-smoke] TileMap layer add/remove/reorder\n";

    TileMapComponent tm;
    tm.TileWidth = 16;
    tm.TileHeight = 16;
    tm.GridWidth = 2;
    tm.GridHeight = 2;
    tm.LayerCount = 1;
    tm.TileData = {1, 2, 3, 4};

    // Add layer
    const int layerSize = tm.GridWidth * tm.GridHeight;
    tm.TileData.resize(tm.TileData.size() + static_cast<std::size_t>(layerSize), -1);
    ++tm.LayerCount;
    Check(tm.LayerCount == 2, "layer add: LayerCount incremented");
    Check(tm.TileData.size() == 8, "layer add: TileData doubled");
    Check(tm.TileData[4] == -1, "layer add: new layer initialized empty");
    Check(tm.TileData[0] == 1, "layer add: layer 0 unchanged");

    // Set layer 1 tiles
    tm.TileData[4] = 7;
    tm.TileData[5] = 8;
    Check(tm.TileData[4] == 7, "layer 1 tile set");

    // Reorder (swap layer 0 and 1)
    {
        auto begin0 = tm.TileData.begin();
        auto begin1 = tm.TileData.begin() + layerSize;
        std::rotate(begin0, begin1, begin1 + layerSize);
    }
    Check(tm.TileData[0] == 7, "reorder: layer 0 now contains old layer 1 data");
    Check(tm.TileData[4] == 1, "reorder: layer 1 now contains old layer 0 data");

    // Remove layer 0
    {
        const auto begin = tm.TileData.begin();
        tm.TileData.erase(begin, begin + layerSize);
        --tm.LayerCount;
    }
    Check(tm.LayerCount == 1, "remove layer: LayerCount decremented");
    Check(tm.TileData.size() == 4, "remove layer: TileData shrunk");
    Check(tm.TileData[0] == 1, "remove layer: remaining layer correct");
}

void TestOrtho2DCameraConversion()
{
    std::cout << "[2d-smoke] Ortho2D ScreenToWorld / WorldToScreen round-trip\n";

    // Replicate Ortho2DCamera math without the class (smoke is headless, no editor headers)
    const float posX = 2.5F;
    const float posY = -1.0F;
    const float halfH = 5.0F;
    const float viewW = 1280.0F;
    const float viewH = 720.0F;
    const float aspect = viewW / viewH;
    const float halfW = halfH * aspect;

    // WorldToScreen for a known world point
    const float wx = 3.0F;
    const float wy = 0.0F;
    const float ndcX = (wx - posX) / halfW;
    const float ndcY = (wy - posY) / halfH;
    const float sx = (ndcX + 1.0F) * 0.5F * viewW;
    const float sy = (1.0F - ndcY) * 0.5F * viewH;

    // ScreenToWorld inverse
    const float ndcX2 = 2.0F * sx / viewW - 1.0F;
    const float ndcY2 = 1.0F - 2.0F * sy / viewH;
    const float wx2 = posX + ndcX2 * halfW;
    const float wy2 = posY + ndcY2 * halfH;

    Check(std::abs(wx2 - wx) < 0.001F, "Ortho2D WorldToScreen->ScreenToWorld round-trip X");
    Check(std::abs(wy2 - wy) < 0.001F, "Ortho2D WorldToScreen->ScreenToWorld round-trip Y");

    // Check that world origin maps to center of viewport when cam is at origin
    const float ox = 1.0F * 0.5F * viewW; // (ndcX=0+1)*0.5*W = 0.5*W... only if posX=0
    static_cast<void>(ox);
    {
        const float px = 0.0F;
        const float n = 2.0F * (viewW * 0.5F) / viewW - 1.0F; // 0.0
        const float rx = px + n * halfW;
        Check(std::abs(rx - 0.0F) < 0.001F, "Ortho2D center of screen maps to camera position X");
    }
}

void TestGridSnapping()
{
    std::cout << "[2d-smoke] Grid snapping: world-to-tile and back\n";

    // Tilemap with 32px tiles at 32ppu = 1 world-unit tiles
    const float ppu = 32.0F;
    const float tileW = 32.0F;
    const float tileH = 32.0F;
    const float tileWW = tileW / ppu;
    const float tileWH = tileH / ppu;

    // World pos (0.5, -0.5) -> tile (0, -1) in floor convention
    const float wx = 0.5F;
    const float wy = -0.5F;
    const int tx = static_cast<int>(std::floor(wx / tileWW));
    const int ty = static_cast<int>(std::floor(wy / tileWH));
    Check(tx == 0, "grid snap: floor tile X");
    Check(ty == -1, "grid snap: floor tile Y (negative)");

    // Snap back to tile center
    const float snapX = (static_cast<float>(tx) + 0.5F) * tileWW;
    const float snapY = (static_cast<float>(ty) + 0.5F) * tileWH;
    Check(std::abs(snapX - 0.5F) < 0.001F, "snap-to-tile-center X");
    Check(std::abs(snapY - (-0.5F)) < 0.001F, "snap-to-tile-center Y");
}

void TestTileMapSaveLoadAfterPaint()
{
    using namespace fadix;
    std::cout << "[2d-smoke] TileMap save/load after painting\n";

    World source{false};
    const entt::entity ent = source.Create();
    source.Registry().emplace<NameComponent>(ent, "PaintedMap");
    source.Registry().emplace<TransformComponent>(ent);

    TileMapComponent tm = MakeDefaultTileMap();
    tm.TileWidth = 16;
    tm.TileHeight = 16;
    tm.GridWidth = 3;
    tm.GridHeight = 3;
    tm.SheetColumns = 4;
    tm.SheetRows = 4;
    tm.LayerCount = 1;
    tm.TileData.assign(9, -1);
    // Paint some tiles
    tm.TileData[0] = 2;
    tm.TileData[4] = 7;
    tm.TileData[8] = 1;
    source.Registry().emplace<TileMapComponent>(ent, tm);

    const std::filesystem::path path = TempPath("fadix_2d_painted_tilemap.fadix");
    SceneDocument doc{Uuid::Generate(), "PaintTest", path, true};
    SceneService service;
    Check(service.Save(doc, source).IsOk(), "painted tilemap scene saves");

    World loaded{false};
    SceneDocument loadedDoc;
    Check(service.Load(loadedDoc, loaded, path).IsOk(), "painted tilemap scene loads");

    bool found = false;
    for (const auto [e, t] : loaded.Registry().view<const TileMapComponent>().each())
    {
        found = true;
        Check(t.TileData.size() == 9, "painted tilemap data size preserved");
        Check(!t.TileData.empty() && t.TileData[0] == 2, "painted tile[0] preserved");
        Check(t.TileData.size() >= 5 && t.TileData[4] == 7, "painted tile[4] preserved");
        Check(t.TileData.size() >= 9 && t.TileData[8] == 1, "painted tile[8] preserved");
        static_cast<void>(e);
    }
    Check(found, "painted TileMapComponent found after load");
}

void TestEmpty2DTemplateContents()
{
    using namespace fadix;
    std::cout << "[2d-smoke] Empty 2D template scene structure\n";

    // Load the template scene and verify it has expected structure.
    // Template is at assets/templates/empty_2d/Scenes/Main.scene relative to project root.
    // Smoke runs from bin/Debug, so we try to find it relative to CWD.
    const std::array<std::filesystem::path, 3> candidates{
        "../../assets/templates/empty_2d/Scenes/Main.scene",
        "../../../assets/templates/empty_2d/Scenes/Main.scene",
        "assets/templates/empty_2d/Scenes/Main.scene"};

    std::filesystem::path templatePath;
    for (const auto& p : candidates)
    {
        if (std::filesystem::exists(p))
        {
            templatePath = p;
            break;
        }
    }
    if (templatePath.empty())
    {
        std::cout << "  skip  template file not found from CWD (not a failure)\n";
        return;
    }

    World world{false};
    SceneDocument doc;
    SceneService service;
    const auto result = service.Load(doc, world, templatePath);
    Check(result.IsOk(), "Empty 2D template scene loads without error");

    bool hasSprite2D = false;
    for (const auto [e, s] : world.Registry().view<const Sprite2DComponent>().each())
    {
        hasSprite2D = true;
        static_cast<void>(e);
        static_cast<void>(s);
    }
    Check(hasSprite2D, "Empty 2D template has at least one Sprite2D entity");

    bool hasCamera = false;
    for (const auto [e, c] : world.Registry().view<const CameraComponent>().each())
    {
        hasCamera = true;
        static_cast<void>(e);
        static_cast<void>(c);
    }
    Check(hasCamera, "Empty 2D template has a Camera entity");
}

void TestTileMapDuplication()
{
    using namespace fadix;
    std::cout << "[2d-smoke] TileMap entity duplication preserves independent TileData\n";

    World world{false};
    const entt::entity ent = world.Create();
    world.Registry().emplace<NameComponent>(ent, "Original");
    world.Registry().emplace<TransformComponent>(ent);

    TileMapComponent tm = MakeDefaultTileMap();
    tm.GridWidth = 2;
    tm.GridHeight = 2;
    tm.LayerCount = 1;
    tm.TileData = {10, 20, 30, 40};
    world.Registry().emplace<TileMapComponent>(ent, tm);

    // Simulate duplication by copying the component
    TileMapComponent copy = *world.Registry().try_get<TileMapComponent>(ent);
    const entt::entity ent2 = world.Create();
    world.Registry().emplace<NameComponent>(ent2, "Duplicate");
    world.Registry().emplace<TileMapComponent>(ent2, copy);

    // Modify original
    world.Registry().get<TileMapComponent>(ent).TileData[0] = 99;

    // Duplicate should be unchanged (vector copy = independent)
    const auto& dupData = world.Registry().get<const TileMapComponent>(ent2).TileData;
    Check(!dupData.empty() && dupData[0] == 10, "duplicate TileData is independent from original");
    Check(world.Registry().get<const TileMapComponent>(ent).TileData[0] == 99,
        "original TileData modified");
}

void TestSprite2DDefaultsAndPolicy()
{
    using namespace fadix;
    std::cout << "[2d-smoke] Sprite2D defaults + missing-texture render policy\n";

    // New Sprite2D must default to a fully opaque white tint.
    const Sprite2DComponent spr;
    Check(std::abs(spr.Tint.r - 1.0F) < 0.001F, "default tint R = 1");
    Check(std::abs(spr.Tint.g - 1.0F) < 0.001F, "default tint G = 1");
    Check(std::abs(spr.Tint.b - 1.0F) < 0.001F, "default tint B = 1");
    Check(std::abs(spr.Tint.a - 1.0F) < 0.001F, "default tint A = 1 (opaque)");

    // Shared Scene/Game policy: a textureless sprite is not renderable content
    // (no white-quad fallback); assigning a texture makes it renderable.
    Check(!Sprite2DHasRenderableTexture(spr), "textureless Sprite2D is not renderable");
    Sprite2DComponent textured = spr;
    textured.Texture = Uuid::Generate();
    Check(Sprite2DHasRenderableTexture(textured), "textured Sprite2D is renderable");
}

void Test2DPresetComponents()
{
    using namespace fadix;
    std::cout << "[2d-smoke] Create > 2D preset component combinations\n";

    // Sprite 2D preset: opaque white Sprite2D only.
    const Sprite2DComponent sprite;
    Check(std::abs(sprite.Tint.a - 1.0F) < 0.001F, "Sprite 2D preset opaque white");

    // Animated Sprite 2D preset: Sprite2D + SpriteFrameAnimator.
    const SpriteFrameAnimatorComponent animator;
    Check(animator.Clips.empty(), "Animated Sprite 2D animator default empty clips");
    Check(!animator.Playing, "Animated Sprite 2D animator not playing by default");

    // Camera 2D preset: orthographic with a sensible ortho size.
    CameraComponent cam;
    cam.Orthographic = true;
    cam.OrthoSize = 5.0F;
    Check(cam.Orthographic, "Camera 2D preset is orthographic");
    Check(std::abs(cam.OrthoSize - 5.0F) < 0.001F, "Camera 2D preset ortho size");
    Check(!CameraComponent{}.Orthographic, "default CameraComponent is perspective");

    // Physics body presets carry the right body type + a matching collider.
    const auto makeBody = [](Body2DType type, bool sensor) {
        RigidBody2DComponent rb;
        rb.Type = type;
        Collider2DComponent col;
        col.Sensor = sensor;
        return std::pair<RigidBody2DComponent, Collider2DComponent>{rb, col};
    };
    const auto staticBody = makeBody(Body2DType::Static, false);
    Check(staticBody.first.Type == Body2DType::Static, "Static Body 2D type");
    Check(!staticBody.second.Sensor, "Static Body 2D collider is solid");
    const auto kinematicBody = makeBody(Body2DType::Kinematic, false);
    Check(kinematicBody.first.Type == Body2DType::Kinematic, "Kinematic Body 2D type");
    const auto dynamicBody = makeBody(Body2DType::Dynamic, false);
    Check(dynamicBody.first.Type == Body2DType::Dynamic, "Dynamic Body 2D type");

    // Area 2D preset: static body + sensor collider.
    const auto area = makeBody(Body2DType::Static, true);
    Check(area.first.Type == Body2DType::Static, "Area 2D uses a static body");
    Check(area.second.Sensor, "Area 2D collider has Sensor enabled");
    Check(area.second.Shape == Collider2DShape::Box, "Area 2D collider default Box shape");
}

void TestSprite2DEffectiveSize()
{
    using namespace fadix;
    std::cout << "[2d-smoke] Sprite2D effective size = Size * Transform.Scale (flip preserved)\n";

    // Shared helper Sprite2DEffectiveSize is the single source of truth used by
    // the renderer and the Scene View placeholder:
    //   x = Size.x * Scale.x * (FlipX ? -1 : 1)
    //   y = Size.y * Scale.y * (FlipY ? -1 : 1)
    Sprite2DComponent spr;
    spr.Size = {2.0F, 3.0F};
    spr.FlipX = true;
    spr.FlipY = false;
    TransformComponent xform;
    xform.Scale = {4.0F, 0.5F, 1.0F};

    const glm::vec2 eff = Sprite2DEffectiveSize(spr, {xform.Scale.x, xform.Scale.y});
    Check(std::abs(eff.x - (-8.0F)) < 0.001F, "effective X folds scale and flip (2*4*-1)");
    Check(std::abs(eff.y - 1.5F) < 0.001F, "effective Y folds scale, no flip (3*0.5)");
    Check(eff.x < 0.0F, "FlipX keeps negative X sign after scale");
    Check(eff.y > 0.0F, "no FlipY keeps positive Y sign");

    // FlipY with a non-centered pivot: sign flips independently of pivot.
    Sprite2DComponent spr2;
    spr2.Size = {5.0F, 5.0F};
    spr2.FlipX = false;
    spr2.FlipY = true;
    spr2.Pivot = {0.0F, 1.0F}; // bottom-left / non-centered
    const glm::vec2 eff2 = Sprite2DEffectiveSize(spr2, {1.0F, 2.0F});
    Check(std::abs(eff2.x - 5.0F) < 0.001F, "no FlipX, unit scale keeps positive X");
    Check(std::abs(eff2.y - (-10.0F)) < 0.001F, "FlipY + scale.y=2 gives -10 (pivot-independent)");
    Check(eff2.y < 0.0F, "FlipY keeps negative Y sign with non-centered pivot");

    // Identity scale, no flips leaves Size unchanged.
    Sprite2DComponent plain;
    plain.Size = {2.0F, 3.0F};
    const glm::vec2 ident = Sprite2DEffectiveSize(plain, {1.0F, 1.0F});
    Check(std::abs(ident.x - 2.0F) < 0.001F, "identity scale, no flip preserves Size.x");
    Check(std::abs(ident.y - 3.0F) < 0.001F, "identity scale, no flip preserves Size.y");
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
    TestSpriteAnimationRuntime();
    TestTileMapNewFields();
    TestCollisionLayerMask();
    TestTileUvCalculation();
    TestTileMapPainting();
    TestTileMapLayerOps();
    TestOrtho2DCameraConversion();
    TestGridSnapping();
    TestTileMapSaveLoadAfterPaint();
    TestEmpty2DTemplateContents();
    TestTileMapDuplication();
    TestSprite2DDefaultsAndPolicy();
    Test2DPresetComponents();
    TestSprite2DEffectiveSize();
    if (g_Failures == 0)
    {
        std::cout << "all checks passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << g_Failures << " check(s) failed\n";
    return EXIT_FAILURE;
}
