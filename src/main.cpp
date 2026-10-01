// Spotbuilder (was BundleRipper): rips levels out of Unity asset bundles (Skater XL mod maps and other Unity
// games) to a binary glTF.
//
//   Spotbuilder                        opens the window
//   Spotbuilder <map> [options]        command line (dropping a map on the exe does this)
//   Spotbuilder --gui [map] [options]  the window, with these settings filled in
#include "gui.h"
#include "job.h"
#include "log.h"
#include "options.h"

#include <Windows.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace xl;

namespace {

// True when this process has the console to itself: started from Explorer (double-click or a
// drop on the exe) rather than from a terminal.
bool owns_console() {
    DWORD ids[4];
    return GetConsoleProcessList(ids, 4) <= 1;
}

int run_cli(const ParsedArgs& parsed) {
    if (parsed.help) {
        std::fputs(usage_text().c_str(), stdout);
        return 0;
    }
    if (parsed.job.input.empty()) {
        std::fputs(usage_text().c_str(), stdout);
        return 1;
    }
    run_job(parsed.job);
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    bool own = owns_console();

    ParsedArgs parsed;
    std::string error;
    try {
        parsed = parse_args(args);
    } catch (const std::exception& e) {
        error = e.what();
    }

    if (error.empty() && (args.empty() || parsed.gui)) {
        // The window. Double-clicked, the console Windows opened for us is not needed.
        if (own) FreeConsole();
        else std::fputs("Spotbuilder: opened the window (Spotbuilder --help for the command line)\n", stdout);
        return run_gui(parsed.job, args.empty());
    }

    int code = 1;
    if (!error.empty()) {
        std::fprintf(stdout, "error: %s\n", error.c_str());
    } else {
        try {
            code = run_cli(parsed);
        } catch (const std::exception& e) {
            std::fprintf(stdout, "error: %s\n", e.what());
            code = 1;
        }
    }
    if (own) {  // dropped on the exe: keep the window open to read
        std::fputs("\npress Enter to close\n", stdout);
        std::getchar();
    }
    return code;
}
