// Minimal, dependency-free JSON value with an order-preserving object type.
// Object key order is kept so scene files and tool output stay readable.
#pragma once

#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace sf {

class JsonError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Object = std::vector<Member>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    template <typename T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Json(T v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(const char* s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Json(std::string_view s) : type_(Type::String), str_(s) {}
    Json(Array a) : type_(Type::Array), arr_(std::move(a)) {}
    Json(Object o) : type_(Type::Object), obj_(std::move(o)) {}

    static Json object(std::initializer_list<Member> members = {}) { return Json(Object(members)); }
    static Json array(std::initializer_list<Json> items = {}) { return Json(Array(items)); }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool() const;
    double asNumber() const;
    int asInt() const { return static_cast<int>(asNumber()); }
    const std::string& asString() const;
    const Array& asArray() const;
    Array& asArray();
    const Object& asObject() const;
    Object& asObject();

    // Object access. find() returns nullptr when missing or when not an object.
    const Json* find(std::string_view key) const;
    Json* find(std::string_view key);
    bool contains(std::string_view key) const { return find(key) != nullptr; }
    Json& operator[](std::string_view key);              // inserts (null -> object)
    const Json& operator[](std::string_view key) const;  // null value when missing
    bool erase(std::string_view key);

    // Array access.
    const Json& at(size_t i) const;
    Json& at(size_t i);
    void push_back(Json v);  // null -> array

    size_t size() const;

    double numberOr(std::string_view key, double def) const;
    std::string stringOr(std::string_view key, const std::string& def) const;
    bool boolOr(std::string_view key, bool def) const;

    static Json parse(std::string_view text);
    // indent < 0: compact single line.
    std::string dump(int indent = -1) const;

    bool operator==(const Json& o) const;
    bool operator!=(const Json& o) const { return !(*this == o); }

private:
    void dumpTo(std::string& out, int indent, int level) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    Array arr_;
    Object obj_;
};

const char* jsonTypeName(Json::Type t);

}  // namespace sf
