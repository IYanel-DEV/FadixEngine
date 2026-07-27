#include "editor/scripting/FxsEditorSession.hpp"

#include "project/ProjectJson.hpp"

namespace fadix::editor
{
std::string SerializeFxsSession(const FxsEditorSession& session)
{
    project_json::Value root = project_json::Value::MakeObject();
    project_json::Value open = project_json::Value::MakeArray();
    for (const auto& doc : session.Open)
    {
        project_json::Value item = project_json::Value::MakeObject();
        item["name"] = project_json::Value::MakeString(doc.Name);
        open.Push(std::move(item));
    }
    root["open"] = std::move(open);
    root["active"] = project_json::Value::MakeString(session.Active);
    return project_json::Stringify(root, 2);
}

bool ParseFxsSession(const std::string_view json, FxsEditorSession& out)
{
    out = {};
    const auto parsed = project_json::Parse(json);
    if (!parsed || !parsed->IsObject())
    {
        return false;
    }
    const project_json::Value& root = *parsed;
    if (root.Contains("active") && root.at("active").IsString())
    {
        out.Active = root.at("active").AsString();
    }
    if (root.Contains("open") && root.at("open").IsArray())
    {
        for (const auto& item : root.at("open").Array())
        {
            if (!item.IsObject() || !item.Contains("name") || !item.at("name").IsString())
            {
                continue;
            }
            out.Open.push_back(FxsSessionDoc{item.at("name").AsString()});
        }
    }
    return true;
}
}
