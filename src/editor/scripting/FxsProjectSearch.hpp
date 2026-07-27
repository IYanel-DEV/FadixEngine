#pragma once

#include "editor/scripting/FxsTextSearch.hpp"

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fadix::editor
{
struct FxsProjectHit
{
    std::string ScriptName;
    std::filesystem::path Path;
    std::size_t Line{1};
    std::size_t Column{1};
    std::string Snippet;
    std::size_t MatchStart{0}; // byte offset in file
    std::size_t MatchEnd{0};
};

struct FxsProjectSearchRequest
{
    std::string Query;
    FxsSearchFlags Flags{};
    std::vector<std::filesystem::path> Roots; // empty → caller fills Scripts/
    bool ScriptsOnly{true};
};

enum class FxsProjectSearchState : std::uint8_t
{
    Idle = 0,
    Running,
    Completed,
    Cancelled,
};

/// Background project text search. Poll from UI; Cancel is cooperative.
class FxsProjectSearch
{
public:
    FxsProjectSearch();
    ~FxsProjectSearch();

    FxsProjectSearch(const FxsProjectSearch&) = delete;
    FxsProjectSearch& operator=(const FxsProjectSearch&) = delete;

    void Start(FxsProjectSearchRequest request);
    void Cancel();

    [[nodiscard]] FxsProjectSearchState State() const noexcept;
    [[nodiscard]] std::vector<FxsProjectHit> TakeNewHits();
    [[nodiscard]] std::vector<FxsProjectHit> SnapshotHits() const;
    [[nodiscard]] std::size_t HitCount() const;
    [[nodiscard]] std::string Status() const;

    /// Build replace preview from current hits (does not write disk).
    [[nodiscard]] static std::vector<std::string> PreviewReplace(
        const std::vector<FxsProjectHit>& hits,
        std::string_view replacement,
        std::size_t maxItems = 50);

    /// Apply replacements to files. Requires prior confirmation. Returns files touched.
    [[nodiscard]] static std::size_t ApplyReplaceInFiles(
        const std::vector<FxsProjectHit>& hits,
        std::string_view query,
        std::string_view replacement,
        FxsSearchFlags flags);

private:
    void WorkerMain(FxsProjectSearchRequest request);
    void SearchFile(const std::filesystem::path& path, const FxsProjectSearchRequest& request);

    mutable std::mutex m_Mutex;
    std::thread m_Thread;
    std::atomic<bool> m_Cancel{false};
    std::atomic<FxsProjectSearchState> m_State{FxsProjectSearchState::Idle};
    std::vector<FxsProjectHit> m_Hits;
    std::size_t m_Consumed{0};
    std::string m_Status;
};
}
