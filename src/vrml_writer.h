#pragma once

#include "geometry.h"
#include <string>
#include <vector>

namespace ipc2kicad {

struct VrmlBodyParams {
    std::vector<Point> outline;    // 2D polygon in mm, Y-up
    double height = 0.0;           // extrusion height in mm
    std::string name;              // for comments
};

class VrmlWriter {
public:
    static bool write_body(const std::string& output_path, const VrmlBodyParams& params);
};

} // namespace ipc2kicad
