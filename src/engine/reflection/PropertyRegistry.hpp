#pragma once

#include "engine/Result.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fadix
{
struct PropertyHooks
{
    std::function<void(void*)> Inspect;
    std::function<std::string(const void*)> Serialize;
    std::function<Result<void>(void*, std::string_view)> Deserialize;
    std::function<void(const void*, void*)> Clone;
};

struct PropertyDescriptor
{
    std::string Name;
    PropertyHooks Hooks;
};

struct ComponentDescriptor
{
    std::string Name;
    std::type_index Type{typeid(void)};
    std::vector<PropertyDescriptor> Properties;
};

class PropertyRegistry final
{
public:
    template <typename Component>
    void RegisterComponent(std::string name)
    {
        m_Components.insert_or_assign(
            std::type_index{typeid(Component)},
            ComponentDescriptor{std::move(name), std::type_index{typeid(Component)}, {}});
    }

    template <typename Component>
    void AddProperty(std::string name, PropertyHooks hooks = {})
    {
        const auto iterator = m_Components.find(std::type_index{typeid(Component)});
        if (iterator != m_Components.end())
        {
            iterator->second.Properties.push_back({std::move(name), std::move(hooks)});
        }
    }

    [[nodiscard]] const ComponentDescriptor* Find(const std::type_index type) const noexcept
    {
        const auto iterator = m_Components.find(type);
        return iterator == m_Components.end() ? nullptr : &iterator->second;
    }

private:
    std::unordered_map<std::type_index, ComponentDescriptor> m_Components;
};
}
