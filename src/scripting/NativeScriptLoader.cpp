#include "scripting/NativeScriptLoader.hpp"

#include "scripting/NativeScript.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fadix
{
NativeScriptLoader::NativeScriptLoader(std::filesystem::path intermediateDir)
    : m_IntermediateDir(std::move(intermediateDir))
{
}

NativeScriptLoader::~NativeScriptLoader() = default;

void NativeScriptLoader::SetLogger(Logger logger) { m_Logger = std::move(logger); }

void NativeScriptLoader::SetIncludeDirs(std::vector<std::filesystem::path> includeDirs)
{
    m_IncludeDirs = std::move(includeDirs);
}

void NativeScriptLoader::Log(const std::string& message, const char* severity) const
{
    if (m_Logger)
    {
        m_Logger(message, severity);
    }
}

bool NativeScriptLoader::ToolchainAvailable()
{
#ifdef _WIN32
    return std::system("cl.exe /nologo /? >nul 2>&1") == 0;
#else
    return false;
#endif
}

NativeScriptLoader::Loaded NativeScriptLoader::CompileAndLoad(const std::filesystem::path& sourceFile)
{
    Loaded result;
#ifdef _WIN32
    std::filesystem::path dll;
    std::string output;
    if (!Compile(sourceFile, dll, output))
    {
        Log("Native script compile failed for " + sourceFile.filename().string() + ":\n" +
                (output.empty()
                        ? "A Visual Studio developer environment (cl.exe on PATH) is required."
                        : output),
            "error");
        return result;
    }

    HMODULE module = LoadLibraryW(dll.wstring().c_str());
    if (module == nullptr)
    {
        Log("Native script DLL failed to load: " + dll.string(), "error");
        return result;
    }
    // GetProcAddress returns FARPROC; the void* hop silences the function-pointer
    // cast warning without changing behaviour.
    auto* symbol = reinterpret_cast<void*>(GetProcAddress(module, NativeScriptFactoryName));
    auto factory = reinterpret_cast<NativeScriptFactory>(symbol);
    if (factory == nullptr)
    {
        Log("Native script is missing the " + std::string{NativeScriptFactoryName} +
                " export: " + sourceFile.filename().string(),
            "error");
        FreeLibrary(module);
        return result;
    }

    result.Module = module;
    result.Instance = factory();
    if (result.Instance == nullptr)
    {
        Log("Native script factory returned null: " + sourceFile.filename().string(), "error");
        FreeLibrary(module);
        result.Module = nullptr;
    }
#else
    Log("Native C++ scripts are only supported on Windows.", "warn");
    static_cast<void>(sourceFile);
#endif
    return result;
}

std::optional<std::string> NativeScriptLoader::Validate(
    const std::filesystem::path& sourceFile)
{
#ifdef _WIN32
    if (!ToolchainAvailable())
    {
        return std::string{
            "Visual Studio C++ toolchain required (cl.exe not on PATH). "
            "Open a Developer Command Prompt or install VS Build Tools."};
    }
    std::filesystem::path dll;
    std::string output;
    if (!Compile(sourceFile, dll, output))
    {
        if (output.empty()
            || output.find("not recognized") != std::string::npos
            || output.find("is not recognized") != std::string::npos)
        {
            return std::string{
                "Visual Studio C++ toolchain required (cl.exe not on PATH). "
                "Open a Developer Command Prompt or install VS Build Tools."};
        }
        return output;
    }
    std::error_code error;
    std::filesystem::remove(dll, error);
    return std::nullopt;
#else
    static_cast<void>(sourceFile);
    return std::string{"Native C++ scripts are only supported on Windows."};
#endif
}

bool NativeScriptLoader::Compile(
    const std::filesystem::path& sourceFile,
    std::filesystem::path& dll,
    std::string& output) const
{
#ifdef _WIN32
    std::error_code error;
    std::filesystem::create_directories(m_IntermediateDir, error);
    if (error)
    {
        output = "Could not create native script build folder: " + error.message();
        return false;
    }
    dll = m_IntermediateDir / (sourceFile.stem().string() + ".dll");
    std::filesystem::remove(dll, error);

    // Match the host CRT so std::string / new / delete cross the DLL boundary safely.
#ifdef _DEBUG
    const char* runtime = "/MDd /Zi";
#else
    const char* runtime = "/MD";
#endif
    std::ostringstream compile;
    compile << "cl.exe /nologo /LD /std:c++20 /EHsc " << runtime;
    for (const std::filesystem::path& dir : m_IncludeDirs)
    {
        compile << " /I \"" << dir.string() << "\"";
    }
    compile << " \"" << sourceFile.string() << "\""
            << " /Fe:\"" << dll.string() << "\""
            << " /Fo:\"" << m_IntermediateDir.string() << "\\\\\"";

    const std::string command = compile.str() + " 2>&1";
    FILE* pipe = _popen(command.c_str(), "r");
    if (pipe == nullptr)
    {
        output = "Could not start cl.exe.";
        return false;
    }
    char buffer[4096];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr)
    {
        output += buffer;
    }
    const int code = _pclose(pipe);
    return code == 0 && std::filesystem::exists(dll, error) && !error;
#else
    static_cast<void>(sourceFile);
    static_cast<void>(dll);
    output = "Native C++ scripts are only supported on Windows.";
    return false;
#endif
}

void NativeScriptLoader::Unload(Loaded& loaded)
{
    // Destroy the instance before its DLL. Reversing this is a speedrun to undefined behavior.
    delete loaded.Instance;
    loaded.Instance = nullptr;
#ifdef _WIN32
    if (loaded.Module != nullptr)
    {
        FreeLibrary(static_cast<HMODULE>(loaded.Module));
    }
#endif
    loaded.Module = nullptr;
}
}
