#include "editor/scripting/FxsDiagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace fadix::editor
{
namespace
{
bool IsIdentChar(const char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::size_t LineStartOffset(const std::string_view text, const std::size_t line)
{
    std::size_t at = 1;
    std::size_t i = 0;
    while (i < text.size() && at < line)
    {
        if (text[i] == '\n')
        {
            ++at;
        }
        ++i;
    }
    return i;
}

std::size_t LineEndOffset(const std::string_view text, const std::size_t start)
{
    std::size_t i = start;
    while (i < text.size() && text[i] != '\n')
    {
        ++i;
    }
    return i;
}
}

FxsParsedDiagnostic ParseLuaCompileError(const std::string_view errorText)
{
    FxsParsedDiagnostic out;
    std::string_view text = errorText;
    while (!text.empty()
        && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'
            || text.front() == '\n'))
    {
        text.remove_prefix(1);
    }
    if (text.empty())
    {
        return out;
    }

    // Take first line only.
    if (const auto nl = text.find('\n'); nl != std::string_view::npos)
    {
        text = text.substr(0, nl);
    }
    if (!text.empty() && text.back() == '\r')
    {
        text.remove_suffix(1);
    }

    std::string_view rest = text;
    // [@chunk] or [string "..."]:
    if (!rest.empty() && rest.front() == '[')
    {
        const auto close = rest.find(']');
        if (close != std::string_view::npos)
        {
            out.File = std::string{rest.substr(1, close - 1)};
            if (out.File.rfind("string \"", 0) == 0 && out.File.size() >= 9
                && out.File.back() == '"')
            {
                out.File = out.File.substr(8, out.File.size() - 9);
            }
            if (!out.File.empty() && out.File.front() == '@')
            {
                out.File.erase(out.File.begin());
            }
            rest.remove_prefix(close + 1);
            if (!rest.empty() && rest.front() == ':')
            {
                rest.remove_prefix(1);
            }
        }
    }
    else
    {
        // path:line: message  or  path:line:column: message
        const auto firstColon = rest.find(':');
        if (firstColon != std::string_view::npos
            && firstColon + 1 < rest.size()
            && std::isdigit(static_cast<unsigned char>(rest[firstColon + 1])) != 0)
        {
            out.File = std::string{rest.substr(0, firstColon)};
            if (!out.File.empty() && out.File.front() == '@')
            {
                out.File.erase(out.File.begin());
            }
            rest.remove_prefix(firstColon + 1);
        }
    }

    // line[:column]: message
    std::size_t i = 0;
    while (i < rest.size() && std::isdigit(static_cast<unsigned char>(rest[i])) != 0)
    {
        ++i;
    }
    if (i == 0)
    {
        out.Message = std::string{text};
        out.Ok = true;
        out.HasLocation = false;
        return out;
    }
    out.HasLocation = true;
    out.Line = static_cast<std::size_t>(std::stoull(std::string{rest.substr(0, i)}));
    rest.remove_prefix(i);
    if (!rest.empty() && rest.front() == ':')
    {
        rest.remove_prefix(1);
        std::size_t j = 0;
        while (j < rest.size() && std::isdigit(static_cast<unsigned char>(rest[j])) != 0)
        {
            ++j;
        }
        // Ambiguous: "3: unexpected" vs "3:10: msg". Prefer column only when
        // digits are followed by another colon.
        if (j > 0 && j < rest.size() && rest[j] == ':')
        {
            out.Column = static_cast<std::size_t>(std::stoull(std::string{rest.substr(0, j)}));
            rest.remove_prefix(j + 1);
        }
    }
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
    {
        rest.remove_prefix(1);
    }
    out.Message = rest.empty() ? std::string{text} : std::string{rest};
    out.Ok = true;
    if (out.Line == 0)
    {
        out.Line = 1;
    }
    if (out.Column == 0)
    {
        out.Column = 1;
    }
    return out;
}

FxsTextRange RangeForLineColumn(
    const std::string_view text, const std::size_t line, const std::size_t column)
{
    const std::size_t start = LineStartOffset(text, line == 0 ? 1 : line);
    const std::size_t end = LineEndOffset(text, start);
    const std::size_t col = column == 0 ? 1 : column;
    std::size_t at = start;
    for (std::size_t c = 1; c < col && at < end; ++c)
    {
        ++at;
    }
    return {at, at < end ? at + 1 : at};
}

FxsTextRange SquiggleRangeForLineColumn(
    const std::string_view text, const std::size_t line, const std::size_t column)
{
    const auto point = RangeForLineColumn(text, line, column);
    const std::size_t lineEnd = LineEndOffset(text, point.Start);
    std::size_t end = point.Start;
    if (end < lineEnd && IsIdentChar(text[end]))
    {
        while (end < lineEnd && IsIdentChar(text[end]))
        {
            ++end;
        }
    }
    else if (end < lineEnd)
    {
        ++end;
        while (end < lineEnd && std::isspace(static_cast<unsigned char>(text[end])) == 0
            && IsIdentChar(text[end]) == 0)
        {
            // one punctuation token
            break;
        }
    }
    if (end <= point.Start)
    {
        end = (point.Start < lineEnd) ? point.Start + 1 : point.Start;
    }
    // Prefer at least to end of line when column is past content.
    if (point.Start >= lineEnd && lineEnd > LineStartOffset(text, line == 0 ? 1 : line))
    {
        const std::size_t ls = LineStartOffset(text, line == 0 ? 1 : line);
        return {ls, lineEnd};
    }
    return {point.Start, end};
}

std::string TrimTrailingWhitespace(const std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size())
    {
        const std::size_t lineStart = i;
        while (i < text.size() && text[i] != '\n')
        {
            ++i;
        }
        std::size_t lineEnd = i;
        while (lineEnd > lineStart
            && (text[lineEnd - 1] == ' ' || text[lineEnd - 1] == '\t'
                || text[lineEnd - 1] == '\r'))
        {
            --lineEnd;
        }
        out.append(text.data() + lineStart, lineEnd - lineStart);
        if (i < text.size() && text[i] == '\n')
        {
            out.push_back('\n');
            ++i;
        }
    }
    return out;
}

FxsCommentToggleResult ToggleLineComments(
    const std::string_view text,
    const std::size_t anchor,
    const std::size_t caret,
    const std::string_view lineCommentPrefix)
{
    FxsCommentToggleResult result;
    const std::size_t a = std::min(anchor, caret);
    const std::size_t b = std::max(anchor, caret);
    std::size_t lineStart = a;
    while (lineStart > 0 && text[lineStart - 1] != '\n')
    {
        --lineStart;
    }
    std::size_t lineEnd = b;
    if (lineEnd > lineStart && lineEnd < text.size() && text[lineEnd - 1] == '\n')
    {
        --lineEnd;
    }
    while (lineEnd < text.size() && text[lineEnd] != '\n')
    {
        ++lineEnd;
    }

    const std::string prefix{lineCommentPrefix};
    std::string body{text.substr(lineStart, lineEnd - lineStart)};
    std::vector<std::pair<std::size_t, std::size_t>> lines;
    std::size_t p = 0;
    while (p <= body.size())
    {
        const std::size_t nl = body.find('\n', p);
        const std::size_t end = nl == std::string::npos ? body.size() : nl;
        lines.push_back({p, end});
        if (nl == std::string::npos)
        {
            break;
        }
        p = nl + 1;
    }

    bool allCommented = false;
    bool anyCodeLine = false;
    for (const auto& [ls, le] : lines)
    {
        if (ls == le)
        {
            continue;
        }
        anyCodeLine = true;
        std::size_t i = ls;
        while (i < le && (body[i] == ' ' || body[i] == '\t'))
        {
            ++i;
        }
        if (i < le && body.compare(i, prefix.size(), prefix) == 0)
        {
            allCommented = true;
        }
        else
        {
            allCommented = false;
            break;
        }
    }
    if (!anyCodeLine)
    {
        allCommented = false;
    }

    std::string rebuilt;
    rebuilt.reserve(body.size() + lines.size() * prefix.size());
    for (std::size_t li = 0; li < lines.size(); ++li)
    {
        const auto [ls, le] = lines[li];
        std::string line = body.substr(ls, le - ls);
        if (ls != le)
        {
            std::size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            {
                ++i;
            }
            if (allCommented)
            {
                if (i + prefix.size() <= line.size() && line.compare(i, prefix.size(), prefix) == 0)
                {
                    line.erase(i, prefix.size());
                    if (i < line.size() && line[i] == ' ')
                    {
                        line.erase(i, 1);
                    }
                }
            }
            else
            {
                line.insert(i, prefix);
                if (prefix.back() != ' ')
                {
                    line.insert(i + prefix.size(), " ");
                }
            }
        }
        rebuilt += line;
        if (li + 1 < lines.size())
        {
            rebuilt.push_back('\n');
        }
    }

    result.Text = std::string{text.substr(0, lineStart)} + rebuilt + std::string{text.substr(lineEnd)};
    result.Anchor = lineStart;
    result.Caret = lineStart + rebuilt.size();
    return result;
}

bool ShouldAutoClose(
    const std::string_view text, const std::size_t caret, const char /*open*/, const char close)
{
    if (caret < text.size() && text[caret] == close)
    {
        return false;
    }
    return true;
}

std::string IndentUnit(const int tabWidth, const bool useTabs)
{
    if (useTabs)
    {
        return "\t";
    }
    return std::string(static_cast<std::size_t>(std::max(1, tabWidth)), ' ');
}

std::string LineIndentAt(const std::string_view text, const std::size_t offset)
{
    std::size_t start = std::min(offset, text.size());
    while (start > 0 && text[start - 1] != '\n')
    {
        --start;
    }
    std::size_t i = start;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
    {
        ++i;
    }
    return std::string{text.substr(start, i - start)};
}
}
