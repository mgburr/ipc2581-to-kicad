#pragma once

#include <cmath>
#include <vector>
#include <string>

namespace ipc2kicad {

constexpr double PI = 3.14159265358979323846;

struct Point {
    double x = 0.0;
    double y = 0.0;

    Point() = default;
    Point(double x, double y) : x(x), y(y) {}

    Point operator+(const Point& o) const { return {x + o.x, y + o.y}; }
    Point operator-(const Point& o) const { return {x - o.x, y - o.y}; }
    Point operator*(double s) const { return {x * s, y * s}; }
    bool operator==(const Point& o) const {
        return std::abs(x - o.x) < 1e-6 && std::abs(y - o.y) < 1e-6;
    }
};

inline double distance(const Point& a, const Point& b) {
    double dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

inline double deg_to_rad(double deg) { return deg * PI / 180.0; }
inline double rad_to_deg(double rad) { return rad * 180.0 / PI; }

struct Segment {
    Point start, end;
    double width = 0.0;
    std::string layer;
};

struct ArcGeom {
    Point start, mid, end;
    double width = 0.0;
    std::string layer;
};

// Convert IPC-2581 center-angle arc to KiCad start/mid/end arc
ArcGeom arc_center_to_mid(const Point& start, const Point& center,
                          double sweep_deg, double width,
                          const std::string& layer);

// Rotate a point around an origin by angle_deg (counter-clockwise)
Point rotate_point(const Point& pt, const Point& origin, double angle_deg);

// Mirror a point across the X axis (negate Y)
inline Point mirror_y(const Point& pt) { return {pt.x, -pt.y}; }

// Flip Y coordinate for IPC-2581 to KiCad coordinate conversion
// IPC-2581: Y+ up, KiCad: Y+ down
inline Point ipc_to_kicad_coords(const Point& pt) { return {pt.x, -pt.y}; }

} // namespace ipc2kicad
