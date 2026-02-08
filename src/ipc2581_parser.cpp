#include "ipc2581_parser.h"
#include "utils.h"
#include "geometry.h"
#include "pugixml.hpp"

#include <iostream>
#include <algorithm>
#include <set>
#include <cassert>

namespace ipc2kicad {

Ipc2581Parser::Ipc2581Parser(const ParserOptions& opts)
    : opts_(opts) {}

bool Ipc2581Parser::parse(const std::string& filename, PcbModel& model) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(filename.c_str());
    if (!result) {
        warn("Failed to parse XML: " + std::string(result.description()));
        return false;
    }

    auto root = doc.child("IPC-2581");
    if (!root) {
        warn("Not a valid IPC-2581 file: missing <IPC-2581> root element");
        return false;
    }

    log("IPC-2581 revision: " + std::string(root.attribute("revision").as_string("unknown")));

    // Parse Content section (dictionaries)
    auto content = root.child("Content");
    if (content) {
        parse_dictionaries(content, model);
    }

    // Parse LogicalNet
    parse_nets(root, model);

    // Find Ecad section
    auto ecad = root.child("Ecad");
    if (!ecad) {
        warn("No <Ecad> section found");
        return false;
    }

    // Parse CadHeader for units
    auto cad_header = ecad.child("CadHeader");
    if (cad_header) {
        parse_units(cad_header);
    }

    // Find CadData
    auto cad_data = ecad.child("CadData");
    if (!cad_data) {
        warn("No <CadData> section found");
        return false;
    }

    // Parse layers
    parse_layers(cad_data, model);

    // Parse stackup
    parse_stackup(cad_data, model);

    // Build layer mapping
    build_layer_mapping(model);

    // Find the Step to convert
    pugi::xml_node step;
    if (opts_.step_name.empty()) {
        step = cad_data.child("Step");
    } else {
        for (auto s : cad_data.children("Step")) {
            if (std::string(s.attribute("name").as_string()) == opts_.step_name) {
                step = s;
                break;
            }
        }
    }

    if (!step) {
        warn("No matching <Step> found" +
             (opts_.step_name.empty() ? "" : " for name '" + opts_.step_name + "'"));
        return false;
    }

    log("Converting step: " + std::string(step.attribute("name").as_string()));

    // Parse step contents
    parse_profile(step, model);
    parse_packages(step, model);
    parse_components(step, model);
    parse_padstack_rotations(step, model);
    parse_padstack_vias(step, model);
    parse_layer_features(step, model);

    log("Parse complete: " + std::to_string(model.components.size()) + " components, " +
        std::to_string(model.traces.size()) + " traces, " +
        std::to_string(model.vias.size()) + " vias, " +
        std::to_string(model.nets.size()) + " nets");

    return true;
}

std::vector<std::string> Ipc2581Parser::list_steps(const std::string& filename) {
    std::vector<std::string> steps;
    pugi::xml_document doc;
    if (!doc.load_file(filename.c_str())) return steps;

    auto root = doc.child("IPC-2581");
    if (!root) return steps;

    auto cad_data = root.child("Ecad").child("CadData");
    if (!cad_data) return steps;

    for (auto step : cad_data.children("Step")) {
        steps.push_back(step.attribute("name").as_string("unnamed"));
    }
    return steps;
}

// --- Unit Parsing ---

void Ipc2581Parser::parse_units(const pugi::xml_node& cad_header) {
    std::string units = cad_header.attribute("units").as_string("MM");
    std::transform(units.begin(), units.end(), units.begin(), ::toupper);
    unit_scale_ = unit_to_mm(units);
    log("Units: " + units + " (scale to mm: " + std::to_string(unit_scale_) + ")");
}

// --- Layer Parsing ---

void Ipc2581Parser::parse_layers(const pugi::xml_node& cad_data, PcbModel& model) {
    int index = 0;
    for (auto layer_node : cad_data.children("Layer")) {
        LayerDef ldef;
        ldef.ipc_name = layer_node.attribute("name").as_string();
        ldef.ipc_function = layer_node.attribute("layerFunction").as_string();
        ldef.ipc_side = layer_node.attribute("side").as_string();
        ldef.copper_order = -1;

        model.layers.push_back(ldef);
        index++;
    }
    log("Found " + std::to_string(model.layers.size()) + " layers");
}

void Ipc2581Parser::build_layer_mapping(PcbModel& model) {
    // Identify copper layers and their ordering
    int copper_count = 0;
    std::vector<int> copper_indices;

    for (size_t i = 0; i < model.layers.size(); i++) {
        auto& l = model.layers[i];
        std::string func = l.ipc_function;
        std::transform(func.begin(), func.end(), func.begin(), ::toupper);

        if (func == "SIGNAL" || func == "POWER_GROUND" || func == "POWER" ||
            func == "GROUND" || func == "MIXED") {
            l.copper_order = copper_count++;
            copper_indices.push_back(static_cast<int>(i));
        }
    }

    // Assign KiCad layer names and IDs
    for (size_t i = 0; i < model.layers.size(); i++) {
        auto& l = model.layers[i];
        std::string func = l.ipc_function;
        std::transform(func.begin(), func.end(), func.begin(), ::toupper);
        std::string side = l.ipc_side;
        std::transform(side.begin(), side.end(), side.begin(), ::toupper);

        // Determine side from copper_order if not explicit
        if (side.empty() && l.copper_order >= 0) {
            if (l.copper_order == 0) side = "TOP";
            else if (l.copper_order == copper_count - 1) side = "BOTTOM";
            else side = "INTERNAL";
        }

        // Map to KiCad layer
        if (l.copper_order >= 0) {
            if (l.copper_order == 0) {
                l.kicad_name = "F.Cu";
                l.kicad_id = 0;
                l.type = "signal";
            } else if (l.copper_order == copper_count - 1) {
                l.kicad_name = "B.Cu";
                l.kicad_id = 31;
                l.type = "signal";
            } else {
                l.kicad_name = "In" + std::to_string(l.copper_order) + ".Cu";
                l.kicad_id = l.copper_order;
                l.type = "signal";
            }
        } else if (func == "SOLDERMASK" || func == "SOLDER_MASK") {
            if (side == "TOP" || side.empty()) {
                l.kicad_name = "F.Mask";
                l.kicad_id = 39;
            } else {
                l.kicad_name = "B.Mask";
                l.kicad_id = 38;
            }
            l.type = "user";
        } else if (func == "PASTEMASK" || func == "SOLDER_PASTE" || func == "SOLDERPASTE") {
            if (side == "TOP" || side.empty()) {
                l.kicad_name = "F.Paste";
                l.kicad_id = 37;
            } else {
                l.kicad_name = "B.Paste";
                l.kicad_id = 36;
            }
            l.type = "user";
        } else if (func == "SILKSCREEN" || func == "SILK_SCREEN") {
            if (side == "TOP" || side.empty()) {
                l.kicad_name = "F.SilkS";
                l.kicad_id = 37;
            } else {
                l.kicad_name = "B.SilkS";
                l.kicad_id = 36;
            }
            l.type = "user";
        } else if (func == "ASSEMBLY" || func == "ASSEMBLY_DRAWING") {
            if (side == "TOP" || side.empty()) {
                l.kicad_name = "F.Fab";
                l.kicad_id = 49;
            } else {
                l.kicad_name = "B.Fab";
                l.kicad_id = 48;
            }
            l.type = "user";
        } else if (func == "BOARD_OUTLINE" || func == "ROUT" || func == "ROUTE") {
            l.kicad_name = "Edge.Cuts";
            l.kicad_id = 44;
            l.type = "user";
        } else if (func == "DRILL" || func == "DRILL_FIGURE" || func == "DRILL_DRAWING") {
            l.kicad_name = ""; // drills are handled specially
            l.type = "user";
        } else if (func == "DOCUMENT" || func == "DOCUMENTATION") {
            l.kicad_name = "Cmts.User";
            l.kicad_id = 46;
            l.type = "user";
        } else {
            l.kicad_name = "Cmts.User";
            l.kicad_id = 46;
            l.type = "user";
        }

        // Store in lookup map
        if (!l.ipc_name.empty() && !l.kicad_name.empty()) {
            model.ipc_layer_to_kicad[l.ipc_name] = l.kicad_name;
        }
    }

    log("Layer mapping built: " + std::to_string(copper_count) + " copper layers, " +
        std::to_string(model.ipc_layer_to_kicad.size()) + " total mapped");
}

// --- Stackup Parsing ---

void Ipc2581Parser::parse_stackup(const pugi::xml_node& cad_data, PcbModel& model) {
    auto stackup_node = cad_data.child("Stackup");
    if (!stackup_node) {
        // Try inside Step
        for (auto step : cad_data.children("Step")) {
            stackup_node = step.child("Stackup");
            if (stackup_node) break;
        }
    }
    if (!stackup_node) {
        log("No stackup information found, using defaults");
        return;
    }

    double total_thickness = 0;

    for (auto group : stackup_node.children("StackupGroup")) {
        for (auto layer_node : group.children("StackupLayer")) {
            StackupLayer sl;
            sl.name = layer_node.attribute("layerOrGroupRef").as_string();
            sl.thickness = to_mm(parse_double(
                layer_node.attribute("thickness").as_string()));
            sl.material = layer_node.attribute("material").as_string("");

            // Determine type from the referenced layer
            for (auto& ldef : model.layers) {
                if (ldef.ipc_name == sl.name) {
                    std::string func = ldef.ipc_function;
                    std::transform(func.begin(), func.end(), func.begin(), ::toupper);
                    if (func == "SIGNAL" || func == "POWER_GROUND" || func == "POWER" ||
                        func == "GROUND" || func == "MIXED") {
                        sl.type = "copper";
                        sl.kicad_layer_id = ldef.kicad_id;
                    } else if (func == "SOLDERMASK" || func == "SOLDER_MASK") {
                        sl.type = "soldermask";
                    } else if (func == "SILKSCREEN" || func == "SILK_SCREEN") {
                        sl.type = "silkscreen";
                    } else {
                        sl.type = "dielectric";
                    }
                    break;
                }
            }

            if (sl.type.empty()) {
                // Check if it's a dielectric layer (no matching layer definition)
                sl.type = "dielectric";
            }

            // Parse dielectric properties
            auto dielectric = layer_node.child("Dielectric");
            if (dielectric) {
                sl.epsilon_r = parse_double(
                    dielectric.attribute("epsilonR").as_string(), 4.5);
                sl.material = dielectric.attribute("material").as_string(sl.material.c_str());
            }

            total_thickness += sl.thickness;
            model.stackup.layers.push_back(sl);
        }
    }

    if (total_thickness > 0) {
        model.stackup.board_thickness = total_thickness;
    }

    log("Stackup: " + std::to_string(model.stackup.layers.size()) +
        " layers, total thickness: " + fmt(model.stackup.board_thickness) + " mm");
}

// --- Dictionary Parsing ---

void Ipc2581Parser::parse_dictionaries(const pugi::xml_node& content, PcbModel& model) {
    // Parse both standard and user dictionaries
    for (auto dict : content.children("DictionaryStandard")) {
        for (auto entry : dict.children("EntryStandard")) {
            parse_dictionary_entry(entry, model);
        }
    }
    for (auto dict : content.children("DictionaryUser")) {
        for (auto entry : dict.children("EntryUser")) {
            parse_dictionary_entry(entry, model);
        }
    }
    // Also handle Dictionary element (some files use this)
    for (auto dict : content.children("Dictionary")) {
        for (auto entry : dict.children("Entry")) {
            parse_dictionary_entry(entry, model);
        }
    }

    log("Parsed " + std::to_string(model.padstack_defs.size()) + " pad stack definitions");
}

void Ipc2581Parser::parse_dictionary_entry(const pugi::xml_node& entry, PcbModel& model) {
    std::string id = entry.attribute("id").as_string();
    if (id.empty()) return;

    PadStackDef ps;
    ps.name = id;

    // Look for pad features within the entry
    for (auto feature : entry.children()) {
        std::string tag = feature.name();

        if (tag == "Circle") {
            PadDef pad;
            pad.name = id;
            pad.shape = PadDef::CIRCLE;
            pad.width = to_mm(parse_double(feature.attribute("diameter").as_string()));
            pad.height = pad.width;
            pad.type = PadDef::SMD;
            ps.pads.push_back(pad);
        } else if (tag == "RectCenter") {
            PadDef pad;
            pad.name = id;
            pad.shape = PadDef::RECT;
            pad.width = to_mm(parse_double(feature.attribute("width").as_string()));
            pad.height = to_mm(parse_double(feature.attribute("height").as_string()));
            pad.type = PadDef::SMD;
            ps.pads.push_back(pad);
        } else if (tag == "RectRound") {
            PadDef pad;
            pad.name = id;
            pad.shape = PadDef::ROUNDRECT;
            pad.width = to_mm(parse_double(feature.attribute("width").as_string()));
            pad.height = to_mm(parse_double(feature.attribute("height").as_string()));
            pad.roundrect_ratio = 0.25;
            pad.type = PadDef::SMD;
            ps.pads.push_back(pad);
        } else if (tag == "Oval") {
            PadDef pad;
            pad.name = id;
            pad.shape = PadDef::OVAL;
            pad.width = to_mm(parse_double(feature.attribute("width").as_string()));
            pad.height = to_mm(parse_double(feature.attribute("height").as_string()));
            pad.type = PadDef::SMD;
            ps.pads.push_back(pad);
        } else if (tag == "Contour" || tag == "Polygon") {
            PadDef pad;
            pad.name = id;
            pad.shape = PadDef::CUSTOM;
            pad.custom_shape = parse_polygon(feature);
            // Scale custom shape points
            for (auto& pt : pad.custom_shape) {
                pt = to_mm(pt);
            }
            if (!pad.custom_shape.empty()) {
                // Compute bounding box for size
                double minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9;
                for (auto& pt : pad.custom_shape) {
                    minx = std::min(minx, pt.x);
                    maxx = std::max(maxx, pt.x);
                    miny = std::min(miny, pt.y);
                    maxy = std::max(maxy, pt.y);
                }
                pad.width = maxx - minx;
                pad.height = maxy - miny;
            }
            pad.type = PadDef::SMD;
            ps.pads.push_back(pad);
        } else if (tag == "Drill" || tag == "DrillHole") {
            ps.drill_diameter = to_mm(parse_double(
                feature.attribute("diameter").as_string()));
            ps.plated = parse_bool(feature.attribute("plated").as_string(), true);
        }
    }

    if (!ps.pads.empty() || ps.drill_diameter > 0) {
        model.padstack_defs[id] = ps;
    }
}

// --- Net Parsing ---

void Ipc2581Parser::parse_nets(const pugi::xml_node& root, PcbModel& model) {
    // Always add the unconnected net (id=0)
    model.nets.push_back({0, ""});
    model.net_name_to_id[""] = 0;

    int net_id = 1;
    for (auto net_node : root.children("LogicalNet")) {
        std::string name = net_node.attribute("name").as_string();
        if (name.empty()) continue;
        if (model.net_name_to_id.count(name)) continue;

        model.nets.push_back({net_id, name});
        model.net_name_to_id[name] = net_id;
        net_id++;
    }

    // Also look for nets inside Ecad/CadData/Step/PhyNetGroup
    auto ecad = root.child("Ecad");
    if (ecad) {
        auto cad_data = ecad.child("CadData");
        if (cad_data) {
            for (auto step : cad_data.children("Step")) {
                for (auto png : step.children("PhyNetGroup")) {
                    for (auto pn : png.children("PhyNet")) {
                        std::string name = pn.attribute("name").as_string();
                        if (name.empty() || model.net_name_to_id.count(name)) continue;
                        model.nets.push_back({net_id, name});
                        model.net_name_to_id[name] = net_id;
                        net_id++;
                    }
                }
            }
        }
    }

    log("Found " + std::to_string(model.nets.size() - 1) + " nets");
}

// --- Profile (Board Outline) Parsing ---

void Ipc2581Parser::parse_profile(const pugi::xml_node& step, PcbModel& model) {
    auto profile = step.child("Profile");
    if (!profile) {
        warn("No <Profile> found in step");
        return;
    }

    // Profile can contain Polygon, Polyline, or Circle
    auto polygon = profile.child("Polygon");
    if (polygon) {
        parse_contour(polygon, model.outline, model.outline_arcs);
        // Set layer to Edge.Cuts
        for (auto& seg : model.outline) seg.layer = "Edge.Cuts";
        for (auto& arc : model.outline_arcs) arc.layer = "Edge.Cuts";
    }

    auto polyline = profile.child("Polyline");
    if (polyline) {
        parse_contour(polyline, model.outline, model.outline_arcs);
        for (auto& seg : model.outline) seg.layer = "Edge.Cuts";
        for (auto& arc : model.outline_arcs) arc.layer = "Edge.Cuts";
    }

    // Circle outline
    auto circle = profile.child("Circle");
    if (circle) {
        double cx = to_mm(parse_double(circle.attribute("centerX").as_string()));
        double cy = to_mm(parse_double(circle.attribute("centerY").as_string()));
        double r = to_mm(parse_double(circle.attribute("radius").as_string()));
        if (r <= 0) {
            r = to_mm(parse_double(circle.attribute("diameter").as_string())) / 2.0;
        }

        // Create a circle as four arcs
        Point center(cx, cy);
        Point top(cx, cy + r), right(cx + r, cy), bottom(cx, cy - r), left(cx - r, cy);
        auto a1 = arc_center_to_mid(right, center, 90, 0.05, "Edge.Cuts");
        auto a2 = arc_center_to_mid(top, center, 90, 0.05, "Edge.Cuts");
        auto a3 = arc_center_to_mid(left, center, 90, 0.05, "Edge.Cuts");
        auto a4 = arc_center_to_mid(bottom, center, 90, 0.05, "Edge.Cuts");
        model.outline_arcs.push_back(a1);
        model.outline_arcs.push_back(a2);
        model.outline_arcs.push_back(a3);
        model.outline_arcs.push_back(a4);
    }

    log("Board outline: " + std::to_string(model.outline.size()) + " segments, " +
        std::to_string(model.outline_arcs.size()) + " arcs");
}

void Ipc2581Parser::parse_contour(const pugi::xml_node& contour,
                                  std::vector<Segment>& segs,
                                  std::vector<ArcGeom>& arcs) {
    Point first_pt, last_pt;
    bool has_first = false;

    for (auto child : contour.children()) {
        std::string tag = child.name();

        if (tag == "PolyBegin") {
            double x = to_mm(parse_double(child.attribute("x").as_string()));
            double y = to_mm(parse_double(child.attribute("y").as_string()));
            first_pt = last_pt = ipc_to_kicad_coords({x, y});
            has_first = true;
        } else if (tag == "PolyStepSegment") {
            double x = to_mm(parse_double(child.attribute("x").as_string()));
            double y = to_mm(parse_double(child.attribute("y").as_string()));
            Point pt = ipc_to_kicad_coords({x, y});
            Segment seg;
            seg.start = last_pt;
            seg.end = pt;
            seg.width = 0.05; // thin outline
            segs.push_back(seg);
            last_pt = pt;
        } else if (tag == "PolyStepCurve") {
            double x = to_mm(parse_double(child.attribute("x").as_string()));
            double y = to_mm(parse_double(child.attribute("y").as_string()));
            double cx = to_mm(parse_double(child.attribute("centerX").as_string()));
            double cy = to_mm(parse_double(child.attribute("centerY").as_string()));
            bool cw = parse_bool(child.attribute("clockwise").as_string(), false);

            Point end_pt = ipc_to_kicad_coords({x, y});
            Point center = ipc_to_kicad_coords({cx, cy});

            // Compute sweep angle
            double start_ang = std::atan2(last_pt.y - center.y, last_pt.x - center.x);
            double end_ang = std::atan2(end_pt.y - center.y, end_pt.x - center.x);
            double sweep = end_ang - start_ang;

            // Y is negated by ipc_to_kicad_coords, which inverts CW/CCW.
            // IPC CW becomes KiCad CCW (positive sweep) and vice versa.
            if (cw) {
                // IPC CW -> KiCad CCW (positive sweep)
                if (sweep <= 0) sweep += 2 * PI;
            } else {
                // IPC CCW -> KiCad CW (negative sweep)
                if (sweep >= 0) sweep -= 2 * PI;
            }

            auto arc = arc_center_to_mid(last_pt, center, rad_to_deg(sweep), 0.05, "");
            arcs.push_back(arc);
            last_pt = end_pt;
        } else if (tag == "Line") {
            double sx = to_mm(parse_double(child.attribute("startX").as_string()));
            double sy = to_mm(parse_double(child.attribute("startY").as_string()));
            double ex = to_mm(parse_double(child.attribute("endX").as_string()));
            double ey = to_mm(parse_double(child.attribute("endY").as_string()));
            Segment seg;
            seg.start = ipc_to_kicad_coords({sx, sy});
            seg.end = ipc_to_kicad_coords({ex, ey});
            seg.width = 0.05;
            segs.push_back(seg);
            last_pt = seg.end;
            if (!has_first) { first_pt = seg.start; has_first = true; }
        }
    }
}

// --- Package (Footprint) Parsing ---

void Ipc2581Parser::parse_packages(const pugi::xml_node& step, PcbModel& model) {
    for (auto pkg : step.children("Package")) {
        std::string name = pkg.attribute("name").as_string();
        if (name.empty()) continue;

        Footprint fp;
        fp.name = name;

        // Parse pads/pins
        int pad_num = 1;
        for (auto child : pkg.children()) {
            std::string tag = child.name();

            if (tag == "Pin" || tag == "Pad") {
                PadDef pad;
                pad.name = child.attribute("number").as_string(
                    std::to_string(pad_num).c_str());

                // Read offset from attributes or child Location element
                double x = to_mm(parse_double(child.attribute("x").as_string()));
                double y = to_mm(parse_double(child.attribute("y").as_string()));
                auto pin_loc = child.child("Location");
                if (pin_loc) {
                    x = to_mm(parse_double(pin_loc.attribute("x").as_string()));
                    y = to_mm(parse_double(pin_loc.attribute("y").as_string()));
                }
                pad.offset = ipc_to_kicad_coords({x, y});
                pad.rotation = parse_double(child.attribute("rotation").as_string());

                // Look up pad stack definition from attribute or child element
                std::string ps_ref = child.attribute("padstackDefRef").as_string();
                if (ps_ref.empty()) {
                    ps_ref = child.attribute("padRef").as_string();
                }
                if (ps_ref.empty()) {
                    auto std_prim = child.child("StandardPrimitiveRef");
                    if (std_prim) {
                        ps_ref = std_prim.attribute("id").as_string();
                    }
                }

                if (!ps_ref.empty() && model.padstack_defs.count(ps_ref)) {
                    auto& ps = model.padstack_defs[ps_ref];
                    if (!ps.pads.empty()) {
                        pad.shape = ps.pads[0].shape;
                        pad.width = ps.pads[0].width;
                        pad.height = ps.pads[0].height;
                        pad.custom_shape = ps.pads[0].custom_shape;
                        pad.roundrect_ratio = ps.pads[0].roundrect_ratio;
                    }
                    if (ps.drill_diameter > 0) {
                        pad.drill_diameter = ps.drill_diameter;
                        pad.type = ps.plated ? PadDef::THRU_HOLE : PadDef::NPTH;
                        pad.layer_side = "ALL";
                    } else {
                        pad.type = PadDef::SMD;
                        pad.layer_side = "TOP"; // will be flipped per component
                    }
                    fp.pad_to_padstack[pad.name] = ps_ref;
                } else {
                    // Default pad
                    pad.shape = PadDef::CIRCLE;
                    pad.width = to_mm(0.5);
                    pad.height = pad.width;
                    pad.type = PadDef::SMD;
                    pad.layer_side = "TOP";
                }

                fp.pads.push_back(pad);
                pad_num++;
            } else if (tag == "SilkScreen" || tag == "Outline" || tag == "Courtyard" ||
                       tag == "AssemblyDrawing") {
                // Parse graphic features within the package
                for (auto feat : child.children()) {
                    std::string ftag = feat.name();
                    GraphicItem gi;

                    std::string layer;
                    if (tag == "SilkScreen") layer = "F.SilkS";
                    else if (tag == "Courtyard") layer = "F.CrtYd";
                    else if (tag == "AssemblyDrawing") layer = "F.Fab";
                    else layer = "F.Fab";

                    if (ftag == "Line") {
                        gi.kind = GraphicItem::LINE;
                        double sx = to_mm(parse_double(feat.attribute("startX").as_string()));
                        double sy = to_mm(parse_double(feat.attribute("startY").as_string()));
                        double ex = to_mm(parse_double(feat.attribute("endX").as_string()));
                        double ey = to_mm(parse_double(feat.attribute("endY").as_string()));
                        gi.start = ipc_to_kicad_coords({sx, sy});
                        gi.end = ipc_to_kicad_coords({ex, ey});
                        gi.width = to_mm(parse_double(feat.attribute("lineWidth").as_string(), 0.1));
                        gi.layer = layer;
                        fp.graphics.push_back(gi);
                    } else if (ftag == "Arc") {
                        gi.kind = GraphicItem::ARC;
                        double sx = to_mm(parse_double(feat.attribute("startX").as_string()));
                        double sy = to_mm(parse_double(feat.attribute("startY").as_string()));
                        double ex = to_mm(parse_double(feat.attribute("endX").as_string()));
                        double ey = to_mm(parse_double(feat.attribute("endY").as_string()));
                        double cx = to_mm(parse_double(feat.attribute("centerX").as_string()));
                        double cy = to_mm(parse_double(feat.attribute("centerY").as_string()));
                        gi.start = ipc_to_kicad_coords({sx, sy});
                        gi.end = ipc_to_kicad_coords({ex, ey});
                        gi.center = ipc_to_kicad_coords({cx, cy});
                        gi.width = to_mm(parse_double(feat.attribute("lineWidth").as_string(), 0.1));
                        gi.layer = layer;
                        fp.graphics.push_back(gi);
                    }
                }
            }
        }

        model.footprint_defs[name] = fp;
    }

    log("Parsed " + std::to_string(model.footprint_defs.size()) + " package definitions");
}

// --- Component Parsing ---

void Ipc2581Parser::parse_components(const pugi::xml_node& step, PcbModel& model) {
    for (auto comp : step.children("Component")) {
        ComponentInstance ci;
        ci.refdes = comp.attribute("refDes").as_string();
        if (ci.refdes.empty()) {
            ci.refdes = comp.attribute("name").as_string();
        }
        ci.footprint_ref = comp.attribute("packageRef").as_string();
        ci.value = comp.attribute("value").as_string();

        std::string layer_ref = comp.attribute("layerRef").as_string();

        // Parse Xform (transform) and Location
        auto xform = comp.child("Xform");
        auto location = comp.child("Location");

        if (xform) {
            ci.rotation = parse_double(xform.attribute("rotation").as_string());
            ci.mirror = parse_bool(xform.attribute("mirror").as_string(), false);
            // Some files put x/y on Xform directly
            double x = to_mm(parse_double(xform.attribute("x").as_string()));
            double y = to_mm(parse_double(xform.attribute("y").as_string()));
            if (x != 0.0 || y != 0.0) {
                ci.position = ipc_to_kicad_coords({x, y});
            }
        }

        // Location element (may exist alongside or instead of Xform)
        if (location) {
            double x = to_mm(parse_double(location.attribute("x").as_string()));
            double y = to_mm(parse_double(location.attribute("y").as_string()));
            if (x != 0.0 || y != 0.0 || (ci.position.x == 0.0 && ci.position.y == 0.0)) {
                ci.position = ipc_to_kicad_coords({x, y});
            }
        }

        // Determine if bottom side from layer reference
        if (!layer_ref.empty()) {
            std::string kicad_layer = model.get_kicad_layer(layer_ref);
            if (kicad_layer == "B.Cu") {
                ci.mirror = true;
            }
        }

        // Parse pin-to-net mapping from component's NonstandardAttribute or from LogicalNet
        for (auto pin : comp.children("Pin")) {
            std::string pin_name = pin.attribute("number").as_string();
            std::string net_name = pin.attribute("net").as_string();
            if (!pin_name.empty() && !net_name.empty()) {
                ci.pin_net_map[pin_name] = net_name;
            }
        }

        if (!ci.refdes.empty()) {
            model.components.push_back(ci);
        }
    }

    // Also parse pin-net from LogicalNet/PinRef
    auto root = step.parent().parent().parent(); // Step -> CadData -> Ecad -> IPC-2581
    for (auto net_node : root.children("LogicalNet")) {
        std::string net_name = net_node.attribute("name").as_string();
        for (auto pin_ref : net_node.children("PinRef")) {
            std::string comp_ref = pin_ref.attribute("componentRef").as_string();
            std::string pin_num = pin_ref.attribute("pin").as_string();
            if (comp_ref.empty() || pin_num.empty()) continue;

            // Find the component and update its pin map
            for (auto& ci : model.components) {
                if (ci.refdes == comp_ref) {
                    ci.pin_net_map[pin_num] = net_name;
                    break;
                }
            }
        }
    }

    log("Parsed " + std::to_string(model.components.size()) + " component instances");
}

// --- PadStack Rotation Extraction ---

void Ipc2581Parser::parse_padstack_rotations(const pugi::xml_node& step, PcbModel& model) {
    // Build a set of copper layer names for filtering
    std::set<std::string> copper_layers;
    for (auto& l : model.layers) {
        std::string func = l.ipc_function;
        std::transform(func.begin(), func.end(), func.begin(), ::toupper);
        if (func == "SIGNAL" || func == "POWER_GROUND" || func == "POWER" ||
            func == "GROUND" || func == "MIXED") {
            copper_layers.insert(l.ipc_name);
        }
    }

    // Build a lookup for component rotation by refdes
    std::map<std::string, double> comp_rotation;
    for (auto& ci : model.components) {
        comp_rotation[ci.refdes] = ci.rotation;
    }

    int count = 0;
    for (auto ps : step.children("PadStack")) {
        for (auto lp : ps.children("LayerPad")) {
            std::string layer_ref = lp.attribute("layerRef").as_string();

            // Only process copper layers to avoid duplicate entries
            if (copper_layers.find(layer_ref) == copper_layers.end())
                continue;

            auto xform = lp.child("Xform");
            if (!xform) continue;

            double abs_rotation = parse_double(xform.attribute("rotation").as_string());
            if (abs_rotation == 0.0) continue;

            auto pin_ref = lp.child("PinRef");
            if (!pin_ref) continue;

            std::string comp_ref = pin_ref.attribute("componentRef").as_string();
            std::string pin = pin_ref.attribute("pin").as_string();
            if (comp_ref.empty() || pin.empty()) continue;

            auto rot_it = comp_rotation.find(comp_ref);
            if (rot_it == comp_rotation.end()) continue;

            double comp_rot = rot_it->second;
            double local_rotation = std::fmod(abs_rotation - comp_rot + 360.0, 360.0);

            if (local_rotation == 0.0) continue;

            // Store on the matching component instance
            for (auto& ci : model.components) {
                if (ci.refdes == comp_ref) {
                    ci.pin_rotation_map[pin] = local_rotation;
                    count++;
                    break;
                }
            }
        }
    }

    if (count > 0) {
        log("Extracted " + std::to_string(count) + " per-pad rotations from PadStack data");
    }
}

// --- PadStack Via & Pin-Net Extraction ---

void Ipc2581Parser::parse_padstack_vias(const pugi::xml_node& step, PcbModel& model) {
    int via_count = 0;
    int pin_net_count = 0;

    for (auto ps : step.children("PadStack")) {
        std::string net_name = ps.attribute("net").as_string();

        // Extract pin-to-net mappings from PinRef elements in LayerPads
        if (!net_name.empty() && net_name != "No Net") {
            for (auto lp : ps.children("LayerPad")) {
                auto pin_ref = lp.child("PinRef");
                if (!pin_ref) continue;

                std::string comp_ref = pin_ref.attribute("componentRef").as_string();
                std::string pin = pin_ref.attribute("pin").as_string();
                if (comp_ref.empty() || pin.empty()) continue;

                // Find the component and set pin-net mapping (only once per pin)
                for (auto& ci : model.components) {
                    if (ci.refdes == comp_ref) {
                        if (ci.pin_net_map.find(pin) == ci.pin_net_map.end()) {
                            ci.pin_net_map[pin] = net_name;
                            pin_net_count++;
                        }
                        break;
                    }
                }
            }
        }

        // Extract vias from PadStacks with LayerHole platingStatus="VIA"
        auto layer_hole = ps.child("LayerHole");
        if (!layer_hole) continue;

        std::string plating = layer_hole.attribute("platingStatus").as_string();
        if (plating != "VIA") continue;

        double drill = to_mm(parse_double(layer_hole.attribute("diameter").as_string()));
        double x = to_mm(parse_double(layer_hole.attribute("x").as_string()));
        double y = to_mm(parse_double(layer_hole.attribute("y").as_string()));
        Point pos = ipc_to_kicad_coords({x, y});

        // Determine layer span
        std::string start_layer = "F.Cu";
        std::string end_layer = "B.Cu";
        auto span = layer_hole.child("Span");
        if (span) {
            std::string from = model.get_kicad_layer(
                span.attribute("fromLayer").as_string());
            std::string to = model.get_kicad_layer(
                span.attribute("toLayer").as_string());
            if (!from.empty()) start_layer = from;
            if (!to.empty()) end_layer = to;
        }

        // Get pad diameter from LayerPad's StandardPrimitiveRef
        double pad_diameter = drill * 2.0;
        auto layer_pad = ps.child("LayerPad");
        if (layer_pad) {
            auto prim_ref = layer_pad.child("StandardPrimitiveRef");
            if (prim_ref) {
                std::string prim_id = prim_ref.attribute("id").as_string();
                if (!prim_id.empty() && model.padstack_defs.count(prim_id)) {
                    auto& psd = model.padstack_defs[prim_id];
                    if (!psd.pads.empty()) {
                        pad_diameter = psd.pads[0].width;
                    }
                }
            }
        }

        int net_id = model.get_net_id(net_name);
        if (net_id == 0 && !net_name.empty()) {
            net_id = static_cast<int>(model.nets.size());
            model.nets.push_back({net_id, net_name});
            model.net_name_to_id[net_name] = net_id;
        }

        Via via;
        via.position = pos;
        via.drill = drill;
        via.diameter = pad_diameter;
        via.start_layer = start_layer;
        via.end_layer = end_layer;
        via.net_id = net_id;
        model.vias.push_back(via);
        via_count++;
    }

    if (via_count > 0) {
        log("Extracted " + std::to_string(via_count) + " vias from PadStack elements");
    }
    if (pin_net_count > 0) {
        log("Extracted " + std::to_string(pin_net_count) + " pin-net mappings from PadStack elements");
    }
}

// --- Layer Feature Parsing ---

void Ipc2581Parser::parse_layer_features(const pugi::xml_node& step, PcbModel& model) {
    for (auto lf : step.children("LayerFeature")) {
        std::string ipc_layer = lf.attribute("layerRef").as_string();
        std::string kicad_layer = model.get_kicad_layer(ipc_layer);

        if (kicad_layer.empty()) {
            // Skip unmapped layers
            continue;
        }

        for (auto set_node : lf.children("Set")) {
            std::string net_name = set_node.attribute("net").as_string();
            int net_id = model.get_net_id(net_name);

            // If net not found but name exists, add it
            if (net_id == 0 && !net_name.empty()) {
                net_id = static_cast<int>(model.nets.size());
                model.nets.push_back({net_id, net_name});
                model.net_name_to_id[net_name] = net_id;
            }

            // Collect all feature nodes — some files place them directly
            // under Set, others nest them inside Features/UserSpecial.
            std::vector<pugi::xml_node> feat_nodes;
            for (auto child : set_node.children()) {
                std::string ctag = child.name();
                if (ctag == "Features" || ctag == "UserSpecial") {
                    for (auto gc : child.children()) {
                        std::string gctag = gc.name();
                        if (gctag == "Features" || gctag == "UserSpecial") {
                            for (auto ggc : gc.children()) {
                                feat_nodes.push_back(ggc);
                            }
                        } else {
                            feat_nodes.push_back(gc);
                        }
                    }
                } else {
                    feat_nodes.push_back(child);
                }
            }

            for (auto& feat : feat_nodes) {
                std::string tag = feat.name();

                if (tag == "Line") {
                    TraceSegment ts;
                    double sx = to_mm(parse_double(feat.attribute("startX").as_string()));
                    double sy = to_mm(parse_double(feat.attribute("startY").as_string()));
                    double ex = to_mm(parse_double(feat.attribute("endX").as_string()));
                    double ey = to_mm(parse_double(feat.attribute("endY").as_string()));
                    ts.start = ipc_to_kicad_coords({sx, sy});
                    ts.end = ipc_to_kicad_coords({ex, ey});
                    ts.width = to_mm(parse_double(feat.attribute("lineWidth").as_string(), 0.25));
                    // Some files put lineWidth in a child LineDesc element
                    auto line_desc = feat.child("LineDesc");
                    if (line_desc) {
                        double lw = parse_double(line_desc.attribute("lineWidth").as_string());
                        if (lw > 0) ts.width = to_mm(lw);
                    }
                    ts.layer = kicad_layer;
                    ts.net_id = net_id;

                    // Only add as trace if on copper layer
                    if (kicad_layer.find(".Cu") != std::string::npos) {
                        model.traces.push_back(ts);
                    } else {
                        GraphicItem gi;
                        gi.kind = GraphicItem::LINE;
                        gi.start = ts.start;
                        gi.end = ts.end;
                        gi.width = ts.width;
                        gi.layer = kicad_layer;
                        model.graphics.push_back(gi);
                    }
                } else if (tag == "Arc") {
                    double sx = to_mm(parse_double(feat.attribute("startX").as_string()));
                    double sy = to_mm(parse_double(feat.attribute("startY").as_string()));
                    double ex = to_mm(parse_double(feat.attribute("endX").as_string()));
                    double ey = to_mm(parse_double(feat.attribute("endY").as_string()));
                    double cx = to_mm(parse_double(feat.attribute("centerX").as_string()));
                    double cy = to_mm(parse_double(feat.attribute("centerY").as_string()));
                    bool cw = parse_bool(feat.attribute("clockwise").as_string(), false);
                    double width = to_mm(parse_double(feat.attribute("lineWidth").as_string(), 0.25));

                    Point start_pt = ipc_to_kicad_coords({sx, sy});
                    Point end_pt = ipc_to_kicad_coords({ex, ey});
                    Point center = ipc_to_kicad_coords({cx, cy});

                    double start_ang = std::atan2(start_pt.y - center.y, start_pt.x - center.x);
                    double end_ang = std::atan2(end_pt.y - center.y, end_pt.x - center.x);
                    double sweep = end_ang - start_ang;
                    // Y negation inverts CW/CCW
                    if (cw) { if (sweep <= 0) sweep += 2 * PI; }
                    else { if (sweep >= 0) sweep -= 2 * PI; }

                    auto arc = arc_center_to_mid(start_pt, center, rad_to_deg(sweep),
                                                 width, kicad_layer);

                    if (kicad_layer.find(".Cu") != std::string::npos) {
                        TraceArc ta;
                        ta.start = arc.start;
                        ta.mid = arc.mid;
                        ta.end = arc.end;
                        ta.width = width;
                        ta.layer = kicad_layer;
                        ta.net_id = net_id;
                        model.trace_arcs.push_back(ta);
                    } else {
                        GraphicItem gi;
                        gi.kind = GraphicItem::ARC;
                        gi.start = arc.start;
                        gi.center = arc.mid; // store mid in center for arcs
                        gi.end = arc.end;
                        gi.width = width;
                        gi.layer = kicad_layer;
                        model.graphics.push_back(gi);
                    }
                } else if (tag == "Pad" || tag == "PadRef") {
                    // A pad placed as a via or standalone pad in routing
                    std::string ps_ref = feat.attribute("padstackDefRef").as_string();
                    if (ps_ref.empty()) ps_ref = feat.attribute("padRef").as_string();

                    double x = to_mm(parse_double(feat.attribute("x").as_string()));
                    double y = to_mm(parse_double(feat.attribute("y").as_string()));
                    Point pos = ipc_to_kicad_coords({x, y});

                    if (!ps_ref.empty() && model.padstack_defs.count(ps_ref)) {
                        auto& ps = model.padstack_defs[ps_ref];
                        if (ps.drill_diameter > 0) {
                            // This is a via
                            Via via;
                            via.position = pos;
                            via.drill = ps.drill_diameter;
                            via.diameter = ps.pads.empty() ? ps.drill_diameter * 2.0
                                                           : ps.pads[0].width;
                            via.start_layer = "F.Cu";
                            via.end_layer = "B.Cu";
                            via.net_id = net_id;
                            model.vias.push_back(via);
                        }
                    }
                } else if (tag == "Polygon" || tag == "Polyline" || tag == "Contour") {
                    // Copper pour / zone
                    // Contour wraps a Polygon (outline) + Cutout children (holes)
                    pugi::xml_node outline_node = feat;
                    std::vector<std::vector<Point>> hole_polys;

                    if (tag == "Contour") {
                        auto inner_poly = feat.child("Polygon");
                        if (inner_poly) {
                            outline_node = inner_poly;
                        }
                        for (auto cutout : feat.children("Cutout")) {
                            auto hole = parse_polygon(cutout);
                            for (auto& pt : hole) {
                                pt = ipc_to_kicad_coords(to_mm(pt));
                            }
                            if (!hole.empty()) {
                                hole_polys.push_back(hole);
                            }
                        }
                    }

                    if (kicad_layer.find(".Cu") != std::string::npos) {
                        Zone zone;
                        zone.layer = kicad_layer;
                        zone.net_id = net_id;
                        zone.net_name = net_name;
                        zone.outline = parse_polygon(outline_node);
                        for (auto& pt : zone.outline) {
                            pt = ipc_to_kicad_coords(to_mm(pt));
                        }
                        zone.holes = hole_polys;
                        if (!zone.outline.empty()) {
                            model.zones.push_back(zone);
                        }
                    } else {
                        // Non-copper polygon → graphic
                        GraphicItem gi;
                        gi.kind = GraphicItem::POLYGON;
                        gi.points = parse_polygon(outline_node);
                        for (auto& pt : gi.points) {
                            pt = ipc_to_kicad_coords(to_mm(pt));
                        }
                        gi.layer = kicad_layer;
                        gi.fill = true;
                        model.graphics.push_back(gi);
                    }
                }
            }
        }
    }

    log("Layer features: " + std::to_string(model.traces.size()) + " traces, " +
        std::to_string(model.trace_arcs.size()) + " arcs, " +
        std::to_string(model.vias.size()) + " vias, " +
        std::to_string(model.zones.size()) + " zones, " +
        std::to_string(model.graphics.size()) + " graphics");
}

// --- Geometry Helpers ---

std::vector<Point> Ipc2581Parser::parse_polygon(const pugi::xml_node& node) {
    std::vector<Point> pts;

    for (auto child : node.children()) {
        std::string tag = child.name();
        if (tag == "PolyBegin") {
            double x = parse_double(child.attribute("x").as_string());
            double y = parse_double(child.attribute("y").as_string());
            pts.push_back({x, y});
        } else if (tag == "PolyStepSegment") {
            double x = parse_double(child.attribute("x").as_string());
            double y = parse_double(child.attribute("y").as_string());
            pts.push_back({x, y});
        } else if (tag == "PolyStepCurve") {
            // Approximate curve as endpoint (simplified)
            double x = parse_double(child.attribute("x").as_string());
            double y = parse_double(child.attribute("y").as_string());
            pts.push_back({x, y});
        } else if (tag == "Point" || tag == "Vertex") {
            double x = parse_double(child.attribute("x").as_string());
            double y = parse_double(child.attribute("y").as_string());
            pts.push_back({x, y});
        }
    }

    return pts;
}

std::vector<Point> Ipc2581Parser::parse_polyline(const pugi::xml_node& node) {
    return parse_polygon(node); // same structure
}

std::string Ipc2581Parser::determine_layer_side(const std::string& ipc_name,
                                                 const std::string& layer_function,
                                                 int layer_index,
                                                 int total_layers) {
    std::string name_upper = ipc_name;
    std::transform(name_upper.begin(), name_upper.end(), name_upper.begin(), ::toupper);

    if (name_upper.find("TOP") != std::string::npos) return "TOP";
    if (name_upper.find("BOTTOM") != std::string::npos ||
        name_upper.find("BOT") != std::string::npos) return "BOTTOM";
    if (layer_index == 0) return "TOP";
    if (layer_index == total_layers - 1) return "BOTTOM";
    return "INTERNAL";
}

// --- Logging ---

void Ipc2581Parser::log(const std::string& msg) {
    if (opts_.verbose) {
        std::cout << "[IPC2581] " << msg << std::endl;
    }
}

void Ipc2581Parser::warn(const std::string& msg) {
    warnings_.push_back(msg);
    std::cerr << "[WARNING] " << msg << std::endl;
}

} // namespace ipc2kicad
