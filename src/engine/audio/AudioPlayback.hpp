#pragma once

#include "engine/audio/AudioEngine.hpp"
#include "engine/assets/IAssetDatabase.hpp"
#include "engine/scene/IWorld.hpp"

namespace fadix
{

// Play-mode audio lifecycle: load entity clips, play PlayOnStart, spatialize, stop.
class AudioPlayback
{
public:
    explicit AudioPlayback(AudioEngine& engine);

    void Start(IWorld& world, IAssetDatabase* assets);
    void Update(IWorld& world, float dt);
    void Stop();

private:
    AudioEngine& m_Engine;
    bool m_Active{false};
};

} // namespace fadix
