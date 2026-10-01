// The Spotbuilder window: pick a map, set options, rip, and watch the log. Dear ImGui on
// Direct3D 11; the controls come from the option table (options.cpp).
#include "gui.h"
#include "log.h"
#include "gui_common.h"
#include "options.h"
#include "scene_view.h"
#include "texture.h"

#include <Windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <memory>
#include <thread>
#include <unordered_map>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace fs = std::filesystem;

namespace xl {

namespace {

using namespace ui;

ImFont* windows_font(std::initializer_list<const wchar_t*> files, float size) {
    static const ImWchar ranges[] = {0x0020, 0x024F, 0x2010, 0x2027, 0x2190, 0x2193, 0};
    wchar_t dir[MAX_PATH] = {};
    GetWindowsDirectoryW(dir, MAX_PATH);
    for (const wchar_t* file : files) {
        fs::path p = fs::path(dir) / L"Fonts" / file;
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) continue;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        if (ImFont* f = ImGui::GetIO().Fonts->AddFontFromFileTTF(u8(p).c_str(), S(size), &cfg, ranges)) return f;
    }
    return nullptr;
}

Fonts load_fonts() {
    Fonts f;
    f.body = windows_font({L"segoeui.ttf"}, 16);
    if (!f.body) f.body = ImGui::GetIO().Fonts->AddFontDefault();
    f.semibold = windows_font({L"seguisb.ttf", L"segoeuib.ttf"}, 16);
    f.title = windows_font({L"seguisb.ttf", L"segoeuib.ttf"}, 26);
    f.caption = windows_font({L"segoeui.ttf"}, 13.5f);
    f.mono = windows_font({L"CascadiaMono.ttf", L"consola.ttf"}, 13.5f);
    for (ImFont** p : {&f.semibold, &f.title, &f.caption, &f.mono})
        if (!*p) *p = f.body;
    ImGui::GetIO().FontDefault = f.body;
    return f;
}

void apply_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(20, 18);
    s.FramePadding = ImVec2(10, 7);
    s.ItemSpacing = ImVec2(10, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.CellPadding = ImVec2(8, 0);
    s.WindowRounding = 0;
    s.ChildRounding = 10;
    s.FrameRounding = 6;
    s.PopupRounding = 8;
    s.GrabRounding = 6;
    s.ScrollbarRounding = 8;
    s.ScrollbarSize = 10;
    s.GrabMinSize = 12;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.DisabledAlpha = 0.4f;
    s.ScaleAllSizes(dpi_scale());
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = col::text;
    c[ImGuiCol_TextDisabled] = col::muted;
    c[ImGuiCol_WindowBg] = col::bg;
    c[ImGuiCol_ChildBg] = col::card;
    c[ImGuiCol_PopupBg] = rgb(28, 31, 39);
    c[ImGuiCol_Border] = col::border;
    c[ImGuiCol_FrameBg] = col::field;
    c[ImGuiCol_FrameBgHovered] = col::field_hover;
    c[ImGuiCol_FrameBgActive] = col::field_active;
    c[ImGuiCol_Button] = col::field;
    c[ImGuiCol_ButtonHovered] = col::field_hover;
    c[ImGuiCol_ButtonActive] = col::field_active;
    c[ImGuiCol_Header] = rgb(96, 142, 255, 0.18f);
    c[ImGuiCol_HeaderHovered] = rgb(96, 142, 255, 0.28f);
    c[ImGuiCol_HeaderActive] = rgb(96, 142, 255, 0.38f);
    c[ImGuiCol_CheckMark] = col::accent;
    c[ImGuiCol_SliderGrab] = col::accent;
    c[ImGuiCol_SliderGrabActive] = col::accent_hover;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgb(58, 64, 78);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(72, 79, 96);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(86, 94, 112);
    c[ImGuiCol_Separator] = col::border;
    c[ImGuiCol_TextSelectedBg] = rgb(96, 142, 255, 0.35f);
    c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);
}

// ---------------------------------------------------------------------------------------------
// The rip running on its own thread

enum class RunState { Idle, Running, Done, Failed };

enum class RunKind { Rip, List, Load, Scan };

struct Runner {
    std::mutex mutex;
    std::vector<std::string> lines;    // guarded
    std::string error;                 // guarded
    JobResult result;                  // guarded
    std::unique_ptr<Preview> preview;  // guarded: a finished scene load, until the view takes it
    std::unique_ptr<std::vector<PlantInfo>> plants;  // guarded: a finished plant scan
    double seconds = 0;                // guarded
    int generation = 0;                // guarded: bumps with every run
    RunKind kind = RunKind::Rip;       // guarded
    std::atomic<RunState> state{RunState::Idle};
    std::chrono::steady_clock::time_point started;
};

// Never destroyed: a rip still running when the window closes must not touch a dead object.
Runner& runner() {
    static Runner* r = new Runner;
    return *r;
}

void runner_sink(const std::string& line) {
    Runner& r = runner();
    std::lock_guard lock(r.mutex);
    r.lines.push_back(line);
}

// Texture size the scene view loads at (base colour only, so memory stays modest).
constexpr int kPreviewTextureSize = 1024;

void start_job(const Job& job, RunKind kind) {
    Runner& r = runner();
    {
        std::lock_guard lock(r.mutex);
        r.lines.clear();
        r.error.clear();
        r.result = {};
        r.preview.reset();
        r.plants.reset();
        r.seconds = 0;
        r.kind = kind;
        ++r.generation;
    }
    r.started = std::chrono::steady_clock::now();
    r.state = RunState::Running;
    log_sink() = runner_sink;
    std::thread([job, kind] {
        Runner& r = runner();
        try {
            JobResult result;
            std::unique_ptr<Preview> preview;
            std::unique_ptr<std::vector<PlantInfo>> plants;
            if (kind == RunKind::Load) preview = std::make_unique<Preview>(load_preview(job, kPreviewTextureSize));
            else if (kind == RunKind::Scan) plants = std::make_unique<std::vector<PlantInfo>>(scan_plants(job));
            else result = run_job(job);
            std::lock_guard lock(r.mutex);
            r.result = result;
            r.preview = std::move(preview);
            r.plants = std::move(plants);
            r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - r.started).count();
            r.state = RunState::Done;
        } catch (const std::exception& e) {
            std::lock_guard lock(r.mutex);
            r.error = e.what();
            r.lines.push_back(std::string("error: ") + e.what());
            r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - r.started).count();
            r.state = RunState::Failed;
        }
    }).detach();
}

// ---------------------------------------------------------------------------------------------
// Windows helpers

fs::path settings_file() {
    PWSTR dir = nullptr;
    fs::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &dir))) p = fs::path(dir) / L"Spotbuilder" / L"settings.txt";
    CoTaskMemFree(dir);
    return p;
}

// Where the settings were kept under the tool's old name (read until the new file exists).
fs::path legacy_settings_file() {
    fs::path p = settings_file();
    return p.empty() ? p : p.parent_path().parent_path() / L"BundleRipper" / L"settings.txt";
}

// Settings are stored as the command-line arguments that reproduce them, one per line.
bool load_settings(Job& job) {
    fs::path p = settings_file();
    std::error_code ec;
    if (!fs::exists(p, ec) && fs::exists(legacy_settings_file(), ec)) p = legacy_settings_file();
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::vector<std::wstring> args;
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        args.push_back(widen(line));
    }
    try {
        job = parse_args(args).job;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void save_settings(const Job& job) {
    fs::path p = settings_file();
    if (p.empty()) return;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    for (auto& a : job_args(job)) f << narrow(a) << "\n";
}

// A shell file dialog: open a file, pick a folder, or save a .glb. Empty when cancelled.
fs::path pick_path(HWND owner, bool folder, bool save, const fs::path& start, const wchar_t* title,
                   const wchar_t* filter = nullptr) {
    IFileDialog* dlg = nullptr;
    HRESULT hr = save ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS((IFileSaveDialog**)&dlg))
                      : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS((IFileOpenDialog**)&dlg));
    if (FAILED(hr) || !dlg) return {};
    DWORD options = 0;
    dlg->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
    if (folder) options |= FOS_PICKFOLDERS;
    if (save) options |= FOS_OVERWRITEPROMPT;
    dlg->SetOptions(options);
    dlg->SetTitle(title);
    if (save) {
        static const COMDLG_FILTERSPEC types[] = {{L"glTF binary (*.glb)", L"*.glb"}};
        dlg->SetFileTypes(1, types);
        dlg->SetDefaultExtension(L"glb");
        if (start.has_filename()) dlg->SetFileName(start.filename().c_str());
    } else if (filter) {
        const COMDLG_FILTERSPEC types[] = {{filter, filter}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, types);
    } else if (!folder) {
        static const COMDLG_FILTERSPEC types[] = {{L"All files", L"*.*"},
                                                  {L"Unity bundles (*.bundle, *.unity3d, *.assets)", L"*.bundle;*.unity3d;*.assets"}};
        dlg->SetFileTypes(2, types);
    }
    std::error_code ec;
    fs::path dir = fs::is_directory(start, ec) ? start : start.parent_path();
    IShellItem* item = nullptr;
    if (!dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        dlg->SetFolder(item);
        item->Release();
    }
    fs::path result;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* picked = nullptr;
        if (SUCCEEDED(dlg->GetResult(&picked))) {
            PWSTR name = nullptr;
            if (SUCCEEDED(picked->GetDisplayName(SIGDN_FILESYSPATH, &name))) result = name;
            CoTaskMemFree(name);
            picked->Release();
        }
    }
    dlg->Release();
    ImGui::GetIO().AddMouseButtonEvent(0, false);  // the release happened inside the dialog
    return result;
}

void reveal(const fs::path& p) {
    std::error_code ec;
    if (fs::exists(p, ec)) {
        std::wstring args = L"/select,\"" + p.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else if (!p.parent_path().empty()) {
        ShellExecuteW(nullptr, L"open", p.parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

std::string format_size(uint64_t bytes) {
    char buf[32];
    if (bytes >= (1ull << 30)) std::snprintf(buf, sizeof buf, "%.2f GB", bytes / 1073741824.0);
    else std::snprintf(buf, sizeof buf, "%.1f MB", bytes / 1048576.0);
    return buf;
}

// ---------------------------------------------------------------------------------------------
// Direct3D 11

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_target = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;
std::vector<fs::path> g_dropped;

void create_target() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_target);
        back->Release();
    }
}

void release_target() {
    if (g_target) g_target->Release();
    g_target = nullptr;
}

bool create_device(HWND window) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = window;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &sd, &g_swap, &g_device, &got, &g_context);
    if (FAILED(hr))  // no usable GPU: software rendering
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                           &g_swap, &g_device, &got, &g_context);
    if (FAILED(hr)) return false;
    create_target();
    return true;
}

void destroy_device() {
    release_target();
    if (g_swap) g_swap->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    g_swap = nullptr, g_context = nullptr, g_device = nullptr;
}

// The frame just drawn, as a PNG (test hook, see run_gui).
void save_frame(const fs::path& file) {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (!back) return;
    D3D11_TEXTURE2D_DESC desc;
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &staging))) {
        g_context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            Image img;
            img.w = (int)desc.Width, img.h = (int)desc.Height;
            img.px.resize((size_t)img.w * img.h * 4);
            for (int y = 0; y < img.h; ++y)  // encode_png takes rows bottom-up
                std::memcpy(&img.px[(size_t)(img.h - 1 - y) * img.w * 4], (const uint8_t*)mapped.pData + (size_t)y * mapped.RowPitch,
                            (size_t)img.w * 4);
            g_context->Unmap(staging, 0);
            std::vector<uint8_t> png = encode_png(img, 3);
            std::ofstream(file, std::ios::binary).write((const char*)png.data(), (std::streamsize)png.size());
        }
        staging->Release();
    }
    back->Release();
}

LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, msg, wparam, lparam)) return 1;
    switch (msg) {
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) g_resize_w = LOWORD(lparam), g_resize_h = HIWORD(lparam);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = (MINMAXINFO*)lparam;
        info->ptMinTrackSize.x = (LONG)S(940);
        info->ptMinTrackSize.y = (LONG)S(620);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wparam;
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            std::wstring path(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
            path.resize(DragQueryFileW(drop, i, path.data(), (UINT)path.size()));
            g_dropped.emplace_back(path);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_CLOSE:
        if (runner().state == RunState::Running &&
            MessageBoxW(window, L"A rip is still running. Stop it and close Spotbuilder?", L"Spotbuilder",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return 0;
        DestroyWindow(window);
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;  // no Alt menu
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, msg, wparam, lparam);
}

// ---------------------------------------------------------------------------------------------
// Widgets

// A path text field bound to `p`; shows `hint` greyed out while empty. True when edited.
bool path_field(const char* id, const char* hint, fs::path& p, float width) {
    struct Field {
        std::vector<char> text = std::vector<char>(4096);
        bool editing = false;
    };
    static std::unordered_map<ImGuiID, Field> fields;
    Field& field = fields[ImGui::GetID(id)];
    std::vector<char>& buf = field.text;
    if (!field.editing) {  // not being typed in: show the current value
        std::string s = u8(p);
        size_t n = std::min(s.size(), buf.size() - 1);
        std::memcpy(buf.data(), s.data(), n);
        buf[n] = 0;
    }
    ImGui::SetNextItemWidth(width);
    bool edited = ImGui::InputTextWithHint(id, hint, buf.data(), buf.size());
    field.editing = ImGui::IsItemActive();
    if (edited) p = fs::path(widen(buf.data()));
    return edited;
}

// A text field bound to `s`; shows `hint` greyed out while empty. True when edited.
bool text_field(const char* id, const char* hint, std::string& s, float width) {
    fs::path p = widen(s);
    if (!path_field(id, hint, p, width)) return false;
    s = u8(p);
    return true;
}

void note_text(const char* text) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col::muted, "%s", text);
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, S(4)));
}

// Label on the left; the caller's control fills the right part of the row.
void row_label(const char* label, float control_w) {
    float avail = ImGui::GetContentRegionAvail().x;  // SameLine's offset counts from the row group's left
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(avail - control_w);
}

void option_tooltip(const OptionDef& d) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) return;
    if (!ImGui::BeginTooltip()) return;
    ImGui::PushTextWrapPos(S(340));
    ImGui::TextUnformatted(d.help);
    std::string flag;
    if (d.kind == OptionDef::Toggle) {
        if (d.flag) flag += d.flag;
        if (d.off_flag) flag += std::string(flag.empty() ? "" : "  /  ") + d.off_flag;
    } else {
        flag = std::string(d.flag) + " " + d.value_name;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, col::muted);
    ImGui::TextUnformatted(flag.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// ---------------------------------------------------------------------------------------------
// The window

// The steps a run goes through, as the bottom strip shows them.
using Steps = std::vector<const char*>;
const Steps kRipSteps = {"Load", "Build", "Optimise", "Textures", "Write"};
const Steps kModSteps = {"Load", "Build", "Optimise", "Textures", "Write", "Blender", "Studio"};
const Steps kLoadSteps = {"Load", "Build", "Optimise", "Textures"};
const Steps kNoSteps = {};
constexpr int kFinished = 100;

// Which step a top-level log line starts (-1: none; kFinished: done).
int step_of(const std::string& l) {
    auto starts = [&](const char* s) { return l.rfind(s, 0) == 0; };
    if (starts("loading ")) return 0;
    if (starts("building scene")) return 1;
    if (starts("optimizing meshes")) return 2;
    if (starts("converting ")) return 3;
    if (starts("writing ")) return 4;
    if (starts("making ")) return 5;
    if (starts("compiling the mod")) return 6;
    if (starts("done:")) return kFinished;
    return -1;
}

// How far a counter line is ("textures 12/23", "tile 5 of 245", "[export  93%]"), 0..1; -1: not one.
float sub_progress(const std::string& l) {
    auto digit = [&](size_t i) { return i < l.size() && std::isdigit((unsigned char)l[i]); };
    auto number_before = [&](size_t end) {
        size_t a = end;
        while (a > 0 && digit(a - 1)) --a;
        return std::atof(l.c_str() + a);
    };
    for (size_t i = l.size(); i-- > 1;) {
        if (l[i] == '%' && digit(i - 1)) return (float)std::min(100.0, number_before(i)) / 100.f;
        if (l[i] == '/' && digit(i - 1) && digit(i + 1)) {
            double num = number_before(i), den = std::atof(l.c_str() + i + 1);
            if (den > 0 && num <= den) return (float)(num / den);
        }
        if (l.compare(i, 4, " of ") == 0 && digit(i - 1) && digit(i + 4)) {
            double num = number_before(i), den = std::atof(l.c_str() + i + 4);
            if (den > 0 && num <= den) return (float)(num / den);
        }
    }
    return -1;
}

// A log line without its indent and tool prefixes ("studio: [12.3s] ").
std::string plain_line(const std::string& l) {
    size_t a = l.find_first_not_of(' ');
    std::string t = a == std::string::npos ? std::string() : l.substr(a);
    for (const char* prefix : {"studio: ", "blender: "})
        if (t.rfind(prefix, 0) == 0) t.erase(0, std::strlen(prefix));
    if (!t.empty() && t[0] == '[') {
        size_t close = t.find("] ");
        if (close != std::string::npos && close < 16 && t.find('s') < close) t.erase(0, close + 2);
    }
    return t;
}

// `text` cut with an ellipsis to fit `width`.
std::string fit_text(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    std::string t = text;
    while (!t.empty() && ImGui::CalcTextSize((t + "\xE2\x80\xA6").c_str()).x > width) {
        t.pop_back();
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();  // whole UTF-8 characters
    }
    return t + "\xE2\x80\xA6";
}

class App {
public:
    App(HWND window, const Fonts& fonts, Job job, SceneView& scene)
        : window_(window), fonts_(fonts), job_(std::move(job)), scene_(scene) {
        absolute_input();
        last_input_ = job_.input;
        std::error_code ec;
        want_load_ = !job_.input.empty() && fs::exists(job_.input, ec);  // reopen the last map
        load_map_edits();
    }
    // Writes unsaved map edits (the window is closing).
    void flush_edits() {
        if (edits_dirty_) save_map_edits();
    }
    void frame();
    const Job& job() const { return job_; }
    void press_rip() { rip(false); }
    void scan() { begin(job_, RunKind::Scan); }
    void open_scene() { want_load_ = true; }
    void open_settings() { drawer_open_ = true; }
    void open_dock() { dock_open_ = true; }
    bool scene_ready() const { return scene_.loaded() && !running() && !want_load_; }
    void open_export(bool mod) { job_.build_mod = mod, open_export_ = true; }

private:
    HWND window_;
    Fonts fonts_;
    Job job_;
    std::vector<std::string> log_;
    int generation_ = 0;
    size_t consumed_ = 0;  // runner lines already taken into log_
    int step_ = -1;
    float sub_ = 0;               // progress within the current step
    Steps steps_ = kRipSteps;     // of the run shown in the bottom strip
    std::string last_line_;       // the newest log line, for the strip
    bool scroll_to_end_ = false;
    std::string toast_;
    double toast_until_ = 0;
    SceneView& scene_;
    std::vector<std::wstring> scene_key_;  // what the shown scene was loaded with
    std::vector<std::wstring> pending_key_;
    std::string scene_error_;
    int scene_generation_ = -1;   // the load or scan whose result was taken
    int announced_ = -1;          // the export whose result was announced
    bool want_load_ = false;      // load the scene as soon as nothing else runs
    bool drawer_open_ = false;
    bool open_export_ = false;   // open the Export window next frame
    float drawer_t_ = 0;          // settings drawer slide, 0..1
    bool dock_open_ = false;      // the bottom dock's panel (its tab strip always shows)
    int dock_tab_ = 0;            // 0: Output
    float dock_h_ = 0;            // panel height (0: not set yet)
    // A mod build's tools, looked up now and then while the settings show
    StudioTools tools_;
    bool skate_running_ = false;
    double tools_at_ = -100;

    std::vector<PlantInfo> plants_;   // what the map paints on its terrains (after a scan or a scene load)
    bool plants_known_ = false;
    fs::path plants_map_;             // the map plants_ belongs to
    fs::path last_input_;
    fs::path scene_input_;  // the map the scene view shows
    bool scroll_to_plants_ = false;

    // Map edits, kept beside the map (<map>.spotbuilder.json) and saved shortly after each change
    std::shared_ptr<Edits> edits_ = std::make_shared<Edits>();
    // Undo: whole copies of the edits. A change becomes one step once nothing is being dragged or
    // typed into, so a slider drag, a gizmo drag or a typed name undoes in one go.
    std::vector<Edits> undo_, redo_;
    Edits committed_;  // the edits as of the newest step
    bool undo_pending_ = false;
    void reset_undo() {
        undo_.clear();
        redo_.clear();
        committed_ = *edits_;
        undo_pending_ = false;
    }
    void commit_undo() {
        if (!undo_pending_) return;
        undo_pending_ = false;
        undo_.push_back(std::move(committed_));
        if (undo_.size() > 200) undo_.erase(undo_.begin());
        committed_ = *edits_;
        redo_.clear();
    }
    void restore(std::vector<Edits>& from, std::vector<Edits>& to, const char* what) {
        commit_undo();
        if (from.empty()) return;
        to.push_back(*edits_);
        *edits_ = std::move(from.back());
        from.pop_back();
        committed_ = *edits_;
        scene_.edits_restored();
        edits_dirty_ = true;
        edits_changed_at_ = ImGui::GetTime();
        notify(std::string(what) + (from.empty() ? "" : "  \xC2\xB7  " + std::to_string(from.size()) + " more"));
    }
    void undo() { restore(undo_, redo_, "Undone"); }
    void redo() { restore(redo_, undo_, "Redone"); }
    bool edits_dirty_ = false;
    double edits_changed_at_ = 0;
    fs::path migrate_from_;  // edits read from the file the old name wrote, replaced on the next save
    void load_map_edits() {
        *edits_ = Edits{};
        edits_dirty_ = false;
        reset_undo();
        if (job_.input.empty()) return;
        fs::path file = existing_edits_file(job_.input);
        std::error_code ec;
        migrate_from_.clear();
        if (!fs::exists(file, ec)) return;
        std::string why;
        if (!load_edits(file, *edits_, why)) notify("Could not read the saved edits: " + why);
        else if (file != edits_file(job_.input)) migrate_from_ = file;  // the old name: moves on the next save
        reset_undo();
    }
    void save_map_edits() {
        edits_dirty_ = false;
        if (job_.input.empty()) return;
        fs::path file = edits_file(job_.input);
        std::error_code ec;
        Edits out = *edits_;
        if (scene_.loaded() && scene_input_ == job_.input) scene_.drop_unchanged_imports(out);  // made again on load
        if (out.empty()) {
            if (fs::exists(file, ec)) fs::remove(file, ec);  // nothing changed any more: no file
        } else {
            save_edits(file, out);
        }
        if (!migrate_from_.empty()) {  // read from <map>.bundleripper.json: now in the new file (or nothing left)
            fs::remove(migrate_from_, ec);
            migrate_from_.clear();
        }
    }
    // Edits that change something (the map's lights made editable but untouched do not count).
    size_t real_edit_count() const {
        size_t n = edits_->count();
        if (scene_.loaded() && scene_input_ == job_.input) n -= scene_.unchanged_imports(*edits_);
        return n;
    }

    // The settings that change what the scene view shows (LOD choices switch live).
    static std::vector<std::wstring> scene_key(const Job& job) {
        Job j = job;
        j.output.clear();
        j.texture_dump.clear();
        j.verbose = false;
        j.opt.lod = 0;
        j.opt.tree_lod = -1;
        j.opt.all_lods = false;
        j.opt.plant_lod.clear();
        return job_args(j);
    }
    void absolute_input() {
        std::error_code ec;
        if (!job_.input.empty() && job_.input.is_relative()) job_.input = fs::absolute(job_.input, ec).lexically_normal();
    }
    void open_map(const fs::path& p) {
        job_.input = p;
        job_.output.clear();
        absolute_input();
        want_load_ = true;
    }
    void choose_map(bool folder) {
        fs::path p = pick_path(window_, folder, false, job_.input,
                               folder ? L"Choose a folder of Unity bundles" : L"Choose a Unity map bundle");
        if (!p.empty()) open_map(p);
    }
    bool can_export() const {
        std::error_code ec;
        return !running() && !job_.input.empty() && fs::exists(job_.input, ec);
    }

    void sync_log();
    void take_scene();
    void announce();
    void top_bar(float height);
    void settings_drawer(float top, float bottom);
    void bottom_dock();
    float dock_height() const { return S(34) + (dock_open_ ? dock_h_ : 0); }
    void output_tab(ImVec2 size);
    void export_dialog();
    void glb_output();
    void mod_status();
    void plants_card();
    void card(const char* group, int format = 0);
    void option_row(const OptionDef& d);
    void notify(const std::string& text) {
        toast_ = text;
        toast_until_ = ImGui::GetTime() + 2.5;
    }
    bool running() const { return runner().state == RunState::Running; }
    const char* export_label() const { return job_.build_mod ? "Build mod" : "Export"; }
    void begin(const Job& job, RunKind kind) {
        steps_ = kind == RunKind::Rip ? (job.build_mod ? kModSteps : kRipSteps) : kind == RunKind::Load ? kLoadSteps : kNoSteps;
        start_job(job, kind);
    }
    void rip(bool list) {
        Job j = job_;
        j.list = list;
        j.edits = std::make_shared<const Edits>(*edits_);
        save_settings(job_);
        begin(j, list ? RunKind::List : RunKind::Rip);
        if (list) dock_open_ = true, dock_tab_ = 0;  // the listing is the log
    }
    void load_scene() {
        save_settings(job_);
        pending_key_ = scene_key(job_);
        scene_error_.clear();
        begin(job_, RunKind::Load);
    }
};

void App::sync_log() {
    Runner& r = runner();
    std::lock_guard lock(r.mutex);
    if (r.generation != generation_) {
        generation_ = r.generation;
        log_.clear();
        consumed_ = 0;
        step_ = 0;
        sub_ = 0;
        last_line_.clear();
    }
    // Counters like "  textures 12/23" update one line instead of adding one each.
    auto counter = [](const std::string& l) {
        size_t slash = l.find('/');
        return l.size() > 2 && l[0] == ' ' && slash != std::string::npos && slash + 1 < l.size() &&
               std::isdigit((unsigned char)l[slash - 1]) && std::isdigit((unsigned char)l[slash + 1]) &&
               l.find_first_of("()") == std::string::npos;
    };
    if (r.lines.size() > consumed_) {
        for (; consumed_ < r.lines.size(); ++consumed_) {
            const std::string& l = r.lines[consumed_];
            int s = step_of(l);
            if (s >= 0) {
                if (s != step_) sub_ = 0;
                step_ = s;
            } else if (!l.empty() && l[0] == ' ') {
                float f = sub_progress(l);
                if (f >= 0) sub_ = f;
            }
            if (l.find_first_not_of(' ') != std::string::npos) last_line_ = plain_line(l);
            if (counter(l) && !log_.empty() && counter(log_.back()) &&
                l.compare(0, l.find_last_of(' '), log_.back(), 0, log_.back().find_last_of(' ')) == 0)
                log_.back() = l;
            else
                log_.push_back(l);
        }
        scroll_to_end_ = true;
    }
}

void App::frame() {
    for (auto& p : g_dropped) open_map(p);  // a map dropped on the window
    g_dropped.clear();
    absolute_input();
    if (job_.input != last_input_) {  // another map: its plants, LOD choices and edits start over
        if (edits_dirty_) {  // the old map's edits first
            fs::path now = job_.input;
            job_.input = last_input_;
            save_map_edits();
            job_.input = now;
        }
        if (!last_input_.empty()) job_.opt.plant_lod.clear();
        last_input_ = job_.input;
        load_map_edits();
        scene_.edits_replaced();
    }
    sync_log();
    take_scene();
    announce();
    if (edits_dirty_ && ImGui::GetTime() - edits_changed_at_ > 0.5) save_map_edits();
    if (want_load_ && !running()) {
        want_load_ = false;
        std::error_code ec;
        if (!job_.input.empty() && fs::exists(job_.input, ec)) load_scene();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal) && !running()) choose_map(false);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Enter, ImGuiInputFlags_RouteGlobal) && can_export()) open_export_ = true;
    if (drawer_open_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) drawer_open_ = false;
    {  // Ctrl+Z undo, Ctrl+Y or Ctrl+Shift+Z redo (text fields keep their own)
        ImGuiIO& io = ImGui::GetIO();
        bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if (io.KeyCtrl && !io.WantTextInput && !popup && !ImGui::IsAnyItemActive()) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z) && !io.KeyShift) undo();
            else if (ImGui::IsKeyPressed(ImGuiKey_Y) || (ImGui::IsKeyPressed(ImGuiKey_Z) && io.KeyShift)) redo();
        }
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
    ImGui::Begin("Spotbuilder", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    float bar_h = S(40);
    top_bar(bar_h);
    float body_top = ImGui::GetCursorScreenPos().y;

    // The scene: object list, view and inspector
    SceneViewStatus status;
    status.busy = running();
    {
        Runner& r = runner();
        std::lock_guard lock(r.mutex);
        status.loading = (status.busy && r.kind == RunKind::Load) || want_load_;
    }
    if (status.loading) {
        for (auto it = log_.rbegin(); it != log_.rend(); ++it)
            if (!it->empty() && (*it)[0] != ' ') {
                status.step = *it;
                break;
            }
        if (status.step.size() > 90) status.step = status.step.substr(0, 87) + "...";
    }
    std::error_code ec;
    status.can_load = !job_.input.empty() && fs::exists(job_.input, ec);
    status.error = scene_error_;
    status.stale = scene_.loaded() && scene_key(job_) != scene_key_;
    if (dock_h_ <= 0) dock_h_ = S(250);
    dock_h_ = std::clamp(dock_h_, S(120), std::max(S(120), vp->Size.y - S(260)));
    float dock_top = vp->Pos.y + vp->Size.y - dock_height();
    ImVec2 scene_size(ImGui::GetContentRegionAvail().x, std::max(S(80), dock_top - S(6) - ImGui::GetCursorScreenPos().y));
    ImGui::BeginChild("scene", scene_size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    switch (scene_.draw(status, job_, *edits_)) {
    case SceneAction::Load: load_scene(); break;
    case SceneAction::OpenFile: choose_map(false); break;
    case SceneAction::OpenFolder: choose_map(true); break;
    case SceneAction::None: break;
    }
    ImGui::EndChild();
    ImGui::End();
    if (scene_.take_edits_changed()) {
        edits_dirty_ = true;
        edits_changed_at_ = ImGui::GetTime();
        undo_pending_ = true;
    }
    if (undo_pending_ && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) commit_undo();
    if (std::string m = scene_.take_message(); !m.empty()) notify(m);

    bottom_dock();
    settings_drawer(body_top, dock_top);
    export_dialog();

    if (!toast_.empty() && ImGui::GetTime() < toast_until_) {  // a short confirmation, bottom centre
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 ts = ImGui::CalcTextSize(toast_.c_str());
        ImVec2 pad(S(14), S(9));
        ImVec2 a(vp->Pos.x + (vp->Size.x - ts.x) * 0.5f - pad.x, dock_top - ts.y - pad.y * 2 - S(16));
        ImVec2 b(a.x + ts.x + pad.x * 2, a.y + ts.y + pad.y * 2);
        dl->AddRectFilled(a, b, ImGui::GetColorU32(rgb(40, 45, 58)), S(8));
        dl->AddText(ImVec2(a.x + pad.x, a.y + pad.y), ImGui::GetColorU32(col::text), toast_.c_str());
    }
}

// An export that finished gets a toast; a failure opens the log.
void App::announce() {
    Runner& r = runner();
    RunState state = r.state;
    if (state != RunState::Done && state != RunState::Failed) return;
    std::lock_guard lock(r.mutex);
    if (r.generation == announced_) return;
    announced_ = r.generation;
    if (state == RunState::Failed) {
        dock_open_ = true, dock_tab_ = 0;
    } else if (r.kind == RunKind::Rip && r.result.mod) {
        notify(r.result.deployed.empty() ? "Mod built in " + u8(r.result.output)
                                         : "Mod installed into Skate as " + u8(r.result.deployed.filename()));
    } else if (r.kind == RunKind::Rip && !r.result.output.empty()) {
        notify("Exported " + u8(r.result.output.filename()) + "  \xC2\xB7  " + format_size(r.result.bytes));
    }
}

void App::top_bar(float height) {
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float right = right_edge();
    float y = ImGui::GetCursorPosY();
    float frame_h = ImGui::GetFrameHeight();
    float mid = y + (height - frame_h) * 0.5f;

    ImGui::SetCursorPosY(y + (height - ImGui::GetFontSize() * 1.6f) * 0.5f);
    ImGui::PushFont(fonts_.title);
    ImGui::TextUnformatted("Spotbuilder");
    ImGui::PopFont();

    ImGui::SameLine(0, S(20));
    ImGui::SetCursorPosY(mid);
    ImGui::BeginDisabled(running());
    if (ImGui::Button("Open\xE2\x80\xA6")) ImGui::OpenPopup("open_menu");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Open a map (Ctrl+O), or drop one on the window");
    if (ImGui::BeginPopup("open_menu")) {
        if (ImGui::Selectable("Map file\xE2\x80\xA6")) choose_map(false);
        if (ImGui::Selectable("Folder of bundles\xE2\x80\xA6")) choose_map(true);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetCursorPosY(mid);
    ImGui::AlignTextToFramePadding();
    if (!job_.input.empty()) {
        ImGui::TextUnformatted(u8(job_.input.filename()).c_str());
        ImGui::SetItemTooltip("%s", u8(job_.input).c_str());
        if (size_t n = real_edit_count()) {
            ImGui::SameLine();
            ImGui::TextColored(col::muted, "\xC2\xB7  %zu %s", n, n == 1 ? "edit" : "edits");
            ImGui::SetItemTooltip("Saved beside the map in %s; exports (and command-line rips) apply them",
                                  u8(edits_file(job_.input).filename()).c_str());
        }
        ImGui::SameLine(0, S(14));
        ImGui::SetCursorPosY(mid);
        ImGui::BeginDisabled(undo_.empty() && !undo_pending_);
        if (ImGui::Button("Undo")) undo();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Undo the last edit (Ctrl+Z)");
        ImGui::SameLine(0, S(4));
        ImGui::SetCursorPosY(mid);
        ImGui::BeginDisabled(redo_.empty());
        if (ImGui::Button("Redo")) redo();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Redo (Ctrl+Y or Ctrl+Shift+Z)");
    } else {
        ImGui::TextColored(col::muted, "No map open");
    }

    // Right side: Settings and Export
    float export_w = S(118);
    ImGuiStyle& style = ImGui::GetStyle();
    float buttons_w = export_w + ImGui::CalcTextSize("Settings").x + S(24) + S(8);
    (void)style;
    ImGui::SameLine(right - buttons_w);
    ImGui::SetCursorPosY(mid);
    chip("Settings", drawer_open_, "How the map is ripped: textures, objects, LODs, optimisation, splines and terrain plants");
    ImGui::SameLine(0, S(8));
    ImGui::BeginDisabled(!can_export());
    if (accent_button("Export\xE2\x80\xA6", ImVec2(export_w, 0))) open_export_ = true;
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Choose a .glb file or a Skate mod, then export (Ctrl+Enter)");
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height + S(6)));
}

void App::settings_drawer(float top, float bottom) {
    float target = drawer_open_ ? 1.f : 0.f;
    drawer_t_ += (target - drawer_t_) * std::min(1.f, ImGui::GetIO().DeltaTime * 14.f);
    if (std::fabs(drawer_t_ - target) < 0.002f) drawer_t_ = target;
    if (drawer_t_ <= 0) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float w = std::min(S(500), vp->Size.x * 0.6f);
    float ease = 1 - (1 - drawer_t_) * (1 - drawer_t_);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - w * ease, top));
    ImGui::SetNextWindowSize(ImVec2(w, bottom - top));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, rgb(20, 23, 29));
    ImGui::PushStyleColor(ImGuiCol_Border, col::border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
    ImGui::Begin("##settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    // shadow along the left edge
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    bg->AddRectFilledMultiColor(ImVec2(wp.x - S(24), wp.y), ImVec2(wp.x, wp.y + ImGui::GetWindowHeight()), 0,
                                ImGui::GetColorU32(ImVec4(0, 0, 0, 0.35f)), ImGui::GetColorU32(ImVec4(0, 0, 0, 0.35f)), 0);

    float right = right_edge();
    ImGui::PushFont(fonts_.title);
    ImGui::TextUnformatted("Settings");
    ImGui::PopFont();
    ImGui::SameLine(right - ImGui::GetFrameHeight());
    if (ImGui::Button("\xC3\x97", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) drawer_open_ = false;
    ImGui::SetItemTooltip("Close (Esc)");

    float footer = S(40) + ImGui::GetStyle().ItemSpacing.y * 2 + S(8);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("settings_scroll", ImVec2(0, ImGui::GetContentRegionAvail().y - footer), ImGuiChildFlags_None);
    ImGui::PopStyleColor();
    note_text("How the map is ripped, whichever way it is exported. The .glb and Skate mod options are in the Export window.");
    const char* group = "";
    for (auto& d : option_table())
        if (d.format == 0 && std::string(group) != d.group) {
            group = d.group;
            card(group, 0);
        }
    plants_card();
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::BeginDisabled(!can_export());
    if (ImGui::Button("List contents")) rip(true);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Print what the map contains: object counts, scripts, plants and textures");
    ImGui::SameLine();
    if (ImGui::Button("Copy command line")) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::vector<std::wstring> args = {exe};
        for (auto& a : job_args(job_)) args.push_back(a);
        ImGui::SetClipboardText(narrow(join_args(args)).c_str());
        notify("Command line copied");
    }
    ImGui::SetItemTooltip("The same export from a terminal or a script, with these settings");
    ImGui::SameLine();
    if (ImGui::Button("Reset options")) {
        Job fresh;
        fresh.input = job_.input;
        fresh.output = job_.output;
        job_ = fresh;
        notify("Options reset to defaults");
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::BeginDisabled(!can_export());
    if (accent_button("Export\xE2\x80\xA6", ImVec2(-FLT_MIN, S(40)))) open_export_ = true;
    ImGui::EndDisabled();
    ImGui::End();
}

// The Export window: a .glb file or a Skate mod, each with its own options.
void App::export_dialog() {
    if (open_export_) {
        ImGui::OpenPopup("##export");
        open_export_ = false;
        tools_at_ = -100;  // look Blender and Studio up again
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float w = std::min(S(660), vp->Size.x - S(40)), h = std::min(S(720), vp->Size.y - S(60));
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22), S(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(22, 25, 32));
    ImGui::PushStyleColor(ImGuiCol_Border, col::border);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0.55f));
    bool open = ImGui::BeginPopupModal("##export", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
    if (!open) return;
    bool typing = ImGui::GetIO().WantTextInput;
    float right = right_edge();
    ImGui::PushFont(fonts_.title);
    ImGui::TextUnformatted("Export");
    ImGui::PopFont();
    ImGui::SameLine(right - ImGui::GetFrameHeight());
    if (ImGui::Button("\xC3\x97", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())) ||
        (!typing && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    std::string sub = u8(job_.input.filename());
    if (size_t n = edits_->count()) sub += "  \xC2\xB7  " + std::to_string(n) + (n == 1 ? " map edit" : " map edits") + " applied";
    ImGui::TextColored(col::muted, "%s", sub.c_str());
    ImGui::Dummy(ImVec2(0, S(6)));

    // The two ways out, as cards
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float gap = S(12);
        float card_w = (ImGui::GetContentRegionAvail().x - gap) * 0.5f, card_h = S(104);
        const char* titles[] = {".glb file", "Skate mod"};
        const char* texts[] = {"One file for Blender or other 3D tools: textures inside, the grind splines as .obj curves next to it.",
                               "Built by ReSkate Studio through Blender, with every map edit, grind curve and NPC route, and "
                               "installed into Skate ready to play."};
        for (int i = 0; i < 2; ++i) {
            if (i) ImGui::SameLine(0, gap);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##format", ImVec2(card_w, card_h))) job_.build_mod = i == 1;
            bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            bool on = job_.build_mod == (i == 1);
            ImVec2 q(p.x + card_w, p.y + card_h);
            dl->AddRectFilled(p, q, ImGui::GetColorU32(on ? rgb(96, 142, 255, 0.13f) : hovered ? col::field_hover : col::field), S(10));
            dl->AddRect(p, q, ImGui::GetColorU32(on ? col::accent : col::border), S(10), 0, on ? S(2) : S(1));
            ImVec2 dot(p.x + S(22), p.y + S(26));
            dl->AddCircle(dot, S(7), ImGui::GetColorU32(on ? col::accent : col::muted), 20, S(1.5f));
            if (on) dl->AddCircleFilled(dot, S(3.5f), ImGui::GetColorU32(col::accent), 16);
            ImGui::PushFont(fonts_.semibold);
            dl->AddText(ImVec2(p.x + S(38), p.y + S(16)), ImGui::GetColorU32(col::text), titles[i]);
            ImGui::PopFont();
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + S(18), p.y + S(44)), ImGui::GetColorU32(col::muted), texts[i],
                        nullptr, card_w - S(36));
        }
    }
    ImGui::Dummy(ImVec2(0, S(6)));

    // Its options
    float footer = S(60);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##export_options", ImVec2(0, ImGui::GetContentRegionAvail().y - footer), ImGuiChildFlags_None);
    ImGui::PopStyleColor();
    if (!job_.build_mod) {
        glb_output();
        card("Blender", 1);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(14)));
        ImGui::BeginChild("glb_extra", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        ImGui::PushFont(fonts_.semibold);
        ImGui::TextUnformatted("Extras");
        ImGui::PopFont();
        for (auto& d : option_table())
            if (d.format == 1 && std::string(d.group) != "Blender") option_row(d);
        ImGui::EndChild();
    } else {
        mod_status();
        card("Skate mod", 2);
        card("Skate mod tools", 2);
    }
    note_text("How the map is ripped (textures, objects, LODs, optimisation, splines) follows Settings.");
    ImGui::EndChild();

    // Go
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, S(4)));
    bool ready = can_export();
    std::string why;
    if (job_.build_mod) {
        if (!tools_.missing.empty()) ready = false, why = tools_.missing;
        else if (mod_name(job_).empty()) ready = false, why = "the mod needs a name";
        else if (job_.mod.deploy && skate_running_) ready = false, why = "Skate is running: close it, or turn off Install into Skate";
    }
    const char* go = job_.build_mod ? "Build mod" : "Export .glb";
    float go_w = S(150), cancel_w = S(96);
    std::string left = !why.empty() ? why
                       : job_.build_mod ? (job_.mod.deploy ? "Builds and installs " + mod_name(job_) : "Builds " + mod_name(job_) + " (not installed)")
                                        : "Writes " + u8(output_path(job_).filename());
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(why.empty() ? col::muted : col::warn, "%s",
                       fit_text(left, ImGui::GetContentRegionAvail().x - go_w - cancel_w - S(24)).c_str());
    ImGui::SameLine(right - go_w - cancel_w - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button("Cancel", ImVec2(cancel_w, S(36)))) ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    ImGui::BeginDisabled(!ready);
    bool pressed = accent_button(go, ImVec2(go_w, S(36)));
    ImGui::EndDisabled();
    if (pressed || (ready && !typing && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))) {
        ImGui::CloseCurrentPopup();
        rip(false);
    }
    ImGui::EndPopup();
}

// A finished scene load goes to the view; a failed one leaves its reason.
void App::take_scene() {
    Runner& r = runner();
    RunState state = r.state;
    if (state == RunState::Running || state == RunState::Idle) return;
    std::unique_ptr<Preview> preview;
    std::unique_ptr<std::vector<PlantInfo>> plants;
    {
        std::lock_guard lock(r.mutex);
        if ((r.kind != RunKind::Load && r.kind != RunKind::Scan) || r.generation == scene_generation_) return;
        scene_generation_ = r.generation;
        preview = std::move(r.preview);
        plants = std::move(r.plants);
        if (state == RunState::Failed && r.kind == RunKind::Load) scene_error_ = r.error;
    }
    if (preview) {
        plants_ = preview->plants;  // the view keeps its own copy
        plants_known_ = true;
        plants_map_ = job_.input;
        scene_.set(std::move(*preview));
        scene_key_ = pending_key_;
        scene_input_ = job_.input;
        if (scene_.import_map_lights(*edits_)) reset_undo();  // the map's lights, editable: where undo starts
    }
    if (plants) {
        plants_ = std::move(*plants);
        plants_known_ = true;
        plants_map_ = job_.input;
        scroll_to_plants_ = true;  // show what the scan found
    }
}

// Where the .glb goes.
void App::glb_output() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(14)));
    ImGui::BeginChild("output", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PushFont(fonts_.semibold);
    ImGui::TextUnformatted("Output file");
    ImGui::PopFont();
    float w = ImGui::GetContentRegionAvail().x;
    Job defaults = job_;
    defaults.output.clear();
    std::string hint = job_.input.empty() ? "<map>.glb next to the map" : u8(output_path(defaults));
    float browse_w = ImGui::CalcTextSize("Browse\xE2\x80\xA6").x + ImGui::GetStyle().FramePadding.x * 2;
    float reset_w = job_.output.empty() ? 0 : ImGui::CalcTextSize("Default").x + ImGui::GetStyle().FramePadding.x * 2 +
                                                  ImGui::GetStyle().ItemSpacing.x;
    path_field("##output", hint.c_str(), job_.output, w - browse_w - reset_w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::SameLine();
    if (ImGui::Button("Browse\xE2\x80\xA6")) {
        fs::path p = pick_path(window_, false, true, output_path(job_), L"Save the .glb as");
        if (!p.empty()) job_.output = p;
    }
    if (!job_.output.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Default")) job_.output.clear();
        ImGui::SetItemTooltip("Back to <map>.glb next to the map");
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, S(4)));
}

// What a mod build will use: the mod's name and where Blender, Studio and Skate were found.
void App::mod_status() {
    if (ImGui::GetTime() - tools_at_ > 2.0) {
        tools_ = find_studio_tools(job_.mod);
        skate_running_ = skate_running();
        tools_at_ = ImGui::GetTime();
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(14)));
    ImGui::BeginChild("mod_status", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PushFont(fonts_.semibold);
    ImGui::TextUnformatted("ReSkate Studio");
    ImGui::PopFont();
    auto tool = [&](const char* what, const fs::path& found, const char* missing) {
        // a drawn tick or cross (the fonts have neither)
        float h = ImGui::GetTextLineHeight();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float k = h * 0.28f;
        ImVec2 c(p.x + h * 0.4f, p.y + h * 0.5f);
        if (found.empty()) {
            ImU32 bad = ImGui::GetColorU32(col::bad);
            dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), bad, S(1.8f));
            dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), bad, S(1.8f));
        } else {
            ImVec2 pts[3] = {ImVec2(c.x - k, c.y), ImVec2(c.x - k * 0.25f, c.y + k * 0.8f), ImVec2(c.x + k * 1.1f, c.y - k * 0.9f)};
            dl->AddPolyline(pts, 3, ImGui::GetColorU32(col::good), 0, S(1.8f));
        }
        ImGui::Dummy(ImVec2(h, h));
        ImGui::SameLine();
        ImGui::TextUnformatted(what);
        ImGui::SameLine(S(140));
        std::string text = found.empty() ? std::string(missing) : u8(found);
        ImGui::TextColored(found.empty() ? col::bad : col::muted, "%s", fit_text(text, ImGui::GetContentRegionAvail().x).c_str());
        if (!found.empty()) ImGui::SetItemTooltip("%s", text.c_str());
    };
    tool("Blender", tools_.blender, "not found: set it under Skate mod tools");
    tool("ReSkate Studio", tools_.cli, "reskate_cli.exe not found: set it under Skate mod tools");
    tool("Skate", tools_.game, "game folder not found: set it under Skate mod tools");
    std::string name = mod_name(job_);
    if (job_.mod.deploy && !tools_.game.empty() && !name.empty())
        ImGui::TextColored(col::muted, "Installs into %s", fit_text(u8(tools_.game / L"Mods" / widen(name)), ImGui::GetContentRegionAvail().x - S(80)).c_str());
    if (job_.mod.deploy && skate_running_) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(col::warn, "Skate is running. Close it before building, or turn off Install into Skate.");
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, S(4)));
}

void App::option_row(const OptionDef& d) {
    bool enabled = !d.enabled || d.enabled(job_);
    ImGui::BeginDisabled(!enabled);
    ImGui::PushID(d.label);
    ImGui::BeginGroup();
    float control_w = std::min(S(200), ImGui::GetContentRegionAvail().x * 0.55f);
    switch (d.kind) {
    case OptionDef::Toggle: {
        bool v = d.get(job_) != 0;
        if (switch_row(d.label, v)) d.set(job_, v ? 1 : 0);
        break;
    }
    case OptionDef::Choice: {
        int v = (int)d.get(job_);
        std::string preview = std::to_string(v);
        for (auto& it : d.items)
            if (it.value == v) preview = it.label;
        row_label(d.label, control_w);
        ImGui::SetNextItemWidth(control_w);
        if (ImGui::BeginCombo("##v", preview.c_str())) {
            for (auto& it : d.items)
                if (ImGui::Selectable(it.label, it.value == v)) d.set(job_, it.value);
            ImGui::EndCombo();
        }
        break;
    }
    case OptionDef::Number: {
        float v = (float)d.get(job_);
        row_label(d.label, control_w);
        ImGui::SetNextItemWidth(control_w);
        std::string format = v == 0 ? "0 (lossless only)" : std::string("%.1f ") + d.unit;
        if (ImGui::SliderFloat("##v", &v, (float)d.min, (float)d.max, format.c_str(), ImGuiSliderFlags_AlwaysClamp))
            d.set(job_, std::round(v * 10) / 10);
        break;
    }
    case OptionDef::Integer: {
        int v = (int)d.get(job_);
        row_label(d.label, control_w);
        ImGui::SetNextItemWidth(control_w);
        std::string format = std::string("%d ") + d.unit;
        if (ImGui::DragInt("##v", &v, 4, (int)d.min, (int)d.max, format.c_str(), ImGuiSliderFlags_AlwaysClamp))
            d.set(job_, v);
        break;
    }
    case OptionDef::Folder:
    case OptionDef::File: {
        fs::path& p = d.path(job_);
        float bw = ImGui::GetFrameHeight();
        row_label(d.label, control_w);
        path_field("##v", d.placeholder, p, control_w - bw - ImGui::GetStyle().ItemSpacing.x);
        ImGui::SameLine();
        if (ImGui::Button("\xE2\x80\xA6", ImVec2(bw, 0))) {
            std::wstring title = widen(d.label);
            fs::path picked = pick_path(window_, d.kind == OptionDef::Folder, false, p.empty() ? job_.input : p,
                                        title.c_str(), d.filter);
            if (!picked.empty()) p = picked;
        }
        break;
    }
    case OptionDef::Text: {
        row_label(d.label, control_w);
        text_field("##v", d.placeholder, d.text(job_), control_w);
        break;
    }
    }
    ImGui::EndGroup();
    option_tooltip(d);
    ImGui::PopID();
    ImGui::EndDisabled();
}

void App::card(const char* group, int format) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(14)));
    ImGui::BeginChild(group, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PushFont(fonts_.semibold);
    ImGui::TextUnformatted(group);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, S(1)));
    for (auto& d : option_table())
        if (std::string(d.group) == group && d.format == format) option_row(d);
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, S(4)));
}

std::string with_commas(size_t v) {
    std::string s = std::to_string(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return s;
}

// The trees, bushes and grass painted on the map's terrains, each with its own LOD choice.
void App::plants_card() {
    if (scroll_to_plants_) {  // bring the card to the top of the options list
        ImGui::SetScrollY(std::max(0.0f, ImGui::GetCursorPosY() - S(4)));
        scroll_to_plants_ = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(14)));
    ImGui::BeginChild("plants", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    bool known = plants_known_ && plants_map_ == job_.input;
    float right = right_edge();
    ImGui::PushFont(fonts_.semibold);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Terrain plants");
    ImGui::PopFont();
    if (known && !plants_.empty()) {
        size_t placed = 0;
        for (auto& p : plants_) placed += p.placed;
        ImGui::SameLine();
        ImGui::TextColored(col::muted, "%zu types, %s placed", plants_.size(), with_commas(placed).c_str());
    }
    const char* scan_label = known ? "Rescan" : "Scan map";
    ImGui::SameLine(right - ImGui::CalcTextSize(scan_label).x - ImGui::GetStyle().FramePadding.x * 2);
    std::error_code ec;
    ImGui::BeginDisabled(running() || job_.input.empty() || !fs::exists(job_.input, ec));
    if (ImGui::Button(scan_label)) begin(job_, RunKind::Scan);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("List the trees, bushes and grass the map paints on its terrains (loading the scene does too)");

    if (!known) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(col::muted, "Scan the map to list the trees, bushes and grass painted on its terrains, and pick a "
                                       "level of detail for each. Lower levels can cut foliage triangles a lot.");
        ImGui::PopTextWrapPos();
    } else if (plants_.empty()) {
        ImGui::TextColored(col::muted, "This map paints nothing on its terrains.");
    } else {
        auto& own = job_.opt.plant_lod;
        size_t total = 0, total_full = 0;
        ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX;
        ImGui::PushStyleColor(ImGuiCol_TableRowBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, rgb(255, 255, 255, 0.025f));
        if (ImGui::BeginTable("plant_table", 4, flags)) {
            ImGui::TableSetupColumn("Plant", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Placed", ImGuiTableColumnFlags_WidthFixed, S(70));
            ImGui::TableSetupColumn("Level of detail", ImGuiTableColumnFlags_WidthFixed, S(210));
            ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthFixed, S(110));
            ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, col::muted);
            ImGui::TableHeadersRow();
            ImGui::PopStyleColor(2);
            for (auto& p : plants_) {
                ImGui::PushID(p.name.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(p.name.c_str());
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(with_commas(p.placed).c_str());
                ImGui::TableNextColumn();
                auto it = own.find(p.name);
                int choice = it != own.end() ? it->second : -1;
                int level = plant_level(p, choice, job_.opt.tree_lod, job_.opt.lod);
                int last = (int)p.lod_triangles.size() - 1;
                auto level_name = [&](int l) {
                    std::string s = "LOD" + std::to_string(l) + "  \xC2\xB7  " + with_commas(p.lod_triangles[l]);
                    if (l == last && last > 0) s += "  (lowest)";
                    return s;
                };
                std::string preview = p.lod_triangles.size() < 2 ? "No LODs" : (choice < 0 ? "Default: " : "") + level_name(level);
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::BeginDisabled(p.lod_triangles.size() < 2 || job_.opt.all_lods);
                if (ImGui::BeginCombo("##lod", preview.c_str())) {
                    if (ImGui::Selectable("Default (Trees and grass LOD)", choice < 0)) own.erase(p.name);
                    for (int l = 0; l <= last; ++l)
                        if (ImGui::Selectable(level_name(l).c_str(), choice >= 0 && level == l)) own[p.name] = l == last ? 99 : l;
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                size_t drawn = p.placed * (p.lod_triangles.empty() ? 0 : p.lod_triangles[level]);
                total += drawn;
                total_full += p.placed * (p.lod_triangles.empty() ? 0 : p.lod_triangles[0]);
                ImGui::TextUnformatted(with_commas(drawn).c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleColor(2);
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::TextColored(col::muted, "Plants draw %s triangles", with_commas(total).c_str());
        if (total_full > total) {
            ImGui::SameLine();
            ImGui::TextColored(col::good, "(%.0f%% of full detail)", 100.0 * total / total_full);
        }
        float set_w = 0;
        const char* sets[] = {"All default", "All full detail", "All lowest"};
        for (const char* l : sets) set_w += ImGui::CalcTextSize(l).x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SameLine(right - set_w + ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button(sets[0])) own.clear();
        ImGui::SameLine();
        if (ImGui::Button(sets[1]))
            for (auto& p : plants_) own[p.name] = 0;
        ImGui::SameLine();
        if (ImGui::Button(sets[2]))
            for (auto& p : plants_)
                if (p.lod_triangles.size() > 1) own[p.name] = 99;
    }
    ImGui::EndChild();
}

// The strip along the bottom of the window: its tabs, what is running and how far it is, and
// (opened) the tab's panel above the strip's bottom edge.
void App::bottom_dock() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float strip = S(34);
    float total = dock_height();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - total));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, total));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, rgb(20, 23, 29));
    ImGui::Begin("##dock", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float w = vp->Size.x;
    dl->AddLine(o, ImVec2(o.x + w, o.y), ImGui::GetColorU32(col::border), S(1));

    // Dragging the top edge resizes the open panel
    if (dock_open_) {
        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y - S(3)));
        ImGui::InvisibleButton("##dock_resize", ImVec2(w, S(7)));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive()) dock_h_ -= ImGui::GetIO().MouseDelta.y;
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            dl->AddLine(o, ImVec2(o.x + w, o.y), ImGui::GetColorU32(col::accent), S(2));
    }

    Runner& r = runner();
    RunState state = r.state;
    RunKind kind;
    JobResult result;
    std::string error;
    double seconds = 0;
    {
        std::lock_guard lock(r.mutex);
        kind = r.kind, result = r.result, error = r.error, seconds = r.seconds;
    }
    if (state == RunState::Running) seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - r.started).count();

    // Tabs
    float pad = S(14);
    float cy = o.y + strip * 0.5f;
    float x = o.x + pad * 0.5f;
    const char* tabs[] = {"Output"};
    for (int i = 0; i < IM_ARRAYSIZE(tabs); ++i) {
        ImVec2 ts = ImGui::CalcTextSize(tabs[i]);
        ImVec2 a(x, o.y), b(x + ts.x + S(28), o.y + strip);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(b.x - a.x, b.y - a.y))) {
            if (dock_open_ && dock_tab_ == i) dock_open_ = false;
            else dock_open_ = true, dock_tab_ = i;
        }
        bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        bool active = dock_open_ && dock_tab_ == i;
        if (hovered) dl->AddRectFilled(ImVec2(a.x, a.y + S(4)), ImVec2(b.x, b.y - S(4)), ImGui::GetColorU32(rgb(255, 255, 255, 0.04f)), S(6));
        float tx = a.x + S(14);
        // the chevron says which way the tab goes
        float cx = tx + ts.x + S(6);
        (void)cx;
        dl->AddText(ImVec2(tx, cy - ts.y * 0.5f), ImGui::GetColorU32(active || hovered ? col::text : col::muted), tabs[i]);
        if (active) dl->AddRectFilled(ImVec2(tx, b.y - S(3)), ImVec2(tx + ts.x, b.y - S(1)), ImGui::GetColorU32(col::accent), S(1));
        ImGui::SetItemTooltip(active ? "Hide the panel" : "Progress, messages and the result of the last run");
        x = b.x + S(4);
    }
    dl->AddLine(ImVec2(x + S(6), o.y + S(9)), ImVec2(x + S(6), o.y + strip - S(9)), ImGui::GetColorU32(col::border), S(1));
    x += S(18);

    // What is running
    int n = (int)steps_.size();
    bool running_now = state == RunState::Running;
    const char* what = kind == RunKind::Load ? "Loading scene" : kind == RunKind::Scan ? "Scanning plants"
                      : kind == RunKind::List ? "Listing contents" : steps_ == kModSteps ? "Building mod" : "Exporting";
    std::string status;
    ImVec4 status_col = col::muted;
    switch (state) {
    case RunState::Idle: status = "Ready"; break;
    case RunState::Running:
        status = what;
        if (step_ >= 0 && step_ < n) status += std::string("  \xC2\xB7  ") + steps_[step_];
        status_col = col::text;
        break;
    case RunState::Done:
        status_col = col::good;
        if (kind == RunKind::Rip && result.mod) status = result.deployed.empty() ? "Mod built" : "Mod installed into Skate";
        else if (kind == RunKind::Rip && !result.output.empty()) status = "Exported " + u8(result.output.filename());
        else if (kind == RunKind::Load) status = "Scene loaded";
        else status = "Done";
        break;
    case RunState::Failed:
        status_col = col::bad;
        status = std::string(kind == RunKind::Load ? "Loading failed" : "Failed") + ": " + error.substr(0, error.find('\n'));
        break;
    }
    // right: time and percentage
    char right_text[64] = "";
    float fraction = 0;
    if (state != RunState::Idle) {
        if (n > 0) fraction = state == RunState::Done ? 1.f : (std::min(step_, n) + std::clamp(sub_, 0.f, 0.999f)) / n;
        else fraction = state == RunState::Running ? 0 : 1.f;
        if (running_now && n > 0) std::snprintf(right_text, sizeof right_text, "%d%%   %.1f s", (int)(fraction * 100), seconds);
        else std::snprintf(right_text, sizeof right_text, "%.1f s", seconds);
    }
    float right_w = ImGui::CalcTextSize("100%   000.0 s").x;
    float chevron_w = S(30);
    float right_x = o.x + w - pad - chevron_w - right_w;

    // the status text gets up to 40% of what is left, the bar the rest
    float avail = right_x - S(12) - x;
    float status_w = std::min(ImGui::CalcTextSize(status.c_str()).x, avail * 0.42f);
    if (running_now) {  // spinner
        float t = (float)ImGui::GetTime() * 6.0f;
        dl->PathArcTo(ImVec2(x + S(6), cy), S(6), t, t + 4.4f, 16);
        dl->PathStroke(ImGui::GetColorU32(col::accent), 0, S(2));
        x += S(20);
        status_w = std::min(status_w, avail * 0.42f - S(20));
    } else if (state != RunState::Idle) {
        dl->AddCircleFilled(ImVec2(x + S(4), cy), S(4), ImGui::GetColorU32(status_col), 12);
        x += S(16);
    }
    std::string shown = fit_text(status, std::max(S(40), status_w));
    ImVec2 ss = ImGui::CalcTextSize(shown.c_str());
    dl->AddText(ImVec2(x, cy - ss.y * 0.5f), ImGui::GetColorU32(status_col), shown.c_str());
    if (shown != status) {
        ImGui::SetCursorScreenPos(ImVec2(x, o.y));
        ImGui::InvisibleButton("##status", ImVec2(ss.x, strip));
        ImGui::SetItemTooltip("%s", status.c_str());
    }
    x += ss.x + S(14);

    // The bar, the whole width that is left; the newest message sits on it while running
    float bar_l = x, bar_r = right_x - S(12);
    if (bar_r - bar_l > S(40)) {
        float bh = S(18);
        ImVec2 a(bar_l, cy - bh * 0.5f), b(bar_r, cy + bh * 0.5f);
        dl->AddRectFilled(a, b, ImGui::GetColorU32(col::inset), S(5));
        ImVec4 fill = state == RunState::Failed ? col::bad : state == RunState::Done ? col::good : col::accent;
        if (running_now && n == 0) {  // no steps to count: a sweeping band
            float t = std::fmod((float)ImGui::GetTime() * 0.6f, 1.4f) - 0.4f;
            float l = bar_l + (bar_r - bar_l) * std::max(0.f, t), rr = bar_l + (bar_r - bar_l) * std::min(1.f, t + 0.4f);
            if (rr > l) dl->AddRectFilled(ImVec2(l, a.y), ImVec2(rr, b.y), ImGui::GetColorU32(ImVec4(fill.x, fill.y, fill.z, 0.55f)), S(5));
        } else if (fraction > 0) {
            float fx = bar_l + (bar_r - bar_l) * fraction;
            float alpha = state == RunState::Done ? 0.35f : 0.55f;
            dl->AddRectFilled(a, ImVec2(std::max(fx, a.x + S(10)), b.y), ImGui::GetColorU32(ImVec4(fill.x, fill.y, fill.z, alpha)), S(5));
        }
        dl->AddRect(a, b, ImGui::GetColorU32(col::border), S(5));
        // step ticks
        for (int i = 1; i < n; ++i) {
            float tx = bar_l + (bar_r - bar_l) * i / n;
            dl->AddLine(ImVec2(tx, a.y + S(4)), ImVec2(tx, b.y - S(4)), ImGui::GetColorU32(rgb(255, 255, 255, 0.08f)), S(1));
        }
        std::string msg = running_now ? last_line_ : state == RunState::Idle ? std::string() : std::string();
        if (state == RunState::Done && kind == RunKind::Rip && !result.output.empty())
            msg = result.mod ? u8(result.deployed.empty() ? result.output : result.deployed) : format_size(result.bytes);
        if (!msg.empty()) {
            ImGui::PushFont(fonts_.caption);
            std::string m = fit_text(msg, bar_r - bar_l - S(16));
            ImVec2 ms = ImGui::CalcTextSize(m.c_str());
            dl->AddText(ImVec2(bar_l + S(8), cy - ms.y * 0.5f), ImGui::GetColorU32(rgb(230, 233, 239, 0.85f)), m.c_str());
            ImGui::PopFont();
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##bar", ImVec2(b.x - a.x, b.y - a.y));
        if (ImGui::IsItemClicked()) dock_open_ = true, dock_tab_ = 0;
        if (!msg.empty()) ImGui::SetItemTooltip("%s\n(click for the full log)", msg.c_str());
    }
    if (right_text[0]) {
        ImVec2 rs = ImGui::CalcTextSize(right_text);
        dl->AddText(ImVec2(right_x + right_w - rs.x, cy - rs.y * 0.5f), ImGui::GetColorU32(col::muted), right_text);
    }
    // open / close
    {
        ImVec2 a(o.x + w - pad - chevron_w + S(4), o.y + S(5));
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("##dock_toggle", ImVec2(chevron_w - S(4), strip - S(10)))) dock_open_ = !dock_open_;
        bool hovered = ImGui::IsItemHovered();
        ImGui::SetItemTooltip(dock_open_ ? "Hide the panel" : "Show the panel");
        if (hovered) dl->AddRectFilled(a, ImVec2(a.x + chevron_w - S(4), a.y + strip - S(10)), ImGui::GetColorU32(rgb(255, 255, 255, 0.05f)), S(5));
        ImVec2 c(a.x + (chevron_w - S(4)) * 0.5f, cy);
        float k = S(4);
        ImU32 cc = ImGui::GetColorU32(hovered ? col::text : col::muted);
        if (dock_open_) dl->AddTriangleFilled(ImVec2(c.x - k, c.y - k * 0.5f), ImVec2(c.x + k, c.y - k * 0.5f), ImVec2(c.x, c.y + k * 0.6f), cc);
        else dl->AddTriangleFilled(ImVec2(c.x - k, c.y + k * 0.5f), ImVec2(c.x + k, c.y + k * 0.5f), ImVec2(c.x, c.y - k * 0.6f), cc);
    }

    // The panel
    if (dock_open_) {
        dl->AddLine(ImVec2(o.x, o.y + strip), ImVec2(o.x + w, o.y + strip), ImGui::GetColorU32(col::border), S(1));
        ImGui::SetCursorScreenPos(ImVec2(o.x + pad, o.y + strip + S(10)));
        ImVec2 size(w - pad * 2, dock_h_ - S(20));
        if (dock_tab_ == 0) output_tab(size);
    }
    ImGui::End();
}

// The Output tab: the steps and the result on the left, the log on the right.
void App::output_tab(ImVec2 size) {
    Runner& r = runner();
    RunState state = r.state;
    JobResult result;
    std::string error;
    double seconds = 0;
    RunKind kind;
    {
        std::lock_guard lock(r.mutex);
        result = r.result, error = r.error, seconds = r.seconds, kind = r.kind;
    }
    bool list = kind == RunKind::List || kind == RunKind::Scan;
    ImVec2 o = ImGui::GetCursorScreenPos();
    float left_w = std::min(S(270), size.x * 0.35f);

    // Left: steps, result, buttons
    ImGui::BeginChild("output_left", ImVec2(left_w, size.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        int n = (int)steps_.size();
        int current = state == RunState::Idle ? -1 : (state == RunState::Done ? n : std::min(step_, n));
        if (n > 0) {
            float row = S(22);
            ImVec2 p = ImGui::GetCursorScreenPos();
            for (int i = 0; i < n; ++i) {
                ImVec2 c(p.x + S(6), p.y + row * i + row * 0.5f);
                ImVec4 dot = col::switch_off;
                if (current > i) dot = col::good;
                else if (current == i) dot = state == RunState::Failed ? col::bad : col::accent;
                if (i + 1 < n)
                    dl->AddLine(ImVec2(c.x, c.y + S(6)), ImVec2(c.x, c.y + row - S(6)),
                                ImGui::GetColorU32(current > i ? col::good : col::border), S(2));
                if (current == i && state == RunState::Running) {
                    float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 5.f);
                    dl->AddCircleFilled(c, S(5) + S(3) * pulse, ImGui::GetColorU32(ImVec4(dot.x, dot.y, dot.z, 0.25f)), 20);
                }
                dl->AddCircleFilled(c, S(5), ImGui::GetColorU32(dot), 20);
                ImVec2 ts = ImGui::CalcTextSize(steps_[i]);
                dl->AddText(ImVec2(c.x + S(14), c.y - ts.y * 0.5f), ImGui::GetColorU32(current >= i ? col::text : col::muted), steps_[i]);
                if (current == i && state == RunState::Running && sub_ > 0) {
                    char pct[16];
                    std::snprintf(pct, sizeof pct, "%d%%", (int)(sub_ * 100));
                    ImVec2 ps = ImGui::CalcTextSize(pct);
                    dl->AddText(ImVec2(p.x + left_w - ps.x - S(10), c.y - ps.y * 0.5f), ImGui::GetColorU32(col::muted), pct);
                }
            }
            ImGui::Dummy(ImVec2(left_w, row * n + S(6)));
        }
        bool show_result = state == RunState::Done && !list && kind == RunKind::Rip && !result.output.empty();
        if (show_result) {
            ImGui::PushFont(fonts_.semibold);
            if (result.mod) ImGui::TextColored(col::good, result.deployed.empty() ? "Mod built" : "Mod installed into Skate");
            else ImGui::TextColored(col::good, "Saved %s", fit_text(u8(result.output.filename()), left_w - S(60)).c_str());
            ImGui::PopFont();
            if (result.mod)
                ImGui::TextColored(col::muted, "%s  \xC2\xB7  %.1f s", result.deployed.empty() ? "package ready" : "start Skate to play it", seconds);
            else
                ImGui::TextColored(col::muted, "%s  \xC2\xB7  %.1f s", format_size(result.bytes).c_str(), seconds);
            fs::path where = result.mod && !result.deployed.empty() ? result.deployed : result.output;
            if (accent_button("Show in Explorer", ImVec2(0, 0))) reveal(where);
            if (!result.mod) {
                ImGui::SameLine();
                if (ImGui::Button("Open")) ShellExecuteW(window_, L"open", result.output.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                ImGui::SetItemTooltip("Open the .glb in its default app");
            }
        } else if (state == RunState::Failed) {
            ImGui::PushFont(fonts_.semibold);
            ImGui::TextColored(col::bad, kind == RunKind::Load ? "Loading failed" : result.mod || steps_ == kModSteps ? "The mod build failed" : "The rip failed");
            ImGui::PopFont();
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(col::muted, "%s", error.c_str());
            ImGui::PopTextWrapPos();
        } else if (state == RunState::Idle) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(col::muted, "Open a map, edit it in the scene, then press Export.");
            ImGui::PopTextWrapPos();
        }
        ImGui::BeginDisabled(log_.empty());
        if (ImGui::Button("Copy log")) {
            std::string all;
            for (auto& l : log_) all += l + "\n";
            ImGui::SetClipboardText(all.c_str());
            notify("Log copied");
        }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();

    // Right: the log
    ImGui::SetCursorScreenPos(ImVec2(o.x + left_w + S(12), o.y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::inset);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(8));
    // Long lines wrap; a very long (verbose) log switches to one row per line so it stays fast.
    bool wrap = log_.size() <= 2000;
    ImGui::BeginChild("log", ImVec2(size.x - left_w - S(12), size.y), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders,
                      wrap ? ImGuiWindowFlags_None : ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (log_.empty()) {
        const char* hint = state == RunState::Running ? "Starting\xE2\x80\xA6" : "Messages from loading, exporting and building show here.";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        ImVec2 region = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(std::max(S(12), (region.x - ts.x) * 0.5f), region.y * 0.45f));
        ImGui::TextColored(col::muted, "%s", hint);
    } else {
        bool at_end = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - S(4);
        ImGui::PushFont(fonts_.mono);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(3)));
        auto draw_line = [&](const std::string& l) {
            ImVec4 c = col::text;
            std::string t = plain_line(l);
            if (l.rfind("error", 0) == 0) c = col::bad;
            else if (l.rfind("warning", 0) == 0 || t.rfind("warning", 0) == 0) c = col::warn;
            else if (l.rfind("done:", 0) == 0) c = col::good;
            else if (!l.empty() && l[0] == ' ') c = col::muted;
            ImGui::PushStyleColor(ImGuiCol_Text, c);
            ImGui::TextUnformatted(l.c_str());
            ImGui::PopStyleColor();
        };
        if (wrap) {
            ImGui::PushTextWrapPos(0.0f);
            for (auto& l : log_) draw_line(l);
            ImGui::PopTextWrapPos();
        } else {
            ImGuiListClipper clipper;
            clipper.Begin((int)log_.size());
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) draw_line(log_[i]);
        }
        ImGui::PopStyleVar();
        ImGui::PopFont();
        if (scroll_to_end_ && at_end) ImGui::SetScrollHereY(1.0f);
        scroll_to_end_ = false;
    }
    ImGui::EndChild();
}

} // namespace

int run_gui(const Job& initial, bool use_saved) {
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    dpi_scale() = std::max(1.0f, GetDpiForSystem() / 96.0f);

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));  // IDI_APPLICATION
    wc.hbrBackground = CreateSolidBrush(RGB(16, 18, 23));
    wc.lpszClassName = L"SpotbuilderWindow";
    RegisterClassExW(&wc);

    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = std::min((int)S(1240), (int)(work.right - work.left) - 40);
    int h = std::min((int)S(860), (int)(work.bottom - work.top) - 40);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"Spotbuilder", WS_OVERLAPPEDWINDOW,
                                  work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w,
                                  h, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    dpi_scale() = std::max(1.0f, GetDpiForWindow(window) / 96.0f);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = RGB(16, 18, 23);
    DwmSetWindowAttribute(window, 35 /* DWMWA_CAPTION_COLOR (Windows 11) */, &caption, sizeof(caption));

    if (!create_device(window)) {
        MessageBoxW(nullptr, L"Direct3D 11 is not available, so the window cannot open.\n\nThe command line still works: "
                             L"Spotbuilder <map> [options] (see --help).",
                    L"Spotbuilder", MB_OK | MB_ICONERROR);
        DestroyWindow(window);
        return 1;
    }
    DragAcceptFiles(window, TRUE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    Fonts fonts = load_fonts();
    apply_style();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_context);

    Job job = initial;
    if (use_saved) load_settings(job);
    auto scene = std::make_unique<SceneView>(g_device, g_context, fonts);
    App app(window, fonts, job, *scene);

    // Test runs (SPOTBUILDER_SNAPSHOT, below) open off-screen and unfocused, out of the user's way.
    if (GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT", nullptr, 0)) {
        SetWindowPos(window, nullptr, -20000, -20000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(window, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(window, SW_SHOWNORMAL);
    }
    UpdateWindow(window);

    // Test hook: SPOTBUILDER_SNAPSHOT=<file.png>[|<seconds>] saves one frame as a PNG after that
    // long (default 1 s) and closes the window; SPOTBUILDER_SNAPSHOT_RIP=1 presses Rip map first,
    // SPOTBUILDER_SNAPSHOT_SCENE=1 opens the Scene page and loads the map.
    fs::path snapshot;
    double snapshot_after = 1;
    {
        wchar_t buf[2048] = {};
        if (GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT", buf, 2048)) {
            std::wstring v = buf;
            size_t bar = v.find(L'|');
            snapshot = v.substr(0, bar);
            if (bar != std::wstring::npos) snapshot_after = _wtof(v.c_str() + bar + 1);
            if (GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT_RIP", buf, 2048)) app.press_rip();
            if (GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT_SCENE", buf, 2048)) app.open_scene();
            if (GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT_SCAN", buf, 2048)) app.scan();
            if (GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT_SETTINGS", buf, 2048)) app.open_settings();
            if (GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT_DOCK", buf, 2048)) app.open_dock();
            if (GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT_EXPORT", buf, 2048)) app.open_export(buf[0] == L'm');
        }
    }
    // Test hook: SPOTBUILDER_SNAPSHOT_PRESS=<letters> presses Ctrl+<letter> for each, a few frames
    // apart, once the scene has loaded (e.g. "cvvzzy": copy, paste twice, undo twice, redo).
    std::string test_keys;
    size_t test_key_at = 0;
    int test_key_frame = 0;
    {
        wchar_t buf[256] = {};
        if (GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT_PRESS", buf, 256)) test_keys = narrow(buf);
    }
    auto opened = std::chrono::steady_clock::now();

    bool running = true;
    while (running) {
        // Idle in the background: wake for input, or now and then for a running rip's log.
        if (GetForegroundWindow() != window && runner().state != RunState::Running)
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (IsIconic(window)) {
            Sleep(50);
            continue;
        }
        if (g_resize_w && g_resize_h) {
            release_target();
            g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            create_target();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        if (test_key_at < test_keys.size() && app.scene_ready() && ++test_key_frame > 20) {
            ImGuiIO& io = ImGui::GetIO();
            char c = (char)std::tolower((unsigned char)test_keys[test_key_at]);
            ImGuiKey key = c >= 'a' && c <= 'z' ? (ImGuiKey)(ImGuiKey_A + (c - 'a')) : ImGuiKey_None;
            int phase = (test_key_frame - 21) % 8;
            if (phase == 0) io.AddKeyEvent(ImGuiMod_Ctrl, true), io.AddKeyEvent(ImGuiKey_LeftCtrl, true), io.AddKeyEvent(key, true);
            if (phase == 2) io.AddKeyEvent(key, false), io.AddKeyEvent(ImGuiKey_LeftCtrl, false), io.AddKeyEvent(ImGuiMod_Ctrl, false);
            if (phase == 7) ++test_key_at;
        }
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        const float clear[4] = {col::bg.x, col::bg.y, col::bg.z, 1};
        g_context->OMSetRenderTargets(1, &g_target, nullptr);
        g_context->ClearRenderTargetView(g_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (!snapshot.empty() &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - opened).count() >= snapshot_after) {
            save_frame(snapshot);
            snapshot.clear();
            DestroyWindow(window);
        }
        g_swap->Present(1, 0);
    }

    save_settings(app.job());
    app.flush_edits();
    bool still_running = runner().state == RunState::Running;
    scene.reset();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_device();
    if (IsWindow(window)) DestroyWindow(window);
    if (com) CoUninitialize();
    if (still_running) TerminateProcess(GetCurrentProcess(), 0);  // the user chose to stop the rip
    return 0;
}

namespace ui {

bool accent_button(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, col::accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col::accent_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col::accent_active);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

bool switch_row(const char* label, bool& value) {
    float w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetFrameHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    ImGuiID id = ImGui::GetID("switch");
    bool pressed = ImGui::InvisibleButton("row", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (pressed) value = !value;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered) dl->AddRectFilled(ImVec2(p.x - S(6), p.y), ImVec2(p.x + w + S(6), p.y + h), ImGui::GetColorU32(rgb(255, 255, 255, 0.035f)), S(6));
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x, p.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label);

    float* t = ImGui::GetStateStorage()->GetFloatRef(id, value ? 1.f : 0.f);
    float target = value ? 1.f : 0.f;
    *t += (target - *t) * std::min(1.f, ImGui::GetIO().DeltaTime * 14.f);
    if (std::fabs(*t - target) < 0.01f) *t = target;
    float th = S(20), tw = S(36);
    ImVec2 a(p.x + w - tw, p.y + (h - th) * 0.5f), b(a.x + tw, a.y + th);
    ImVec4 track = mix(col::switch_off, col::accent, *t);
    dl->AddRectFilled(a, b, ImGui::GetColorU32(track), th * 0.5f);
    float r = th * 0.5f - S(3);
    ImVec2 knob(a.x + th * 0.5f + (tw - th) * *t, a.y + th * 0.5f);
    dl->AddCircleFilled(knob, r, ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), 24);
    return pressed;
}

bool chip(const char* label, bool& value, const char* tooltip) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(20));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), S(5)));
    ImGui::PushStyleColor(ImGuiCol_Button, value ? rgb(96, 142, 255, 0.22f) : col::field);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, value ? rgb(96, 142, 255, 0.32f) : col::field_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, value ? rgb(96, 142, 255, 0.42f) : col::field_active);
    ImGui::PushStyleColor(ImGuiCol_Text, value ? col::accent_hover : col::muted);
    bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    if (pressed) value = !value;
    if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
    return pressed;
}

} // namespace ui

} // namespace xl
