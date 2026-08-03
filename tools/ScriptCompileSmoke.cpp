#include "assets/ScriptDatabase.hpp"
#include "editor/scripting/ScriptEditorController.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

int main()
{
    const std::filesystem::path folder =
        std::filesystem::current_path() / "bin" / "smoke_tmp" / "script_compile";
    std::error_code error;
    std::filesystem::remove_all(folder, error);

    fadix::ScriptDatabase database;
    database.ScanFolder(folder);
    if (database.CreateNew("BackgroundProof", fadix::ScriptLanguage::Cpp) == nullptr)
    {
        std::cerr << "FAIL: create native script\n";
        return 1;
    }

    fadix::editor::ScriptEditorController controller;
    controller.Bind(database);
    controller.SelectScript("BackgroundProof");
    controller.SetValidator(
        [](const fadix::ScriptAsset&) {
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
            return fadix::editor::ScriptValidationResult{};
        },
        [](const fadix::ScriptAsset&, const fadix::editor::ScriptValidationResult&) {});
    controller.SetValidationDebounceMs(0);
    controller.SelectAll();
    controller.InsertText("// background validation\n");
    controller.TickValidation();
    if (!controller.IsCompiling())
    {
        std::cerr << "FAIL: native validation blocked the editor\n";
        return 1;
    }

    for (int i = 0; i < 100 && controller.IsCompiling(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        controller.TickValidation();
    }
    if (controller.IsCompiling() || !controller.LastValidation()
        || !controller.LastValidation()->Success)
    {
        std::cerr << "FAIL: background validation did not complete\n";
        return 1;
    }

    std::filesystem::remove_all(folder, error);
    std::cout << "ALL PASS: native script validation stays responsive\n";
    return 0;
}
