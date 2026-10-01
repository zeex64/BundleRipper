#include "studio_build.h"
#include "job.h"
#include "json_parse.h"
#include "log.h"

#include <Windows.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace xl {

namespace {

fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}
bool is_dir(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_directory(p, ec);
}

// ReSkate Studio's settings: BlenderExecutable and GameDirectory.
Json studio_settings() {
    fs::path file = known_folder(FOLDERID_LocalAppData) / L"ReSkateStudio" / L"settings.json";
    std::ifstream f(file, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    Json j;
    std::string error;
    return parse_json(ss.str(), j, error) ? j : Json{};
}
std::string setting(const Json& j, const char* key) {
    if (j.t != Json::Obj) return {};
    for (auto& [k, v] : j.o)
        if (k == key && v.t == Json::Str) return v.s;
    return {};
}

// The folder of a running "ReSkate Studio.exe", if any.
fs::path running_studio() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W e{sizeof(e)};
    fs::path found;
    for (BOOL ok = Process32FirstW(snap, &e); ok && found.empty(); ok = Process32NextW(snap, &e)) {
        if (_wcsicmp(e.szExeFile, L"ReSkate Studio.exe") != 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, path, &n)) found = fs::path(path).parent_path();
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}

std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (wchar_t c : a) {
        if (c == L'"') q += L'\\';
        q += c;
    }
    return q + L"\"";
}

// make_blend.py, built into the exe (resource 2, RCDATA).
std::string blender_script() {
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(self, MAKEINTRESOURCEW(2), MAKEINTRESOURCEW(10));
    if (!r) return {};
    HGLOBAL g = LoadResource(self, r);
    const char* data = g ? (const char*)LockResource(g) : nullptr;
    return data ? std::string(data, SizeofResource(self, r)) : std::string();
}

Json point(const V3& unity) {
    V3 b = unity_to_blender(unity);
    return Json::list(b.x, b.y, b.z);
}

} // namespace

fs::path build_folder(const std::string& name) { return known_folder(FOLDERID_LocalAppData) / L"Spotbuilder" / L"Builds" / widen(name); }

StudioTools find_studio_tools(const ModBuild& m) {
    StudioTools t;
    Json studio = studio_settings();
    // Blender
    if (is_file(m.blender)) t.blender = m.blender;
    else if (fs::path s = widen(setting(studio, "BlenderExecutable")); is_file(s)) t.blender = s;
    else {
        fs::path root = known_folder(FOLDERID_ProgramFiles) / L"Blender Foundation";
        std::error_code ec;
        std::vector<fs::path> found;
        if (is_dir(root))
            for (auto& d : fs::directory_iterator(root, ec))
                if (is_file(d.path() / L"blender.exe")) found.push_back(d.path() / L"blender.exe");
        std::sort(found.begin(), found.end());
        if (!found.empty()) t.blender = found.back();  // the newest version
    }
    // Studio's command line
    std::vector<fs::path> clis = {m.studio_cli};
    if (fs::path s = running_studio(); !s.empty()) clis.push_back(s / L"reskate_cli.exe");
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    clis.push_back(fs::path(self).parent_path() / L"reskate_cli.exe");
    clis.push_back(known_folder(FOLDERID_LocalAppData) / L"Programs" / L"ReSkate Studio" / L"reskate_cli.exe");
    clis.push_back(known_folder(FOLDERID_ProgramFiles) / L"ReSkate Studio" / L"reskate_cli.exe");
    clis.push_back(known_folder(FOLDERID_Documents) / L"GitHub" / L"ReSkateStudio" / L"out" / L"build" / L"windows-x64" /
                   L"bin" / L"Release" / L"reskate_cli.exe");
    for (auto& c : clis)
        if (is_file(c)) {
            t.cli = c;
            break;
        }
    // Skate
    if (is_dir(m.game)) t.game = m.game;
    else if (fs::path s = widen(setting(studio, "GameDirectory")); is_dir(s)) t.game = s;
    else if (fs::path s = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Skate"; is_dir(s)) t.game = s;

    std::vector<std::string> missing;
    if (t.blender.empty()) missing.push_back("Blender (set it under Skate mod)");
    if (t.cli.empty()) missing.push_back("ReSkate Studio's reskate_cli.exe (set it under Skate mod)");
    if (t.game.empty()) missing.push_back("Skate's game folder (set it under Skate mod)");
    for (size_t i = 0; i < missing.size(); ++i) t.missing += (i ? ", " : "not found: ") + missing[i];
    return t;
}

std::vector<ExportCurve> export_curves(const Scene& sc, const Edits* edits) {
    std::vector<ExportCurve> out;
    auto ripped = [&](const std::vector<OutCurve>& list, bool automatic) {
        for (auto& c : list) {
            ExportCurve e;
            e.curve = c;
            e.automatic = automatic;
            if (edits) {
                auto it = edits->ripped_splines.find(c.key);
                if (it != edits->ripped_splines.end()) {
                    e.grind = it->second.grind;
                    const V3& d = it->second.offset;
                    for (auto* list : {&e.curve.points, &e.curve.left, &e.curve.right})
                        for (V3& p : *list) p = p + d;
                }
            }
            out.push_back(std::move(e));
        }
    };
    ripped(sc.curves, false);
    ripped(sc.auto_curves, true);
    if (edits)
        for (size_t k = 0; k < edits->splines.size(); ++k) {
            const UserSpline& s = edits->splines[k];
            if (s.points.size() < 2) continue;
            ExportCurve e;
            e.curve.name = s.name.empty() ? (s.npc ? "NPC route " : "Grind curve ") + std::to_string(k + 1) : s.name;
            e.curve.closed = s.closed;
            e.curve.points = s.points;
            e.npc = s.npc;
            e.grind = s.grind;
            e.route = s.route;
            out.push_back(std::move(e));
        }
    return out;
}

void write_curves_json(const std::vector<ExportCurve>& curves, const fs::path& file) {
    static const char* const kinds[] = {"pedestrian", "vehicle", "bus"};
    Json list = Json::array();
    for (auto& e : curves) {
        const OutCurve& c = e.curve;
        if (c.points.size() < 2) continue;
        Json j = Json::object();
        j.set("name", c.name);
        j.set("kind", e.npc ? "npc" : "grind");
        j.set("closed", c.closed);
        Json pts = Json::array();
        for (auto& p : c.points) pts.push(point(p));
        j.set("points", pts);
        if (c.bezier && c.left.size() == c.points.size() && c.right.size() == c.points.size()) {
            j.set("bezier", true);
            Json l = Json::array(), r = Json::array();
            for (size_t i = 0; i < c.points.size(); ++i) l.push(point(c.left[i])), r.push(point(c.right[i]));
            j.set("left", l);
            j.set("right", r);
        }
        if (e.npc) {
            Json n = Json::object();
            n.set("kind", kinds[(int)e.route.kind]);
            n.set("width", e.route.width);
            n.set("spacing", e.route.spacing);
            n.set("weight", e.route.weight);
            n.set("speed", e.route.speed);
            n.set("bidirectional", e.route.bidirectional);
            n.set("stairs", e.route.stairs);
            j.set("npc", n);
        } else {
            Json g = Json::object();
            g.set("enabled", e.grind.enabled);
            g.set("radius", e.grind.radius);
            g.set("surface", kGrindSurfaceIds[std::clamp(e.grind.surface, 0, kGrindSurfaceCount - 1)]);
            j.set("grind", g);
        }
        list.push(j);
    }
    Json root = Json::object();
    root.set("curves", list);
    std::ofstream f(file, std::ios::binary);
    std::string s;
    root.dump(s);
    f << s;
}

int run_process(const fs::path& exe, const std::vector<std::wstring>& args, const std::function<void(const std::string&)>& line) {
    std::wstring cmd = quote(exe.wstring());
    for (auto& a : args) cmd += L" " + quote(a);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) throw std::runtime_error("cannot start " + u8(exe));
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = write;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             exe.parent_path().c_str(), &si, &pi);
    CloseHandle(write);
    if (!ok) {
        CloseHandle(read);
        throw std::runtime_error("cannot start " + u8(exe));
    }
    std::string pending;
    char chunk[4096];
    DWORD got = 0;
    while (ReadFile(read, chunk, sizeof chunk, &got, nullptr) && got) {
        pending.append(chunk, got);
        for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
            std::string l = pending.substr(0, nl);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            line(l);
            pending.erase(0, nl + 1);
        }
    }
    if (!pending.empty()) line(pending);
    CloseHandle(read);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

bool skate_running() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{sizeof(e)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
        found = _wcsicmp(e.szExeFile, L"Skate.exe") == 0;
    CloseHandle(snap);
    return found;
}

void make_blend(const StudioTools& tools, const fs::path& glb, const fs::path& curves, const fs::path& blend) {
    std::string script = blender_script();
    if (script.empty()) throw std::runtime_error("the Blender script is missing from this build");
    fs::path script_file = blend.parent_path() / L"make_blend.py";
    {
        std::ofstream f(script_file, std::ios::binary);
        f << script;
    }
    std::error_code ec;
    fs::remove(blend, ec);
    std::vector<std::string> tail;  // the last lines, for an error message
    int code = run_process(tools.blender,
                           {L"--background", L"--factory-startup", L"--python-exit-code", L"1", L"--python",
                            script_file.wstring(), L"--", glb.wstring(), curves.wstring(), blend.wstring()},
                           [&](const std::string& l) {
                               if (l.rfind("SB_PROGRESS\t", 0) == 0) {
                                   size_t tab = l.find('\t', 12);
                                   log_info("  blender: %s", tab == std::string::npos ? l.c_str() + 12 : l.c_str() + tab + 1);
                               } else if (l.find("Studio add-on") != std::string::npos || l.find("rror") != std::string::npos ||
                                          l.find("Traceback") != std::string::npos || l.find("could not set") != std::string::npos) {
                                   log_info("  blender: %s", l.c_str());
                               } else {
                                   log_verbose("blender: %s", l.c_str());
                               }
                               tail.push_back(l);
                               if (tail.size() > 6) tail.erase(tail.begin());
                           });
    if (code != 0 || !fs::exists(blend, ec)) {
        std::string why;
        for (auto& l : tail) why += "\n  " + l;
        throw std::runtime_error("Blender could not make the .blend (exit code " + std::to_string(code) + ")" + why);
    }
}

void compile_mod(const StudioTools& tools, const ModBuild& m, const std::string& name, const fs::path& blend,
                 const fs::path& package) {
    static const char* const times[] = {"", "morning", "noon", "afternoon", "evening", "night"};
    std::vector<std::wstring> args = {L"compile-map", tools.game.wstring(), blend.wstring(), package.wstring()};
    if (m.deploy) args.push_back(L"--deploy");
    args.push_back(L"--mod-folder");
    args.push_back(widen(name));
    args.push_back(L"--blender");
    args.push_back(tools.blender.wstring());
    args.push_back(L"--pause-map");
    args.push_back(m.pause_map == 1 ? L"2d" : L"3d");
    if (m.time_of_day > 0 && m.time_of_day < 6) {
        args.push_back(L"--time-of-day");
        args.push_back(widen(times[m.time_of_day]));
    }
    if (!m.gi) args.push_back(L"--no-gi");
    if (m.streaming == 1) args.push_back(L"--stream");
    if (m.streaming == 2) args.push_back(L"--no-stream");
    if (!m.lods) args.push_back(L"--no-lods");
    std::vector<std::string> tail;
    // Studio prints a line per bundle, tile and statistic; a run of lines that differ only in
    // numbers and paths shows its first, the rest (and the key=value statistics) go to the
    // detailed log.
    std::string last_kind;
    int code = run_process(tools.cli, args, [&](const std::string& l) {
        if (l.empty()) return;
        std::string kind;
        bool word_path = false;
        for (size_t i = 0; i <= l.size(); ++i) {
            char c = i < l.size() ? l[i] : ' ';
            if (c == ' ') {
                if (word_path) kind += '*';
                word_path = false;
                kind += ' ';
            } else if (c == '/' || c == '\\') {
                word_path = true;
            } else if (!word_path && !std::isdigit((unsigned char)c) && c != '.') {
                kind += c;
            }
        }
        size_t eq = l.find('=');
        bool statistic = eq != std::string::npos && l.find(' ') > eq && l.rfind("deployed=", 0) != 0;
        if (statistic || kind == last_kind) log_verbose("studio: %s", l.c_str());
        else log_info("  studio: %s", l.c_str());
        last_kind = kind;
        tail.push_back(l);
        if (tail.size() > 6) tail.erase(tail.begin());
    });
    if (code != 0) {
        std::string why;
        for (auto& l : tail) why += "\n  " + l;
        throw std::runtime_error("ReSkate Studio could not build the mod (exit code " + std::to_string(code) + ")" + why);
    }
}

} // namespace xl
