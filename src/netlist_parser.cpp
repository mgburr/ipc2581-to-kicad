#include "netlist_parser.h"
#include "geometry.h"
#include "utils.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <set>
#include <queue>

namespace ipc2kicad {

NetlistParser::NetlistParser(bool verbose) : verbose_(verbose) {}

static std::string safe_substr(const std::string& s, size_t pos, size_t len) {
    if (pos >= s.size()) return "";
    return s.substr(pos, std::min(len, s.size() - pos));
}

bool NetlistParser::parse(const std::string& filepath, PcbModel& model) {
    std::ifstream f(filepath);
    if (!f.is_open()) {
        warn("Cannot open netlist file: " + filepath);
        return false;
    }

    log("Parsing IPC-D-356 netlist: " + filepath);

    std::vector<NetlistRecord> records;

    // Units: default mils (0.001 inch)
    double unit_scale = 0.0254;  // mils to mm

    std::string line;
    while (std::getline(f, line)) {
        // Trim trailing CR/whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();

        if (line.size() < 3) continue;

        // Units specification
        if (line.substr(0, 6) == "UNITS ") {
            std::string units_str = line.substr(6);
            // "CUST 0" = customary units with 0 = mils
            // "CUST 1" = mm
            // "CUST 2" = inches
            // Also: "SI" = mm, "CUST" alone = mils
            std::string lower = units_str;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

            if (lower.find("si") != std::string::npos ||
                (lower.find("cust") != std::string::npos && lower.find("1") != std::string::npos)) {
                unit_scale = 1.0;  // already mm
            } else if (lower.find("cust") != std::string::npos && lower.find("2") != std::string::npos) {
                unit_scale = 25.4; // inches to mm
            } else {
                unit_scale = 0.0254; // mils to mm (default)
            }

            if (verbose_) {
                log("  Units scale: " + std::to_string(unit_scale) + " mm/unit");
            }
            continue;
        }

        // Test records: 327 (SMD) or 317 (through-hole)
        std::string code = line.substr(0, 3);
        if (code != "327" && code != "317") continue;

        // Fixed-width field extraction:
        // Cols 4-17 (14 chars): net name
        // Cols 21-26 (6 chars): component refdes
        // Cols 27-30 (4 chars): pin number
        // Cols 33-36 (4 chars): mid pad (optional)
        // Cols 37-42 (6 chars): X coordinate
        // Cols 43-48 (6 chars): Y coordinate
        // Cols 49-52 (4 chars): hole diameter (optional)
        // Cols 53-57 (5 chars): access (optional)

        NetlistRecord rec;
        rec.net_name = trim(safe_substr(line, 3, 14));
        rec.component_ref = trim(safe_substr(line, 20, 6));
        rec.pin = trim(safe_substr(line, 26, 4));

        // Remove trailing '-' or padding from fields
        while (!rec.component_ref.empty() && rec.component_ref.back() == '-')
            rec.component_ref.pop_back();
        while (!rec.pin.empty() && rec.pin.back() == '-')
            rec.pin.pop_back();

        // Coordinates: may have sign prefix
        std::string x_str = trim(safe_substr(line, 36, 6));
        std::string y_str = trim(safe_substr(line, 42, 6));

        // Check for sign in preceding column
        if (line.size() > 35 && (line[35] == '+' || line[35] == '-')) {
            x_str = line[35] + x_str;
        }
        if (line.size() > 41 && (line[41] == '+' || line[41] == '-')) {
            y_str = line[41] + y_str;
        }

        double x = 0.0, y = 0.0;
        try {
            if (!x_str.empty()) x = std::stod(x_str) * unit_scale;
            if (!y_str.empty()) y = std::stod(y_str) * unit_scale;
        } catch (...) {
            continue;  // skip malformed records
        }

        // Gerber/IPC-D-356 Y-up -> KiCad Y-down
        rec.position = {x, -y};

        // Hole diameter
        std::string hole_str = trim(safe_substr(line, 48, 4));
        rec.hole_diameter = 0.0;
        try {
            if (!hole_str.empty()) rec.hole_diameter = std::stod(hole_str) * unit_scale;
        } catch (...) {}

        records.push_back(rec);
    }

    log("Read " + std::to_string(records.size()) + " netlist records");

    if (!records.empty()) {
        apply_to_model(records, model);
    }

    return true;
}

void NetlistParser::apply_to_model(const std::vector<NetlistRecord>& records,
                                    PcbModel& model) {
    // Step 1: Add all unique net names to model
    std::set<std::string> net_names;
    for (auto& rec : records) {
        if (!rec.net_name.empty()) {
            net_names.insert(rec.net_name);
        }
    }

    for (auto& name : net_names) {
        if (model.net_name_to_id.count(name)) continue;
        int id = (int)model.nets.size();
        NetDef nd;
        nd.id = id;
        nd.name = name;
        model.nets.push_back(nd);
        model.net_name_to_id[name] = id;
    }

    int via_assigned = 0;
    int trace_assigned = 0;

    // Step 2: Match records to vias by position
    const double tolerance = 0.01; // mm
    for (auto& rec : records) {
        if (rec.net_name.empty()) continue;
        int net_id = model.get_net_id(rec.net_name);
        if (net_id == 0) continue;

        // Match to vias
        for (auto& via : model.vias) {
            if (via.net_id != 0) continue; // already assigned
            if (distance(via.position, rec.position) < tolerance) {
                via.net_id = net_id;
                via_assigned++;
                break;
            }
        }

        // Match to trace endpoints
        for (auto& trace : model.traces) {
            if (trace.net_id != 0) continue;
            if (distance(trace.start, rec.position) < tolerance ||
                distance(trace.end, rec.position) < tolerance) {
                trace.net_id = net_id;
                trace_assigned++;
            }
        }
    }

    log("Direct assignment: " + std::to_string(via_assigned) + " vias, " +
        std::to_string(trace_assigned) + " traces");

    // Step 3: BFS net propagation through connected copper
    // Build adjacency: trace endpoints and via positions
    bool changed = true;
    int propagation_rounds = 0;
    while (changed) {
        changed = false;
        propagation_rounds++;

        // Propagate from assigned traces to connected traces/vias
        for (auto& t : model.traces) {
            if (t.net_id == 0) continue;

            // Find connected unassigned traces
            for (auto& t2 : model.traces) {
                if (t2.net_id != 0) continue;
                if (t2.layer != t.layer) continue;
                if (distance(t.start, t2.start) < tolerance ||
                    distance(t.start, t2.end) < tolerance ||
                    distance(t.end, t2.start) < tolerance ||
                    distance(t.end, t2.end) < tolerance) {
                    t2.net_id = t.net_id;
                    changed = true;
                }
            }

            // Find connected unassigned vias
            for (auto& v : model.vias) {
                if (v.net_id != 0) continue;
                if (distance(t.start, v.position) < tolerance ||
                    distance(t.end, v.position) < tolerance) {
                    v.net_id = t.net_id;
                    changed = true;
                }
            }
        }

        // Propagate from assigned vias to connected traces
        for (auto& v : model.vias) {
            if (v.net_id == 0) continue;
            for (auto& t : model.traces) {
                if (t.net_id != 0) continue;
                if (distance(t.start, v.position) < tolerance ||
                    distance(t.end, v.position) < tolerance) {
                    t.net_id = v.net_id;
                    changed = true;
                }
            }
        }

        // Also propagate through trace arcs
        for (auto& ta : model.trace_arcs) {
            if (ta.net_id != 0) {
                for (auto& t : model.traces) {
                    if (t.net_id != 0) continue;
                    if (t.layer != ta.layer) continue;
                    if (distance(t.start, ta.start) < tolerance ||
                        distance(t.start, ta.end) < tolerance ||
                        distance(t.end, ta.start) < tolerance ||
                        distance(t.end, ta.end) < tolerance) {
                        t.net_id = ta.net_id;
                        changed = true;
                    }
                }
                for (auto& v : model.vias) {
                    if (v.net_id != 0) continue;
                    if (distance(ta.start, v.position) < tolerance ||
                        distance(ta.end, v.position) < tolerance) {
                        v.net_id = ta.net_id;
                        changed = true;
                    }
                }
            }
        }

        // Safety limit
        if (propagation_rounds > 100) break;
    }

    log("Net propagation completed in " + std::to_string(propagation_rounds) + " rounds");
}

void NetlistParser::log(const std::string& msg) {
    if (verbose_) std::cerr << "NetlistParser: " << msg << "\n";
}

void NetlistParser::warn(const std::string& msg) {
    warnings_.push_back(msg);
    std::cerr << "Warning: " << msg << "\n";
}

} // namespace ipc2kicad
