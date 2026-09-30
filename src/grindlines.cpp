#include "grindlines.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace xl {

namespace {

constexpr double kWeld = 0.001;          // metres
constexpr double kSharp = 35.0;          // degrees: creases at least this sharp are edges
constexpr double kTubeRadius = 0.30;     // metres: smooth tubes up to this count
constexpr double kMinRun = 0.5;          // metres
constexpr double kSimplify = 0.005;      // metres
constexpr double kDuplicate = 0.06;      // metres: parallel runs this close are one grind
constexpr double kTurn = 60.0;           // degrees: runs split at sharper corners

struct KeyHash {
    size_t operator()(const std::array<int64_t, 3>& k) const {
        return (size_t)(k[0] * 73856093) ^ (size_t)(k[1] * 19349663) ^ (size_t)(k[2] * 83492791);
    }
};

std::vector<V3> simplify(const std::vector<V3>& pts, double tol) {
    if (pts.size() < 3) return pts;
    std::vector<char> keep(pts.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> stack{{0, pts.size() - 1}};
    while (!stack.empty()) {
        auto [a, b] = stack.back();
        stack.pop_back();
        V3 d = pts[b] - pts[a];
        double l2 = dot(d, d), worst = -1;
        size_t wi = 0;
        for (size_t i = a + 1; i < b; ++i) {
            V3 o = pts[i] - pts[a];
            if (l2 > 1e-12) o = o - d * std::clamp(dot(o, d) / l2, 0.0, 1.0);
            double dist = length(o);
            if (dist > worst) {
                worst = dist;
                wi = i;
            }
        }
        if (worst > tol) {
            keep[wi] = 1;
            stack.push_back({a, wi});
            stack.push_back({wi, b});
        }
    }
    std::vector<V3> out;
    for (size_t i = 0; i < pts.size(); ++i)
        if (keep[i]) out.push_back(pts[i]);
    return out;
}

double run_length(const std::vector<V3>& r) {
    double l = 0;
    for (size_t i = 1; i < r.size(); ++i) l += length(r[i] - r[i - 1]);
    return l;
}

double point_segment(const V3& p, const V3& a, const V3& b) {
    V3 d = b - a, o = p - a;
    double l2 = dot(d, d);
    if (l2 > 1e-12) o = o - d * std::clamp(dot(o, d) / l2, 0.0, 1.0);
    return length(o);
}

} // namespace

std::vector<std::vector<V3>> find_grind_lines(const std::vector<V3>& positions, const std::vector<uint32_t>& triangles) {
    // Weld by position so faces that only share coordinates (split normals/UVs) connect.
    std::unordered_map<std::array<int64_t, 3>, uint32_t, KeyHash> weld;
    std::vector<V3> P;
    std::vector<uint32_t> remap(positions.size());
    for (size_t i = 0; i < positions.size(); ++i) {
        const V3& p = positions[i];
        std::array<int64_t, 3> k{(int64_t)std::llround(p.x / kWeld), (int64_t)std::llround(p.y / kWeld),
                                 (int64_t)std::llround(p.z / kWeld)};
        auto [it, fresh] = weld.emplace(k, (uint32_t)P.size());
        if (fresh) P.push_back(p);
        remap[i] = it->second;
    }
    struct Tri {
        uint32_t v[3];
        V3 n;
    };
    std::vector<Tri> tris;
    for (size_t t = 0; t + 2 < triangles.size(); t += 3) {
        Tri tr{{remap[triangles[t]], remap[triangles[t + 1]], remap[triangles[t + 2]]}, {}};
        if (tr.v[0] == tr.v[1] || tr.v[1] == tr.v[2] || tr.v[0] == tr.v[2]) continue;
        V3 n = cross(P[tr.v[1]] - P[tr.v[0]], P[tr.v[2]] - P[tr.v[0]]);  // Unity winding: outward
        if (length(n) < 1e-12) continue;
        tr.n = normalize(n);
        tris.push_back(tr);
    }
    // Edge -> the triangles on it.
    std::unordered_map<uint64_t, std::vector<uint32_t>> edges;
    for (uint32_t t = 0; t < tris.size(); ++t)
        for (int k = 0; k < 3; ++k) {
            uint32_t a = tris[t].v[k], b = tris[t].v[(k + 1) % 3];
            edges[((uint64_t)std::min(a, b) << 32) | std::max(a, b)].push_back(t);
        }

    const V3 up{0, 1, 0};
    const double sharp_cos = std::cos(kSharp * 3.14159265358979 / 180);
    std::unordered_map<uint32_t, std::vector<uint32_t>> graph;  // vertex -> selected neighbours
    for (auto& [key, faces] : edges) {
        if (faces.size() != 2) continue;
        uint32_t a = (uint32_t)(key >> 32), b = (uint32_t)(key & 0xffffffffu);
        V3 e = P[b] - P[a];
        double elen = length(e);
        if (elen < 1e-6) continue;
        e = e * (1 / elen);
        if (std::fabs(e.y) > 0.8) continue;  // near-vertical: not a grind
        const Tri& f1 = tris[faces[0]];
        const Tri& f2 = tris[faces[1]];
        auto opposite = [&](const Tri& f) {
            for (uint32_t v : f.v)
                if (v != a && v != b) return v;
            return f.v[0];
        };
        V3 o1 = P[opposite(f1)], o2 = P[opposite(f2)];
        if (dot(f1.n, o2 - P[a]) > -1e-5) continue;  // concave or flat: nothing to grind
        double c = std::clamp(dot(f1.n, f2.n), -1.0, 1.0);
        double bend = std::acos(c);
        if (bend < 2.0 * 3.14159265358979 / 180) continue;
        // `up` squashed into the plane across the edge: is it inside the wedge of the two normals?
        V3 u = up - e * dot(up, e);
        double ul = length(u);
        bool top = false;
        if (ul > 1e-6) {
            u = u * (1 / ul);
            double s12 = dot(cross(f1.n, f2.n), e);
            double s1u = dot(cross(f1.n, u), e), su2 = dot(cross(u, f2.n), e);
            if (s12 < 0) {
                s1u = -s1u;
                su2 = -su2;
            }
            top = s1u >= 0 && su2 > 0 && dot(u, f1.n + f2.n) > 0;
        }
        bool pick = false;
        if (c <= sharp_cos) {
            // Sharp: a top face meeting a side face (ledge, box, square rail), or a ridge.
            bool top_side = (f1.n.y >= 0.7 && f2.n.y <= 0.35 && f2.n.y >= -0.6) ||
                            (f2.n.y >= 0.7 && f1.n.y <= 0.35 && f1.n.y >= -0.6);
            pick = top_side || top;
        } else if (top) {
            // Smooth: only the top line of a thin tube.
            double h1 = point_segment(o1, P[a], P[b]), h2 = point_segment(o2, P[a], P[b]);
            double radius = (h1 + h2) * 0.5 / (2 * std::sin(bend * 0.5));
            pick = radius <= kTubeRadius;
        }
        if (!pick) continue;
        graph[a].push_back(b);
        graph[b].push_back(a);
    }

    // Walk the selected edges into runs, breaking at junctions and sharp turns.
    std::unordered_map<uint64_t, char> used;
    auto ekey = [](uint32_t a, uint32_t b) { return ((uint64_t)std::min(a, b) << 32) | std::max(a, b); };
    std::vector<std::vector<V3>> runs;
    const double turn_cos = std::cos(kTurn * 3.14159265358979 / 180);
    auto walk = [&](uint32_t start, uint32_t next) {
        std::vector<V3> run{P[start]};
        uint32_t prev = start, cur = next;
        used[ekey(prev, cur)] = 1;
        while (true) {
            run.push_back(P[cur]);
            auto& nb = graph[cur];
            if (nb.size() != 2) break;
            uint32_t nxt = nb[0] == prev ? nb[1] : nb[0];
            if (used.count(ekey(cur, nxt))) break;
            V3 d0 = normalize(P[cur] - P[prev]), d1 = normalize(P[nxt] - P[cur]);
            if (dot(d0, d1) < turn_cos) break;
            used[ekey(cur, nxt)] = 1;
            prev = cur;
            cur = nxt;
        }
        return run;
    };
    for (auto& [v, nb] : graph)  // runs from ends and junctions first
        if (nb.size() != 2)
            for (uint32_t n : nb)
                if (!used.count(ekey(v, n))) runs.push_back(walk(v, n));
    for (auto& [v, nb] : graph)  // then what is left (loops, corner splits)
        for (uint32_t n : nb)
            if (!used.count(ekey(v, n))) runs.push_back(walk(v, n));

    // Longest first; drop short runs and ones lying along an already kept run.
    std::sort(runs.begin(), runs.end(), [](auto& x, auto& y) { return run_length(x) > run_length(y); });
    std::vector<std::vector<V3>> kept;
    for (auto& r : runs) {
        if (run_length(r) < kMinRun) continue;
        size_t near = 0, samples = 0;
        for (size_t i = 0; i < r.size(); ++i) {
            ++samples;
            bool close = false;
            for (auto& k : kept) {
                for (size_t j = 1; j < k.size() && !close; ++j) close = point_segment(r[i], k[j - 1], k[j]) < kDuplicate;
                if (close) break;
            }
            near += close;
        }
        if (samples && near * 10 >= samples * 8) continue;
        kept.push_back(simplify(r, kSimplify));
    }
    return kept;
}

} // namespace xl
