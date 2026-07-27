#include "engine/audio/AudioPlayback.hpp"

#include "runtime/Components.hpp"

#include <entt/entity/registry.hpp>

#include <filesystem>
#include <string>

namespace fadix
{
namespace
{
[[nodiscard]] std::string ClipIdFor(const AssetHandle& handle)
{
    return handle.IsValid() ? handle.ToString() : std::string{};
}

[[nodiscard]] glm::vec3 ListenerPosition(const IWorld& world)
{
    for (const auto [entity, transform, listener] :
         world.Registry()
             .view<const TransformComponent, const AudioListenerComponent>()
             .each())
    {
        static_cast<void>(entity);
        if (listener.Active)
        {
            return transform.Position;
        }
    }
    for (const auto [entity, transform, camera] :
         world.Registry().view<const TransformComponent, const CameraComponent>().each())
    {
        static_cast<void>(entity);
        if (camera.Primary)
        {
            return transform.Position;
        }
    }
    return glm::vec3{0.0F};
}
}

AudioPlayback::AudioPlayback(AudioEngine& engine) : m_Engine(engine) {}

void AudioPlayback::Start(IWorld& world, IAssetDatabase* assets)
{
    Stop();
    if (!m_Engine.IsInitialized())
    {
        return;
    }
    m_Active = true;

    for (const auto [entity, source] : world.Registry().view<AudioSourceComponent>().each())
    {
        static_cast<void>(entity);
        source.ActiveTrack = -1;
        if (!source.Sound.IsValid())
        {
            continue;
        }
        const std::string id = ClipIdFor(source.Sound);
        if (assets != nullptr)
        {
            if (const AssetMetadata* meta = assets->Meta(source.Sound))
            {
                const std::filesystem::path& path =
                    !meta->SourcePath.empty() ? meta->SourcePath : meta->ImportedPath;
                if (!path.empty())
                {
                    m_Engine.Load(id, path.string());
                }
            }
        }
        if (!source.PlayOnStart)
        {
            continue;
        }
        const int loops = source.Loop ? -1 : 0;
        source.ActiveTrack = m_Engine.Play(id, loops, source.Volume);
    }
}

void AudioPlayback::Update(IWorld& world, const float /*dt*/)
{
    if (!m_Active || !m_Engine.IsInitialized())
    {
        return;
    }
    const glm::vec3 listener = ListenerPosition(world);
    for (const auto [entity, transform, source] :
         world.Registry().view<const TransformComponent, AudioSourceComponent>().each())
    {
        static_cast<void>(entity);
        if (source.ActiveTrack < 0)
        {
            continue;
        }
        if (!m_Engine.IsPlaying(source.ActiveTrack))
        {
            source.ActiveTrack = -1;
            continue;
        }
        if (source.Spatial)
        {
            m_Engine.SetTrackWorldOffset(source.ActiveTrack, transform.Position - listener);
        }
        else
        {
            m_Engine.ClearTrackSpatial(source.ActiveTrack);
        }
    }
}

void AudioPlayback::Stop()
{
    if (!m_Active)
    {
        return;
    }
    m_Engine.StopAll();
    m_Engine.UnloadAll();
    m_Active = false;
}

} // namespace fadix
