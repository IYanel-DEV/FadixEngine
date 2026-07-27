#include "editor/play/PlaySession.hpp"

#include "engine/scene/IWorld.hpp"

#include <algorithm>
#include <utility>

namespace fadix
{
PlaySession::PlaySession(const float fixedDeltaSeconds)
    : m_FixedDeltaSeconds(std::max(fixedDeltaSeconds, 0.0001F))
{
}

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
    m_Scripts.SetLogger(std::move(logger));
    m_Scripts.SetNativeLoader(nativeLoader);
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
        m_Physics->SyncFromWorld(*m_RuntimeWorld);
        m_Physics->StepFixed(m_FixedDeltaSeconds);
        m_Physics->SyncToWorld(*m_RuntimeWorld);
    }
}

PlayState PlaySession::State() const noexcept { return m_State; }
IWorld* PlaySession::RuntimeWorld() noexcept { return m_RuntimeWorld.get(); }
const IWorld* PlaySession::RuntimeWorld() const noexcept { return m_RuntimeWorld.get(); }
}
