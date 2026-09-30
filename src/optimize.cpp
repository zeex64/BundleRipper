#include "optimize.h"
#include "log.h"
#include "meshoptimizer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace xl {

namespace {

// How far an attribute may drift, measured like the position error: at these deviations an
// attribute alone uses up the whole error budget.
constexpr float kNormalTolerance = 0.02f;         // ~1.1 degrees
constexpr float kUvTolerance = 1.0f / 2048.0f;    // half a texel of a 1024 texture (after tiling)
constexpr float kColorTolerance = 1.0f / 128.0f;
// meshopt weighs attribute error by triangle area, so on the small triangles of a big mesh the
// attributes barely steer it; this makes it choose collapses the check below accepts more often.
constexpr float kAttributeBoost = 10.0f;
// The check every simplified submesh must pass (see misses()).
const float kVerifyNormalCos = (float)std::cos(3.0 * 3.14159265358979 / 180.0);  // 3 degrees
constexpr float kVerifyUv = 1.0f / 1024.0f;  // one texel of a 1024 texture (after tiling)
constexpr float kVerifyColor = 1.0f / 64.0f;
constexpr int kVerifyRounds = 16;

// What the scene does with one MeshData.
struct Use {
    double scale = 0;    // largest world stretch over its placed copies (0 = never placed)
    size_t copies = 0;
    bool lines = false;  // line list: left alone
    bool decal = false;  // sits 2 mm above the surface: weld only, never move toward it
    bool uv_used[4] = {true, false, false, false};
    std::vector<float> tiling;  // per submesh: largest material UV tiling the writer multiplies in
};

double stretch(const V3& s) { return std::max({std::fabs(s.x), std::fabs(s.y), std::fabs(s.z)}); }

struct TriKey {
    uint32_t a, b, c;
    bool operator==(const TriKey& o) const { return a == o.a && b == o.b && c == o.c; }
};
struct TriKeyHash {
    size_t operator()(const TriKey& k) const {
        uint64_t h = k.a * 0x9E3779B97F4A7C15ull;
        h ^= (k.b + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2));
        h ^= (k.c + 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2));
        return (size_t)h;
    }
};

struct Result {
    size_t v0 = 0, v1 = 0, t0 = 0, t1 = 0;
    bool simplified = false;
    std::vector<std::pair<size_t, size_t>> whole;  // (submesh, triangles) never passing the check: kept as is
};

// Float offsets inside one interleaved vertex (-1 = absent).
struct Layout {
    size_t stride = 3;
    int nrm = -1, col = -1, uv[4] = {-1, -1, -1, -1};
};

// What the simplified surface must still match at every point of the original one.
struct Tolerance {
    float pos = 0;      // mesh units
    float nrm_cos = 1;  // smallest cosine between the normals
    float uv[4] = {};
    float col = 0;
};

// Bounding-volume tree over a triangle list, for "is any triangle near this point" queries.
class TriBvh {
public:
    TriBvh(const float* v, size_t stride, const std::vector<uint32_t>& idx) : v_(v), stride_(stride), idx_(idx) {
        size_t n = idx.size() / 3;
        order_.resize(n);
        boxes_.resize(n);
        for (size_t t = 0; t < n; ++t) {
            order_[t] = (uint32_t)t;
            Box& b = boxes_[t];
            for (int k = 0; k < 3; ++k) b.lo[k] = 1e30f, b.hi[k] = -1e30f;
            for (int c = 0; c < 3; ++c) grow(b, &v[idx[t * 3 + c] * stride]);
        }
        if (n) build(0, n);
    }

    // Calls f(triangle) for the triangles whose box overlaps [lo, hi] until f returns true.
    template <class F>
    bool any(const float lo[3], const float hi[3], F&& f) const {
        if (nodes_.empty()) return false;
        uint32_t stack[128];
        int sp = 0;
        stack[sp++] = 0;
        while (sp) {
            uint32_t id = stack[--sp];
            const Node& nd = nodes_[id];
            if (!overlaps(nd.box, lo, hi)) continue;
            if (nd.count) {
                for (uint32_t k = 0; k < nd.count; ++k) {
                    uint32_t t = order_[nd.first + k];
                    if (overlaps(boxes_[t], lo, hi) && f(t)) return true;
                }
            } else if (sp + 2 <= 128) {
                stack[sp++] = id + 1;
                stack[sp++] = nd.right;
            }
        }
        return false;
    }

private:
    struct Box {
        float lo[3], hi[3];
    };
    struct Node {
        Box box;
        uint32_t first = 0, count = 0, right = 0;  // leaf: order_[first, first + count); inner: children id+1, right
    };
    const float* v_;
    size_t stride_;
    const std::vector<uint32_t>& idx_;
    std::vector<uint32_t> order_;
    std::vector<Box> boxes_;
    std::vector<Node> nodes_;

    static void grow(Box& b, const float* p) {
        for (int k = 0; k < 3; ++k) {
            b.lo[k] = std::min(b.lo[k], p[k]);
            b.hi[k] = std::max(b.hi[k], p[k]);
        }
    }
    static bool overlaps(const Box& b, const float lo[3], const float hi[3]) {
        for (int k = 0; k < 3; ++k)
            if (b.lo[k] > hi[k] || b.hi[k] < lo[k]) return false;
        return true;
    }
    uint32_t build(size_t first, size_t count) {
        uint32_t id = (uint32_t)nodes_.size();
        nodes_.emplace_back();
        Box b, c;
        for (int k = 0; k < 3; ++k) b.lo[k] = c.lo[k] = 1e30f, b.hi[k] = c.hi[k] = -1e30f;
        for (size_t i = first; i < first + count; ++i) {
            const Box& t = boxes_[order_[i]];
            grow(b, t.lo);
            grow(b, t.hi);
            float mid[3] = {(t.lo[0] + t.hi[0]) * 0.5f, (t.lo[1] + t.hi[1]) * 0.5f, (t.lo[2] + t.hi[2]) * 0.5f};
            grow(c, mid);
        }
        nodes_[id].box = b;
        if (count <= 4) {
            nodes_[id].first = (uint32_t)first;
            nodes_[id].count = (uint32_t)count;
            return id;
        }
        int axis = 0;
        for (int k = 1; k < 3; ++k)
            if (c.hi[k] - c.lo[k] > c.hi[axis] - c.lo[axis]) axis = k;
        size_t half = first + count / 2;
        std::nth_element(order_.begin() + first, order_.begin() + half, order_.begin() + first + count,
                         [&](uint32_t x, uint32_t y) {
                             return boxes_[x].lo[axis] + boxes_[x].hi[axis] < boxes_[y].lo[axis] + boxes_[y].hi[axis];
                         });
        build(first, half - first);
        uint32_t right = build(half, first + count - half);
        nodes_[id].right = right;
        return id;
    }
};

// Barycentric weights of the point of triangle abc closest to p (Ericson, Real-Time Collision Detection 5.1.5).
void closest_on_triangle(const double p[3], const double a[3], const double b[3], const double c[3], double w[3]) {
    auto sub = [](const double* x, const double* y, double* o) { o[0] = x[0] - y[0], o[1] = x[1] - y[1], o[2] = x[2] - y[2]; };
    auto dot = [](const double* x, const double* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    auto set = [&](double a0, double a1, double a2) { w[0] = a0, w[1] = a1, w[2] = a2; };
    double ab[3], ac[3], ap[3], bp[3], cp[3];
    sub(b, a, ab), sub(c, a, ac), sub(p, a, ap);
    double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return set(1, 0, 0);
    sub(p, b, bp);
    double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return set(0, 1, 0);
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        double t = d1 / (d1 - d3);
        return set(1 - t, t, 0);
    }
    sub(p, c, cp);
    double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return set(0, 0, 1);
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        double t = d2 / (d2 - d6);
        return set(1 - t, 0, t);
    }
    double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        double t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return set(0, 1 - t, t);
    }
    double den = va + vb + vc;
    if (std::fabs(den) < 1e-300) return set(1, 0, 0);
    w[1] = vb / den;
    w[2] = vc / den;
    w[0] = 1 - w[1] - w[2];
}

// The original vertices a simplified submesh no longer follows: every vertex it dropped, and
// the centre of every triangle it replaced, must lie within the tolerance of some new triangle
// whose interpolated normal, UVs and colour there are within theirs too (any one of the
// triangles meeting at a seam will do).
std::vector<uint32_t> misses(const std::vector<float>& w, size_t vertex_count, const Layout& L, const Tolerance& tol,
                             const std::vector<uint32_t>& before, const std::vector<uint32_t>& after) {
    TriBvh bvh(w.data(), L.stride, after);
    std::vector<uint8_t> kept(vertex_count, 0);
    for (uint32_t i : after) kept[i] = 1;
    std::unordered_set<TriKey, TriKeyHash> kept_tris;
    auto key_of = [](uint32_t a, uint32_t b, uint32_t c) {
        return a < b && a < c ? TriKey{a, b, c} : b < c ? TriKey{b, c, a} : TriKey{c, a, b};
    };
    for (size_t t = 0; t + 2 < after.size(); t += 3) kept_tris.insert(key_of(after[t], after[t + 1], after[t + 2]));

    const size_t S = L.stride;
    auto fits = [&](const float* s) {
        float lo[3], hi[3];
        for (int k = 0; k < 3; ++k) lo[k] = s[k] - tol.pos, hi[k] = s[k] + tol.pos;
        double p[3] = {s[0], s[1], s[2]};
        double sn = 0;
        if (L.nrm >= 0) sn = std::sqrt((double)s[L.nrm] * s[L.nrm] + (double)s[L.nrm + 1] * s[L.nrm + 1] + (double)s[L.nrm + 2] * s[L.nrm + 2]);
        return bvh.any(lo, hi, [&](uint32_t t) {
            const float* v[3] = {&w[after[t * 3] * S], &w[after[t * 3 + 1] * S], &w[after[t * 3 + 2] * S]};
            double a[3] = {v[0][0], v[0][1], v[0][2]}, b[3] = {v[1][0], v[1][1], v[1][2]}, c[3] = {v[2][0], v[2][1], v[2][2]};
            double bw[3];
            closest_on_triangle(p, a, b, c, bw);
            double d2 = 0;
            for (int k = 0; k < 3; ++k) {
                double q = a[k] * bw[0] + b[k] * bw[1] + c[k] * bw[2] - p[k];
                d2 += q * q;
            }
            if (d2 > (double)tol.pos * tol.pos) return false;
            auto at = [&](int off) { return v[0][off] * bw[0] + v[1][off] * bw[1] + v[2][off] * bw[2]; };
            if (L.nrm >= 0 && sn > 1e-6) {
                double n[3] = {at(L.nrm), at(L.nrm + 1), at(L.nrm + 2)};
                double ln = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (ln < 1e-9) return false;
                double cosang = (n[0] * s[L.nrm] + n[1] * s[L.nrm + 1] + n[2] * s[L.nrm + 2]) / (ln * sn);
                if (cosang < tol.nrm_cos) return false;
            }
            for (int k = 0; k < 4; ++k)
                if (L.uv[k] >= 0 && (std::fabs(at(L.uv[k]) - s[L.uv[k]]) > tol.uv[k] ||
                                     std::fabs(at(L.uv[k] + 1) - s[L.uv[k] + 1]) > tol.uv[k]))
                    return false;
            if (L.col >= 0)
                for (int k = 0; k < 4; ++k)
                    if (std::fabs(at(L.col + k) - s[L.col + k]) > tol.col) return false;
            return true;
        });
    };

    std::vector<uint32_t> bad;
    std::vector<uint8_t> checked(vertex_count, 0);
    for (uint32_t i : before) {
        if (kept[i] || checked[i]) continue;
        checked[i] = 1;
        if (!fits(&w[i * S])) bad.push_back(i);
    }
    std::vector<float> centre(S);
    for (size_t t = 0; t + 2 < before.size(); t += 3) {
        uint32_t a = before[t], b = before[t + 1], c = before[t + 2];
        if (kept_tris.count(key_of(a, b, c))) continue;
        for (size_t k = 0; k < S; ++k) centre[k] = (w[a * S + k] + w[b * S + k] + w[c * S + k]) / 3.0f;
        if (!fits(centre.data())) bad.insert(bad.end(), {a, b, c});
    }
    return bad;
}

Result optimize_one(MeshData& m, const Use& use, const OptimizeOptions& opt) {
    Result r;
    size_t n = m.vertex_count;
    r.v0 = n;
    for (auto& s : m.subs) r.t0 += s.idx.size() / 3;
    r.v1 = r.v0;
    r.t1 = r.t0;
    if (!n || !r.t0 || m.pos.size() != n * 3) return r;

    // One interleaved vertex: position, normal, the UV sets materials sample, colour.
    bool has_n = m.nrm.size() == n * 3;
    bool has_c = opt.keep_colors && m.col.size() == n * 4;
    bool has_uv[4];
    size_t stride = 3 + (has_n ? 3 : 0) + (has_c ? 4 : 0);
    for (int k = 0; k < 4; ++k) {
        has_uv[k] = use.uv_used[k] && m.uv[k].size() == n * 2;
        stride += has_uv[k] ? 2 : 0;
    }
    std::vector<float> v(n * stride);
    for (size_t i = 0; i < n; ++i) {
        float* o = &v[i * stride];
        auto put = [&](const float* src, int count) {
            for (int k = 0; k < count; ++k) *o++ = src[k] == 0.0f ? 0.0f : src[k];  // -0 welds with +0
        };
        put(&m.pos[i * 3], 3);
        if (has_n) put(&m.nrm[i * 3], 3);
        for (int k = 0; k < 4; ++k)
            if (has_uv[k]) put(&m.uv[k][i * 2], 2);
        if (has_c) put(&m.col[i * 4], 4);
    }

    // Weld vertices that are identical in everything written.
    std::vector<uint32_t> all;
    all.reserve(r.t0 * 3);
    for (auto& s : m.subs) all.insert(all.end(), s.idx.begin(), s.idx.end());
    for (uint32_t i : all)
        if (i >= n) return r;  // broken indices: leave the mesh as it is
    std::vector<uint32_t> remap(n);
    size_t u = meshopt_generateVertexRemap(remap.data(), all.data(), all.size(), v.data(), n, stride * 4);
    std::vector<float> w(u * stride);
    meshopt_remapVertexBuffer(w.data(), v.data(), n, stride * 4, remap.data());
    v.clear();
    v.shrink_to_fit();
    auto same_pos = [&](uint32_t a, uint32_t b) { return std::memcmp(&w[a * stride], &w[b * stride], 12) == 0; };
    for (auto& s : m.subs) {
        meshopt_remapIndexBuffer(s.idx.data(), s.idx.data(), s.idx.size(), remap.data());
        // Zero-area triangles and exact repeats (same corners, same winding) draw nothing new.
        std::unordered_set<TriKey, TriKeyHash> seen;
        size_t out = 0;
        for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
            uint32_t a = s.idx[t], b = s.idx[t + 1], c = s.idx[t + 2];
            if (same_pos(a, b) || same_pos(b, c) || same_pos(a, c)) continue;
            TriKey key = a < b && a < c ? TriKey{a, b, c} : b < c ? TriKey{b, c, a} : TriKey{c, a, b};
            if (!seen.insert(key).second) continue;
            s.idx[out++] = a;
            s.idx[out++] = b;
            s.idx[out++] = c;
        }
        s.idx.resize(out);
    }

    // Collapse edges while every placed copy stays within max_error metres of the original
    // surface, with normals/UVs/colours within their tolerances. Open edges, material borders
    // and UV/normal seams keep their shape (LockBorder; seams only collapse along themselves).
    // meshopt's error is an average over the area a vertex has absorbed, so a small bump in a big
    // flat area can go under it: every result is measured against the original, and the vertices
    // it strays from are pinned for another pass.
    if (opt.max_error > 0 && use.scale > 0 && !use.decal) {
        float target = (float)(opt.max_error / use.scale);  // in mesh units
        float extent = meshopt_simplifyScale(w.data(), u, stride * 4);
        Layout L;
        L.stride = stride;
        {
            int at = 3;
            if (has_n) L.nrm = at, at += 3;
            for (int k = 0; k < 4; ++k)
                if (has_uv[k]) L.uv[k] = at, at += 2;
            if (has_c) L.col = at;
        }
        std::vector<uint32_t> same_place(u);  // vertex -> first vertex at the same position
        meshopt_generatePositionRemap(same_place.data(), w.data(), u, stride * 4);
        if (extent > 0) {
            for (size_t si = 0; si < m.subs.size(); ++si) {
                auto& idx = m.subs[si].idx;
                if (idx.size() < 3 * 4) continue;
                // meshopt measures attributes against positions scaled to a unit box.
                float unit = target / extent * kAttributeBoost;
                float tiling = si < use.tiling.size() ? std::max(use.tiling[si], 1e-6f) : 1.0f;
                std::vector<float> weights;
                if (has_n) weights.insert(weights.end(), 3, unit / kNormalTolerance);
                for (int k = 0; k < 4; ++k)
                    if (has_uv[k]) weights.insert(weights.end(), 2, unit * (k == 0 ? tiling : 1.0f) / kUvTolerance);
                if (has_c) weights.insert(weights.end(), 4, unit / kColorTolerance);
                Tolerance tol;
                tol.pos = target;
                tol.nrm_cos = kVerifyNormalCos;
                for (int k = 0; k < 4; ++k) tol.uv[k] = kVerifyUv / (k == 0 ? tiling : 1.0f);
                tol.col = kVerifyColor;

                std::vector<uint8_t> lock(u, 0), pinned(u, 0);
                std::vector<uint32_t> dst(idx.size());
                unsigned options = meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute;
                bool accepted = false, tried = false;
                size_t sub_vertices = 0;
                {
                    std::vector<uint8_t> seen(u, 0);
                    for (uint32_t i : idx) sub_vertices += seen[i] ? 0 : (seen[i] = 1);
                }
                float goal = target;  // meshopt's own limit; tightened when pinning alone doesn't settle it
                for (int round = 0; round < kVerifyRounds; ++round) {
                    size_t count =
                        weights.empty()
                            ? meshopt_simplifyWithAttributes(dst.data(), idx.data(), idx.size(), w.data(), u, stride * 4,
                                                             nullptr, 0, nullptr, 0, lock.data(), 0, goal, options, nullptr)
                            : meshopt_simplifyWithAttributes(dst.data(), idx.data(), idx.size(), w.data(), u, stride * 4,
                                                             w.data() + 3, stride * 4, weights.data(), weights.size(),
                                                             lock.data(), 0, goal, options, nullptr);
                    if (count >= idx.size()) break;  // nothing left to drop
                    std::vector<uint32_t> result(dst.begin(), dst.begin() + count);
                    std::vector<uint32_t> bad = misses(w, u, L, tol, idx, result);
                    if (bad.empty()) {
                        idx.swap(result);
                        r.simplified = true;
                        accepted = true;
                        break;
                    }
                    // Misses all over: meshopt's own limit is too loose for this surface, so halve it.
                    // A few misses: pin them (with every seam copy at the same place) and go again. A
                    // pinned vertex can still lose its triangles when the edge across from it
                    // collapses; then the corners of its original triangles get pinned too.
                    tried = true;
                    if (bad.size() * 20 > sub_vertices) {
                        goal *= 0.5f;
                        continue;
                    }
                    std::unordered_set<uint32_t> stuck;
                    for (uint32_t b : bad) {
                        if (pinned[same_place[b]]) stuck.insert(same_place[b]);
                        pinned[same_place[b]] = 1;
                    }
                    if (!stuck.empty())
                        for (size_t t = 0; t + 2 < idx.size(); t += 3)
                            for (int c = 0; c < 3; ++c)
                                if (stuck.count(same_place[idx[t + c]]))
                                    for (int e = 0; e < 3; ++e) pinned[same_place[idx[t + e]]] = 1;
                    for (size_t i = 0; i < u; ++i)
                        if (pinned[same_place[i]]) lock[i] = meshopt_SimplifyVertex_Lock;
                    if (round >= kVerifyRounds / 2) goal *= 0.5f;  // still unsettled: tighten as well
                }
                if (!accepted && tried) r.whole.push_back({si, idx.size() / 3});
            }
        }
    }

    // GPU order: triangles for the post-transform cache, then vertices in first-use order
    // (also drops the vertices simplification left unused).
    for (auto& s : m.subs)
        if (!s.idx.empty()) meshopt_optimizeVertexCache(s.idx.data(), s.idx.data(), s.idx.size(), u);
    all.clear();
    for (auto& s : m.subs) all.insert(all.end(), s.idx.begin(), s.idx.end());
    size_t kept = all.empty() ? 0 : meshopt_optimizeVertexFetch(w.data(), all.data(), all.size(), w.data(), u, stride * 4);
    size_t at = 0;
    for (auto& s : m.subs) {
        std::copy(all.begin() + at, all.begin() + at + s.idx.size(), s.idx.begin());
        at += s.idx.size();
    }

    m.vertex_count = kept;
    m.pos.resize(kept * 3);
    if (has_n) m.nrm.resize(kept * 3);
    else m.nrm.clear();
    if (has_c) m.col.resize(kept * 4);
    else m.col.clear();
    for (int k = 0; k < 4; ++k) {
        if (has_uv[k]) m.uv[k].resize(kept * 2);
        else m.uv[k].clear();
    }
    for (size_t i = 0; i < kept; ++i) {
        const float* o = &w[i * stride];
        std::copy(o, o + 3, &m.pos[i * 3]);
        o += 3;
        if (has_n) {
            std::copy(o, o + 3, &m.nrm[i * 3]);
            o += 3;
        }
        for (int k = 0; k < 4; ++k)
            if (has_uv[k]) {
                std::copy(o, o + 2, &m.uv[k][i * 2]);
                o += 2;
            }
        if (has_c) std::copy(o, o + 4, &m.col[i * 4]);
    }
    r.v1 = kept;
    r.t1 = 0;
    for (auto& s : m.subs) r.t1 += s.idx.size() / 3;
    return r;
}

} // namespace

void optimize_meshes(Scene& sc, const OptimizeOptions& opt, OptimizeStats& st) {
    if (!opt.enabled) return;

    // How each mesh is used: its materials (which UV sets they sample and at what tiling) and
    // the largest scale any placed copy has, so the error limit holds in world space.
    std::unordered_map<MeshData*, Use> uses;
    for (auto& om : sc.meshes) {
        if (!om.data) continue;
        Use& u = uses[om.data.get()];
        u.lines = u.lines || om.lines;
        if (u.tiling.size() < om.data->subs.size()) u.tiling.resize(om.data->subs.size(), 1.0f);
        for (size_t s = 0; s < om.materials.size() && s < u.tiling.size(); ++s) {
            int mi = om.materials[s];
            if (mi < 0 || mi >= (int)sc.materials.size()) continue;
            const OutMaterial& mat = sc.materials[mi];
            for (const TexRef* t : {&mat.base_tex, &mat.normal_tex, &mat.orm_tex, &mat.emissive_tex})
                if (t->image >= 0 && t->uv >= 0 && t->uv < 4) u.uv_used[t->uv] = true;
            if (mat.uv_transform)
                u.tiling[s] = std::max(u.tiling[s], std::max(std::fabs(mat.uv_scale[0]), std::fabs(mat.uv_scale[1])));
        }
    }
    std::vector<double> mesh_scale(sc.meshes.size(), 0.0);
    std::vector<size_t> mesh_copies(sc.meshes.size(), 0);
    std::vector<bool> mesh_decal(sc.meshes.size(), false);
    struct Item {
        int node;
        double scale;
    };
    std::vector<Item> stack;
    for (int r : sc.roots) stack.push_back({r, 1.0});
    while (!stack.empty()) {
        Item it = stack.back();
        stack.pop_back();
        if (it.node < 0 || it.node >= (int)sc.nodes.size()) continue;
        const OutNode& n = sc.nodes[it.node];
        double s = it.scale * stretch(n.s);
        if (n.mesh >= 0 && n.mesh < (int)sc.meshes.size()) {
            mesh_scale[n.mesh] = std::max(mesh_scale[n.mesh], s);
            ++mesh_copies[n.mesh];
            for (auto& p : n.extras.o)
                if (p.first == "xl_decal") mesh_decal[n.mesh] = true;
        }
        for (int c : n.children) stack.push_back({c, s});
    }
    for (size_t i = 0; i < sc.meshes.size(); ++i) {
        auto& om = sc.meshes[i];
        if (!om.data) continue;
        Use& u = uses[om.data.get()];
        u.scale = std::max(u.scale, mesh_scale[i]);
        u.copies += mesh_copies[i];
        u.decal = u.decal || mesh_decal[i];
    }

    std::vector<std::pair<MeshData*, const Use*>> work;
    for (auto& [m, u] : uses)
        if (!u.lines && u.copies) work.push_back({m, &u});
    std::sort(work.begin(), work.end(), [](auto& a, auto& b) { return a.first->vertex_count > b.first->vertex_count; });

    std::vector<Result> results(work.size());
    std::atomic<size_t> next{0};
    auto worker = [&] {
        for (size_t i; (i = next++) < work.size();) results[i] = optimize_one(*work[i].first, *work[i].second, opt);
    };
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    if (verbose_logging()) {
        std::vector<size_t> order(work.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        auto drawn = [&](size_t i) { return results[i].t0 * work[i].second->copies; };
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return drawn(a) > drawn(b); });
        for (size_t k = 0; k < order.size() && k < 15; ++k) {
            size_t i = order[k];
            const Result& r = results[i];
            log_verbose("  %-40s x%-6zu %7zu -> %7zu tris, %7zu -> %7zu verts (scale %.2f)", work[i].first->name.c_str(),
                        work[i].second->copies, r.t0, r.t1, r.v0, r.v1, work[i].second->scale);
        }
    }
    for (size_t i = 0; i < work.size(); ++i) {
        const Result& r = results[i];
        size_t copies = work[i].second->copies;
        ++st.meshes;
        st.simplified += r.simplified;
        st.kept_whole += r.whole.size();
        for (auto& [sub, tris] : r.whole)
            log_verbose("    %s submesh %zu (%zu triangles) kept whole", work[i].first->name.c_str(), sub, tris);
        st.vertices_before += r.v0;
        st.vertices_after += r.v1;
        st.triangles_before += r.t0;
        st.triangles_after += r.t1;
        st.drawn_before += r.t0 * copies;
        st.drawn_after += r.t1 * copies;
    }
}

} // namespace xl
