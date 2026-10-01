#include "sky.h"
#include "log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace xl {

namespace {

constexpr int kCubemap = 89;
constexpr int kRenderSettings = 104;
constexpr double kPi = 3.14159265358979;

float to_linear(uint8_t v) {
    float c = v / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
uint8_t to_srgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f;
    return (uint8_t)std::lround(s * 255.0f);
}

// Bilinear sample of an image with rows bottom-up (Unity's), at u right, v up, both 0..1.
void sample(const Image& img, double u, double v, bool wrap_u, float out[3]) {
    double x = u * img.w - 0.5, y = v * img.h - 0.5;
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    double fx = x - x0, fy = y - y0;
    auto at = [&](int px, int py, int k) {
        px = wrap_u ? ((px % img.w) + img.w) % img.w : std::clamp(px, 0, img.w - 1);
        py = std::clamp(py, 0, img.h - 1);
        return to_linear(img.px[((size_t)py * img.w + px) * 4 + k]);
    };
    for (int k = 0; k < 3; ++k)
        out[k] = (float)((at(x0, y0, k) * (1 - fx) + at(x0 + 1, y0, k) * fx) * (1 - fy) +
                         (at(x0, y0 + 1, k) * (1 - fx) + at(x0 + 1, y0 + 1, k) * fx) * fy);
}

// Unity's Skybox shaders turn the view direction by _Rotation degrees about Y.
void rotate_y(double deg, double& x, double& z) {
    double a = deg * kPi / 180, c = std::cos(a), s = std::sin(a);
    double nx = c * x - s * z, nz = s * x + c * z;
    x = nx, z = nz;
}

struct Source {
    enum Kind { None, Cube, Panorama, SixSided } kind = None;
    ObjRef tex;                  // cube or panorama
    std::array<ObjRef, 6> faces; // six sided: +X -X +Y -Y +Z -Z
    double rotation = 0, exposure = 1;
    float tint[3] = {1, 1, 1};   // linear multiplier
    std::string what;
};

// HDRP: an HDRI Sky override in a volume profile.
bool hdrp_sky(const Database& db, Source& src) {
    for (ObjRef ref : db.objects_of(kMonoBehaviour)) {
        Value mb;
        try {
            mb = db.read(ref);
        } catch (const std::exception&) {
            continue;
        }
        const Value& hdri = mb["hdriSky"];
        if (hdri.is_null()) continue;
        ObjRef tex = db.resolve(ref.file, hdri["m_Value"]);
        if (!tex.valid() || tex.builtin()) continue;
        src.kind = Source::Cube;
        src.tex = tex;
        src.rotation = mb["rotation"]["m_Value"].num(0);
        src.what = "HDRP HDRI sky";
        return true;
    }
    return false;
}

// Built-in render pipeline / URP: RenderSettings' skybox material.
bool skybox_material(const Database& db, Source& src) {
    for (ObjRef ref : db.objects_of(kRenderSettings)) {
        Value rs = db.read(ref);
        ObjRef mat_ref = db.resolve(ref.file, rs["m_SkyboxMaterial"]);
        if (!mat_ref.valid() || mat_ref.builtin()) continue;
        Value m = db.read(mat_ref);
        const Value& props = m["m_SavedProperties"];
        std::map<std::string, ObjRef> tex;
        std::map<std::string, double> num;
        std::map<std::string, std::array<float, 3>> col;
        for (auto& p : props["m_TexEnvs"].items) {
            ObjRef t = db.resolve(mat_ref.file, p["second"]["m_Texture"]);
            if (t.valid() && !t.builtin()) tex[p["first"].s()] = t;
        }
        for (auto& p : props["m_Floats"].items) num[p["first"].s()] = p["second"].num();
        for (auto& p : props["m_Colors"].items) {
            const Value& c = p["second"];
            col[p["first"].s()] = {(float)c["r"].num(1), (float)c["g"].num(1), (float)c["b"].num(1)};
        }
        src.rotation = num.count("_Rotation") ? num["_Rotation"] : 0;
        src.exposure = num.count("_Exposure") ? num["_Exposure"] : 1;
        if (col.count("_Tint")) {  // grey 0.5 is neutral (the shaders double it, in gamma terms)
            for (int k = 0; k < 3; ++k) src.tint[k] = std::pow(col["_Tint"][k] / 0.5f, 2.2f);
        }
        std::string name = m["m_Name"].s();
        static const char* const six[6] = {"_LeftTex", "_RightTex", "_UpTex", "_DownTex", "_FrontTex", "_BackTex"};
        if (tex.count("_Tex")) {
            src.kind = Source::Cube, src.tex = tex["_Tex"];
        } else if (tex.count("_MainTex")) {
            src.kind = db.class_of(tex["_MainTex"]) == kCubemap ? Source::Cube : Source::Panorama;
            src.tex = tex["_MainTex"];
        } else if (tex.count("_FrontTex")) {
            src.kind = Source::SixSided;
            for (int f = 0; f < 6; ++f) src.faces[f] = tex.count(six[f]) ? tex[six[f]] : ObjRef{};
        } else {
            continue;  // procedural: nothing to show as a picture
        }
        src.what = "skybox material " + name;
        return true;
    }
    return false;
}

} // namespace

bool extract_sky(const Database& db, int width, SkyImage& out, std::string& why) {
    Source src;
    bool found = false;
    try {
        found = skybox_material(db, src) || hdrp_sky(db, src);
    } catch (const std::exception& e) {
        why = e.what();
        return false;
    }
    if (!found) {
        why = "the map's sky is procedural or a gradient, not a picture";
        return false;
    }
    // Decode the picture(s): a cubemap's six faces, a panorama, or six separate textures.
    std::array<Image, 6> faces;
    Image pano;
    int face_size = std::max(64, width / 4);
    if (src.kind == Source::Cube) {
        for (int f = 0; f < 6; ++f)
            if (!decode_texture(db, src.tex, face_size, faces[f], why, f)) {
                why = "cubemap: " + why;
                return false;
            }
    } else if (src.kind == Source::Panorama) {
        if (!decode_texture(db, src.tex, width, pano, why)) return false;
    } else {
        for (int f = 0; f < 6; ++f)
            if (!src.faces[f].valid() || !decode_texture(db, src.faces[f], face_size, faces[f], why)) {
                why = "six-sided skybox: " + (why.empty() ? std::string("a face is missing") : why);
                return false;
            }
    }
    int w = width, h = width / 2;
    out.img.w = w, out.img.h = h;
    out.img.px.assign((size_t)w * h * 4, 255);
    for (int y = 0; y < h; ++y) {
        double theta = (y + 0.5) / h * kPi;  // from straight up
        for (int x = 0; x < w; ++x) {
            double phi = ((x + 0.5) / w - 0.5) * 2 * kPi;
            double dx = std::sin(theta) * std::sin(phi), dy = std::cos(theta), dz = std::sin(theta) * std::cos(phi);
            rotate_y(src.rotation, dx, dz);
            float c[3] = {0, 0, 0};
            if (src.kind == Source::Panorama) {
                // Unity's ToRadialCoords: u = 0.5 - atan2(z, x) / 2pi, v = 1 - acos(y) / pi (v up)
                double u = 0.5 - std::atan2(dz, dx) / (2 * kPi), v = 1 - std::acos(std::clamp(dy, -1.0, 1.0)) / kPi;
                sample(pano, u - std::floor(u), v, true, c);
            } else {
                // Cube faces in Unity's (Direct3D) layout: +X -X +Y -Y +Z -Z; s right, t down.
                double ax = std::fabs(dx), ay = std::fabs(dy), az = std::fabs(dz);
                int f;
                double sc, tc, ma;
                if (ax >= ay && ax >= az) f = dx > 0 ? 0 : 1, sc = dx > 0 ? -dz : dz, tc = -dy, ma = ax;
                else if (ay >= az) f = dy > 0 ? 2 : 3, sc = dx, tc = dy > 0 ? dz : -dz, ma = ay;
                else f = dz > 0 ? 4 : 5, sc = dz > 0 ? dx : -dx, tc = -dy, ma = az;
                double u = (sc / ma + 1) * 0.5, t = (tc / ma + 1) * 0.5;
                if (src.kind == Source::SixSided) {  // separate textures: Unity's layout, seen from inside
                    sample(faces[f], u, 1 - t, false, c);
                } else {
                    sample(faces[f], u, t, false, c);  // a cubemap's faces are stored top-down
                }
            }
            uint8_t* o = &out.img.px[((size_t)y * w + x) * 4];
            for (int k = 0; k < 3; ++k) o[k] = to_srgb(c[k] * (float)src.exposure * src.tint[k]);
        }
    }
    out.source = src.what;
    return true;
}

} // namespace xl
