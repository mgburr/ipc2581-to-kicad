#include "json_import.h"

#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

namespace ipc2kicad {

// ── helpers ─────────────────────────────────────────────────────────

static Point read_point(const json& j) {
    if (j.is_array() && j.size() >= 2) {
        return {j[0].get<double>(), j[1].get<double>()};
    }
    return {};
}

static PadDef::Shape parse_pad_shape(const std::string& s) {
    if (s == "circle")    return PadDef::CIRCLE;
    if (s == "oval")      return PadDef::OVAL;
    if (s == "roundrect") return PadDef::ROUNDRECT;
    if (s == "trapezoid") return PadDef::TRAPEZOID;
    if (s == "custom")    return PadDef::CUSTOM;
    return PadDef::RECT;
}

static PadDef::Type parse_pad_type(const std::string& s) {
    if (s == "thru_hole") return PadDef::THRU_HOLE;
    if (s == "npth")      return PadDef::NPTH;
    return PadDef::SMD;
}

static GraphicItem::Kind parse_graphic_kind(const std::string& s) {
    if (s == "arc")     return GraphicItem::ARC;
    if (s == "circle")  return GraphicItem::CIRCLE;
    if (s == "rect")    return GraphicItem::RECT;
    if (s == "polygon") return GraphicItem::POLYGON;
    if (s == "text")    return GraphicItem::TEXT;
    return GraphicItem::LINE;
}

// ── section readers ─────────────────────────────────────────────────

static void read_outline(const json& j, PcbModel& model) {
    if (!j.contains("outline")) return;
    auto& ol = j["outline"];

    if (ol.contains("segments")) {
        for (auto& seg : ol["segments"]) {
            Segment s;
            s.start = read_point(seg["start"]);
            s.end   = read_point(seg["end"]);
            s.width = seg.value("width", 0.0);
            model.outline.push_back(s);
        }
    }

    if (ol.contains("arcs")) {
        for (auto& arc : ol["arcs"]) {
            ArcGeom a;
            a.start = read_point(arc["start"]);
            a.mid   = read_point(arc["mid"]);
            a.end   = read_point(arc["end"]);
            a.width = arc.value("width", 0.0);
            model.outline_arcs.push_back(a);
        }
    }
}

static void read_layers(const json& j, PcbModel& model) {
    if (!j.contains("layers")) return;
    for (auto& lj : j["layers"]) {
        LayerDef ld;
        ld.kicad_id     = lj.value("kicad_id", 0);
        ld.kicad_name   = lj.value("kicad_name", "");
        ld.type         = lj.value("type", "signal");
        ld.ipc_name     = lj.value("ipc_name", "");
        ld.ipc_function = lj.value("ipc_function", "");
        ld.ipc_side     = lj.value("ipc_side", "");
        ld.copper_order = lj.value("copper_order", -1);
        model.layers.push_back(ld);

        if (!ld.kicad_name.empty()) {
            model.ipc_layer_to_kicad[ld.ipc_name] = ld.kicad_name;
        }
    }
}

static void read_nets(const json& j, PcbModel& model) {
    if (!j.contains("nets")) return;
    for (auto& nj : j["nets"]) {
        NetDef nd;
        nd.id   = nj.value("id", 0);
        nd.name = nj.value("name", "");
        model.nets.push_back(nd);

        model.net_name_to_id[nd.name] = nd.id;
    }
}

static void read_stackup(const json& j, PcbModel& model) {
    if (!j.contains("stackup")) return;
    auto& sj = j["stackup"];
    model.stackup.board_thickness = sj.value("board_thickness", 1.6);

    if (sj.contains("layers")) {
        for (auto& slj : sj["layers"]) {
            StackupLayer sl;
            sl.name           = slj.value("name", "");
            sl.type           = slj.value("type", "");
            sl.thickness      = slj.value("thickness", 0.0);
            sl.material       = slj.value("material", "");
            sl.epsilon_r      = slj.value("epsilon_r", 4.5);
            sl.kicad_layer_id = slj.value("kicad_layer_id", -1);
            model.stackup.layers.push_back(sl);
        }
    }
}

static PadDef read_pad(const json& pj) {
    PadDef pad;
    pad.name            = pj.value("name", "");
    pad.shape           = parse_pad_shape(pj.value("shape", "rect"));
    pad.width           = pj.value("width", 0.0);
    pad.height          = pj.value("height", 0.0);
    pad.drill_diameter  = pj.value("drill_diameter", 0.0);
    pad.offset          = read_point(pj.value("offset", json::array()));
    pad.roundrect_ratio = pj.value("roundrect_ratio", 0.25);
    pad.type            = parse_pad_type(pj.value("type", "smd"));
    pad.layer_side      = pj.value("layer_side", "TOP");
    pad.rotation        = pj.value("rotation", 0.0);

    if (pj.contains("custom_shape")) {
        for (auto& pt : pj["custom_shape"]) {
            pad.custom_shape.push_back(read_point(pt));
        }
    }
    return pad;
}

static GraphicItem read_graphic(const json& gj) {
    GraphicItem gi;
    gi.kind        = parse_graphic_kind(gj.value("kind", "line"));
    gi.start       = read_point(gj.value("start", json::array()));
    gi.end         = read_point(gj.value("end", json::array()));
    gi.center      = read_point(gj.value("center", json::array()));
    gi.radius      = gj.value("radius", 0.0);
    gi.width       = gj.value("width", 0.1);
    gi.layer       = gj.value("layer", "");
    gi.fill        = gj.value("fill", false);
    gi.sweep_angle = gj.value("sweep_angle", 0.0);

    if (gj.contains("points")) {
        for (auto& pt : gj["points"]) {
            gi.points.push_back(read_point(pt));
        }
    }

    if (gj.contains("text")) {
        gi.text           = gj.value("text", "");
        gi.text_size      = gj.value("text_size", 1.0);
        gi.text_thickness = gj.value("text_thickness", 0.15);
    }

    return gi;
}

static void read_footprints(const json& j, PcbModel& model) {
    if (!j.contains("footprints")) return;
    for (auto& [name, fpj] : j["footprints"].items()) {
        Footprint fp;
        fp.name   = fpj.value("name", name);
        fp.origin = read_point(fpj.value("origin", json::array()));

        if (fpj.contains("pads")) {
            for (auto& pj : fpj["pads"]) {
                fp.pads.push_back(read_pad(pj));
            }
        }

        if (fpj.contains("graphics")) {
            for (auto& gj : fpj["graphics"]) {
                fp.graphics.push_back(read_graphic(gj));
            }
        }

        model.footprint_defs[name] = fp;
    }
}

static void read_components(const json& j, PcbModel& model) {
    if (!j.contains("components")) return;
    for (auto& cj : j["components"]) {
        ComponentInstance comp;
        comp.refdes        = cj.value("refdes", "");
        comp.footprint_ref = cj.value("footprint_ref", "");
        comp.value         = cj.value("value", "");
        comp.position      = read_point(cj.value("position", json::array()));
        comp.rotation      = cj.value("rotation", 0.0);
        comp.mirror        = cj.value("mirror", false);

        if (cj.contains("pin_net_map")) {
            for (auto& [pin, net] : cj["pin_net_map"].items()) {
                comp.pin_net_map[pin] = net.get<std::string>();
            }
        }

        model.components.push_back(comp);
    }
}

static void read_traces(const json& j, PcbModel& model) {
    if (!j.contains("traces")) return;
    for (auto& tj : j["traces"]) {
        TraceSegment ts;
        ts.start  = read_point(tj["start"]);
        ts.end    = read_point(tj["end"]);
        ts.width  = tj.value("width", 0.25);
        ts.layer  = tj.value("layer", "");
        ts.net_id = tj.value("net_id", 0);
        model.traces.push_back(ts);
    }
}

static void read_trace_arcs(const json& j, PcbModel& model) {
    if (!j.contains("trace_arcs")) return;
    for (auto& aj : j["trace_arcs"]) {
        TraceArc ta;
        ta.start  = read_point(aj["start"]);
        ta.mid    = read_point(aj["mid"]);
        ta.end    = read_point(aj["end"]);
        ta.width  = aj.value("width", 0.25);
        ta.layer  = aj.value("layer", "");
        ta.net_id = aj.value("net_id", 0);
        model.trace_arcs.push_back(ta);
    }
}

static void read_vias(const json& j, PcbModel& model) {
    if (!j.contains("vias")) return;
    for (auto& vj : j["vias"]) {
        Via v;
        v.position    = read_point(vj["position"]);
        v.diameter    = vj.value("diameter", 0.8);
        v.drill       = vj.value("drill", 0.4);
        v.start_layer = vj.value("start_layer", "F.Cu");
        v.end_layer   = vj.value("end_layer", "B.Cu");
        v.net_id      = vj.value("net_id", 0);
        model.vias.push_back(v);
    }
}

static void read_zones(const json& j, PcbModel& model) {
    if (!j.contains("zones")) return;
    for (auto& zj : j["zones"]) {
        Zone z;
        z.layer         = zj.value("layer", "");
        z.net_id        = zj.value("net_id", 0);
        z.net_name      = zj.value("net_name", "");
        z.min_thickness = zj.value("min_thickness", 0.25);
        z.clearance     = zj.value("clearance", 0.5);

        if (zj.contains("outline")) {
            for (auto& pt : zj["outline"]) {
                z.outline.push_back(read_point(pt));
            }
        }

        if (zj.contains("holes")) {
            for (auto& hole : zj["holes"]) {
                std::vector<Point> hole_pts;
                for (auto& pt : hole) {
                    hole_pts.push_back(read_point(pt));
                }
                z.holes.push_back(hole_pts);
            }
        }

        model.zones.push_back(z);
    }
}

static void read_graphics(const json& j, PcbModel& model) {
    if (!j.contains("graphics")) return;
    for (auto& gj : j["graphics"]) {
        model.graphics.push_back(read_graphic(gj));
    }
}

// ── public API ──────────────────────────────────────────────────────

bool read_json(std::istream& in, PcbModel& model) {
    try {
        json j = json::parse(in);

        read_outline(j, model);
        read_layers(j, model);
        read_nets(j, model);
        read_stackup(j, model);
        read_footprints(j, model);
        read_components(j, model);
        read_traces(j, model);
        read_trace_arcs(j, model);
        read_vias(j, model);
        read_zones(j, model);
        read_graphics(j, model);

        return true;
    } catch (const json::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << "\n";
        return false;
    }
}

bool read_json(const std::string& json_text, PcbModel& model) {
    std::istringstream iss(json_text);
    return read_json(iss, model);
}

} // namespace ipc2kicad
