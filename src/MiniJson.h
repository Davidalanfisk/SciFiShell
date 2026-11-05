#pragma once
// Minimal JSON reader: enough to pull fields out of web API responses.
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sf {

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::map<std::string, Json> o;

    const Json& operator[](const std::string& k) const {
        static const Json nil;
        auto f = o.find(k);
        return f == o.end() ? nil : f->second;
    }
    const Json& at(size_t i) const {
        static const Json nil;
        return i < a.size() ? a[i] : nil;
    }
    bool isNull() const { return type == Null; }
    const Json& first() const {
        static const Json nil;
        return type == Arr ? at(0) : (o.empty() ? nil : o.begin()->second);
    }
    std::string str(const std::string& d = "") const { return type == Str ? s : (type == Num ? std::to_string(static_cast<long long>(n)) : d); }
    double num(double d = 0) const { return type == Num ? n : (type == Str ? std::atof(s.c_str()) : d); }
    size_t size() const { return type == Arr ? a.size() : o.size(); }

    static Json parse(const std::string& text) {
        size_t i = 0;
        Json j = value(text, i, 0);
        return j;
    }

private:
    static void ws(const std::string& t, size_t& i) {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 63)); o += static_cast<char>(0x80 | (cp & 63)); }
        else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 63)); o += static_cast<char>(0x80 | ((cp >> 6) & 63)); o += static_cast<char>(0x80 | (cp & 63)); }
    }
    static std::string string(const std::string& t, size_t& i) {
        std::string r;
        ++i;
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') { r += c; continue; }
            if (i >= t.size()) break;
            c = t[i++];
            switch (c) {
                case 'n': r += '\n'; break;
                case 't': r += '\t'; break;
                case 'r': break;
                case 'b': case 'f': break;
                case 'u': {
                    unsigned cp = static_cast<unsigned>(std::strtoul(t.substr(i, 4).c_str(), nullptr, 16));
                    i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && t.compare(i, 2, "\\u") == 0) {
                        const unsigned lo = static_cast<unsigned>(std::strtoul(t.substr(i + 2, 4).c_str(), nullptr, 16));
                        i += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(r, cp);
                    break;
                }
                default: r += c;
            }
        }
        ++i;
        return r;
    }
    static Json value(const std::string& t, size_t& i, int depth) {
        Json j;
        ws(t, i);
        if (i >= t.size() || depth > 40) return j;
        const char c = t[i];
        if (c == '{') {
            j.type = Obj;
            ++i;
            for (;;) {
                ws(t, i);
                if (i >= t.size()) break;
                if (t[i] == '}') { ++i; break; }
                if (t[i] == ',') { ++i; continue; }
                if (t[i] != '"') break;
                std::string k = string(t, i);
                ws(t, i);
                if (i < t.size() && t[i] == ':') ++i;
                j.o[k] = value(t, i, depth + 1);
            }
        } else if (c == '[') {
            j.type = Arr;
            ++i;
            for (;;) {
                ws(t, i);
                if (i >= t.size()) break;
                if (t[i] == ']') { ++i; break; }
                if (t[i] == ',') { ++i; continue; }
                j.a.push_back(value(t, i, depth + 1));
            }
        } else if (c == '"') {
            j.type = Str;
            j.s = string(t, i);
        } else if (t.compare(i, 4, "true") == 0) { j.type = Bool; j.b = true; i += 4; }
        else if (t.compare(i, 5, "false") == 0) { j.type = Bool; i += 5; }
        else if (t.compare(i, 4, "null") == 0) { i += 4; }
        else {
            char* e = nullptr;
            j.n = std::strtod(t.c_str() + i, &e);
            j.type = Num;
            const size_t adv = static_cast<size_t>(e - (t.c_str() + i));
            i += adv ? adv : 1;
        }
        return j;
    }
};

}  // namespace sf
