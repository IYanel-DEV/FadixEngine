#include "engine/audio/AudioEngine.hpp"

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace fadix
{
namespace
{
[[nodiscard]] float Clamp01(const float value)
{
    return std::clamp(value, 0.0F, 1.0F);
}
}

struct AudioEngine::Impl
{
    MIX_Mixer* Mixer{nullptr};
    std::unordered_map<std::string, MIX_Audio*> Clips;
    struct TrackSlot
    {
        MIX_Track* Track{nullptr};
        std::string Id;
        float Volume{1.0F};
        bool InUse{false};
    };
    std::vector<TrackSlot> Tracks;
    float MasterVolume{1.0F};
    float SoundVolume{1.0F};
    float MusicVolume{1.0F};
    bool Initialized{false};

    void ApplyMixerGain()
    {
        if (Mixer != nullptr)
        {
            MIX_SetMixerGain(Mixer, MasterVolume);
        }
    }

    void ApplyTrackGain(TrackSlot& slot)
    {
        if (slot.Track == nullptr)
        {
            return;
        }
        MIX_SetTrackGain(slot.Track, SoundVolume * Clamp01(slot.Volume));
    }
};

AudioEngine::AudioEngine() : m_Impl(std::make_unique<Impl>()) {}

AudioEngine::~AudioEngine()
{
    Shutdown();
}

bool AudioEngine::Initialize()
{
    if (m_Impl->Initialized)
    {
        return true;
    }
    if (!MIX_Init())
    {
        return false;
    }
    m_Impl->Mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (m_Impl->Mixer == nullptr)
    {
        MIX_Quit();
        return false;
    }
    m_Impl->Tracks.resize(32);
    for (Impl::TrackSlot& slot : m_Impl->Tracks)
    {
        slot.Track = MIX_CreateTrack(m_Impl->Mixer);
        if (slot.Track == nullptr)
        {
            Shutdown();
            return false;
        }
    }
    m_Impl->ApplyMixerGain();
    m_Impl->Initialized = true;
    return true;
}

void AudioEngine::Shutdown()
{
    if (!m_Impl->Initialized && m_Impl->Mixer == nullptr)
    {
        return;
    }
    UnloadAll();
    for (Impl::TrackSlot& slot : m_Impl->Tracks)
    {
        if (slot.Track != nullptr)
        {
            MIX_DestroyTrack(slot.Track);
            slot.Track = nullptr;
        }
        slot.InUse = false;
    }
    m_Impl->Tracks.clear();
    if (m_Impl->Mixer != nullptr)
    {
        MIX_DestroyMixer(m_Impl->Mixer);
        m_Impl->Mixer = nullptr;
    }
    MIX_Quit();
    m_Impl->Initialized = false;
}

bool AudioEngine::IsInitialized() const
{
    return m_Impl->Initialized;
}

bool AudioEngine::Load(const std::string& id, const std::string& filePath)
{
    if (!m_Impl->Initialized || id.empty() || filePath.empty())
    {
        return false;
    }
    MIX_Audio* audio = MIX_LoadAudio(m_Impl->Mixer, filePath.c_str(), true);
    if (audio == nullptr)
    {
        return false;
    }
    const auto existing = m_Impl->Clips.find(id);
    if (existing != m_Impl->Clips.end())
    {
        MIX_DestroyAudio(existing->second);
        existing->second = audio;
    }
    else
    {
        m_Impl->Clips.emplace(id, audio);
    }
    return true;
}

bool AudioEngine::LoadMemory(
    const std::string& id, const std::span<const std::byte> encodedAudio)
{
    if (!m_Impl->Initialized || id.empty() || encodedAudio.empty())
    {
        return false;
    }
    SDL_IOStream* io = SDL_IOFromConstMem(encodedAudio.data(), encodedAudio.size());
    if (io == nullptr)
    {
        return false;
    }
    MIX_Audio* audio = MIX_LoadAudio_IO(m_Impl->Mixer, io, true, true);
    if (audio == nullptr)
    {
        return false;
    }
    const auto existing = m_Impl->Clips.find(id);
    if (existing != m_Impl->Clips.end())
    {
        MIX_DestroyAudio(existing->second);
        existing->second = audio;
    }
    else
    {
        m_Impl->Clips.emplace(id, audio);
    }
    return true;
}

void AudioEngine::Unload(const std::string& id)
{
    StopById(id);
    const auto it = m_Impl->Clips.find(id);
    if (it == m_Impl->Clips.end())
    {
        return;
    }
    MIX_DestroyAudio(it->second);
    m_Impl->Clips.erase(it);
}

void AudioEngine::UnloadAll()
{
    StopAll();
    for (auto& [id, audio] : m_Impl->Clips)
    {
        static_cast<void>(id);
        MIX_DestroyAudio(audio);
    }
    m_Impl->Clips.clear();
}

int AudioEngine::Play(const std::string& id, const int loops, const float volume)
{
    if (!m_Impl->Initialized)
    {
        return -1;
    }
    const auto clip = m_Impl->Clips.find(id);
    if (clip == m_Impl->Clips.end())
    {
        return -1;
    }

    int freeIndex = -1;
    for (int i = 0; i < static_cast<int>(m_Impl->Tracks.size()); ++i)
    {
        Impl::TrackSlot& slot = m_Impl->Tracks[static_cast<std::size_t>(i)];
        if (!slot.InUse || !MIX_TrackPlaying(slot.Track))
        {
            freeIndex = i;
            break;
        }
    }
    if (freeIndex < 0)
    {
        return -1;
    }

    Impl::TrackSlot& slot = m_Impl->Tracks[static_cast<std::size_t>(freeIndex)];
    MIX_StopTrack(slot.Track, 0);
    if (!MIX_SetTrackAudio(slot.Track, clip->second))
    {
        return -1;
    }
    slot.Id = id;
    slot.Volume = Clamp01(volume);
    slot.InUse = true;
    m_Impl->ApplyTrackGain(slot);
    ClearTrackSpatial(freeIndex);

    SDL_PropertiesID props = SDL_CreateProperties();
    if (props != 0)
    {
        SDL_SetNumberProperty(props, MIX_PROP_PLAY_LOOPS_NUMBER, loops);
    }
    const bool ok = MIX_PlayTrack(slot.Track, props);
    if (props != 0)
    {
        SDL_DestroyProperties(props);
    }
    if (!ok)
    {
        slot.InUse = false;
        slot.Id.clear();
        return -1;
    }
    return freeIndex;
}

void AudioEngine::Stop(const int trackHandle)
{
    if (trackHandle < 0 || trackHandle >= static_cast<int>(m_Impl->Tracks.size()))
    {
        return;
    }
    Impl::TrackSlot& slot = m_Impl->Tracks[static_cast<std::size_t>(trackHandle)];
    MIX_StopTrack(slot.Track, 0);
    slot.InUse = false;
    slot.Id.clear();
}

void AudioEngine::StopAll()
{
    for (int i = 0; i < static_cast<int>(m_Impl->Tracks.size()); ++i)
    {
        Stop(i);
    }
}

void AudioEngine::StopById(const std::string& id)
{
    for (int i = 0; i < static_cast<int>(m_Impl->Tracks.size()); ++i)
    {
        if (m_Impl->Tracks[static_cast<std::size_t>(i)].Id == id)
        {
            Stop(i);
        }
    }
}

void AudioEngine::SetMasterVolume(const float volume)
{
    m_Impl->MasterVolume = Clamp01(volume);
    m_Impl->ApplyMixerGain();
}

void AudioEngine::SetSoundVolume(const float volume)
{
    m_Impl->SoundVolume = Clamp01(volume);
    for (Impl::TrackSlot& slot : m_Impl->Tracks)
    {
        if (slot.InUse)
        {
            m_Impl->ApplyTrackGain(slot);
        }
    }
}

void AudioEngine::SetMusicVolume(const float volume)
{
    // Music bus reserved for future dedicated music tracks; keep as stored gain.
    m_Impl->MusicVolume = Clamp01(volume);
}

float AudioEngine::GetMasterVolume() const
{
    return m_Impl->MasterVolume;
}

float AudioEngine::GetSoundVolume() const
{
    return m_Impl->SoundVolume;
}

float AudioEngine::GetMusicVolume() const
{
    return m_Impl->MusicVolume;
}

void AudioEngine::SetTrackWorldOffset(const int trackHandle, const glm::vec3& relativePosition)
{
    if (trackHandle < 0 || trackHandle >= static_cast<int>(m_Impl->Tracks.size()))
    {
        return;
    }
    MIX_Point3D point{relativePosition.x, relativePosition.y, relativePosition.z};
    MIX_SetTrack3DPosition(m_Impl->Tracks[static_cast<std::size_t>(trackHandle)].Track, &point);
}

void AudioEngine::ClearTrackSpatial(const int trackHandle)
{
    if (trackHandle < 0 || trackHandle >= static_cast<int>(m_Impl->Tracks.size()))
    {
        return;
    }
    MIX_SetTrack3DPosition(m_Impl->Tracks[static_cast<std::size_t>(trackHandle)].Track, nullptr);
}

bool AudioEngine::IsPlaying(const int trackHandle) const
{
    if (trackHandle < 0 || trackHandle >= static_cast<int>(m_Impl->Tracks.size()))
    {
        return false;
    }
    const Impl::TrackSlot& slot = m_Impl->Tracks[static_cast<std::size_t>(trackHandle)];
    return slot.InUse && MIX_TrackPlaying(slot.Track);
}

} // namespace fadix
