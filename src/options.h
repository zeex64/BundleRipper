// Every export option, described once. The command-line parser, --help, the GUI's controls
// and tooltips, its remembered settings and "Copy command line" all come from this table, so
// a new option is one entry in options.cpp.
#pragma once
#include "job.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace xl {

struct OptionDef {
    enum Kind { Toggle, Choice, Number, Integer, Folder, File, Text };
    struct Item {
        int value;
        const char* label;          // GUI
        const char* cli = nullptr;  // command-line spelling; null: the number itself
    };

    const char* group = "";  // GUI section
    const char* label = "";  // GUI label
    const char* help = "";   // tooltip and --help text
    Kind kind = Toggle;
    const char* flag = nullptr;      // Toggle: turns it on; others: takes a value
    const char* off_flag = nullptr;  // Toggle: turns it off; Number: sets 0
    const char* off_help = nullptr;  // --help text for off_flag on options that start on
    const char* value_name = "";     // "<n>" in --help
    std::vector<Item> items;         // Choice
    double min = 0, max = 0;         // Number / Integer range in the GUI
    const char* unit = "";           // Number / Integer: shown after the value
    // Toggle: 0/1, Choice: the item value, Number/Integer: the value.
    std::function<double(const Job&)> get;
    std::function<void(Job&, double)> set;
    std::function<std::filesystem::path&(Job&)> path;  // Folder, File
    std::function<std::string&(Job&)> text;            // Text
    const char* placeholder = "";                       // Folder, File, Text: shown while empty
    const wchar_t* filter = nullptr;                    // File: the picker's file type, e.g. L"blender.exe"
    bool is_path() const { return kind == Folder || kind == File; }
    bool takes_string() const { return kind == Folder || kind == File || kind == Text; }
    std::function<bool(const Job&)> enabled;          // GUI: greyed out when false (null: always)
    // GUI: 0 shown under Settings (every export); 1 only for a .glb, 2 only for a Skate mod (both
    // in the Export window); -1 not shown (the Export window's own choice).
    int format = 0;
};

const std::vector<OptionDef>& option_table();

struct ParsedArgs {
    Job job;
    bool help = false;
    bool gui = false;
};
// Command-line arguments without the program name; throws std::runtime_error on bad ones.
ParsedArgs parse_args(const std::vector<std::wstring>& args);
// The arguments that reproduce `job`: the map, -o when set, then every option that differs
// from its default.
std::vector<std::wstring> job_args(const Job& job);
// Arguments joined into one command line, quoted where needed.
std::wstring join_args(const std::vector<std::wstring>& args);
std::string usage_text();

} // namespace xl
