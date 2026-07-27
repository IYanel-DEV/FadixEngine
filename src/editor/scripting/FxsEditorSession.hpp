#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace fadix::editor
{
struct FxsSessionDoc
{
    std::string Name;
};

struct FxsEditorSession
{
    std::vector<FxsSessionDoc> Open;
    std::string Active;
};

[[nodiscard]] std::string SerializeFxsSession(const FxsEditorSession& session);
[[nodiscard]] bool ParseFxsSession(std::string_view json, FxsEditorSession& out);
}
