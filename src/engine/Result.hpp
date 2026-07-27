#pragma once

#include <optional>
#include <string>
#include <utility>

namespace fadix
{
template <typename T>
class Result final
{
public:
    [[nodiscard]] static Result Ok(T value) { return Result{std::move(value), {}}; }
    [[nodiscard]] static Result Error(std::string error) { return Result{std::nullopt, std::move(error)}; }

    [[nodiscard]] bool IsOk() const noexcept { return m_Value.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return IsOk(); }
    [[nodiscard]] T& Value() & { return m_Value.value(); }
    [[nodiscard]] const T& Value() const& { return m_Value.value(); }
    [[nodiscard]] T&& Value() && { return std::move(m_Value).value(); }
    [[nodiscard]] const std::string& ErrorMessage() const noexcept { return m_Error; }

private:
    Result(std::optional<T> value, std::string error)
        : m_Value(std::move(value)), m_Error(std::move(error))
    {
    }

    std::optional<T> m_Value;
    std::string m_Error;
};

template <>
class Result<void> final
{
public:
    [[nodiscard]] static Result Ok() { return Result{true, {}}; }
    [[nodiscard]] static Result Error(std::string error) { return Result{false, std::move(error)}; }

    [[nodiscard]] bool IsOk() const noexcept { return m_Ok; }
    [[nodiscard]] explicit operator bool() const noexcept { return IsOk(); }
    [[nodiscard]] const std::string& ErrorMessage() const noexcept { return m_Error; }

private:
    Result(const bool ok, std::string error) : m_Ok(ok), m_Error(std::move(error)) {}

    bool m_Ok{false};
    std::string m_Error;
};
}
