#include "core/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mf {
namespace {
const Json k_null;
// Deep enough for any real document, shallow enough that a hostile
// one cannot exhaust the stack.
constexpr int kMaxDepth = 64;
}  // namespace

const Json &Json::operator[](const std::string &key) const {
    if (type_ != Type::Object) return k_null;
    auto it = object_.find(key);
    return it == object_.end() ? k_null : it->second;
}

const Json &Json::operator[](size_t index) const {
    if (type_ != Type::Array || index >= array_.size()) return k_null;
    return array_[index];
}

bool Json::has(const std::string &key) const {
    return type_ == Type::Object && object_.count(key) != 0;
}

struct JsonParser {
    const char *p = nullptr;
    const char *end = nullptr;
    std::string error;

    void skip() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
    }
    bool fail(const char *why) {
        if (error.empty()) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s at byte %td", why, p - end);
            error = buf;
        }
        return false;
    }

    bool value(Json *out, int depth) {
        if (depth > kMaxDepth) return fail("nested too deeply");
        skip();
        if (p >= end) return fail("unexpected end");
        switch (*p) {
            case '{': return object(out, depth);
            case '[': return array(out, depth);
            case '"': {
                std::string s;
                if (!parse_string(&s)) return false;
                *out = Json(std::move(s));
                return true;
            }
            case 't':
                if (end - p < 4 || std::memcmp(p, "true", 4)) return fail("bad literal");
                p += 4;
                *out = Json(true);
                return true;
            case 'f':
                if (end - p < 5 || std::memcmp(p, "false", 5)) return fail("bad literal");
                p += 5;
                *out = Json(false);
                return true;
            case 'n':
                if (end - p < 4 || std::memcmp(p, "null", 4)) return fail("bad literal");
                p += 4;
                *out = Json();
                return true;
            default: return number(out);
        }
    }

    bool number(Json *out) {
        const char *start = p;
        if (p < end && (*p == '-' || *p == '+')) p++;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' ||
                           *p == 'E' || *p == '+' || *p == '-'))
            p++;
        if (p == start) return fail("expected a value");
        // strtod over a bounded copy: the buffer may not be
        // null-terminated, and strtod would read past the end.
        char buf[64];
        const size_t n = size_t(p - start);
        if (n >= sizeof(buf)) return fail("number too long");
        std::memcpy(buf, start, n);
        buf[n] = 0;
        char *stop = nullptr;
        const double v = std::strtod(buf, &stop);
        if (stop == buf) return fail("not a number");
        *out = Json(v);
        return true;
    }

    bool parse_string(std::string *out) {
        if (p >= end || *p != '"') return fail("expected a string");
        p++;
        out->clear();
        while (p < end) {
            const char c = *p++;
            if (c == '"') return true;
            if (c != '\\') {
                out->push_back(c);
                continue;
            }
            if (p >= end) return fail("escape at end of input");
            const char e = *p++;
            switch (e) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    if (end - p < 4) return fail("truncated \\u escape");
                    unsigned code = 0;
                    for (int i = 0; i < 4; i++) {
                        const char h = p[i];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= unsigned(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= unsigned(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= unsigned(h - 'A' + 10);
                        else return fail("bad hex in \\u escape");
                    }
                    p += 4;
                    // UTF-8, so a name with an accent in it survives.
                    // Surrogate pairs are left as the replacement
                    // character: glTF names are not emoji.
                    if (code < 0x80) {
                        out->push_back(char(code));
                    } else if (code < 0x800) {
                        out->push_back(char(0xC0 | (code >> 6)));
                        out->push_back(char(0x80 | (code & 0x3F)));
                    } else if (code >= 0xD800 && code <= 0xDFFF) {
                        out->append("\xEF\xBF\xBD");
                    } else {
                        out->push_back(char(0xE0 | (code >> 12)));
                        out->push_back(char(0x80 | ((code >> 6) & 0x3F)));
                        out->push_back(char(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }

    bool array(Json *out, int depth) {
        p++;  // [
        out->type_ = Json::Type::Array;
        skip();
        if (p < end && *p == ']') {
            p++;
            return true;
        }
        while (p < end) {
            Json item;
            if (!value(&item, depth + 1)) return false;
            out->array_.push_back(std::move(item));
            skip();
            if (p < end && *p == ',') {
                p++;
                continue;
            }
            if (p < end && *p == ']') {
                p++;
                return true;
            }
            return fail("expected ',' or ']'");
        }
        return fail("unterminated array");
    }

    bool object(Json *out, int depth) {
        p++;  // {
        out->type_ = Json::Type::Object;
        skip();
        if (p < end && *p == '}') {
            p++;
            return true;
        }
        while (p < end) {
            skip();
            std::string key;
            if (!parse_string(&key)) return false;
            skip();
            if (p >= end || *p != ':') return fail("expected ':'");
            p++;
            Json item;
            if (!value(&item, depth + 1)) return false;
            out->object_[key] = std::move(item);
            skip();
            if (p < end && *p == ',') {
                p++;
                continue;
            }
            if (p < end && *p == '}') {
                p++;
                return true;
            }
            return fail("expected ',' or '}'");
        }
        return fail("unterminated object");
    }
};

Json Json::parse(const char *text, size_t length, std::string *error) {
    Json root;
    if (!text || !length) {
        if (error) *error = "empty input";
        return root;
    }
    JsonParser parser;
    parser.p = text;
    parser.end = text + length;
    if (!parser.value(&root, 0)) {
        if (error) *error = parser.error;
        return Json();
    }
    parser.skip();
    if (parser.p != parser.end) {
        // Trailing data is a sign the file is not what it claims to
        // be, and silently ignoring it hides that.
        if (error) *error = "trailing data after the document";
        return Json();
    }
    if (error) error->clear();
    return root;
}

}  // namespace mf
