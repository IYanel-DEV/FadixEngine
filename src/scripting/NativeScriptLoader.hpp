#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fadix
{
class NativeScript;

// Compiles a single C++ script file into a DLL with cl.exe, then loads the
// exported FadixCreateScript factory and returns an owned NativeScript*.
//
// Windows-only and it needs a Visual Studio developer environment (cl.exe on
// PATH, INCLUDE/LIB set) plus the engine + EnTT + glm include dirs passed via
// SetIncludeDirs. Missing toolchain or headers make CompileAndLoad return a
// null Instance (reported through the logger); the Lua path is unaffected.
class NativeScriptLoader
{
public:
    using Logger = std::function<void(const std::string& message, const char* severity)>;

    struct Loaded
    {
        void* Module{nullptr};           // HMODULE, freed on Unload
        NativeScript* Instance{nullptr}; // owned, deleted on Unload
    };

    explicit NativeScriptLoader(std::filesystem::path intermediateDir);
    ~NativeScriptLoader();

    void SetLogger(Logger logger);
    void SetIncludeDirs(std::vector<std::filesystem::path> includeDirs);

    [[nodiscard]] Loaded CompileAndLoad(const std::filesystem::path& sourceFile);
    // Compile without loading or executing the script. Empty means success.
    [[nodiscard]] std::optional<std::string> Validate(
        const std::filesystem::path& sourceFile);
    void Unload(Loaded& loaded);

    // True when cl.exe is reachable on PATH.
    [[nodiscard]] static bool ToolchainAvailable();

private:
    [[nodiscard]] bool Compile(
        const std::filesystem::path& sourceFile,
        std::filesystem::path& dll,
        std::string& output) const;
    void Log(const std::string& message, const char* severity) const;

    std::filesystem::path m_IntermediateDir;
    std::vector<std::filesystem::path> m_IncludeDirs;
    Logger m_Logger;
};
}
