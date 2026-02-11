#pragma once

#include "geometry.h"
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace ipc2kicad {

struct PadDef {
    std::string name;       // pad number/name (e.g., "1", "A1")
    enum Shape { CIRCLE, RECT, OVAL, ROUNDRECT, TRAPEZOID, CUSTOM };
    Shape shape = RECT;
    double width = 0.0;     // X size
    double height = 0.0;    // Y size
    double drill_diameter = 0.0;  // 0 for SMD
    Point offset;           // offset from pad center
    double roundrect_ratio = 0.25;
    std::vector<Point> custom_shape; // for arbitrary polygon pads
    enum Type { SMD, THRU_HOLE, NPTH };
    Type type = SMD;
    std::string layer_side; // "TOP", "BOTTOM", or "ALL" for through-hole
    double rotation = 0.0;  // pad rotation in degrees
    double solder_mask_margin = 0.0; // >0 means explicit mask expansion
};

struct PadStackDef {
    std::string name;
    std::vector<PadDef> pads;  // per-layer pad definitions
    double drill_diameter = 0.0;
    bool plated = true;
};

struct GraphicItem {
    enum Kind { LINE, ARC, CIRCLE, RECT, POLYGON, TEXT };
    Kind kind = LINE;
    Point start, end, center;
    double radius = 0.0;
    double width = 0.1;     // line width
    std::string layer;
    bool fill = false;
    std::vector<Point> points; // for polygon
    double sweep_angle = 0.0;  // for arc (degrees)
    // Text fields
    std::string text;
    double text_size = 1.0;
    double text_thickness = 0.15;
};

struct Footprint {
    std::string name;
    std::vector<PadDef> pads;
    std::vector<GraphicItem> graphics; // courtyard, silkscreen, fab
    Point origin;
    // Map from pad name to pad stack definition name
    std::map<std::string, std::string> pad_to_padstack;

    // 3D body data (from IPC-2581 Package element)
    double pkg_height = 0.0;             // component height in mm
    std::vector<Point> body_outline;     // 2D body polygon in IPC coords (Y-up, mm)
};

struct ComponentInstance {
    std::string refdes;
    std::string footprint_ref;   // reference to Footprint.name
    std::string value;
    std::string description;     // from BOM description
    std::string part_number;     // from BOM OEMDesignNumberRef (after colon)
    Point position;
    double rotation = 0.0;
    bool mirror = false;   // bottom side placement
    // Pin-to-net mapping: pad_name -> net_name
    std::map<std::string, std::string> pin_net_map;
    // Per-pin local rotation (footprint-relative): pad_name -> degrees
    std::map<std::string, double> pin_rotation_map;
    // Per-instance graphics (footprint-local coords, attached from board-level)
    std::vector<GraphicItem> instance_graphics;
};

struct TraceSegment {
    Point start, end;
    double width = 0.25;
    std::string layer;
    int net_id = 0;
};

struct TraceArc {
    Point start, mid, end;
    double width = 0.25;
    std::string layer;
    int net_id = 0;
};

struct Via {
    Point position;
    double diameter = 0.8;
    double drill = 0.4;
    std::string start_layer;
    std::string end_layer;
    int net_id = 0;
};

struct Zone {
    std::string layer;
    int net_id = 0;
    std::string net_name;
    std::vector<Point> outline;
    std::vector<std::vector<Point>> holes;
    // Fill settings
    double min_thickness = 0.25;
    double clearance = 0.5;
    enum FillType { SOLID, HATCHED };
    FillType fill_type = SOLID;
};

struct DrillHole {
    Point position;
    double diameter = 0.0;
    bool plated = true;
};

struct LayerDef {
    int kicad_id = 0;
    std::string kicad_name;
    std::string type;         // "signal", "user", "power"
    std::string ipc_name;     // original IPC-2581 layer name
    std::string ipc_function; // layerFunction attribute
    std::string ipc_side;     // side: TOP, BOTTOM, INTERNAL, ALL
    int copper_order = -1;    // copper layer order (0 = top, last = bottom)
};

struct NetDef {
    int id = 0;
    std::string name;
};

struct StackupLayer {
    std::string name;
    std::string type;       // "copper", "dielectric", "soldermask", "silkscreen", etc.
    double thickness = 0.0; // mm
    std::string material;
    double epsilon_r = 4.5;
    int kicad_layer_id = -1; // for copper layers
};

struct Stackup {
    std::vector<StackupLayer> layers;
    double board_thickness = 1.6; // mm
};

struct PcbModel {
    // Global
    std::vector<LayerDef> layers;
    std::vector<NetDef> nets;
    Stackup stackup;

    // Board outline
    std::vector<Segment> outline;
    std::vector<ArcGeom> outline_arcs;

    // Footprint definitions (templates)
    std::map<std::string, Footprint> footprint_defs;

    // Component instances (placed components)
    std::vector<ComponentInstance> components;

    // Routing
    std::vector<TraceSegment> traces;
    std::vector<TraceArc> trace_arcs;
    std::vector<Via> vias;

    // Zones
    std::vector<Zone> zones;

    // Drills
    std::vector<DrillHole> drills;

    // Graphics (silkscreen, fab, document, etc. on board level)
    std::vector<GraphicItem> graphics;

    // Pad stack dictionary (from IPC-2581 Dictionary entries)
    std::map<std::string, PadStackDef> padstack_defs;

    // Utility lookups
    std::map<std::string, int> net_name_to_id;
    std::map<std::string, std::string> ipc_layer_to_kicad;

    int get_net_id(const std::string& net_name) const {
        auto it = net_name_to_id.find(net_name);
        return (it != net_name_to_id.end()) ? it->second : 0;
    }

    std::string get_kicad_layer(const std::string& ipc_layer) const {
        auto it = ipc_layer_to_kicad.find(ipc_layer);
        return (it != ipc_layer_to_kicad.end()) ? it->second : "";
    }
};

} // namespace ipc2kicad
