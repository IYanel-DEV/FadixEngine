#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fadix::project_json
{
class Value
{
public:
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Object,
        Array
    };

    Value() = default;
    static Value MakeNull();
    static Value MakeBool(bool value);
    static Value MakeNumber(double value);
    static Value MakeString(std::string value);
    static Value MakeObject();
    static Value MakeArray();

    [[nodiscard]] Type GetType() const noexcept { return m_Type; }
    [[nodiscard]] bool IsObject() const noexcept { return m_Type == Type::Object; }
    [[nodiscard]] bool IsArray() const noexcept { return m_Type == Type::Array; }
    [[nodiscard]] bool IsString() const noexcept { return m_Type == Type::String; }

    [[nodiscard]] const std::string& AsString() const;
    [[nodiscard]] double AsNumber() const;
    [[nodiscard]] bool AsBool() const;

    Value& operator[](std::string_view key);
    const Value& at(std::string_view key) const;
    [[nodiscard]] bool Contains(std::string_view key) const;
    void Push(Value value);

    [[nodiscard]] const std::unordered_map<std::string, Value>& Object() const { return m_Object; }
    [[nodiscard]] const std::vector<Value>& Array() const { return m_Array; }
    std::unordered_map<std::string, Value>& ObjectMutable() { return m_Object; }
    std::vector<Value>& ArrayMutable() { return m_Array; }

private:
    Type m_Type{Type::Null};
    bool m_Bool{false};
    double m_Number{0.0};
    std::string m_String;
    std::unordered_map<std::string, Value> m_Object;
    std::vector<Value> m_Array;
};

[[nodiscard]] std::optional<Value> Parse(std::string_view text);
[[nodiscard]] std::string Stringify(const Value& value, int indent = 2);
[[nodiscard]] std::string Escape(std::string_view text);

// Current on-disk schema versions. Bump when a format changes incompatibly.
inline constexpr int kProjectFormatVersion = 1;
inline constexpr int kManifestFormatVersion = 1;

// Validates an integer format-version field. Returns an error message when the
// field is missing/non-numeric or newer than this build supports; nullopt = OK
// to load (accepts the current version and any older version).
[[nodiscard]] std::optional<std::string> CheckFormatVersion(
    const Value& object, std::string_view key, int expected, std::string_view label);
}
