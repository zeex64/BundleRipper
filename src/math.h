// Small double-precision vector / quaternion / affine matrix helpers.
#pragma once
#include <algorithm>
#include <cmath>

namespace xl {

struct V3 {
    double x = 0, y = 0, z = 0;
    V3() = default;
    V3(double a, double b, double c) : x(a), y(b), z(c) {}
    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(double s) const { return {x * s, y * s, z * s}; }
    V3 operator-() const { return {-x, -y, -z}; }
    double operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};
inline double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double length(const V3& a) { return std::sqrt(dot(a, a)); }
inline V3 normalize(const V3& a) {
    double l = length(a);
    return l > 1e-20 ? a * (1.0 / l) : V3{0, 1, 0};
}
inline V3 vmin(const V3& a, const V3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline V3 vmax(const V3& a, const V3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

struct Quat {
    double x = 0, y = 0, z = 0, w = 1;
};

inline Quat normalize(const Quat& q) {
    double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-20) return {};
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}

// Row-major 4x4 affine matrix acting on column vectors: p' = M p.
struct M4 {
    double m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
};

inline M4 operator*(const M4& a, const M4& b) {
    M4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

inline M4 trs(const V3& t, const Quat& q0, const V3& s) {
    Quat q = normalize(q0);
    double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    M4 r;
    r.m[0][0] = (1 - 2 * (yy + zz)) * s.x;
    r.m[0][1] = (2 * (xy - wz)) * s.y;
    r.m[0][2] = (2 * (xz + wy)) * s.z;
    r.m[1][0] = (2 * (xy + wz)) * s.x;
    r.m[1][1] = (1 - 2 * (xx + zz)) * s.y;
    r.m[1][2] = (2 * (yz - wx)) * s.z;
    r.m[2][0] = (2 * (xz - wy)) * s.x;
    r.m[2][1] = (2 * (yz + wx)) * s.y;
    r.m[2][2] = (1 - 2 * (xx + yy)) * s.z;
    r.m[0][3] = t.x;
    r.m[1][3] = t.y;
    r.m[2][3] = t.z;
    return r;
}

inline V3 xform_point(const M4& a, const V3& p) {
    return {a.m[0][0] * p.x + a.m[0][1] * p.y + a.m[0][2] * p.z + a.m[0][3],
            a.m[1][0] * p.x + a.m[1][1] * p.y + a.m[1][2] * p.z + a.m[1][3],
            a.m[2][0] * p.x + a.m[2][1] * p.y + a.m[2][2] * p.z + a.m[2][3]};
}
inline V3 xform_dir(const M4& a, const V3& p) {
    return {a.m[0][0] * p.x + a.m[0][1] * p.y + a.m[0][2] * p.z, a.m[1][0] * p.x + a.m[1][1] * p.y + a.m[1][2] * p.z,
            a.m[2][0] * p.x + a.m[2][1] * p.y + a.m[2][2] * p.z};
}

inline double det3(const M4& a) {
    return a.m[0][0] * (a.m[1][1] * a.m[2][2] - a.m[1][2] * a.m[2][1]) -
           a.m[0][1] * (a.m[1][0] * a.m[2][2] - a.m[1][2] * a.m[2][0]) +
           a.m[0][2] * (a.m[1][0] * a.m[2][1] - a.m[1][1] * a.m[2][0]);
}

// Inverse of an affine matrix (upper 3x3 + translation).
inline M4 inverse_affine(const M4& a) {
    double d = det3(a);
    M4 r;
    if (std::fabs(d) < 1e-30) return r;
    double inv = 1.0 / d;
    r.m[0][0] = (a.m[1][1] * a.m[2][2] - a.m[1][2] * a.m[2][1]) * inv;
    r.m[0][1] = (a.m[0][2] * a.m[2][1] - a.m[0][1] * a.m[2][2]) * inv;
    r.m[0][2] = (a.m[0][1] * a.m[1][2] - a.m[0][2] * a.m[1][1]) * inv;
    r.m[1][0] = (a.m[1][2] * a.m[2][0] - a.m[1][0] * a.m[2][2]) * inv;
    r.m[1][1] = (a.m[0][0] * a.m[2][2] - a.m[0][2] * a.m[2][0]) * inv;
    r.m[1][2] = (a.m[0][2] * a.m[1][0] - a.m[0][0] * a.m[1][2]) * inv;
    r.m[2][0] = (a.m[1][0] * a.m[2][1] - a.m[1][1] * a.m[2][0]) * inv;
    r.m[2][1] = (a.m[0][1] * a.m[2][0] - a.m[0][0] * a.m[2][1]) * inv;
    r.m[2][2] = (a.m[0][0] * a.m[1][1] - a.m[0][1] * a.m[1][0]) * inv;
    V3 t{a.m[0][3], a.m[1][3], a.m[2][3]};
    V3 it = xform_dir(r, t);
    r.m[0][3] = -it.x;
    r.m[1][3] = -it.y;
    r.m[2][3] = -it.z;
    return r;
}

// Splits an affine matrix into translation, rotation and scale (negative X when mirrored).
// Returns false when the matrix is sheared, which translation/rotation/scale cannot hold.
inline bool decompose(const M4& a, V3& t, Quat& q, V3& s) {
    t = {a.m[0][3], a.m[1][3], a.m[2][3]};
    V3 c[3];
    for (int j = 0; j < 3; ++j) c[j] = {a.m[0][j], a.m[1][j], a.m[2][j]};
    s = {length(c[0]), length(c[1]), length(c[2])};
    if (s.x < 1e-12 || s.y < 1e-12 || s.z < 1e-12) return false;
    if (det3(a) < 0) s.x = -s.x;
    V3 r0 = c[0] * (1 / s.x), r1 = c[1] * (1 / s.y), r2 = c[2] * (1 / s.z);
    if (std::fabs(dot(r0, r1)) > 1e-4 || std::fabs(dot(r0, r2)) > 1e-4 || std::fabs(dot(r1, r2)) > 1e-4) return false;
    double m00 = r0.x, m10 = r0.y, m20 = r0.z, m01 = r1.x, m11 = r1.y, m21 = r1.z, m02 = r2.x, m12 = r2.y, m22 = r2.z;
    double tr = m00 + m11 + m22, S;
    if (tr > 0) {
        S = std::sqrt(tr + 1) * 2;
        q = {(m21 - m12) / S, (m02 - m20) / S, (m10 - m01) / S, 0.25 * S};
    } else if (m00 > m11 && m00 > m22) {
        S = std::sqrt(1 + m00 - m11 - m22) * 2;
        q = {0.25 * S, (m01 + m10) / S, (m02 + m20) / S, (m21 - m12) / S};
    } else if (m11 > m22) {
        S = std::sqrt(1 + m11 - m00 - m22) * 2;
        q = {(m01 + m10) / S, 0.25 * S, (m12 + m21) / S, (m02 - m20) / S};
    } else {
        S = std::sqrt(1 + m22 - m00 - m11) * 2;
        q = {(m02 + m20) / S, (m12 + m21) / S, 0.25 * S, (m10 - m01) / S};
    }
    q = normalize(q);
    return true;
}

// Matrix for transforming normals: inverse transpose of the upper 3x3.
inline M4 normal_matrix(const M4& a) {
    M4 inv = inverse_affine(a);
    M4 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = inv.m[j][i];
    return r;
}

} // namespace xl
