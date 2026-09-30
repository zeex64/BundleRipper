#pragma once
#include "unity.h"
#include <cstdint>
#include <string>
#include <vector>

namespace xl {

// RGBA8 pixels with rows bottom-up, the way Unity stores them.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    bool has_alpha() const {
        for (size_t i = 3; i < px.size(); i += 4)
            if (px[i] != 255) return true;
        return false;
    }
};

struct TexInfo {
    std::string name;
    int width = 0, height = 0, format = 0, mips = 1;
    int wrap_u = 0, wrap_v = 0;  // 0 repeat, 1 clamp, 2 mirror, 3 mirror once
    int filter = 1;              // 0 point
};

TexInfo texture_info(const Value& tex);
const char* texture_format_name(int format);
// Decodes the largest mip level no bigger than max_size (0 = full size).
bool decode_texture(const Database& db, ObjRef ref, int max_size, Image& out, std::string& why);

// PNG bytes of an image (flipped to top-down rows); channels 3 or 4.
std::vector<uint8_t> encode_png(const Image& img, int channels);

} // namespace xl
