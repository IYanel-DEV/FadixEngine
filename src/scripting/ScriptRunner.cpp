#include "scripting/ScriptRunner.hpp"

#include "runtime/Components.hpp"
#include "scripting/NativeScript.hpp"

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <utility>

namespace fadix
{
ScriptRunner::ScriptRunner() = default;
ScriptRunner::~ScriptRunner() = default;
ScriptRunner::ScriptRunner(ScriptRunner&&) noexcept = default;
ScriptRunner& ScriptRunner::operator=(ScriptRunner&&) noexcept = default;

void ScriptRunner::SetLogger(Logger logger)
{
    m_Logger = std::move(logger);
    m_Vm.SetLogger([this](const std::string& message, const char* severity) {
        if (m_Logger)
        {
            m_Logger(message, severity);
        }
    });
}

void ScriptRunner::BindAudio(AudioEngine* engine)
{
    m_Vm.BindAudio(engine);
}

void ScriptRunner::SetGameCallbacks(
    std::function<std::optional<entt::entity>(const std::string&, float, float, float)> spawnPrefab,
    std::function<void(const std::string&)> loadScene)
{
    m_WorldApi.SpawnPrefab = std::move(spawnPrefab);
    m_WorldApi.LoadScene = std::move(loadScene);
}

void ScriptRunner::Start(entt::registry& registry, const SourceResolver& resolver)
{
    m_Vm.Reset();
    m_Vm.SetWorldApi(&m_WorldApi); // Reset rebuilt the Lua state; rebind the globals.
    m_WorldApi.Registry = &registry;
    m_WorldApi.PendingDestroy = &m_PendingDestroy;
    m_Instances.clear();
    m_NativeInstances.clear();
    m_PendingDestroy.clear();
    m_Started = true;
    if (!resolver)
    {
        return;
    }

    for (const entt::entity entity : registry.view<ScriptComponent>())
    {
        StartEntity(registry, entity, resolver);
    }
    ApplyPendingDestroy(registry);
}

void ScriptRunner::StartEntity(
    entt::registry& registry, const entt::entity entity, const SourceResolver& resolver)
{
    if (!resolver || !registry.valid(entity))
    {
        return;
    }
    const ScriptComponent* component = registry.try_get<ScriptComponent>(entity);
    if (component == nullptr || !component->Enabled)
    {
        return;
    }
    m_WorldApi.Registry = &registry;
    m_WorldApi.PendingDestroy = &m_PendingDestroy;
    for (const std::string& name : component->ScriptNames)
    {
        const std::optional<ResolvedScript> resolved = resolver(name);
        if (!resolved)
        {
            continue;
        }
        if (resolved->Language == ScriptLanguage::Cpp)
        {
            if (m_NativeLoader == nullptr)
            {
                continue; // native path disabled
            }
            NativeScriptLoader::Loaded loaded =
                m_NativeLoader->CompileAndLoad(resolved->SourcePath);
            if (loaded.Instance == nullptr)
            {
                continue; // loader already logged the reason
            }
            ScriptEntity handle{ScriptEntityHandle{&registry, entity, &m_PendingDestroy}};
            loaded.Instance->OnStart(handle);
            m_NativeInstances.push_back(NativeInstance{entity, loaded});
            continue;
        }
        if (!m_Vm.Compile(name, resolved->Source))
        {
            if (m_Logger)
            {
                m_Logger("Script '" + name + "': " + m_Vm.LastError(), "error");
            }
            continue;
        }
        const int instance = m_Vm.Instantiate(name);
        if (instance < 0)
        {
            if (m_Logger)
            {
                m_Logger("Script '" + name + "': " + m_Vm.LastError(), "error");
            }
            continue;
        }
        const ScriptEntityHandle handle{&registry, entity, &m_PendingDestroy};
        m_Vm.CallStart(instance, handle);
        m_Instances.push_back(Instance{entity, instance});
    }
}

void ScriptRunner::Update(entt::registry& registry, const float deltaTime)
{
    if (!m_Started)
    {
        return;
    }
    m_WorldApi.Registry = &registry;
    m_WorldApi.PendingDestroy = &m_PendingDestroy;
    // Index over a snapshot count with copies: an OnUpdate may Prefab.spawn, which
    // StartEntity's push_back can reallocate m_Instances. New instances already
    // got OnStart and are picked up next frame; skip them this frame.
    const std::size_t liveCount = m_Instances.size();
    for (std::size_t i = 0; i < liveCount; ++i)
    {
        const Instance instance = m_Instances[i];
        if (!registry.valid(instance.Entity))
        {
            continue;
        }
        const ScriptEntityHandle handle{&registry, instance.Entity, &m_PendingDestroy};
        m_Vm.CallUpdate(instance.Id, handle, deltaTime);
    }
    const std::size_t liveNative = m_NativeInstances.size();
    for (std::size_t i = 0; i < liveNative; ++i)
    {
        const NativeInstance instance = m_NativeInstances[i];
        if (!registry.valid(instance.Entity) || instance.Loaded.Instance == nullptr)
        {
            continue;
        }
        ScriptEntity handle{ScriptEntityHandle{&registry, instance.Entity, &m_PendingDestroy}};
        instance.Loaded.Instance->OnUpdate(handle, deltaTime);
    }
    ApplyPendingDestroy(registry);
}

void ScriptRunner::Stop(entt::registry& registry)
{
    for (const Instance& instance : m_Instances)
    {
        if (registry.valid(instance.Entity))
        {
            const ScriptEntityHandle handle{&registry, instance.Entity, &m_PendingDestroy};
            m_Vm.CallDestroy(instance.Id, handle);
        }
        m_Vm.DestroyInstance(instance.Id);
    }
    m_Instances.clear();
    for (NativeInstance& instance : m_NativeInstances)
    {
        if (registry.valid(instance.Entity) && instance.Loaded.Instance != nullptr)
        {
            ScriptEntity handle{ScriptEntityHandle{&registry, instance.Entity, &m_PendingDestroy}};
            instance.Loaded.Instance->OnDestroy(handle);
        }
        if (m_NativeLoader != nullptr)
        {
            m_NativeLoader->Unload(instance.Loaded);
        }
    }
    m_NativeInstances.clear();
    ApplyPendingDestroy(registry);
    m_PendingDestroy.clear();
    m_Vm.Reset();
    m_Started = false;
}

void ScriptRunner::ApplyPendingDestroy(entt::registry& registry)
{
    if (m_PendingDestroy.empty())
    {
        return;
    }
    // Snapshot: destroying could, in principle, be requested again mid-loop.
    const std::vector<entt::entity> targets = std::move(m_PendingDestroy);
    m_PendingDestroy.clear();
    for (const entt::entity target : targets)
    {
        for (const Instance& instance : m_Instances)
        {
            if (instance.Entity == target)
            {
                m_Vm.DestroyInstance(instance.Id);
            }
        }
        m_Instances.erase(
            std::remove_if(m_Instances.begin(), m_Instances.end(),
                [target](const Instance& instance) { return instance.Entity == target; }),
            m_Instances.end());
        for (NativeInstance& instance : m_NativeInstances)
        {
            if (instance.Entity == target && m_NativeLoader != nullptr)
            {
                m_NativeLoader->Unload(instance.Loaded);
            }
        }
        m_NativeInstances.erase(
            std::remove_if(m_NativeInstances.begin(), m_NativeInstances.end(),
                [target](const NativeInstance& instance) { return instance.Entity == target; }),
            m_NativeInstances.end());
        if (registry.valid(target))
        {
            registry.destroy(target);
        }
    }
}
}
