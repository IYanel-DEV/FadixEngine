#include "render/ShaderCompiler.hpp"

#include "assets/EmbeddedAssetProvider.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3dcompiler.h>
#endif

#include <filesystem>
#include <fstream>
#include <cstring>
#include <stdexcept>
#include <string>

namespace fadix::render
{
std::vector<std::byte> ReadShaderSource(const char* fileName)
{
    const std::filesystem::path path = RuntimeAssetRoot() / "shaders" / fileName;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        throw std::runtime_error("Could not open shader: " + path.string());
    }
    const std::streamsize size = stream.tellg();
    stream.seekg(0);
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(result.data()), size))
    {
        throw std::runtime_error("Could not read shader: " + path.string());
    }
    return result;
}

std::vector<std::byte> CompileShader(
    const std::span<const std::byte> source,
    const char* entryPoint,
    const char* target,
    const char* sourceName)
{
#ifdef _WIN32
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (compiler == nullptr)
    {
        throw std::runtime_error("d3dcompiler_47.dll is required for shaders");
    }
    using CompileFunction = HRESULT(WINAPI*)(
        LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
        UINT, UINT, ID3DBlob**, ID3DBlob**);
    const auto compile = reinterpret_cast<CompileFunction>(GetProcAddress(compiler, "D3DCompile"));
    if (compile == nullptr)
    {
        FreeLibrary(compiler);
        throw std::runtime_error("D3DCompile is unavailable");
    }

    ID3DBlob* bytecode = nullptr;
    ID3DBlob* errors = nullptr;
    const UINT flags =
#ifndef NDEBUG
        D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    std::string compatibleSource;
    const void* compileData = source.data();
    std::size_t compileSize = source.size();
    if (std::strcmp(target, "vs_5_0") == 0 || std::strcmp(target, "ps_5_0") == 0)
    {
        compatibleSource.assign(reinterpret_cast<const char*>(source.data()), source.size());
        std::size_t position = 0;
        while ((position = compatibleSource.find("register(", position)) != std::string::npos)
        {
            const std::size_t closing = compatibleSource.find(')', position);
            if (closing == std::string::npos)
            {
                break;
            }
            const std::size_t space = compatibleSource.find(", space", position);
            if (space != std::string::npos && space < closing)
            {
                compatibleSource.erase(space, closing - space);
            }
            position = compatibleSource.find(')', position);
            if (position == std::string::npos)
            {
                break;
            }
            ++position;
        }
        compileData = compatibleSource.data();
        compileSize = compatibleSource.size();
    }
    const HRESULT result = compile(
        compileData, compileSize, sourceName, nullptr, nullptr,
        entryPoint, target, flags, 0, &bytecode, &errors);
    std::string errorMessage;
    if (errors != nullptr)
    {
        errorMessage.assign(
            static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        errors->Release();
    }
    if (FAILED(result) || bytecode == nullptr)
    {
        FreeLibrary(compiler);
        throw std::runtime_error(
            std::string{"Shader '"} + entryPoint + "' compilation failed: " + errorMessage);
    }
    const auto* begin = static_cast<const std::byte*>(bytecode->GetBufferPointer());
    std::vector<std::byte> compiled(begin, begin + bytecode->GetBufferSize());
    bytecode->Release();
    FreeLibrary(compiler);
    return compiled;
#else
    static_cast<void>(source);
    static_cast<void>(entryPoint);
    static_cast<void>(target);
    static_cast<void>(sourceName);
    throw std::runtime_error("Runtime HLSL compilation is only supported on Windows");
#endif
}
}
