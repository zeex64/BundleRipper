// Minimal JSON value for building the glTF document.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace xl {

struct Json {
    enum T { Null, Bool, Int, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    int64_t i = 0;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    Json() = default;
    Json(bool v) : t(Bool), b(v) {}
    Json(int v) : t(Int), i(v) {}
    Json(unsigned v) : t(Int), i(v) {}
    Json(int64_t v) : t(Int), i(v) {}
    Json(uint64_t v) : t(Int), i((int64_t)v) {}
    Json(long v) : t(Int), i(v) {}
    Json(unsigned long v) : t(Int), i((int64_t)v) {}
    Json(double v) : t(Num), n(v) {}
    Json(float v) : t(Num), n(v) {}
    Json(const char* v) : t(Str), s(v) {}
    Json(std::string v) : t(Str), s(std::move(v)) {}

    static Json array() { Json j; j.t = Arr; return j; }
    static Json object() { Json j; j.t = Obj; return j; }
    template <class... A> static Json list(A... v) {
        Json j = array();
        (j.a.push_back(Json(v)), ...);
        return j;
    }

    Json& set(const std::string& k, Json v) {
        if (t != Obj) t = Obj;
        for (auto& p : o)
            if (p.first == k) { p.second = std::move(v); return p.second; }
        o.emplace_back(k, std::move(v));
        return o.back().second;
    }
    // Member `k`, created as an empty object when missing.
    Json& child(const std::string& k) {
        if (t != Obj) t = Obj;
        for (auto& p : o)
            if (p.first == k) return p.second;
        o.emplace_back(k, object());
        return o.back().second;
    }
    Json& push(Json v) {
        if (t != Arr) t = Arr;
        a.push_back(std::move(v));
        return a.back();
    }
    bool empty() const { return (t == Obj && o.empty()) || (t == Arr && a.empty()) || t == Null; }

    void dump(std::string& out) const {
        switch (t) {
        case Null: out += "null"; break;
        case Bool: out += b ? "true" : "false"; break;
        case Int: out += std::to_string(i); break;
        case Num: {
            if (!std::isfinite(n)) { out += "0"; break; }
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.9g", n);
            out += buf;
            bool point = false;  // keep a decimal point: readers that type values (Blender) see a float
            for (const char* c = buf; *c; ++c) point = point || *c == '.' || *c == 'e' || *c == 'E';
            if (!point) out += ".0";
            break;
        }
        case Str: quote(out, s); break;
        case Arr:
            out += '[';
            for (size_t k = 0; k < a.size(); ++k) {
                if (k) out += ',';
                a[k].dump(out);
            }
            out += ']';
            break;
        case Obj:
            out += '{';
            for (size_t k = 0; k < o.size(); ++k) {
                if (k) out += ',';
                quote(out, o[k].first);
                out += ':';
                o[k].second.dump(out);
            }
            out += '}';
            break;
        }
    }

    static void quote(std::string& out, const std::string& v) {
        out += '"';
        for (unsigned char c : v) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
            }
        }
        out += '"';
    }
};

} // namespace xl
