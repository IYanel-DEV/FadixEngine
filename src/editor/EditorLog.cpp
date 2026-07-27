#include "editor/EditorLog.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <sstream>

namespace fadix::editor
{
std::string EditorLog::CurrentTime()
{
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream time;
    time << (local.tm_hour < 10 ? "0" : "") << local.tm_hour << ':'
         << (local.tm_min < 10 ? "0" : "") << local.tm_min << ':'
         << (local.tm_sec < 10 ? "0" : "") << local.tm_sec;
    return time.str();
}

void EditorLog::Clear()
{
    m_Entries.clear();
}

void EditorLog::Log(std::string text, const std::string_view severity)
{
    OutputEntry entry;
    entry.Severity = severity.empty() ? "info" : std::string{severity};
    entry.Time = CurrentTime();
    entry.Text = std::move(text);
    Push(std::move(entry));
}

void EditorLog::LogScriptDiagnostic(
    const std::string_view scriptName,
    const std::size_t line,
    const std::size_t column,
    std::string message)
{
    ClearScriptDiagnostics(scriptName);
    OutputEntry entry;
    const bool toolchainMissing =
        message.find("cl.exe") != std::string::npos
        || message.find("toolchain") != std::string::npos
        || message.find("Developer Command Prompt") != std::string::npos;
    entry.Severity = toolchainMissing ? "warn" : "error";
    entry.Time = CurrentTime();
    entry.ScriptName = std::string{scriptName};
    entry.Line = line;
    entry.Column = column;
    entry.Text = entry.ScriptName + ':' + std::to_string(line) + ':' + std::to_string(column) +
        ": " + std::move(message);
    Push(std::move(entry));
}

void EditorLog::ClearScriptDiagnostics(const std::string_view scriptName)
{
    if (scriptName.empty())
    {
        return;
    }
    m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
                        [&](const OutputEntry& e) {
                            return !e.ScriptName.empty() && e.ScriptName == scriptName;
                        }),
        m_Entries.end());
}

void EditorLog::Push(OutputEntry entry)
{
    if (entry.Severity.empty())
    {
        entry.Severity = "info";
    }
    if (entry.Time.empty())
    {
        entry.Time = CurrentTime();
    }
    m_Entries.push_back(std::move(entry));
    Trim();
}

std::span<const OutputEntry> EditorLog::Entries() const noexcept
{
    return m_Entries;
}

const OutputEntry* EditorLog::At(const std::size_t index) const noexcept
{
    return index < m_Entries.size() ? &m_Entries[index] : nullptr;
}

void EditorLog::Trim()
{
    if (m_Entries.size() > Cap)
    {
        m_Entries.erase(m_Entries.begin(), m_Entries.begin() + static_cast<std::ptrdiff_t>(TrimBatch));
    }
}
}
