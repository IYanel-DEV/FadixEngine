#include "editor/scripting/FxsApiCatalog.hpp"

#include "scripting/FxsApiNames.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <unordered_set>

namespace fadix::editor
{
namespace
{
namespace names = fadix::fxs;

FxsApiEntry Make(
    const char* name,
    const char* owner,
    FxsCompletionKind kind,
    const char* signature,
    std::vector<FxsApiParam> params,
    const char* ret,
    const char* docs,
    const char* snippet = "")
{
    FxsApiEntry e;
    e.Name = name;
    e.Owner = owner != nullptr ? owner : "";
    e.Kind = kind;
    e.Signature = signature != nullptr ? signature : "";
    e.Parameters = std::move(params);
    e.ReturnType = ret != nullptr ? ret : "";
    e.Documentation = docs != nullptr ? docs : "";
    e.InsertSnippet = snippet != nullptr ? snippet : "";
    return e;
}

bool IsIdentStart(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool IsIdent(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::string ToLower(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// Rank: 0 exact prefix, 1 case-insensitive prefix, 2 fuzzy subsequence, -1 no match.
int MatchRank(std::string_view candidate, std::string_view prefix)
{
    if (prefix.empty())
    {
        return 0;
    }
    if (candidate.size() >= prefix.size() && candidate.substr(0, prefix.size()) == prefix)
    {
        return 0;
    }
    const std::string cl = ToLower(candidate);
    const std::string pl = ToLower(prefix);
    if (cl.size() >= pl.size() && cl.compare(0, pl.size(), pl) == 0)
    {
        return 1;
    }
    std::size_t pi = 0;
    for (char c : cl)
    {
        if (pi < pl.size() && c == pl[pi])
        {
            ++pi;
        }
    }
    return pi == pl.size() ? 2 : -1;
}

std::string DefaultInsert(const FxsApiEntry& e)
{
    if (!e.InsertSnippet.empty())
    {
        return e.InsertSnippet;
    }
    if (e.Kind == FxsCompletionKind::Function || e.Kind == FxsCompletionKind::Snippet)
    {
        if (!e.Parameters.empty())
        {
            return e.Name + "(";
        }
        return e.Name + "()";
    }
    return e.Name;
}

std::vector<FxsApiEntry> BuildCatalog()
{
    std::vector<FxsApiEntry> out;

    // Lifecycle (convention via LuaVM::Call* — not registered as Lua globals).
    out.push_back(Make(names::kOnStart, "", FxsCompletionKind::Snippet,
        "function OnStart(entity)", {{"entity", "Entity"}}, "nil",
        "Called once when the script instance starts.",
        "function OnStart(entity)\n    \nend"));
    out.push_back(Make(names::kOnUpdate, "", FxsCompletionKind::Snippet,
        "function OnUpdate(entity, deltaTime)",
        {{"entity", "Entity"}, {"deltaTime", "number"}}, "nil",
        "Called every frame with elapsed seconds.",
        "function OnUpdate(entity, deltaTime)\n    \nend"));
    out.push_back(Make(names::kOnDestroy, "", FxsCompletionKind::Snippet,
        "function OnDestroy(entity)", {{"entity", "Entity"}}, "nil",
        "Called when the entity is destroyed.",
        "function OnDestroy(entity)\n    \nend"));

    // Entity usertype (LuaVM new_usertype).
    out.push_back(Make(names::kEntityId, names::kEntityType, FxsCompletionKind::Property,
        "entity.id", {}, "number", "entt entity id (uint32)."));
    out.push_back(Make(names::kEntityGetName, names::kEntityType, FxsCompletionKind::Function,
        "entity:getName()", {}, "string", "NameComponent value, or empty."));
    out.push_back(Make(names::kEntityGetPosition, names::kEntityType, FxsCompletionKind::Function,
        "entity:getPosition()", {}, "number, number, number",
        "World position as x, y, z."));
    out.push_back(Make(names::kEntitySetPosition, names::kEntityType, FxsCompletionKind::Function,
        "entity:setPosition(x, y, z)",
        {{"x", "number"}, {"y", "number"}, {"z", "number"}}, "nil",
        "Set TransformComponent position."));
    out.push_back(Make(names::kEntityGetRotation, names::kEntityType, FxsCompletionKind::Function,
        "entity:getRotation()", {}, "number, number, number",
        "Euler rotation in degrees."));
    out.push_back(Make(names::kEntitySetRotation, names::kEntityType, FxsCompletionKind::Function,
        "entity:setRotation(x, y, z)",
        {{"x", "number"}, {"y", "number"}, {"z", "number"}}, "nil",
        "Set rotation from euler degrees."));
    out.push_back(Make(names::kEntityGetScale, names::kEntityType, FxsCompletionKind::Function,
        "entity:getScale()", {}, "number, number, number", "Local scale."));
    out.push_back(Make(names::kEntitySetScale, names::kEntityType, FxsCompletionKind::Function,
        "entity:setScale(x, y, z)",
        {{"x", "number"}, {"y", "number"}, {"z", "number"}}, "nil",
        "Set local scale."));
    out.push_back(Make(names::kEntityDestroy, names::kEntityType, FxsCompletionKind::Function,
        "entity:destroy()", {}, "nil", "Queue entity for destruction."));
    out.push_back(Make(names::kEntityGetTarget, names::kEntityType, FxsCompletionKind::Function,
        "entity:getTarget()", {}, "Entity|nil",
        "ScriptComponent target entity, or nil."));
    out.push_back(Make(names::kEntityMoveCharacter, names::kEntityType, FxsCompletionKind::Function,
        "entity:moveCharacter(x, z)", {{"x", "number"}, {"z", "number"}}, "nil",
        "Move a Character Controller on the ground plane; input length is clamped to one."));
    out.push_back(Make(names::kEntityJumpCharacter, names::kEntityType, FxsCompletionKind::Function,
        "entity:jumpCharacter()", {}, "nil", "Jump when the Character Controller is grounded."));
    out.push_back(Make(names::kEntityIsCharacterGrounded, names::kEntityType,
        FxsCompletionKind::Function, "entity:isCharacterGrounded()", {}, "boolean",
        "True while the Character Controller is standing on walkable ground."));

    out.push_back(Make(names::kPrint, "", FxsCompletionKind::Function,
        "print(...)", {{"...", "any"}}, "nil",
        "Writes to the editor Output panel."));

    out.push_back(Make(names::kInput, "", FxsCompletionKind::Module,
        "Input", {}, "table", "Keyboard input helpers."));
    out.push_back(Make(names::kInputIsDown, names::kInput, FxsCompletionKind::Function,
        "Input.isDown(key)", {{"key", "string"}}, "boolean",
        "True while the named scancode key is held (SDL)."));

    out.push_back(Make(names::kAudio, "", FxsCompletionKind::Module,
        "audio", {}, "table|nil",
        "Audio helpers when an AudioEngine is bound; otherwise nil."));
    out.push_back(Make(names::kAudioLoad, names::kAudio, FxsCompletionKind::Function,
        "audio.load(id, path)", {{"id", "string"}, {"path", "string"}}, "boolean",
        "Load a sound/music asset by id."));
    out.push_back(Make(names::kAudioPlay, names::kAudio, FxsCompletionKind::Function,
        "audio.play(id [, volume])",
        {{"id", "string"}, {"volume", "number?"}}, "boolean",
        "Play a previously loaded id (volume default 1)."));
    out.push_back(Make(names::kAudioStop, names::kAudio, FxsCompletionKind::Function,
        "audio.stop(id)", {{"id", "string"}}, "nil", "Stop playback by id."));
    out.push_back(Make(names::kAudioSetMasterVolume, names::kAudio, FxsCompletionKind::Function,
        "audio.setMasterVolume(volume)", {{"volume", "number"}}, "nil",
        "Set master volume."));
    out.push_back(Make(names::kAudioSetSoundVolume, names::kAudio, FxsCompletionKind::Function,
        "audio.setSoundVolume(volume)", {{"volume", "number"}}, "nil",
        "Set sound (SFX) bus volume."));
    out.push_back(Make(names::kAudioSetMusicVolume, names::kAudio, FxsCompletionKind::Function,
        "audio.setMusicVolume(volume)", {{"volume", "number"}}, "nil",
        "Set music bus volume."));

    // Gameplay world API (LuaVM::BindGameTables).
    out.push_back(Make(names::kWorld, "", FxsCompletionKind::Module,
        "World", {}, "table", "Runtime world queries."));
    out.push_back(Make(names::kWorldFind, names::kWorld, FxsCompletionKind::Function,
        "World.find(name)", {{"name", "string"}}, "Entity|nil",
        "First entity whose NameComponent equals name, or nil."));

    out.push_back(Make(names::kPrefab, "", FxsCompletionKind::Module,
        "Prefab", {}, "table", "Prefab instantiation."));
    out.push_back(Make(names::kPrefabSpawn, names::kPrefab, FxsCompletionKind::Function,
        "Prefab.spawn(path, x, y, z)",
        {{"path", "string"}, {"x", "number"}, {"y", "number"}, {"z", "number"}}, "Entity|nil",
        "Instantiate a .prefab at the position; returns the root entity or nil."));

    out.push_back(Make(names::kScene, "", FxsCompletionKind::Module,
        "Scene", {}, "table", "Scene / level control."));
    out.push_back(Make(names::kSceneLoad, names::kScene, FxsCompletionKind::Function,
        "Scene.load(pathOrName)", {{"pathOrName", "string"}}, "nil",
        "Load a scene at end of tick. Accepts a project-relative path "
        "(\"Scenes/Level2.scene\") or a scene display name (\"Lobby Scene\"); "
        "display-name match is case-insensitive and must be unique."));

    // Libraries actually opened by LuaVM::open_libraries (table names only).
    out.push_back(Make("math", "", FxsCompletionKind::Module, "math", {}, "table",
        "Standard Lua math library (opened by LuaVM)."));
    out.push_back(Make("string", "", FxsCompletionKind::Module, "string", {}, "table",
        "Standard Lua string library (opened by LuaVM)."));
    out.push_back(Make("table", "", FxsCompletionKind::Module, "table", {}, "table",
        "Standard Lua table library (opened by LuaVM)."));
    out.push_back(Make("os", "", FxsCompletionKind::Module, "os", {}, "table",
        "Standard Lua os library (opened by LuaVM)."));

    // Keywords.
    for (const char* kw : {"and", "break", "do", "else", "elseif", "end", "false", "for",
             "function", "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return",
             "then", "true", "until", "while"})
    {
        out.push_back(Make(kw, "", FxsCompletionKind::Keyword, kw, {}, "", "Lua keyword."));
    }

    return out;
}

// Lightweight scan: -- line comments and " ' [[ ]] strings. Not a full lexer.
bool InStringOrCommentAt(std::string_view text, std::size_t cursor)
{
    enum class Mode
    {
        Code,
        LineComment,
        StringDq,
        StringSq,
        LongString,
        LongComment,
    };
    Mode mode = Mode::Code;
    int longEq = 0;
    for (std::size_t i = 0; i < cursor && i < text.size(); ++i)
    {
        const char c = text[i];
        const char n = (i + 1 < text.size()) ? text[i + 1] : '\0';
        switch (mode)
        {
        case Mode::Code:
            if (c == '-' && n == '-')
            {
                if (i + 2 < text.size() && text[i + 2] == '[')
                {
                    int eq = 0;
                    std::size_t j = i + 3;
                    while (j < text.size() && text[j] == '=')
                    {
                        ++eq;
                        ++j;
                    }
                    if (j < text.size() && text[j] == '[')
                    {
                        mode = Mode::LongComment;
                        longEq = eq;
                        i = j;
                        break;
                    }
                }
                mode = Mode::LineComment;
                ++i;
            }
            else if (c == '"' )
            {
                mode = Mode::StringDq;
            }
            else if (c == '\'')
            {
                mode = Mode::StringSq;
            }
            else if (c == '[' && n == '[')
            {
                mode = Mode::LongString;
                longEq = 0;
                ++i;
            }
            else if (c == '[')
            {
                int eq = 0;
                std::size_t j = i + 1;
                while (j < text.size() && text[j] == '=')
                {
                    ++eq;
                    ++j;
                }
                if (j < text.size() && text[j] == '[')
                {
                    mode = Mode::LongString;
                    longEq = eq;
                    i = j;
                }
            }
            break;
        case Mode::LineComment:
            if (c == '\n')
            {
                mode = Mode::Code;
            }
            break;
        case Mode::StringDq:
            if (c == '\\' && i + 1 < cursor)
            {
                ++i;
            }
            else if (c == '"')
            {
                mode = Mode::Code;
            }
            else if (c == '\n')
            {
                mode = Mode::Code;
            }
            break;
        case Mode::StringSq:
            if (c == '\\' && i + 1 < cursor)
            {
                ++i;
            }
            else if (c == '\'')
            {
                mode = Mode::Code;
            }
            else if (c == '\n')
            {
                mode = Mode::Code;
            }
            break;
        case Mode::LongString:
        case Mode::LongComment:
        {
            if (c == ']')
            {
                int eq = 0;
                std::size_t j = i + 1;
                while (j < text.size() && text[j] == '=')
                {
                    ++eq;
                    ++j;
                }
                if (eq == longEq && j < text.size() && text[j] == ']')
                {
                    mode = Mode::Code;
                    i = j;
                }
            }
            break;
        }
        }
    }
    return mode != Mode::Code;
}

std::string NormalizeOwner(std::string_view raw)
{
    const std::string lower = ToLower(raw);
    if (lower == "entity")
    {
        return names::kEntityType;
    }
    if (raw == names::kInput)
    {
        return names::kInput;
    }
    if (raw == names::kAudio)
    {
        return names::kAudio;
    }
    return std::string{raw};
}
}

const std::vector<FxsApiEntry>& FxsApiCatalog::Entries()
{
    static const std::vector<FxsApiEntry> kEntries = BuildCatalog();
    return kEntries;
}

std::vector<std::string> FxsApiCatalog::RequiredPublicApiKeys()
{
    // owner/name keys that must exist — mirrors LuaVM public surface + lifecycle.
    return {
        std::string("/") + names::kOnStart,
        std::string("/") + names::kOnUpdate,
        std::string("/") + names::kOnDestroy,
        std::string(names::kEntityType) + "/" + names::kEntityId,
        std::string(names::kEntityType) + "/" + names::kEntityGetName,
        std::string(names::kEntityType) + "/" + names::kEntityGetPosition,
        std::string(names::kEntityType) + "/" + names::kEntitySetPosition,
        std::string(names::kEntityType) + "/" + names::kEntityGetRotation,
        std::string(names::kEntityType) + "/" + names::kEntitySetRotation,
        std::string(names::kEntityType) + "/" + names::kEntityGetScale,
        std::string(names::kEntityType) + "/" + names::kEntitySetScale,
        std::string(names::kEntityType) + "/" + names::kEntityDestroy,
        std::string(names::kEntityType) + "/" + names::kEntityGetTarget,
        std::string("/") + names::kPrint,
        std::string(names::kInput) + "/" + names::kInputIsDown,
        std::string(names::kAudio) + "/" + names::kAudioLoad,
        std::string(names::kAudio) + "/" + names::kAudioPlay,
        std::string(names::kAudio) + "/" + names::kAudioStop,
        std::string(names::kAudio) + "/" + names::kAudioSetMasterVolume,
        std::string(names::kAudio) + "/" + names::kAudioSetSoundVolume,
        std::string(names::kAudio) + "/" + names::kAudioSetMusicVolume,
        std::string(names::kWorld) + "/" + names::kWorldFind,
        std::string(names::kPrefab) + "/" + names::kPrefabSpawn,
        std::string(names::kScene) + "/" + names::kSceneLoad,
        "/math",
        "/string",
        "/table",
        "/os",
    };
}

bool FxsApiCatalog::ContainsPublicApi(std::string_view owner, std::string_view name)
{
    for (const auto& e : Entries())
    {
        if (e.Owner == owner && e.Name == name)
        {
            return true;
        }
    }
    return false;
}

std::vector<FxsLocalSymbol> FxsApiCatalog::IndexDocumentSymbols(std::string_view text)
{
    std::vector<FxsLocalSymbol> symbols;
    std::unordered_set<std::string> seen;

    auto add = [&](std::string name, FxsCompletionKind kind, const char* docs) {
        if (name.empty() || !seen.insert(name).second)
        {
            return;
        }
        FxsLocalSymbol s;
        s.Name = std::move(name);
        s.Kind = kind;
        s.Documentation = docs;
        symbols.push_back(std::move(s));
    };

    // function Name( or local function Name(
    for (std::size_t i = 0; i < text.size();)
    {
        if (InStringOrCommentAt(text, i))
        {
            ++i;
            continue;
        }
        if (text.compare(i, 8, "function") == 0
            && (i == 0 || !IsIdent(text[i - 1]))
            && i + 8 < text.size() && !IsIdent(text[i + 8]))
        {
            std::size_t j = i + 8;
            while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j])))
            {
                ++j;
            }
            std::size_t start = j;
            while (j < text.size() && (IsIdent(text[j]) || text[j] == '.' || text[j] == ':'))
            {
                ++j;
            }
            if (j > start)
            {
                std::string full{text.substr(start, j - start)};
                const auto colon = full.find_last_of(".:");
                std::string name = colon == std::string::npos ? full : full.substr(colon + 1);
                add(std::move(name), FxsCompletionKind::Function, "Local function");
            }
            // params
            while (j < text.size() && text[j] != '(' && text[j] != '\n')
            {
                ++j;
            }
            if (j < text.size() && text[j] == '(')
            {
                ++j;
                std::size_t p0 = j;
                while (j < text.size() && text[j] != ')' && text[j] != '\n')
                {
                    ++j;
                }
                std::string params{text.substr(p0, j - p0)};
                std::size_t p = 0;
                while (p < params.size())
                {
                    while (p < params.size()
                        && (std::isspace(static_cast<unsigned char>(params[p])) || params[p] == ','))
                    {
                        ++p;
                    }
                    std::size_t ps = p;
                    while (p < params.size() && IsIdent(params[p]))
                    {
                        ++p;
                    }
                    if (p > ps)
                    {
                        add(std::string{params.substr(ps, p - ps)}, FxsCompletionKind::Variable,
                            "Function parameter");
                    }
                }
            }
            i = j;
            continue;
        }
        if (text.compare(i, 5, "local") == 0
            && (i == 0 || !IsIdent(text[i - 1]))
            && i + 5 < text.size() && !IsIdent(text[i + 5]))
        {
            std::size_t j = i + 5;
            while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j])))
            {
                ++j;
            }
            if (text.compare(j, 8, "function") == 0)
            {
                i = j;
                continue;
            }
            while (j < text.size() && text[j] != '\n' && text[j] != ';')
            {
                while (j < text.size()
                    && (std::isspace(static_cast<unsigned char>(text[j])) || text[j] == ','))
                {
                    ++j;
                }
                std::size_t start = j;
                while (j < text.size() && IsIdent(text[j]))
                {
                    ++j;
                }
                if (j > start)
                {
                    add(std::string{text.substr(start, j - start)}, FxsCompletionKind::Variable,
                        "Local variable");
                }
                while (j < text.size() && text[j] != ',' && text[j] != '\n' && text[j] != ';')
                {
                    // skip = expr
                    if (text[j] == '\'' || text[j] == '"' || text[j] == '-')
                    {
                        break;
                    }
                    ++j;
                }
                if (j < text.size() && text[j] == ',')
                {
                    ++j;
                    continue;
                }
                break;
            }
            i = j;
            continue;
        }
        ++i;
    }
    return symbols;
}

FxsApiCatalog::CompletionContext FxsApiCatalog::AnalyzeContext(
    std::string_view document, std::size_t cursor)
{
    CompletionContext ctx;
    cursor = std::min(cursor, document.size());
    ctx.InStringOrComment = InStringOrCommentAt(document, cursor);
    if (ctx.InStringOrComment)
    {
        return ctx;
    }

    std::size_t i = cursor;
    while (i > 0 && IsIdent(document[i - 1]))
    {
        --i;
    }
    ctx.Prefix = std::string{document.substr(i, cursor - i)};

    if (i > 0 && (document[i - 1] == '.' || document[i - 1] == ':'))
    {
        ctx.MemberAccess = true;
        std::size_t end = i - 1;
        std::size_t start = end;
        while (start > 0 && IsIdent(document[start - 1]))
        {
            --start;
        }
        ctx.OwnerFilter = NormalizeOwner(document.substr(start, end - start));
    }
    return ctx;
}

std::vector<FxsSuggestion> FxsApiCatalog::Suggest(const FxsSuggestQuery& query)
{
    std::vector<FxsSuggestion> result;
    const auto ctx = AnalyzeContext(query.Document, query.Cursor);
    if (ctx.InStringOrComment)
    {
        return result;
    }

    const std::string owner =
        !query.OwnerFilter.empty() ? std::string{query.OwnerFilter} : ctx.OwnerFilter;
    const std::string prefix =
        !query.Prefix.empty() ? std::string{query.Prefix} : ctx.Prefix;
    const bool member = query.MemberAccess || ctx.MemberAccess;

    auto pushEntry = [&](const FxsApiEntry& e, int rank) {
        FxsSuggestion s;
        s.Label = e.Name;
        s.InsertText = DefaultInsert(e);
        s.Kind = e.Kind;
        s.Signature = e.Signature;
        s.Documentation = e.Documentation;
        s.Parameters = e.Parameters;
        s.Rank = rank;
        result.push_back(std::move(s));
    };

    for (const auto& e : Entries())
    {
        if (member)
        {
            if (e.Owner != owner)
            {
                continue;
            }
        }
        else
        {
            if (!e.Owner.empty())
            {
                continue;
            }
        }
        const int rank = MatchRank(e.Name, prefix);
        if (rank < 0)
        {
            continue;
        }
        pushEntry(e, rank);
    }

    if (!member)
    {
        for (const auto& sym : IndexDocumentSymbols(query.Document))
        {
            const int rank = MatchRank(sym.Name, prefix);
            if (rank < 0)
            {
                continue;
            }
            FxsSuggestion s;
            s.Label = sym.Name;
            s.InsertText = sym.Kind == FxsCompletionKind::Function ? sym.Name + "(" : sym.Name;
            s.Kind = sym.Kind;
            s.Documentation = sym.Documentation;
            s.Rank = rank + 10; // prefer API/keywords slightly over locals on ties via sort
            result.push_back(std::move(s));
        }
    }

    std::sort(result.begin(), result.end(), [](const FxsSuggestion& a, const FxsSuggestion& b) {
        if (a.Rank != b.Rank)
        {
            return a.Rank < b.Rank;
        }
        return a.Label < b.Label;
    });

    // Dedupe by label keeping best rank.
    std::vector<FxsSuggestion> deduped;
    std::unordered_set<std::string> seen;
    for (auto& s : result)
    {
        if (seen.insert(s.Label).second)
        {
            deduped.push_back(std::move(s));
        }
    }
    return deduped;
}

FxsCallTip FxsApiCatalog::BuildCallTip(
    std::string_view document, std::size_t cursor, const std::vector<FxsSuggestion>&)
{
    FxsCallTip tip;
    cursor = std::min(cursor, document.size());
    if (InStringOrCommentAt(document, cursor))
    {
        return tip;
    }

    // Walk back to matching '(' for the active call.
    int depth = 0;
    std::size_t paren = std::string::npos;
    int argIndex = 0;
    for (std::size_t i = cursor; i > 0;)
    {
        --i;
        if (InStringOrCommentAt(document, i))
        {
            continue;
        }
        const char c = document[i];
        if (c == ')')
        {
            ++depth;
        }
        else if (c == '(')
        {
            if (depth == 0)
            {
                paren = i;
                break;
            }
            --depth;
        }
        else if (c == ',' && depth == 0)
        {
            ++argIndex;
        }
    }
    if (paren == std::string::npos || paren == 0)
    {
        return tip;
    }

    // Identifier before '('.
    std::size_t end = paren;
    while (end > 0 && std::isspace(static_cast<unsigned char>(document[end - 1])))
    {
        --end;
    }
    std::size_t start = end;
    while (start > 0 && IsIdent(document[start - 1]))
    {
        --start;
    }
    const std::string name{document.substr(start, end - start)};
    if (name.empty())
    {
        return tip;
    }

    std::string owner;
    if (start > 0 && (document[start - 1] == '.' || document[start - 1] == ':'))
    {
        std::size_t oend = start - 1;
        std::size_t ostart = oend;
        while (ostart > 0 && IsIdent(document[ostart - 1]))
        {
            --ostart;
        }
        owner = NormalizeOwner(document.substr(ostart, oend - ostart));
    }

    const FxsApiEntry* match = nullptr;
    for (const auto& e : Entries())
    {
        if (e.Name != name)
        {
            continue;
        }
        if (!owner.empty() && e.Owner != owner)
        {
            continue;
        }
        if (owner.empty() && !e.Owner.empty())
        {
            // allow Entity methods when called as getPosition( after colon stripped already
            continue;
        }
        match = &e;
        break;
    }
    // Member method without resolving owner: first function with that name.
    if (match == nullptr)
    {
        for (const auto& e : Entries())
        {
            if (e.Name == name && e.Kind == FxsCompletionKind::Function)
            {
                match = &e;
                break;
            }
        }
    }
    if (match == nullptr || match->Signature.empty())
    {
        return tip;
    }

    tip.Active = true;
    tip.Text = match->Signature;
    if (!match->Documentation.empty())
    {
        tip.Text += "\n";
        tip.Text += match->Documentation;
    }

    // Highlight active parameter inside signature's (...).
    const auto open = tip.Text.find('(');
    const auto close = tip.Text.find(')', open == std::string::npos ? 0 : open);
    if (open != std::string::npos && close != std::string::npos && close > open)
    {
        std::size_t p = open + 1;
        int idx = 0;
        std::size_t argStart = p;
        while (p <= close)
        {
            if (p == close || tip.Text[p] == ',')
            {
                if (idx == argIndex)
                {
                    tip.HighlightStart = static_cast<int>(argStart);
                    tip.HighlightEnd = static_cast<int>(p);
                    break;
                }
                ++idx;
                argStart = p + 1;
                while (argStart < close
                    && std::isspace(static_cast<unsigned char>(tip.Text[argStart])))
                {
                    ++argStart;
                }
            }
            ++p;
        }
    }
    return tip;
}
}
