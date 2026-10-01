#include "images.h"
#include "log.h"
#include "texture.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

namespace xl {

namespace {

// Nearest sample of `img` at the same relative position as (x, y) in a w x h image.
const uint8_t* sample(const Image& img, int x, int y, int w, int h) {
    int sx = img.w == w ? x : std::min(img.w - 1, (int)((int64_t)x * img.w / w));
    int sy = img.h == h ? y : std::min(img.h - 1, (int)((int64_t)y * img.h / h));
    return &img.px[((size_t)sy * img.w + sx) * 4];
}

uint8_t unit8(double v) { return (uint8_t)std::lround(std::clamp(v, 0.0, 1.0) * 255.0); }

bool process(const Database& db, ImageJob& job, int max_size, std::string& why) {
    Image src[3];
    bool have[3] = {false, false, false};
    for (int k = 0; k < 3; ++k) {
        if (!job.src[k].valid()) continue;
        std::string w;
        have[k] = decode_texture(db, job.src[k], max_size, src[k], w);
        if (!have[k]) {
            why = w;
            bool optional = job.role == ImageJob::PackORM || (job.role == ImageJob::MetallicStd && k > 0);
            if (!optional) return false;
        }
    }
    int lead = have[0] ? 0 : have[1] ? 1 : have[2] ? 2 : -1;
    if (lead < 0) return false;
    try {
        TexInfo info = texture_info(db.read(job.src[lead]));
        job.wrap_u = info.wrap_u;
        job.wrap_v = info.wrap_v;
        job.filter = info.filter;
    } catch (const std::exception&) {
    }
    Image& out = src[lead];
    int w = out.w, h = out.h;
    int channels = 3;
    bool colour = job.role == ImageJob::Color || job.role == ImageJob::Raw || job.role == ImageJob::Emissive;
    if (colour && job.params[4] > 0.5f) {  // bake the material's linear colour factor into the sRGB texels
        static const auto tables = [] {
            std::pair<std::vector<float>, std::vector<uint8_t>> t{std::vector<float>(256), std::vector<uint8_t>(4096)};
            for (int i = 0; i < 256; ++i) {
                double c = i / 255.0;
                t.first[i] = (float)(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
            }
            for (int i = 0; i < 4096; ++i) {
                double l = i / 4095.0;
                t.second[i] = unit8(l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1 / 2.4) - 0.055);
            }
            return t;
        }();
        const float* q = job.params;
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            uint8_t* p = &out.px[i * 4];
            for (int k = 0; k < 3; ++k) {
                float l = std::clamp(tables.first[p[k]] * q[k], 0.0f, 1.0f);
                p[k] = tables.second[(size_t)(l * 4095.0f + 0.5f)];
            }
            p[3] = unit8(p[3] / 255.0 * q[3]);
        }
    }
    switch (job.role) {
    case ImageJob::Color:
    case ImageJob::Raw: {
        channels = out.has_alpha() ? 4 : 3;
        size_t clear = 0, solid = 0, n = (size_t)w * h;
        for (size_t i = 0; i < n; ++i) {
            uint8_t a = out.px[i * 4 + 3];
            clear += a < 32;
            solid += a > 223;
        }
        job.cutout = n && clear >= n / 20 && solid >= n / 5;
        break;
    }
    case ImageJob::PackORM: {
        const float* q = job.params;
        Image res;
        res.w = w;
        res.h = h;
        res.px.resize((size_t)w * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                double ao = have[0] ? sample(src[0], x, y, w, h)[0] / 255.0 : 1.0;
                double rough = q[1];
                if (have[1]) {
                    double v = sample(src[1], x, y, w, h)[0] / 255.0;
                    rough = q[0] > 0.5f ? 1 - v : v;
                }
                double metal = have[2] ? sample(src[2], x, y, w, h)[0] / 255.0 : q[2];
                uint8_t* p = &res.px[((size_t)y * w + x) * 4];
                p[0] = unit8(ao);
                p[1] = unit8(rough);
                p[2] = unit8(metal);
                p[3] = 255;
            }
        out = std::move(res);
        break;
    }
    case ImageJob::Emissive:
        break;
    case ImageJob::Normal:
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            uint8_t* p = &out.px[i * 4];
            double x = (p[0] / 255.0) * (p[3] / 255.0) * 2 - 1;  // DXT5nm keeps X in alpha
            double y = p[1] / 255.0 * 2 - 1;
            double z = std::sqrt(std::max(0.0, 1 - x * x - y * y));
            p[0] = unit8(x * 0.5 + 0.5);
            p[1] = unit8(y * 0.5 + 0.5);
            p[2] = unit8(z * 0.5 + 0.5);
            p[3] = 255;
        }
        break;
    case ImageJob::MaskHDRP: {
        const float* q = job.params;
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            uint8_t* p = &out.px[i * 4];
            double metal = q[4] + (q[5] - q[4]) * (p[0] / 255.0);
            double ao = q[2] + (q[3] - q[2]) * (p[1] / 255.0);
            double smooth = q[0] + (q[1] - q[0]) * (p[3] / 255.0);
            p[0] = unit8(ao);
            p[1] = unit8(1 - smooth);
            p[2] = unit8(metal);
            p[3] = 255;
        }
        break;
    }
    case ImageJob::MetallicStd: {
        const float* q = job.params;
        Image res;
        res.w = w;
        res.h = h;
        res.px.resize((size_t)w * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                double metal = q[0], smooth_src = 1, occ = 1;
                if (have[0] && q[3] > 0.5f) {
                    const uint8_t* m = sample(src[0], x, y, w, h);
                    metal = m[0] / 255.0;
                    smooth_src = m[3] / 255.0;
                }
                if (have[2] && q[4] > 0.5f) smooth_src = sample(src[2], x, y, w, h)[3] / 255.0;
                if (have[1]) occ = 1 - q[2] + q[2] * (sample(src[1], x, y, w, h)[1] / 255.0);
                uint8_t* p = &res.px[((size_t)y * w + x) * 4];
                p[0] = unit8(occ);
                p[1] = unit8(1 - smooth_src * q[1]);
                p[2] = unit8(metal);
                p[3] = 255;
            }
        out = std::move(res);
        break;
    }
    }
    if (job.keep_pixels) {
        job.px_w = out.w;
        job.px_h = out.h;
        job.pixels = std::move(out.px);
        return true;
    }
    job.png = encode_png(out, channels);
    return !job.png.empty();
}

} // namespace

void run_image_jobs(const Database& db, std::vector<ImageJob>& jobs, int max_size) {
    std::atomic<size_t> next{0}, done{0};
    std::mutex log_mutex;
    size_t total = jobs.size();
    auto worker = [&] {
        for (size_t i; (i = next++) < total;) {
            std::string why;
            try {
                jobs[i].ok = process(db, jobs[i], max_size, why);
            } catch (const std::exception& e) {
                why = e.what();
                jobs[i].ok = false;
            }
            size_t d = ++done;
            std::lock_guard<std::mutex> lock(log_mutex);
            if (!jobs[i].ok) log_warn("texture '%s': %s", jobs[i].name.c_str(), why.empty() ? "decode failed" : why.c_str());
            if (total >= 20 && d % std::max<size_t>(1, total / 10) == 0) log_info("  textures %zu/%zu", d, total);
        }
    };
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
}

} // namespace xl
