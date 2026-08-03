#include "scripting/LuaVM.hpp"

#ifdef FADIX_ENABLE_LUA

#include "engine/audio/AudioEngine.hpp"
#include "runtime/AnimationRuntime.hpp"
#include "runtime/Components.hpp"
#include "scripting/FxsApiNames.hpp"

#include <SDL3/SDL.h>
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

    [[nodiscard]] bool startAnimator()
    {
        if (reg == nullptr)
        {
            return false;
        }
        if (auto* animator = reg->try_get<AnimatorComponent>(e);
            animator != nullptr && animator->Graph &&
            animator->Graph->OutputNodeIndex >= 0 &&
            animator->Graph->OutputNodeIndex < static_cast<int>(animator->Graph->Nodes.size()))
        {
            animator->Graph->ResetRuntime();
            animator->RuntimeParameters = animator->Graph->Parameters;
            animator->Playing = true;
            animator->Paused = false;
            animator->ClearBlend();
            animator->ClearEventState();
            if (auto* transform = reg->try_get<TransformAnimatorComponent>(e))
            {
                transform->Playing = false;
                transform->Paused = false;
                transform->ClearControllerRuntime();
            }
            return true;
        }
        if (auto* animator = reg->try_get<TransformAnimatorComponent>(e);
            animator != nullptr && !animator->Controller.States.empty())
        {
            const std::string& entryName = animator->Controller.EntryState.empty()
                ? animator->Controller.States.front().Name
                : animator->Controller.EntryState;
            const AnimatorState* entry = FindAnimatorState(animator->Controller, entryName);
            if (entry == nullptr || FindTransformClip(
                    static_cast<const TransformAnimatorComponent&>(*animator),
                    entry->ClipName) == nullptr)
            {
                return false;
            }
            if (!StartAnimatorController(*animator))
            {
                return false;
            }
            if (auto* skeletal = reg->try_get<AnimatorComponent>(e))
            {
                skeletal->Playing = false;
                skeletal->Paused = false;
                skeletal->ClearControllerRuntime();
            }
            return true;
        }
        if (auto* animator = reg->try_get<AnimatorComponent>(e);
            animator != nullptr && !animator->Controller.States.empty())
        {
            if (!StartAnimatorController(*animator))
            {
                return false;
            }
            if (auto* transform = reg->try_get<TransformAnimatorComponent>(e))
            {
                transform->Playing = false;
                transform->Paused = false;
                transform->ClearControllerRuntime();
            }
            return true;
        }
        return false;
    }

    [[nodiscard]] bool setAnimatorBool(const std::string& name, const bool value)
    {
        bool changed = false;
        if (auto* animator = reg ? reg->try_get<AnimatorComponent>(e) : nullptr)
        {
            changed = SetAnimatorBool(*animator, name, value) || changed;
        }
        if (auto* animator = reg ? reg->try_get<TransformAnimatorComponent>(e) : nullptr)
        {
            changed = SetAnimatorBool(*animator, name, value) || changed;
        }
        return changed;
    }

    [[nodiscard]] bool setAnimatorFloat(const std::string& name, const float value)
    {
        bool changed = false;
        if (auto* animator = reg ? reg->try_get<AnimatorComponent>(e) : nullptr)
        {
            changed = SetAnimatorFloat(*animator, name, value) || changed;
        }
        if (auto* animator = reg ? reg->try_get<TransformAnimatorComponent>(e) : nullptr)
        {
            changed = SetAnimatorFloat(*animator, name, value) || changed;
        }
        return changed;
    }

    [[nodiscard]] bool setAnimatorInt(const std::string& name, const int value)
    {
        bool changed = false;
        if (auto* animator = reg ? reg->try_get<AnimatorComponent>(e) : nullptr)
        {
            changed = SetAnimatorInt(*animator, name, value) || changed;
        }
        if (auto* animator = reg ? reg->try_get<TransformAnimatorComponent>(e) : nullptr)
        {
            changed = SetAnimatorInt(*animator, name, value) || changed;
        }
        return changed;
    }

    [[nodiscard]] bool triggerAnimator(const std::string& name)
    {
        return setAnimatorBool(name, true);
    }

    [[nodiscard]] bool playAnimation(const sol::optional<std::string>& requestedClip)
    {
        if (reg == nullptr)
        {
            return false;
        }
        auto* transformAnimator = reg->try_get<TransformAnimatorComponent>(e);
        if (requestedClip && !requestedClip->empty() && transformAnimator != nullptr)
        {
            if (AnimationClipAsset* clip = FindTransformClip(*transformAnimator, *requestedClip))
            {
                if (auto* skeletal = reg->try_get<AnimatorComponent>(e))
                {
                    skeletal->Playing = false;
                    skeletal->Paused = false;
                    skeletal->CurrentTime = 0.0F;
                    ClearAnimationBlend(*skeletal);
                    skeletal->ClearEventState();
                    skeletal->ClearControllerRuntime();
                }
                transformAnimator->ClipName = clip->Name;
                transformAnimator->ClearControllerRuntime();
                transformAnimator->CurrentTime = 0.0F;
                transformAnimator->Paused = false;
                transformAnimator->Playing = true;
                ClearAnimationBlend(*transformAnimator);
                transformAnimator->ClearEventState();
                transformAnimator->EmitStartEvents = true;
                return true;
            }
        }
        if (auto* animator = reg->try_get<AnimatorComponent>(e))
        {
            if (requestedClip && !requestedClip->empty())
            {
                animator->ClipName = *requestedClip;
            }
            animator->CurrentTime = 0.0F;
            animator->ClearControllerRuntime();
            if (animator->Graph)
            {
                animator->Graph->ResetRuntime();
            }
            animator->Paused = false;
            animator->Playing = true;
            ClearAnimationBlend(*animator);
            animator->ClearEventState();
            animator->EmitStartEvents = true;
            if (transformAnimator != nullptr)
            {
                transformAnimator->Playing = false;
                transformAnimator->Paused = false;
                transformAnimator->CurrentTime = 0.0F;
                ClearAnimationBlend(*transformAnimator);
                transformAnimator->ClearEventState();
                transformAnimator->ClearControllerRuntime();
            }
            return true;
        }
        if (transformAnimator != nullptr)
        {
            if (requestedClip && !requestedClip->empty() &&
                FindTransformClip(*transformAnimator, *requestedClip) == nullptr)
            {
                return false;
            }
            if (FindTransformClip(*transformAnimator) == nullptr)
            {
                return false;
            }
            transformAnimator->CurrentTime = 0.0F;
            transformAnimator->ClearControllerRuntime();
            transformAnimator->Paused = false;
            transformAnimator->Playing = true;
            ClearAnimationBlend(*transformAnimator);
            transformAnimator->ClearEventState();
            transformAnimator->EmitStartEvents = true;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool crossFadeAnimation(const std::string& requestedClip,
        const sol::optional<float>& requestedDuration)
    {
        if (reg == nullptr || requestedClip.empty())
        {
            return false;
        }
        const float duration = std::max(requestedDuration.value_or(0.25F), 0.0F);
        auto* transformAnimator = reg->try_get<TransformAnimatorComponent>(e);
        if (transformAnimator != nullptr)
        {
            if (const AnimationClipAsset* target = FindTransformClip(
                    static_cast<const TransformAnimatorComponent&>(*transformAnimator),
                    requestedClip))
            {
                const bool hasSource = (transformAnimator->Playing || transformAnimator->Paused) &&
                    !transformAnimator->ClipName.empty() && duration > 0.0F;
                const std::string fromName = transformAnimator->ClipName;
                const float fromTime = transformAnimator->CurrentTime;
                transformAnimator->ClearControllerRuntime();
                transformAnimator->ClipName = target->Name;
                transformAnimator->CurrentTime = 0.0F;
                transformAnimator->Playing = true;
                transformAnimator->Paused = false;
                ClearAnimationBlend(*transformAnimator);
                transformAnimator->ClearEventState();
                transformAnimator->EmitStartEvents = true;
                if (hasSource)
                {
                    transformAnimator->BlendFromClipName = fromName;
                    transformAnimator->BlendFromTime = fromTime;
                    transformAnimator->BlendDuration = duration;
                }
                if (auto* skeletal = reg->try_get<AnimatorComponent>(e))
                {
                    skeletal->Playing = false;
                    skeletal->Paused = false;
                    skeletal->CurrentTime = 0.0F;
                    ClearAnimationBlend(*skeletal);
                    skeletal->ClearEventState();
                    skeletal->ClearControllerRuntime();
                }
                return true;
            }
        }
        if (auto* animator = reg->try_get<AnimatorComponent>(e))
        {
            const bool hasSource = (animator->Playing || animator->Paused) &&
                !animator->ClipName.empty() && duration > 0.0F;
            const std::string fromName = animator->ClipName;
            const float fromTime = animator->CurrentTime;
            animator->ClearControllerRuntime();
            animator->ClipName = requestedClip;
            animator->CurrentTime = 0.0F;
            animator->Playing = true;
            animator->Paused = false;
            ClearAnimationBlend(*animator);
            animator->ClearEventState();
            animator->EmitStartEvents = true;
            if (hasSource)
            {
                animator->BlendFromClipName = fromName;
                animator->BlendFromTime = fromTime;
                animator->BlendDuration = duration;
            }
            if (transformAnimator != nullptr)
            {
                transformAnimator->Playing = false;
                transformAnimator->Paused = false;
                transformAnimator->CurrentTime = 0.0F;
                ClearAnimationBlend(*transformAnimator);
                transformAnimator->ClearEventState();
                transformAnimator->ClearControllerRuntime();
            }
            return true;
        }
        return false;
    }

    [[nodiscard]] bool pauseAnimation()
    {
        bool paused = false;
        if (auto* animator = reg ? reg->try_get<AnimatorComponent>(e) : nullptr;
            animator != nullptr && animator->Playing)
        {
            animator->Playing = false;
            animator->Paused = true;
            paused = true;
        }
        if (auto* animator = reg ? reg->try_get<TransformAnimatorComponent>(e) : nullptr;
            animator != nullptr && animator->Playing)
        {
            animator->Playing = false;
            animator->Paused = true;
            paused = true;
        }
        return paused;
    }

    [[nodiscard]] bool resumeAnimation()
    {
        bool resumed = false;
        if (auto* animator = reg ? reg->try_get<AnimatorComponent>(e) : nullptr;
            animator != nullptr && animator->Paused)
        {
            animator->Paused = false;
            animator->Playing = true;
            resumed = true;
        }
        if (auto* animator = reg ? reg->try_get<TransformAnimatorComponent>(e) : nullptr;
            animator != nullptr && animator->Paused)
        {
            animator->Paused = false;
            animator->Playing = true;
            resumed = true;
        }
        return resumed;
    }

    void stopAnimation()
    {
        if (auto* animator = reg ? reg->try_get<AnimatorComponent>(e) : nullptr)
        {
            animator->Playing = false;
            animator->Paused = false;
            animator->CurrentTime = 0.0F;
            ClearAnimationBlend(*animator);
            animator->ClearEventState();
            animator->ClearControllerRuntime();
            if (animator->Graph)
            {
                animator->Graph->ResetRuntime();
            }
        }
        if (auto* animator = reg ? reg->try_get<TransformAnimatorComponent>(e) : nullptr)
        {
            animator->Playing = false;
            animator->Paused = false;
            animator->CurrentTime = 0.0F;
            ClearAnimationBlend(*animator);
            animator->ClearEventState();
            animator->ClearControllerRuntime();
        }
    }

    [[nodiscard]] bool seekAnimation(const float seconds)
    {
        bool found = false;
        const float time = std::max(seconds, 0.0F);
        if (auto* animator = reg ? reg->try_get<AnimatorComponent>(e) : nullptr)
        {
            animator->CurrentTime = time;
            if (animator->Graph)
            {
                animator->Graph->SetRuntimeTime(time);
            }
            found = true;
        }
        if (auto* animator = reg ? reg->try_get<TransformAnimatorComponent>(e) : nullptr)
        {
            const AnimationClipAsset* clip = FindTransformClip(*animator);
            animator->CurrentTime = clip != nullptr && clip->Duration > 0.0F
                ? std::min(time, clip->Duration)
                : time;
            found = true;
        }
        return found;
    }

    [[nodiscard]] bool setAnimationSpeed(const float speed)
    {
        bool found = false;
        if (auto* animator = reg ? reg->try_get<AnimatorComponent>(e) : nullptr)
        {
            animator->Speed = speed;
            found = true;
        }
        if (auto* animator = reg ? reg->try_get<TransformAnimatorComponent>(e) : nullptr)
        {
            animator->Speed = speed;
            found = true;
        }
        return found;
    }

    [[nodiscard]] std::string getCurrentAnimation() const
    {
        const auto* skeletal = reg ? reg->try_get<AnimatorComponent>(e) : nullptr;
        const auto* transform = reg ? reg->try_get<TransformAnimatorComponent>(e) : nullptr;
        if (transform != nullptr && (transform->Playing || transform->Paused))
        {
            return transform->ClipName;
        }
        if (skeletal != nullptr)
        {
            return skeletal->ClipName;
        }
        return transform != nullptr ? transform->ClipName : std::string{};
    }

    [[nodiscard]] float getAnimationTime() const
    {
        const auto* skeletal = reg ? reg->try_get<AnimatorComponent>(e) : nullptr;
        const auto* transform = reg ? reg->try_get<TransformAnimatorComponent>(e) : nullptr;
        if (transform != nullptr && (transform->Playing || transform->Paused))
        {
            return transform->CurrentTime;
        }
        if (skeletal != nullptr)
        {
            return skeletal->CurrentTime;
        }
        return transform != nullptr ? transform->CurrentTime : 0.0F;
    }

    [[nodiscard]] bool isAnimationPlaying() const
    {
        const auto* skeletal = reg ? reg->try_get<AnimatorComponent>(e) : nullptr;
        const auto* transformAnimator = reg ? reg->try_get<TransformAnimatorComponent>(e) : nullptr;
        return (skeletal != nullptr && skeletal->Playing) ||
            (transformAnimator != nullptr && transformAnimator->Playing);
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

    [[nodiscard]] std::tuple<float, float, float, float> getSpriteTint() const
    {
        if (const auto* s = reg ? reg->try_get<Sprite2DComponent>(e) : nullptr)
        {
            return {s->Tint.r, s->Tint.g, s->Tint.b, s->Tint.a};
        }
        return {1.0F, 1.0F, 1.0F, 1.0F};
    }
    void setSpriteTint(float r, float g, float b, float a)
    {
        if (auto* s = reg ? reg->try_get<Sprite2DComponent>(e) : nullptr)
        {
            s->Tint = {r, g, b, a};
        }
    }
    void setSpriteVisible(const bool visible)
    {
        if (auto* s = reg ? reg->try_get<Sprite2DComponent>(e) : nullptr)
        {
            s->Tint.a = visible ? 1.0F : 0.0F;
        }
    }
    void setSortOrder(const int layer, const int order)
    {
        if (auto* s = reg ? reg->try_get<Sprite2DComponent>(e) : nullptr)
        {
            s->SortingLayer = layer;
            s->OrderInLayer = order;
        }
    }
    void playSpriteAnimation(const std::string& clipName)
    {
        if (auto* a = reg ? reg->try_get<SpriteFrameAnimatorComponent>(e) : nullptr)
        {
            a->CurrentClip = clipName;
            a->Playing = true;
            a->CurrentTime = 0.0F;
            a->CurrentFrame = 0;
        }
    }
    [[nodiscard]] std::tuple<float, float> getVelocity2D() const
    {
        if (reg == nullptr)
        {
            return {0.0F, 0.0F};
        }
        const RigidBody2DComponent* rb = reg->try_get<RigidBody2DComponent>(e);
        if (rb == nullptr)
        {
            return {0.0F, 0.0F};
        }
        return {rb->InitialLinearVelocity.x, rb->InitialLinearVelocity.y};
    }
    void setVelocity2D(const float vx, const float vy)
    {
        if (auto* rb = reg ? reg->try_get<RigidBody2DComponent>(e) : nullptr)
        {
            rb->InitialLinearVelocity = {vx, vy};
        }
    }
    void applyImpulse2D(const float ix, const float iy)
    {
        if (auto* rb = reg ? reg->try_get<RigidBody2DComponent>(e) : nullptr)
        {
            rb->InitialLinearVelocity.x += ix;
            rb->InitialLinearVelocity.y += iy;
        }
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

class InputActions
{
public:
    ~InputActions() { CloseGamepad(); }

    bool Bind(const std::string& action, const std::string& text)
    {
        const std::optional<Binding> binding = ParseBinding(text);
        const std::string key = ActionKey(action);
        if (key.empty() || !binding)
        {
            return false;
        }
        m_Actions[key] = {*binding};
        return true;
    }

    bool AddBinding(const std::string& action, const std::string& text)
    {
        const std::optional<Binding> binding = ParseBinding(text);
        const std::string key = ActionKey(action);
        if (key.empty() || !binding)
        {
            return false;
        }
        std::vector<Binding>& bindings = m_Actions[key];
        if (std::find(bindings.begin(), bindings.end(), *binding) == bindings.end())
        {
            bindings.push_back(*binding);
        }
        return true;
    }

    bool Clear(const std::string& action) { return m_Actions.erase(ActionKey(action)) != 0; }

    void Reset()
    {
        m_Actions.clear();
        CloseGamepad();
    }

    [[nodiscard]] bool IsDown(const std::string& action)
    {
        const auto found = m_Actions.find(ActionKey(action));
        if (found == m_Actions.end())
        {
            return false;
        }
        return std::any_of(found->second.begin(), found->second.end(),
            [this](const Binding& binding) { return BindingIsDown(binding); });
    }

private:
    enum class Kind
    {
        Keyboard,
        Mouse,
        Gamepad
    };

    struct Binding
    {
        Kind Type{Kind::Keyboard};
        int Code{};
        bool operator==(const Binding&) const = default;
    };

    static std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
            [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    static std::string ActionKey(const std::string& action) { return Lower(action); }

    static std::string Compact(std::string text)
    {
        text = Lower(std::move(text));
        std::erase_if(text, [](const char c) { return c == ' ' || c == '_' || c == '-'; });
        return text;
    }

    static std::optional<Binding> ParseBinding(const std::string& text)
    {
        if (text.empty())
        {
            return std::nullopt;
        }
        const std::size_t separator = text.find(':');
        const std::string prefix = separator == std::string::npos
            ? "key"
            : Lower(text.substr(0, separator));
        const std::string value = separator == std::string::npos
            ? text
            : text.substr(separator + 1);
        if (value.empty())
        {
            return std::nullopt;
        }
        if (prefix == "key" || prefix == "keyboard")
        {
            const SDL_Scancode code = SDL_GetScancodeFromName(value.c_str());
            return code == SDL_SCANCODE_UNKNOWN
                ? std::nullopt
                : std::optional{Binding{Kind::Keyboard, static_cast<int>(code)}};
        }
        const std::string token = Compact(value);
        if (prefix == "mouse")
        {
            int button = 0;
            if (token == "left") button = SDL_BUTTON_LEFT;
            else if (token == "middle") button = SDL_BUTTON_MIDDLE;
            else if (token == "right") button = SDL_BUTTON_RIGHT;
            else if (token == "x1") button = SDL_BUTTON_X1;
            else if (token == "x2") button = SDL_BUTTON_X2;
            return button == 0
                ? std::nullopt
                : std::optional{Binding{Kind::Mouse, button}};
        }
        if (prefix == "gamepad" || prefix == "pad")
        {
            SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_INVALID;
            if (token == "a" || token == "south") button = SDL_GAMEPAD_BUTTON_SOUTH;
            else if (token == "b" || token == "east") button = SDL_GAMEPAD_BUTTON_EAST;
            else if (token == "x" || token == "west") button = SDL_GAMEPAD_BUTTON_WEST;
            else if (token == "y" || token == "north") button = SDL_GAMEPAD_BUTTON_NORTH;
            else if (token == "back") button = SDL_GAMEPAD_BUTTON_BACK;
            else if (token == "guide") button = SDL_GAMEPAD_BUTTON_GUIDE;
            else if (token == "start") button = SDL_GAMEPAD_BUTTON_START;
            else if (token == "leftstick") button = SDL_GAMEPAD_BUTTON_LEFT_STICK;
            else if (token == "rightstick") button = SDL_GAMEPAD_BUTTON_RIGHT_STICK;
            else if (token == "lb" || token == "leftshoulder")
                button = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
            else if (token == "rb" || token == "rightshoulder")
                button = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
            else if (token == "dpadup") button = SDL_GAMEPAD_BUTTON_DPAD_UP;
            else if (token == "dpaddown") button = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
            else if (token == "dpadleft") button = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
            else if (token == "dpadright") button = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
            return button == SDL_GAMEPAD_BUTTON_INVALID
                ? std::nullopt
                : std::optional{Binding{Kind::Gamepad, static_cast<int>(button)}};
        }
        return std::nullopt;
    }

    [[nodiscard]] bool BindingIsDown(const Binding& binding)
    {
        if (binding.Type == Kind::Keyboard)
        {
            if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0)
            {
                return false;
            }
            int count = 0;
            const bool* state = SDL_GetKeyboardState(&count);
            return state != nullptr && binding.Code >= 0 && binding.Code < count &&
                   state[binding.Code];
        }
        if (binding.Type == Kind::Mouse)
        {
            return (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0 &&
                   (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(binding.Code)) != 0;
        }
        SDL_Gamepad* gamepad = Gamepad();
        return gamepad != nullptr && SDL_GetGamepadButton(
            gamepad, static_cast<SDL_GamepadButton>(binding.Code));
    }

    [[nodiscard]] SDL_Gamepad* Gamepad()
    {
        if (m_Gamepad != nullptr && !SDL_GamepadConnected(m_Gamepad))
        {
            CloseGamepad();
        }
        if (m_Gamepad != nullptr || (SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) == 0)
        {
            return m_Gamepad;
        }
        int count = 0;
        SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
        if (gamepads != nullptr && count > 0)
        {
            m_Gamepad = SDL_OpenGamepad(gamepads[0]);
        }
        SDL_free(gamepads);
        return m_Gamepad;
    }

    void CloseGamepad()
    {
        if (m_Gamepad != nullptr)
        {
            SDL_CloseGamepad(m_Gamepad);
            m_Gamepad = nullptr;
        }
    }

    std::unordered_map<std::string, std::vector<Binding>> m_Actions;
    SDL_Gamepad* m_Gamepad{};
};
}

class LuaVMImpl
{
public:
    LuaVMImpl() { Setup(); }

    void Setup()
    {
        m_InputActions.Reset();
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
            fxs::kEntityPlayAnimation, &LuaEntity::playAnimation,
            fxs::kEntityCrossFadeAnimation, &LuaEntity::crossFadeAnimation,
            fxs::kEntityPauseAnimation, &LuaEntity::pauseAnimation,
            fxs::kEntityResumeAnimation, &LuaEntity::resumeAnimation,
            fxs::kEntityStopAnimation, &LuaEntity::stopAnimation,
            fxs::kEntityIsAnimationPlaying, &LuaEntity::isAnimationPlaying,
            fxs::kEntitySeekAnimation, &LuaEntity::seekAnimation,
            fxs::kEntitySetAnimationSpeed, &LuaEntity::setAnimationSpeed,
            fxs::kEntityGetCurrentAnimation, &LuaEntity::getCurrentAnimation,
            fxs::kEntityGetAnimationTime, &LuaEntity::getAnimationTime,
            fxs::kEntityStartAnimator, &LuaEntity::startAnimator,
            fxs::kEntitySetAnimatorBool, &LuaEntity::setAnimatorBool,
            fxs::kEntitySetAnimatorFloat, &LuaEntity::setAnimatorFloat,
            fxs::kEntitySetAnimatorInt, &LuaEntity::setAnimatorInt,
            fxs::kEntityTriggerAnimator, &LuaEntity::triggerAnimator,
            fxs::kEntityDestroy, &LuaEntity::destroy,
            fxs::kEntityGetTarget, &LuaEntity::getTarget,
            fxs::kEntityGetSpriteTint, &LuaEntity::getSpriteTint,
            fxs::kEntitySetSpriteTint, &LuaEntity::setSpriteTint,
            fxs::kEntitySetSpriteVisible, &LuaEntity::setSpriteVisible,
            fxs::kEntitySetSortOrder, &LuaEntity::setSortOrder,
            fxs::kEntityPlaySpriteAnimation, &LuaEntity::playSpriteAnimation,
            fxs::kEntityGetVelocity2D, &LuaEntity::getVelocity2D,
            fxs::kEntitySetVelocity2D, &LuaEntity::setVelocity2D,
            fxs::kEntityApplyImpulse2D, &LuaEntity::applyImpulse2D);

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
        input[fxs::kInputAction] =
            [this](const std::string& action) { return m_InputActions.IsDown(action); };
        input[fxs::kInputBind] = [this](const std::string& action, const std::string& binding) {
            return m_InputActions.Bind(action, binding);
        };
        input[fxs::kInputAddBinding] =
            [this](const std::string& action, const std::string& binding) {
                return m_InputActions.AddBinding(action, binding);
            };
        input[fxs::kInputClear] =
            [this](const std::string& action) { return m_InputActions.Clear(action); };
        m_Lua[fxs::kInput] = input;

        BindAudioTable();
        BindGameTables();
    }

    void SetWorldApi(ScriptWorldApi* api)
    {
        m_WorldApi = api;
        BindGameTables();
    }

    // Gameplay globals are rebound once per Lua state; the
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

        sol::table save = m_Lua.create_table();
        save[fxs::kSaveWrite] = [this](const std::string& slot) {
            return m_WorldApi != nullptr && m_WorldApi->WriteSave &&
                   m_WorldApi->WriteSave(slot);
        };
        save[fxs::kSaveLoad] = [this](const std::string& slot) {
            return m_WorldApi != nullptr && m_WorldApi->LoadSave &&
                   m_WorldApi->LoadSave(slot);
        };
        m_Lua[fxs::kSave] = save;
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

    void CallAnimationEvent(int instance, const ScriptEntityHandle& handle,
        const std::string& name, const std::string& payload)
    {
        if (instance < 0 || instance >= static_cast<int>(m_Instances.size()) ||
            !m_Instances[static_cast<std::size_t>(instance)])
        {
            return;
        }
        sol::environment& environment = *m_Instances[static_cast<std::size_t>(instance)];
        sol::protected_function callback = environment[fxs::kOnAnimationEvent];
        if (!callback.valid())
        {
            return;
        }
        const LuaEntity entity{handle.Registry, handle.Entity, handle.PendingDestroy};
        const sol::protected_function_result result = callback(entity, name, payload);
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
    InputActions m_InputActions;
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
void LuaVM::CallAnimationEvent(int instance, const ScriptEntityHandle& entity,
    const std::string& name, const std::string& payload)
{
    m_Impl->CallAnimationEvent(instance, entity, name, payload);
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
void LuaVM::CallAnimationEvent(int, const ScriptEntityHandle&, const std::string&, const std::string&) {}
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
