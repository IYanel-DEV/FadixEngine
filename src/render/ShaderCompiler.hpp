#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace fadix::render
{
// Reads assets/shaders/<fileName> as raw bytes. Throws std::runtime_error on failure.
[[nodiscard]] std::vector<std::byte> ReadShaderSource(const char* fileName);

// Compiles HLSL via d3dcompiler_47.dll. sourceName is used only in error messages.
// Throws std::runtime_error on failure.
[[nodiscard]] std::vector<std::byte> CompileShader(
    std::span<const std::byte> source,
    const char* entryPoint,
    const char* target,
    const char* sourceName);
}
