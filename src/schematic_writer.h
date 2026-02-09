#pragma once

#include "pcb_model.h"
#include <string>
#include <ostream>
#include <vector>
#include <map>
#include <set>

namespace ipc2kicad {

struct SchematicWriterOptions {
    bool verbose = false;
    std::string paper_size; // empty = auto-select
    bool use_kicad_symbols = false;
    std::string kicad_symbol_dir; // path to .kicad_sym files (auto-detected if empty)
};

class SchematicWriter {
public:
    explicit SchematicWriter(const SchematicWriterOptions& opts = {});

    bool write(const std::string& filename, const PcbModel& model);
    bool write(std::ostream& out, const PcbModel& model);

private:
    struct PinDef {
        std::string name;
        double x = 0, y = 0;    // schematic-relative offset from symbol center (Y-down)
        int side = 0;            // 0=left, 1=right
        std::string type;        // "passive", "unspecified", etc.
    };

    struct SymbolDef {
        std::string footprint_name;
        std::string ref_prefix;   // "R", "C", "U", etc.
        double body_width = 5.08;
        double body_height = 5.08;
        std::vector<PinDef> pins;
        // When using KiCad library symbols:
        std::string kicad_lib_id;      // e.g. "Device:R"
        std::string kicad_symbol_text;  // raw symbol s-expression from .kicad_sym file
    };

    struct SymbolInstance {
        std::string refdes;
        std::string value;
        std::string footprint_name;
        double x = 0, y = 0;
        int rotation = 0;  // 0, 90, 180, 270 degrees (CW in KiCad Y-down)
        const ComponentInstance* comp = nullptr;
    };

    // Chain-based layout data structures
    struct ChainNode {
        int instance_idx = -1;
        std::string connecting_net;   // net connecting to parent
        std::string inward_pin;       // pin facing toward hub/parent
        std::string outward_pin;      // pin continuing chain (2-pin comps)
        std::vector<ChainNode> branches; // T-branches from junction
    };

    struct ChainTree {
        int hub_instance_idx = -1;
        std::string hub_pin;          // which hub pin starts this chain
        std::string net_name;         // net on this hub pin
        std::vector<ChainNode> roots; // components on this net
    };

    struct WireSegment {
        double x1, y1, x2, y2;
    };

    struct JunctionPoint {
        double x, y;
    };

    // Power port symbol placed at a wire stub end
    struct PowerPort {
        std::string net_name;    // e.g. "GND", "+5V"
        std::string lib_id;      // e.g. "power:GND"
        std::string refdes;      // e.g. "#PWR01"
        double x = 0, y = 0;
        int angle = 0;           // rotation in degrees (0=up, 90=left, 180=down, 270=right)
        std::string uuid;
        std::string pin_uuid;
    };

    SchematicWriterOptions opts_;
    std::string sheet_uuid_;
    std::map<std::string, SymbolDef> symbol_defs_; // keyed by footprint name
    std::vector<SymbolInstance> instances_;
    // Cache of loaded library files: filename -> full file content
    std::map<std::string, std::string> lib_file_cache_;
    // Power port instances to emit
    std::vector<PowerPort> power_ports_;
    // Power symbol definitions loaded from library (lib_id -> symbol text)
    std::map<std::string, std::string> power_symbol_defs_;

    // Chain-based layout state
    std::vector<ChainTree> chain_trees_;
    std::vector<WireSegment> wire_segments_;
    std::vector<JunctionPoint> junctions_;
    std::set<int> placed_instances_;  // indices into instances_
    // net_name -> [(instance_idx, pin_name)] for signal nets
    std::map<std::string, std::vector<std::pair<int, std::string>>> net_components_;

    void build_symbol_defs(const PcbModel& model);
    void layout_components(const PcbModel& model);
    std::string select_paper(int count) const;
    std::string ref_prefix(const std::string& refdes) const;

    // KiCad library symbol support
    std::string detect_symbol_dir() const;
    std::string load_kicad_symbol(const std::string& lib_file, const std::string& symbol_name);
    std::string map_to_kicad_symbol(const std::string& ref_prefix, int pin_count,
                                     const Footprint& fp,
                                     std::string& out_lib_file, std::string& out_symbol_name) const;

    // Power net detection
    bool is_power_net(const std::string& net_name) const;
    std::string power_net_symbol_name(const std::string& net_name) const;

    // Chain-based layout
    int find_hub_component();
    void build_net_components_map();
    void build_chain_trees(int hub_idx);
    ChainNode extend_chain(int inst_idx, const std::string& connecting_net,
                           const std::string& inward_pin);
    void place_chains();
    void place_node_horizontal(ChainNode& node, double x, double y, bool facing_right);
    void place_node_vertical(ChainNode& node, double x, double y, bool facing_down);

    // Wire routing
    void compute_wires();
    void draw_chain_wires(const ChainNode& node, double parent_px, double parent_py);
    void draw_routed_wire(double x1, double y1, double x2, double y2);

    // Pin position helpers
    std::pair<double, double> pin_schematic_pos(int inst_idx, const std::string& pin_name) const;
    int rotation_for_pin_facing(int inst_idx, const std::string& pin_name,
                                 int direction) const; // direction: 0=left,1=right,2=up,3=down

    void write_header(std::ostream& out, const std::string& paper);
    void write_lib_symbols(std::ostream& out);
    void write_symbol_instances(std::ostream& out, const PcbModel& model);
    void write_wires_and_labels(std::ostream& out, const PcbModel& model);
    void write_sheet_instances(std::ostream& out);

    void log(const std::string& msg);
};

} // namespace ipc2kicad
