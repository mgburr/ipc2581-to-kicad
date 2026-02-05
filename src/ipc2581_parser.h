#pragma once

#include "pcb_model.h"
#include <string>
#include <vector>
#include <map>
#include <functional>

// Forward declare pugixml types
namespace pugi {
class xml_node;
class xml_document;
}

namespace ipc2kicad {

struct ParserOptions {
    std::string step_name;    // which Step to parse (empty = first)
    bool verbose = false;
    bool list_steps = false;
    bool list_layers = false;
};

class Ipc2581Parser {
public:
    explicit Ipc2581Parser(const ParserOptions& opts = {});

    // Parse an IPC-2581 XML file. Returns true on success.
    bool parse(const std::string& filename, PcbModel& model);

    // List available steps in the file
    std::vector<std::string> list_steps(const std::string& filename);

    // Get any parse errors/warnings
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    ParserOptions opts_;
    std::vector<std::string> warnings_;
    double unit_scale_ = 1.0; // multiplier to convert to mm

    // Sub-parsers
    void parse_units(const pugi::xml_node& cad_header);
    void parse_layers(const pugi::xml_node& cad_data, PcbModel& model);
    void parse_stackup(const pugi::xml_node& cad_data, PcbModel& model);
    void parse_dictionaries(const pugi::xml_node& content, PcbModel& model);
    void parse_dictionary_entry(const pugi::xml_node& entry, PcbModel& model);
    void parse_nets(const pugi::xml_node& root, PcbModel& model);
    void parse_profile(const pugi::xml_node& step, PcbModel& model);
    void parse_packages(const pugi::xml_node& step, PcbModel& model);
    void parse_components(const pugi::xml_node& step, PcbModel& model);
    void parse_layer_features(const pugi::xml_node& step, PcbModel& model);

    // Geometry parsers
    std::vector<Point> parse_polygon(const pugi::xml_node& node);
    std::vector<Point> parse_polyline(const pugi::xml_node& node);
    void parse_contour(const pugi::xml_node& contour, std::vector<Segment>& segs,
                       std::vector<ArcGeom>& arcs);

    // Helper to determine layer side from IPC layer name/function
    std::string determine_layer_side(const std::string& ipc_name,
                                     const std::string& layer_function,
                                     int layer_index, int total_layers);

    // Build layer mapping after all layers are parsed
    void build_layer_mapping(PcbModel& model);

    // Convert coordinates from IPC units to mm
    double to_mm(double val) const { return val * unit_scale_; }
    Point to_mm(const Point& pt) const { return {pt.x * unit_scale_, pt.y * unit_scale_}; }

    void log(const std::string& msg);
    void warn(const std::string& msg);
};

} // namespace ipc2kicad
