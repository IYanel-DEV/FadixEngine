#include "project/ProjectJson.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace fadix::project_json
{
namespace
{
struct Parser
{
    std::string_view Text;
    std::size_t Index{0};

    void SkipWs()
    {
        while (Index < Text.size() &&
               std::isspace(static_cast<unsigned char>(Text[Index])) != 0)
        {
            ++Index;
        }
    }

    bool Match(const char expected)
    {
        SkipWs();
        if (Index >= Text.size() || Text[Index] != expected)
        {
            return false;
        }
        ++Index;
        return true;
    }

    std::optional<Value> ParseValue();
    std::optional<std::string> ParseString();
    std::optional<Value> ParseNumber();
    std::optional<Value> ParseObject();
    std::optional<Value> ParseArray();
};

std::optional<std::string> Parser::ParseString()
{
    SkipWs();
    if (Index >= Text.size() || Text[Index] != '"')
    {
        return std::nullopt;
    }
    ++Index;
    std::string result;
    while (Index < Text.size())
    {
        const char ch = Text[Index++];
        if (ch == '"')
        {
            return result;
        }
        if (ch == '\\')
        {
            if (Index >= Text.size())
            {
                return std::nullopt;
            }
            const char escaped = Text[Index++];
            switch (escaped)
            {
            case '"':
            case '\\':
            case '/':
                result.push_back(escaped);
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u':
                if (Index + 4 > Text.size())
                {
                    return std::nullopt;
                }
                Index += 4;
                result.push_back('?');
                break;
            default:
                return std::nullopt;
            }
            continue;
        }
        result.push_back(ch);
    }
    return std::nullopt;
}

std::optional<Value> Parser::ParseNumber()
{
    SkipWs();
    const std::size_t start = Index;
    if (Index < Text.size() && Text[Index] == '-')
    {
        ++Index;
    }
    if (Index >= Text.size() || std::isdigit(static_cast<unsigned char>(Text[Index])) == 0)
    {
        return std::nullopt;
    }
    while (Index < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Index])) != 0)
    {
        ++Index;
    }
    if (Index < Text.size() && Text[Index] == '.')
    {
        ++Index;
        if (Index >= Text.size() || std::isdigit(static_cast<unsigned char>(Text[Index])) == 0)
        {
            return std::nullopt;
        }
        while (Index < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Index])) != 0)
        {
            ++Index;
        }
    }
    if (Index < Text.size() && (Text[Index] == 'e' || Text[Index] == 'E'))
    {
        ++Index;
        if (Index < Text.size() && (Text[Index] == '+' || Text[Index] == '-'))
        {
            ++Index;
        }
        if (Index >= Text.size() || std::isdigit(static_cast<unsigned char>(Text[Index])) == 0)
        {
            return std::nullopt;
        }
        while (Index < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Index])) != 0)
        {
            ++Index;
        }
    }
    char* end = nullptr;
    const std::string number{Text.substr(start, Index - start)};
    const double value = std::strtod(number.c_str(), &end);
    if (end == number.c_str())
    {
        return std::nullopt;
    }
    return Value::MakeNumber(value);
}

std::optional<Value> Parser::ParseObject()
{
    if (!Match('{'))
    {
        return std::nullopt;
    }
    Value object = Value::MakeObject();
    SkipWs();
    if (Match('}'))
    {
        return object;
    }
    while (true)
    {
        auto key = ParseString();
        if (!key || !Match(':'))
        {
            return std::nullopt;
        }
        auto value = ParseValue();
        if (!value)
        {
            return std::nullopt;
        }
        object.ObjectMutable().insert_or_assign(*key, std::move(*value));
        SkipWs();
        if (Match('}'))
        {
            return object;
        }
        if (!Match(','))
        {
            return std::nullopt;
        }
    }
}

std::optional<Value> Parser::ParseArray()
{
    if (!Match('['))
    {
        return std::nullopt;
    }
    Value array = Value::MakeArray();
    SkipWs();
    if (Match(']'))
    {
        return array;
    }
    while (true)
    {
        auto value = ParseValue();
        if (!value)
        {
            return std::nullopt;
        }
        array.Push(std::move(*value));
        SkipWs();
        if (Match(']'))
        {
            return array;
        }
        if (!Match(','))
        {
            return std::nullopt;
        }
    }
}

std::optional<Value> Parser::ParseValue()
{
    SkipWs();
    if (Index >= Text.size())
    {
        return std::nullopt;
    }
    const char ch = Text[Index];
    if (ch == '{')
    {
        return ParseObject();
    }
    if (ch == '[')
    {
        return ParseArray();
    }
    if (ch == '"')
    {
        auto text = ParseString();
        return text ? std::optional{Value::MakeString(std::move(*text))} : std::nullopt;
    }
    if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0)
    {
        return ParseNumber();
    }
    if (Text.substr(Index, 4) == "true")
    {
        Index += 4;
        return Value::MakeBool(true);
    }
    if (Text.substr(Index, 5) == "false")
    {
        Index += 5;
        return Value::MakeBool(false);
    }
    if (Text.substr(Index, 4) == "null")
    {
        Index += 4;
        return Value::MakeNull();
    }
    return std::nullopt;
}

void AppendIndent(std::string& out, const int indent, const int depth)
{
    out.append(static_cast<std::size_t>(indent * depth), ' ');
}

void StringifyInto(const Value& value, std::string& out, const int indent, const int depth)
{
    switch (value.GetType())
    {
    case Value::Type::Null:
        out += "null";
        break;
    case Value::Type::Bool:
        out += value.AsBool() ? "true" : "false";
        break;
    case Value::Type::Number:
    {
        std::ostringstream stream;
        stream.precision(15);
        stream << value.AsNumber();
        out += stream.str();
        break;
    }
    case Value::Type::String:
        out.push_back('"');
        out += Escape(value.AsString());
        out.push_back('"');
        break;
    case Value::Type::Object:
    {
        out.push_back('{');
        if (!value.Object().empty())
        {
            out.push_back('\n');
            bool first = true;
            for (const auto& [key, child] : value.Object())
            {
                if (!first)
                {
                    out += ",\n";
                }
                first = false;
                AppendIndent(out, indent, depth + 1);
                out.push_back('"');
                out += Escape(key);
                out += "\": ";
                StringifyInto(child, out, indent, depth + 1);
            }
            out.push_back('\n');
            AppendIndent(out, indent, depth);
        }
        out.push_back('}');
        break;
    }
    case Value::Type::Array:
    {
        out.push_back('[');
        if (!value.Array().empty())
        {
            out.push_back('\n');
            for (std::size_t i = 0; i < value.Array().size(); ++i)
            {
                AppendIndent(out, indent, depth + 1);
                StringifyInto(value.Array()[i], out, indent, depth + 1);
                if (i + 1 < value.Array().size())
                {
                    out.push_back(',');
                }
                out.push_back('\n');
            }
            AppendIndent(out, indent, depth);
        }
        out.push_back(']');
        break;
    }
    }
}
}

Value Value::MakeNull()
{
    return {};
}

Value Value::MakeBool(const bool value)
{
    Value result;
    result.m_Type = Type::Bool;
    result.m_Bool = value;
    return result;
}

Value Value::MakeNumber(const double value)
{
    Value result;
    result.m_Type = Type::Number;
    result.m_Number = value;
    return result;
}

Value Value::MakeString(std::string value)
{
    Value result;
    result.m_Type = Type::String;
    result.m_String = std::move(value);
    return result;
}

Value Value::MakeObject()
{
    Value result;
    result.m_Type = Type::Object;
    return result;
}

Value Value::MakeArray()
{
    Value result;
    result.m_Type = Type::Array;
    return result;
}

const std::string& Value::AsString() const
{
    return m_String;
}

double Value::AsNumber() const
{
    return m_Number;
}

bool Value::AsBool() const
{
    return m_Bool;
}

Value& Value::operator[](const std::string_view key)
{
    return m_Object[std::string{key}];
}

const Value& Value::at(const std::string_view key) const
{
    static const Value kNull{};
    const auto iterator = m_Object.find(std::string{key});
    return iterator == m_Object.end() ? kNull : iterator->second;
}

bool Value::Contains(const std::string_view key) const
{
    return m_Object.contains(std::string{key});
}

void Value::Push(Value value)
{
    m_Array.push_back(std::move(value));
}

std::optional<Value> Parse(const std::string_view text)
{
    Parser parser{text, 0};
    auto value = parser.ParseValue();
    if (!value)
    {
        return std::nullopt;
    }
    parser.SkipWs();
    if (parser.Index != parser.Text.size())
    {
        return std::nullopt;
    }
    return value;
}

std::string Stringify(const Value& value, const int indent)
{
    std::string out;
    StringifyInto(value, out, indent, 0);
    out.push_back('\n');
    return out;
}

std::string Escape(const std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const unsigned char ch : text)
    {
        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                constexpr char Hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(Hex[ch >> 4U]);
                out.push_back(Hex[ch & 0x0FU]);
            }
            else
            {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return out;
}
}
