#include "schematic_writer.h"
#include "utils.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <set>
#include <regex>

namespace ipc2kicad {

// Always double-quote a string for schematic s-expression output.
// KiCad schematic parser is stricter than PCB parser about quoting.
static std::string sq(const std::string& s) {
    std::string result = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') result += '\\';
        result += c;
    }
    result += '"';
    return result;
}

static const double PIN_LEN = 2.54;
static const double PIN_PITCH = 2.54;
static const double GRID = 1.27;         // KiCad schematic grid
static const double GRID_CELL_W = 40.64; // mm between columns (multiple of 1.27)
static const double GRID_MARGIN = 10.16; // mm vertical margin (multiple of 1.27)

// Snap a value to the 1.27mm grid
static double snap(double v) {
    return std::round(v / GRID) * GRID;
}

SchematicWriter::SchematicWriter(const SchematicWriterOptions& opts)
    : opts_(opts)
    , sheet_uuid_(generate_uuid_from_seed("schematic_sheet_root"))
{}

bool SchematicWriter::write(const std::string& filename, const PcbModel& model) {
    std::ofstream out(filename);
    if (!out.is_open()) {
        std::cerr << "Error: cannot open " << filename << " for writing\n";
        return false;
    }
    return write(out, model);
}

bool SchematicWriter::write(std::ostream& out, const PcbModel& model) {
    build_symbol_defs(model);
    layout_components(model);

    // Pre-scan for power nets and load their symbol definitions
    if (opts_.use_kicad_symbols) {
        std::string sym_dir = opts_.kicad_symbol_dir.empty()
            ? detect_symbol_dir() : opts_.kicad_symbol_dir;
        if (!sym_dir.empty()) {
            std::set<std::string> seen_power;
            for (auto& inst : instances_) {
                if (!inst.comp) continue;
                for (auto& [pin_name, net_name] : inst.comp->pin_net_map) {
                    if (net_name.empty() || net_name == "No Net") continue;
                    if (!is_power_net(net_name)) continue;
                    if (!seen_power.insert(net_name).second) continue;

                    std::string sym_name = power_net_symbol_name(net_name);
                    std::string lib_id = "power:" + sym_name;
                    if (power_symbol_defs_.find(lib_id) != power_symbol_defs_.end()) continue;

                    std::string sym_text = load_kicad_symbol(
                        sym_dir + "/power.kicad_sym", sym_name);
                    if (!sym_text.empty()) {
                        std::string old_name = "(symbol \"" + sym_name + "\"";
                        std::string new_name = "(symbol \"" + lib_id + "\"";
                        auto name_pos = sym_text.find(old_name);
                        if (name_pos != std::string::npos) {
                            sym_text.replace(name_pos, old_name.size(), new_name);
                        }
                        power_symbol_defs_[lib_id] = sym_text;
                        log("Loaded power symbol: " + lib_id);
                    }
                }
            }
        }
    }

    std::string paper = opts_.paper_size.empty()
        ? select_paper(static_cast<int>(instances_.size()))
        : opts_.paper_size;

    write_header(out, paper);
    write_lib_symbols(out);
    out << "\n";
    write_wires_and_labels(out, model);
    out << "\n";
    write_symbol_instances(out, model);
    out << "\n";
    write_sheet_instances(out);
    out << ")\n";

    log("Wrote schematic with " + std::to_string(instances_.size()) + " symbols, "
        + std::to_string(symbol_defs_.size()) + " unique footprints");
    return out.good();
}

// ── Power net detection ──────────────────────────────────────────────

bool SchematicWriter::is_power_net(const std::string& net_name) const {
    if (net_name.empty()) return false;
    // Check for exact known power net names or patterns
    std::string upper = net_name;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    // Ground variants
    if (upper == "GND" || upper == "PGND" || upper == "AGND" || upper == "DGND" ||
        upper == "VSS" || upper == "GNDD" || upper == "GNDA") return true;

    // Positive supply variants
    if (upper == "VCC" || upper == "VDD" || upper == "VBUS") return true;

    // +NV patterns: +5V, +3V3, +3.3V, +12V, +1V8, etc.
    if (net_name[0] == '+' && net_name.size() >= 2) return true;

    return false;
}

std::string SchematicWriter::power_net_symbol_name(const std::string& net_name) const {
    // Map net names to KiCad power symbol names in the power library.
    // Most power symbols use the net name directly as the symbol name.
    std::string upper = net_name;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    // Ground variants all map to GND symbol
    if (upper == "GND" || upper == "PGND" || upper == "DGND" || upper == "GNDD")
        return "GND";
    if (upper == "AGND" || upper == "GNDA")
        return "GNDA";
    if (upper == "VSS")
        return "GND";

    // VCC/VDD
    if (upper == "VCC") return "VCC";
    if (upper == "VDD") return "VDD";
    if (upper == "VBUS") return "VBUS";

    // +NV patterns: check if the exact symbol exists in the power library
    // Common ones: +5V, +3V3, +3.3V, +12V, +1V8
    if (net_name[0] == '+') return net_name;

    return net_name;
}

// ── KiCad library symbol support ────────────────────────────────────

std::string SchematicWriter::detect_symbol_dir() const {
    // Check common locations
    const char* candidates[] = {
        "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols",
        "/usr/share/kicad/symbols",
        "/usr/local/share/kicad/symbols",
    };
    for (auto& c : candidates) {
        std::ifstream test(std::string(c) + "/Device.kicad_sym");
        if (test.good()) return c;
    }
    return "";
}

std::string SchematicWriter::load_kicad_symbol(const std::string& lib_file,
                                                const std::string& symbol_name) {
    // Load and cache library file
    if (lib_file_cache_.find(lib_file) == lib_file_cache_.end()) {
        std::ifstream in(lib_file);
        if (!in.good()) {
            log("Warning: cannot open symbol library " + lib_file);
            lib_file_cache_[lib_file] = ""; // cache empty to avoid retrying
            return "";
        }
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        lib_file_cache_[lib_file] = std::move(content);
    }

    auto& content = lib_file_cache_[lib_file];
    if (content.empty()) return "";

    // Find the top-level symbol definition: (symbol "NAME"
    // Must match at a nesting depth of 1 (inside the root kicad_symbol_lib)
    std::string search = "(symbol \"" + symbol_name + "\"";
    size_t pos = 0;
    while ((pos = content.find(search, pos)) != std::string::npos) {
        // Check that this is at nesting depth 1 by counting parens before this position
        // A simpler approach: check that the character before `(symbol` (after whitespace)
        // is at the right level. We'll use a heuristic: look backwards for the indent level.
        // Top-level symbols are indented with a tab or a few spaces after the root.
        // Sub-symbols like "R_0_1" will also match "(symbol \"R_0_1\"" but we want
        // the parent "(symbol \"R\"".

        // More reliable: check that the symbol name exactly matches (no underscore suffix)
        // The search string already contains the exact name with closing quote.
        // Verify the character after the name+quote is a newline or whitespace or paren
        size_t after = pos + search.size();
        if (after < content.size() && content[after] != '\n' && content[after] != '\r' &&
            content[after] != ' ' && content[after] != '\t') {
            pos = after;
            continue;
        }

        // Extract the complete symbol by tracking parenthesis depth
        size_t start = pos;
        int depth = 0;
        bool found_end = false;
        for (size_t i = start; i < content.size(); i++) {
            char c = content[i];
            if (c == '(') depth++;
            else if (c == ')') {
                depth--;
                if (depth == 0) {
                    // Found the matching close paren
                    std::string sym_text = content.substr(start, i - start + 1);
                    return sym_text;
                }
            }
            // Skip strings to avoid counting parens inside them
            if (c == '"') {
                i++;
                while (i < content.size() && content[i] != '"') {
                    if (content[i] == '\\') i++; // skip escaped char
                    i++;
                }
            }
        }

        pos = after;
    }

    log("Warning: symbol \"" + symbol_name + "\" not found in " + lib_file);
    return "";
}

std::string SchematicWriter::map_to_kicad_symbol(const std::string& prefix, int pin_count,
                                                   const Footprint& fp,
                                                   std::string& out_lib_file,
                                                   std::string& out_symbol_name) const {
    // Returns lib_id string like "Device:R" or empty if no mapping

    if (prefix == "R" && pin_count == 2) {
        out_lib_file = "Device.kicad_sym";
        out_symbol_name = "R";
        return "Device:R";
    }
    if (prefix == "C" && pin_count == 2) {
        out_lib_file = "Device.kicad_sym";
        out_symbol_name = "C";
        return "Device:C";
    }
    if (prefix == "L" && pin_count == 2) {
        out_lib_file = "Device.kicad_sym";
        out_symbol_name = "L";
        return "Device:L";
    }
    if (prefix == "D" && pin_count == 2) {
        out_lib_file = "Device.kicad_sym";
        out_symbol_name = "D";
        return "Device:D";
    }
    if (prefix == "TP" && pin_count == 1) {
        out_lib_file = "Connector.kicad_sym";
        out_symbol_name = "TestPoint";
        return "Connector:TestPoint";
    }
    if (prefix == "SW" && (pin_count == 2 || pin_count == 4)) {
        out_lib_file = "Switch.kicad_sym";
        out_symbol_name = "SW_Push";
        return "Switch:SW_Push";
    }
    if ((prefix == "J" || prefix == "P" || prefix == "CN") && pin_count >= 1) {
        // Check that all pad names are numeric sequential (1, 2, 3, ...)
        bool sequential = true;
        for (int i = 0; i < pin_count; i++) {
            try {
                int num = std::stoi(fp.pads[i].name);
                if (num != i + 1) { sequential = false; break; }
            } catch (...) {
                sequential = false;
                break;
            }
        }
        if (sequential && pin_count <= 40) {
            std::string n = std::to_string(pin_count);
            if (n.size() < 2) n = "0" + n;
            out_lib_file = "Connector_Generic.kicad_sym";
            out_symbol_name = "Conn_01x" + n + "_Pin";
            return "Connector_Generic:Conn_01x" + n + "_Pin";
        }
    }

    return ""; // no mapping, use auto-generated
}

// ── Symbol definition building ──────────────────────────────────────

std::string SchematicWriter::ref_prefix(const std::string& refdes) const {
    // Extract alphabetic prefix from refdes like "R1", "C12", "U3"
    std::string prefix;
    for (char c : refdes) {
        if (std::isalpha(static_cast<unsigned char>(c)))
            prefix += c;
        else
            break;
    }
    if (prefix.empty()) prefix = "U";
    return prefix;
}

void SchematicWriter::build_symbol_defs(const PcbModel& model) {
    symbol_defs_.clear();

    // Resolve KiCad symbol directory if using library symbols
    std::string sym_dir;
    if (opts_.use_kicad_symbols) {
        sym_dir = opts_.kicad_symbol_dir.empty() ? detect_symbol_dir() : opts_.kicad_symbol_dir;
        if (sym_dir.empty()) {
            log("Warning: KiCad symbol directory not found, falling back to auto-generated symbols");
        } else {
            log("Using KiCad symbol libraries from: " + sym_dir);
        }
    }

    // Collect which ref prefix is most common for each footprint
    std::map<std::string, std::map<std::string, int>> fp_prefix_counts;
    for (auto& comp : model.components) {
        fp_prefix_counts[comp.footprint_ref][ref_prefix(comp.refdes)]++;
    }

    for (auto& [fp_name, fp] : model.footprint_defs) {
        if (fp.pads.empty()) continue; // skip fiducials/mounting holes

        SymbolDef sym;
        sym.footprint_name = fp_name;

        // Determine ref prefix from most common refdes using this footprint
        auto it = fp_prefix_counts.find(fp_name);
        if (it != fp_prefix_counts.end() && !it->second.empty()) {
            int best = 0;
            for (auto& [p, cnt] : it->second) {
                if (cnt > best) { best = cnt; sym.ref_prefix = p; }
            }
        }
        if (sym.ref_prefix.empty()) sym.ref_prefix = "U";

        int n = static_cast<int>(fp.pads.size());

        // Try to map to a KiCad library symbol
        bool use_library = false;
        if (opts_.use_kicad_symbols && !sym_dir.empty()) {
            std::string lib_file, symbol_name;
            std::string lib_id = map_to_kicad_symbol(sym.ref_prefix, n, fp,
                                                      lib_file, symbol_name);
            if (!lib_id.empty()) {
                std::string full_path = sym_dir + "/" + lib_file;
                std::string sym_text = load_kicad_symbol(full_path, symbol_name);
                if (!sym_text.empty()) {
                    // Rename the outer symbol to use the full lib_id.
                    // KiCad requires lib_symbols entries to match the lib_id
                    // used by instances (e.g. "Device:R"), while sub-symbols
                    // keep their short names (e.g. "R_0_1", "R_1_1").
                    std::string old_name = "(symbol \"" + symbol_name + "\"";
                    std::string new_name = "(symbol \"" + lib_id + "\"";
                    auto name_pos = sym_text.find(old_name);
                    if (name_pos != std::string::npos) {
                        sym_text.replace(name_pos, old_name.size(), new_name);
                    }
                    sym.kicad_lib_id = lib_id;
                    sym.kicad_symbol_text = sym_text;
                    use_library = true;
                    log("Mapped " + fp_name + " -> " + lib_id);
                }
            }
        }

        if (use_library) {
            // For library symbols, we still need pin info for wire/label generation.
            // Parse pin positions from the library symbol text.
            // We need to know pin numbers and their approximate positions.
            // For known symbols, use hardcoded pin layouts:
            if (sym.kicad_lib_id == "Device:R" || sym.kicad_lib_id == "Device:C" ||
                sym.kicad_lib_id == "Device:L") {
                // Vertical 2-pin: pin 1 at top (0, 3.81), pin 2 at bottom (0, -3.81)
                sym.body_width = 2.54;
                sym.body_height = 7.62;
                sym.pins.push_back({"1", 0, -3.81, 0, "passive"}); // top (negative Y = up in schematic)
                sym.pins.push_back({"2", 0, 3.81, 0, "passive"});  // bottom
            } else if (sym.kicad_lib_id == "Device:D") {
                // Horizontal 2-pin: pin 1 (K) at left, pin 2 (A) at right
                sym.body_width = 7.62;
                sym.body_height = 2.54;
                sym.pins.push_back({"1", -3.81, 0, 0, "passive"});
                sym.pins.push_back({"2", 3.81, 0, 1, "passive"});
            } else if (sym.kicad_lib_id == "Connector:TestPoint") {
                // Single pin at bottom
                sym.body_width = 2.54;
                sym.body_height = 5.08;
                sym.pins.push_back({"1", 0, 0, 0, "passive"});
            } else if (sym.kicad_lib_id == "Switch:SW_Push") {
                // Horizontal 2-pin: pin 1 at left, pin 2 at right
                sym.body_width = 10.16;
                sym.body_height = 5.08;
                sym.pins.push_back({"1", -5.08, 0, 0, "passive"});
                sym.pins.push_back({"2", 5.08, 0, 1, "passive"});
            } else if (sym.kicad_lib_id.find("Connector_Generic:Conn_01x") == 0) {
                // Vertical connector: all pins on right side, spaced 2.54mm apart
                sym.body_width = 5.08;
                sym.body_height = (n - 1) * PIN_PITCH + 2 * PIN_PITCH;
                for (int i = 0; i < n; i++) {
                    PinDef pin;
                    pin.name = std::to_string(i + 1);
                    pin.x = 5.08;
                    pin.y = -((n - 1) * PIN_PITCH / 2.0) + i * PIN_PITCH;
                    pin.side = 1;
                    pin.type = "passive";
                    sym.pins.push_back(pin);
                }
            }
        } else {
            // Auto-generated symbol (existing behavior)
            std::string pin_type = (n == 2) ? "passive" : "unspecified";

            // Split pins: left side vs right side
            int left_count = (n + 1) / 2;
            for (int i = 0; i < n; i++) {
                PinDef pin;
                pin.name = fp.pads[i].name;
                pin.type = pin_type;
                if (i < left_count) {
                    pin.side = 0;
                } else {
                    pin.side = 1;
                }
                sym.pins.push_back(pin);
            }

            int left_n = left_count;
            int right_n = n - left_count;
            double body_h = std::max(left_n, right_n) * PIN_PITCH + PIN_PITCH;
            body_h = std::max(body_h, 2 * PIN_PITCH);

            double label_w = static_cast<double>(fp_name.size()) * 1.27 + 2.54;
            double body_w = std::max(5.08, label_w);
            body_w = std::ceil(body_w / PIN_PITCH) * PIN_PITCH;

            sym.body_width = body_w;
            sym.body_height = body_h;

            double half_w = body_w / 2.0;
            for (int i = 0, li = 0, ri = 0; i < n; i++) {
                if (sym.pins[i].side == 0) {
                    double y_off = -(left_n - 1) * PIN_PITCH / 2.0 + li * PIN_PITCH;
                    sym.pins[i].x = -(half_w + PIN_LEN);
                    sym.pins[i].y = y_off;
                    li++;
                } else {
                    double y_off = -(right_n - 1) * PIN_PITCH / 2.0 + ri * PIN_PITCH;
                    sym.pins[i].x = half_w + PIN_LEN;
                    sym.pins[i].y = y_off;
                    ri++;
                }
            }
        }

        symbol_defs_[fp_name] = std::move(sym);
    }
}

// ── Layout ──────────────────────────────────────────────────────────

// Natural sort comparison for refdes strings (R1 < R2 < R10)
static bool natural_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) &&
            std::isdigit(static_cast<unsigned char>(b[j]))) {
            // Compare numeric portions
            size_t ni = i, nj = j;
            while (ni < a.size() && std::isdigit(static_cast<unsigned char>(a[ni]))) ni++;
            while (nj < b.size() && std::isdigit(static_cast<unsigned char>(b[nj]))) nj++;
            long na = std::stol(a.substr(i, ni - i));
            long nb = std::stol(b.substr(j, nj - j));
            if (na != nb) return na < nb;
            i = ni; j = nj;
        } else {
            unsigned char ca = std::toupper(static_cast<unsigned char>(a[i]));
            unsigned char cb = std::toupper(static_cast<unsigned char>(b[j]));
            if (ca != cb) return ca < cb;
            i++; j++;
        }
    }
    return a.size() < b.size();
}

void SchematicWriter::layout_components(const PcbModel& model) {
    instances_.clear();

    // Build instance list, skipping components whose footprint has no pads
    for (auto& comp : model.components) {
        if (symbol_defs_.find(comp.footprint_ref) == symbol_defs_.end())
            continue;
        SymbolInstance inst;
        inst.refdes = comp.refdes;
        inst.value = comp.value.empty() ? comp.footprint_ref : comp.value;
        inst.footprint_name = comp.footprint_ref;
        inst.comp = &comp;
        instances_.push_back(inst);
    }

    // Sort by refdes (natural order)
    std::sort(instances_.begin(), instances_.end(),
        [](const SymbolInstance& a, const SymbolInstance& b) {
            return natural_less(a.refdes, b.refdes);
        });

    // Grid placement (all positions snapped to 1.27mm grid)
    double x = snap(30.48);  // left margin
    double y = snap(30.48);  // top margin
    double col_max_y = y;
    int col_count = 0;
    int max_per_col = 8;

    for (auto& inst : instances_) {
        auto& sym = symbol_defs_[inst.footprint_name];
        double cell_h = snap(sym.body_height + GRID_MARGIN);

        if (col_count >= max_per_col) {
            x += GRID_CELL_W;
            y = snap(30.48);
            col_count = 0;
        }

        inst.x = snap(x);
        inst.y = snap(y + sym.body_height / 2.0);
        y += cell_h;
        if (y > col_max_y) col_max_y = y;
        col_count++;
    }
}

std::string SchematicWriter::select_paper(int count) const {
    if (count <= 15) return "A4";
    if (count <= 60) return "A3";
    return "A2";
}

// ── Section writers ─────────────────────────────────────────────────

void SchematicWriter::write_header(std::ostream& out, const std::string& paper) {
    out << "(kicad_sch\n"
        << "  (version 20250114)\n"
        << "  (generator \"ipc2581_to_kicad\")\n"
        << "  (generator_version \"1.0\")\n"
        << "  (uuid \"" << sheet_uuid_ << "\")\n"
        << "  (paper \"" << paper << "\")\n";
}

void SchematicWriter::write_lib_symbols(std::ostream& out) {
    out << "  (lib_symbols\n";

    // Track which KiCad library symbols have been written (by lib_id)
    std::set<std::string> written_lib_symbols;

    for (auto& [fp_name, sym] : symbol_defs_) {
        if (!sym.kicad_lib_id.empty() && !sym.kicad_symbol_text.empty()) {
            // Write KiCad library symbol (only once per lib_id)
            if (written_lib_symbols.insert(sym.kicad_lib_id).second) {
                // Indent the symbol text by 4 spaces for proper nesting
                out << "    " << sym.kicad_symbol_text << "\n";
            }
        } else {
            // Auto-generated symbol
            std::string lib_name = "ipc2581_auto:" + fp_name;
            std::string q_name = sq(lib_name);
            std::string q_fp = sq(fp_name);

            out << "    (symbol " << q_name << "\n";

            // Properties
            out << "      (property \"Reference\" \"" << sym.ref_prefix << "\""
                << " (at 0 " << fmt(sym.body_height / 2.0 + 1.27) << " 0)"
                << " (effects (font (size 1.27 1.27))))\n";
            out << "      (property \"Value\" " << q_fp
                << " (at 0 " << fmt(-(sym.body_height / 2.0 + 1.27)) << " 0)"
                << " (effects (font (size 1.27 1.27))))\n";
            out << "      (property \"Footprint\" " << sq("ipc2581:" + fp_name)
                << " (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n";

            // Symbol body (unit 0, style 1): rectangle
            out << "      (symbol " << sq(fp_name + "_0_1") << "\n";
            double hw = sym.body_width / 2.0;
            double hh = sym.body_height / 2.0;
            out << "        (rectangle (start " << fmt(-hw) << " " << fmt(-hh) << ")"
                << " (end " << fmt(hw) << " " << fmt(hh) << ")"
                << " (stroke (width 0.254) (type default))"
                << " (fill (type background)))\n";
            out << "      )\n";

            // Symbol pins (unit 1, style 1)
            out << "      (symbol " << sq(fp_name + "_1_1") << "\n";
            for (auto& pin : sym.pins) {
                int angle = (pin.side == 0) ? 0 : 180;
                out << "        (pin " << pin.type << " line"
                    << " (at " << fmt(pin.x) << " " << fmt(pin.y) << " " << angle << ")"
                    << " (length " << fmt(PIN_LEN) << ")"
                    << " (name " << sq(pin.name) << " (effects (font (size 1.27 1.27))))"
                    << " (number " << sq(pin.name) << " (effects (font (size 1.27 1.27)))))\n";
            }
            out << "      )\n";

            out << "    )\n"; // end symbol
        }
    }

    // Write power symbol definitions
    for (auto& [lib_id, sym_text] : power_symbol_defs_) {
        out << "    " << sym_text << "\n";
    }

    out << "  )\n"; // end lib_symbols
}

void SchematicWriter::write_wires_and_labels(std::ostream& out, const PcbModel& model) {
    power_ports_.clear();
    int pwr_index = 1;

    // For each placed symbol, emit wire stubs and labels for each pin
    for (auto& inst : instances_) {
        auto& sym = symbol_defs_[inst.footprint_name];
        if (!inst.comp) continue;

        for (auto& pin : sym.pins) {
            // Pin endpoint in schematic coordinates
            double px = inst.x + pin.x;
            double py = inst.y + pin.y;

            // Wire stub extends outward from pin
            double stub_len = PIN_PITCH; // 2.54mm
            double wx, wy;
            if (pin.side == 0) {
                wx = px - stub_len;
                wy = py;
            } else {
                wx = px + stub_len;
                wy = py;
            }

            // Look up net for this pin
            auto net_it = inst.comp->pin_net_map.find(pin.name);
            bool has_net = (net_it != inst.comp->pin_net_map.end()
                           && !net_it->second.empty()
                           && net_it->second != "No Net");

            if (has_net) {
                const std::string& net_name = net_it->second;

                // Wire stub
                std::string wire_uuid = generate_uuid_from_seed(
                    "wire_" + inst.refdes + "_" + pin.name);
                out << "  (wire (pts (xy " << fmt(px) << " " << fmt(py) << ")"
                    << " (xy " << fmt(wx) << " " << fmt(wy) << "))\n"
                    << "    (stroke (width 0) (type default))\n"
                    << "    (uuid \"" << wire_uuid << "\"))\n";

                if (opts_.use_kicad_symbols && is_power_net(net_name)) {
                    // Place a power port symbol instead of a net label
                    std::string sym_name = power_net_symbol_name(net_name);
                    std::string lib_id = "power:" + sym_name;

                    // Determine orientation: GND points down, supplies point up
                    std::string upper = sym_name;
                    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
                    bool is_ground = (upper == "GND" || upper == "GNDA" || upper == "GNDD" ||
                                     upper == "VSS");

                    // Power port pin connects at (0,0) of the symbol.
                    // GND: pin points down (angle 270 in symbol def → place at wire end)
                    // +5V: pin points up (angle 90 in symbol def → place at wire end)
                    // The power symbol is placed AT the wire end point.
                    int angle = 0;
                    if (is_ground) {
                        // GND symbol should hang below the wire
                        // Pin direction is 270 (down), symbol placed at wire end
                        angle = 0; // no rotation needed, GND naturally points down
                    } else {
                        // Supply symbol points up naturally
                        angle = 0;
                    }

                    // For horizontal wires, we need to rotate the power symbol
                    // so it connects properly. Power pin is at (0,0).
                    // If wire goes left (pin.side == 0), power port needs rotation
                    // to connect the pin to the wire end.
                    // Actually: place the power port at the wire end point.
                    // Power symbols connect via their pin at local (0,0).
                    // GND pin: at (0 0 270) means pin exits downward
                    // +5V pin: at (0 0 90) means pin exits upward
                    // For a horizontal wire, we rotate the power symbol 90 degrees.
                    if (pin.side == 0) {
                        // Wire goes left; power port should point left
                        angle = is_ground ? 90 : 270;
                    } else {
                        // Wire goes right; power port should point right
                        angle = is_ground ? 270 : 90;
                    }

                    char refdes_buf[16];
                    snprintf(refdes_buf, sizeof(refdes_buf), "#PWR%02d", pwr_index++);

                    PowerPort pp;
                    pp.net_name = net_name;
                    pp.lib_id = lib_id;
                    pp.refdes = refdes_buf;
                    pp.x = wx;
                    pp.y = wy;
                    pp.angle = angle;
                    pp.uuid = generate_uuid_from_seed("pwr_" + inst.refdes + "_" + pin.name);
                    pp.pin_uuid = generate_uuid_from_seed("pwrpin_" + inst.refdes + "_" + pin.name);
                    power_ports_.push_back(pp);
                } else {
                    // Regular net label
                    int label_angle = (pin.side == 0) ? 180 : 0;
                    std::string label_uuid = generate_uuid_from_seed(
                        "label_" + inst.refdes + "_" + pin.name);
                    out << "  (label " << sq(net_name)
                        << " (at " << fmt(wx) << " " << fmt(wy) << " " << label_angle << ")\n"
                        << "    (effects (font (size 1.27 1.27)) (justify left))\n"
                        << "    (uuid \"" << label_uuid << "\"))\n";
                }
            } else {
                // No-connect marker at pin end
                std::string nc_uuid = generate_uuid_from_seed(
                    "nc_" + inst.refdes + "_" + pin.name);
                out << "  (no_connect (at " << fmt(px) << " " << fmt(py) << ")"
                    << " (uuid \"" << nc_uuid << "\"))\n";
            }
        }
    }

    // Write power port symbol instances
    for (auto& pp : power_ports_) {
        out << "  (symbol\n"
            << "    (lib_id " << sq(pp.lib_id) << ")\n"
            << "    (at " << fmt(pp.x) << " " << fmt(pp.y) << " " << pp.angle << ")\n"
            << "    (uuid \"" << pp.uuid << "\")\n";
        out << "    (property \"Reference\" " << sq(pp.refdes)
            << " (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n";
        out << "    (property \"Value\" " << sq(pp.net_name)
            << " (at 0 0 0) (effects (font (size 1.27 1.27))))\n";
        out << "    (property \"Footprint\" \"\""
            << " (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n";
        out << "    (pin \"1\" (uuid \"" << pp.pin_uuid << "\"))\n";
        out << "    (instances\n"
            << "      (project \"ipc2581\"\n"
            << "        (path \"/" << sheet_uuid_ << "\"\n"
            << "          (reference " << sq(pp.refdes) << ")"
            << " (unit 1))))\n";
        out << "  )\n";
    }

    if (!power_ports_.empty()) {
        log("Placed " + std::to_string(power_ports_.size()) + " power port symbols");
    }
}

void SchematicWriter::write_symbol_instances(std::ostream& out, const PcbModel& model) {
    for (auto& inst : instances_) {
        auto& sym = symbol_defs_[inst.footprint_name];
        std::string sym_uuid = generate_uuid_from_seed("sym_" + inst.refdes);
        std::string lib_id = sym.kicad_lib_id.empty()
            ? "ipc2581_auto:" + inst.footprint_name
            : sym.kicad_lib_id;

        out << "  (symbol\n"
            << "    (lib_id " << sq(lib_id) << ")\n"
            << "    (at " << fmt(inst.x) << " " << fmt(inst.y) << " 0)\n"
            << "    (uuid \"" << sym_uuid << "\")\n";

        // Properties
        out << "    (property \"Reference\" " << sq(inst.refdes)
            << " (at " << fmt(inst.x) << " "
            << fmt(inst.y - sym.body_height / 2.0 - 2.54) << " 0)"
            << " (effects (font (size 1.27 1.27))))\n";
        out << "    (property \"Value\" " << sq(inst.value)
            << " (at " << fmt(inst.x) << " "
            << fmt(inst.y + sym.body_height / 2.0 + 2.54) << " 0)"
            << " (effects (font (size 1.27 1.27))))\n";
        out << "    (property \"Footprint\" " << sq("ipc2581:" + inst.footprint_name)
            << " (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n";

        // BOM properties
        if (inst.comp && !inst.comp->description.empty()) {
            out << "    (property \"Description\" " << sq(inst.comp->description)
                << " (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n";
        }
        if (inst.comp && !inst.comp->part_number.empty()) {
            out << "    (property \"Part_Number\" " << sq(inst.comp->part_number)
                << " (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n";
        }

        // Pin UUIDs
        for (auto& pin : sym.pins) {
            std::string pin_uuid = generate_uuid_from_seed(
                "pin_" + inst.refdes + "_" + pin.name);
            out << "    (pin " << sq(pin.name)
                << " (uuid \"" << pin_uuid << "\"))\n";
        }

        // Instance annotation
        out << "    (instances\n"
            << "      (project \"ipc2581\"\n"
            << "        (path \"/" << sheet_uuid_ << "\"\n"
            << "          (reference " << sq(inst.refdes) << ")"
            << " (unit 1))))\n";

        out << "  )\n"; // end symbol
    }
}

void SchematicWriter::write_sheet_instances(std::ostream& out) {
    out << "  (sheet_instances\n"
        << "    (path \"/\" (page \"1\")))\n"
        << "  (embedded_fonts no)\n";
}

void SchematicWriter::log(const std::string& msg) {
    if (opts_.verbose) {
        std::cerr << "[schematic] " << msg << "\n";
    }
}

} // namespace ipc2kicad
