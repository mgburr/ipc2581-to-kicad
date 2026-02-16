#include "gerber_parser.h"
#include "drill_parser.h"
#include "netlist_parser.h"
#include "ipc2581_parser.h"
#include "geometry.h"
#include "utils.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <regex>
#include <set>
#include <cctype>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace ipc2kicad {

GerberParser::GerberParser(bool verbose)
    : verbose_(verbose) {}

// ─── Phase 1: Scan & Identify ─────────────────────────────────────────

static std::string basename_of(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

static std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

static std::string get_extension(const std::string& filename) {
    auto dot = filename.rfind('.');
    if (dot == std::string::npos) return "";
    return to_lower(filename.substr(dot));
}

GerberSet GerberParser::scan_directory(const std::string& dir) {
    GerberSet gset;
    gset.directory = dir;

#ifndef _WIN32
    DIR* d = opendir(dir.c_str());
    if (!d) {
        warn("Cannot open directory: " + dir);
        return gset;
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        std::string fullpath = dir + "/" + name;
        struct stat st;
        if (stat(fullpath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;

        auto info = identify_file(fullpath);
        if (info.file_type != GerberFileInfo::UNKNOWN) {
            gset.files.push_back(std::move(info));
        }
    }
    closedir(d);
#else
    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile((dir + "\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        warn("Cannot open directory: " + dir);
        return gset;
    }
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string fullpath = dir + "\\" + name;
        auto info = identify_file(fullpath);
        if (info.file_type != GerberFileInfo::UNKNOWN) {
            gset.files.push_back(std::move(info));
        }
    } while (FindNextFile(hFind, &fd));
    FindClose(hFind);
#endif

    // Sort by filename for deterministic ordering
    std::sort(gset.files.begin(), gset.files.end(),
              [](const GerberFileInfo& a, const GerberFileInfo& b) {
                  return a.filename < b.filename;
              });

    log("Scanned " + std::to_string(gset.files.size()) + " files in " + dir);
    return gset;
}

GerberSet GerberParser::scan_files(const std::vector<std::string>& paths) {
    GerberSet gset;
    for (auto& p : paths) {
        auto info = identify_file(p);
        if (info.file_type != GerberFileInfo::UNKNOWN) {
            gset.files.push_back(std::move(info));
        }
    }
    return gset;
}

GerberFileInfo GerberParser::identify_file(const std::string& filepath) {
    GerberFileInfo info;
    info.filepath = filepath;
    info.filename = basename_of(filepath);
    info.file_type = detect_file_type(filepath, info.filename);

    if (info.file_type == GerberFileInfo::GERBER) {
        // Try X2 attribute first
        info.x2_file_function = detect_layer_x2(filepath);
        if (!info.x2_file_function.empty()) {
            info.detected_layer = info.x2_file_function;
        }
        // Try extension-based detection
        if (info.detected_layer.empty()) {
            info.detected_layer = detect_layer_extension(info.filename);
        }
        // Try KiCad naming convention
        if (info.detected_layer.empty()) {
            info.detected_layer = detect_layer_kicad_naming(info.filename);
        }
        // Try keyword matching
        if (info.detected_layer.empty()) {
            info.detected_layer = detect_layer_keywords(info.filename);
        }
        // Fallback
        if (info.detected_layer.empty()) {
            info.detected_layer = "(unmapped)";
        }
    } else if (info.file_type == GerberFileInfo::DRILL) {
        info.detected_layer = "(drill)";
    } else if (info.file_type == GerberFileInfo::NETLIST) {
        info.detected_layer = "(netlist)";
    }

    info.assigned_layer = info.detected_layer;

    if (verbose_) {
        std::string type_name = "unknown";
        if (info.file_type == GerberFileInfo::GERBER) type_name = "gerber";
        else if (info.file_type == GerberFileInfo::DRILL) type_name = "drill";
        else if (info.file_type == GerberFileInfo::NETLIST) type_name = "netlist";
        log("  " + info.filename + " -> " + type_name + " -> " + info.detected_layer);
    }

    return info;
}

GerberFileInfo::FileType GerberParser::detect_file_type(
        const std::string& filepath, const std::string& filename) {

    std::string ext = get_extension(filename);

    // Extension-based quick checks
    if (ext == ".drl" || ext == ".nc" || ext == ".xln" || ext == ".exc")
        return GerberFileInfo::DRILL;
    if (ext == ".ipc" || ext == ".d356")
        return GerberFileInfo::NETLIST;

    // Content-based detection: read first ~50 lines
    std::ifstream f(filepath);
    if (!f.is_open()) return GerberFileInfo::UNKNOWN;

    bool has_fs = false, has_d_cmd = false, has_ad = false;
    bool has_m48 = false, has_tool_def = false, has_metric_inch = false;
    bool has_ipc_record = false;
    int line_count = 0;

    std::string line;
    while (std::getline(f, line) && line_count < 100) {
        line_count++;
        std::string tline = trim(line);
        if (tline.empty()) continue;

        // Gerber markers
        if (tline.find("%FS") != std::string::npos) has_fs = true;
        if (tline.find("%AD") != std::string::npos) has_ad = true;
        if (tline.find("D01*") != std::string::npos ||
            tline.find("D02*") != std::string::npos ||
            tline.find("D03*") != std::string::npos) has_d_cmd = true;
        if (tline == "M02*" || tline == "M00*") has_d_cmd = true;

        // Drill markers
        if (tline == "M48" || tline == "M48*") has_m48 = true;
        if (tline.size() >= 3 && tline[0] == 'T' && std::isdigit(tline[1]) &&
            tline.find('C') != std::string::npos) has_tool_def = true;
        if (tline == "METRIC" || tline == "METRIC,TZ" || tline == "METRIC,LZ" ||
            tline == "INCH" || tline == "INCH,TZ" || tline == "INCH,LZ")
            has_metric_inch = true;

        // IPC-D-356 markers
        if (tline.size() >= 3) {
            std::string code = tline.substr(0, 3);
            if (code == "327" || code == "317" || code == "999") has_ipc_record = true;
        }
        if (tline.substr(0, 5) == "P  JO" || tline.substr(0, 6) == "C  IPC")
            has_ipc_record = true;
    }

    // Priority: drill > netlist > gerber
    if (has_m48 || (has_tool_def && has_metric_inch))
        return GerberFileInfo::DRILL;
    if (has_ipc_record)
        return GerberFileInfo::NETLIST;
    if (has_fs || has_ad || has_d_cmd)
        return GerberFileInfo::GERBER;

    // Extension fallback for common Gerber extensions
    static const std::set<std::string> gerber_exts = {
        ".gbr", ".ger", ".art", ".pho",
        ".gtl", ".gbl", ".gts", ".gbs", ".gto", ".gbo",
        ".gtp", ".gbp", ".gko", ".gm1", ".gm2",
        ".g1", ".g2", ".g3", ".g4", ".g5", ".g6",
        ".sol", ".cmp", ".stc", ".sts", ".plc", ".pls",
    };
    if (gerber_exts.count(ext)) return GerberFileInfo::GERBER;

    return GerberFileInfo::UNKNOWN;
}

std::string GerberParser::detect_layer_x2(const std::string& filepath) {
    std::ifstream f(filepath);
    if (!f.is_open()) return "";

    std::string line;
    int count = 0;
    while (std::getline(f, line) && count < 50) {
        count++;
        // Look for %TF.FileFunction,...*%
        auto pos = line.find("%TF.FileFunction,");
        if (pos == std::string::npos) continue;

        // Extract the parameters between , and *%
        auto start = pos + 17; // after "%TF.FileFunction,"
        auto end = line.find("*%", start);
        if (end == std::string::npos) end = line.find("*", start);
        if (end == std::string::npos) continue;

        std::string params = line.substr(start, end - start);
        // params is e.g. "Copper,L1,Top" or "SolderMask,Top" or "Legend,Top" or "Profile,NP"

        std::string lower_params = to_lower(params);

        if (lower_params.find("copper") != std::string::npos) {
            if (lower_params.find("top") != std::string::npos ||
                lower_params.find(",l1,") != std::string::npos ||
                lower_params.find(",l1") == lower_params.size() - 3)
                return "F.Cu";
            if (lower_params.find("bot") != std::string::npos)
                return "B.Cu";
            // Inner layers: Copper,L2,Inr or Copper,L3,Inr etc
            std::regex inner_re("copper,l(\\d+)", std::regex::icase);
            std::smatch m;
            if (std::regex_search(params, m, inner_re)) {
                int n = std::stoi(m[1].str());
                if (n >= 2) return "In" + std::to_string(n - 1) + ".Cu";
            }
        }
        if (lower_params.find("soldermask") != std::string::npos ||
            lower_params.find("solder_mask") != std::string::npos) {
            if (lower_params.find("top") != std::string::npos) return "F.Mask";
            if (lower_params.find("bot") != std::string::npos) return "B.Mask";
        }
        if (lower_params.find("legend") != std::string::npos ||
            lower_params.find("silkscreen") != std::string::npos) {
            if (lower_params.find("top") != std::string::npos) return "F.SilkS";
            if (lower_params.find("bot") != std::string::npos) return "B.SilkS";
        }
        if (lower_params.find("solderpaste") != std::string::npos ||
            lower_params.find("paste") != std::string::npos) {
            if (lower_params.find("top") != std::string::npos) return "F.Paste";
            if (lower_params.find("bot") != std::string::npos) return "B.Paste";
        }
        if (lower_params.find("profile") != std::string::npos ||
            lower_params.find("outline") != std::string::npos) {
            return "Edge.Cuts";
        }
        if (lower_params.find("fabrication") != std::string::npos ||
            lower_params.find("assembly") != std::string::npos) {
            if (lower_params.find("top") != std::string::npos) return "F.Fab";
            if (lower_params.find("bot") != std::string::npos) return "B.Fab";
        }
    }

    return "";
}

std::string GerberParser::detect_layer_extension(const std::string& filename) {
    std::string ext = get_extension(filename);

    // Protel/Altium extensions
    if (ext == ".gtl") return "F.Cu";
    if (ext == ".gbl") return "B.Cu";
    if (ext == ".gts") return "F.Mask";
    if (ext == ".gbs") return "B.Mask";
    if (ext == ".gto") return "F.SilkS";
    if (ext == ".gbo") return "B.SilkS";
    if (ext == ".gtp") return "F.Paste";
    if (ext == ".gbp") return "B.Paste";
    if (ext == ".gko" || ext == ".gm1") return "Edge.Cuts";

    // Inner copper layers .G1 through .G30
    if (ext.size() >= 2 && ext[0] == '.' && ext[1] == 'g') {
        std::string num_str = ext.substr(2);
        if (!num_str.empty() && std::all_of(num_str.begin(), num_str.end(), ::isdigit)) {
            int n = std::stoi(num_str);
            if (n >= 1 && n <= 30) return "In" + std::to_string(n) + ".Cu";
        }
    }

    // Eagle extensions
    if (ext == ".cmp" || ext == ".sol" || ext == ".top") return "F.Cu";
    if (ext == ".bot") return "B.Cu";
    if (ext == ".stc") return "F.Mask";
    if (ext == ".sts") return "B.Mask";
    if (ext == ".plc") return "F.SilkS";
    if (ext == ".pls") return "B.SilkS";

    return "";
}

std::string GerberParser::detect_layer_kicad_naming(const std::string& filename) {
    // KiCad exports: projectname-F_Cu.gbr, projectname-B_Mask.gbr, etc.
    std::string lower = to_lower(filename);

    struct { const char* pattern; const char* layer; } kicad_patterns[] = {
        {"-f_cu.",          "F.Cu"},
        {"-b_cu.",          "B.Cu"},
        {"-f_mask.",        "F.Mask"},
        {"-b_mask.",        "B.Mask"},
        {"-f_silks.",       "F.SilkS"},
        {"-b_silks.",       "B.SilkS"},
        {"-f_silkscreen.",  "F.SilkS"},
        {"-b_silkscreen.",  "B.SilkS"},
        {"-f_paste.",       "F.Paste"},
        {"-b_paste.",       "B.Paste"},
        {"-f_fab.",         "F.Fab"},
        {"-b_fab.",         "B.Fab"},
        {"-f_crtyd.",       "F.CrtYd"},
        {"-b_crtyd.",       "B.CrtYd"},
        {"-f_courtyard.",   "F.CrtYd"},
        {"-b_courtyard.",   "B.CrtYd"},
        {"-f_adhesive.",    "F.Adhes"},
        {"-b_adhesive.",    "B.Adhes"},
        {"-edge_cuts.",     "Edge.Cuts"},
        {"-dwgs_user.",     "Dwgs.User"},
        {"-cmts_user.",     "Cmts.User"},
        {"-user_drawings.", "Dwgs.User"},
        {"-user_comments.", "Cmts.User"},
        {"-user_eco1.",     "Eco1.User"},
        {"-user_eco2.",     "Eco2.User"},
        {"-margin.",        "Margin"},
    };

    for (auto& [pattern, layer] : kicad_patterns) {
        if (lower.find(pattern) != std::string::npos) return layer;
    }

    // Inner copper: -in1_cu., -in2_cu., etc.
    std::regex inner_re("-in(\\d+)_cu\\.", std::regex::icase);
    std::smatch m;
    if (std::regex_search(filename, m, inner_re)) {
        return "In" + m[1].str() + ".Cu";
    }

    return "";
}

std::string GerberParser::detect_layer_keywords(const std::string& filename) {
    std::string lower = to_lower(filename);
    // Remove extension for keyword matching
    auto dot = lower.rfind('.');
    if (dot != std::string::npos) lower = lower.substr(0, dot);

    // Check for specific combinations
    bool has_top = lower.find("top") != std::string::npos ||
                   lower.find("front") != std::string::npos ||
                   lower.find("_f") != std::string::npos;
    bool has_bot = lower.find("bot") != std::string::npos ||
                   lower.find("back") != std::string::npos ||
                   lower.find("rear") != std::string::npos ||
                   lower.find("_b") != std::string::npos;
    bool has_copper = lower.find("copper") != std::string::npos ||
                      lower.find("_cu") != std::string::npos;
    bool has_mask = lower.find("mask") != std::string::npos ||
                    lower.find("solder") != std::string::npos;
    bool has_silk = lower.find("silk") != std::string::npos ||
                    lower.find("legend") != std::string::npos ||
                    lower.find("overlay") != std::string::npos;
    bool has_paste = lower.find("paste") != std::string::npos ||
                     lower.find("cream") != std::string::npos;
    bool has_outline = lower.find("outline") != std::string::npos ||
                       lower.find("edge") != std::string::npos ||
                       lower.find("contour") != std::string::npos ||
                       lower.find("profile") != std::string::npos ||
                       lower.find("boardoutline") != std::string::npos;
    bool has_fab = lower.find("fab") != std::string::npos ||
                   lower.find("assembly") != std::string::npos;

    if (has_outline) return "Edge.Cuts";
    if (has_mask && has_top) return "F.Mask";
    if (has_mask && has_bot) return "B.Mask";
    if (has_silk && has_top) return "F.SilkS";
    if (has_silk && has_bot) return "B.SilkS";
    if (has_paste && has_top) return "F.Paste";
    if (has_paste && has_bot) return "B.Paste";
    if (has_fab && has_top) return "F.Fab";
    if (has_fab && has_bot) return "B.Fab";
    if (has_copper && has_top) return "F.Cu";
    if (has_copper && has_bot) return "B.Cu";
    // Simple top/bottom without qualifiers -> copper
    if (has_top && !has_mask && !has_silk && !has_paste && !has_fab) return "F.Cu";
    if (has_bot && !has_mask && !has_silk && !has_paste && !has_fab) return "B.Cu";

    return "";
}

// ─── Phase 2: Parse & Populate ────────────────────────────────────────

bool GerberParser::parse(const GerberSet& gset, PcbModel& model) {
    // First, set up layers from the Gerber set
    std::set<std::string> used_layers;
    for (auto& fi : gset.files) {
        std::string layer = fi.assigned_layer;
        if (layer.empty() || layer == "(unmapped)" ||
            layer == "(drill)" || layer == "(netlist)") continue;
        used_layers.insert(layer);
    }

    // Build layer definitions
    for (auto& layer_name : used_layers) {
        LayerDef ld;
        ld.kicad_name = layer_name;
        ld.kicad_id = kicad_layer_name_to_id(layer_name);
        ld.ipc_name = layer_name;  // use KiCad name as ipc_name for Gerber
        ld.type = (layer_name.find(".Cu") != std::string::npos) ? "signal" : "user";
        if (layer_name.find("Top") != std::string::npos ||
            layer_name.find("F.") != std::string::npos)
            ld.ipc_side = "TOP";
        else if (layer_name.find("Bottom") != std::string::npos ||
                 layer_name.find("B.") != std::string::npos)
            ld.ipc_side = "BOTTOM";
        else if (layer_name.find("In") != std::string::npos)
            ld.ipc_side = "INTERNAL";
        else
            ld.ipc_side = "ALL";
        model.ipc_layer_to_kicad[layer_name] = layer_name;
        model.layers.push_back(ld);
    }

    // Add net 0 (unconnected)
    if (model.nets.empty()) {
        NetDef n0;
        n0.id = 0;
        n0.name = "";
        model.nets.push_back(n0);
        model.net_name_to_id[""] = 0;
    }

    // Parse each file
    for (auto& fi : gset.files) {
        if (fi.file_type == GerberFileInfo::GERBER) {
            std::string layer = fi.assigned_layer;
            if (layer.empty() || layer == "(unmapped)") {
                if (verbose_) log("Skipping unmapped file: " + fi.filename);
                continue;
            }
            if (!parse_gerber_file(fi, model)) {
                warn("Failed to parse Gerber file: " + fi.filename);
            }
        } else if (fi.file_type == GerberFileInfo::DRILL) {
            DrillParser dp(verbose_);
            if (!dp.parse(fi.filepath, model)) {
                warn("Failed to parse drill file: " + fi.filename);
            }
        } else if (fi.file_type == GerberFileInfo::NETLIST) {
            NetlistParser np(verbose_);
            if (!np.parse(fi.filepath, model)) {
                warn("Failed to parse netlist file: " + fi.filename);
            }
        }
    }

    log("Parsed " + std::to_string(model.traces.size()) + " traces, " +
        std::to_string(model.vias.size()) + " vias, " +
        std::to_string(model.zones.size()) + " zones, " +
        std::to_string(model.graphics.size()) + " graphics");

    return true;
}

// ─── RS-274X State Machine ────────────────────────────────────────────

bool GerberParser::parse_gerber_file(const GerberFileInfo& info, PcbModel& model) {
    std::ifstream f(info.filepath);
    if (!f.is_open()) {
        warn("Cannot open file: " + info.filepath);
        return false;
    }

    log("Parsing Gerber: " + info.filename + " -> " + info.assigned_layer);

    GerberState state;
    state.reset();
    state.kicad_layer = info.assigned_layer;
    state.is_copper = (info.assigned_layer.find(".Cu") != std::string::npos);

    // Read entire file content
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // Process line by line, but also handle multi-line blocks delimited by *
    // Gerber commands end with * and extended commands are wrapped in %...*%
    std::string buffer;
    bool in_extended = false;
    std::string extended_buffer;

    for (size_t i = 0; i < content.size(); i++) {
        char c = content[i];

        if (c == '%') {
            if (!in_extended) {
                // Start of extended command
                in_extended = true;
                extended_buffer.clear();
            } else {
                // End of extended command
                in_extended = false;
                // Process the extended command
                process_command_block("%" + extended_buffer + "%", state, model);
                extended_buffer.clear();
            }
            continue;
        }

        if (in_extended) {
            if (c == '\n' || c == '\r') continue;  // skip newlines in extended blocks
            extended_buffer += c;
            continue;
        }

        if (c == '\n' || c == '\r') {
            // Skip, but don't clear buffer
            continue;
        }

        buffer += c;

        if (c == '*') {
            // End of a command word
            if (!buffer.empty()) {
                process_command_block(buffer, state, model);
                buffer.clear();
            }
        }
    }

    // Flush any pending region
    if (state.region_mode && !state.region_points.empty()) {
        flush_region(state, model);
    }

    return true;
}

double GerberParser::parse_coordinate(const std::string& str, int n_int, int n_dec,
                                       bool leading_zero_suppression) {
    if (str.empty()) return 0.0;

    // Handle sign
    bool negative = false;
    std::string digits;
    for (char c : str) {
        if (c == '-') negative = true;
        else if (c == '+') { /* skip */ }
        else if (std::isdigit(c)) digits += c;
    }

    if (digits.empty()) return 0.0;

    int total = n_int + n_dec;

    if (leading_zero_suppression) {
        // Leading zeros are omitted. Pad on the left to total length.
        while ((int)digits.size() < total) digits = "0" + digits;
    } else {
        // Trailing zeros are omitted. Pad on the right to total length.
        while ((int)digits.size() < total) digits += "0";
    }

    // Insert decimal point
    double val = 0.0;
    if ((int)digits.size() > n_dec) {
        std::string int_part = digits.substr(0, digits.size() - n_dec);
        std::string dec_part = digits.substr(digits.size() - n_dec);
        val = std::stod(int_part + "." + dec_part);
    } else {
        // All decimal
        std::string padded = std::string(n_dec - digits.size(), '0') + digits;
        val = std::stod("0." + padded);
    }

    return negative ? -val : val;
}

Point GerberParser::state_to_kicad(const GerberState& state) {
    double x = state.cur_x;
    double y = state.cur_y;

    // Convert to mm if needed
    if (!state.metric) {
        x *= 25.4;
        y *= 25.4;
    }

    // Gerber Y-up to KiCad Y-down
    return {x, -y};
}

void GerberParser::process_command_block(const std::string& block,
                                          GerberState& state, PcbModel& model) {
    if (block.empty()) return;

    // Extended commands (start with %)
    if (block[0] == '%') {
        // Strip % delimiters and trailing *
        std::string cmd = block.substr(1);
        if (!cmd.empty() && cmd.back() == '%') cmd.pop_back();
        // May contain multiple *-separated commands
        std::istringstream ss(cmd);
        std::string part;
        while (std::getline(ss, part, '*')) {
            if (part.empty()) continue;

            // Format specification
            if (part.substr(0, 2) == "FS") {
                // e.g. FSLAX24Y24 or FSTAX24Y24
                for (size_t i = 2; i < part.size(); i++) {
                    if (part[i] == 'L') state.leading_zero_suppression = true;
                    else if (part[i] == 'T') state.leading_zero_suppression = false;
                    else if (part[i] == 'A') state.absolute_mode = true;
                    else if (part[i] == 'I') state.absolute_mode = false;
                    else if (part[i] == 'X' && i + 2 < part.size()) {
                        state.x_int = part[i+1] - '0';
                        state.x_dec = part[i+2] - '0';
                        i += 2;
                    } else if (part[i] == 'Y' && i + 2 < part.size()) {
                        state.y_int = part[i+1] - '0';
                        state.y_dec = part[i+2] - '0';
                        i += 2;
                    }
                }
                if (verbose_) {
                    log("  FS: X=" + std::to_string(state.x_int) + "." +
                        std::to_string(state.x_dec) + " Y=" +
                        std::to_string(state.y_int) + "." +
                        std::to_string(state.y_dec) +
                        (state.leading_zero_suppression ? " LZ" : " TZ"));
                }
            }
            // Units
            else if (part.substr(0, 2) == "MO") {
                if (part.find("MM") != std::string::npos) state.metric = true;
                else if (part.find("IN") != std::string::npos) state.metric = false;
            }
            // Aperture definition
            else if (part.substr(0, 2) == "AD") {
                // ADDnnShape,params
                // e.g. ADD10C,0.1 or ADD11R,1.0X0.5 or ADD12O,1.0X0.5
                if (part.size() < 4 || part[2] != 'D') continue;
                // Find aperture code
                size_t shape_start = 3;
                while (shape_start < part.size() && std::isdigit(part[shape_start]))
                    shape_start++;
                int code = std::stoi(part.substr(3, shape_start - 3));

                Aperture ap;
                ap.code = code;

                // Find shape and params
                auto comma = part.find(',', shape_start);
                std::string shape_str;
                std::string params_str;
                if (comma != std::string::npos) {
                    shape_str = part.substr(shape_start, comma - shape_start);
                    params_str = part.substr(comma + 1);
                } else {
                    shape_str = part.substr(shape_start);
                }

                if (shape_str == "C") {
                    ap.shape = Aperture::CIRCLE;
                    if (!params_str.empty()) {
                        // diameter[XholeX[Xhole_y]]
                        auto x_pos = params_str.find('X');
                        if (x_pos != std::string::npos) {
                            ap.param1 = std::stod(params_str.substr(0, x_pos));
                            ap.hole_diameter = std::stod(params_str.substr(x_pos + 1));
                        } else {
                            ap.param1 = std::stod(params_str);
                        }
                    }
                } else if (shape_str == "R") {
                    ap.shape = Aperture::RECT;
                    if (!params_str.empty()) {
                        auto x_pos = params_str.find('X');
                        if (x_pos != std::string::npos) {
                            ap.param1 = std::stod(params_str.substr(0, x_pos));
                            std::string rest = params_str.substr(x_pos + 1);
                            auto x2 = rest.find('X');
                            if (x2 != std::string::npos) {
                                ap.param2 = std::stod(rest.substr(0, x2));
                                ap.hole_diameter = std::stod(rest.substr(x2 + 1));
                            } else {
                                ap.param2 = std::stod(rest);
                            }
                        } else {
                            ap.param1 = std::stod(params_str);
                            ap.param2 = ap.param1;
                        }
                    }
                } else if (shape_str == "O") {
                    ap.shape = Aperture::OBLONG;
                    if (!params_str.empty()) {
                        auto x_pos = params_str.find('X');
                        if (x_pos != std::string::npos) {
                            ap.param1 = std::stod(params_str.substr(0, x_pos));
                            std::string rest = params_str.substr(x_pos + 1);
                            auto x2 = rest.find('X');
                            if (x2 != std::string::npos) {
                                ap.param2 = std::stod(rest.substr(0, x2));
                                ap.hole_diameter = std::stod(rest.substr(x2 + 1));
                            } else {
                                ap.param2 = std::stod(rest);
                            }
                        }
                    }
                } else if (shape_str == "P") {
                    ap.shape = Aperture::POLYGON;
                    if (!params_str.empty()) {
                        // diameter X n_vertices [X rotation [X hole]]
                        std::vector<std::string> ps;
                        std::istringstream pss(params_str);
                        std::string tok;
                        while (std::getline(pss, tok, 'X')) ps.push_back(tok);
                        if (ps.size() >= 1) ap.param1 = std::stod(ps[0]);
                        if (ps.size() >= 2) ap.n_vertices = std::stoi(ps[1]);
                        if (ps.size() >= 3) ap.rotation = std::stod(ps[2]);
                        if (ps.size() >= 4) ap.hole_diameter = std::stod(ps[3]);
                    }
                } else {
                    // Macro aperture
                    ap.shape = Aperture::MACRO;
                    ap.macro_name = shape_str;
                    // Parse comma-separated parameters (if any)
                    // For now, just store basic info
                    if (!params_str.empty()) {
                        ap.param1 = std::stod(params_str);
                    }
                }

                state.apertures[code] = ap;
            }
            // Aperture macro definition (store name but skip complex parsing for now)
            else if (part.substr(0, 2) == "AM") {
                // Just acknowledge, we handle macro apertures by their flash dimensions
            }
            // Layer polarity
            else if (part.substr(0, 2) == "LP") {
                if (part.size() > 2) {
                    state.dark_polarity = (part[2] == 'D');
                }
            }
            // X2 attributes (TF, TA, TD, TO) - already handled in Phase 1
        }
        return;
    }

    // Regular commands (end with *)
    std::string cmd = block;
    if (!cmd.empty() && cmd.back() == '*') cmd.pop_back();
    if (cmd.empty()) return;

    // G-codes
    if (cmd[0] == 'G') {
        int gcode = 0;
        size_t i = 1;
        while (i < cmd.size() && std::isdigit(cmd[i])) {
            gcode = gcode * 10 + (cmd[i] - '0');
            i++;
        }

        switch (gcode) {
            case 1: state.interpolation = 1; break;   // linear
            case 2: state.interpolation = 2; break;   // CW arc
            case 3: state.interpolation = 3; break;   // CCW arc
            case 36: state.region_mode = true; break;  // region on
            case 37:                                    // region off
                flush_region(state, model);
                state.region_mode = false;
                break;
            case 4:  break; // comment
            case 54: break; // select aperture (obsolete, aperture follows)
            case 70: state.metric = false; break; // deprecated: inch
            case 71: state.metric = true;  break; // deprecated: mm
            case 74: break; // single quadrant mode (default for arcs)
            case 75: break; // multi quadrant mode
        }

        // Process remaining part of command after G-code (e.g. G01X...Y...D01*)
        if (i < cmd.size()) {
            process_command_block(cmd.substr(i) + "*", state, model);
        }
        return;
    }

    // M-codes
    if (cmd[0] == 'M') {
        // M02 = end of file, M00 = program stop
        return;
    }

    // D-code for aperture selection (Dnn where nn >= 10)
    if (cmd[0] == 'D' && cmd.size() >= 2 && std::isdigit(cmd[1])) {
        int dcode = std::stoi(cmd.substr(1));
        if (dcode >= 10) {
            state.current_aperture = dcode;
            return;
        }
    }

    // Coordinate data: X...Y...I...J...Dnn*
    // Parse X, Y, I, J values and D operation
    double new_x = state.cur_x, new_y = state.cur_y;
    double i_val = 0.0, j_val = 0.0;
    bool has_x = false, has_y = false, has_i = false, has_j = false;
    int d_op = 0; // 1=draw, 2=move, 3=flash

    size_t pos = 0;
    while (pos < cmd.size()) {
        char key = cmd[pos];
        pos++;

        if (key == 'X' || key == 'Y' || key == 'I' || key == 'J' || key == 'D') {
            // Collect digits (and sign)
            std::string val_str;
            if (pos < cmd.size() && (cmd[pos] == '+' || cmd[pos] == '-')) {
                val_str += cmd[pos++];
            }
            while (pos < cmd.size() && std::isdigit(cmd[pos])) {
                val_str += cmd[pos++];
            }

            if (key == 'D') {
                if (!val_str.empty()) d_op = std::stoi(val_str);
            } else {
                double v;
                if (key == 'X') {
                    v = parse_coordinate(val_str, state.x_int, state.x_dec,
                                         state.leading_zero_suppression);
                    if (state.absolute_mode) new_x = v;
                    else new_x += v;
                    has_x = true;
                } else if (key == 'Y') {
                    v = parse_coordinate(val_str, state.y_int, state.y_dec,
                                         state.leading_zero_suppression);
                    if (state.absolute_mode) new_y = v;
                    else new_y += v;
                    has_y = true;
                } else if (key == 'I') {
                    i_val = parse_coordinate(val_str, state.x_int, state.x_dec,
                                              state.leading_zero_suppression);
                    has_i = true;
                } else if (key == 'J') {
                    j_val = parse_coordinate(val_str, state.y_int, state.y_dec,
                                              state.leading_zero_suppression);
                    has_j = true;
                }
            }
        }
        // Skip other characters
    }

    if (d_op == 0 && !has_x && !has_y) return; // nothing to do

    // Apply unit conversion
    auto to_mm = [&](double v) -> double { return state.metric ? v : v * 25.4; };

    if (d_op == 2) {
        // D02: Move (pen up)
        state.cur_x = new_x;
        state.cur_y = new_y;
        return;
    }

    if (d_op == 3) {
        // D03: Flash
        state.cur_x = new_x;
        state.cur_y = new_y;
        Point kp = {to_mm(state.cur_x), -to_mm(state.cur_y)};

        auto it = state.apertures.find(state.current_aperture);
        if (it == state.apertures.end()) return;
        auto& ap = it->second;

        double ap_w = state.metric ? ap.param1 : ap.param1 * 25.4;
        double ap_h = state.metric ? ap.param2 : ap.param2 * 25.4;

        if (state.is_copper) {
            // Flash on copper -> graphic circle or polygon (fill=true)
            GraphicItem gi;
            gi.layer = state.kicad_layer;
            gi.fill = true;
            gi.width = 0.0;

            if (ap.shape == Aperture::CIRCLE) {
                gi.kind = GraphicItem::CIRCLE;
                gi.center = kp;
                gi.radius = ap_w / 2.0;
            } else if (ap.shape == Aperture::RECT) {
                gi.kind = GraphicItem::POLYGON;
                double hw = ap_w / 2.0, hh = ap_h / 2.0;
                gi.points = {
                    {kp.x - hw, kp.y - hh},
                    {kp.x + hw, kp.y - hh},
                    {kp.x + hw, kp.y + hh},
                    {kp.x - hw, kp.y + hh},
                };
            } else if (ap.shape == Aperture::OBLONG) {
                gi.kind = GraphicItem::POLYGON;
                // Approximate oblong as rectangle (sufficient for most uses)
                double hw = ap_w / 2.0, hh = ap_h / 2.0;
                gi.points = {
                    {kp.x - hw, kp.y - hh},
                    {kp.x + hw, kp.y - hh},
                    {kp.x + hw, kp.y + hh},
                    {kp.x - hw, kp.y + hh},
                };
            } else {
                // Macro or polygon — approximate as circle
                gi.kind = GraphicItem::CIRCLE;
                gi.center = kp;
                gi.radius = ap_w / 2.0;
            }

            model.graphics.push_back(gi);
        } else {
            // Flash on non-copper (silkscreen, mask, etc.)
            GraphicItem gi;
            gi.layer = state.kicad_layer;
            gi.fill = true;
            gi.width = 0.0;

            if (ap.shape == Aperture::CIRCLE) {
                gi.kind = GraphicItem::CIRCLE;
                gi.center = kp;
                gi.radius = ap_w / 2.0;
            } else if (ap.shape == Aperture::RECT || ap.shape == Aperture::OBLONG) {
                gi.kind = GraphicItem::POLYGON;
                double hw = ap_w / 2.0, hh = ap_h / 2.0;
                gi.points = {
                    {kp.x - hw, kp.y - hh},
                    {kp.x + hw, kp.y - hh},
                    {kp.x + hw, kp.y + hh},
                    {kp.x - hw, kp.y + hh},
                };
            } else {
                gi.kind = GraphicItem::CIRCLE;
                gi.center = kp;
                gi.radius = ap_w / 2.0;
            }

            model.graphics.push_back(gi);
        }
        return;
    }

    if (d_op == 1 || (d_op == 0 && (has_x || has_y))) {
        // D01: Draw (or implicit draw when coords present without D-code)
        double old_x = state.cur_x, old_y = state.cur_y;
        state.cur_x = new_x;
        state.cur_y = new_y;

        Point from = {to_mm(old_x), -to_mm(old_y)};
        Point to = {to_mm(new_x), -to_mm(new_y)};

        if (state.region_mode) {
            // Accumulate region points
            if (state.region_points.empty()) {
                state.region_points.push_back(from);
            }

            if (state.interpolation == 1) {
                // Linear segment
                state.region_points.push_back(to);
            } else if ((state.interpolation == 2 || state.interpolation == 3) &&
                       (has_i || has_j)) {
                // Arc in region — approximate with line segments
                double cx = to_mm(old_x + i_val);
                double cy = -to_mm(old_y + j_val);
                Point center = {cx, cy};
                double r = distance(from, center);
                double start_angle = std::atan2(from.y - center.y, from.x - center.x);
                double end_angle = std::atan2(to.y - center.y, to.x - center.x);

                // Determine sweep direction
                double sweep;
                if (state.interpolation == 2) {
                    // CW in Gerber (Y-up) = CCW in KiCad (Y-down)
                    sweep = start_angle - end_angle;
                    if (sweep <= 0) sweep += 2.0 * PI;
                } else {
                    // CCW in Gerber (Y-up) = CW in KiCad (Y-down)
                    sweep = end_angle - start_angle;
                    if (sweep <= 0) sweep += 2.0 * PI;
                }

                // Approximate arc with line segments
                int n_segs = std::max(8, (int)(sweep * r / 0.1));
                for (int s = 1; s <= n_segs; s++) {
                    double t = (double)s / n_segs;
                    double angle;
                    if (state.interpolation == 2) {
                        angle = start_angle - t * sweep;
                    } else {
                        angle = start_angle + t * sweep;
                    }
                    state.region_points.push_back({
                        center.x + r * std::cos(angle),
                        center.y + r * std::sin(angle)
                    });
                }
            } else {
                state.region_points.push_back(to);
            }
            return;
        }

        // Get aperture width
        double width = 0.0;
        auto it = state.apertures.find(state.current_aperture);
        if (it != state.apertures.end()) {
            width = state.metric ? it->second.param1 : it->second.param1 * 25.4;
        }

        if (state.kicad_layer == "Edge.Cuts") {
            // Board outline
            if (state.interpolation == 1) {
                Segment seg;
                seg.start = from;
                seg.end = to;
                seg.width = width;
                seg.layer = "Edge.Cuts";
                model.outline.push_back(seg);
            } else if ((state.interpolation == 2 || state.interpolation == 3) &&
                       (has_i || has_j)) {
                double cx = to_mm(old_x + i_val);
                double cy = -to_mm(old_y + j_val);
                Point center = {cx, cy};
                double r = distance(from, center);
                double start_angle = std::atan2(from.y - center.y, from.x - center.x);
                double end_angle = std::atan2(to.y - center.y, to.x - center.x);

                double sweep;
                if (state.interpolation == 2) {
                    sweep = start_angle - end_angle;
                    if (sweep <= 0) sweep += 2.0 * PI;
                } else {
                    sweep = end_angle - start_angle;
                    if (sweep <= 0) sweep += 2.0 * PI;
                }

                double mid_angle;
                if (state.interpolation == 2) {
                    mid_angle = start_angle - sweep / 2.0;
                } else {
                    mid_angle = start_angle + sweep / 2.0;
                }
                Point mid = {center.x + r * std::cos(mid_angle),
                             center.y + r * std::sin(mid_angle)};

                ArcGeom arc;
                arc.start = from;
                arc.mid = mid;
                arc.end = to;
                arc.width = width;
                arc.layer = "Edge.Cuts";
                model.outline_arcs.push_back(arc);
            }
        } else if (state.is_copper) {
            // Copper trace
            if (state.interpolation == 1) {
                TraceSegment ts;
                ts.start = from;
                ts.end = to;
                ts.width = width;
                ts.layer = state.kicad_layer;
                ts.net_id = 0;
                model.traces.push_back(ts);
            } else if ((state.interpolation == 2 || state.interpolation == 3) &&
                       (has_i || has_j)) {
                double cx = to_mm(old_x + i_val);
                double cy = -to_mm(old_y + j_val);
                Point center = {cx, cy};
                double r = distance(from, center);
                double start_angle = std::atan2(from.y - center.y, from.x - center.x);
                double end_angle = std::atan2(to.y - center.y, to.x - center.x);

                double sweep;
                if (state.interpolation == 2) {
                    sweep = start_angle - end_angle;
                    if (sweep <= 0) sweep += 2.0 * PI;
                } else {
                    sweep = end_angle - start_angle;
                    if (sweep <= 0) sweep += 2.0 * PI;
                }

                double mid_angle;
                if (state.interpolation == 2) {
                    mid_angle = start_angle - sweep / 2.0;
                } else {
                    mid_angle = start_angle + sweep / 2.0;
                }
                Point mid = {center.x + r * std::cos(mid_angle),
                             center.y + r * std::sin(mid_angle)};

                TraceArc ta;
                ta.start = from;
                ta.mid = mid;
                ta.end = to;
                ta.width = width;
                ta.layer = state.kicad_layer;
                ta.net_id = 0;
                model.trace_arcs.push_back(ta);
            }
        } else {
            // Non-copper layer: graphic items
            if (state.interpolation == 1) {
                GraphicItem gi;
                gi.kind = GraphicItem::LINE;
                gi.start = from;
                gi.end = to;
                gi.width = width;
                gi.layer = state.kicad_layer;
                model.graphics.push_back(gi);
            } else if ((state.interpolation == 2 || state.interpolation == 3) &&
                       (has_i || has_j)) {
                double cx = to_mm(old_x + i_val);
                double cy = -to_mm(old_y + j_val);
                Point center = {cx, cy};
                double r = distance(from, center);
                double start_angle = std::atan2(from.y - center.y, from.x - center.x);
                double end_angle = std::atan2(to.y - center.y, to.x - center.x);

                double sweep;
                if (state.interpolation == 2) {
                    sweep = start_angle - end_angle;
                    if (sweep <= 0) sweep += 2.0 * PI;
                } else {
                    sweep = end_angle - start_angle;
                    if (sweep <= 0) sweep += 2.0 * PI;
                }

                double mid_angle;
                if (state.interpolation == 2) {
                    mid_angle = start_angle - sweep / 2.0;
                } else {
                    mid_angle = start_angle + sweep / 2.0;
                }
                Point mid = {center.x + r * std::cos(mid_angle),
                             center.y + r * std::sin(mid_angle)};

                GraphicItem gi;
                gi.kind = GraphicItem::ARC;
                gi.start = from;
                gi.center = mid;  // stored as mid-point for KiCad
                gi.end = to;
                gi.width = width;
                gi.layer = state.kicad_layer;
                model.graphics.push_back(gi);
            }
        }
    }
}

void GerberParser::flush_region(GerberState& state, PcbModel& model) {
    if (state.region_points.size() < 3) {
        state.region_points.clear();
        return;
    }

    if (state.kicad_layer == "Edge.Cuts") {
        // Edge.Cuts region -> outline segments (board outline or cutout)
        // KiCad expects line segments forming closed loops, not filled polygons
        auto& pts = state.region_points;
        for (size_t i = 0; i < pts.size(); i++) {
            Point a = pts[i];
            Point b = pts[(i + 1) % pts.size()];
            // Skip degenerate zero-length segments
            if (std::abs(a.x - b.x) < 1e-6 && std::abs(a.y - b.y) < 1e-6)
                continue;
            Segment seg;
            seg.start = a;
            seg.end = b;
            seg.width = 0.05;  // standard Edge.Cuts line width
            seg.layer = "Edge.Cuts";
            model.outline.push_back(seg);
        }
    } else if (state.is_copper) {
        // Copper region -> Zone
        Zone z;
        z.layer = state.kicad_layer;
        z.net_id = 0;
        z.outline = state.region_points;
        z.clearance = 0.0;
        model.zones.push_back(std::move(z));
    } else {
        // Non-copper region -> filled polygon graphic
        GraphicItem gi;
        gi.kind = GraphicItem::POLYGON;
        gi.layer = state.kicad_layer;
        gi.fill = true;
        gi.width = 0.0;
        gi.points = state.region_points;
        model.graphics.push_back(gi);
    }

    state.region_points.clear();
}

void GerberParser::log(const std::string& msg) {
    if (verbose_) std::cerr << "GerberParser: " << msg << "\n";
}

void GerberParser::warn(const std::string& msg) {
    warnings_.push_back(msg);
    std::cerr << "Warning: " << msg << "\n";
}

} // namespace ipc2kicad
