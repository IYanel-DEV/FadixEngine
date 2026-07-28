#include "scripting/LuaVM.hpp"

#ifdef FADIX_ENABLE_LUA

#include "engine/audio/AudioEngine.hpp"
#include "runtime/Components.hpp"
#include "scripting/FxsApiNames.hpp"

#include <SDL3/SDL_keyboard.h>
#include <sol/sol.hpp>

#include <entt/entity/registry.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fadix
{
namespace
{
// The value handed to Lua as the `entity` argument. Non-owning and rebuilt for
// every lifecycle call, so it never outlives the world it points into.
struct LuaEntity
{
    entt::registry* reg{nullptr};
    entt::entity e{entt::null};
    std::vector<entt::entity>* pendingDestroy{nullptr};

    [[nodiscard]] TransformComponent* transform() const
    {
        return reg ? reg->try_get<TransformComponent>(e) : nullptr;
    }

    [[nodiscard]] std::uint32_t id() const { return static_cast<std::uint32_t>(e); }

    [[nodiscard]] std::string getName() const
    {
        if (const auto* n = reg ? reg->try_get<NameComponent>(e) : nullptr)
        {
            return n->Name;
        }
        return {};
    }

    [[nodiscard]] std::tuple<float, float, float> getPosition() const
    {
        if (const auto* t = transform())
        {
            return {t->Position.x, t->Position.y, t->Position.z};
        }
        return {0.0F, 0.0F, 0.0F};
    }
    void setPosition(float x, float y, float z)
    {
        if (auto* t = transform())
        {
            t->Position = {x, y, z};
        }
    }

    // Rotation is exposed as euler degrees. The quaternion<->euler round-trip is
    // not bit-stable, which is fine for gameplay scripting.
    [[nodiscard]] std::tuple<float, float, float> getRotation() const
    {
        if (const auto* t = transform())
        {
            const glm::vec3 euler = glm::degrees(glm::eulerAngles(t->Rotation));
            return {euler.x, euler.y, euler.z};
        }
        return {0.0F, 0.0F, 0.0F};
    }
    void setRotation(float x, float y, float z)
    {
        if (auto* t = transform())
        {
            t->Rotation = glm::quat(glm::radians(glm::vec3{x, y, z}));
        }
    }

    [[nodiscard]] std::tuple<float, float, float> getScale() const
    {
        if (const auto* t = transform())
        {
            return {t->Scale.x, t->Scale.y, t->Scale.z};
        }
        return {1.0F, 1.0F, 1.0F};
    }
    void setScale(float x, float y, float z)
    {
        if (auto* t = transform())
        {
            t->Scale = {x, y, z};
        }
    }

    void moveCharacter(float x, float z)
    {
        if (auto* character = reg ? reg->try_get<CharacterControllerComponent>(e) : nullptr)
        {
            character->MoveInput = {x, z};
        }
    }

    void jumpCharacter()
    {
        if (auto* character = reg ? reg->try_get<CharacterControllerComponent>(e) : nullptr)
        {
            character->JumpRequested = true;
        }
    }

    [[nodiscard]] bool isCharacterGrounded() const
    {
        const auto* character = reg ? reg->try_get<CharacterControllerComponent>(e) : nullptr;
        return character != nullptr && character->Grounded;
    }

    void destroy()
    {
        if (pendingDestroy)
        {
            pendingDestroy->push_back(e);
        }
    }

    [[nodiscard]] sol::object getTarget(sol::this_state state) const
    {
        if (reg == nullptr)
        {
            return sol::make_object(state, sol::lua_nil);
        }
        const ScriptComponent* scripts = reg->try_get<ScriptComponent>(e);
        if (scripts == nullptr || !scripts->Target.IsValid())
        {
            return sol::make_object(state, sol::lua_nil);
        }
        for (const auto [other, uuid] : reg->view<UuidComponent>().each())
        {
            if (uuid.Id == scripts->Target)
            {
                return sol::make_object(state, LuaEntity{reg, other, pendingDestroy});
            }
        }
        return sol::make_object(state, sol::lua_nil);
    }
};

// Physical scancodes via SDL so WASD stays layout-stable. Returns false when
// SDL is not initialized (e.g. lua smoke targets).
bool InputIsDown(std::string key)
{
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const SDL_Scancode scancode = SDL_GetScancodeFromName(key.c_str());
    if (scancode == SDL_SCANCODE_UNKNOWN)
    {
        return false;
    }
    int count = 0;
    const bool* state = SDL_GetKeyboardState(&count);
    if (state == nullptr || static_cast<int>(scancode) >= count)
    {
        return false;
    }
    return state[scancode];
}
}

class LuaVMImpl
{
public:
    LuaVMImpl() { Setup(); }

    void Setup()
    {
        m_Lua = sol::state{};
        m_Lua.open_libraries(
            sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::os);

        m_Lua.new_usertype<LuaEntity>(fxs::kEntityType, sol::no_constructor,
            fxs::kEntityId, sol::property([](const LuaEntity& self) { return self.id(); }),
            fxs::kEntityGetName, &LuaEntity::getName,
            fxs::kEntityGetPosition, &LuaEntity::getPosition,
            fxs::kEntitySetPosition, &LuaEntity::setPosition,
            fxs::kEntityGetRotation, &LuaEntity::getRotation,
            fxs::kEntitySetRotation, &LuaEntity::setRotation,
            fxs::kEntityGetScale, &LuaEntity::getScale,
            fxs::kEntitySetScale, &LuaEntity::setScale,
            fxs::kEntityMoveCharacter, &LuaEntity::moveCharacter,
            fxs::kEntityJumpCharacter, &LuaEntity::jumpCharacter,
            fxs::kEntityIsCharacterGrounded, &LuaEntity::isCharacterGrounded,
            fxs::kEntityDestroy, &LuaEntity::destroy,
            fxs::kEntityGetTarget, &LuaEntity::getTarget);

        // Route print() to the editor's Output panel instead of stdout.
        m_Lua.set_function(fxs::kPrint, [this](sol::variadic_args args) {
            std::string line;
            sol::function tostring = m_Lua["tostring"];
            bool first = true;
            for (auto argument : args)
            {
                if (!first)
                {
                    line += '\t';
                }
                first = false;
                line += tostring(argument).get<std::string>();
            }
            if (m_Logger)
            {
                m_Logger(line, "info");
            }
        });

        sol::table input = m_Lua.create_table();
        input[fxs::kInputIsDown] = &InputIsDown;
        m_Lua[fxs::kInput] = input;

        BindAudioTable();
        BindGameTables();
    }

    void SetWorldApi(ScriptWorldApi* api)
    {
        m_WorldApi = api;
        BindGameTables();
    }

    // World.find / Prefab.spawn / Scene.load. Bound once per Lua state; the
    // closures read m_WorldApi live, so refreshing the context needs no rebind.
    void BindGameTables()
    {
        sol::table world = m_Lua.create_table();
        world[fxs::kWorldFind] =
            [this](const std::string& name, sol::this_state state) -> sol::object {
            if (m_WorldApi == nullptr || m_WorldApi->Registry == nullptr)
            {
                return sol::make_object(state, sol::lua_nil);
            }
            for (const auto [entity, component] : m_WorldApi->Registry->view<NameComponent>().each())
            {
                if (component.Name == name)
                {
                    return sol::make_object(state,
                        LuaEntity{m_WorldApi->Registry, entity, m_WorldApi->PendingDestroy});
                }
            }
            return sol::make_object(state, sol::lua_nil);
        };
        m_Lua[fxs::kWorld] = world;

        sol::table prefab = m_Lua.create_table();
        prefab[fxs::kPrefabSpawn] = [this](const std::string& path, float x, float y, float z,
                                        sol::this_state state) -> sol::object {
            if (m_WorldApi == nullptr || !m_WorldApi->SpawnPrefab)
            {
                return sol::make_object(state, sol::lua_nil);
            }
            const std::optional<entt::entity> entity = m_WorldApi->SpawnPrefab(path, x, y, z);
            if (!entity)
            {
                return sol::make_object(state, sol::lua_nil);
            }
            return sol::make_object(state,
                LuaEntity{m_WorldApi->Registry, *entity, m_WorldApi->PendingDestroy});
        };
        m_Lua[fxs::kPrefab] = prefab;

        sol::table scene = m_Lua.create_table();
        scene[fxs::kSceneLoad] = [this](const std::string& path) {
            if (m_WorldApi != nullptr && m_WorldApi->LoadScene)
            {
                m_WorldApi->LoadScene(path);
            }
            else
            {
                Fail("Scene.load unavailable: no active play session");
            }
        };
        m_Lua[fxs::kScene] = scene;
    }

    void BindAudio(AudioEngine* engine)
    {
        m_AudioEngine = engine;
        BindAudioTable();
    }

    void BindAudioTable()
    {
        if (m_AudioEngine == nullptr)
        {
            m_Lua[fxs::kAudio] = sol::lua_nil;
            return;
        }
        AudioEngine* engine = m_AudioEngine;
        sol::table audio = m_Lua.create_table();
        audio[fxs::kAudioLoad] = [engine](const std::string& id, const std::string& path) {
            return engine->Load(id, path);
        };
        audio[fxs::kAudioPlay] = [engine](const std::string& id, sol::optional<float> volume) {
            return engine->Play(id, 0, volume.value_or(1.0F));
        };
        audio[fxs::kAudioStop] = [engine](const std::string& id) { engine->StopById(id); };
        audio[fxs::kAudioSetMasterVolume] = [engine](float volume) { engine->SetMasterVolume(volume); };
        audio[fxs::kAudioSetSoundVolume] = [engine](float volume) { engine->SetSoundVolume(volume); };
        audio[fxs::kAudioSetMusicVolume] = [engine](float volume) { engine->SetMusicVolume(volume); };
        m_Lua[fxs::kAudio] = audio;
    }

    bool Compile(const std::string& name, const std::string& source)
    {
        sol::load_result loaded = m_Lua.load(source, "@" + name);
        if (!loaded.valid())
        {
            const sol::error error = loaded;
            Fail(error.what());
            return false;
        }
        m_Sources[name] = source;
        return true;
    }

    int Instantiate(const std::string& name)
    {
        const auto source = m_Sources.find(name);
        if (source == m_Sources.end())
        {
            Fail("script not compiled: " + name);
            return -1;
        }
        // Each instance runs in its own environment whose fallback is the shared
        // globals, so two entities running the same script keep separate state.
        sol::environment environment(m_Lua, sol::create, m_Lua.globals());
        sol::load_result loaded = m_Lua.load(source->second, "@" + name);
        if (!loaded.valid())
        {
            const sol::error error = loaded;
            Fail(error.what());
            return -1;
        }
        sol::protected_function chunk = loaded;
        sol::set_environment(environment, chunk);
        const sol::protected_function_result result = chunk();
        if (!result.valid())
        {
            const sol::error error = result;
            Fail(error.what());
            return -1;
        }
        const int instance = static_cast<int>(m_Instances.size());
        m_Instances.emplace_back(std::move(environment));
        return instance;
    }

    void Call(int instance, const char* function, const ScriptEntityHandle& handle,
        const float* deltaTime)
    {
        if (instance < 0 || instance >= static_cast<int>(m_Instances.size())
            || !m_Instances[static_cast<std::size_t>(instance)])
        {
            return;
        }
        sol::environment& environment = *m_Instances[static_cast<std::size_t>(instance)];
        sol::protected_function callback = environment[function];
        if (!callback.valid())
        {
            return;
        }
        const LuaEntity entity{handle.Registry, handle.Entity, handle.PendingDestroy};
        const sol::protected_function_result result =
            deltaTime ? callback(entity, *deltaTime) : callback(entity);
        if (!result.valid())
        {
            const sol::error error = result;
            Fail(error.what());
        }
    }

    void DestroyInstance(int instance)
    {
        if (instance >= 0 && instance < static_cast<int>(m_Instances.size()))
        {
            m_Instances[static_cast<std::size_t>(instance)].reset();
        }
    }

    void Reset()
    {
        m_Instances.clear();
        m_Sources.clear();
        Setup();
    }

    void SetLogger(LuaVM::LogFn logger) { m_Logger = std::move(logger); }
    [[nodiscard]] bool HasError() const { return m_HasError; }
    [[nodiscard]] const std::string& LastError() const { return m_LastError; }

private:
    void Fail(std::string message)
    {
        m_LastError = std::move(message);
        m_HasError = true;
        if (m_Logger)
        {
            m_Logger(m_LastError, "error");
        }
    }

    sol::state m_Lua;
    LuaVM::LogFn m_Logger;
    AudioEngine* m_AudioEngine{nullptr};
    ScriptWorldApi* m_WorldApi{nullptr};
    std::unordered_map<std::string, std::string> m_Sources;
    std::vector<std::optional<sol::environment>> m_Instances;
    std::string m_LastError;
    bool m_HasError{false};
};

LuaVM::LuaVM() : m_Impl(std::make_unique<LuaVMImpl>()) {}
LuaVM::~LuaVM() = default;
LuaVM::LuaVM(LuaVM&&) noexcept = default;
LuaVM& LuaVM::operator=(LuaVM&&) noexcept = default;

void LuaVM::SetLogger(LogFn logger) { m_Impl->SetLogger(std::move(logger)); }
void LuaVM::BindAudio(AudioEngine* engine) { m_Impl->BindAudio(engine); }
void LuaVM::SetWorldApi(ScriptWorldApi* api) { m_Impl->SetWorldApi(api); }
bool LuaVM::Compile(const std::string& name, const std::string& source)
{
    return m_Impl->Compile(name, source);
}
int LuaVM::Instantiate(const std::string& name) { return m_Impl->Instantiate(name); }
void LuaVM::CallStart(int instance, const ScriptEntityHandle& entity)
{
    m_Impl->Call(instance, fxs::kOnStart, entity, nullptr);
}
void LuaVM::CallUpdate(int instance, const ScriptEntityHandle& entity, float deltaTime)
{
    m_Impl->Call(instance, fxs::kOnUpdate, entity, &deltaTime);
}
void LuaVM::CallDestroy(int instance, const ScriptEntityHandle& entity)
{
    m_Impl->Call(instance, fxs::kOnDestroy, entity, nullptr);
}
void LuaVM::DestroyInstance(int instance) { m_Impl->DestroyInstance(instance); }
void LuaVM::Reset() { m_Impl->Reset(); }
bool LuaVM::HasError() const { return m_Impl->HasError(); }
const std::string& LuaVM::LastError() const { return m_Impl->LastError(); }
}

#else // FADIX_ENABLE_LUA

namespace fadix
{
class LuaVMImpl
{
};

LuaVM::LuaVM() = default;
LuaVM::~LuaVM() = default;
LuaVM::LuaVM(LuaVM&&) noexcept = default;
LuaVM& LuaVM::operator=(LuaVM&&) noexcept = default;

void LuaVM::SetLogger(LogFn) {}
void LuaVM::BindAudio(AudioEngine*) {}
void LuaVM::SetWorldApi(ScriptWorldApi*) {}
bool LuaVM::Compile(const std::string&, const std::string&) { return false; }
int LuaVM::Instantiate(const std::string&) { return -1; }
void LuaVM::CallStart(int, const ScriptEntityHandle&) {}
void LuaVM::CallUpdate(int, const ScriptEntityHandle&, float) {}
void LuaVM::CallDestroy(int, const ScriptEntityHandle&) {}
void LuaVM::DestroyInstance(int) {}
void LuaVM::Reset() {}
bool LuaVM::HasError() const { return false; }
const std::string& LuaVM::LastError() const
{
    static const std::string empty;
    return empty;
}
}

#endif // FADIX_ENABLE_LUA
