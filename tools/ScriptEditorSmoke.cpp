#include "assets/ScriptDatabase.hpp"
#include "editor/scripting/FxsApiCatalog.hpp"
#include "editor/scripting/FxsDiagnostics.hpp"
#include "editor/scripting/FxsEditorSession.hpp"
#include "editor/scripting/FxsEditorSettings.hpp"
#include "editor/scripting/FxsProjectSearch.hpp"
#include "editor/scripting/FxsTextSearch.hpp"
#include "editor/scripting/ScriptEditorController.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <unordered_set>

namespace
{
int g_Failures = 0;

bool Check(const bool condition, const char* label)
{
    std::cout << (condition ? "  ok   " : "  FAIL ") << label << '\n';
    if (!condition)
    {
        ++g_Failures;
    }
    return condition;
}
}

int main()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "fadix_script_editor_smoke";
    std::error_code error;
    std::filesystem::remove_all(folder, error);

    fadix::ScriptDatabase database;
    database.ScanFolder(folder);
    if (!Check(database.CreateNew("EditorProof", fadix::ScriptLanguage::Lua) != nullptr,
            "create source file"))
    {
        return 1;
    }
    if (!Check(database.CreateNew("NativeProof", fadix::ScriptLanguage::Cpp) != nullptr,
            "create cpp source file"))
    {
        return 1;
    }

    fadix::editor::ScriptEditorController controller;
    controller.Bind(database);
    controller.SelectScript("EditorProof");

    const bool loadedFromDisk =
        controller.Source().find("function OnUpdate") != std::string::npos;
    Check(loadedFromDisk, "SelectScript loads file contents into the editor");
    Check(controller.HasScript(), "SelectScript activates a script");
    Check(std::string{controller.LanguageLabel()} == "FXS", "Lua scripts label as FXS");

    controller.InsertText("-- typed through owned editor\n");
    const bool changed =
        controller.Source().find("-- typed through owned editor") != std::string::npos;
    Check(changed, "InsertText changes the editor value");

    controller.SelectAll();
    const bool selectedAll =
        controller.SelectionAnchor() == 0 && controller.Cursor() == controller.Source().size();
    Check(selectedAll, "SelectAll selects the whole script");

    const std::string edited = "aaa X aaa X aaa X\nline2\nline3\n";
    controller.InsertText(edited);
    const bool replacedDoc = controller.Source() == edited;
    Check(replacedDoc, "typing replaces the selection");

    controller.SetFindQuery("X");
    Check(controller.FindNext(true), "find next locates first match");
    const std::size_t first = controller.SelectionAnchor();
    Check(controller.FindNext(true), "find next advances");
    const std::size_t second = controller.SelectionAnchor();
    Check(second > first, "search direction moves forward");
    Check(controller.FindNext(false), "find prev moves backward");
    Check(controller.SelectionAnchor() == first, "search direction moves backward to prior match");
    Check(controller.FindNext(true), "find next after prev");
    Check(controller.FindNext(true), "find next to third match");
    Check(controller.FindNext(true) && controller.SelectionAnchor() == first,
        "search wraps to first match");

    controller.SetFindQuery("aaa");
    controller.SetReplaceText("bb");
    const auto replaceAll = controller.ReplaceAll();
    Check(replaceAll.Ok && replaceAll.Count == 3, "replace-all rewrites every match");
    Check(controller.Source().find("aaa") == std::string::npos, "replace-all removed query text");

    controller.SetFindFlags({false, false, true});
    controller.SetFindQuery("(");
    controller.RefreshFindMatches();
    Check(!controller.FindScan().Ok, "invalid regex is reported");
    const auto badReplace = controller.ReplaceAll();
    Check(!badReplace.Ok, "replace-all rejects invalid regex");
    controller.SetFindFlags({});
    controller.SetFindQuery("");

    controller.GotoLineColumnInDocument(2, 1);
    Check(controller.StatusLine() == 2, "go-to-line selects the requested line");

    bool validated = false;
    bool reported = false;
    controller.SetValidator(
        [&validated](const fadix::ScriptAsset&) {
            validated = true;
            return fadix::editor::ScriptValidationResult{false, "mock syntax error", 2, 4};
        },
        [&reported](const fadix::ScriptAsset&, const fadix::editor::ScriptValidationResult& result) {
            reported = !result.Success && result.Line == 2 && result.Column == 4;
        });
    controller.SaveSelected();
    Check(validated && reported, "Save reports script validation diagnostics");

    controller.SelectScriptAt("EditorProof", 2, 4);
    Check(controller.StatusLine() == 2, "diagnostic navigation selects its source location");

    const auto asset = database.Get("EditorProof");
    Check(asset && !asset->get().Dirty, "saved asset is clean");

    controller.SelectScript("NativeProof");
    Check(std::string{controller.LanguageLabel()} == "C++", "C++ scripts label as C++");
    Check(controller.HasScript(), "switching scripts keeps an active document");
    Check(controller.AllScriptNames().size() == 2, "quick-open source lists project scripts");

    fadix::editor::FxsEditorSettings defaults = fadix::editor::FxsEditorSettings::Load();
    defaults.Zoom = 2;
    defaults.WordWrap = true;
    defaults.TabWidth = 2;
    defaults.UseTabs = false;
    defaults.Save();
    const fadix::editor::FxsEditorSettings reloaded = fadix::editor::FxsEditorSettings::Load();
    Check(reloaded.Zoom == 2 && reloaded.WordWrap && reloaded.TabWidth == 2 && !reloaded.UseTabs,
        "editor-global FXS settings round-trip");

    const auto uni = fadix::editor::ScanDocument("café café", "café", {});
    Check(uni.Ok && uni.Matches.size() == 2, "utf-8 literal search finds matches");

    {
        const auto parsed = fadix::editor::ParseLuaCompileError(
            "[@EditorProof.lua]:4: unexpected symbol near '!'");
        Check(parsed.Ok && parsed.Line == 4 && parsed.Column == 1, "parse lua error line");
        Check(parsed.File == "EditorProof.lua", "parse lua error file");
        Check(parsed.Message.find("unexpected symbol") != std::string::npos,
            "parse lua error message");

        const auto withCol = fadix::editor::ParseLuaCompileError("chunk.lua:2:5: near 'end'");
        Check(withCol.Ok && withCol.Line == 2 && withCol.Column == 5, "parse lua error column");

        const std::string src = "local a = 1\nlocal b = 2!!!\n";
        const auto sq = fadix::editor::SquiggleRangeForLineColumn(src, 2, 12);
        Check(sq.End > sq.Start, "squiggle range is non-empty");

        const auto trimmed = fadix::editor::TrimTrailingWhitespace("a  \nb\t\n");
        Check(trimmed == "a\nb\n", "trim trailing whitespace");

        const auto commented = fadix::editor::ToggleLineComments("local x = 1\n", 0, 11, "--");
        Check(commented.Text.find("--") != std::string::npos, "toggle line comment adds prefix");
        const auto uncommented =
            fadix::editor::ToggleLineComments(commented.Text, 0, commented.Text.size(), "--");
        Check(uncommented.Text.find("local x") != std::string::npos
                && uncommented.Text.find("--") == std::string::npos,
            "toggle line comment removes prefix");

        Check(fadix::editor::ShouldAutoClose("(", 1, '(', ')'), "auto-close when closer missing");
        Check(!fadix::editor::ShouldAutoClose("()", 1, '(', ')'),
            "no duplicate closer when already present");
    }

    {
        controller.SelectScript("EditorProof");
        controller.SetValidationDebounceMs(0);
        int liveValidations = 0;
        controller.SetValidator(
            [&liveValidations](const fadix::ScriptAsset& asset) {
                ++liveValidations;
                fadix::editor::ScriptValidationResult result;
                const std::string source{asset.SourceCode.begin(), asset.SourceCode.end()};
                if (source.find("!!!") != std::string::npos)
                {
                    result.Success = false;
                    result.Message = "[@EditorProof.lua]:1: unexpected symbol near '!'";
                    result.Line = 1;
                    result.Column = 1;
                }
                return result;
            },
            [](const fadix::ScriptAsset&, const fadix::editor::ScriptValidationResult&) {});
        controller.SelectAll();
        controller.InsertText("local ok = 1\n");
        controller.FlushPendingValidation();
        Check(liveValidations >= 1 && controller.LastValidation()
                && controller.LastValidation()->Success,
            "debounce validation clears on good source");
        controller.SelectAll();
        controller.InsertText("local bad = !!!\n");
        controller.FlushPendingValidation();
        Check(controller.LastValidation() && !controller.LastValidation()->Success,
            "debounce validation reports syntax error");
        Check(controller.Diagnostics().size() == 1, "diagnostics attached after failure");
        controller.SelectAll();
        controller.InsertText("print(unresolvedGlobal)\n");
        controller.FlushPendingValidation();
        // Mock treats only '!!!' as error — mirrors real Lua compile accepting free globals.
        Check(controller.LastValidation() && controller.LastValidation()->Success,
            "unresolved globals are not reported as syntax errors");

        controller.SelectAll();
        controller.InsertText("a  \nb\t\n");
        controller.TrimTrailingWhitespace();
        Check(controller.Source() == "a\nb\n", "controller trim trailing whitespace");
    }

    {
        std::unordered_set<std::string> keys;
        for (const auto& e : fadix::editor::FxsApiCatalog::Entries())
        {
            keys.insert(e.Owner + "/" + e.Name);
        }
        bool allPresent = true;
        for (const auto& required : fadix::editor::FxsApiCatalog::RequiredPublicApiKeys())
        {
            if (keys.find(required) == keys.end())
            {
                std::cout << "  missing catalog key: " << required << '\n';
                allPresent = false;
            }
        }
        Check(allPresent, "catalog contains every public FXS API key");
        Check(fadix::editor::FxsApiCatalog::ContainsPublicApi("Entity", "getPosition"),
            "Entity.getPosition is catalogued");
        Check(fadix::editor::FxsApiCatalog::ContainsPublicApi("Input", "isDown"),
            "Input.isDown is catalogued");
        Check(fadix::editor::FxsApiCatalog::ContainsPublicApi("audio", "play"),
            "audio.play is catalogued");
        Check(!fadix::editor::FxsApiCatalog::ContainsPublicApi("Entity", "teleport"),
            "invented Entity.teleport is not catalogued");
    }

    {
        const std::string src =
            "function OnUpdate(entity, deltaTime)\n"
            "  local speed = 1\n"
            "  entity.";
        fadix::editor::FxsSuggestQuery q;
        q.Document = src;
        q.Cursor = src.size();
        const auto member = fadix::editor::FxsApiCatalog::Suggest(q);
        const bool onlyEntity = !member.empty()
            && std::all_of(member.begin(), member.end(), [](const fadix::editor::FxsSuggestion& s) {
                   return s.Label == "id" || s.Label == "destroy" || s.Label == "getTarget"
                       || s.Label.rfind("get", 0) == 0 || s.Label.rfind("set", 0) == 0;
               });
        Check(onlyEntity, "entity. suggests only Entity members");
        Check(std::any_of(member.begin(), member.end(),
                  [](const auto& s) { return s.Label == "getPosition"; }),
            "entity. includes getPosition");
        Check(std::none_of(member.begin(), member.end(),
                  [](const auto& s) { return s.Label == "isDown" || s.Label == "print"; }),
            "entity. excludes Input/globals");

        const std::string inputSrc = "Input.";
        q.Document = inputSrc;
        q.Cursor = inputSrc.size();
        const auto inputMembers = fadix::editor::FxsApiCatalog::Suggest(q);
        Check(inputMembers.size() == 1 && inputMembers[0].Label == "isDown",
            "Input. suggests isDown only");

        const std::string audioSrc = "audio.p";
        q.Document = audioSrc;
        q.Cursor = audioSrc.size();
        const auto audioMembers = fadix::editor::FxsApiCatalog::Suggest(q);
        Check(std::any_of(audioMembers.begin(), audioMembers.end(),
                  [](const auto& s) { return s.Label == "play"; }),
            "audio.p ranks/finds play");
        Check(std::none_of(audioMembers.begin(), audioMembers.end(),
                  [](const auto& s) { return s.Label == "getPosition"; }),
            "audio. excludes Entity members");

        const std::string commentSrc = "-- entity.";
        q.Document = commentSrc;
        q.Cursor = commentSrc.size();
        Check(fadix::editor::FxsApiCatalog::Suggest(q).empty(),
            "no suggestions inside comments");

        const std::string strSrc = "local s = \"entity.\"";
        q.Document = strSrc;
        q.Cursor = strSrc.size() - 1;
        Check(fadix::editor::FxsApiCatalog::Suggest(q).empty(),
            "no suggestions inside strings");

        const std::string symbolsSrc =
            "function OnUpdate(entity, deltaTime)\n"
            "  local speed = 1\n";
        const auto locals = fadix::editor::FxsApiCatalog::IndexDocumentSymbols(symbolsSrc);
        Check(std::any_of(locals.begin(), locals.end(),
                  [](const auto& s) { return s.Name == "OnUpdate"; }),
            "symbol index finds local function OnUpdate");
        Check(std::any_of(locals.begin(), locals.end(),
                  [](const auto& s) { return s.Name == "entity"; }),
            "symbol index finds parameter entity");
        Check(std::any_of(locals.begin(), locals.end(),
                  [](const auto& s) { return s.Name == "speed"; }),
            "symbol index finds local variable speed");

        const std::string callSrc = "entity:setPosition(1, ";
        const auto tip = fadix::editor::FxsApiCatalog::BuildCallTip(callSrc, callSrc.size(), {});
        Check(tip.Active && tip.Text.find("setPosition") != std::string::npos,
            "call tip resolves setPosition signature");
        Check(tip.HighlightEnd > tip.HighlightStart, "call tip highlights active parameter");
    }

    {
        // Multi-document: independent dirty + close confirm + session + external + project search
        fadix::editor::ScriptEditorController multi;
        multi.Bind(database);
        multi.OpenScript("EditorProof");
        multi.SelectAll();
        multi.InsertText("-- tab A\n");
        Check(multi.Dirty() && multi.NameDirty("EditorProof"), "active tab dirty");
        multi.OpenScript("NativeProof");
        Check(multi.OpenDocuments().size() == 2, "two open documents");
        Check(multi.SelectedScript() == "NativeProof", "switch activates second tab");
        Check(!multi.Dirty(), "second tab starts clean");
        Check(multi.NameDirty("EditorProof"), "first tab stays dirty while inactive");
        multi.NextDocument(true);
        Check(multi.SelectedScript() == "EditorProof", "Ctrl+Tab cycles documents");
        Check(multi.Source().find("tab A") != std::string::npos, "tab content preserved");

        Check(multi.RequestCloseActive() == fadix::editor::FxsCloseDecision::NeedsSavePrompt,
            "dirty close asks for save prompt");
        multi.CancelClose();
        Check(multi.OpenDocuments().size() == 2, "cancel keeps document open");
        multi.ConfirmCloseDiscard(); // no pending — no-op
        Check(multi.RequestCloseActive() == fadix::editor::FxsCloseDecision::NeedsSavePrompt,
            "dirty close prompt again");
        multi.ConfirmCloseDiscard();
        Check(multi.OpenDocuments().size() == 1, "discard closes dirty document");

        multi.DebugMarkExternalChange("NativeProof", "-- disk version\n");
        Check(multi.HasExternalConflict(), "external change detected");
        multi.ResolveExternal(fadix::editor::FxsExternalDecision::KeepEditor);
        Check(!multi.HasExternalConflict(), "keep editor clears conflict");

        const auto sessionPath = folder / "fxs_session.json";
        multi.SetSessionPath(sessionPath);
        multi.OpenScript("EditorProof");
        Check(multi.SaveSession(), "session save");
        fadix::editor::ScriptEditorController restored;
        restored.Bind(database);
        restored.SetSessionPath(sessionPath);
        Check(restored.LoadSession(), "session load");
        Check(restored.OpenDocuments().size() >= 1, "session reopens documents");

        fadix::editor::FxsEditorSession roundTrip;
        Check(fadix::editor::ParseFxsSession(
                  fadix::editor::SerializeFxsSession({{{"A"}, {"B"}}, "B"}), roundTrip)
                && roundTrip.Active == "B" && roundTrip.Open.size() == 2,
            "session json round-trip");

        // Write searchable content and run project search on Scripts folder.
        {
            auto a = database.Get("EditorProof");
            Check(a.has_value(), "editor proof exists for search");
            const std::string body = "unique_search_token_xyz = 1\n";
            a->get().SourceCode.assign(body.begin(), body.end());
            a->get().Dirty = true;
            Check(database.Save("EditorProof"), "save search fixture");
        }
        fadix::editor::FxsProjectSearch search;
        fadix::editor::FxsProjectSearchRequest req;
        req.Query = "unique_search_token_xyz";
        req.Roots = {database.ScriptsFolder()};
        search.Start(req);
        for (int i = 0; i < 200 && search.State() == fadix::editor::FxsProjectSearchState::Running;
             ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Check(search.State() == fadix::editor::FxsProjectSearchState::Completed, "project search completes");
        Check(search.HitCount() >= 1, "project search finds script hit");
        const auto hits = search.SnapshotHits();
        Check(!hits.empty() && hits[0].Line >= 1 && !hits[0].Snippet.empty(),
            "project hit has path line snippet");
        search.Cancel();
    }

    std::filesystem::remove_all(folder, error);
    return g_Failures == 0 ? 0 : 1;
}
