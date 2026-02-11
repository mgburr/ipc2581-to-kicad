#include "vrml_writer.h"

#include <fstream>
#include <cmath>

namespace ipc2kicad {

bool VrmlWriter::write_body(const std::string& output_path, const VrmlBodyParams& params) {
    if (params.outline.size() < 3 || params.height <= 0)
        return false;

    // Remove closing duplicate point if present
    auto pts = params.outline;
    if (pts.size() > 1 && pts.front() == pts.back())
        pts.pop_back();
    if (pts.size() < 3)
        return false;

    int n = static_cast<int>(pts.size());
    // KiCad VRML convention: 1 unit = 2.54mm (0.1 inch)
    constexpr double VRML_SCALE = 2.54;
    double h = params.height / VRML_SCALE;

    std::ofstream out(output_path);
    if (!out.is_open())
        return false;

    out << "#VRML V2.0 utf8\n";
    out << "# Generated body for " << params.name << "\n\n";

    out << "Shape {\n";
    out << "  appearance Appearance {\n";
    out << "    material Material {\n";
    out << "      diffuseColor 0.15 0.15 0.15\n";
    out << "      specularColor 0.3 0.3 0.3\n";
    out << "      shininess 0.3\n";
    out << "    }\n";
    out << "  }\n";
    out << "  geometry IndexedFaceSet {\n";
    out << "    coord Coordinate {\n";
    out << "      point [\n";

    // Bottom face vertices (Z=0), then top face vertices (Z=h)
    for (int i = 0; i < n; i++) {
        double x = pts[i].x / VRML_SCALE;
        double y = pts[i].y / VRML_SCALE;
        out << "        " << x << " " << y << " 0";
        out << ",\n";
    }
    for (int i = 0; i < n; i++) {
        double x = pts[i].x / VRML_SCALE;
        double y = pts[i].y / VRML_SCALE;
        out << "        " << x << " " << y << " " << h;
        if (i < n - 1) out << ",";
        out << "\n";
    }

    out << "      ]\n";
    out << "    }\n";
    out << "    coordIndex [\n";

    // Bottom face (reversed winding for outward-facing normal pointing down)
    out << "      ";
    for (int i = n - 1; i >= 0; i--) {
        out << i << " ";
    }
    out << "-1,\n";

    // Top face
    out << "      ";
    for (int i = 0; i < n; i++) {
        out << (n + i) << " ";
    }
    out << "-1,\n";

    // Side quads
    for (int i = 0; i < n; i++) {
        int next = (i + 1) % n;
        // Bottom[i], Bottom[next], Top[next], Top[i]
        out << "      " << i << " " << next << " " << (n + next) << " " << (n + i) << " -1,\n";
    }

    out << "    ]\n";
    out << "  }\n";
    out << "}\n";

    out.close();
    return true;
}

} // namespace ipc2kicad
