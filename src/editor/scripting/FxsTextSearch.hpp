#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace fadix::editor
{
struct FxsSearchFlags
{
    bool MatchCase{false};
    bool WholeWord{false};
    bool Regex{false};
};

struct FxsSearchMatch
{
    std::size_t Start{0};
    std::size_t End{0};
};

struct FxsSearchScan
{
    bool Ok{true};
    std::string Error;
    std::vector<FxsSearchMatch> Matches;
};

/// UTF-8 document search used by smoke tests and the no-HWND edit path.
[[nodiscard]] FxsSearchScan ScanDocument(
    std::string_view text, std::string_view query, FxsSearchFlags flags);

/// Next/prev match index wrapping around. Returns npos if none.
[[nodiscard]] std::size_t NextMatchIndex(
    const std::vector<FxsSearchMatch>& matches,
    std::size_t from,
    bool forward);

/// Replace all non-overlapping matches. Empty query → 0. Invalid regex → Ok=false.
struct FxsReplaceAllResult
{
    bool Ok{true};
    std::string Error;
    std::string Text;
    std::size_t Count{0};
};

[[nodiscard]] FxsReplaceAllResult ReplaceAllInDocument(
    std::string_view text,
    std::string_view query,
    std::string_view replacement,
    FxsSearchFlags flags);
}
