#include "kicad_writer.h"
#include "utils.h"
#include "geometry.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <set>
#include <algorithm>

namespace ipc2kicad {

KicadWriter::KicadWriter(const WriterOptions& opts)
    : opts_(opts) {}

bool KicadWriter::write(const std::string& filename, const PcbModel& model) {
    std::ofstream out(filename);
    if (!out.is_open()) {
        std::cerr << "Error: cannot open output file: " << filename << std::endl;
        return false;
    }
    bool ok = write(out, model);
    out.close();
    return ok;
}

bool KicadWriter::write(std::ostream& out, const PcbModel& model) {
    out << "(kicad_pcb ";
    write_header(out);
    out << "\n";
    write_general(out, model);
    write_paper(out);
    write_layers(out, model);
    write_setup(out, model);
    out << "\n";
    write_nets(out, model);
    out << "\n";
    write_footprints(out, model);
    out << "\n";
    write_outline(out, model);
    out << "\n";
    write_graphics(out, model);
    out << "\n";
    write_traces(out, model);
    out << "\n";
    write_vias(out, model);
    out << "\n";
    write_zones(out, model);
    out << "\n";
    out << ")\n";

    log("KiCad PCB written successfully");
    return true;
}

// --- Header ---

void KicadWriter::write_header(std::ostream& out) {
    if (opts_.version == KiCadVersion::V8) {
        out << "(version 20240108) (generator \"ipc2581_to_kicad\") "
               "(generator_version \"1.0\")";
    } else {
        out << "(version 20221018) (generator \"ipc2581_to_kicad\")";
    }
}

// --- General ---

void KicadWriter::write_general(std::ostream& out, const PcbModel& model) {
    out << "  (general\n";
    out << "    (thickness " << fmt(model.stackup.board_thickness) << ")\n";
    if (opts_.version == KiCadVersion::V8) {
        out << "    (legacy_teardrops no)\n";
    }
    out << "  )\n\n";
}

// --- Paper ---

void KicadWriter::write_paper(std::ostream& out) {
    out << "  (paper \"A4\")\n\n";
}

// --- Layers ---

void KicadWriter::write_layers(std::ostream& out, const PcbModel& model) {
    out << "  (layers\n";

    // Collect unique KiCad layers
    std::set<int> written_ids;

    // Always write F.Cu and B.Cu
    out << "    (0 \"F.Cu\" signal)\n";
    written_ids.insert(0);

    // Inner copper layers
    for (auto& l : model.layers) {
        if (l.kicad_id > 0 && l.kicad_id < 31 && l.type == "signal") {
            if (written_ids.insert(l.kicad_id).second) {
                out << "    (" << l.kicad_id << " \"" << l.kicad_name << "\" signal)\n";
            }
        }
    }

    out << "    (31 \"B.Cu\" signal)\n";
    written_ids.insert(31);

    // Non-copper layers (standard set)
    out << "    (32 \"B.Adhes\" user \"B.Adhesive\")\n";
    out << "    (33 \"F.Adhes\" user \"F.Adhesive\")\n";
    out << "    (34 \"B.Paste\" user)\n";
    out << "    (35 \"F.Paste\" user)\n";
    out << "    (36 \"B.SilkS\" user \"B.Silkscreen\")\n";
    out << "    (37 \"F.SilkS\" user \"F.Silkscreen\")\n";
    out << "    (38 \"B.Mask\" user)\n";
    out << "    (39 \"F.Mask\" user)\n";
    out << "    (40 \"Dwgs.User\" user \"User.Drawings\")\n";
    out << "    (41 \"Cmts.User\" user \"User.Comments\")\n";
    out << "    (42 \"Eco1.User\" user \"User.Eco1\")\n";
    out << "    (43 \"Eco2.User\" user \"User.Eco2\")\n";
    out << "    (44 \"Edge.Cuts\" user)\n";
    out << "    (45 \"Margin\" user)\n";
    out << "    (46 \"B.CrtYd\" user \"B.Courtyard\")\n";
    out << "    (47 \"F.CrtYd\" user \"F.Courtyard\")\n";
    out << "    (48 \"B.Fab\" user)\n";
    out << "    (49 \"F.Fab\" user)\n";

    out << "  )\n\n";
}

// --- Setup ---

void KicadWriter::write_setup(std::ostream& out, const PcbModel& model) {
    out << "  (setup\n";

    // Stackup
    if (!model.stackup.layers.empty()) {
        write_stackup(out, model);
    }

    out << "    (pad_to_mask_clearance 0)\n";
    out << "    (pcbplotparams\n";
    out << "      (usegerberextensions false)\n";
    out << "      (usegerberattributes true)\n";
    out << "      (usegerberadvancedattributes true)\n";
    out << "      (creategerberjobfile true)\n";
    out << "      (dashed_line_dash_ratio 12.000000)\n";
    out << "      (dashed_line_gap_ratio 3.000000)\n";
    out << "      (svgprecision 4)\n";
    out << "      (plotframeref false)\n";
    out << "      (viasonmask false)\n";
    out << "      (mode 1)\n";
    out << "      (useauxorigin false)\n";
    out << "      (hpglpennumber 1)\n";
    out << "      (hpglpenspeed 20)\n";
    out << "      (hpglpendiameter 15.000000)\n";
    out << "      (pdf_front_fp_property_popups true)\n";
    out << "      (pdf_back_fp_property_popups true)\n";
    out << "      (dxfpolygonmode true)\n";
    out << "      (dxfimperialunits true)\n";
    out << "      (dxfusepcbnewfont true)\n";
    out << "      (psnegative false)\n";
    out << "      (psa4output false)\n";
    out << "      (plotreference true)\n";
    out << "      (plotvalue true)\n";
    out << "      (plotfptext true)\n";
    out << "      (plotinvisibletext false)\n";
    out << "      (sketchpadsonfab false)\n";
    out << "      (subtractmaskfromsilk false)\n";
    out << "      (outputformat 1)\n";
    out << "      (mirror false)\n";
    out << "      (drillshape 1)\n";
    out << "      (scaleselection 1)\n";
    out << "      (outputdirectory \"\")\n";
    out << "    )\n";
    out << "  )\n\n";
}

void KicadWriter::write_stackup(std::ostream& out, const PcbModel& model) {
    out << "    (stackup\n";

    for (auto& sl : model.stackup.layers) {
        if (sl.type == "copper") {
            std::string layer_name;
            if (sl.kicad_layer_id == 0) layer_name = "F.Cu";
            else if (sl.kicad_layer_id == 31) layer_name = "B.Cu";
            else layer_name = "In" + std::to_string(sl.kicad_layer_id) + ".Cu";

            out << "      (layer \"" << layer_name << "\"\n";
            out << "        (type \"copper\")\n";
            out << "        (thickness " << fmt(sl.thickness) << ")\n";
            out << "      )\n";
        } else if (sl.type == "dielectric") {
            out << "      (layer \"dielectric\"\n";
            out << "        (type \"" << sl.type << "\")\n";
            out << "        (thickness " << fmt(sl.thickness) << ")\n";
            if (!sl.material.empty()) {
                out << "        (material \"" << sl.material << "\")\n";
            }
            out << "        (epsilon_r " << fmt(sl.epsilon_r) << ")\n";
            out << "      )\n";
        } else if (sl.type == "soldermask") {
            out << "      (layer \"" << sl.name << "\"\n";
            out << "        (type \"" << sl.type << "\")\n";
            out << "        (thickness " << fmt(sl.thickness) << ")\n";
            out << "      )\n";
        } else if (sl.type == "silkscreen") {
            out << "      (layer \"" << sl.name << "\"\n";
            out << "        (type \"" << sl.type << "\")\n";
            out << "      )\n";
        }
    }

    out << "      (copper_finish \"None\")\n";
    out << "      (dielectric_constraints no)\n";
    out << "    )\n";
}

// --- Nets ---

void KicadWriter::write_nets(std::ostream& out, const PcbModel& model) {
    for (auto& net : model.nets) {
        out << "  (net " << net.id << " " << sexp_quote(net.name) << ")\n";
    }
}

// --- Footprints ---

void KicadWriter::write_footprints(std::ostream& out, const PcbModel& model) {
    for (auto& comp : model.components) {
        auto it = model.footprint_defs.find(comp.footprint_ref);
        if (it == model.footprint_defs.end()) {
            log("Warning: footprint '" + comp.footprint_ref + "' not found for " + comp.refdes);
            continue;
        }
        write_footprint(out, model, comp, it->second);
    }
}

void KicadWriter::write_footprint(std::ostream& out, const PcbModel& model,
                                   const ComponentInstance& comp,
                                   const Footprint& fp) {
    std::string layer = comp.mirror ? "B.Cu" : "F.Cu";

    out << "  (footprint \"ipc2581:" << fp.name << "\"\n";
    out << "    (layer \"" << layer << "\")\n";
    if (opts_.version == KiCadVersion::V8) {
        out << "    (uuid " << uuid_from("fp_" + comp.refdes) << ")\n";
    }
    out << "    (at " << fmt(comp.position.x) << " " << fmt(comp.position.y);
    if (comp.rotation != 0.0) {
        out << " " << fmt(comp.rotation);
    }
    out << ")\n";

    // Properties
    out << "    (property \"Reference\" " << sexp_quote(comp.refdes) << "\n";
    out << "      (at 0 -2 0)\n";
    out << "      (layer \"" << (comp.mirror ? "B.SilkS" : "F.SilkS") << "\")\n";
    if (opts_.version == KiCadVersion::V8) {
        out << "      (uuid " << uuid_from("ref_" + comp.refdes) << ")\n";
    }
    out << "      (effects (font (size 1 1) (thickness 0.15)))\n";
    out << "    )\n";

    out << "    (property \"Value\" " << sexp_quote(comp.value.empty() ? fp.name : comp.value) << "\n";
    out << "      (at 0 2 0)\n";
    out << "      (layer \"" << (comp.mirror ? "B.Fab" : "F.Fab") << "\")\n";
    if (opts_.version == KiCadVersion::V8) {
        out << "      (uuid " << uuid_from("val_" + comp.refdes) << ")\n";
    }
    out << "      (effects (font (size 1 1) (thickness 0.15)))\n";
    out << "    )\n";

    out << "    (property \"Footprint\" " << sexp_quote("ipc2581:" + fp.name) << "\n";
    out << "      (at 0 0 0)\n";
    out << "      (layer \"" << (comp.mirror ? "B.Fab" : "F.Fab") << "\")\n";
    out << "      (hide yes)\n";
    if (opts_.version == KiCadVersion::V8) {
        out << "      (uuid " << uuid_from("fprop_" + comp.refdes) << ")\n";
    }
    out << "      (effects (font (size 1.27 1.27) (thickness 0.15)))\n";
    out << "    )\n";

    // Footprint graphics (silkscreen, fab, courtyard lines)
    for (size_t i = 0; i < fp.graphics.size(); i++) {
        auto& gi = fp.graphics[i];
        std::string glayer = gi.layer;
        // Flip layers if component is mirrored
        if (comp.mirror) {
            if (glayer == "F.SilkS") glayer = "B.SilkS";
            else if (glayer == "F.Fab") glayer = "B.Fab";
            else if (glayer == "F.CrtYd") glayer = "B.CrtYd";
        }

        if (gi.kind == GraphicItem::LINE) {
            out << "    (fp_line (start " << fmt(gi.start.x) << " " << fmt(gi.start.y) << ")"
                << " (end " << fmt(gi.end.x) << " " << fmt(gi.end.y) << ")"
                << " (stroke (width " << fmt(gi.width) << ") (type solid))"
                << " (layer \"" << glayer << "\")";
            if (opts_.version == KiCadVersion::V8) {
                out << " (uuid " << uuid_from("fpline_" + comp.refdes + "_" + std::to_string(i)) << ")";
            }
            out << ")\n";
        } else if (gi.kind == GraphicItem::ARC) {
            out << "    (fp_arc (start " << fmt(gi.start.x) << " " << fmt(gi.start.y) << ")"
                << " (mid " << fmt(gi.center.x) << " " << fmt(gi.center.y) << ")"
                << " (end " << fmt(gi.end.x) << " " << fmt(gi.end.y) << ")"
                << " (stroke (width " << fmt(gi.width) << ") (type solid))"
                << " (layer \"" << glayer << "\")";
            if (opts_.version == KiCadVersion::V8) {
                out << " (uuid " << uuid_from("fparc_" + comp.refdes + "_" + std::to_string(i)) << ")";
            }
            out << ")\n";
        }
    }

    // Pads
    for (auto& pad : fp.pads) {
        write_pad(out, pad, comp, model);
    }

    out << "  )\n\n";
}

void KicadWriter::write_pad(std::ostream& out, const PadDef& pad,
                             const ComponentInstance& comp,
                             const PcbModel& model) {
    // Determine pad type string
    std::string type_str;
    switch (pad.type) {
        case PadDef::SMD:       type_str = "smd"; break;
        case PadDef::THRU_HOLE: type_str = "thru_hole"; break;
        case PadDef::NPTH:      type_str = "np_thru_hole"; break;
    }

    // Determine shape string
    std::string shape_str;
    switch (pad.shape) {
        case PadDef::CIRCLE:    shape_str = "circle"; break;
        case PadDef::RECT:      shape_str = "rect"; break;
        case PadDef::OVAL:      shape_str = "oval"; break;
        case PadDef::ROUNDRECT: shape_str = "roundrect"; break;
        case PadDef::TRAPEZOID: shape_str = "trapezoid"; break;
        case PadDef::CUSTOM:    shape_str = "custom"; break;
    }

    out << "    (pad " << sexp_quote(pad.name) << " " << type_str << " " << shape_str;

    // Position (relative to footprint origin)
    out << " (at " << fmt(pad.offset.x) << " " << fmt(pad.offset.y);
    if (pad.rotation != 0.0) {
        out << " " << fmt(pad.rotation);
    }
    out << ")";

    // Size
    out << " (size " << fmt(pad.width) << " " << fmt(pad.height) << ")";

    // Drill
    if (pad.drill_diameter > 0) {
        out << " (drill " << fmt(pad.drill_diameter) << ")";
    }

    // Layers
    out << " (layers";
    if (pad.type == PadDef::THRU_HOLE || pad.type == PadDef::NPTH) {
        out << " \"*.Cu\" \"*.Mask\"";
    } else if (comp.mirror) {
        out << " \"B.Cu\" \"B.Paste\" \"B.Mask\"";
    } else {
        out << " \"F.Cu\" \"F.Paste\" \"F.Mask\"";
    }
    out << ")";

    // Roundrect ratio
    if (pad.shape == PadDef::ROUNDRECT) {
        out << " (roundrect_rratio " << fmt(pad.roundrect_ratio) << ")";
    }

    // Net
    auto net_it = comp.pin_net_map.find(pad.name);
    if (net_it != comp.pin_net_map.end() && !net_it->second.empty()) {
        int net_id = model.get_net_id(net_it->second);
        out << " (net " << net_id << " " << sexp_quote(net_it->second) << ")";
    }

    if (opts_.version == KiCadVersion::V8) {
        out << " (uuid " << uuid_from("pad_" + comp.refdes + "_" + pad.name) << ")";
    }

    // Custom shape primitives
    if (pad.shape == PadDef::CUSTOM && !pad.custom_shape.empty()) {
        out << "\n      (primitives\n";
        out << "        (gr_poly (pts";
        for (auto& pt : pad.custom_shape) {
            out << " (xy " << fmt(pt.x) << " " << fmt(pt.y) << ")";
        }
        out << ") (width 0) (fill yes))\n";
        out << "      )";
    }

    out << ")\n";
}

// --- Traces ---

void KicadWriter::write_traces(std::ostream& out, const PcbModel& model) {
    for (size_t i = 0; i < model.traces.size(); i++) {
        auto& t = model.traces[i];
        out << "  (segment (start " << fmt(t.start.x) << " " << fmt(t.start.y) << ")"
            << " (end " << fmt(t.end.x) << " " << fmt(t.end.y) << ")"
            << " (width " << fmt(t.width) << ")"
            << " (layer \"" << t.layer << "\")"
            << " (net " << t.net_id << ")";
        if (opts_.version == KiCadVersion::V8) {
            out << " (uuid " << uuid_from("seg_" + std::to_string(i)) << ")";
        }
        out << ")\n";
    }

    for (size_t i = 0; i < model.trace_arcs.size(); i++) {
        auto& a = model.trace_arcs[i];
        out << "  (arc (start " << fmt(a.start.x) << " " << fmt(a.start.y) << ")"
            << " (mid " << fmt(a.mid.x) << " " << fmt(a.mid.y) << ")"
            << " (end " << fmt(a.end.x) << " " << fmt(a.end.y) << ")"
            << " (width " << fmt(a.width) << ")"
            << " (layer \"" << a.layer << "\")"
            << " (net " << a.net_id << ")";
        if (opts_.version == KiCadVersion::V8) {
            out << " (uuid " << uuid_from("arc_" + std::to_string(i)) << ")";
        }
        out << ")\n";
    }
}

// --- Vias ---

void KicadWriter::write_vias(std::ostream& out, const PcbModel& model) {
    for (size_t i = 0; i < model.vias.size(); i++) {
        auto& v = model.vias[i];
        out << "  (via (at " << fmt(v.position.x) << " " << fmt(v.position.y) << ")"
            << " (size " << fmt(v.diameter) << ")"
            << " (drill " << fmt(v.drill) << ")"
            << " (layers \"" << v.start_layer << "\" \"" << v.end_layer << "\")"
            << " (net " << v.net_id << ")";
        if (opts_.version == KiCadVersion::V8) {
            out << " (uuid " << uuid_from("via_" + std::to_string(i)) << ")";
        }
        out << ")\n";
    }
}

// --- Zones ---

void KicadWriter::write_zones(std::ostream& out, const PcbModel& model) {
    for (size_t i = 0; i < model.zones.size(); i++) {
        auto& z = model.zones[i];
        out << "  (zone (net " << z.net_id << ")"
            << " (net_name " << sexp_quote(z.net_name) << ")"
            << " (layer \"" << z.layer << "\")";
        if (opts_.version == KiCadVersion::V8) {
            out << " (uuid " << uuid_from("zone_" + std::to_string(i)) << ")";
        }
        out << "\n";

        // Fill settings
        out << "    (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5))\n";

        // Outline polygon
        out << "    (polygon\n";
        out << "      (pts\n";
        for (auto& pt : z.outline) {
            out << "        (xy " << fmt(pt.x) << " " << fmt(pt.y) << ")\n";
        }
        out << "      )\n";
        out << "    )\n";

        // Holes
        for (auto& hole : z.holes) {
            out << "    (polygon\n";
            out << "      (pts\n";
            for (auto& pt : hole) {
                out << "        (xy " << fmt(pt.x) << " " << fmt(pt.y) << ")\n";
            }
            out << "      )\n";
            out << "    )\n";
        }

        out << "  )\n\n";
    }
}

// --- Board Outline ---

void KicadWriter::write_outline(std::ostream& out, const PcbModel& model) {
    for (size_t i = 0; i < model.outline.size(); i++) {
        auto& seg = model.outline[i];
        out << "  (gr_line (start " << fmt(seg.start.x) << " " << fmt(seg.start.y) << ")"
            << " (end " << fmt(seg.end.x) << " " << fmt(seg.end.y) << ")"
            << " (stroke (width " << fmt(seg.width) << ") (type solid))"
            << " (layer \"Edge.Cuts\")";
        if (opts_.version == KiCadVersion::V8) {
            out << " (uuid " << uuid_from("outline_" + std::to_string(i)) << ")";
        }
        out << ")\n";
    }

    for (size_t i = 0; i < model.outline_arcs.size(); i++) {
        auto& arc = model.outline_arcs[i];
        out << "  (gr_arc (start " << fmt(arc.start.x) << " " << fmt(arc.start.y) << ")"
            << " (mid " << fmt(arc.mid.x) << " " << fmt(arc.mid.y) << ")"
            << " (end " << fmt(arc.end.x) << " " << fmt(arc.end.y) << ")"
            << " (stroke (width " << fmt(arc.width) << ") (type solid))"
            << " (layer \"Edge.Cuts\")";
        if (opts_.version == KiCadVersion::V8) {
            out << " (uuid " << uuid_from("outarc_" + std::to_string(i)) << ")";
        }
        out << ")\n";
    }
}

// --- Graphics ---

void KicadWriter::write_graphics(std::ostream& out, const PcbModel& model) {
    for (size_t i = 0; i < model.graphics.size(); i++) {
        auto& gi = model.graphics[i];

        if (gi.kind == GraphicItem::LINE) {
            out << "  (gr_line (start " << fmt(gi.start.x) << " " << fmt(gi.start.y) << ")"
                << " (end " << fmt(gi.end.x) << " " << fmt(gi.end.y) << ")"
                << " (stroke (width " << fmt(gi.width) << ") (type solid))"
                << " (layer \"" << gi.layer << "\")";
            if (opts_.version == KiCadVersion::V8) {
                out << " (uuid " << uuid_from("grline_" + std::to_string(i)) << ")";
            }
            out << ")\n";
        } else if (gi.kind == GraphicItem::ARC) {
            out << "  (gr_arc (start " << fmt(gi.start.x) << " " << fmt(gi.start.y) << ")"
                << " (mid " << fmt(gi.center.x) << " " << fmt(gi.center.y) << ")"
                << " (end " << fmt(gi.end.x) << " " << fmt(gi.end.y) << ")"
                << " (stroke (width " << fmt(gi.width) << ") (type solid))"
                << " (layer \"" << gi.layer << "\")";
            if (opts_.version == KiCadVersion::V8) {
                out << " (uuid " << uuid_from("grarc_" + std::to_string(i)) << ")";
            }
            out << ")\n";
        } else if (gi.kind == GraphicItem::POLYGON) {
            out << "  (gr_poly (pts";
            for (auto& pt : gi.points) {
                out << " (xy " << fmt(pt.x) << " " << fmt(pt.y) << ")";
            }
            out << ")"
                << " (stroke (width " << fmt(gi.width) << ") (type solid))"
                << " (fill " << (gi.fill ? "yes" : "none") << ")"
                << " (layer \"" << gi.layer << "\")";
            if (opts_.version == KiCadVersion::V8) {
                out << " (uuid " << uuid_from("grpoly_" + std::to_string(i)) << ")";
            }
            out << ")\n";
        } else if (gi.kind == GraphicItem::CIRCLE) {
            out << "  (gr_circle (center " << fmt(gi.center.x) << " " << fmt(gi.center.y) << ")"
                << " (end " << fmt(gi.center.x + gi.radius) << " " << fmt(gi.center.y) << ")"
                << " (stroke (width " << fmt(gi.width) << ") (type solid))"
                << " (fill " << (gi.fill ? "yes" : "none") << ")"
                << " (layer \"" << gi.layer << "\")";
            if (opts_.version == KiCadVersion::V8) {
                out << " (uuid " << uuid_from("grcircle_" + std::to_string(i)) << ")";
            }
            out << ")\n";
        }
    }
}

// --- Helpers ---

std::string KicadWriter::ind() const {
    return std::string(indent_ * 2, ' ');
}

std::string KicadWriter::uuid() const {
    if (opts_.version == KiCadVersion::V8) {
        return generate_uuid();
    }
    return "";
}

std::string KicadWriter::uuid_from(const std::string& seed) const {
    return generate_uuid_from_seed(seed);
}

void KicadWriter::log(const std::string& msg) {
    if (opts_.verbose) {
        std::cout << "[KiCad] " << msg << std::endl;
    }
}

} // namespace ipc2kicad
