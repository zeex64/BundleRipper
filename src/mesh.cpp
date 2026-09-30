#include "mesh.h"
#include "binary.h"
#include "log.h"
#include "math.h"

#include <cmath>
#include <cstring>

namespace xl {

namespace {

enum class Fmt { F32, F16, UN8, SN8, UN16, SN16, U8, S8, U16, S16, U32, S32, Bad };

Fmt map_format(int f, int major) {
    if (major < 2017) {
        static const Fmt t[] = {Fmt::F32, Fmt::F16, Fmt::UN8, Fmt::U8, Fmt::U32};
        return f >= 0 && f < 5 ? t[f] : Fmt::Bad;
    }
    if (major < 2019) {
        static const Fmt t[] = {Fmt::F32, Fmt::F16, Fmt::UN8, Fmt::UN8, Fmt::SN8, Fmt::UN16, Fmt::SN16,
                                Fmt::U8,  Fmt::S8,  Fmt::U16, Fmt::S16, Fmt::U32, Fmt::S32};
        return f >= 0 && f < 13 ? t[f] : Fmt::Bad;
    }
    static const Fmt t[] = {Fmt::F32, Fmt::F16, Fmt::UN8, Fmt::SN8, Fmt::UN16, Fmt::SN16,
                            Fmt::U8,  Fmt::S8,  Fmt::U16, Fmt::S16, Fmt::U32,  Fmt::S32};
    return f >= 0 && f < 12 ? t[f] : Fmt::Bad;
}

size_t fmt_size(Fmt f) {
    switch (f) {
    case Fmt::F32: case Fmt::U32: case Fmt::S32: return 4;
    case Fmt::F16: case Fmt::UN16: case Fmt::SN16: case Fmt::U16: case Fmt::S16: return 2;
    case Fmt::UN8: case Fmt::SN8: case Fmt::U8: case Fmt::S8: return 1;
    default: return 0;
    }
}

float read_comp(const uint8_t* p, Fmt f) {
    switch (f) {
    case Fmt::F32: { float v; std::memcpy(&v, p, 4); return v; }
    case Fmt::F16: { uint16_t h; std::memcpy(&h, p, 2); return half_to_float(h); }
    case Fmt::UN8: return p[0] / 255.0f;
    case Fmt::SN8: return std::max(-1.0f, (int8_t)p[0] / 127.0f);
    case Fmt::UN16: { uint16_t v; std::memcpy(&v, p, 2); return v / 65535.0f; }
    case Fmt::SN16: { int16_t v; std::memcpy(&v, p, 2); return std::max(-1.0f, v / 32767.0f); }
    case Fmt::U8: return p[0];
    case Fmt::S8: return (int8_t)p[0];
    case Fmt::U16: { uint16_t v; std::memcpy(&v, p, 2); return v; }
    case Fmt::S16: { int16_t v; std::memcpy(&v, p, 2); return v; }
    case Fmt::U32: { uint32_t v; std::memcpy(&v, p, 4); return (float)v; }
    case Fmt::S32: { int32_t v; std::memcpy(&v, p, 4); return (float)v; }
    default: return 0;
    }
}

enum Sem { kPos, kNormal, kTangent, kColor, kUV0, kUV1, kUV2, kUV3, kUVHigh, kSkip };

Sem semantic(int channel, int major) {
    if (major >= 2018) {
        static const Sem t[] = {kPos, kNormal, kTangent, kColor, kUV0, kUV1, kUV2, kUV3,
                                kUVHigh, kUVHigh, kUVHigh, kUVHigh, kSkip, kSkip};
        return channel < 14 ? t[channel] : kSkip;
    }
    switch (channel) {
    case 0: return kPos;
    case 1: return kNormal;
    case 2: return kColor;
    case 3: return kUV0;
    case 4: return kUV1;
    case 5: return major >= 5 ? kUV2 : kTangent;
    case 6: return kUV3;
    case 7: return kTangent;
    default: return kSkip;
    }
}

std::vector<uint32_t> unpack_ints(const Value& pv, size_t start = 0, size_t count = SIZE_MAX) {
    const Value& data = pv["m_Data"];
    int bits = (int)pv["m_BitSize"].i64();
    size_t n = (size_t)pv["m_NumItems"].i64();
    if (count == SIZE_MAX) count = n;
    std::vector<uint32_t> out(count);
    if (bits <= 0 || data.kind != Kind::Bytes) return out;
    const uint8_t* d = data.data;
    size_t dsize = data.size;
    size_t bitpos = (size_t)bits * start;
    size_t idx = bitpos / 8;
    int bp = (int)(bitpos % 8);
    for (size_t i = 0; i < count; ++i) {
        uint64_t v = 0;
        int got = 0;
        while (got < bits) {
            if (idx >= dsize) return out;
            v |= (uint64_t)(d[idx] >> bp) << got;
            int num = std::min(bits - got, 8 - bp);
            bp += num;
            got += num;
            if (bp == 8) { ++idx; bp = 0; }
        }
        out[i] = (uint32_t)(v & ((bits >= 32) ? 0xffffffffull : ((1ull << bits) - 1)));
    }
    return out;
}

std::vector<float> unpack_floats(const Value& pv, size_t start = 0, size_t count = SIZE_MAX) {
    int bits = (int)pv["m_BitSize"].i64();
    double range = pv["m_Range"].num(), st = pv["m_Start"].num();
    size_t n = (size_t)pv["m_NumItems"].i64();
    if (count == SIZE_MAX) count = n;
    std::vector<float> out(count, (float)st);
    if (bits <= 0) return out;
    auto ints = unpack_ints(pv, start, count);
    double scale = range / (double)((bits >= 32 ? 0xffffffffull : (1ull << bits)) - 1);
    for (size_t i = 0; i < count; ++i) out[i] = (float)(ints[i] * scale + st);
    return out;
}

void decode_compressed(const Value& cm, MeshData& m, std::vector<uint32_t>& indices) {
    const Value& verts = cm["m_Vertices"];
    size_t vc = (size_t)verts["m_NumItems"].i64() / 3;
    m.vertex_count = vc;
    if (vc == 0) return;
    auto p = unpack_floats(verts);
    m.pos.assign(p.begin(), p.begin() + vc * 3);

    const Value& uv = cm["m_UV"];
    if (uv["m_NumItems"].i64() > 0) {
        uint32_t info = (uint32_t)cm["m_UVInfo"].i64();
        if (info != 0) {
            size_t off = 0;
            for (int ch = 0; ch < 8; ++ch) {
                uint32_t b = (info >> (ch * 4)) & 0xf;
                if (!(b & 4)) continue;
                int dim = 1 + (int)(b & 3);
                auto u = unpack_floats(uv, off, vc * dim);
                if (ch < 4) {
                    m.uv[ch].resize(vc * 2);
                    for (size_t i = 0; i < vc; ++i) {
                        m.uv[ch][i * 2] = u[i * dim];
                        m.uv[ch][i * 2 + 1] = dim > 1 ? u[i * dim + 1] : 0.0f;
                    }
                }
                off += vc * dim;
            }
        } else {
            m.uv[0] = unpack_floats(uv, 0, vc * 2);
            if ((size_t)uv["m_NumItems"].i64() >= vc * 4) m.uv[1] = unpack_floats(uv, vc * 2, vc * 2);
        }
    }
    const Value& nrm = cm["m_Normals"];
    if (nrm["m_NumItems"].i64() > 0) {
        auto n = unpack_floats(nrm);
        auto signs = unpack_ints(cm["m_NormalSigns"]);
        m.nrm.resize(vc * 3);
        for (size_t i = 0; i < vc && i * 2 + 1 < n.size(); ++i) {
            double x = n[i * 2], y = n[i * 2 + 1];
            double zz = 1 - x * x - y * y;
            V3 v = zz >= 0 ? V3{x, y, std::sqrt(zz)} : normalize(V3{x, y, 0});
            if (i < signs.size() && signs[i] == 0) v.z = -v.z;
            m.nrm[i * 3] = (float)v.x;
            m.nrm[i * 3 + 1] = (float)v.y;
            m.nrm[i * 3 + 2] = (float)v.z;
        }
    }
    const Value& fc = cm["m_FloatColors"];
    if (!fc.is_null() && fc["m_NumItems"].i64() > 0) {
        auto c = unpack_floats(fc);
        if (c.size() >= vc * 4) m.col.assign(c.begin(), c.begin() + vc * 4);
    }
    const Value& tri = cm["m_Triangles"];
    if (tri["m_NumItems"].i64() > 0) indices = unpack_ints(tri);
}

} // namespace

std::shared_ptr<MeshData> decode_mesh(const Database& db, ObjRef ref) {
    if (ref.builtin()) return ref.file == kBuiltinDefault ? builtin_mesh(ref.id) : nullptr;
    Value v = db.read(ref);
    if (v.is_null()) return nullptr;
    auto m = std::make_shared<MeshData>();
    m->name = v["m_Name"].s();
    int major = db.files[ref.file]->unity_major;

    std::vector<uint32_t> indices;
    const Value& vd = v["m_VertexData"];
    size_t vc = (size_t)vd["m_VertexCount"].i64();
    const Value& raw = vd["m_DataSize"];
    const uint8_t* data = raw.kind == Kind::Bytes ? raw.data : nullptr;
    size_t data_size = raw.kind == Kind::Bytes ? raw.size : 0;
    const Value& sd = v["m_StreamData"];
    if (!sd.is_null() && sd["size"].i64() > 0 && !sd["path"].s().empty()) {
        auto res = db.resource(sd["path"].s(), (uint64_t)sd["offset"].i64(), (uint64_t)sd["size"].i64());
        data = res.first;
        data_size = res.second;
        if (!data) log_warn("mesh '%s' streams its vertices from missing '%s'", m->name.c_str(), sd["path"].s().c_str());
    }

    if (vc > 0 && data) {
        m->vertex_count = vc;
        struct Ch { int stream, offset, dim; Fmt fmt; };
        std::vector<Ch> chans;
        for (auto& c : vd["m_Channels"].items) {
            int f = (int)c["format"].i64();
            chans.push_back({(int)c["stream"].i64(), (int)c["offset"].i64(), (int)(c["dimension"].i64() & 0xf),
                             map_format(f, major)});
        }
        if (major < 2018)  // packed 32-bit colour
            for (size_t i = 0; i < chans.size(); ++i)
                if (i == 2 && chans[i].fmt == Fmt::UN8 && chans[i].dim > 0) chans[i].dim = 4;
        struct Stream { size_t offset, stride; };
        std::vector<Stream> streams;
        if (vd.has("m_Streams") && vd["m_Streams"].kind == Kind::Array) {
            for (auto& s : vd["m_Streams"].items)
                streams.push_back({(size_t)s["offset"].i64(), (size_t)s["stride"].i64()});
        } else {
            int count = 0;
            for (auto& c : chans) count = std::max(count, c.stream + 1);
            size_t off = 0;
            for (int s = 0; s < count; ++s) {
                size_t stride = 0;
                for (auto& c : chans)
                    if (c.stream == s && c.dim > 0) stride += c.dim * fmt_size(c.fmt);
                streams.push_back({off, stride});
                off += vc * stride;
                off = (off + 15) & ~(size_t)15;
            }
        }
        for (size_t ci = 0; ci < chans.size(); ++ci) {
            auto& c = chans[ci];
            if (c.dim == 0 || c.fmt == Fmt::Bad || c.stream >= (int)streams.size()) continue;
            Sem sem = semantic((int)ci, major);
            if (sem == kSkip || sem == kTangent || sem == kUVHigh) continue;
            auto& st = streams[c.stream];
            size_t cs = fmt_size(c.fmt);
            if (st.offset + (vc - 1) * st.stride + c.offset + c.dim * cs > data_size) {
                log_warn("mesh '%s' channel %zu runs past its data", m->name.c_str(), ci);
                continue;
            }
            int want = sem == kPos || sem == kNormal ? 3 : sem == kColor ? 4 : 2;
            std::vector<float>* dst = sem == kPos ? &m->pos : sem == kNormal ? &m->nrm : sem == kColor ? &m->col
                                                                                                        : &m->uv[sem - kUV0];
            dst->assign(vc * want, sem == kColor ? 1.0f : 0.0f);
            for (size_t i = 0; i < vc; ++i) {
                const uint8_t* p = data + st.offset + i * st.stride + c.offset;
                for (int k = 0; k < std::min(want, c.dim); ++k) (*dst)[i * want + k] = read_comp(p + k * cs, c.fmt);
            }
        }
    } else if (vc == 0) {
        decode_compressed(v["m_CompressedMesh"], *m, indices);
    }
    if (m->pos.empty()) return m;

    bool wide = v["m_IndexFormat"].i64() == 1;
    const Value& ib = v["m_IndexBuffer"];
    if (indices.empty() && ib.kind == Kind::Bytes) {
        size_t n = ib.size / (wide ? 4 : 2);
        indices.resize(n);
        for (size_t i = 0; i < n; ++i) {
            if (wide) std::memcpy(&indices[i], ib.data + i * 4, 4);
            else { uint16_t s; std::memcpy(&s, ib.data + i * 2, 2); indices[i] = s; }
        }
    }
    size_t isz = wide ? 4 : 2;
    for (auto& sm : v["m_SubMeshes"].items) {
        MeshData::Sub sub;
        size_t first = (size_t)sm["firstByte"].i64() / isz;
        size_t count = (size_t)sm["indexCount"].i64();
        int topo = (int)sm["topology"].i64();
        uint32_t base = (uint32_t)sm["baseVertex"].i64();
        auto at = [&](size_t k) -> uint32_t { return indices[first + k] + base; };
        if (first + count > indices.size()) count = first < indices.size() ? indices.size() - first : 0;
        auto push = [&](uint32_t a, uint32_t b, uint32_t c) {
            if (a == b || b == c || a == c) return;
            if (a >= m->vertex_count || b >= m->vertex_count || c >= m->vertex_count) return;
            sub.idx.insert(sub.idx.end(), {a, b, c});
        };
        if (topo == 0) {
            for (size_t k = 0; k + 2 < count; k += 3) push(at(k), at(k + 1), at(k + 2));
        } else if (topo == 1) {
            for (size_t k = 0; k + 2 < count; ++k)
                if (k & 1) push(at(k + 1), at(k), at(k + 2));
                else push(at(k), at(k + 1), at(k + 2));
        } else if (topo == 2) {
            for (size_t k = 0; k + 3 < count; k += 4) {
                push(at(k), at(k + 1), at(k + 2));
                push(at(k), at(k + 2), at(k + 3));
            }
        }
        m->subs.push_back(std::move(sub));
    }
    return m;
}

void ensure_normals(MeshData& m) {
    if (!m.nrm.empty() || m.vertex_count == 0) return;
    std::vector<double> acc(m.vertex_count * 3, 0.0);
    for (auto& s : m.subs)
        for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
            uint32_t i[3] = {s.idx[t], s.idx[t + 1], s.idx[t + 2]};
            V3 p[3];
            for (int k = 0; k < 3; ++k) p[k] = {m.pos[i[k] * 3], m.pos[i[k] * 3 + 1], m.pos[i[k] * 3 + 2]};
            V3 n = cross(p[1] - p[0], p[2] - p[0]);
            for (int k = 0; k < 3; ++k) {
                acc[i[k] * 3] += n.x;
                acc[i[k] * 3 + 1] += n.y;
                acc[i[k] * 3 + 2] += n.z;
            }
        }
    m.nrm.resize(m.vertex_count * 3);
    for (size_t i = 0; i < m.vertex_count; ++i) {
        V3 n = normalize(V3{acc[i * 3], acc[i * 3 + 1], acc[i * 3 + 2]});
        m.nrm[i * 3] = (float)n.x;
        m.nrm[i * 3 + 1] = (float)n.y;
        m.nrm[i * 3 + 2] = (float)n.z;
    }
}

// ---------------------------------------------------------------------------
// Built-in primitives. Plane and Quad match Unity's vertex layout exactly; the
// round shapes match its dimensions.

namespace {

struct Builder {
    MeshData m;
    MeshData::Sub sub;
    uint32_t vert(V3 p, V3 n, double u, double v) {
        m.pos.insert(m.pos.end(), {(float)p.x, (float)p.y, (float)p.z});
        m.nrm.insert(m.nrm.end(), {(float)n.x, (float)n.y, (float)n.z});
        m.uv[0].insert(m.uv[0].end(), {(float)u, (float)v});
        return (uint32_t)m.vertex_count++;
    }
    // Adds a triangle, wound so Unity treats the side its normals face as the front.
    void tri(uint32_t a, uint32_t b, uint32_t c) {
        auto P = [&](uint32_t i) { return V3{m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]}; };
        auto N = [&](uint32_t i) { return V3{m.nrm[i * 3], m.nrm[i * 3 + 1], m.nrm[i * 3 + 2]}; };
        V3 f = cross(P(b) - P(a), P(c) - P(a));
        if (dot(f, N(a) + N(b) + N(c)) < 0) std::swap(b, c);
        sub.idx.insert(sub.idx.end(), {a, b, c});
    }
    void quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        tri(a, b, c);
        tri(a, c, d);
    }
    std::shared_ptr<MeshData> finish(const char* name) {
        m.name = name;
        m.subs.push_back(std::move(sub));
        return std::make_shared<MeshData>(std::move(m));
    }
};

std::shared_ptr<MeshData> make_cube() {
    Builder b;
    const V3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (auto& n : axes) {
        V3 u = std::fabs(n.y) > 0.5 ? V3{1, 0, 0} : normalize(cross(V3{0, 1, 0}, n));
        V3 w = cross(n, u);
        uint32_t v[4];
        const double c[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int k = 0; k < 4; ++k)
            v[k] = b.vert(n * 0.5 + u * (c[k][0] - 0.5) + w * (c[k][1] - 0.5), n, c[k][0], c[k][1]);
        b.quad(v[0], v[1], v[2], v[3]);
    }
    return b.finish("Cube");
}

std::shared_ptr<MeshData> make_plane() {
    Builder b;
    for (int r = 0; r <= 10; ++r)
        for (int c = 0; c <= 10; ++c) b.vert({5.0 - c, 0, 5.0 - r}, {0, 1, 0}, c / 10.0, r / 10.0);
    for (int r = 0; r < 10; ++r)
        for (int c = 0; c < 10; ++c) {
            uint32_t a = r * 11 + c, bb = a + 1, d = a + 11, e = d + 1;
            b.sub.idx.insert(b.sub.idx.end(), {a, e, bb, a, d, e});
        }
    return b.finish("Plane");
}

std::shared_ptr<MeshData> make_quad() {
    Builder b;
    b.vert({-0.5, -0.5, 0}, {0, 0, -1}, 0, 0);
    b.vert({0.5, -0.5, 0}, {0, 0, -1}, 1, 0);
    b.vert({-0.5, 0.5, 0}, {0, 0, -1}, 0, 1);
    b.vert({0.5, 0.5, 0}, {0, 0, -1}, 1, 1);
    b.sub.idx = {0, 3, 1, 3, 0, 2};
    return b.finish("Quad");
}

// Capsule/sphere: rings of a sphere of radius r split at the equator by `half` (cylinder half height).
std::shared_ptr<MeshData> make_round(const char* name, double r, double half, int segs, int rings) {
    Builder b;
    std::vector<std::vector<uint32_t>> grid;
    auto ring = [&](double phi, double yoff, double v) {
        std::vector<uint32_t> row;
        for (int s = 0; s <= segs; ++s) {
            double th = 2 * 3.14159265358979 * s / segs;
            V3 n{std::cos(th) * std::sin(phi), std::cos(phi), std::sin(th) * std::sin(phi)};
            row.push_back(b.vert(V3{n.x * r, n.y * r + yoff, n.z * r}, n, (double)s / segs, v));
        }
        grid.push_back(row);
    };
    for (int i = 0; i <= rings / 2; ++i) ring(3.14159265358979 * i / rings, half, 1.0 - (double)i / rings);
    for (int i = rings / 2 + (half == 0.0 ? 1 : 0); i <= rings; ++i) ring(3.14159265358979 * i / rings, -half, 1.0 - (double)i / rings);
    for (size_t i = 0; i + 1 < grid.size(); ++i)
        for (int s = 0; s < segs; ++s) {
            uint32_t a = grid[i][s], c = grid[i][s + 1], d = grid[i + 1][s + 1], e = grid[i + 1][s];
            b.tri(a, c, d);
            b.tri(a, d, e);
        }
    return b.finish(name);
}

std::shared_ptr<MeshData> make_cylinder() {
    Builder b;
    const int segs = 20;
    std::vector<uint32_t> top, bot;
    for (int s = 0; s <= segs; ++s) {
        double th = 2 * 3.14159265358979 * s / segs;
        V3 n{std::cos(th), 0, std::sin(th)};
        top.push_back(b.vert({n.x * 0.5, 1, n.z * 0.5}, n, (double)s / segs, 1));
        bot.push_back(b.vert({n.x * 0.5, -1, n.z * 0.5}, n, (double)s / segs, 0));
    }
    for (int s = 0; s < segs; ++s) b.quad(top[s], top[s + 1], bot[s + 1], bot[s]);
    for (int cap = 0; cap < 2; ++cap) {
        double y = cap ? 1 : -1;
        V3 n{0, y, 0};
        uint32_t c = b.vert({0, y, 0}, n, 0.5, 0.5);
        std::vector<uint32_t> rim;
        for (int s = 0; s <= segs; ++s) {
            double th = 2 * 3.14159265358979 * s / segs;
            rim.push_back(b.vert({std::cos(th) * 0.5, y, std::sin(th) * 0.5}, n, 0.5 + std::cos(th) * 0.5,
                                 0.5 + std::sin(th) * 0.5));
        }
        for (int s = 0; s < segs; ++s) b.tri(c, rim[s], rim[s + 1]);
    }
    return b.finish("Cylinder");
}

} // namespace

std::shared_ptr<MeshData> builtin_mesh(int64_t id) {
    switch (id) {
    case 10202: return make_cube();
    case 10206: return make_cylinder();
    case 10207: return make_round("Sphere", 0.5, 0.0, 24, 16);
    case 10208: return make_round("Capsule", 0.5, 0.5, 24, 16);
    case 10209: return make_plane();
    case 10210: return make_quad();
    default: return nullptr;
    }
}

} // namespace xl
