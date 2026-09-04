#include "json.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <utility>

namespace json {

namespace {

std::string utf8Encode(uint32_t cp) {
    std::string out;
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

struct Parser {
    std::string_view s;
    size_t i = 0;

    void skipWs() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    }

    bool eof() const { return i >= s.size(); }

    char peek() const { return i < s.size() ? s[i] : '\0'; }

    char next() { return i < s.size() ? s[i++] : '\0'; }

    void expect(char c) {
        if (eof() || s[i] != c) throw std::runtime_error("json: expected '" + std::string(1, c) + "'");
        i++;
    }

    Value parseValue() {
        skipWs();
        if (eof()) throw std::runtime_error("json: unexpected end");
        char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return Value(parseString());
        if (c == 't' || c == 'f') return Value(parseBool());
        if (c == 'n') { parseLiteral("null"); return Value(nullptr); }
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        throw std::runtime_error("json: unexpected character");
    }

    Value parseObject() {
        expect('{');
        Object obj;
        skipWs();
        if (peek() == '}') { next(); return Value(std::move(obj)); }
        while (true) {
            skipWs();
            if (peek() != '"') throw std::runtime_error("json: expected string key");
            std::string key = parseString();
            skipWs();
            expect(':');
            obj[std::move(key)] = parseValue();
            skipWs();
            if (peek() == ',') { next(); continue; }
            expect('}');
            break;
        }
        return Value(std::move(obj));
    }

    Value parseArray() {
        expect('[');
        Array arr;
        skipWs();
        if (peek() == ']') { next(); return Value(std::move(arr)); }
        while (true) {
            arr.push_back(parseValue());
            skipWs();
            if (peek() == ',') { next(); continue; }
            expect(']');
            break;
        }
        return Value(std::move(arr));
    }

    bool parseBool() {
        if (peek() == 't') {
            parseLiteral("true");
            return true;
        }
        parseLiteral("false");
        return false;
    }

    void parseLiteral(const char* lit) {
        for (const char* p = lit; *p; ++p) {
            if (eof() || s[i] != *p) throw std::runtime_error("json: bad literal");
            i++;
        }
    }

    static int hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    uint32_t parseHex4() {
        if (i + 4 > s.size()) throw std::runtime_error("json: bad \\u escape");
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            int h = hexVal(s[i++]);
            if (h < 0) throw std::runtime_error("json: bad \\u escape");
            v = (v << 4) | static_cast<uint32_t>(h);
        }
        return v;
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (!eof()) {
            char c = next();
            if (c == '"') return out;
            if (c == '\\') {
                if (eof()) throw std::runtime_error("json: bad escape");
                char e = next();
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = parseHex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // High surrogate: expect a low surrogate.
                            if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                                i += 2;
                                uint32_t low = parseHex4();
                                if (low >= 0xDC00 && low <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                }
                            }
                        }
                        out += utf8Encode(cp);
                        break;
                    }
                    default: throw std::runtime_error("json: bad escape");
                }
            } else {
                out.push_back(c);
            }
        }
        throw std::runtime_error("json: unterminated string");
    }

    Value parseNumber() {
        size_t start = i;
        if (peek() == '-') next();
        bool isFloat = false;
        while (!eof()) {
            char c = peek();
            if (c >= '0' && c <= '9') { next(); continue; }
            if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                isFloat = true;
                next();
                continue;
            }
            break;
        }
        std::string token(s.substr(start, i - start));
        if (isFloat) {
            char* end = nullptr;
            double d = std::strtod(token.c_str(), &end);
            if (end == token.c_str()) throw std::runtime_error("json: bad number");
            return Value(d);
        }
        errno = 0;
        char* end = nullptr;
        long long ll = std::strtoll(token.c_str(), &end, 10);
        if (end == token.c_str() || errno == ERANGE) throw std::runtime_error("json: bad number");
        return Value(static_cast<int64_t>(ll));
    }
};

std::string escapeString(std::string_view v) {
    std::string out;
    out.reserve(v.size() + 2);
    out.push_back('"');
    for (char c : v) {
        unsigned char u = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (u < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", u);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

void indent(std::string& out, int depth) {
    out.append(static_cast<size_t>(depth * 2), ' ');
}

std::string serializeValue(const Value& v, int depth) {
    using namespace std;
    if (v.isNull()) return "null";
    if (v.isBool()) return v.asBool() ? "true" : "false";
    if (v.isInt()) return std::to_string(v.asInt());
    if (v.isDouble()) {
        ostringstream os;
        os << setprecision(17) << v.asDouble();
        return os.str();
    }
    if (v.isString()) return escapeString(v.asString());
    if (v.isArray()) {
        const auto& arr = v.asArray();
        if (arr.empty()) return "[]";
        string out = "[";
        for (size_t k = 0; k < arr.size(); k++) {
            if (k) out += ",";
            out += "\n";
            indent(out, depth + 1);
            out += serializeValue(arr[k], depth + 1);
        }
        out += "\n";
        indent(out, depth);
        out += "]";
        return out;
    }
    const auto& obj = v.asObject();
    if (obj.empty()) return "{}";
    string out = "{";
    size_t k = 0;
    for (const auto& [key, val] : obj) {
        if (k++) out += ",";
        out += "\n";
        indent(out, depth + 1);
        out += escapeString(key);
        out += ": ";
        out += serializeValue(val, depth + 1);
    }
    out += "\n";
    indent(out, depth);
    out += "}";
    return out;
}

} // namespace

Value parse(std::string_view text) {
    Parser p{text};
    Value v = p.parseValue();
    p.skipWs();
    if (!p.eof()) throw std::runtime_error("json: trailing content");
    return v;
}

std::string serialize(const Value& v, int indentDepth) {
    return serializeValue(v, indentDepth);
}

} // namespace json
