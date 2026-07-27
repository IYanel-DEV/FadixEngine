#pragma once

#include <glm/vec3.hpp>

#include <cstdint>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace fadix
{

// Thin wrapper over SDL3_mixer (MIX_* API). One mixer device; clips keyed by id;
// playback returns a track handle (>=0) or -1 on failure.
class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    [[nodiscard]] bool Initialize();
    void Shutdown();
    [[nodiscard]] bool IsInitialized() const;

    // Load (or replace) a decoded clip. Returns false on failure.
    bool Load(const std::string& id, const std::string& filePath);
    bool LoadMemory(const std::string& id, std::span<const std::byte> encodedAudio);
    void Unload(const std::string& id);
    void UnloadAll();

    // Play a loaded clip. loops: 0 = once, -1 = infinite.
    // Returns track handle (>=0) or -1.
    [[nodiscard]] int Play(const std::string& id, int loops = 0, float volume = 1.0F);
    void Stop(int trackHandle);
    void StopAll();
    void StopById(const std::string& id);

    void SetMasterVolume(float volume);
    void SetSoundVolume(float volume);
    void SetMusicVolume(float volume);
    [[nodiscard]] float GetMasterVolume() const;
    [[nodiscard]] float GetSoundVolume() const;
    [[nodiscard]] float GetMusicVolume() const;

    // Listener is always (0,0,0) in mixer space — pass source position relative to listener.
    void SetTrackWorldOffset(int trackHandle, const glm::vec3& relativePosition);
    void ClearTrackSpatial(int trackHandle);

    [[nodiscard]] bool IsPlaying(int trackHandle) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace fadix
