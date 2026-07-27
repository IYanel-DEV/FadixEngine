#include "editor/dock/DockLayout.hpp"
#include "project/ProjectJson.hpp"

#include <iostream>
#include <string>

namespace
{
bool Check(const bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "[dock-smoke] " << message << '\n';
    }
    return condition;
}
}

int main()
{
    using namespace fadix::editor;
    bool ok = true;
    DockLayout layout = DockLayout::Default();
    std::string error;
    ok &= Check(layout.Validate(error), "default layout invalid");
    ok &= Check(layout.FindLeaf("center") != nullptr, "viewport leaf missing");

    ok &= Check(layout.DockPanel("inspector", "center", DropRegion::Left),
        "edge split failed");
    ok &= Check(layout.FindLeaf("inspector") != nullptr, "split lost inspector");
    ok &= Check(layout.DockPanel("left-panel", "center", DropRegion::Center),
        "center swap failed");
    ok &= Check(layout.FindLeaf("left-panel") != nullptr &&
        layout.FindLeaf("center") != nullptr, "center swap lost a panel");

    FloatingPanel floating{"bottom-panel", 120.0F, 90.0F, 420.0F, 260.0F};
    ok &= Check(layout.FloatPanel("bottom-panel", floating), "tear-off failed");
    ok &= Check(layout.Floating.size() == 1, "floating list mismatch");
    ok &= Check(layout.DockPanel("bottom-panel", "center", DropRegion::Bottom),
        "floating re-dock failed");
    ok &= Check(layout.Floating.empty(), "re-dock retained floating entry");

    const auto encoded = layout.ToJson();
    auto decoded = DockLayout::FromJson(encoded, error);
    ok &= Check(decoded.has_value(), "schema-v2 parse failed");
    ok &= Check(decoded && decoded->Validate(error), "schema-v2 round-trip invalid");

    const auto old = fadix::project_json::Parse(R"json({
      "version": 1,
      "zones": {
        "left": "left-panel",
        "center": "center",
        "right": "inspector",
        "bottom": "bottom-panel"
      },
      "dimensions": {
        "leftWidth": 232,
        "rightWidth": 340,
        "bottomHeight": 190
      }
    })json");
    auto migrated = old ? DockLayout::FromJson(*old, error) : std::nullopt;
    ok &= Check(migrated.has_value(), "schema-v1 migration failed");
    ok &= Check(migrated && migrated->FindLeaf("center") != nullptr,
        "migration lost viewport");
    ok &= Check(migrated && migrated->Floating.empty(),
        "migration invented floating panels");

    if (!ok)
    {
        return 1;
    }
    std::cout << "[dock-smoke] PASS layout operations and migration\n";
    return 0;
}
