#pragma once
// ============================================================================
// Minimal, dependency-free JSON reader (subset) for asset manifests.
// Supports: objects, arrays, strings, numbers, true/false/null.
// Tolerant: on any parse error it returns a null value; callers fall back to
// defaults. This is intentionally small and forgiving so hand-edited manifests
// never crash the game.
// ============================================================================
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdlib>
#include <fstream>
#include <sstream>

struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<JsonValue> arr;
    std::map<std::string, JsonValue> obj;

    bool is_null()   const { return type == Null; }
    bool is_object() const { return type == Object; }
    bool is_array()  const { return type == Array; }

    // Safe object lookup (returns Null value if missing / not an object).
    const JsonValue& operator[](const std::string& key) const {
        static const JsonValue null_val;
        if (type != Object) return null_val;
        auto it = obj.find(key);
        return it == obj.end() ? null_val : it->second;
    }
    // Typed accessors with defaults.
    std::string as_string(const std::string& def = "") const {
        return type == String ? str : def;
    }
    double as_number(double def = 0.0) const {
        return type == Number ? num : def;
    }
    bool as_bool(bool def = false) const {
        return type == Bool ? b : def;
    }
};

class JsonParser {
public:
    // Parse text; returns Null value on any error.
    static JsonValue parse(const std::string& text) {
        JsonParser p(text);
        p.skip_ws();
        JsonValue v = p.parse_value();
        if (p.failed) return JsonValue();
        return v;
    }
    // Parse a file; returns Null if missing/unreadable/invalid.
    static JsonValue parse_file(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f.good()) return JsonValue();
        std::stringstream ss; ss << f.rdbuf();
        return parse(ss.str());
    }

private:
    const std::string& s;
    size_t i = 0;
    bool failed = false;
    explicit JsonParser(const std::string& t) : s(t) {}

    void skip_ws() {
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
            // line comments (// ...) — convenience for hand-edited manifests
            if (c == '/' && i + 1 < s.size() && s[i+1] == '/') {
                while (i < s.size() && s[i] != '\n') i++;
                continue;
            }
            break;
        }
    }
    char peek() { return i < s.size() ? s[i] : '\0'; }

    JsonValue parse_value() {
        if (failed) return JsonValue();
        skip_ws();
        char c = peek();
        switch (c) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': return parse_string_val();
            case 't': case 'f': return parse_bool();
            case 'n': return parse_null();
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
                failed = true; return JsonValue();
        }
    }

    JsonValue parse_object() {
        JsonValue v; v.type = JsonValue::Object;
        i++; // {
        skip_ws();
        if (peek() == '}') { i++; return v; }
        while (!failed) {
            skip_ws();
            if (peek() != '"') { failed = true; break; }
            std::string key = parse_string();
            skip_ws();
            if (peek() != ':') { failed = true; break; }
            i++; // :
            JsonValue val = parse_value();
            if (failed) break;
            v.obj[key] = val;
            skip_ws();
            char c = peek();
            if (c == ',') { i++; continue; }
            if (c == '}') { i++; break; }
            failed = true; break;
        }
        return v;
    }

    JsonValue parse_array() {
        JsonValue v; v.type = JsonValue::Array;
        i++; // [
        skip_ws();
        if (peek() == ']') { i++; return v; }
        while (!failed) {
            JsonValue val = parse_value();
            if (failed) break;
            v.arr.push_back(val);
            skip_ws();
            char c = peek();
            if (c == ',') { i++; continue; }
            if (c == ']') { i++; break; }
            failed = true; break;
        }
        return v;
    }

    std::string parse_string() {
        std::string out;
        i++; // opening quote
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return out;
            if (c == '\\' && i < s.size()) {
                char e = s[i++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        failed = true; return out;
    }
    JsonValue parse_string_val() {
        JsonValue v; v.type = JsonValue::String;
        v.str = parse_string();
        return v;
    }

    JsonValue parse_number() {
        size_t start = i;
        if (peek() == '-') i++;
        while (i < s.size()) {
            char c = s[i];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') i++;
            else break;
        }
        JsonValue v; v.type = JsonValue::Number;
        v.num = std::strtod(s.substr(start, i - start).c_str(), nullptr);
        return v;
    }

    JsonValue parse_bool() {
        JsonValue v; v.type = JsonValue::Bool;
        if (s.compare(i, 4, "true") == 0) { v.b = true; i += 4; }
        else if (s.compare(i, 5, "false") == 0) { v.b = false; i += 5; }
        else failed = true;
        return v;
    }
    JsonValue parse_null() {
        if (s.compare(i, 4, "null") == 0) { i += 4; return JsonValue(); }
        failed = true; return JsonValue();
    }
};
