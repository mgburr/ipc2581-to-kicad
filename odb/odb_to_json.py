#!/usr/bin/env python3
"""ODB++ to JSON bridge.

Parses an ODB++ archive and outputs JSON matching the schema produced by
json_export.cpp, so that the C++ json_import module can read it into the
same PcbModel consumed by the KiCad writer.

Usage:
    python3 odb/odb_to_json.py input.tgz [-v] [-s step] [--list-steps]
"""

import argparse
import json
import logging
import math
import sys
from pathlib import Path

from .pcb_model import (
    ComponentInstance, Footprint, FootprintPad, GraphicItem,
    LayerDef, LayerType, NetDef, PadDef, PadShape, PcbModel,
    Point, Side, StackupLayer, TraceArc, TraceSegment, Via,
    Zone, ZonePolygon,
)
from .odb_parser import parse_odb, OdbParser
from .utils import fmt

log = logging.getLogger(__name__)


def _point(p):
    """Format a Point as [x, y] list."""
    return [float(fmt(p.x)), float(fmt(p.y))]


def _pad_shape_str(shape):
    return {
        PadShape.CIRCLE: "circle",
        PadShape.RECT: "rect",
        PadShape.OVAL: "oval",
        PadShape.ROUNDRECT: "roundrect",
        PadShape.CUSTOM: "custom",
    }.get(shape, "rect")


def _layer_type_str(lt):
    return {
        LayerType.SIGNAL: "signal",
        LayerType.POWER: "power",
        LayerType.MIXED: "mixed",
        LayerType.SOLDER_MASK: "user",
        LayerType.SILK_SCREEN: "user",
        LayerType.SOLDER_PASTE: "user",
        LayerType.DRILL: "user",
        LayerType.DOCUMENT: "user",
        LayerType.COMPONENT: "user",
        LayerType.OTHER: "user",
    }.get(lt, "user")


def _ipc_function_str(lt):
    return {
        LayerType.SIGNAL: "SIGNAL",
        LayerType.POWER: "POWER_GROUND",
        LayerType.MIXED: "SIGNAL",
        LayerType.SOLDER_MASK: "SOLDERMASK",
        LayerType.SILK_SCREEN: "SILKSCREEN",
        LayerType.SOLDER_PASTE: "PASTEMASK",
        LayerType.DRILL: "DRILL",
        LayerType.DOCUMENT: "DOCUMENT",
        LayerType.COMPONENT: "ASSEMBLY",
        LayerType.OTHER: "DOCUMENT",
    }.get(lt, "DOCUMENT")


def _side_str(side):
    return {
        Side.TOP: "TOP",
        Side.BOTTOM: "BOTTOM",
        Side.BOTH: "ALL",
    }.get(side, "ALL")


def _rotate_point(x, y, angle_deg):
    """Rotate point (x,y) around origin by angle_deg (counter-clockwise)."""
    if abs(angle_deg) < 0.001:
        return x, y
    rad = math.radians(angle_deg)
    cos_a = math.cos(rad)
    sin_a = math.sin(rad)
    return x * cos_a - y * sin_a, x * sin_a + y * cos_a


def model_to_json(model):
    """Convert a Python PcbModel to a JSON-serializable dict matching json_export.cpp schema."""

    result = {}

    # ── outline ──────────────────────────────────────────────────────
    segments = []
    arcs = []
    for item in model.outline:
        if item.item_type == "line":
            segments.append({
                "start": _point(item.start),
                "end": _point(item.end),
                "width": float(fmt(item.width)),
            })
        elif item.item_type == "arc" and item.mid is not None:
            arcs.append({
                "start": _point(item.start),
                "mid": _point(item.mid),
                "end": _point(item.end),
                "width": float(fmt(item.width)),
            })
    result["outline"] = {"segments": segments, "arcs": arcs}

    # ── layers ───────────────────────────────────────────────────────
    result["layers"] = []
    for layer in model.layers:
        result["layers"].append({
            "kicad_id": layer.layer_id,
            "kicad_name": layer.kicad_name,
            "type": _layer_type_str(layer.layer_type),
            "ipc_name": layer.odb_name,
            "ipc_function": _ipc_function_str(layer.layer_type),
            "ipc_side": _side_str(layer.side),
            "copper_order": layer.copper_order,
        })

    # ── nets ─────────────────────────────────────────────────────────
    result["nets"] = []
    for nd in model.nets:
        result["nets"].append({"id": nd.index, "name": nd.name})

    # ── stackup ──────────────────────────────────────────────────────
    stackup_layers = []
    for sl in model.stackup:
        stackup_layers.append({
            "name": sl.name,
            "type": sl.layer_type,
            "thickness": float(fmt(sl.thickness)),
            "material": sl.material,
            "epsilon_r": float(fmt(sl.epsilon_r)),
            "kicad_layer_id": -1,
        })
    result["stackup"] = {
        "board_thickness": float(fmt(model.board_thickness)),
        "layers": stackup_layers,
    }

    # ── footprints & components ──────────────────────────────────────
    # Build de-duplicated footprint definitions from per-component footprints.
    # Convert pad positions from absolute to footprint-relative.
    footprint_defs = {}
    components_json = []

    for comp in model.components:
        fp = comp.footprint
        if fp is None:
            continue

        fp_name = fp.name or comp.footprint_name or comp.reference

        # Build footprint definition if not already seen
        if fp_name not in footprint_defs:
            pads_json = []
            for pad in fp.pads:
                # Convert absolute pad position to footprint-relative:
                # Subtract component position, then un-rotate by component rotation
                rel_x = pad.pos.x - comp.pos.x
                rel_y = pad.pos.y - comp.pos.y
                if abs(comp.rotation) > 0.001:
                    rel_x, rel_y = _rotate_point(rel_x, rel_y, -comp.rotation)
                if comp.side == Side.BOTTOM:
                    rel_x = -rel_x  # mirror X for bottom

                # Determine pad type string
                pad_type = pad.pad_type if pad.pad_type else "smd"

                # Determine layer_side
                layer_side = "TOP"
                if comp.side == Side.BOTTOM:
                    layer_side = "BOTTOM"
                if pad.pad_type == "thru_hole":
                    layer_side = "ALL"

                pd = pad.pad_def
                pad_json = {
                    "name": pad.number,
                    "shape": _pad_shape_str(pd.shape),
                    "width": float(fmt(pd.width)),
                    "height": float(fmt(pd.height)),
                    "drill_diameter": float(fmt(pd.drill)),
                    "offset": [0.0, 0.0],
                    "roundrect_ratio": float(fmt(pd.roundrect_ratio)),
                    "type": pad_type,
                    "layer_side": layer_side,
                    "rotation": float(fmt(pad.rotation)),
                }
                if pd.custom_outline:
                    pad_json["custom_shape"] = [[p[0], p[1]] for p in pd.custom_outline]

                # Store the relative position as the pad offset from origin
                # In json_export schema, pad position is part of the pad definition
                # within the footprint, stored in the pad's offset or implicitly
                # via the pad's position in the footprint's coordinate space
                pad_json["offset"] = [float(fmt(rel_x)), float(fmt(rel_y))]
                pads_json.append(pad_json)

            footprint_defs[fp_name] = {
                "name": fp_name,
                "origin": [0.0, 0.0],
                "pads": pads_json,
                "graphics": [],
            }

        # Build component instance
        pin_net_map = {}
        for pad in fp.pads:
            if pad.net_name:
                pin_net_map[pad.number] = pad.net_name

        components_json.append({
            "refdes": comp.reference,
            "footprint_ref": fp_name,
            "value": comp.properties.get("VALUE", comp.properties.get("COMP_VALUE", "")),
            "position": _point(comp.pos),
            "rotation": float(fmt(comp.rotation)),
            "mirror": comp.side == Side.BOTTOM,
            "pin_net_map": pin_net_map,
        })

    result["footprints"] = footprint_defs
    result["components"] = components_json

    # ── traces ───────────────────────────────────────────────────────
    result["traces"] = []
    for t in model.traces:
        result["traces"].append({
            "start": _point(t.start),
            "end": _point(t.end),
            "width": float(fmt(t.width)),
            "layer": t.layer,
            "net_id": t.net_index,
        })

    # ── trace_arcs ───────────────────────────────────────────────────
    result["trace_arcs"] = []
    for a in model.arcs:
        result["trace_arcs"].append({
            "start": _point(a.start),
            "mid": _point(a.mid),
            "end": _point(a.end),
            "width": float(fmt(a.width)),
            "layer": a.layer,
            "net_id": a.net_index,
        })

    # ── vias ─────────────────────────────────────────────────────────
    result["vias"] = []
    for v in model.vias:
        start_layer = v.layers[0] if len(v.layers) > 0 else "F.Cu"
        end_layer = v.layers[1] if len(v.layers) > 1 else "B.Cu"
        result["vias"].append({
            "position": _point(v.pos),
            "diameter": float(fmt(v.diameter)),
            "drill": float(fmt(v.drill)),
            "start_layer": start_layer,
            "end_layer": end_layer,
            "net_id": v.net_index,
        })

    # ── zones ────────────────────────────────────────────────────────
    # Flatten: one JSON zone per ZonePolygon
    result["zones"] = []
    for zone in model.zones:
        for poly in zone.polygons:
            outline_pts = [_point(p) for p in poly.outline]
            holes_pts = [[_point(p) for p in hole] for hole in poly.holes]
            zone_json = {
                "layer": zone.layer,
                "net_id": zone.net_index,
                "net_name": zone.net_name,
                "min_thickness": 0.25,
                "clearance": 0.5,
                "outline": outline_pts,
            }
            if holes_pts:
                zone_json["holes"] = holes_pts
            result["zones"].append(zone_json)

    # ── graphics ─────────────────────────────────────────────────────
    result["graphics"] = []
    for g in model.graphics:
        gj = {
            "kind": g.item_type,
            "start": _point(g.start),
            "end": _point(g.end),
            "center": [0.0, 0.0],
            "radius": 0.0,
            "width": float(fmt(g.width)),
            "layer": g.layer,
            "fill": g.fill,
            "sweep_angle": 0.0,
        }
        if g.mid is not None:
            gj["center"] = _point(g.mid)
        result["graphics"].append(gj)

    return result


def list_steps(path):
    """List available steps in an ODB++ archive."""
    from pathlib import Path
    import tempfile
    import tarfile
    import zipfile

    path = Path(path)

    # Open archive if needed
    root = path
    temp_dir = None
    try:
        if path.is_dir():
            root = path
        elif path.suffixes[-2:] == [".tar", ".gz"] or path.suffix == ".tgz":
            temp_dir = tempfile.mkdtemp(prefix="odb_steps_")
            with tarfile.open(path, "r:gz") as tf:
                tf.extractall(temp_dir, filter="data")
            root = Path(temp_dir)
        elif path.suffix == ".zip":
            temp_dir = tempfile.mkdtemp(prefix="odb_steps_")
            with zipfile.ZipFile(path, "r") as zf:
                zf.extractall(temp_dir)
            root = Path(temp_dir)

        # Find the ODB++ root (look for matrix/matrix)
        parser = OdbParser()
        parser.root = root
        try:
            parser._find_root()
        except FileNotFoundError:
            return []

        # Find steps directory
        steps_dir = parser._find_ci(parser.root, "steps")
        if not steps_dir:
            return []

        return sorted(
            entry.name for entry in steps_dir.iterdir() if entry.is_dir()
        )
    finally:
        if temp_dir:
            import shutil
            shutil.rmtree(temp_dir, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(
        description="Parse ODB++ and output JSON for ipc2581-to-kicad"
    )
    parser.add_argument("input", help="ODB++ archive (.tgz/.zip) or directory")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="Enable verbose logging")
    parser.add_argument("-s", "--step", default=None,
                        help="Step name to convert (default: first step)")
    parser.add_argument("--list-steps", action="store_true",
                        help="List available steps and exit")

    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.WARNING,
        format="%(levelname)s: %(message)s",
        stream=sys.stderr,
    )

    input_path = Path(args.input)
    if not input_path.exists():
        print(f"Error: {input_path} not found", file=sys.stderr)
        sys.exit(1)

    # Handle --list-steps
    if args.list_steps:
        steps = list_steps(input_path)
        if steps:
            print(f"Steps in {input_path}:")
            for s in steps:
                print(f"  {s}")
        else:
            print(f"No steps found in {input_path}")
        sys.exit(0)

    # Parse ODB++
    try:
        model = parse_odb(input_path)
    except Exception as e:
        print(f"Error parsing ODB++: {e}", file=sys.stderr)
        if args.verbose:
            import traceback
            traceback.print_exc(file=sys.stderr)
        sys.exit(1)

    # Log stats to stderr
    log.info("Layers: %d", len(model.layers))
    log.info("Nets: %d", len(model.nets))
    log.info("Components: %d", len(model.components))
    log.info("Traces: %d", len(model.traces))
    log.info("Arcs: %d", len(model.arcs))
    log.info("Vias: %d", len(model.vias))
    log.info("Zones: %d", len(model.zones))

    # Convert to JSON and write to stdout
    json_data = model_to_json(model)
    json.dump(json_data, sys.stdout, separators=(",", ":"))
    print()  # trailing newline


if __name__ == "__main__":
    main()
