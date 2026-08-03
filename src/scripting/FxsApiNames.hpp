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
inline constexpr const char* kEntityPlayAnimation = "playAnimation";
inline constexpr const char* kEntityCrossFadeAnimation = "crossFadeAnimation";
inline constexpr const char* kEntityPauseAnimation = "pauseAnimation";
inline constexpr const char* kEntityResumeAnimation = "resumeAnimation";
inline constexpr const char* kEntityStopAnimation = "stopAnimation";
inline constexpr const char* kEntityIsAnimationPlaying = "isAnimationPlaying";
inline constexpr const char* kEntitySeekAnimation = "seekAnimation";
inline constexpr const char* kEntitySetAnimationSpeed = "setAnimationSpeed";
inline constexpr const char* kEntityGetCurrentAnimation = "getCurrentAnimation";
inline constexpr const char* kEntityGetAnimationTime = "getAnimationTime";
inline constexpr const char* kEntityStartAnimator = "startAnimator";
inline constexpr const char* kEntitySetAnimatorBool = "setAnimatorBool";
inline constexpr const char* kEntitySetAnimatorFloat = "setAnimatorFloat";
inline constexpr const char* kEntitySetAnimatorInt = "setAnimatorInt";
inline constexpr const char* kEntityTriggerAnimator = "triggerAnimator";

inline constexpr const char* kPrint = "print";
inline constexpr const char* kInput = "Input";
inline constexpr const char* kInputIsDown = "isDown";
inline constexpr const char* kInputAction = "action";
inline constexpr const char* kInputBind = "bind";
inline constexpr const char* kInputAddBinding = "addBinding";
inline constexpr const char* kInputClear = "clear";

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
inline constexpr const char* kSave = "Save";
inline constexpr const char* kSaveWrite = "write";
inline constexpr const char* kSaveLoad = "load";

inline constexpr const char* kEntityGetSpriteTint = "getSpriteTint";
inline constexpr const char* kEntitySetSpriteTint = "setSpriteTint";
inline constexpr const char* kEntitySetSpriteVisible = "setSpriteVisible";
inline constexpr const char* kEntitySetSortOrder = "setSortOrder";
inline constexpr const char* kEntityPlaySpriteAnimation = "playSpriteAnimation";
inline constexpr const char* kEntityGetVelocity2D = "getVelocity2D";
inline constexpr const char* kEntitySetVelocity2D = "setVelocity2D";
inline constexpr const char* kEntityApplyImpulse2D = "applyImpulse2D";

inline constexpr const char* kOnStart = "OnStart";
inline constexpr const char* kOnUpdate = "OnUpdate";
inline constexpr const char* kOnDestroy = "OnDestroy";
inline constexpr const char* kOnAnimationEvent = "OnAnimationEvent";
}
