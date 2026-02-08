#include "ipc2581_parser.h"
#include "kicad_writer.h"
#include "json_export.h"
#include "json_import.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <array>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#include <io.h>
#else
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>
#endif

static void print_help() {
    std::cout << "Usage: ipc2581-to-kicad [options] <input>\n"
              << "\n"
              << "Convert IPC-2581 or ODB++ files to KiCad .kicad_pcb format.\n"
              << "\n"
              << "Supported input formats:\n"
              << "  .xml, .cvg          IPC-2581 XML files\n"
              << "  .tgz, .tar.gz, .zip ODB++ archives (requires Python 3)\n"
              << "  directory/          ODB++ extracted directory (requires Python 3)\n"
              << "  .json               JSON (with --import-json)\n"
              << "\n"
              << "Options:\n"
              << "  -o, --output <file>       Output .kicad_pcb file (default: <input>.kicad_pcb)\n"
              << "  -v, --version <7|8|9>     Target KiCad version (default: 9)\n"
              << "  -s, --step <name>         Step name to convert (default: first step)\n"
              << "  --list-steps              List available steps and exit\n"
              << "  --list-layers             List layer mapping and exit\n"
              << "  --export-json             Export parsed PCB data as JSON to stdout\n"
              << "  --import-json             Import from JSON file (output of --export-json)\n"
              << "  --verbose                 Verbose output during conversion\n"
              << "  -h, --help                Show help\n";
}

static std::string replace_extension(const std::string& path, const std::string& new_ext) {
    auto dot = path.rfind('.');
    if (dot != std::string::npos) {
        // Handle .tar.gz specially
        if (dot > 4 && path.substr(dot - 4, 4) == ".tar") {
            return path.substr(0, dot - 4) + new_ext;
        }
        return path.substr(0, dot) + new_ext;
    }
    return path + new_ext;
}

// Detect input format from file path
enum class InputFormat { IPC2581, ODB, JSON, UNKNOWN };

static bool is_directory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool has_odb_matrix(const std::string& dir_path) {
    // Check if directory contains matrix/matrix (sign of ODB++ directory)
    std::string matrix_path = dir_path + "/matrix/matrix";
    struct stat st;
    return stat(matrix_path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static InputFormat detect_format(const std::string& path, bool import_json) {
    if (import_json) return InputFormat::JSON;

    // Check if it's a directory with ODB++ structure
    if (is_directory(path)) {
        if (has_odb_matrix(path)) return InputFormat::ODB;
        // Also check one level down
        // (ODB++ archives sometimes have a subdirectory)
        return InputFormat::ODB; // Assume ODB++ if it's a directory
    }

    // Check file extension
    std::string lower_path = path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);

    if (lower_path.size() >= 4 && lower_path.substr(lower_path.size() - 4) == ".tgz")
        return InputFormat::ODB;
    if (lower_path.size() >= 7 && lower_path.substr(lower_path.size() - 7) == ".tar.gz")
        return InputFormat::ODB;
    if (lower_path.size() >= 4 && lower_path.substr(lower_path.size() - 4) == ".zip")
        return InputFormat::ODB;
    if (lower_path.size() >= 5 && lower_path.substr(lower_path.size() - 5) == ".json")
        return InputFormat::JSON;
    if (lower_path.size() >= 4 && lower_path.substr(lower_path.size() - 4) == ".xml")
        return InputFormat::IPC2581;
    if (lower_path.size() >= 4 && lower_path.substr(lower_path.size() - 4) == ".cvg")
        return InputFormat::IPC2581;

    return InputFormat::UNKNOWN;
}

// Find the odb/odb_to_json.py script relative to the executable
static std::string find_odb_script(const std::string& argv0) {
    // Try several locations relative to the executable
    std::vector<std::string> candidates;

#ifndef _WIN32
    // Get directory of executable via /proc/self/exe or argv[0]
    std::string exe_dir;
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        exe_dir = std::string(buf);
        auto slash = exe_dir.rfind('/');
        if (slash != std::string::npos) exe_dir = exe_dir.substr(0, slash);
    }
    if (exe_dir.empty()) {
        // Fallback: derive from argv[0]
        std::string a0 = argv0;
        auto slash = a0.rfind('/');
        if (slash != std::string::npos) {
            exe_dir = a0.substr(0, slash);
        } else {
            exe_dir = ".";
        }
    }

    // Typical locations:
    //   build/ipc2581-to-kicad -> ../odb/odb_to_json.py
    //   ./ipc2581-to-kicad    -> odb/odb_to_json.py
    //   /usr/bin/              -> ../share/ipc2581-to-kicad/odb/odb_to_json.py
    candidates.push_back(exe_dir + "/../odb/odb_to_json.py");
    candidates.push_back(exe_dir + "/odb/odb_to_json.py");
    candidates.push_back(exe_dir + "/../share/ipc2581-to-kicad/odb/odb_to_json.py");
    candidates.push_back("odb/odb_to_json.py");
#else
    candidates.push_back("odb\\odb_to_json.py");
    candidates.push_back("..\\odb\\odb_to_json.py");
#endif

    for (auto& c : candidates) {
        struct stat st;
        if (stat(c.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            return c;
        }
    }
    return "";
}

// Find Python3 interpreter
static std::string find_python3() {
    // Try common names
    const char* names[] = {"python3", "python"};
    for (auto name : names) {
        std::string cmd = std::string("which ") + name + " 2>/dev/null";
        FILE* fp = popen(cmd.c_str(), "r");
        if (fp) {
            char buf[256];
            if (fgets(buf, sizeof(buf), fp)) {
                pclose(fp);
                std::string path(buf);
                // Trim newline
                while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
                    path.pop_back();
                if (!path.empty()) return path;
            } else {
                pclose(fp);
            }
        }
    }
    return "";
}

// Run the ODB++ Python parser and capture its JSON output
static bool run_odb_parser(const std::string& argv0, const std::string& input_file,
                           const std::string& step_name, bool verbose,
                           bool list_steps_mode,
                           ipc2kicad::PcbModel& model) {
    std::string python = find_python3();
    if (python.empty()) {
        std::cerr << "Error: Python 3 not found. ODB++ support requires Python 3.\n";
        return false;
    }

    std::string script = find_odb_script(argv0);
    if (script.empty()) {
        std::cerr << "Error: odb/odb_to_json.py not found.\n"
                  << "  Looked relative to executable and current directory.\n";
        return false;
    }

    // Find project root (parent directory of odb/) from the script path
    std::string project_root;
    {
        std::string script_dir = script;
        auto slash = script_dir.rfind('/');
        if (slash != std::string::npos) {
            script_dir = script_dir.substr(0, slash);  // strip /odb_to_json.py -> odb/
            slash = script_dir.rfind('/');
            if (slash != std::string::npos) {
                project_root = script_dir.substr(0, slash);  // strip /odb -> project root
            } else {
                project_root = ".";
            }
        } else {
            project_root = ".";
        }
    }

    // Use -m to run as module with PYTHONPATH set to project root
    std::string cmd = "PYTHONPATH=\"" + project_root + "\" " +
                      python + " -m odb.odb_to_json";

    if (verbose)     cmd += " -v";
    if (!step_name.empty()) cmd += " -s \"" + step_name + "\"";
    if (list_steps_mode) cmd += " --list-steps";

    cmd += " \"" + input_file + "\"";
    cmd += " 2>&1";

    if (verbose) {
        std::cerr << "Running: " << cmd << "\n";
    }

    // Run and capture output
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) {
        std::cerr << "Error: failed to run ODB++ parser\n";
        return false;
    }

    std::string output;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) {
        output += buf;
    }

    int status = pclose(fp);
    if (status != 0) {
        std::cerr << "Error: ODB++ parser failed (exit " << WEXITSTATUS(status) << ")\n";
        if (!output.empty()) {
            std::cerr << output << "\n";
        }
        return false;
    }

    if (list_steps_mode) {
        // Just print the output as-is for --list-steps
        std::cout << output;
        return true;
    }

    // Parse JSON output
    // stderr lines from verbose mode are mixed in when using 2>&1
    // The JSON starts with '{' - find it
    auto json_start = output.find('{');
    if (json_start == std::string::npos) {
        std::cerr << "Error: no JSON output from ODB++ parser\n";
        if (verbose && !output.empty()) {
            std::cerr << "Parser output:\n" << output << "\n";
        }
        return false;
    }

    // Print any stderr content before the JSON
    if (verbose && json_start > 0) {
        std::cerr << output.substr(0, json_start);
    }

    std::string json_str = output.substr(json_start);
    std::istringstream json_stream(json_str);

    if (!ipc2kicad::read_json(json_stream, model)) {
        std::cerr << "Error: failed to parse JSON from ODB++ parser\n";
        return false;
    }

    return true;
}

int main(int argc, char* argv[]) {
    std::string input_file;
    std::string output_file;
    int kicad_version = 9;
    std::string step_name;
    bool list_steps = false;
    bool list_layers = false;
    bool export_json = false;
    bool import_json = false;
    bool verbose = false;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_help();
            return 0;
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "Error: -o requires an argument\n";
                return 1;
            }
            output_file = argv[++i];
        } else if (arg == "-v" || arg == "--version") {
            if (i + 1 >= argc) {
                std::cerr << "Error: -v requires an argument (7, 8, or 9)\n";
                return 1;
            }
            kicad_version = std::stoi(argv[++i]);
            if (kicad_version != 7 && kicad_version != 8 && kicad_version != 9) {
                std::cerr << "Error: version must be 7, 8, or 9\n";
                return 1;
            }
        } else if (arg == "-s" || arg == "--step") {
            if (i + 1 >= argc) {
                std::cerr << "Error: -s requires an argument\n";
                return 1;
            }
            step_name = argv[++i];
        } else if (arg == "--list-steps") {
            list_steps = true;
        } else if (arg == "--list-layers") {
            list_layers = true;
        } else if (arg == "--export-json") {
            export_json = true;
        } else if (arg == "--import-json") {
            import_json = true;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg[0] == '-') {
            std::cerr << "Error: unknown option '" << arg << "'\n";
            print_help();
            return 1;
        } else {
            input_file = arg;
        }
    }

    if (input_file.empty()) {
        std::cerr << "Error: no input file specified\n";
        print_help();
        return 1;
    }

    if (output_file.empty()) {
        output_file = replace_extension(input_file, ".kicad_pcb");
    }

    // Detect input format
    InputFormat format = detect_format(input_file, import_json);

    if (format == InputFormat::UNKNOWN) {
        std::cerr << "Error: cannot determine input format for '" << input_file << "'\n";
        std::cerr << "  Supported: .xml, .cvg (IPC-2581), .tgz, .tar.gz, .zip (ODB++), .json\n";
        return 1;
    }

    ipc2kicad::PcbModel model;

    if (format == InputFormat::IPC2581) {
        // ── IPC-2581 path (existing) ────────────────────────────────
        ipc2kicad::ParserOptions parser_opts;
        parser_opts.step_name = step_name;
        parser_opts.verbose = verbose;
        parser_opts.list_steps = list_steps;
        parser_opts.list_layers = list_layers;

        ipc2kicad::Ipc2581Parser parser(parser_opts);

        if (list_steps) {
            auto steps = parser.list_steps(input_file);
            if (steps.empty()) {
                std::cout << "No steps found in " << input_file << "\n";
            } else {
                std::cout << "Steps in " << input_file << ":\n";
                for (auto& s : steps) {
                    std::cout << "  " << s << "\n";
                }
            }
            return 0;
        }

        if (!parser.parse(input_file, model)) {
            std::cerr << "Error: failed to parse " << input_file << "\n";
            for (auto& w : parser.warnings()) {
                std::cerr << "  " << w << "\n";
            }
            return 1;
        }

    } else if (format == InputFormat::ODB) {
        // ── ODB++ path (new) ────────────────────────────────────────
        if (list_steps) {
            // For list-steps, run the Python parser in list-steps mode
            if (!run_odb_parser(argv[0], input_file, step_name, verbose, true, model)) {
                return 1;
            }
            return 0;
        }

        if (verbose) {
            std::cerr << "Detected ODB++ input format\n";
        }

        if (!run_odb_parser(argv[0], input_file, step_name, verbose, false, model)) {
            return 1;
        }

    } else if (format == InputFormat::JSON) {
        // ── JSON import path (new) ──────────────────────────────────
        if (verbose) {
            std::cerr << "Importing from JSON\n";
        }

        std::ifstream json_file(input_file);
        if (!json_file.is_open()) {
            std::cerr << "Error: cannot open " << input_file << "\n";
            return 1;
        }

        if (!ipc2kicad::read_json(json_file, model)) {
            std::cerr << "Error: failed to parse JSON from " << input_file << "\n";
            return 1;
        }
    }

    // Handle --list-layers
    if (list_layers) {
        std::cout << "Layer mapping:\n";
        std::cout << "  " << std::string(40, '-') << "\n";
        for (auto& l : model.layers) {
            std::cout << "  " << l.ipc_name;
            if (!l.ipc_function.empty()) {
                std::cout << " (" << l.ipc_function << ")";
            }
            std::cout << " -> " << (l.kicad_name.empty() ? "(unmapped)" : l.kicad_name) << "\n";
        }
        return 0;
    }

    // Handle --export-json
    if (export_json) {
        ipc2kicad::write_json(std::cout, model);
        return 0;
    }

    // Write KiCad PCB
    ipc2kicad::WriterOptions writer_opts;
    writer_opts.version = (kicad_version == 7) ? ipc2kicad::KiCadVersion::V7
                         : (kicad_version == 8) ? ipc2kicad::KiCadVersion::V8
                                                : ipc2kicad::KiCadVersion::V9;
    writer_opts.verbose = verbose;

    ipc2kicad::KicadWriter writer(writer_opts);
    if (!writer.write(output_file, model)) {
        std::cerr << "Error: failed to write " << output_file << "\n";
        return 1;
    }

    std::cout << "Converted " << input_file << " -> " << output_file;
    std::cout << " (KiCad " << kicad_version << " format)\n";
    std::cout << "  Components: " << model.components.size() << "\n";
    std::cout << "  Traces: " << model.traces.size() << "\n";
    std::cout << "  Vias: " << model.vias.size() << "\n";
    std::cout << "  Nets: " << (model.nets.empty() ? 0 : model.nets.size() - 1) << "\n";

    return 0;
}
