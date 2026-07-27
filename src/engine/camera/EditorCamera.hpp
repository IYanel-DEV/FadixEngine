#pragma once

#include <array>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace fadix
{
struct Bounds
{
    glm::vec3 Minimum{0.0F};
    glm::vec3 Maximum{0.0F};
};

struct Frustum
{
    std::array<glm::vec4, 6> Planes{};
};

class EditorCamera
{
public:
    virtual ~EditorCamera() = default;

    virtual void Orbit(glm::vec2 delta) { static_cast<void>(delta); }
    virtual void Pan(glm::vec2 delta) { static_cast<void>(delta); }
    virtual void Dolly(float delta) { static_cast<void>(delta); }
    virtual void FocusBounds(const Bounds& bounds) { static_cast<void>(bounds); }
    [[nodiscard]] virtual glm::mat4 View() const { return glm::mat4{1.0F}; }
    [[nodiscard]] virtual glm::mat4 Projection() const { return glm::mat4{1.0F}; }
    [[nodiscard]] virtual Frustum GetFrustum() const { return {}; }
};
}
