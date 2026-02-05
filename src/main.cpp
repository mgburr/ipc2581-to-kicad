#include "ipc2581_parser.h"
#include "kicad_writer.h"

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

static void print_help() {
    std::cout << "Usage: ipc2581-to-kicad [options] <input.xml>\n"
              << "\n"
              << "Convert IPC-2581 XML files to KiCad .kicad_pcb format.\n"
              << "\n"
              << "Options:\n"
              << "  -o, --output <file>       Output .kicad_pcb file (default: <input>.kicad_pcb)\n"
              << "  -v, --version <7|8>       Target KiCad version (default: 8)\n"
              << "  -s, --step <name>         Step name to convert (default: first step)\n"
              << "  --list-steps              List available steps and exit\n"
              << "  --list-layers             List layer mapping and exit\n"
              << "  --verbose                 Verbose output during conversion\n"
              << "  -h, --help                Show help\n";
}

static std::string replace_extension(const std::string& path, const std::string& new_ext) {
    auto dot = path.rfind('.');
    if (dot != std::string::npos) {
        return path.substr(0, dot) + new_ext;
    }
    return path + new_ext;
}

int main(int argc, char* argv[]) {
    std::string input_file;
    std::string output_file;
    int kicad_version = 8;
    std::string step_name;
    bool list_steps = false;
    bool list_layers = false;
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
                std::cerr << "Error: -v requires an argument (7 or 8)\n";
                return 1;
            }
            kicad_version = std::stoi(argv[++i]);
            if (kicad_version != 7 && kicad_version != 8) {
                std::cerr << "Error: version must be 7 or 8\n";
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

    // Set up parser
    ipc2kicad::ParserOptions parser_opts;
    parser_opts.step_name = step_name;
    parser_opts.verbose = verbose;
    parser_opts.list_steps = list_steps;
    parser_opts.list_layers = list_layers;

    ipc2kicad::Ipc2581Parser parser(parser_opts);

    // Handle --list-steps
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

    // Parse IPC-2581
    ipc2kicad::PcbModel model;
    if (!parser.parse(input_file, model)) {
        std::cerr << "Error: failed to parse " << input_file << "\n";
        for (auto& w : parser.warnings()) {
            std::cerr << "  " << w << "\n";
        }
        return 1;
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

    // Write KiCad PCB
    ipc2kicad::WriterOptions writer_opts;
    writer_opts.version = (kicad_version == 7) ? ipc2kicad::KiCadVersion::V7
                                               : ipc2kicad::KiCadVersion::V8;
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
    std::cout << "  Nets: " << (model.nets.size() - 1) << "\n";

    return 0;
}
