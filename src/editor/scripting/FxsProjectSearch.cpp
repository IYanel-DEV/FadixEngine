#include "editor/scripting/FxsProjectSearch.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace fadix::editor
{
namespace
{
bool IsScriptPath(const std::filesystem::path& path)
{
    const auto ext = path.extension().string();
    return ext == ".lua" || ext == ".fxs" || ext == ".cpp" || ext == ".hpp" || ext == ".h"
        || ext == ".cc" || ext == ".cxx";
}

std::string SnippetAround(const std::string& text, const std::size_t start, const std::size_t end)
{
    std::size_t lineStart = start;
    while (lineStart > 0 && text[lineStart - 1] != '\n')
    {
        --lineStart;
    }
    std::size_t lineEnd = end;
    while (lineEnd < text.size() && text[lineEnd] != '\n')
    {
        ++lineEnd;
    }
    std::string snip = text.substr(lineStart, lineEnd - lineStart);
    if (snip.size() > 120)
    {
        snip.resize(117);
        snip += "...";
    }
    return snip;
}

std::pair<std::size_t, std::size_t> LineColumnAt(const std::string& text, const std::size_t offset)
{
    std::size_t line = 1;
    std::size_t col = 1;
    for (std::size_t i = 0; i < offset && i < text.size(); ++i)
    {
        if (text[i] == '\n')
        {
            ++line;
            col = 1;
        }
        else
        {
            ++col;
        }
    }
    return {line, col};
}
}

FxsProjectSearch::FxsProjectSearch() = default;

FxsProjectSearch::~FxsProjectSearch()
{
    Cancel();
    if (m_Thread.joinable())
    {
        m_Thread.join();
    }
}

void FxsProjectSearch::Start(FxsProjectSearchRequest request)
{
    Cancel();
    if (m_Thread.joinable())
    {
        m_Thread.join();
    }
    {
        std::lock_guard lock{m_Mutex};
        m_Hits.clear();
        m_Consumed = 0;
        m_Status = "Searching…";
    }
    m_Cancel = false;
    m_State = FxsProjectSearchState::Running;
    m_Thread = std::thread([this, req = std::move(request)]() mutable { WorkerMain(std::move(req)); });
}

void FxsProjectSearch::Cancel()
{
    m_Cancel = true;
}

FxsProjectSearchState FxsProjectSearch::State() const noexcept
{
    return m_State.load();
}

std::vector<FxsProjectHit> FxsProjectSearch::TakeNewHits()
{
    std::lock_guard lock{m_Mutex};
    if (m_Consumed >= m_Hits.size())
    {
        return {};
    }
    std::vector<FxsProjectHit> out(m_Hits.begin() + static_cast<std::ptrdiff_t>(m_Consumed), m_Hits.end());
    m_Consumed = m_Hits.size();
    return out;
}

std::vector<FxsProjectHit> FxsProjectSearch::SnapshotHits() const
{
    std::lock_guard lock{m_Mutex};
    return m_Hits;
}

std::size_t FxsProjectSearch::HitCount() const
{
    std::lock_guard lock{m_Mutex};
    return m_Hits.size();
}

std::string FxsProjectSearch::Status() const
{
    std::lock_guard lock{m_Mutex};
    return m_Status;
}

void FxsProjectSearch::WorkerMain(FxsProjectSearchRequest request)
{
    if (request.Query.empty())
    {
        std::lock_guard lock{m_Mutex};
        m_Status = "Empty query";
        m_State = FxsProjectSearchState::Completed;
        return;
    }

    std::size_t files = 0;
    for (const auto& root : request.Roots)
    {
        if (m_Cancel)
        {
            break;
        }
        std::error_code error;
        if (!std::filesystem::exists(root, error))
        {
            continue;
        }
        const auto options = std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::recursive_directory_iterator it{root, options, error}, end;
            it != end && !m_Cancel; it.increment(error))
        {
            if (error)
            {
                error.clear();
                continue;
            }
            if (!it->is_regular_file(error))
            {
                continue;
            }
            const auto& path = it->path();
            if (request.ScriptsOnly && !IsScriptPath(path))
            {
                continue;
            }
            SearchFile(path, request);
            ++files;
            if ((files % 8) == 0)
            {
                std::lock_guard lock{m_Mutex};
                m_Status = "Searched " + std::to_string(files) + " files, "
                    + std::to_string(m_Hits.size()) + " hits";
            }
        }
    }

    std::lock_guard lock{m_Mutex};
    if (m_Cancel)
    {
        m_Status = "Cancelled (" + std::to_string(m_Hits.size()) + " hits)";
        m_State = FxsProjectSearchState::Cancelled;
    }
    else
    {
        m_Status = "Done — " + std::to_string(m_Hits.size()) + " hits in "
            + std::to_string(files) + " files";
        m_State = FxsProjectSearchState::Completed;
    }
}

void FxsProjectSearch::SearchFile(
    const std::filesystem::path& path, const FxsProjectSearchRequest& request)
{
    std::ifstream in{path, std::ios::binary};
    if (!in)
    {
        return;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());

    const auto scan = ScanDocument(text, request.Query, request.Flags);
    if (!scan.Ok)
    {
        return;
    }
    const std::string name = path.stem().string();
    std::lock_guard lock{m_Mutex};
    for (const auto& match : scan.Matches)
    {
        if (m_Cancel)
        {
            return;
        }
        FxsProjectHit hit;
        hit.ScriptName = name;
        hit.Path = path;
        const auto [line, col] = LineColumnAt(text, match.Start);
        hit.Line = line;
        hit.Column = col;
        hit.Snippet = SnippetAround(text, match.Start, match.End);
        hit.MatchStart = match.Start;
        hit.MatchEnd = match.End;
        m_Hits.push_back(std::move(hit));
    }
}

std::vector<std::string> FxsProjectSearch::PreviewReplace(
    const std::vector<FxsProjectHit>& hits,
    const std::string_view replacement,
    const std::size_t maxItems)
{
    std::vector<std::string> out;
    out.reserve(std::min(hits.size(), maxItems));
    for (std::size_t i = 0; i < hits.size() && out.size() < maxItems; ++i)
    {
        const auto& h = hits[i];
        out.push_back(h.Path.filename().string() + ':' + std::to_string(h.Line) + "  "
            + h.Snippet + "  →  …" + std::string{replacement} + "…");
    }
    return out;
}

std::size_t FxsProjectSearch::ApplyReplaceInFiles(
    const std::vector<FxsProjectHit>& hits,
    const std::string_view query,
    const std::string_view replacement,
    const FxsSearchFlags flags)
{
    std::unordered_map<std::string, std::vector<FxsProjectHit>> byPath;
    for (const auto& h : hits)
    {
        byPath[h.Path.string()].push_back(h);
    }
    std::size_t files = 0;
    for (auto& [pathStr, fileHits] : byPath)
    {
        const std::filesystem::path path{pathStr};
        std::ifstream in{path, std::ios::binary};
        if (!in)
        {
            continue;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        std::string text = buffer.str();
        const auto result = ReplaceAllInDocument(text, query, replacement, flags);
        if (!result.Ok || result.Count == 0)
        {
            continue;
        }
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        if (!out)
        {
            continue;
        }
        out << result.Text;
        ++files;
    }
    return files;
}
}
