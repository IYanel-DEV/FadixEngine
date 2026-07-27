#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fadix::editor
{
struct FxsParsedDiagnostic
{
    bool Ok{false};
    bool HasLocation{false};
    std::string File; // may be empty
    std::size_t Line{1};
    std::size_t Column{1};
    std::string Message;
};

struct FxsTextRange
{
    std::size_t Start{0};
    std::size_t End{0};
};

/// Parse Lua `load`/compile error text into file/line/column/message.
/// Does not treat unresolved globals as errors — those never appear in compile output.
[[nodiscard]] FxsParsedDiagnostic ParseLuaCompileError(std::string_view errorText);

/// UTF-8-ish byte offsets for a 1-based line/column (column clamped to line).
[[nodiscard]] FxsTextRange RangeForLineColumn(
    std::string_view text, std::size_t line, std::size_t column);

/// Squiggle from column to end of token/line (compile errors rarely give an end).
[[nodiscard]] FxsTextRange SquiggleRangeForLineColumn(
    std::string_view text, std::size_t line, std::size_t column);

[[nodiscard]] std::string TrimTrailingWhitespace(std::string_view text);

/// Toggle `-- ` (Lua) or `// ` (C++) on each selected line. Returns new text + caret hints.
struct FxsCommentToggleResult
{
    std::string Text;
    std::size_t Anchor{0};
    std::size_t Caret{0};
};
[[nodiscard]] FxsCommentToggleResult ToggleLineComments(
    std::string_view text,
    std::size_t anchor,
    std::size_t caret,
    std::string_view lineCommentPrefix);

/// True when typing `open` should also insert `close` (next char is not already `close`).
[[nodiscard]] bool ShouldAutoClose(
    std::string_view text, std::size_t caret, char open, char close);

/// Indent string for one level given tab width / use-tabs.
[[nodiscard]] std::string IndentUnit(int tabWidth, bool useTabs);

/// Leading whitespace of the line containing `offset`.
[[nodiscard]] std::string LineIndentAt(std::string_view text, std::size_t offset);
}
