#include "json_parse.h"

#include <cstdlib>

namespace xl {

namespace {

class Parser {
public:
    explicit Parser(const std::string& t) : t_(t) {}
    bool parse(Json& out, std::string& error) {
        try {
            out = value();
            space();
            if (i_ != t_.size()) fail("unexpected text after the value");
            return true;
        } catch (const std::string& e) {
            error = e;
            return false;
        }
    }

private:
    const std::string& t_;
    size_t i_ = 0;

    [[noreturn]] void fail(const char* what) {
        size_t line = 1;
        for (size_t k = 0; k < i_ && k < t_.size(); ++k) line += t_[k] == '\n';
        throw std::string(what) + " (line " + std::to_string(line) + ")";
    }
    void space() {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) ++i_;
    }
    bool take(char c) {
        space();
        if (i_ < t_.size() && t_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }
    void expect(char c) {
        if (!take(c)) fail("unexpected character");
    }
    bool word(const char* w) {
        size_t n = std::char_traits<char>::length(w);
        if (t_.compare(i_, n, w) != 0) return false;
        i_ += n;
        return true;
    }

    Json value() {
        space();
        if (i_ >= t_.size()) fail("unexpected end");
        char c = t_[i_];
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') return Json(string());
        if (word("true")) return Json(true);
        if (word("false")) return Json(false);
        if (word("null")) return Json();
        return number();
    }

    Json object() {
        expect('{');
        Json j = Json::object();
        if (take('}')) return j;
        do {
            space();
            if (i_ >= t_.size() || t_[i_] != '"') fail("expected a key");
            std::string k = string();
            expect(':');
            j.o.emplace_back(std::move(k), value());
        } while (take(','));
        expect('}');
        return j;
    }

    Json array() {
        expect('[');
        Json j = Json::array();
        if (take(']')) return j;
        do j.a.push_back(value());
        while (take(','));
        expect(']');
        return j;
    }

    void utf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) s += (char)(0xC0 | cp >> 6), s += (char)(0x80 | (cp & 63));
        else if (cp < 0x10000) s += (char)(0xE0 | cp >> 12), s += (char)(0x80 | (cp >> 6 & 63)), s += (char)(0x80 | (cp & 63));
        else s += (char)(0xF0 | cp >> 18), s += (char)(0x80 | (cp >> 12 & 63)), s += (char)(0x80 | (cp >> 6 & 63)), s += (char)(0x80 | (cp & 63));
    }
    uint32_t hex4() {
        if (i_ + 4 > t_.size()) fail("bad \\u escape");
        uint32_t v = (uint32_t)std::strtoul(t_.substr(i_, 4).c_str(), nullptr, 16);
        i_ += 4;
        return v;
    }

    std::string string() {
        ++i_;  // opening quote
        std::string s;
        while (true) {
            if (i_ >= t_.size()) fail("unterminated string");
            char c = t_[i_++];
            if (c == '"') break;
            if (c != '\\') {
                s += c;
                continue;
            }
            if (i_ >= t_.size()) fail("unterminated string");
            char e = t_[i_++];
            switch (e) {
            case 'n': s += '\n'; break;
            case 't': s += '\t'; break;
            case 'r': s += '\r'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'u': {
                uint32_t cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && t_.compare(i_, 2, "\\u") == 0) {  // surrogate pair
                    i_ += 2;
                    uint32_t lo = hex4();
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(s, cp);
                break;
            }
            default: s += e; break;  // \" \\ \/
            }
        }
        return s;
    }

    Json number() {
        size_t start = i_;
        bool real = false;
        if (i_ < t_.size() && (t_[i_] == '-' || t_[i_] == '+')) ++i_;
        while (i_ < t_.size()) {
            char c = t_[i_];
            if (c >= '0' && c <= '9') ++i_;
            else if (c == '.' || c == 'e' || c == 'E' || ((c == '-' || c == '+') && real)) real = true, ++i_;
            else break;
        }
        if (i_ == start) fail("unexpected character");
        std::string text = t_.substr(start, i_ - start);
        if (!real) return Json((int64_t)std::strtoll(text.c_str(), nullptr, 10));
        return Json(std::strtod(text.c_str(), nullptr));
    }
};

void pretty(const Json& j, std::string& out, int depth) {
    auto indent = [&](int d) { out += '\n' + std::string((size_t)d * 2, ' '); };
    if (j.t == Json::Obj && !j.o.empty()) {
        out += '{';
        for (size_t k = 0; k < j.o.size(); ++k) {
            if (k) out += ',';
            indent(depth + 1);
            Json::quote(out, j.o[k].first);
            out += ": ";
            pretty(j.o[k].second, out, depth + 1);
        }
        indent(depth);
        out += '}';
    } else if (j.t == Json::Arr && !j.a.empty()) {
        out += '[';
        for (size_t k = 0; k < j.a.size(); ++k) {
            if (k) out += ',';
            indent(depth + 1);
            pretty(j.a[k], out, depth + 1);
        }
        indent(depth);
        out += ']';
    } else {
        j.dump(out);
    }
}

} // namespace

bool parse_json(const std::string& text, Json& out, std::string& error) { return Parser(text).parse(out, error); }

std::string dump_pretty(const Json& j) {
    std::string out;
    pretty(j, out, 0);
    out += '\n';
    return out;
}

} // namespace xl
