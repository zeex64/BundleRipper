// Look and widgets shared by the window's pages (gui.cpp, scene_view.cpp).
#pragma once
#include <imgui.h>

namespace xl::ui {

// DPI scale of the window; S(px) is a size in 96-DPI pixels.
inline float& dpi_scale() {
    static float s = 1;
    return s;
}
inline float S(float v) { return v * dpi_scale(); }

inline ImVec4 rgb(int r, int g, int b, float a = 1) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }
inline ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
// The x of the right edge of the content area, in window coordinates (for SameLine).
inline float right_edge() { return ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x; }

namespace col {
inline const ImVec4 bg = rgb(16, 18, 23);
inline const ImVec4 card = rgb(24, 27, 34);
inline const ImVec4 inset = rgb(19, 21, 27);
inline const ImVec4 field = rgb(35, 39, 49);
inline const ImVec4 field_hover = rgb(44, 49, 61);
inline const ImVec4 field_active = rgb(52, 58, 72);
inline const ImVec4 border = rgb(40, 45, 56);
inline const ImVec4 text = rgb(230, 233, 239);
inline const ImVec4 muted = rgb(136, 144, 160);
inline const ImVec4 switch_off = rgb(58, 64, 78);
inline const ImVec4 accent = rgb(96, 142, 255);
inline const ImVec4 accent_hover = rgb(122, 162, 255);
inline const ImVec4 accent_active = rgb(78, 122, 232);
inline const ImVec4 good = rgb(70, 190, 110);
inline const ImVec4 warn = rgb(222, 170, 60);
inline const ImVec4 bad = rgb(245, 95, 88);
} // namespace col

struct Fonts {
    ImFont* body = nullptr;
    ImFont* semibold = nullptr;
    ImFont* title = nullptr;
    ImFont* caption = nullptr;
    ImFont* mono = nullptr;
};

// Implemented in gui.cpp.
bool accent_button(const char* label, ImVec2 size);
// A settings row: label on the left, a switch on the right; the whole row toggles.
bool switch_row(const char* label, bool& value);
// A small on/off pill for toolbars.
bool chip(const char* label, bool& value, const char* tooltip = nullptr);

} // namespace xl::ui
