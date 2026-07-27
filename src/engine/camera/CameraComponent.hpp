#pragma once

namespace fadix
{
// Serialized game-camera data. Runtime components should include and reuse this
// type instead of maintaining a second definition.
struct CameraComponent
{
    float FieldOfView{60.0F};
    float NearPlane{0.05F};
    float FarPlane{5000.0F};
    bool Primary{false};
};
}
