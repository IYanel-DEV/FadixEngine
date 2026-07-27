// Compile-time validation of the HLSL that the engine otherwise only compiles
// lazily at runtime. Runs D3DCompile over every entry point of the viewport,
// post-processing and shadow shaders so a broken edit fails the build's test
// step instead of the first frame in the editor. Windows-only (d3dcompiler_47).

#include "render/ShaderCompiler.hpp"

#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace
{
int g_Failures = 0;

struct Entry
{
    const char* File;
    const char* EntryPoint;
    const char* Target;
};

void Compile(const Entry& entry)
{
    const std::string label = std::string{entry.File} + ":" + entry.EntryPoint;
    try
    {
        const std::vector<std::byte> source = fadix::render::ReadShaderSource(entry.File);
        const std::vector<std::byte> code = fadix::render::CompileShader(
            std::span<const std::byte>{source}, entry.EntryPoint, entry.Target, entry.File);
        if (code.empty())
        {
            std::cerr << "  FAIL " << label << " (empty bytecode)\n";
            ++g_Failures;
            return;
        }
        std::cout << "  ok   " << label << " (" << entry.Target << ")\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "  FAIL " << label << ": " << error.what() << '\n';
        ++g_Failures;
    }
}
}

int main()
{
    const Entry entries[] = {
        {"viewport.hlsl", "VertexMain", "vs_5_1"},
        {"viewport.hlsl", "FragmentMain", "ps_5_1"},
        {"viewport.hlsl", "UnlitFragmentMain", "ps_5_1"},
        {"viewport.hlsl", "UnlitLdrFragmentMain", "ps_5_1"},
        {"viewport.hlsl", "SkyVertexMain", "vs_5_1"},
        {"viewport.hlsl", "SkyFragmentMain", "ps_5_1"},
        {"postprocess.hlsl", "FullscreenVS", "vs_5_1"},
        {"postprocess.hlsl", "BrightExtractPS", "ps_5_1"},
        {"postprocess.hlsl", "BloomDownsamplePS", "ps_5_1"},
        {"postprocess.hlsl", "BloomUpsamplePS", "ps_5_1"},
        {"postprocess.hlsl", "CompositePS", "ps_5_1"},
        {"postprocess.hlsl", "TonemapPS", "ps_5_1"},
        {"postprocess.hlsl", "ColorGradePS", "ps_5_1"},
        {"postprocess.hlsl", "FxaaPS", "ps_5_1"},
        {"postprocess.hlsl", "CopyDitherPS", "ps_5_1"},
        {"postprocess.hlsl", "CopyPS", "ps_5_1"},
        {"postprocess.hlsl", "TaaPS", "ps_5_1"},
        {"postprocess.hlsl", "MotionVectorsPS", "ps_5_1"},
        {"shadow_depth.hlsl", "VertexMain", "vs_5_1"},
        {"shadow_depth.hlsl", "FragmentMain", "ps_5_1"},
        {"ssao.hlsl", "FullscreenVS", "vs_5_1"},
        {"ssao.hlsl", "SsaoPS", "ps_5_1"},
        {"ssao.hlsl", "BlurPS", "ps_5_1"},
        {"particle.hlsl", "VertexMain", "vs_5_1"},
        {"particle.hlsl", "FragmentMain", "ps_5_1"},
        {"terrain.hlsl", "VertexMain", "vs_5_1"},
        {"terrain.hlsl", "FragmentMain", "ps_5_1"},
    };

    std::cout << "Shader compilation smoke\n";
    for (const Entry& entry : entries)
    {
        Compile(entry);
    }

    if (g_Failures != 0)
    {
        std::cerr << g_Failures << " shader(s) failed to compile\n";
        return 1;
    }
    std::cout << "All shaders compiled\n";
    return 0;
}
