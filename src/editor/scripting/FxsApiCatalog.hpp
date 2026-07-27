#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fadix::editor
{
enum class FxsCompletionKind : std::uint8_t
{
    Function = 1,
    Property = 2,
    Variable = 3,
    Keyword = 4,
    Snippet = 5,
    Module = 6,
};

struct FxsApiParam
{
    std::string Name;
    std::string Type;
};

struct FxsApiEntry
{
    std::string Name;
    std::string Owner; // empty = global; "Entity" / "Input" / "audio"
    FxsCompletionKind Kind{FxsCompletionKind::Function};
    std::string Signature;
    std::vector<FxsApiParam> Parameters;
    std::string ReturnType;
    std::string Documentation;
    std::string InsertSnippet; // empty → insert Name (or Name() for functions)
};

struct FxsLocalSymbol
{
    std::string Name;
    FxsCompletionKind Kind{FxsCompletionKind::Variable};
    std::string Documentation;
};

struct FxsSuggestQuery
{
    std::string_view Document;
    std::size_t Cursor{0};
    std::string_view Prefix;     // text after last . : or start of ident
    std::string_view OwnerFilter; // "Entity" / "Input" / "audio" / ""
    bool MemberAccess{false};    // true after . or :
    bool Force{false};           // Ctrl+Space
};

struct FxsSuggestion
{
    std::string Label;
    std::string InsertText;
    FxsCompletionKind Kind{FxsCompletionKind::Function};
    std::string Signature;
    std::string Documentation;
    std::vector<FxsApiParam> Parameters;
    int Rank{0}; // lower = better
};

struct FxsCallTip
{
    bool Active{false};
    std::string Text;
    int HighlightStart{0};
    int HighlightEnd{0};
};

/// Static FXS API catalog + lightweight document symbol scan (not a Lua parser).
class FxsApiCatalog
{
public:
    [[nodiscard]] static const std::vector<FxsApiEntry>& Entries();

    /// Names that must appear in the catalog (public FXS surface).
    [[nodiscard]] static std::vector<std::string> RequiredPublicApiKeys();

    [[nodiscard]] static bool ContainsPublicApi(std::string_view owner, std::string_view name);

    [[nodiscard]] static std::vector<FxsLocalSymbol> IndexDocumentSymbols(std::string_view text);

    /// Context before `cursor`: owner filter, prefix, whether inside string/comment (Lua-ish).
    struct CompletionContext
    {
        std::string OwnerFilter;
        std::string Prefix;
        bool MemberAccess{false};
        bool InStringOrComment{false};
    };
    [[nodiscard]] static CompletionContext AnalyzeContext(std::string_view document, std::size_t cursor);

    [[nodiscard]] static std::vector<FxsSuggestion> Suggest(const FxsSuggestQuery& query);

    [[nodiscard]] static FxsCallTip BuildCallTip(
        std::string_view document, std::size_t cursor, const std::vector<FxsSuggestion>& fallbackCatalog);
};
}
