#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fadix::editor
{
/// One console/output line. UI-neutral — shared by Rml and ImGui editors.
struct OutputEntry
{
    std::string Severity; // "info" | "warn" | "error"
    std::string Time;
    std::string Text;
    std::string ScriptName; // non-empty → clickable diagnostic
    std::size_t Line{0};
    std::size_t Column{0};
};

/// Ring-capped editor log. Not a logging framework — stores UI output entries only.
class EditorLog final
{
public:
    static constexpr std::size_t Cap{500};
    static constexpr std::size_t TrimBatch{100};

    [[nodiscard]] static std::string CurrentTime();

    void Clear();
    void Log(std::string text, std::string_view severity = "info");
    void LogScriptDiagnostic(
        std::string_view scriptName,
        std::size_t line,
        std::size_t column,
        std::string message);
    /// Drop prior clickable diagnostics for this script (keeps other log lines).
    void ClearScriptDiagnostics(std::string_view scriptName);
    void Push(OutputEntry entry);

    [[nodiscard]] std::span<const OutputEntry> Entries() const noexcept;
    [[nodiscard]] const OutputEntry* At(std::size_t index) const noexcept;

private:
    void Trim();

    std::vector<OutputEntry> m_Entries;
};
}
