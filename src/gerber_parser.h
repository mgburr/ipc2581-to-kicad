#pragma once

#include "pcb_model.h"
#include <string>
#include <vector>
#include <map>

namespace ipc2kicad {

// Describes one file in a Gerber/drill/netlist set
struct GerberFileInfo {
    std::string filepath;           // full path
    std::string filename;           // basename
    std::string detected_layer;     // auto-detected KiCad layer name
    std::string assigned_layer;     // user-confirmed (from --layer-map)
    std::string x2_file_function;   // from %TF.FileFunction% if present
    enum FileType { GERBER, DRILL, NETLIST, UNKNOWN };
    FileType file_type = UNKNOWN;
};

// A complete set of files for one PCB
struct GerberSet {
    std::string directory;
    std::vector<GerberFileInfo> files;
};

// RS-274X aperture definition
struct Aperture {
    int code = 0;
    enum Shape { CIRCLE, RECT, OBLONG, POLYGON, MACRO };
    Shape shape = CIRCLE;
    double param1 = 0.0;   // diameter (circle) or X (rect/oblong)
    double param2 = 0.0;   // Y (rect/oblong)
    double hole_diameter = 0.0;
    std::string macro_name;
    int n_vertices = 0;
    double rotation = 0.0;
};

struct ParserOptions;  // forward from ipc2581_parser.h

class GerberParser {
public:
    explicit GerberParser(bool verbose = false);

    // Phase 1: Scan & Identify
    GerberSet scan_directory(const std::string& dir);
    GerberSet scan_files(const std::vector<std::string>& paths);

    // Phase 2: Parse & Populate
    bool parse(const GerberSet& gset, PcbModel& model);

    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    bool verbose_;
    std::vector<std::string> warnings_;

    // Phase 1 helpers
    GerberFileInfo identify_file(const std::string& filepath);
    GerberFileInfo::FileType detect_file_type(const std::string& filepath,
                                               const std::string& filename);
    std::string detect_layer_x2(const std::string& filepath);
    std::string detect_layer_extension(const std::string& filename);
    std::string detect_layer_kicad_naming(const std::string& filename);
    std::string detect_layer_keywords(const std::string& filename);

    // Phase 2: parse a single Gerber file into model
    bool parse_gerber_file(const GerberFileInfo& info, PcbModel& model);

    // Gerber state machine internals
    struct GerberState {
        // Coordinate format
        int x_int = 2, x_dec = 4;
        int y_int = 2, y_dec = 4;
        bool leading_zero_suppression = true;  // default: leading zeros omitted
        bool absolute_mode = true;

        // Units
        bool metric = true;  // true=mm, false=inch

        // Aperture table
        std::map<int, Aperture> apertures;

        // Current state
        double cur_x = 0.0, cur_y = 0.0;
        int current_aperture = 0;
        int interpolation = 1;  // 1=linear, 2=CW arc, 3=CCW arc
        bool region_mode = false;
        bool dark_polarity = true;  // true=dark (draw), false=clear (erase)

        // Region polygon accumulator
        std::vector<Point> region_points;

        // Current layer info
        std::string kicad_layer;
        bool is_copper = false;

        void reset() {
            x_int = 2; x_dec = 4;
            y_int = 2; y_dec = 4;
            leading_zero_suppression = true;
            absolute_mode = true;
            metric = true;
            apertures.clear();
            cur_x = cur_y = 0.0;
            current_aperture = 0;
            interpolation = 1;
            region_mode = false;
            dark_polarity = true;
            region_points.clear();
        }
    };

    double parse_coordinate(const std::string& str, int n_int, int n_dec,
                            bool leading_zero_suppression);
    void process_gerber_line(const std::string& line, GerberState& state,
                             PcbModel& model);
    void process_command_block(const std::string& block, GerberState& state,
                               PcbModel& model);
    void flush_region(GerberState& state, PcbModel& model);
    Point state_to_kicad(const GerberState& state);

    void log(const std::string& msg);
    void warn(const std::string& msg);
};

} // namespace ipc2kicad
