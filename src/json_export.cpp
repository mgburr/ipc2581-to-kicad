#include "json_export.h"
#include "utils.h"

#include <iostream>

namespace ipc2kicad {

// Helper: escape a string for JSON (handle quotes, backslashes, control chars)
static std::string json_str(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

static void write_point(std::ostream& out, const Point& pt) {
    out << "[" << fmt(pt.x) << "," << fmt(pt.y) << "]";
}

static void write_outline(std::ostream& out, const PcbModel& model) {
    out << "\"outline\":{\"segments\":[";
    for (size_t i = 0; i < model.outline.size(); i++) {
        if (i > 0) out << ",";
        auto& s = model.outline[i];
        out << "{\"start\":";
        write_point(out, s.start);
        out << ",\"end\":";
        write_point(out, s.end);
        out << ",\"width\":" << fmt(s.width) << "}";
    }
    out << "],\"arcs\":[";
    for (size_t i = 0; i < model.outline_arcs.size(); i++) {
        if (i > 0) out << ",";
        auto& a = model.outline_arcs[i];
        out << "{\"start\":";
        write_point(out, a.start);
        out << ",\"mid\":";
        write_point(out, a.mid);
        out << ",\"end\":";
        write_point(out, a.end);
        out << ",\"width\":" << fmt(a.width) << "}";
    }
    out << "]}";
}

static void write_layers(std::ostream& out, const PcbModel& model) {
    out << "\"layers\":[";
    for (size_t i = 0; i < model.layers.size(); i++) {
        if (i > 0) out << ",";
        auto& l = model.layers[i];
        out << "{\"kicad_id\":" << l.kicad_id
            << ",\"kicad_name\":" << json_str(l.kicad_name)
            << ",\"type\":" << json_str(l.type)
            << ",\"ipc_name\":" << json_str(l.ipc_name)
            << ",\"ipc_function\":" << json_str(l.ipc_function)
            << ",\"ipc_side\":" << json_str(l.ipc_side)
            << ",\"copper_order\":" << l.copper_order << "}";
    }
    out << "]";
}

static void write_nets(std::ostream& out, const PcbModel& model) {
    out << "\"nets\":[";
    for (size_t i = 0; i < model.nets.size(); i++) {
        if (i > 0) out << ",";
        auto& n = model.nets[i];
        out << "{\"id\":" << n.id << ",\"name\":" << json_str(n.name) << "}";
    }
    out << "]";
}

static void write_stackup(std::ostream& out, const PcbModel& model) {
    out << "\"stackup\":{\"board_thickness\":" << fmt(model.stackup.board_thickness)
        << ",\"layers\":[";
    for (size_t i = 0; i < model.stackup.layers.size(); i++) {
        if (i > 0) out << ",";
        auto& sl = model.stackup.layers[i];
        out << "{\"name\":" << json_str(sl.name)
            << ",\"type\":" << json_str(sl.type)
            << ",\"thickness\":" << fmt(sl.thickness)
            << ",\"material\":" << json_str(sl.material)
            << ",\"epsilon_r\":" << fmt(sl.epsilon_r)
            << ",\"kicad_layer_id\":" << sl.kicad_layer_id << "}";
    }
    out << "]}";
}

static std::string pad_shape_str(PadDef::Shape s) {
    switch (s) {
        case PadDef::CIRCLE:    return "circle";
        case PadDef::RECT:      return "rect";
        case PadDef::OVAL:      return "oval";
        case PadDef::ROUNDRECT: return "roundrect";
        case PadDef::TRAPEZOID: return "trapezoid";
        case PadDef::CUSTOM:    return "custom";
    }
    return "rect";
}

static std::string pad_type_str(PadDef::Type t) {
    switch (t) {
        case PadDef::SMD:       return "smd";
        case PadDef::THRU_HOLE: return "thru_hole";
        case PadDef::NPTH:      return "npth";
    }
    return "smd";
}

static void write_pad(std::ostream& out, const PadDef& pad) {
    out << "{\"name\":" << json_str(pad.name)
        << ",\"shape\":" << json_str(pad_shape_str(pad.shape))
        << ",\"width\":" << fmt(pad.width)
        << ",\"height\":" << fmt(pad.height)
        << ",\"drill_diameter\":" << fmt(pad.drill_diameter)
        << ",\"offset\":";
    write_point(out, pad.offset);
    out << ",\"roundrect_ratio\":" << fmt(pad.roundrect_ratio)
        << ",\"type\":" << json_str(pad_type_str(pad.type))
        << ",\"layer_side\":" << json_str(pad.layer_side)
        << ",\"rotation\":" << fmt(pad.rotation);
    if (!pad.custom_shape.empty()) {
        out << ",\"custom_shape\":[";
        for (size_t i = 0; i < pad.custom_shape.size(); i++) {
            if (i > 0) out << ",";
            write_point(out, pad.custom_shape[i]);
        }
        out << "]";
    }
    out << "}";
}

static std::string graphic_kind_str(GraphicItem::Kind k) {
    switch (k) {
        case GraphicItem::LINE:    return "line";
        case GraphicItem::ARC:     return "arc";
        case GraphicItem::CIRCLE:  return "circle";
        case GraphicItem::RECT:    return "rect";
        case GraphicItem::POLYGON: return "polygon";
        case GraphicItem::TEXT:    return "text";
    }
    return "line";
}

static void write_graphic_item(std::ostream& out, const GraphicItem& gi) {
    out << "{\"kind\":" << json_str(graphic_kind_str(gi.kind))
        << ",\"start\":";
    write_point(out, gi.start);
    out << ",\"end\":";
    write_point(out, gi.end);
    out << ",\"center\":";
    write_point(out, gi.center);
    out << ",\"radius\":" << fmt(gi.radius)
        << ",\"width\":" << fmt(gi.width)
        << ",\"layer\":" << json_str(gi.layer)
        << ",\"fill\":" << (gi.fill ? "true" : "false")
        << ",\"sweep_angle\":" << fmt(gi.sweep_angle);
    if (!gi.points.empty()) {
        out << ",\"points\":[";
        for (size_t i = 0; i < gi.points.size(); i++) {
            if (i > 0) out << ",";
            write_point(out, gi.points[i]);
        }
        out << "]";
    }
    if (!gi.text.empty()) {
        out << ",\"text\":" << json_str(gi.text)
            << ",\"text_size\":" << fmt(gi.text_size)
            << ",\"text_thickness\":" << fmt(gi.text_thickness);
    }
    out << "}";
}

static void write_footprints(std::ostream& out, const PcbModel& model) {
    out << "\"footprints\":{";
    bool first = true;
    for (auto& [name, fp] : model.footprint_defs) {
        if (!first) out << ",";
        first = false;
        out << json_str(name) << ":{\"name\":" << json_str(fp.name)
            << ",\"origin\":";
        write_point(out, fp.origin);
        out << ",\"pads\":[";
        for (size_t i = 0; i < fp.pads.size(); i++) {
            if (i > 0) out << ",";
            write_pad(out, fp.pads[i]);
        }
        out << "],\"graphics\":[";
        for (size_t i = 0; i < fp.graphics.size(); i++) {
            if (i > 0) out << ",";
            write_graphic_item(out, fp.graphics[i]);
        }
        out << "]}";
    }
    out << "}";
}

static void write_components(std::ostream& out, const PcbModel& model) {
    out << "\"components\":[";
    for (size_t i = 0; i < model.components.size(); i++) {
        if (i > 0) out << ",";
        auto& c = model.components[i];
        out << "{\"refdes\":" << json_str(c.refdes)
            << ",\"footprint_ref\":" << json_str(c.footprint_ref)
            << ",\"value\":" << json_str(c.value)
            << ",\"description\":" << json_str(c.description)
            << ",\"part_number\":" << json_str(c.part_number)
            << ",\"position\":";
        write_point(out, c.position);
        out << ",\"rotation\":" << fmt(c.rotation)
            << ",\"mirror\":" << (c.mirror ? "true" : "false")
            << ",\"pin_net_map\":{";
        bool first = true;
        for (auto& [pin, net] : c.pin_net_map) {
            if (!first) out << ",";
            first = false;
            out << json_str(pin) << ":" << json_str(net);
        }
        out << "}}";
    }
    out << "]";
}

static void write_traces(std::ostream& out, const PcbModel& model) {
    out << "\"traces\":[";
    for (size_t i = 0; i < model.traces.size(); i++) {
        if (i > 0) out << ",";
        auto& t = model.traces[i];
        out << "{\"start\":";
        write_point(out, t.start);
        out << ",\"end\":";
        write_point(out, t.end);
        out << ",\"width\":" << fmt(t.width)
            << ",\"layer\":" << json_str(t.layer)
            << ",\"net_id\":" << t.net_id << "}";
    }
    out << "]";
}

static void write_trace_arcs(std::ostream& out, const PcbModel& model) {
    out << "\"trace_arcs\":[";
    for (size_t i = 0; i < model.trace_arcs.size(); i++) {
        if (i > 0) out << ",";
        auto& a = model.trace_arcs[i];
        out << "{\"start\":";
        write_point(out, a.start);
        out << ",\"mid\":";
        write_point(out, a.mid);
        out << ",\"end\":";
        write_point(out, a.end);
        out << ",\"width\":" << fmt(a.width)
            << ",\"layer\":" << json_str(a.layer)
            << ",\"net_id\":" << a.net_id << "}";
    }
    out << "]";
}

static void write_vias(std::ostream& out, const PcbModel& model) {
    out << "\"vias\":[";
    for (size_t i = 0; i < model.vias.size(); i++) {
        if (i > 0) out << ",";
        auto& v = model.vias[i];
        out << "{\"position\":";
        write_point(out, v.position);
        out << ",\"diameter\":" << fmt(v.diameter)
            << ",\"drill\":" << fmt(v.drill)
            << ",\"start_layer\":" << json_str(v.start_layer)
            << ",\"end_layer\":" << json_str(v.end_layer)
            << ",\"net_id\":" << v.net_id << "}";
    }
    out << "]";
}

static void write_zones(std::ostream& out, const PcbModel& model) {
    out << "\"zones\":[";
    for (size_t i = 0; i < model.zones.size(); i++) {
        if (i > 0) out << ",";
        auto& z = model.zones[i];
        out << "{\"layer\":" << json_str(z.layer)
            << ",\"net_id\":" << z.net_id
            << ",\"net_name\":" << json_str(z.net_name)
            << ",\"min_thickness\":" << fmt(z.min_thickness)
            << ",\"clearance\":" << fmt(z.clearance)
            << ",\"outline\":[";
        for (size_t j = 0; j < z.outline.size(); j++) {
            if (j > 0) out << ",";
            write_point(out, z.outline[j]);
        }
        out << "]";
        if (!z.holes.empty()) {
            out << ",\"holes\":[";
            for (size_t h = 0; h < z.holes.size(); h++) {
                if (h > 0) out << ",";
                out << "[";
                for (size_t j = 0; j < z.holes[h].size(); j++) {
                    if (j > 0) out << ",";
                    write_point(out, z.holes[h][j]);
                }
                out << "]";
            }
            out << "]";
        }
        out << "}";
    }
    out << "]";
}

static void write_graphics(std::ostream& out, const PcbModel& model) {
    out << "\"graphics\":[";
    for (size_t i = 0; i < model.graphics.size(); i++) {
        if (i > 0) out << ",";
        write_graphic_item(out, model.graphics[i]);
    }
    out << "]";
}

void write_json(std::ostream& out, const PcbModel& model) {
    out << "{";
    write_outline(out, model);      out << ",";
    write_layers(out, model);       out << ",";
    write_nets(out, model);         out << ",";
    write_stackup(out, model);      out << ",";
    write_footprints(out, model);   out << ",";
    write_components(out, model);   out << ",";
    write_traces(out, model);       out << ",";
    write_trace_arcs(out, model);   out << ",";
    write_vias(out, model);         out << ",";
    write_zones(out, model);        out << ",";
    write_graphics(out, model);
    out << "}\n";
}

} // namespace ipc2kicad
