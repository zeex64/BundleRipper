// The map's own sky: the skybox Unity draws behind the level, as one panorama.
#pragma once
#include "texture.h"

#include <string>

namespace xl {

// An equirectangular panorama with rows top-down: the pixel for a Unity direction d is at
// u = 0.5 + atan2(d.x, d.z) / 2pi, v = acos(d.y) / pi.
struct SkyImage {
    Image img;
    std::string source;  // what it came from, for the log
    bool ok() const { return img.w > 0; }
};

// The skybox from the scene's RenderSettings material (Skybox/Cubemap, Panoramic or 6 Sided) or
// an HDRP volume's HDRI sky, `width` pixels wide. False (with `why`) when the map has none that
// is a picture (a procedural or gradient sky).
bool extract_sky(const Database& db, int width, SkyImage& out, std::string& why);

} // namespace xl
