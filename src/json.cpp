#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sf {

namespace {

const Json& nullJson() {
    static const Json kNull;
    return kNull;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

struct Parser {
    std::string_view s;
    size_t i = 0;
    int depth = 0;

    [[noreturn]] void fail(const std::string& msg) const {
        size_t line = 1, col = 1;
        for (size_t k = 0; k < i && k < s.size(); ++k) {
            if (s[k] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        throw JsonError("JSON parse error at line " + std::to_string(line) + ", column " + std::to_string(col) +
                        ": " + msg);
    }

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool consume(char c) {
        ws();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    void expect(char c) {
        if (!consume(c)) fail(std::string("expected '") + c + "'");
    }
    bool peek(char c) {
        ws();
        return i < s.size() && s[i] == c;
    }

    Json value() {
        ws();
        if (i >= s.size()) fail("unexpected end of input");
        if (++depth > 512) fail("nesting too deep");
        Json out;
        char c = s[i];
        if (c == '{') {
            out = object();
        } else if (c == '[') {
            out = array();
        } else if (c == '"') {
            out = Json(string());
        } else if (c == 't') {
            literal("true");
            out = Json(true);
        } else if (c == 'f') {
            literal("false");
            out = Json(false);
        } else if (c == 'n') {
            literal("null");
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            out = number();
        } else {
            fail(std::string("unexpected character '") + c + "'");
        }
        --depth;
        return out;
    }

    void literal(const char* lit) {
        size_t n = std::strlen(lit);
        if (s.substr(i, n) != lit) fail("invalid literal");
        i += n;
    }

    Json object() {
        ++i;  // '{'
        Json::Object obj;
        if (consume('}')) return Json(std::move(obj));
        while (true) {
            ws();
            if (i >= s.size() || s[i] != '"') fail("expected string key");
            std::string key = string();
            expect(':');
            Json v = value();
            bool replaced = false;
            for (auto& m : obj) {
                if (m.first == key) {
                    m.second = std::move(v);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) obj.emplace_back(std::move(key), std::move(v));
            if (consume(',')) {
                if (peek('}')) {  // tolerate trailing comma
                    ++i;
                    break;
                }
                continue;
            }
            expect('}');
            break;
        }
        return Json(std::move(obj));
    }

    Json array() {
        ++i;  // '['
        Json::Array arr;
        if (consume(']')) return Json(std::move(arr));
        while (true) {
            arr.push_back(value());
            if (consume(',')) {
                if (peek(']')) {
                    ++i;
                    break;
                }
                continue;
            }
            expect(']');
            break;
        }
        return Json(std::move(arr));
    }

    uint32_t hex4() {
        if (i + 4 > s.size()) fail("truncated \\u escape");
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else fail("invalid hex digit in \\u escape");
        }
        return v;
    }

    std::string string() {
        ++i;  // opening quote
        std::string out;
        while (true) {
            if (i >= s.size()) fail("unterminated string");
            char c = s[i++];
            if (c == '"') break;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) fail("unterminated escape");
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        uint32_t lo = hex4();
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail(std::string("invalid escape '\\") + e + "'");
            }
        }
        return out;
    }

    Json number() {
        size_t start = i;
        while (i < s.size()) {
            char c = s[i];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') ++i;
            else break;
        }
        std::string tok(s.substr(start, i - start));
        char* end = nullptr;
        double v = std::strtod(tok.c_str(), &end);
        if (end == tok.c_str() || *end != '\0') {
            i = start;
            fail("invalid number '" + tok + "'");
        }
        return Json(v);
    }
};

void dumpString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

void dumpNumber(std::string& out, double v) {
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f", v);
        out += (std::strcmp(buf, "-0") == 0) ? "0" : buf;
        return;
    }
    char buf[40];
    for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    out += buf;
}

}  // namespace

const char* jsonTypeName(Json::Type t) {
    switch (t) {
        case Json::Type::Null: return "null";
        case Json::Type::Bool: return "boolean";
        case Json::Type::Number: return "number";
        case Json::Type::String: return "string";
        case Json::Type::Array: return "array";
        case Json::Type::Object: return "object";
    }
    return "unknown";
}

bool Json::asBool() const {
    if (type_ != Type::Bool) throw JsonError(std::string("expected boolean, got ") + jsonTypeName(type_));
    return bool_;
}
double Json::asNumber() const {
    if (type_ != Type::Number) throw JsonError(std::string("expected number, got ") + jsonTypeName(type_));
    return num_;
}
const std::string& Json::asString() const {
    if (type_ != Type::String) throw JsonError(std::string("expected string, got ") + jsonTypeName(type_));
    return str_;
}
const Json::Array& Json::asArray() const {
    if (type_ != Type::Array) throw JsonError(std::string("expected array, got ") + jsonTypeName(type_));
    return arr_;
}
Json::Array& Json::asArray() {
    if (type_ != Type::Array) throw JsonError(std::string("expected array, got ") + jsonTypeName(type_));
    return arr_;
}
const Json::Object& Json::asObject() const {
    if (type_ != Type::Object) throw JsonError(std::string("expected object, got ") + jsonTypeName(type_));
    return obj_;
}
Json::Object& Json::asObject() {
    if (type_ != Type::Object) throw JsonError(std::string("expected object, got ") + jsonTypeName(type_));
    return obj_;
}

const Json* Json::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& m : obj_)
        if (m.first == key) return &m.second;
    return nullptr;
}
Json* Json::find(std::string_view key) {
    if (type_ != Type::Object) return nullptr;
    for (auto& m : obj_)
        if (m.first == key) return &m.second;
    return nullptr;
}

Json& Json::operator[](std::string_view key) {
    if (type_ == Type::Null) type_ = Type::Object;
    if (type_ != Type::Object) throw JsonError(std::string("cannot index ") + jsonTypeName(type_) + " by key");
    if (Json* v = find(key)) return *v;
    obj_.emplace_back(std::string(key), Json());
    return obj_.back().second;
}
const Json& Json::operator[](std::string_view key) const {
    const Json* v = find(key);
    return v ? *v : nullJson();
}
bool Json::erase(std::string_view key) {
    if (type_ != Type::Object) return false;
    for (auto it = obj_.begin(); it != obj_.end(); ++it) {
        if (it->first == key) {
            obj_.erase(it);
            return true;
        }
    }
    return false;
}

const Json& Json::at(size_t i) const {
    if (type_ != Type::Array || i >= arr_.size()) throw JsonError("array index out of range");
    return arr_[i];
}
Json& Json::at(size_t i) {
    if (type_ != Type::Array || i >= arr_.size()) throw JsonError("array index out of range");
    return arr_[i];
}
void Json::push_back(Json v) {
    if (type_ == Type::Null) type_ = Type::Array;
    if (type_ != Type::Array) throw JsonError("push_back on non-array");
    arr_.push_back(std::move(v));
}

size_t Json::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

double Json::numberOr(std::string_view key, double def) const {
    const Json* v = find(key);
    return (v && v->isNumber()) ? v->num_ : def;
}
std::string Json::stringOr(std::string_view key, const std::string& def) const {
    const Json* v = find(key);
    return (v && v->isString()) ? v->str_ : def;
}
bool Json::boolOr(std::string_view key, bool def) const {
    const Json* v = find(key);
    return (v && v->isBool()) ? v->bool_ : def;
}

Json Json::parse(std::string_view text) {
    Parser p{text};
    Json v = p.value();
    p.ws();
    if (p.i != text.size()) p.fail("unexpected trailing characters");
    return v;
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

void Json::dumpTo(std::string& out, int indent, int level) const {
    auto newline = [&](int lvl) {
        if (indent < 0) return;
        out += '\n';
        out.append(static_cast<size_t>(indent * lvl), ' ');
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: dumpNumber(out, num_); break;
        case Type::String: dumpString(out, str_); break;
        case Type::Array: {
            if (arr_.empty()) {
                out += "[]";
                break;
            }
            // Arrays of plain numbers (vectors, points) stay on one line for readability.
            bool flat = true;
            for (const auto& v : arr_)
                if (v.isArray() || v.isObject()) flat = false;
            out += '[';
            for (size_t k = 0; k < arr_.size(); ++k) {
                if (k) out += (indent >= 0 && flat) ? ", " : ",";
                if (!flat) newline(level + 1);
                arr_[k].dumpTo(out, indent, level + 1);
            }
            if (!flat) newline(level);
            out += ']';
            break;
        }
        case Type::Object: {
            if (obj_.empty()) {
                out += "{}";
                break;
            }
            out += '{';
            for (size_t k = 0; k < obj_.size(); ++k) {
                if (k) out += ',';
                newline(level + 1);
                dumpString(out, obj_[k].first);
                out += indent >= 0 ? ": " : ":";
                obj_[k].second.dumpTo(out, indent, level + 1);
            }
            newline(level);
            out += '}';
            break;
        }
    }
}

bool Json::operator==(const Json& o) const {
    if (type_ != o.type_) return false;
    switch (type_) {
        case Type::Null: return true;
        case Type::Bool: return bool_ == o.bool_;
        case Type::Number: return num_ == o.num_;
        case Type::String: return str_ == o.str_;
        case Type::Array: return arr_ == o.arr_;
        case Type::Object: return obj_ == o.obj_;
    }
    return false;
}

}  // namespace sf
