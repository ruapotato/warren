// Warren -- JSON, because glTF is JSON.
//
// Small on purpose. This exists to read one family of files written
// by well-behaved exporters, so it parses the whole of RFC 8259 and
// nothing beyond it: no comments, no trailing commas, no
// single-quoted strings. A parser that accepts more than the format
// allows is a parser that lets a broken file through to somewhere
// harder to debug.
//
// It is also reading files a user downloaded, so depth is bounded
// and every number is checked. A stack overflow from nested arrays
// is a real way to crash an editor.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wr {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    explicit Json(bool b) : type_(Type::Bool), number_(b ? 1.0 : 0.0) {}
    explicit Json(double n) : type_(Type::Number), number_(n) {}
    explicit Json(std::string s) : type_(Type::String), string_(std::move(s)) {}

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_bool() const { return type_ == Type::Bool; }

    // Reading, with a default for everything. A glTF is full of
    // optional fields and a reader that throws on a missing one is a
    // reader wrapped in a hundred checks at the call site.
    double number(double fallback = 0.0) const {
        return type_ == Type::Number ? number_ : fallback;
    }
    int integer(int fallback = 0) const {
        return type_ == Type::Number ? int(number_) : fallback;
    }
    bool boolean(bool fallback = false) const {
        return type_ == Type::Bool ? number_ != 0.0 : fallback;
    }
    const std::string &string(const std::string &fallback = {}) const {
        return type_ == Type::String ? string_ : fallback;
    }

    // Missing keys and out-of-range indices give a null, which reads
    // as its default -- so `doc["meshes"][3]["name"].string()` is
    // safe on a file that has none of those.
    const Json &operator[](const std::string &key) const;
    const Json &operator[](size_t index) const;
    size_t size() const { return array_.size(); }
    bool has(const std::string &key) const;
    const std::vector<Json> &items() const { return array_; }
    const std::map<std::string, Json> &fields() const { return object_; }

    // --- building ---------------------------------------------------
    //
    // The engine answers questions in JSON as well as reading it, so
    // the type has to work in both directions. An agent driving this
    // thing needs structured replies rather than log lines it has to
    // guess the shape of.
    static Json object() {
        Json j;
        j.type_ = Type::Object;
        return j;
    }
    static Json array() {
        Json j;
        j.type_ = Type::Array;
        return j;
    }
    Json &set(const std::string &key, Json value) {
        type_ = Type::Object;
        object_[key] = std::move(value);
        return *this;
    }
    Json &set(const std::string &key, const char *value) {
        return set(key, Json(std::string(value)));
    }
    Json &set(const std::string &key, const std::string &value) {
        return set(key, Json(value));
    }
    Json &set(const std::string &key, double value) {
        return set(key, Json(value));
    }
    Json &set(const std::string &key, int value) {
        return set(key, Json(double(value)));
    }
    Json &set(const std::string &key, bool value) {
        return set(key, Json(value));
    }
    Json &push(Json value) {
        type_ = Type::Array;
        array_.push_back(std::move(value));
        return *this;
    }

    // `indent` of zero is one line, which is what a protocol wants;
    // anything else is for a person reading a schema dump.
    std::string to_string(int indent = 0) const;

    // Null on a parse error, with `error` saying where.
    static Json parse(const char *text, size_t length,
                      std::string *error = nullptr);
    static Json parse(const std::string &text, std::string *error = nullptr) {
        return parse(text.data(), text.size(), error);
    }

private:
    friend struct JsonParser;
    Type type_ = Type::Null;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> array_;
    std::map<std::string, Json> object_;
};

}  // namespace wr
