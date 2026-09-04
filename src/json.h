#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace json {

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

class Value {
public:
    using Storage = std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object>;

    Value() : storage_(nullptr) {}
    Value(std::nullptr_t) : storage_(nullptr) {}
    Value(bool b) : storage_(b) {}
    Value(int v) : storage_(static_cast<int64_t>(v)) {}
    Value(unsigned v) : storage_(static_cast<int64_t>(v)) {}
    Value(int64_t v) : storage_(v) {}
    Value(uint64_t v) : storage_(static_cast<int64_t>(v)) {}
    Value(double v) : storage_(v) {}
    Value(std::string v) : storage_(std::move(v)) {}
    Value(const char* v) : storage_(std::string(v)) {}
    Value(Array v) : storage_(std::move(v)) {}
    Value(Object v) : storage_(std::move(v)) {}

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(storage_); }
    bool isBool() const { return std::holds_alternative<bool>(storage_); }
    bool isInt() const { return std::holds_alternative<int64_t>(storage_); }
    bool isDouble() const { return std::holds_alternative<double>(storage_); }
    bool isString() const { return std::holds_alternative<std::string>(storage_); }
    bool isArray() const { return std::holds_alternative<Array>(storage_); }
    bool isObject() const { return std::holds_alternative<Object>(storage_); }

    bool asBool(bool def = false) const { return isBool() ? std::get<bool>(storage_) : def; }
    int64_t asInt(int64_t def = 0) const {
        if (isInt()) return std::get<int64_t>(storage_);
        if (isDouble()) return static_cast<int64_t>(std::get<double>(storage_));
        return def;
    }
    double asDouble(double def = 0.0) const {
        if (isDouble()) return std::get<double>(storage_);
        if (isInt()) return static_cast<double>(std::get<int64_t>(storage_));
        return def;
    }
    const std::string& asString() const { return std::get<std::string>(storage_); }
    const Array& asArray() const { return std::get<Array>(storage_); }
    const Object& asObject() const { return std::get<Object>(storage_); }
    Array& asArrayRef() { return std::get<Array>(storage_); }
    Object& asObjectRef() { return std::get<Object>(storage_); }

    const Value* find(const std::string& key) const {
        if (!isObject()) return nullptr;
        auto& obj = asObject();
        auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }

    Value& operator[](const std::string& key) {
        if (!isObject()) storage_ = Object();
        return asObjectRef()[key];
    }

    const Storage& storage() const { return storage_; }

private:
    Storage storage_;
};

// Throws std::runtime_error on malformed input.
Value parse(std::string_view text);

// Pretty-printed UTF-8 JSON (2-space indent).
std::string serialize(const Value& v, int indentDepth = 0);

// Convenience helpers.
inline bool has(const Value& v, const std::string& key) { return v.find(key) != nullptr; }

} // namespace json
