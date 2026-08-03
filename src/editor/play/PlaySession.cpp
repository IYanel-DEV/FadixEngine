#include "editor/play/PlaySession.hpp"

#include "editor/scene/PrefabSerializer.hpp"
#include "editor/scene/SceneSerializer.hpp"
#include "engine/app/ModuleRegistration.hpp"
#include "engine/scene/IWorld.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "project/SaveGameService.hpp"
#include "runtime/Components.hpp"

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

namespace fadix
{
PlaySession::PlaySession(const float fixedDeltaSeconds)
    : m_FixedDeltaSeconds(std::max(fixedDeltaSeconds, 0.0001F))
{
}

PlaySession::~PlaySession() = default;

bool PlaySession::Start(const IWorld& authoredWorld, std::unique_ptr<IPhysicsWorld> physics)
{
    if (!physics)
    {
        return false;
    }
    std::unique_ptr<IWorld> runtime = authoredWorld.Clone();
    if (!runtime)
    {
        return false;
    }
    m_RuntimeWorld = std::move(runtime);
    m_Physics = std::move(physics);
    m_Scripts.Start(m_RuntimeWorld->Registry(), m_ScriptResolver);
    // OnStart may move/create/destroy physics entities, so build physics from
    // the post-script state that will actually be rendered.
    m_Physics->SyncFromWorld(*m_RuntimeWorld);
    m_Accumulator = 0.0F;
    m_State = PlayState::Playing;
    return true;
}

void PlaySession::SetScriptContext(
    ScriptRunner::SourceResolver resolver,
    ScriptRunner::Logger logger,
    NativeScriptLoader* nativeLoader)
{
    m_ScriptResolver = std::move(resolver);
    m_Logger = logger;
    m_Scripts.SetLogger(std::move(logger));
    m_Scripts.SetNativeLoader(nativeLoader);
}

void PlaySession::SetGameServices(
    std::function<std::unique_ptr<IPhysicsWorld>()> createPhysics,
    std::function<std::filesystem::path(const std::string&)> resolvePath,
    std::filesystem::path saveDirectory)
{
    m_CreatePhysics = std::move(createPhysics);
    m_ResolvePath = std::move(resolvePath);
    m_SaveGames = std::make_unique<SaveGameService>(std::move(saveDirectory));
    m_Scripts.SetGameCallbacks(
        [this](const std::string& path, float x, float y, float z) {
            return SpawnPrefab(path, x, y, z);
        },
        [this](const std::string& path) { m_PendingSceneLoad = path; },
        [this](const std::string& slot) { return RequestSave(slot); },
        [this](const std::string& slot) { return RequestLoad(slot); });
}

bool PlaySession::RequestSave(const std::string& slot)
{
    if (!m_SaveGames || !SaveGameService::IsValidSlot(slot))
    {
        return false;
    }
    m_PendingSave = slot;
    return true;
}

bool PlaySession::RequestLoad(const std::string& slot)
{
    if (!m_SaveGames || !SaveGameService::IsValidSlot(slot))
    {
        return false;
    }
    m_PendingSaveLoad = slot;
    return true;
}

void PlaySession::PerformSave()
{
    const std::string slot = std::move(m_PendingSave);
    m_PendingSave.clear();
    if (!m_SaveGames || !m_RuntimeWorld)
    {
        return;
    }
    const Result<std::filesystem::path> saved = m_SaveGames->Save(slot, *m_RuntimeWorld);
    if (m_Logger)
    {
        m_Logger(saved ? "Saved game to slot '" + slot + "'"
                       : "Save.write('" + slot + "'): " + saved.ErrorMessage(),
            saved ? "info" : "error");
    }
}

void PlaySession::PerformSaveLoad()
{
    const std::string slot = std::move(m_PendingSaveLoad);
    m_PendingSaveLoad.clear();
    if (!m_SaveGames || !m_RuntimeWorld || !m_CreatePhysics)
    {
        return;
    }
    std::unique_ptr<IWorld> next = sceneplay::CreateEditWorld();
    if (const Result<void> loaded = m_SaveGames->Load(slot, *next); !loaded)
    {
        if (m_Logger)
        {
            m_Logger("Save.load('" + slot + "'): " + loaded.ErrorMessage(), "error");
        }
        return;
    }
    std::unique_ptr<IPhysicsWorld> physics = m_CreatePhysics();
    if (!physics)
    {
        if (m_Logger)
        {
            m_Logger("Save.load('" + slot + "'): physics world unavailable", "error");
        }
        return;
    }
    m_Scripts.Stop(m_RuntimeWorld->Registry());
    m_RuntimeWorld = std::move(next);
    m_Physics = std::move(physics);
    m_Scripts.Start(m_RuntimeWorld->Registry(), m_ScriptResolver);
    m_Physics->SyncFromWorld(*m_RuntimeWorld);
    m_PendingSceneLoad.clear();
    m_Accumulator = 0.0F;
    if (m_Logger)
    {
        m_Logger("Loaded game from slot '" + slot + "'", "info");
    }
}

std::optional<entt::entity> PlaySession::SpawnPrefab(
    const std::string& path, const float x, const float y, const float z)
{
    if (!m_RuntimeWorld || !m_ResolvePath)
    {
        return std::nullopt;
    }
    const Result<Uuid> root =
        PrefabSerializer::Instantiate(*m_RuntimeWorld, m_ResolvePath(path), std::nullopt);
    if (!root)
    {
        if (m_Logger)
        {
            m_Logger("Prefab.spawn('" + path + "'): " + root.ErrorMessage(), "error");
        }
        return std::nullopt;
    }
    const std::optional<entt::entity> entity = m_RuntimeWorld->Find(root.Value());
    if (!entity)
    {
        return std::nullopt;
    }
    if (auto* transform = m_RuntimeWorld->Registry().try_get<TransformComponent>(*entity))
    {
        transform->Position = {x, y, z};
    }
    // Bring the prefab's own scripts to life; Start already ran for the scene.
    m_Scripts.StartEntity(m_RuntimeWorld->Registry(), *entity, m_ScriptResolver);
    return entity;
}

namespace
{
// Scene.load argument that is not a file path: match it against scene display names
// (root entity name, case-insensitive) and file stems under Scenes/. Returns all
// matches so the caller can report "no match" vs "ambiguous" distinctly.
std::vector<std::filesystem::path> ScenesMatchingDisplayName(
    const std::filesystem::path& scenesDir, const std::string& query)
{
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string want = lower(query);
    std::vector<std::filesystem::path> matches;
    std::error_code ec;
    if (!std::filesystem::exists(scenesDir, ec))
    {
        return matches;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator{scenesDir, ec})
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".scene")
        {
            continue;
        }
        if (lower(entry.path().stem().string()) == want)
        {
            matches.push_back(entry.path());
            continue;
        }
        if (const auto display = SceneSerializer::PeekDisplayName(entry.path());
            display && lower(*display) == want)
        {
            matches.push_back(entry.path());
        }
    }
    return matches;
}
}

void PlaySession::PerformSceneLoad()
{
    const std::string relative = std::move(m_PendingSceneLoad);
    m_PendingSceneLoad.clear();
    if (!m_ResolvePath || !m_CreatePhysics)
    {
        if (m_Logger)
        {
            m_Logger("Scene.load('" + relative + "'): game services not wired", "error");
        }
        return;
    }
    std::filesystem::path absolute = m_ResolvePath(relative);
    // Not a path? Resolve against scene display names / file stems under Scenes/.
    std::error_code exists;
    if (!std::filesystem::exists(absolute, exists))
    {
        const auto matches = ScenesMatchingDisplayName(m_ResolvePath("Scenes"), relative);
        if (matches.size() == 1)
        {
            absolute = matches.front();
        }
        else if (m_Logger)
        {
            m_Logger("Scene.load('" + relative + "'): " +
                    (matches.empty() ? "no scene file or display name matches"
                                     : "ambiguous display name (" +
                            std::to_string(matches.size()) + " scenes match)"),
                "error");
            return;
        }
        else
        {
            return;
        }
    }
    std::unique_ptr<IWorld> next = sceneplay::CreateEditWorld();
    SceneDocument document{Uuid::Generate(), absolute.stem().string(), absolute, false};
    SceneService scenes;
    if (const Result<void> loaded = scenes.Load(document, *next, absolute); !loaded)
    {
        if (m_Logger)
        {
            m_Logger("Scene.load('" + relative + "'): " + loaded.ErrorMessage(), "error");
        }
        return; // keep the current world running
    }
    m_Scripts.Stop(m_RuntimeWorld->Registry()); // OnDestroy on the outgoing scene
    m_RuntimeWorld = std::move(next);
    m_Physics = m_CreatePhysics();
    m_Scripts.Start(m_RuntimeWorld->Registry(), m_ScriptResolver);
    m_Physics->SyncFromWorld(*m_RuntimeWorld);
    m_Accumulator = 0.0F;
}

void PlaySession::BindAudio(AudioEngine* engine)
{
    m_Scripts.BindAudio(engine);
}

void PlaySession::Stop() noexcept
{
    if (m_RuntimeWorld)
    {
        m_Scripts.Stop(m_RuntimeWorld->Registry());
    }
    m_Physics.reset();
    m_RuntimeWorld.reset();
    m_Accumulator = 0.0F;
    m_PendingSceneLoad.clear();
    m_PendingSave.clear();
    m_PendingSaveLoad.clear();
    m_State = PlayState::Stopped;
}

void PlaySession::Pause() noexcept
{
    if (m_State == PlayState::Playing)
    {
        m_State = PlayState::Paused;
    }
}

void PlaySession::Resume() noexcept
{
    if (m_State == PlayState::Paused)
    {
        m_State = PlayState::Playing;
    }
}

void PlaySession::SingleStep()
{
    if (m_State == PlayState::Paused)
    {
        Tick();
    }
}

void PlaySession::Update(const float deltaSeconds)
{
    if (m_State != PlayState::Playing || deltaSeconds <= 0.0F)
    {
        return;
    }
    m_Accumulator = std::min(m_Accumulator + deltaSeconds, m_FixedDeltaSeconds * 4.0F);
    while (m_Accumulator >= m_FixedDeltaSeconds)
    {
        Tick();
        m_Accumulator -= m_FixedDeltaSeconds;
    }
}

void PlaySession::Tick()
{
    if (m_RuntimeWorld && m_Physics)
    {
        // Push gameplay transforms into physics before stepping. Syncing in the
        // opposite order erases scripted movement on every dynamic body.
        m_Scripts.Update(m_RuntimeWorld->Registry(), m_FixedDeltaSeconds);
        if (!m_PendingSave.empty())
        {
            PerformSave();
        }
        if (!m_PendingSaveLoad.empty())
        {
            PerformSaveLoad();
            return;
        }
        // A script may have requested a level transition; swap worlds before
        // stepping physics we are about to discard.
        if (!m_PendingSceneLoad.empty())
        {
            PerformSceneLoad();
            return;
        }
        m_Physics->SyncFromWorld(*m_RuntimeWorld);
        m_Physics->StepFixed(m_FixedDeltaSeconds);
        m_Physics->SyncToWorld(*m_RuntimeWorld);
    }
}

PlayState PlaySession::State() const noexcept { return m_State; }
IWorld* PlaySession::RuntimeWorld() noexcept { return m_RuntimeWorld.get(); }
const IWorld* PlaySession::RuntimeWorld() const noexcept { return m_RuntimeWorld.get(); }
}
