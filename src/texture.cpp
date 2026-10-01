#include "texture.h"
#include "binary.h"

#define BCDEC_IMPLEMENTATION
#include <bcdec.h>
#include <crn_decomp.h>
#include <miniz.h>
#include <astc.h>
#include <etc.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace xl {

namespace {

enum TexFmt {
    kAlpha8 = 1, kARGB4444 = 2, kRGB24 = 3, kRGBA32 = 4, kARGB32 = 5, kRGB565 = 7, kR16 = 9, kDXT1 = 10,
    kDXT3 = 11, kDXT5 = 12, kRGBA4444 = 13, kBGRA32 = 14, kRHalf = 15, kRGHalf = 16, kRGBAHalf = 17,
    kRFloat = 18, kRGFloat = 19, kRGBAFloat = 20, kRGB9e5 = 22, kBC6H = 24, kBC7 = 25, kBC4 = 26, kBC5 = 27,
    kDXT1Crunched = 28, kDXT5Crunched = 29, kRG16 = 62, kR8 = 63, kETC_RGB4Crunched = 64,
    kETC2_RGBA8Crunched = 65, kRG32 = 72, kRGB48 = 73, kRGBA64 = 74,
    kETC_RGB4 = 34, kEAC_R = 41, kEAC_R_SIGNED = 42, kEAC_RG = 43, kEAC_RG_SIGNED = 44, kETC2_RGB = 45,
    kETC2_RGBA1 = 46, kETC2_RGBA8 = 47,
};

// Mobile block formats handled by the texture2ddecoder codecs (ASTC, ETC/EAC).
struct Codec {
    int bw = 0, bh = 0, bytes = 0;
    int kind = 0;  // 1 ASTC, 2 ETC1, 3 ETC2, 4 ETC2A1, 5 ETC2A8, 6 EAC R, 7 EAC R signed, 8 EAC RG, 9 EAC RG signed
};

Codec codec_of(int f) {
    static const int astc[] = {4, 5, 6, 8, 10, 12};
    if (f >= 48 && f <= 53) return {astc[f - 48], astc[f - 48], 16, 1};  // ASTC_4x4 .. 12x12
    if (f >= 54 && f <= 59) return {astc[f - 54], astc[f - 54], 16, 1};  // old ASTC_RGBA_* ids
    if (f >= 66 && f <= 71) return {astc[f - 66], astc[f - 66], 16, 1};  // ASTC_HDR_*
    switch (f) {
    case kETC_RGB4: return {4, 4, 8, 2};
    case kETC2_RGB: return {4, 4, 8, 3};
    case kETC2_RGBA1: return {4, 4, 8, 4};
    case kETC2_RGBA8: return {4, 4, 16, 5};
    case kEAC_R: return {4, 4, 8, 6};
    case kEAC_R_SIGNED: return {4, 4, 8, 7};
    case kEAC_RG: return {4, 4, 16, 8};
    case kEAC_RG_SIGNED: return {4, 4, 16, 9};
    default: return {};
    }
}

bool decode_codec(const Codec& c, const uint8_t* src, size_t size, int w, int h, Image& out) {
    size_t need = (size_t)((w + c.bw - 1) / c.bw) * ((h + c.bh - 1) / c.bh) * c.bytes;
    if (need > size) return false;
    std::vector<uint32_t> px((size_t)w * h);
    int ok = 0;
    switch (c.kind) {
    case 1: ok = decode_astc(src, w, h, c.bw, c.bh, px.data()); break;
    case 2: ok = decode_etc1(src, w, h, px.data()); break;
    case 3: ok = decode_etc2(src, w, h, px.data()); break;
    case 4: ok = decode_etc2a1(src, w, h, px.data()); break;
    case 5: ok = decode_etc2a8(src, w, h, px.data()); break;
    case 6: ok = decode_eacr(src, w, h, px.data()); break;
    case 7: ok = decode_eacr_signed(src, w, h, px.data()); break;
    case 8: ok = decode_eacrg(src, w, h, px.data()); break;
    case 9: ok = decode_eacrg_signed(src, w, h, px.data()); break;
    }
    if (!ok) return false;
    out.w = w;
    out.h = h;
    out.px.resize((size_t)w * h * 4);
    for (size_t i = 0; i < px.size(); ++i) {  // the decoders write BGRA
        uint32_t v = px[i];
        out.px[i * 4] = (uint8_t)(v >> 16);
        out.px[i * 4 + 1] = (uint8_t)(v >> 8);
        out.px[i * 4 + 2] = (uint8_t)v;
        out.px[i * 4 + 3] = (uint8_t)(v >> 24);
    }
    return true;
}

int block_bytes(int f) {
    switch (f) {
    case kDXT1: case kBC4: return 8;
    case kDXT3: case kDXT5: case kBC5: case kBC6H: case kBC7: return 16;
    default: return 0;
    }
}

int pixel_bytes(int f) {
    switch (f) {
    case kAlpha8: case kR8: return 1;
    case kARGB4444: case kRGBA4444: case kRGB565: case kR16: case kRHalf: case kRG16: return 2;
    case kRGB24: return 3;
    case kRGBA32: case kARGB32: case kBGRA32: case kRGHalf: case kRFloat: case kRGB9e5: case kRG32: return 4;
    case kRGB48: return 6;
    case kRGBAHalf: case kRGFloat: case kRGBA64: return 8;
    case kRGBAFloat: return 16;
    default: return 0;
    }
}

uint8_t to8(float v) {
    if (!(v > 0)) return 0;
    if (v >= 1) return 255;
    return (uint8_t)(v * 255.0f + 0.5f);
}

float f32_at(const uint8_t* p) { float f; std::memcpy(&f, p, 4); return f; }
float h16_at(const uint8_t* p) { uint16_t h; std::memcpy(&h, p, 2); return half_to_float(h); }
uint16_t u16_at(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

void decode_pixel(int f, const uint8_t* p, uint8_t* o) {
    o[0] = o[1] = o[2] = 0;
    o[3] = 255;
    switch (f) {
    case kAlpha8: o[0] = o[1] = o[2] = 255; o[3] = p[0]; break;
    case kR8: o[0] = o[1] = o[2] = p[0]; break;
    case kARGB4444: {
        uint16_t v = u16_at(p);
        o[3] = (uint8_t)(((v >> 12) & 15) * 17); o[0] = (uint8_t)(((v >> 8) & 15) * 17);
        o[1] = (uint8_t)(((v >> 4) & 15) * 17); o[2] = (uint8_t)((v & 15) * 17);
        break;
    }
    case kRGBA4444: {
        uint16_t v = u16_at(p);
        o[0] = (uint8_t)(((v >> 12) & 15) * 17); o[1] = (uint8_t)(((v >> 8) & 15) * 17);
        o[2] = (uint8_t)(((v >> 4) & 15) * 17); o[3] = (uint8_t)((v & 15) * 17);
        break;
    }
    case kRGB565: {
        uint16_t v = u16_at(p);
        o[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31); o[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
        o[2] = (uint8_t)((v & 31) * 255 / 31);
        break;
    }
    case kR16: o[0] = o[1] = o[2] = p[1]; break;
    case kRGB24: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; break;
    case kRGBA32: std::memcpy(o, p, 4); break;
    case kARGB32: o[3] = p[0]; o[0] = p[1]; o[1] = p[2]; o[2] = p[3]; break;
    case kBGRA32: o[2] = p[0]; o[1] = p[1]; o[0] = p[2]; o[3] = p[3]; break;
    case kRG16: o[0] = p[0]; o[1] = p[1]; break;
    case kRG32: o[0] = p[1]; o[1] = p[3]; break;
    case kRGB48: o[0] = p[1]; o[1] = p[3]; o[2] = p[5]; break;
    case kRGBA64: o[0] = p[1]; o[1] = p[3]; o[2] = p[5]; o[3] = p[7]; break;
    case kRHalf: o[0] = o[1] = o[2] = to8(h16_at(p)); break;
    case kRGHalf: o[0] = to8(h16_at(p)); o[1] = to8(h16_at(p + 2)); break;
    case kRGBAHalf:
        for (int k = 0; k < 4; ++k) o[k] = to8(h16_at(p + 2 * k));
        break;
    case kRFloat: o[0] = o[1] = o[2] = to8(f32_at(p)); break;
    case kRGFloat: o[0] = to8(f32_at(p)); o[1] = to8(f32_at(p + 4)); break;
    case kRGBAFloat:
        for (int k = 0; k < 4; ++k) o[k] = to8(f32_at(p + 4 * k));
        break;
    case kRGB9e5: {
        uint32_t v;
        std::memcpy(&v, p, 4);
        float scale = std::ldexp(1.0f, (int)(v >> 27) - 15 - 9);
        o[0] = to8((v & 511) * scale); o[1] = to8(((v >> 9) & 511) * scale); o[2] = to8(((v >> 18) & 511) * scale);
        break;
    }
    }
}

// Decodes w x h of 4x4 blocks into RGBA8.
bool decode_blocks(int f, const uint8_t* src, size_t src_size, int w, int h, Image& out) {
    int bx = (w + 3) / 4, by = (h + 3) / 4;
    size_t bb = (size_t)block_bytes(f);
    if ((size_t)bx * by * bb > src_size) return false;
    int pw = bx * 4, ph = by * 4;
    std::vector<uint8_t> tmp((size_t)pw * ph * 4);
    uint8_t block[4 * 4 * 16];
    for (int y = 0; y < by; ++y)
        for (int x = 0; x < bx; ++x) {
            const uint8_t* b = src + ((size_t)y * bx + x) * bb;
            uint8_t* dst = tmp.data() + ((size_t)y * 4 * pw + x * 4) * 4;
            switch (f) {
            case kDXT1: bcdec_bc1(b, dst, pw * 4); break;
            case kDXT3: bcdec_bc2(b, dst, pw * 4); break;
            case kDXT5: bcdec_bc3(b, dst, pw * 4); break;
            case kBC7: bcdec_bc7(b, dst, pw * 4); break;
            case kBC4:
                bcdec_bc4(b, block, 4);
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c) {
                        uint8_t* o = dst + ((size_t)r * pw + c) * 4;
                        o[0] = o[1] = o[2] = block[r * 4 + c];
                        o[3] = 255;
                    }
                break;
            case kBC5:
                bcdec_bc5(b, block, 8);
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c) {
                        uint8_t* o = dst + ((size_t)r * pw + c) * 4;
                        o[0] = block[r * 8 + c * 2];
                        o[1] = block[r * 8 + c * 2 + 1];
                        o[2] = 0;
                        o[3] = 255;
                    }
                break;
            case kBC6H: {
                float fb[4 * 4 * 3];
                bcdec_bc6h_float(b, fb, 12, 0);
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c) {
                        uint8_t* o = dst + ((size_t)r * pw + c) * 4;
                        for (int k = 0; k < 3; ++k) {
                            float v = fb[r * 12 + c * 3 + k];
                            o[k] = to8(std::pow(std::max(0.0f, v) / (1.0f + std::max(0.0f, v)), 1.0f / 2.2f));
                        }
                        o[3] = 255;
                    }
                break;
            }
            default: return false;
            }
        }
    out.w = w;
    out.h = h;
    out.px.resize((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) std::memcpy(out.px.data() + (size_t)y * w * 4, tmp.data() + (size_t)y * pw * 4, (size_t)w * 4);
    return true;
}

bool decode_crunched(const uint8_t* src, size_t size, int max_size, Image& out, std::string& why) {
    crnd::crn_texture_info ti;
    if (!crnd::crnd_get_texture_info(src, (crnd::uint32)size, &ti)) { why = "bad crunch header"; return false; }
    uint32_t level = 0;
    while (max_size > 0 && level + 1 < ti.m_levels && std::max(ti.m_width >> level, ti.m_height >> level) > (uint32_t)max_size)
        ++level;
    int w = (int)std::max(1u, ti.m_width >> level), h = (int)std::max(1u, ti.m_height >> level);
    int f;
    switch (ti.m_format) {
    case cCRNFmtDXT1: f = kDXT1; break;
    case cCRNFmtDXT3: f = kDXT3; break;
    case cCRNFmtDXT5: case cCRNFmtDXT5_CCxY: case cCRNFmtDXT5_xGxR: case cCRNFmtDXT5_xGBR: case cCRNFmtDXT5_AGBR:
        f = kDXT5; break;
    case cCRNFmtDXN_XY: case cCRNFmtDXN_YX: f = kBC5; break;
    case cCRNFmtDXT5A: f = kBC4; break;
    case cCRNFmtETC1: case cCRNFmtETC1S: f = kETC_RGB4; break;
    case cCRNFmtETC2: f = kETC2_RGB; break;
    case cCRNFmtETC2A: case cCRNFmtETC2AS: f = kETC2_RGBA8; break;
    default: why = "unsupported crunch format"; return false;
    }
    crnd::crnd_unpack_context ctx = crnd::crnd_unpack_begin(src, (crnd::uint32)size);
    if (!ctx) { why = "crunch unpack failed"; return false; }
    uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    uint32_t pitch = bx * crnd::crnd_get_bytes_per_dxt_block(ti.m_format);
    std::vector<uint8_t> blocks((size_t)pitch * by);
    void* faces[1] = {blocks.data()};
    bool ok = crnd::crnd_unpack_level(ctx, faces, (crnd::uint32)blocks.size(), pitch, level);
    crnd::crnd_unpack_end(ctx);
    if (!ok) { why = "crunch level unpack failed"; return false; }
    Codec codec = codec_of(f);
    if (codec.kind) return decode_codec(codec, blocks.data(), blocks.size(), w, h, out);
    return decode_blocks(f, blocks.data(), blocks.size(), w, h, out);
}

} // namespace

TexInfo texture_info(const Value& t) {
    TexInfo i;
    i.name = t["m_Name"].s();
    i.width = (int)t["m_Width"].i64();
    i.height = (int)t["m_Height"].i64();
    i.format = (int)t["m_TextureFormat"].i64();
    i.mips = std::max<int>(1, (int)t["m_MipCount"].i64(1));
    const Value& s = t["m_TextureSettings"];
    i.wrap_u = (int)(s.has("m_WrapU") ? s["m_WrapU"].i64() : s["m_WrapMode"].i64());
    i.wrap_v = (int)(s.has("m_WrapV") ? s["m_WrapV"].i64() : s["m_WrapMode"].i64());
    i.filter = (int)s["m_FilterMode"].i64(1);
    return i;
}

const char* texture_format_name(int f) {
    switch (f) {
    case kAlpha8: return "Alpha8"; case kARGB4444: return "ARGB4444"; case kRGB24: return "RGB24";
    case kRGBA32: return "RGBA32"; case kARGB32: return "ARGB32"; case kRGB565: return "RGB565";
    case kR16: return "R16"; case kDXT1: return "DXT1"; case kDXT3: return "DXT3"; case kDXT5: return "DXT5";
    case kRGBA4444: return "RGBA4444"; case kBGRA32: return "BGRA32"; case kRHalf: return "RHalf";
    case kRGHalf: return "RGHalf"; case kRGBAHalf: return "RGBAHalf"; case kRFloat: return "RFloat";
    case kRGFloat: return "RGFloat"; case kRGBAFloat: return "RGBAFloat"; case kRGB9e5: return "RGB9e5";
    case kBC6H: return "BC6H"; case kBC7: return "BC7"; case kBC4: return "BC4"; case kBC5: return "BC5";
    case kDXT1Crunched: return "DXT1Crunched"; case kDXT5Crunched: return "DXT5Crunched"; case kRG16: return "RG16";
    case kR8: return "R8"; case kETC_RGB4Crunched: return "ETC_RGB4Crunched";
    case kETC2_RGBA8Crunched: return "ETC2_RGBA8Crunched";
    case kETC_RGB4: return "ETC_RGB4"; case kETC2_RGB: return "ETC2_RGB"; case kETC2_RGBA1: return "ETC2_RGBA1";
    case kETC2_RGBA8: return "ETC2_RGBA8"; case kEAC_R: return "EAC_R"; case kEAC_RG: return "EAC_RG";
    case 48: case 54: return "ASTC_4x4"; case 49: case 55: return "ASTC_5x5"; case 50: case 56: return "ASTC_6x6";
    case 51: case 57: return "ASTC_8x8"; case 52: case 58: return "ASTC_10x10"; case 53: case 59: return "ASTC_12x12";
    default: return "unknown";
    }
}

bool decode_texture(const Database& db, ObjRef ref, int max_size, Image& out, std::string& why, int face) {
    if (ref.builtin()) { why = "built-in texture"; return false; }
    Value t = db.read(ref);
    if (t.is_null()) { why = "missing texture"; return false; }
    TexInfo info = texture_info(t);
    const Value& img = t["image data"];
    const uint8_t* data = img.kind == Kind::Bytes ? img.data : nullptr;
    size_t size = img.kind == Kind::Bytes ? img.size : 0;
    if (size == 0) {
        const Value& sd = t["m_StreamData"];
        if (!sd.is_null() && sd["size"].i64() > 0) {
            auto res = db.resource(sd["path"].s(), (uint64_t)sd["offset"].i64(), (uint64_t)sd["size"].i64());
            data = res.first;
            size = res.second;
            if (!data) { why = "streamed data file '" + sd["path"].s() + "' is missing"; return false; }
        }
    }
    if (!data || size == 0 || info.width <= 0 || info.height <= 0) { why = "no image data"; return false; }
    int f = info.format;
    if (f == kDXT1Crunched || f == kDXT5Crunched || f == kETC_RGB4Crunched || f == kETC2_RGBA8Crunched) {
        if (face) { why = "crunched cubemap faces are not supported"; return false; }
        return decode_crunched(data, size, max_size, out, why);
    }

    int bb = block_bytes(f), pb = pixel_bytes(f);
    Codec codec = codec_of(f);
    if (!bb && !pb && !codec.kind) {
        why = std::string("unsupported format ") + texture_format_name(f) + " (" + std::to_string(f) + ")";
        return false;
    }
    int level = 0;
    auto level_size = [&](int l) -> size_t {
        int w = std::max(1, info.width >> l), h = std::max(1, info.height >> l);
        if (codec.kind) return (size_t)((w + codec.bw - 1) / codec.bw) * ((h + codec.bh - 1) / codec.bh) * codec.bytes;
        return bb ? (size_t)((w + 3) / 4) * ((h + 3) / 4) * bb : (size_t)w * h * pb;
    };
    size_t offset = 0;
    if (face > 0) {  // skip the faces before it, each a whole mip chain
        size_t chain = 0;
        for (int l = 0; l < info.mips; ++l) chain += level_size(l);
        if (chain * (face + 1) > size) { why = "cubemap face " + std::to_string(face) + " is missing"; return false; }
        offset = chain * face;
        data += offset, size -= offset, offset = 0;
    }
    while (max_size > 0 && level + 1 < info.mips &&
           std::max(info.width >> level, info.height >> level) > max_size &&
           offset + level_size(level) + level_size(level + 1) <= size) {
        offset += level_size(level);
        ++level;
    }
    int w = std::max(1, info.width >> level), h = std::max(1, info.height >> level);
    if (offset + level_size(level) > size) { why = "image data is truncated"; return false; }
    if (codec.kind) {
        if (!decode_codec(codec, data + offset, size - offset, w, h, out)) { why = "block decode failed"; return false; }
        return true;
    }
    if (bb) {
        if (!decode_blocks(f, data + offset, size - offset, w, h, out)) { why = "block decode failed"; return false; }
        return true;
    }
    out.w = w;
    out.h = h;
    out.px.resize((size_t)w * h * 4);
    const uint8_t* p = data + offset;
    for (size_t i = 0; i < (size_t)w * h; ++i) decode_pixel(f, p + i * pb, out.px.data() + i * 4);
    return true;
}

std::vector<uint8_t> encode_png(const Image& img, int channels) {
    std::vector<uint8_t> packed;
    const void* src = img.px.data();
    if (channels == 3) {
        packed.resize((size_t)img.w * img.h * 3);
        for (size_t i = 0; i < (size_t)img.w * img.h; ++i) std::memcpy(&packed[i * 3], &img.px[i * 4], 3);
        src = packed.data();
    }
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(src, img.w, img.h, channels, &len, 6, MZ_TRUE);
    std::vector<uint8_t> out;
    if (png) {
        out.assign((uint8_t*)png, (uint8_t*)png + len);
        mz_free(png);
    }
    return out;
}

} // namespace xl
