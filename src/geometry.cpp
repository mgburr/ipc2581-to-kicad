#include "geometry.h"
#include <cmath>

namespace ipc2kicad {

ArcGeom arc_center_to_mid(const Point& start, const Point& center,
                          double sweep_deg, double width,
                          const std::string& layer) {
    ArcGeom arc;
    arc.start = start;
    arc.width = width;
    arc.layer = layer;

    double radius = distance(start, center);
    double start_angle = std::atan2(start.y - center.y, start.x - center.x);
    double sweep_rad = deg_to_rad(sweep_deg);

    // End point
    double end_angle = start_angle + sweep_rad;
    arc.end.x = center.x + radius * std::cos(end_angle);
    arc.end.y = center.y + radius * std::sin(end_angle);

    // Mid point (at half the sweep)
    double mid_angle = start_angle + sweep_rad / 2.0;
    arc.mid.x = center.x + radius * std::cos(mid_angle);
    arc.mid.y = center.y + radius * std::sin(mid_angle);

    return arc;
}

Point rotate_point(const Point& pt, const Point& origin, double angle_deg) {
    double rad = deg_to_rad(angle_deg);
    double dx = pt.x - origin.x;
    double dy = pt.y - origin.y;
    double cos_a = std::cos(rad);
    double sin_a = std::sin(rad);
    return {
        origin.x + dx * cos_a - dy * sin_a,
        origin.y + dx * sin_a + dy * cos_a
    };
}

} // namespace ipc2kicad
