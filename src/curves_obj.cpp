#include "curves_obj.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace xl {

size_t write_curves_obj(const std::vector<OutCurve>& curves, const std::filesystem::path& out) {
    std::ofstream f(out, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + out.string());
    f << "# BundleRipper grind splines: import in Blender with File > Import > Wavefront (.obj)\n";
    char line[160];
    size_t written = 0, vertex = 0;
    for (const OutCurve& c : curves) {
        // Control points in NURBS order. Bezier: P0 R0 L1 P1 R1 L2 P2 ...
        std::vector<V3> cp;
        size_t n = c.points.size();
        if (n < 2) continue;
        size_t count = c.closed ? n + 1 : n;
        for (size_t i = 0; i < count; ++i) {
            size_t k = i % n;
            if (c.bezier && i > 0) {
                cp.push_back(c.right[(i - 1) % n]);
                cp.push_back(c.left[k]);
            }
            cp.push_back(c.points[k]);
        }
        std::string name = c.name.empty() ? "spline" : c.name;
        for (auto& ch : name)
            if (ch == '\n' || ch == '\r') ch = ' ';
        f << "o " << name << "\n";
        for (auto& p : cp) {
            std::snprintf(line, sizeof line, "v %.6f %.6f %.6f\n", -p.x, p.y, p.z);  // Unity -> glTF axes
            f << line;
        }
        size_t segments = count - 1;
        f << "cstype bspline\n" << (c.bezier ? "deg 3\n" : "deg 1\n");
        std::snprintf(line, sizeof line, "curv 0 %zu", segments);
        f << line;
        for (size_t i = 0; i < cp.size(); ++i) f << " " << (vertex + i + 1);
        f << "\nparm u";
        if (c.bezier) {  // 0 0 0 0 1 1 1 2 2 2 ... n n n n
            f << " 0 0 0 0";
            for (size_t s = 1; s < segments; ++s) f << " " << s << " " << s << " " << s;
            f << " " << segments << " " << segments << " " << segments << " " << segments;
        } else {  // 0 0 1 2 ... n n
            f << " 0";
            for (size_t s = 0; s <= segments; ++s) f << " " << s;
            f << " " << segments;
        }
        f << "\nend\n";
        vertex += cp.size();
        ++written;
    }
    if (!f) throw std::runtime_error("failed writing " + out.string());
    return written;
}

} // namespace xl
