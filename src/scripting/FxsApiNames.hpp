#pragma once

// Shared FXS binding name strings. LuaVM uses these for usertype / table keys
// and lifecycle Call*() names; the editor catalog uses the same literals so
// autocomplete cannot drift from runtime. Header-only — no reflection.

namespace fadix::fxs
{
inline constexpr const char* kEntityType = "Entity";
inline constexpr const char* kEntityId = "id";
inline constexpr const char* kEntityGetName = "getName";
inline constexpr const char* kEntityGetPosition = "getPosition";
inline constexpr const char* kEntitySetPosition = "setPosition";
inline constexpr const char* kEntityGetRotation = "getRotation";
inline constexpr const char* kEntitySetRotation = "setRotation";
inline constexpr const char* kEntityGetScale = "getScale";
inline constexpr const char* kEntitySetScale = "setScale";
inline constexpr const char* kEntityDestroy = "destroy";
inline constexpr const char* kEntityGetTarget = "getTarget";
inline constexpr const char* kEntityMoveCharacter = "moveCharacter";
inline constexpr const char* kEntityJumpCharacter = "jumpCharacter";
inline constexpr const char* kEntityIsCharacterGrounded = "isCharacterGrounded";

inline constexpr const char* kPrint = "print";
inline constexpr const char* kInput = "Input";
inline constexpr const char* kInputIsDown = "isDown";

inline constexpr const char* kAudio = "audio";
inline constexpr const char* kAudioLoad = "load";
inline constexpr const char* kAudioPlay = "play";
inline constexpr const char* kAudioStop = "stop";
inline constexpr const char* kAudioSetMasterVolume = "setMasterVolume";
inline constexpr const char* kAudioSetSoundVolume = "setSoundVolume";
inline constexpr const char* kAudioSetMusicVolume = "setMusicVolume";

// Gameplay world API. Global tables bound in LuaVM::BindGameTables, backed by
// the play session's runtime world (World.find), prefab loader (Prefab.spawn)
// and scene loader (Scene.load).
inline constexpr const char* kWorld = "World";
inline constexpr const char* kWorldFind = "find";
inline constexpr const char* kPrefab = "Prefab";
inline constexpr const char* kPrefabSpawn = "spawn";
inline constexpr const char* kScene = "Scene";
inline constexpr const char* kSceneLoad = "load";

inline constexpr const char* kOnStart = "OnStart";
inline constexpr const char* kOnUpdate = "OnUpdate";
inline constexpr const char* kOnDestroy = "OnDestroy";
}
