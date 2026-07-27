#pragma once

#include "engine/render/Picking.hpp"

#include <functional>
#include <memory>
#include <string>

namespace fadix
{
class IAssetDatabase;

class ViewportRenderer;

namespace rhi
{
class Device;
}

[[nodiscard]] std::unique_ptr<ViewportRenderer> CreateViewportRenderer(rhi::Device& device, IAssetDatabase& database);
[[nodiscard]] std::unique_ptr<Picking> CreateViewportPicking(ViewportRenderer& renderer);

// Switches the viewport's projection interpretation without exposing a
// backend-specific type through the engine interface.
void SetViewportOrthographic(ViewportRenderer& renderer, bool enabled) noexcept;

// Routes renderer errors (target creation, shader/pipeline failures, upload
// problems) into the editor output instead of stderr.
void SetViewportLog(ViewportRenderer& renderer, std::function<void(std::string)> log);
}
