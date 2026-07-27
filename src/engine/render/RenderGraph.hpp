#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fadix::rhi
{
class CommandList;
}

namespace fadix
{
class RenderGraph final
{
public:
    using Pass = std::function<void(rhi::CommandList&)>;

    void AddPass(std::string name, Pass pass)
    {
        m_Passes.push_back({std::move(name), std::move(pass)});
    }
    void Clear() noexcept { m_Passes.clear(); }
    void Execute(rhi::CommandList& commands)
    {
        for (Entry& entry : m_Passes)
        {
            entry.Execute(commands);
        }
    }

private:
    struct Entry
    {
        std::string Name;
        Pass Execute;
    };
    std::vector<Entry> m_Passes;
};
}
