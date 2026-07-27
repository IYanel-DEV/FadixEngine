#include "editor/scripting/FxsTextSearch.hpp"

#include <cctype>
#include <regex>

namespace fadix::editor
{
namespace
{
bool IsWordChar(const unsigned char c)
{
    return std::isalnum(c) != 0 || c == '_';
}

bool IsWholeWordAt(const std::string_view text, const std::size_t start, const std::size_t end)
{
    if (start > end || end > text.size())
    {
        return false;
    }
    if (start > 0 && IsWordChar(static_cast<unsigned char>(text[start - 1])))
    {
        return false;
    }
    if (end < text.size() && IsWordChar(static_cast<unsigned char>(text[end])))
    {
        return false;
    }
    return true;
}

std::regex_constants::syntax_option_type RegexOptions(const FxsSearchFlags flags)
{
    auto opts = std::regex_constants::ECMAScript;
    if (!flags.MatchCase)
    {
        opts |= std::regex_constants::icase;
    }
    return opts;
}
}

FxsSearchScan ScanDocument(
    const std::string_view text, const std::string_view query, const FxsSearchFlags flags)
{
    FxsSearchScan scan;
    if (query.empty())
    {
        return scan;
    }

    if (flags.Regex)
    {
        try
        {
            const std::regex re{std::string{query}, RegexOptions(flags)};
            const std::cregex_iterator begin{text.data(), text.data() + text.size(), re};
            const std::cregex_iterator end{};
            for (auto it = begin; it != end; ++it)
            {
                const auto& m = *it;
                const std::size_t start = static_cast<std::size_t>(m.position());
                const std::size_t len = static_cast<std::size_t>(m.length());
                if (flags.WholeWord && !IsWholeWordAt(text, start, start + len))
                {
                    continue;
                }
                scan.Matches.push_back(FxsSearchMatch{start, start + len});
            }
        }
        catch (const std::regex_error& ex)
        {
            scan.Ok = false;
            scan.Error = ex.what();
        }
        return scan;
    }

    const auto equals = [flags](const char a, const char b) {
        if (flags.MatchCase)
        {
            return a == b;
        }
        return std::tolower(static_cast<unsigned char>(a)) ==
            std::tolower(static_cast<unsigned char>(b));
    };

    for (std::size_t i = 0; i + query.size() <= text.size(); ++i)
    {
        bool match = true;
        for (std::size_t j = 0; j < query.size(); ++j)
        {
            if (!equals(text[i + j], query[j]))
            {
                match = false;
                break;
            }
        }
        if (!match)
        {
            continue;
        }
        if (flags.WholeWord && !IsWholeWordAt(text, i, i + query.size()))
        {
            continue;
        }
        scan.Matches.push_back(FxsSearchMatch{i, i + query.size()});
        // Non-overlapping for highlight count consistency with replace-all.
        i += query.size() - 1;
    }
    return scan;
}

std::size_t NextMatchIndex(
    const std::vector<FxsSearchMatch>& matches, const std::size_t from, const bool forward)
{
    if (matches.empty())
    {
        return static_cast<std::size_t>(-1);
    }
    if (forward)
    {
        for (std::size_t i = 0; i < matches.size(); ++i)
        {
            if (matches[i].Start >= from)
            {
                return i;
            }
        }
        return 0; // wrap
    }
    // Caret is usually at match End after Find; require End < from so the current
    // match is skipped (Start < from would re-select it).
    for (std::size_t i = matches.size(); i-- > 0;)
    {
        if (matches[i].End < from)
        {
            return i;
        }
    }
    return matches.size() - 1; // wrap
}

FxsReplaceAllResult ReplaceAllInDocument(
    const std::string_view text,
    const std::string_view query,
    const std::string_view replacement,
    const FxsSearchFlags flags)
{
    FxsReplaceAllResult result;
    result.Text.assign(text.begin(), text.end());
    if (query.empty())
    {
        return result;
    }
    const FxsSearchScan scan = ScanDocument(text, query, flags);
    if (!scan.Ok)
    {
        result.Ok = false;
        result.Error = scan.Error;
        return result;
    }
    // Apply from end so offsets stay valid.
    for (std::size_t i = scan.Matches.size(); i-- > 0;)
    {
        const FxsSearchMatch& m = scan.Matches[i];
        result.Text.replace(m.Start, m.End - m.Start, replacement);
        ++result.Count;
    }
    return result;
}
}
