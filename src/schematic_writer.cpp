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

// Chain layout spacing
static const double CHAIN_SPACING = 20.32;   // ~20mm between chain components (on grid)
static const double ROW_SPACING = 25.4;      // ~25mm between rows
static const double BRANCH_SPACING = 12.7;   // ~12mm for vertical branches

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
                // Vertical 2-pin: pin endpoints in KiCad library (Y-up):
                //   Pin 1: (0, 3.81 270) -> schematic-relative: (0, -3.81)
                //   Pin 2: (0, -3.81 90) -> schematic-relative: (0, 3.81)
                sym.body_width = 2.54;
                sym.body_height = 7.62;
                sym.pins.push_back({"1", 0, -3.81, 0, "passive"}); // top
                sym.pins.push_back({"2", 0, 3.81, 0, "passive"});  // bottom
            } else if (sym.kicad_lib_id == "Device:D") {
                // Horizontal 2-pin: pin 1 (K) at left, pin 2 (A) at right
                // Library: pin 1 at (-3.81, 0), pin 2 at (3.81, 0)
                // Schematic-relative: same (X not flipped)
                sym.body_width = 7.62;
                sym.body_height = 2.54;
                sym.pins.push_back({"1", -3.81, 0, 0, "passive"});
                sym.pins.push_back({"2", 3.81, 0, 1, "passive"});
            } else if (sym.kicad_lib_id == "Connector:TestPoint") {
                // Single pin: library (0, 0 90) -> schematic-relative (0, 0)
                // TestPoint pin is at the symbol origin
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
                // Vertical connector: all pins on LEFT side, spaced 2.54mm apart
                // KiCad library: Conn_01x pins are at x=-3.81 (side=0, facing left)
                // Schematic pin endpoint at x = -5.08
                sym.body_width = 5.08;
                sym.body_height = (n - 1) * PIN_PITCH + 2 * PIN_PITCH;
                for (int i = 0; i < n; i++) {
                    PinDef pin;
                    pin.name = std::to_string(i + 1);
                    pin.x = -5.08;
                    pin.y = -((n - 1) * PIN_PITCH / 2.0) + i * PIN_PITCH;
                    pin.side = 0;
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
                    // Library Y-up: negative = below center
                    // Schematic Y-down: negate -> positive = below center
                    double lib_y = -(left_n - 1) * PIN_PITCH / 2.0 + li * PIN_PITCH;
                    sym.pins[i].x = -(half_w + PIN_LEN);
                    sym.pins[i].y = -lib_y; // negate for schematic-relative
                    li++;
                } else {
                    double lib_y = -(right_n - 1) * PIN_PITCH / 2.0 + ri * PIN_PITCH;
                    sym.pins[i].x = half_w + PIN_LEN;
                    sym.pins[i].y = -lib_y; // negate for schematic-relative
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
    chain_trees_.clear();
    wire_segments_.clear();
    junctions_.clear();
    placed_instances_.clear();
    net_components_.clear();

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

    // Phase 1: Connectivity analysis
    build_net_components_map();
    int hub_idx = find_hub_component();

    if (hub_idx >= 0) {
        // Phase 2: Chain extraction
        build_chain_trees(hub_idx);

        // Phase 3: Placement
        place_chains();

        // Phase 4: Wire computation
        compute_wires();

        log("Chain layout: " + std::to_string(placed_instances_.size()) + " of " +
            std::to_string(instances_.size()) + " components placed in chains");
    }

    // Phase 5: Fallback grid placement for unplaced components
    double grid_x = snap(30.48);
    // Place fallback below the chain layout
    double grid_y = snap(30.48);
    if (!placed_instances_.empty()) {
        // Find max Y of placed components
        double max_y = 0;
        for (int idx : placed_instances_) {
            auto& inst = instances_[idx];
            auto& sym = symbol_defs_[inst.footprint_name];
            double bottom = inst.y + sym.body_height / 2.0 + 10.0;
            if (bottom > max_y) max_y = bottom;
        }
        grid_y = snap(max_y + ROW_SPACING);
    }

    int col_count = 0;
    int max_per_col = 8;

    for (int i = 0; i < (int)instances_.size(); i++) {
        if (placed_instances_.count(i)) continue;

        auto& inst = instances_[i];
        auto& sym = symbol_defs_[inst.footprint_name];
        double cell_h = snap(sym.body_height + GRID_MARGIN);

        if (col_count >= max_per_col) {
            grid_x += GRID_CELL_W;
            grid_y = snap(placed_instances_.empty() ? 30.48 : grid_y);
            col_count = 0;
        }

        inst.x = snap(grid_x);
        inst.y = snap(grid_y + sym.body_height / 2.0);
        grid_y += cell_h;
        col_count++;
    }
}

// ── Pin position and rotation helpers ────────────────────────────────

std::pair<double, double> SchematicWriter::pin_schematic_pos(int inst_idx,
                                                              const std::string& pin_name) const {
    auto& inst = instances_[inst_idx];
    auto& sym = symbol_defs_.at(inst.footprint_name);
    double px = 0, py = 0;
    for (auto& pin : sym.pins) {
        if (pin.name == pin_name) { px = pin.x; py = pin.y; break; }
    }
    // Apply rotation (CW in Y-down coords)
    double rx = px, ry = py;
    switch (inst.rotation) {
        case 0:   rx = px;  ry = py;  break;
        case 90:  rx = py;  ry = -px; break;
        case 180: rx = -px; ry = -py; break;
        case 270: rx = -py; ry = px;  break;
    }
    return {inst.x + rx, inst.y + ry};
}

int SchematicWriter::rotation_for_pin_facing(int inst_idx, const std::string& pin_name,
                                              int direction) const {
    // direction: 0=left(-x), 1=right(+x), 2=up(-y), 3=down(+y)
    // Find the pin's local position in the symbol def
    auto& inst = instances_[inst_idx];
    auto& sym = symbol_defs_.at(inst.footprint_name);
    double px = 0, py = 0;
    for (auto& pin : sym.pins) {
        if (pin.name == pin_name) { px = pin.x; py = pin.y; break; }
    }
    // Try each rotation and see which puts the pin in the desired direction
    int rotations[] = {0, 90, 180, 270};
    for (int rot : rotations) {
        double rx = px, ry = py;
        switch (rot) {
            case 0:   rx = px;  ry = py;  break;
            case 90:  rx = py;  ry = -px; break;
            case 180: rx = -px; ry = -py; break;
            case 270: rx = -py; ry = px;  break;
        }
        bool match = false;
        switch (direction) {
            case 0: match = (rx < -0.1 && std::abs(ry) < 0.1); break; // left
            case 1: match = (rx > 0.1 && std::abs(ry) < 0.1);  break; // right
            case 2: match = (ry < -0.1 && std::abs(rx) < 0.1); break; // up
            case 3: match = (ry > 0.1 && std::abs(rx) < 0.1);  break; // down
        }
        if (match) return rot;
    }
    return 0; // fallback
}

// ── Connectivity analysis ───────────────────────────────────────────

void SchematicWriter::build_net_components_map() {
    net_components_.clear();
    for (int i = 0; i < (int)instances_.size(); i++) {
        auto& inst = instances_[i];
        if (!inst.comp) continue;
        for (auto& [pin_name, net_name] : inst.comp->pin_net_map) {
            if (net_name.empty() || net_name == "No Net") continue;
            if (is_power_net(net_name)) continue;
            net_components_[net_name].push_back({i, pin_name});
        }
    }
}

int SchematicWriter::find_hub_component() {
    // Score each component by number of distinct signal nets
    std::map<int, int> scores;
    for (auto& [net_name, comps] : net_components_) {
        std::set<int> unique_instances;
        for (auto& [idx, pin] : comps) unique_instances.insert(idx);
        for (int idx : unique_instances) scores[idx]++;
    }
    int best_idx = -1, best_score = 0;
    for (auto& [idx, score] : scores) {
        if (score > best_score) {
            best_score = score;
            best_idx = idx;
        }
    }
    if (best_idx >= 0) {
        log("Hub component: " + instances_[best_idx].refdes +
            " with " + std::to_string(best_score) + " signal nets");
    }
    return best_idx;
}

// ── Chain extraction (DFS from hub) ─────────────────────────────────

void SchematicWriter::build_chain_trees(int hub_idx) {
    chain_trees_.clear();
    placed_instances_.clear();
    placed_instances_.insert(hub_idx);

    auto& hub_inst = instances_[hub_idx];
    if (!hub_inst.comp) return;
    auto& hub_sym = symbol_defs_[hub_inst.footprint_name];

    // For each hub pin with a signal net, create a chain tree
    for (auto& pin : hub_sym.pins) {
        auto net_it = hub_inst.comp->pin_net_map.find(pin.name);
        if (net_it == hub_inst.comp->pin_net_map.end()) continue;
        const std::string& net_name = net_it->second;
        if (net_name.empty() || net_name == "No Net" || is_power_net(net_name)) continue;

        auto nc_it = net_components_.find(net_name);
        if (nc_it == net_components_.end()) continue;

        ChainTree tree;
        tree.hub_instance_idx = hub_idx;
        tree.hub_pin = pin.name;
        tree.net_name = net_name;

        // Find all unvisited components on this net
        for (auto& [comp_idx, comp_pin] : nc_it->second) {
            if (placed_instances_.count(comp_idx)) continue;
            placed_instances_.insert(comp_idx);

            ChainNode node = extend_chain(comp_idx, net_name, comp_pin);
            tree.roots.push_back(std::move(node));
        }

        if (!tree.roots.empty()) {
            // Reorder roots: deepest chain first (becomes main horizontal chain)
            // Shorter chains become vertical T-branches
            std::function<int(const ChainNode&)> chain_depth;
            chain_depth = [&](const ChainNode& n) -> int {
                int d = 1;
                for (auto& b : n.branches) d = std::max(d, 1 + chain_depth(b));
                return d;
            };
            std::sort(tree.roots.begin(), tree.roots.end(),
                [&](const ChainNode& a, const ChainNode& b) {
                    return chain_depth(a) > chain_depth(b);
                });

            chain_trees_.push_back(std::move(tree));
        }
    }
}

SchematicWriter::ChainNode SchematicWriter::extend_chain(int inst_idx,
                                                          const std::string& connecting_net,
                                                          const std::string& inward_pin) {
    ChainNode node;
    node.instance_idx = inst_idx;
    node.connecting_net = connecting_net;
    node.inward_pin = inward_pin;

    auto& inst = instances_[inst_idx];
    auto& sym = symbol_defs_[inst.footprint_name];

    // For 2-pin components, find the outward pin (the one that isn't inward)
    if (sym.pins.size() == 2) {
        for (auto& pin : sym.pins) {
            if (pin.name != inward_pin) {
                node.outward_pin = pin.name;
                break;
            }
        }

        // Follow outward pin's net to find next components
        if (!node.outward_pin.empty() && inst.comp) {
            auto net_it = inst.comp->pin_net_map.find(node.outward_pin);
            if (net_it != inst.comp->pin_net_map.end() &&
                !net_it->second.empty() && net_it->second != "No Net" &&
                !is_power_net(net_it->second)) {

                const std::string& next_net = net_it->second;
                auto nc_it = net_components_.find(next_net);
                if (nc_it != net_components_.end()) {
                    for (auto& [comp_idx, comp_pin] : nc_it->second) {
                        if (placed_instances_.count(comp_idx)) continue;
                        placed_instances_.insert(comp_idx);

                        ChainNode child = extend_chain(comp_idx, next_net, comp_pin);
                        node.branches.push_back(std::move(child));
                    }
                }
            }
        }
    }
    // Multi-pin non-hub and 1-pin components: leaf nodes, no extension

    return node;
}

// ── Chain placement ─────────────────────────────────────────────────

void SchematicWriter::place_chains() {
    if (chain_trees_.empty()) return;

    int hub_idx = chain_trees_[0].hub_instance_idx;
    auto& hub_inst = instances_[hub_idx];
    auto& hub_sym = symbol_defs_[hub_inst.footprint_name];

    // Count chain trees to determine layout spacing
    int num_chains = (int)chain_trees_.size();

    // Place chains in rows, each row has enough vertical space for branches
    // Calculate total height needed
    double row_start_y = snap(40.0);
    double current_row_y = row_start_y;

    // Calculate max branch depth for a chain tree (how far branches extend)
    std::function<int(const ChainNode&)> max_depth;
    max_depth = [&](const ChainNode& node) -> int {
        int d = 1;
        for (auto& b : node.branches) {
            d = std::max(d, 1 + max_depth(b));
        }
        return d;
    };

    // First pass: assign each chain a row Y position with adequate spacing
    std::vector<double> chain_row_y(num_chains);
    for (int ci = 0; ci < num_chains; ci++) {
        chain_row_y[ci] = snap(current_row_y);
        // Height: base + downward branch extent
        int max_branch_depth = 0;
        int branch_count = 0;
        for (auto& root : chain_trees_[ci].roots) {
            max_branch_depth = std::max(max_branch_depth, max_depth(root));
            branch_count += (int)root.branches.size();
        }
        // Simple chains (single node, no branches): just need basic spacing
        double row_height;
        if (branch_count == 0 && chain_trees_[ci].roots.size() <= 1) {
            row_height = 15.24; // compact for single-component chains
        } else {
            row_height = 12.7 + max_branch_depth * BRANCH_SPACING +
                         std::max(0, (int)chain_trees_[ci].roots.size() - 1) * BRANCH_SPACING;
        }
        current_row_y += row_height;
    }

    // Place hub to the right, vertically centered on the chains
    double hub_center_y = (chain_row_y.front() + chain_row_y.back()) / 2.0;
    double hub_x = snap(200.0);
    hub_inst.x = hub_x;
    hub_inst.y = snap(hub_center_y);
    hub_inst.rotation = 0;

    log("Placed hub " + hub_inst.refdes + " at (" + fmt(hub_inst.x) + ", " + fmt(hub_inst.y) + ")");

    // Place chains extending left from hub
    // For each chain, draw a wire from the hub pin to the row Y, then left to the chain
    for (int ci = 0; ci < num_chains; ci++) {
        auto& tree = chain_trees_[ci];
        auto [hpx, hpy] = pin_schematic_pos(hub_idx, tree.hub_pin);

        double chain_y = chain_row_y[ci];
        double chain_x = hpx - CHAIN_SPACING;

        // Wire from hub pin to chain row (L-shaped if different Y)
        // This will be handled by compute_wires through the root node positions

        for (size_t ri = 0; ri < tree.roots.size(); ri++) {
            auto& root = tree.roots[ri];
            if (ri == 0) {
                // Main root: place horizontally extending left at chain row Y
                place_node_horizontal(root, chain_x, chain_y, false);
            } else {
                // T-branch: always extend downward to avoid intruding into other chains
                double branch_y = chain_y + BRANCH_SPACING * ri;
                place_node_vertical(root, chain_x, branch_y, true);
            }
        }
    }
}

void SchematicWriter::place_node_horizontal(ChainNode& node, double x, double y,
                                             bool facing_right) {
    auto& inst = instances_[node.instance_idx];
    auto& sym = symbol_defs_[inst.footprint_name];

    // Determine rotation: inward pin should face right (toward hub) if !facing_right chain
    // The chain extends left, so the inward pin needs to face RIGHT (direction=1)
    int desired_dir = facing_right ? 0 : 1; // inward pin faces toward hub
    inst.rotation = rotation_for_pin_facing(node.instance_idx, node.inward_pin, desired_dir);
    inst.x = snap(x);
    inst.y = snap(y);

    log("  Placed " + inst.refdes + " at (" + fmt(inst.x) + ", " + fmt(inst.y) +
        ") rot=" + std::to_string(inst.rotation));

    // Continue chain from outward pin
    if (!node.outward_pin.empty() && !node.branches.empty()) {
        auto [opx, opy] = pin_schematic_pos(node.instance_idx, node.outward_pin);

        for (size_t bi = 0; bi < node.branches.size(); bi++) {
            auto& branch = node.branches[bi];
            if (bi == 0) {
                // Main continuation: horizontal
                double next_x = facing_right ? opx + CHAIN_SPACING : opx - CHAIN_SPACING;
                place_node_horizontal(branch, next_x, opy, facing_right);
            } else {
                // T-branch: always extend downward
                double branch_y = opy + BRANCH_SPACING * bi;
                place_node_vertical(branch, opx, branch_y, true);
            }
        }
    }
}

void SchematicWriter::place_node_vertical(ChainNode& node, double x, double y,
                                           bool facing_down) {
    auto& inst = instances_[node.instance_idx];

    // Inward pin should face UP (toward junction) if facing_down, or DOWN if facing_up
    int desired_dir = facing_down ? 2 : 3; // 2=up, 3=down
    inst.rotation = rotation_for_pin_facing(node.instance_idx, node.inward_pin, desired_dir);
    inst.x = snap(x);
    inst.y = snap(y);

    log("  Placed " + inst.refdes + " (vert) at (" + fmt(inst.x) + ", " + fmt(inst.y) +
        ") rot=" + std::to_string(inst.rotation));

    // Continue chain vertically
    if (!node.outward_pin.empty() && !node.branches.empty()) {
        auto [opx, opy] = pin_schematic_pos(node.instance_idx, node.outward_pin);

        for (size_t bi = 0; bi < node.branches.size(); bi++) {
            auto& branch = node.branches[bi];
            double next_y = facing_down ? opy + BRANCH_SPACING : opy - BRANCH_SPACING;
            place_node_vertical(branch, opx, next_y, facing_down);
        }
    }
}

// ── Wire computation ────────────────────────────────────────────────

void SchematicWriter::compute_wires() {
    wire_segments_.clear();
    junctions_.clear();

    for (auto& tree : chain_trees_) {
        // Wire from hub pin to each root
        auto [hpx, hpy] = pin_schematic_pos(tree.hub_instance_idx, tree.hub_pin);

        for (size_t ri = 0; ri < tree.roots.size(); ri++) {
            auto& root = tree.roots[ri];
            auto [rpx, rpy] = pin_schematic_pos(root.instance_idx, root.inward_pin);

            // Draw wire from hub pin to root's inward pin
            draw_routed_wire(hpx, hpy, rpx, rpy);

            // If multiple roots on same net, add junction at hub pin
            if (tree.roots.size() > 1 && ri > 0) {
                junctions_.push_back({hpx, hpy});
            }

            // Recursively draw wires within the chain
            draw_chain_wires(root, rpx, rpy);
        }
    }
}

void SchematicWriter::draw_chain_wires(const ChainNode& node,
                                        double parent_px, double parent_py) {
    if (node.outward_pin.empty() || node.branches.empty()) return;

    auto [opx, opy] = pin_schematic_pos(node.instance_idx, node.outward_pin);

    for (size_t bi = 0; bi < node.branches.size(); bi++) {
        auto& branch = node.branches[bi];
        auto [bpx, bpy] = pin_schematic_pos(branch.instance_idx, branch.inward_pin);

        // Wire from this node's outward pin to branch's inward pin
        draw_routed_wire(opx, opy, bpx, bpy);

        // Junction if multiple branches
        if (node.branches.size() > 1 && bi > 0) {
            junctions_.push_back({opx, opy});
        }

        // Recurse
        draw_chain_wires(branch, bpx, bpy);
    }
}

void SchematicWriter::draw_routed_wire(double x1, double y1, double x2, double y2) {
    x1 = snap(x1); y1 = snap(y1);
    x2 = snap(x2); y2 = snap(y2);

    if (std::abs(y1 - y2) < 0.01) {
        // Horizontal wire - direct
        wire_segments_.push_back({x1, y1, x2, y2});
    } else if (std::abs(x1 - x2) < 0.01) {
        // Vertical wire - direct
        wire_segments_.push_back({x1, y1, x2, y2});
    } else {
        // L-shaped wire: horizontal first, then vertical
        wire_segments_.push_back({x1, y1, x2, y1});
        wire_segments_.push_back({x2, y1, x2, y2});
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
            // PinDef stores schematic-relative coords; negate Y for library Y-up
            out << "      (symbol " << sq(fp_name + "_1_1") << "\n";
            for (auto& pin : sym.pins) {
                int angle = (pin.side == 0) ? 0 : 180;
                out << "        (pin " << pin.type << " line"
                    << " (at " << fmt(pin.x) << " " << fmt(-pin.y) << " " << angle << ")"
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

    // ── Emit pre-computed wires from chain layout ──
    for (auto& wire : wire_segments_) {
        std::string wire_uuid = generate_uuid_from_seed(
            "cwire_" + fmt(wire.x1) + "_" + fmt(wire.y1) + "_" +
            fmt(wire.x2) + "_" + fmt(wire.y2));
        out << "  (wire (pts (xy " << fmt(wire.x1) << " " << fmt(wire.y1) << ")"
            << " (xy " << fmt(wire.x2) << " " << fmt(wire.y2) << "))\n"
            << "    (stroke (width 0) (type default))\n"
            << "    (uuid \"" << wire_uuid << "\"))\n";
    }

    // ── Emit junctions ──
    for (auto& jct : junctions_) {
        std::string jct_uuid = generate_uuid_from_seed(
            "jct_" + fmt(jct.x) + "_" + fmt(jct.y));
        out << "  (junction (at " << fmt(jct.x) << " " << fmt(jct.y) << ")"
            << " (diameter 0) (color 0 0 0 0)\n"
            << "    (uuid \"" << jct_uuid << "\"))\n";
    }

    // ── For chain-placed components: emit power ports at chain endpoints ──
    // Also handle pins that connect to power nets (wire stub + power port)
    // And pins with no chain wire (need net labels as fallback)
    std::set<std::string> chain_wired_pins; // "refdes:pin" pairs that have chain wires

    // Build set of pins connected by chain wires
    for (auto& tree : chain_trees_) {
        // Hub pin -> root connections
        for (auto& root : tree.roots) {
            chain_wired_pins.insert(instances_[tree.hub_instance_idx].refdes + ":" + tree.hub_pin);
            chain_wired_pins.insert(instances_[root.instance_idx].refdes + ":" + root.inward_pin);
        }
        // Recursively mark wired pins in branches
        std::function<void(const ChainNode&)> mark_wired;
        mark_wired = [&](const ChainNode& node) {
            for (auto& branch : node.branches) {
                if (!node.outward_pin.empty()) {
                    chain_wired_pins.insert(instances_[node.instance_idx].refdes + ":" + node.outward_pin);
                }
                chain_wired_pins.insert(instances_[branch.instance_idx].refdes + ":" + branch.inward_pin);
                mark_wired(branch);
            }
        };
        for (auto& root : tree.roots) mark_wired(root);
    }

    // ── For ALL components: handle pins not covered by chain wires ──
    for (int i = 0; i < (int)instances_.size(); i++) {
        auto& inst = instances_[i];
        auto& sym = symbol_defs_[inst.footprint_name];
        if (!inst.comp) continue;

        for (auto& pin : sym.pins) {
            std::string pin_key = inst.refdes + ":" + pin.name;

            // Look up net for this pin
            auto net_it = inst.comp->pin_net_map.find(pin.name);
            bool has_net = (net_it != inst.comp->pin_net_map.end()
                           && !net_it->second.empty()
                           && net_it->second != "No Net");

            if (!has_net) {
                // No-connect marker at pin position
                auto [px, py] = pin_schematic_pos(i, pin.name);
                std::string nc_uuid = generate_uuid_from_seed(
                    "nc_" + inst.refdes + "_" + pin.name);
                out << "  (no_connect (at " << fmt(px) << " " << fmt(py) << ")"
                    << " (uuid \"" << nc_uuid << "\"))\n";
                continue;
            }

            const std::string& net_name = net_it->second;

            // Pin endpoint in schematic coordinates
            auto [px, py] = pin_schematic_pos(i, pin.name);

            if (is_power_net(net_name) && opts_.use_kicad_symbols) {
                // Power pin: always emit wire stub + power port
                // Determine stub direction from pin's rotated position
                double dx = px - inst.x;
                double dy = py - inst.y;
                double stub_len = PIN_PITCH;
                double wx = px, wy = py;

                // Stub extends away from component center
                if (std::abs(dx) > std::abs(dy)) {
                    wx = px + (dx > 0 ? stub_len : -stub_len);
                } else {
                    wy = py + (dy > 0 ? stub_len : -stub_len);
                }

                // Wire stub
                std::string wire_uuid = generate_uuid_from_seed(
                    "wire_" + inst.refdes + "_" + pin.name);
                out << "  (wire (pts (xy " << fmt(px) << " " << fmt(py) << ")"
                    << " (xy " << fmt(wx) << " " << fmt(wy) << "))\n"
                    << "    (stroke (width 0) (type default))\n"
                    << "    (uuid \"" << wire_uuid << "\"))\n";

                // Power port symbol
                std::string sym_name = power_net_symbol_name(net_name);
                std::string lib_id = "power:" + sym_name;
                std::string upper = sym_name;
                std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
                bool is_ground = (upper == "GND" || upper == "GNDA" || upper == "GNDD" ||
                                 upper == "VSS");

                // Determine power port angle based on stub direction
                int angle = 0;
                double sdx = wx - px, sdy = wy - py;
                if (std::abs(sdx) > std::abs(sdy)) {
                    // Horizontal stub
                    if (sdx < 0) {
                        angle = is_ground ? 90 : 270;
                    } else {
                        angle = is_ground ? 270 : 90;
                    }
                } else {
                    // Vertical stub
                    if (sdy < 0) {
                        angle = is_ground ? 180 : 0;
                    } else {
                        angle = is_ground ? 0 : 180;
                    }
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

            } else if (chain_wired_pins.count(pin_key)) {
                // This pin is connected by a chain wire - no label needed
                continue;

            } else {
                // Fallback: wire stub + net label (for unplaced components or
                // inter-chain connections)
                double dx = px - inst.x;
                double dy = py - inst.y;
                double stub_len = PIN_PITCH;
                double wx = px, wy = py;
                int label_angle = 0;

                if (std::abs(dx) > std::abs(dy)) {
                    wx = px + (dx > 0 ? stub_len : -stub_len);
                    label_angle = (dx > 0) ? 0 : 180;
                } else {
                    wy = py + (dy > 0 ? stub_len : -stub_len);
                    label_angle = (dy > 0) ? 270 : 90;
                }

                // Wire stub
                std::string wire_uuid = generate_uuid_from_seed(
                    "wire_" + inst.refdes + "_" + pin.name);
                out << "  (wire (pts (xy " << fmt(px) << " " << fmt(py) << ")"
                    << " (xy " << fmt(wx) << " " << fmt(wy) << "))\n"
                    << "    (stroke (width 0) (type default))\n"
                    << "    (uuid \"" << wire_uuid << "\"))\n";

                // Net label
                std::string label_uuid = generate_uuid_from_seed(
                    "label_" + inst.refdes + "_" + pin.name);
                out << "  (label " << sq(net_name)
                    << " (at " << fmt(wx) << " " << fmt(wy) << " " << label_angle << ")\n"
                    << "    (effects (font (size 1.27 1.27)) (justify left))\n"
                    << "    (uuid \"" << label_uuid << "\"))\n";
            }
        }
    }

    // ── Write power port symbol instances ──
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
            << "    (at " << fmt(inst.x) << " " << fmt(inst.y) << " " << inst.rotation << ")\n"
            << "    (uuid \"" << sym_uuid << "\")\n";

        // Properties - position them relative to symbol, adjusted for rotation
        double ref_dx = 0, ref_dy = -(sym.body_height / 2.0 + 2.54);
        double val_dx = 0, val_dy = (sym.body_height / 2.0 + 2.54);
        // For rotated symbols, swap property offsets
        if (inst.rotation == 90 || inst.rotation == 270) {
            ref_dx = -(sym.body_height / 2.0 + 2.54);
            ref_dy = 0;
            val_dx = (sym.body_height / 2.0 + 2.54);
            val_dy = 0;
        }
        out << "    (property \"Reference\" " << sq(inst.refdes)
            << " (at " << fmt(inst.x + ref_dx) << " "
            << fmt(inst.y + ref_dy) << " 0)"
            << " (effects (font (size 1.27 1.27))))\n";
        out << "    (property \"Value\" " << sq(inst.value)
            << " (at " << fmt(inst.x + val_dx) << " "
            << fmt(inst.y + val_dy) << " 0)"
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
