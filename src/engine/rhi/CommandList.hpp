#pragma once

#include "engine/rhi/Types.hpp"

#include <cstdint>

namespace fadix::rhi
{
class Buffer;
class Pipeline;
class RenderTarget;

class CommandList
{
public:
    virtual ~CommandList() = default;
    virtual void Begin() = 0;
    virtual void BeginRenderPass(
        RenderTarget& target,
        float clearR = 0.025F,
        float clearG = 0.035F,
        float clearB = 0.055F,
        float clearA = 1.0F) = 0;
    // Load existing color/depth contents (overlay pass after post).
    virtual void BeginRenderPassLoad(RenderTarget& target) = 0;
    virtual void BindPipeline(Pipeline& pipeline) = 0;
    virtual void BindVertexBuffer(Buffer& buffer) = 0;
    virtual void Draw(std::uint32_t vertexCount, std::uint32_t firstVertex = 0) = 0;
    virtual void EndRenderPass() = 0;
    virtual void End() = 0;
};
}
