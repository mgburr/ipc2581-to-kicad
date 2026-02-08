"""ODB++ file format parser.

Reads ODB++ archives (.tgz, .zip) or extracted directories and produces
a PcbModel intermediate representation.

ODB++ structure reference:
  matrix/matrix          - Layer definitions and ordering
  misc/info              - Job metadata (units, etc.)
  steps/<step>/profile   - Board outline
  steps/<step>/eda/data  - Netlist, component-pin mapping
  steps/<step>/layers/<layer>/features - Copper features (traces, pads, zones)
  steps/<step>/layers/<layer>/components - Component placements
"""

import logging
import math
import os
import re
import tarfile
import tempfile
import zipfile
from pathlib import Path
from typing import Optional

from .pcb_model import (
    ComponentInstance, Footprint, FootprintPad, GraphicItem,
    LayerDef, LayerType, NetDef, PadDef, PadShape, PcbModel,
    Point, Side, StackupLayer, TraceArc, TraceSegment, Via,
    Zone, ZonePolygon,
)
from .utils import (
    arc_center_to_mid, convert_to_mm, fmt, make_uuid, negate_y,
    parse_float, parse_int, reset_uuid_counter,
)

log = logging.getLogger(__name__)


def parse_odb(path: Path) -> PcbModel:
    """Parse an ODB++ archive or directory and return a PcbModel.

    Args:
        path: Path to .tgz/.tar.gz, .zip, or extracted directory

    Returns:
        Populated PcbModel
    """
    reset_uuid_counter()
    parser = OdbParser()
    return parser.parse(path)


class OdbParser:
    def __init__(self):
        self.model = PcbModel()
        self.root = None  # Path to ODB++ root directory
        self.step_name = None  # Active step name
        self.units = "MM"
        self._temp_dir = None
        # Layer name mapping: odb_name -> LayerDef
        self._layer_map = {}
        # Copper layers in order (top to bottom)
        self._copper_layers = []
        # Symbol definitions: symbol_name -> PadDef
        self._symbol_defs = {}
        # Per-layer symbol tables: maps local index -> symbol_name
        self._layer_sym_tables = {}
        # EDA net data: feature_id -> net_index (per layer)
        self._layer_net_map = {}  # layer_name -> {feature_id -> net_index}
        # EDA package data: pkg_name -> list of pin PadDefs
        self._eda_packages = {}
        # EDA subnet feature mapping: net_index -> list of (layer, feature_id)
        self._subnet_features = {}
        # Drill tool table: tool_num -> diameter (mm)
        self._drill_tools = {}

    def parse(self, path: Path) -> PcbModel:
        path = Path(path)
        try:
            self._open_archive(path)
            self._find_root()
            self._find_step()
            log.info("ODB++ root: %s, step: %s", self.root, self.step_name)

            self._parse_misc_info()
            self._parse_matrix()
            self._parse_profile()
            self._parse_eda_data()
            self._parse_symbols()
            self._parse_components()
            self._parse_layer_features()
            self._parse_drill_layers()

            # Ensure net 0 (unconnected) exists
            if not self.model.nets or self.model.nets[0].name != "":
                self.model.nets.insert(0, NetDef(index=0, name=""))
            # Re-index nets
            for i, nd in enumerate(self.model.nets):
                nd.index = i
                self.model._net_name_to_index[nd.name] = i

            return self.model
        finally:
            self._cleanup()

    # ── Archive handling ──────────────────────────────────────────────

    def _open_archive(self, path: Path):
        if path.is_dir():
            self.root = path
            return

        self._temp_dir = tempfile.mkdtemp(prefix="odb2kicad_")
        dest = Path(self._temp_dir)

        if path.suffixes[-2:] == [".tar", ".gz"] or path.suffix == ".tgz":
            with tarfile.open(path, "r:gz") as tf:
                tf.extractall(dest, filter="data")
        elif path.suffix == ".zip":
            with zipfile.ZipFile(path, "r") as zf:
                zf.extractall(dest)
        else:
            raise ValueError(f"Unsupported archive format: {path}")

        self.root = dest

    def _cleanup(self):
        if self._temp_dir:
            import shutil
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    def _find_root(self):
        """Locate the ODB++ root by finding matrix/matrix file."""
        # Might be directly in root, or one level down
        for candidate in [self.root] + list(self.root.iterdir()):
            if candidate.is_dir():
                matrix = candidate / "matrix" / "matrix"
                if matrix.exists():
                    self.root = candidate
                    return
                # Try case-insensitive
                matrix_dir = self._find_ci(candidate, "matrix")
                if matrix_dir and self._find_ci(matrix_dir, "matrix"):
                    self.root = candidate
                    return
        # Try two levels deep
        for child in self.root.iterdir():
            if child.is_dir():
                for grandchild in child.iterdir():
                    if grandchild.is_dir():
                        matrix = grandchild / "matrix" / "matrix"
                        if matrix.exists():
                            self.root = grandchild
                            return
        raise FileNotFoundError(
            f"Cannot find matrix/matrix in {self.root}. Not a valid ODB++ archive."
        )

    def _find_step(self):
        """Find the first step directory."""
        steps_dir = self._find_ci(self.root, "steps")
        if not steps_dir:
            raise FileNotFoundError("Cannot find steps/ directory")
        for entry in sorted(steps_dir.iterdir()):
            if entry.is_dir():
                self.step_name = entry.name
                return
        raise FileNotFoundError("No step found in steps/ directory")

    def _find_ci(self, parent: Path, name: str) -> Optional[Path]:
        """Case-insensitive directory/file lookup."""
        target = name.lower()
        if not parent.exists():
            return None
        for entry in parent.iterdir():
            if entry.name.lower() == target:
                return entry
        return None

    def _step_path(self) -> Path:
        steps = self._find_ci(self.root, "steps")
        return self._find_ci(steps, self.step_name)

    def _read_file(self, *parts) -> Optional[str]:
        """Read a file by walking parts with case-insensitive lookup."""
        current = self.root
        for part in parts:
            found = self._find_ci(current, part)
            if not found:
                return None
            current = found
        if current.is_file():
            try:
                return current.read_text(encoding="utf-8", errors="replace")
            except Exception:
                return None
        return None

    def _read_step_file(self, *parts) -> Optional[str]:
        """Read a file under the current step directory."""
        return self._read_file("steps", self.step_name, *parts)

    def _find_step_path(self, *parts) -> Optional[Path]:
        """Find a path under the current step directory."""
        current = self._step_path()
        if not current:
            return None
        for part in parts:
            found = self._find_ci(current, part)
            if not found:
                return None
            current = found
        return current

    # ── misc/info ─────────────────────────────────────────────────────

    def _parse_misc_info(self):
        content = self._read_file("misc", "info")
        if not content:
            log.warning("misc/info not found, assuming MM units")
            return

        for line in content.splitlines():
            line = line.strip()
            if line.upper().startswith("UNITS"):
                parts = line.split("=")
                if len(parts) >= 2:
                    self.units = parts[1].strip().upper()
                    self.model.units = self.units
            elif line.upper().startswith("JOB_NAME"):
                parts = line.split("=")
                if len(parts) >= 2:
                    self.model.job_name = parts[1].strip()

        log.info("Units: %s, Job: %s", self.units, self.model.job_name)

    # ── matrix/matrix ─────────────────────────────────────────────────

    def _parse_matrix(self):
        content = self._read_file("matrix", "matrix")
        if not content:
            raise FileNotFoundError("matrix/matrix not found")

        layers = []
        current_layer = None
        in_step = False

        for line in content.splitlines():
            line = line.strip()

            if line.upper().startswith("STEP"):
                in_step = True
                continue
            if line.upper().startswith("LAYER"):
                in_step = False
                current_layer = LayerDef()
                continue
            if line == "}" or line == "{":
                if line == "}" and current_layer and current_layer.odb_name:
                    layers.append(current_layer)
                    current_layer = None
                continue

            if current_layer is None:
                continue

            # Parse key=value pairs
            m = re.match(r"(\w+)\s*=\s*(.*)", line)
            if not m:
                continue
            key, val = m.group(1).upper(), m.group(2).strip()

            if key == "NAME":
                current_layer.odb_name = val
            elif key == "TYPE":
                current_layer.layer_type = self._classify_layer_type(val)
            elif key == "CONTEXT":
                pass  # BOARD or MISC
            elif key == "POLARITY":
                current_layer.polarity = val.lower()
            elif key == "ROW" or key == "OLD_NAME":
                pass
            elif key == "START_NAME":
                # Drill span
                pass
            elif key == "END_NAME":
                pass

        # Now assign KiCad names and layer IDs
        copper_order = 0
        copper_layers = []
        for layer in layers:
            lt = layer.layer_type
            if lt in (LayerType.SIGNAL, LayerType.POWER, LayerType.MIXED):
                layer.copper_order = copper_order
                copper_layers.append(layer)
                copper_order += 1

        # Map copper layers to KiCad names
        n_copper = len(copper_layers)
        for i, cl in enumerate(copper_layers):
            if i == 0:
                cl.kicad_name = "F.Cu"
                cl.layer_id = 0
                cl.side = Side.TOP
            elif i == n_copper - 1:
                cl.kicad_name = "B.Cu"
                cl.layer_id = 2  # KiCad V9: B.Cu = 2
                cl.side = Side.BOTTOM
            else:
                cl.kicad_name = f"In{i}.Cu"
                cl.layer_id = 2 + 2 * i  # Inner layers: 4, 6, 8, ...
                cl.side = Side.BOTH

        # Map non-copper layers
        for layer in layers:
            lt = layer.layer_type
            name_lower = layer.odb_name.lower()
            if lt in (LayerType.SIGNAL, LayerType.POWER, LayerType.MIXED):
                continue  # Already mapped
            elif lt == LayerType.SOLDER_MASK:
                if "top" in name_lower or "front" in name_lower or "comp" in name_lower:
                    layer.kicad_name = "F.Mask"
                    layer.side = Side.TOP
                else:
                    layer.kicad_name = "B.Mask"
                    layer.side = Side.BOTTOM
            elif lt == LayerType.SILK_SCREEN:
                if "top" in name_lower or "front" in name_lower or "comp" in name_lower:
                    layer.kicad_name = "F.SilkS"
                    layer.side = Side.TOP
                else:
                    layer.kicad_name = "B.SilkS"
                    layer.side = Side.BOTTOM
            elif lt == LayerType.SOLDER_PASTE:
                if "top" in name_lower or "front" in name_lower or "comp" in name_lower:
                    layer.kicad_name = "F.Paste"
                    layer.side = Side.TOP
                else:
                    layer.kicad_name = "B.Paste"
                    layer.side = Side.BOTTOM
            elif lt == LayerType.DRILL:
                layer.kicad_name = "drill"  # handled specially
            elif lt == LayerType.COMPONENT:
                if "top" in name_lower or "comp" in name_lower:
                    layer.kicad_name = "F.Fab"
                    layer.side = Side.TOP
                else:
                    layer.kicad_name = "B.Fab"
                    layer.side = Side.BOTTOM
            else:
                layer.kicad_name = f"User.{layer.odb_name}"

            self._layer_map[layer.odb_name] = layer

        for cl in copper_layers:
            self._layer_map[cl.odb_name] = cl

        self._copper_layers = copper_layers
        self.model.layers = layers

        log.info("Found %d layers (%d copper)", len(layers), len(copper_layers))

    def _classify_layer_type(self, type_str: str) -> LayerType:
        t = type_str.upper().strip()
        mapping = {
            "SIGNAL": LayerType.SIGNAL,
            "POWER_GROUND": LayerType.POWER,
            "POWER": LayerType.POWER,
            "MIXED": LayerType.MIXED,
            "SOLDER_MASK": LayerType.SOLDER_MASK,
            "SILK_SCREEN": LayerType.SILK_SCREEN,
            "SOLDER_PASTE": LayerType.SOLDER_PASTE,
            "DRILL": LayerType.DRILL,
            "DOCUMENT": LayerType.DOCUMENT,
            "COMPONENT": LayerType.COMPONENT,
            "ROUT": LayerType.DRILL,
        }
        return mapping.get(t, LayerType.OTHER)

    # ── steps/<step>/profile ──────────────────────────────────────────

    def _parse_profile(self):
        content = self._read_step_file("profile")
        if not content:
            log.warning("Profile not found, no board outline")
            return

        lines_list = content.splitlines()
        i = 0
        outline_items = []
        in_surface = False
        contour_points = []

        while i < len(lines_list):
            line = lines_list[i].strip()
            i += 1

            # ODB++ profile can use simple coordinate records or surface records
            # Format: OB x y   (outline begin)
            #          OS x y   (outline segment - line)
            #          OC x y xc yc CW/CCW  (outline curve - arc)
            #          OE        (outline end)
            # Or: S P 0 ... (surface)

            m = re.match(r"^OB\s+([\d.eE+-]+)\s+([\d.eE+-]+)", line, re.I)
            if m:
                sx = convert_to_mm(parse_float(m.group(1)), self.units)
                sy = convert_to_mm(parse_float(m.group(2)), self.units)
                contour_points = [(sx, negate_y(sy))]
                continue

            m = re.match(r"^OS\s+([\d.eE+-]+)\s+([\d.eE+-]+)", line, re.I)
            if m:
                ex = convert_to_mm(parse_float(m.group(1)), self.units)
                ey = convert_to_mm(parse_float(m.group(2)), self.units)
                if contour_points:
                    px, py = contour_points[-1]
                    outline_items.append(GraphicItem(
                        item_type="line",
                        layer="Edge.Cuts",
                        start=Point(px, py),
                        end=Point(ex, negate_y(ey)),
                        width=0.05,
                    ))
                contour_points.append((ex, negate_y(ey)))
                continue

            m = re.match(
                r"^OC\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+(\w+)",
                line, re.I
            )
            if m:
                ex = convert_to_mm(parse_float(m.group(1)), self.units)
                ey = convert_to_mm(parse_float(m.group(2)), self.units)
                cx = convert_to_mm(parse_float(m.group(3)), self.units)
                cy = convert_to_mm(parse_float(m.group(4)), self.units)
                cw = m.group(5).upper() == "Y"

                if contour_points:
                    px, py = contour_points[-1]
                    # Convert to KiCad Y-down
                    ey_k = negate_y(ey)
                    cy_k = negate_y(cy)
                    mid_x, mid_y = arc_center_to_mid(px, py, ex, ey_k, cx, cy_k, cw)
                    outline_items.append(GraphicItem(
                        item_type="arc",
                        layer="Edge.Cuts",
                        start=Point(px, py),
                        end=Point(ex, ey_k),
                        mid=Point(mid_x, mid_y),
                        width=0.05,
                    ))
                contour_points.append((ex, negate_y(ey)))
                continue

            if re.match(r"^OE\b", line, re.I):
                # Close the outline if needed
                if len(contour_points) >= 2:
                    first = contour_points[0]
                    last = contour_points[-1]
                    if abs(first[0] - last[0]) > 0.001 or abs(first[1] - last[1]) > 0.001:
                        outline_items.append(GraphicItem(
                            item_type="line",
                            layer="Edge.Cuts",
                            start=Point(last[0], last[1]),
                            end=Point(first[0], first[1]),
                            width=0.05,
                        ))
                contour_points = []
                continue

            # Handle surface-based profile (S P 0 ...)
            if line.upper().startswith("S P"):
                in_surface = True
                continue
            if in_surface and line.upper().startswith("SE"):
                in_surface = False
                continue

        self.model.outline = outline_items
        log.info("Board outline: %d segments", len(outline_items))

    # ── steps/<step>/eda/data ─────────────────────────────────────────

    def _parse_eda_data(self):
        content = self._read_step_file("eda", "data")
        if not content:
            log.warning("eda/data not found, no netlist data")
            return

        lines = content.splitlines()
        i = 0
        current_net = None
        current_net_index = 0
        net_list = []  # (name, index)
        # Per-layer feature-to-net mapping
        layer_feature_net = {}  # layer_name -> {feature_id -> net_index}
        # Package data
        current_pkg = None
        pkg_pins = {}  # pkg_name -> list of pin info dicts
        in_net = False
        in_pkg = False
        in_subnet = False

        net_idx = 1  # 0 is reserved for no-net

        while i < len(lines):
            line = lines[i].strip()
            i += 1

            if not line or line.startswith("#"):
                continue

            # NET <name>
            m = re.match(r"^NET\s+(\S+)", line, re.I)
            if m:
                current_net = m.group(1)
                in_net = True
                in_pkg = False
                net_list.append(NetDef(index=net_idx, name=current_net))
                self.model._net_name_to_index[current_net] = net_idx
                current_net_index = net_idx
                net_idx += 1
                continue

            # SNT (subnet) - contains feature references for net
            m = re.match(r"^SNT\s+(\w+)", line, re.I)
            if m:
                in_subnet = True
                continue

            # FID (feature id reference within subnet)
            # FID L <layer> <feature_id> [P|T]
            m = re.match(r"^FID\s+(\w)\s+(\S+)\s+(\d+)", line, re.I)
            if m and current_net:
                fid_type = m.group(1).upper()
                layer_name = m.group(2)
                feature_id = parse_int(m.group(3))

                if layer_name not in layer_feature_net:
                    layer_feature_net[layer_name] = {}
                layer_feature_net[layer_name][feature_id] = current_net_index
                continue

            # PKG <name>
            m = re.match(r"^PKG\s+(\S+)", line, re.I)
            if m:
                current_pkg = m.group(1)
                in_pkg = True
                in_net = False
                if current_pkg not in pkg_pins:
                    pkg_pins[current_pkg] = []
                continue

            # PIN records within PKG
            # PIN <name> <type> <x> <y> ...
            m = re.match(r"^PIN\s+(\S+)\s+(\w+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)", line, re.I)
            if m and current_pkg and in_pkg:
                pin_name = m.group(1)
                pin_type = m.group(2)
                px = parse_float(m.group(3))
                py = parse_float(m.group(4))
                pkg_pins[current_pkg].append({
                    "name": pin_name,
                    "type": pin_type,
                    "x": px,
                    "y": py,
                })
                continue

            if line == "$" or line.startswith("$"):
                in_subnet = False
                continue

        self.model.nets = [NetDef(index=0, name="")] + net_list
        self._layer_net_map = layer_feature_net
        self._eda_packages = pkg_pins

        log.info("Parsed %d nets from eda/data", len(net_list))

    # ── symbols ───────────────────────────────────────────────────────

    def _parse_symbols(self):
        """Parse symbol definitions from symbols/ or lib/ directories."""
        # Try symbols/ directory first
        sym_dir = self._find_ci(self.root, "symbols")
        if not sym_dir:
            sym_dir = self._find_ci(self.root, "lib")
        if not sym_dir:
            log.warning("No symbols directory found")
            return

        for entry in sorted(sym_dir.iterdir()):
            if entry.is_dir():
                self._parse_symbol_dir(entry)

        log.info("Parsed %d symbol definitions", len(self._symbol_defs))

    def _parse_symbol_dir(self, sym_path: Path):
        """Parse a single symbol directory."""
        name = sym_path.name
        features_file = self._find_ci(sym_path, "features")

        # First, try to determine shape from the name
        pad_def = self._symbol_name_to_pad(name)

        # If we have a features file with custom geometry, parse it
        if features_file and pad_def.shape == PadShape.CUSTOM:
            pad_def = self._parse_symbol_features(features_file) or pad_def

        self._symbol_defs[name] = pad_def

    def _symbol_name_to_pad(self, name: str) -> PadDef:
        """Decode standard ODB++ symbol names into pad definitions.

        Common patterns:
            r<diameter>          - round pad
            s<size>              - square pad
            rect<w>x<h>          - rectangle
            oval<w>x<h>          - oval
            donut_r<od>x<id>     - annular ring
            rc<w>x<h>x<corner>   - rounded rectangle
        All dimensions are typically in mils (thousandths of an inch).
        """
        # Round: r100, r50
        m = re.match(r"^r([\d.]+)$", name, re.I)
        if m:
            d = convert_to_mm(parse_float(m.group(1)), "MIL")
            return PadDef(shape=PadShape.CIRCLE, width=d, height=d)

        # Square: s100
        m = re.match(r"^s([\d.]+)$", name, re.I)
        if m:
            d = convert_to_mm(parse_float(m.group(1)), "MIL")
            return PadDef(shape=PadShape.RECT, width=d, height=d)

        # Rectangle: rect100x50
        m = re.match(r"^rect([\d.]+)x([\d.]+)$", name, re.I)
        if m:
            w = convert_to_mm(parse_float(m.group(1)), "MIL")
            h = convert_to_mm(parse_float(m.group(2)), "MIL")
            return PadDef(shape=PadShape.RECT, width=w, height=h)

        # Oval: oval100x50
        m = re.match(r"^oval([\d.]+)x([\d.]+)$", name, re.I)
        if m:
            w = convert_to_mm(parse_float(m.group(1)), "MIL")
            h = convert_to_mm(parse_float(m.group(2)), "MIL")
            return PadDef(shape=PadShape.OVAL, width=w, height=h)

        # Rounded rectangle: rc100x50x10 or rcr100x50xr10
        m = re.match(r"^rc[r]?([\d.]+)x([\d.]+)x[r]?([\d.]+)$", name, re.I)
        if m:
            w = convert_to_mm(parse_float(m.group(1)), "MIL")
            h = convert_to_mm(parse_float(m.group(2)), "MIL")
            corner = convert_to_mm(parse_float(m.group(3)), "MIL")
            ratio = corner / min(w, h) * 2 if min(w, h) > 0 else 0.25
            return PadDef(shape=PadShape.ROUNDRECT, width=w, height=h,
                          roundrect_ratio=min(ratio, 0.5))

        # Donut: donut_r100x50
        m = re.match(r"^donut_r([\d.]+)x([\d.]+)$", name, re.I)
        if m:
            od = convert_to_mm(parse_float(m.group(1)), "MIL")
            return PadDef(shape=PadShape.CIRCLE, width=od, height=od)

        # Thermal: thermal patterns - treat as circle
        m = re.match(r"^th[r]?([\d.]+)", name, re.I)
        if m:
            d = convert_to_mm(parse_float(m.group(1)), "MIL")
            return PadDef(shape=PadShape.CIRCLE, width=d, height=d)

        # Fallback: try to extract any dimension
        m = re.match(r"^[\w]*?([\d.]+)", name)
        if m:
            d = convert_to_mm(parse_float(m.group(1)), "MIL")
            if d > 0:
                return PadDef(shape=PadShape.CIRCLE, width=d, height=d)

        return PadDef(shape=PadShape.CUSTOM, width=1.0, height=1.0)

    def _parse_symbol_features(self, features_path: Path) -> Optional[PadDef]:
        """Parse a symbol's features file for custom geometry."""
        try:
            content = features_path.read_text(encoding="utf-8", errors="replace")
        except Exception:
            return None

        # Extract contour/surface from symbol features
        outline_pts = []
        in_surface = False
        x_min = y_min = float("inf")
        x_max = y_max = float("-inf")

        for line in content.splitlines():
            line = line.strip()
            if line.startswith("S P") or line.startswith("s p"):
                in_surface = True
                continue
            if in_surface:
                # OB x y - outline begin
                m = re.match(r"^OB\s+([\d.eE+-]+)\s+([\d.eE+-]+)", line, re.I)
                if m:
                    x = convert_to_mm(parse_float(m.group(1)), self.units)
                    y = convert_to_mm(parse_float(m.group(2)), self.units)
                    outline_pts.append((x, y))
                    x_min, x_max = min(x_min, x), max(x_max, x)
                    y_min, y_max = min(y_min, y), max(y_max, y)
                    continue

                m = re.match(r"^OS\s+([\d.eE+-]+)\s+([\d.eE+-]+)", line, re.I)
                if m:
                    x = convert_to_mm(parse_float(m.group(1)), self.units)
                    y = convert_to_mm(parse_float(m.group(2)), self.units)
                    outline_pts.append((x, y))
                    x_min, x_max = min(x_min, x), max(x_max, x)
                    y_min, y_max = min(y_min, y), max(y_max, y)
                    continue

                if line.upper().startswith("SE"):
                    in_surface = False
                    continue

        if outline_pts:
            w = x_max - x_min
            h = y_max - y_min
            # Center the outline
            cx = (x_min + x_max) / 2
            cy = (y_min + y_max) / 2
            centered = [(x - cx, y - cy) for x, y in outline_pts]
            return PadDef(
                shape=PadShape.CUSTOM,
                width=max(w, 0.01),
                height=max(h, 0.01),
                custom_outline=centered,
            )
        return None

    # ── Component layers ──────────────────────────────────────────────

    def _parse_components(self):
        """Parse component placements from comp_+_top and comp_+_bot."""
        for side_name, side in [("top", Side.TOP), ("bottom", Side.BOTTOM)]:
            self._parse_component_layer(side_name, side)

        log.info("Parsed %d component instances", len(self.model.components))

    def _parse_component_layer(self, side_name: str, side: Side):
        """Parse components from a specific side."""
        # Find the component layer directory
        step = self._step_path()
        if not step:
            return

        layers_dir = self._find_ci(step, "layers")
        if not layers_dir:
            return

        # Look for comp_+_top, comp_+_bot, or similar names
        comp_dir = None
        for entry in layers_dir.iterdir():
            name_lower = entry.name.lower()
            if entry.is_dir() and "comp" in name_lower:
                if side_name == "top" and ("top" in name_lower or name_lower.endswith("_t")):
                    comp_dir = entry
                    break
                elif side_name == "bottom" and ("bot" in name_lower or name_lower.endswith("_b")):
                    comp_dir = entry
                    break

        # Also check COMPONENT type layers from matrix
        if not comp_dir:
            for layer in self.model.layers:
                if layer.layer_type == LayerType.COMPONENT:
                    name_lower = layer.odb_name.lower()
                    if side_name == "top" and ("top" in name_lower or "comp" in name_lower):
                        comp_dir = self._find_ci(layers_dir, layer.odb_name)
                        break
                    elif side_name == "bottom" and ("bot" in name_lower or "sold" in name_lower):
                        comp_dir = self._find_ci(layers_dir, layer.odb_name)
                        break

        if not comp_dir:
            return

        comp_file = self._find_ci(comp_dir, "components")
        if not comp_file:
            return

        try:
            content = comp_file.read_text(encoding="utf-8", errors="replace")
        except Exception:
            return

        current_comp = None
        current_pads = []
        current_props = {}

        for line in content.splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue

            # CMP <index> <x> <y> <rotation> <mirror> <comp_name> ; ...
            m = re.match(
                r"^CMP\s+(\d+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+(\w+)\s+(\S+)",
                line, re.I
            )
            if m:
                # Save previous component
                if current_comp:
                    self._finalize_component(current_comp, current_pads, current_props, side)

                idx = parse_int(m.group(1))
                x = convert_to_mm(parse_float(m.group(2)), self.units)
                y = convert_to_mm(parse_float(m.group(3)), self.units)
                rot = parse_float(m.group(4))
                mirror = m.group(5).upper()
                comp_name = m.group(6)

                current_comp = {
                    "index": idx,
                    "x": x,
                    "y": negate_y(y),
                    "rotation": rot,
                    "mirror": mirror,
                    "name": comp_name,
                }
                current_pads = []
                current_props = {}

                # Extract reference from ;ID= or ;REF= or part after semicolons
                rest = line[m.end():]
                ref_m = re.search(r";\s*(?:ID|REF)\s*=\s*(\S+)", rest, re.I)
                if ref_m:
                    current_comp["ref"] = ref_m.group(1)
                continue

            # TOP <pin_num> <x> <y> <rotation> <mirror> <net_name> <pad_usage> ; ...
            m = re.match(
                r"^TOP\s+(\d+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+(\w+)\s+(\d+)\s+(\d+)",
                line, re.I
            )
            if m and current_comp:
                pin_num = parse_int(m.group(1))
                px = convert_to_mm(parse_float(m.group(2)), self.units)
                py = convert_to_mm(parse_float(m.group(3)), self.units)
                prot = parse_float(m.group(4))
                pmirror = m.group(5)
                net_num = parse_int(m.group(6))
                pad_usage = parse_int(m.group(7))

                current_pads.append({
                    "pin": pin_num,
                    "x": px,
                    "y": py,
                    "rotation": prot,
                    "mirror": pmirror,
                    "net_num": net_num,
                    "pad_usage": pad_usage,
                })
                continue

            # BOT records (same format as TOP but bottom layer)
            m = re.match(
                r"^BOT\s+(\d+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+(\w+)\s+(\d+)\s+(\d+)",
                line, re.I
            )
            if m and current_comp:
                pin_num = parse_int(m.group(1))
                px = convert_to_mm(parse_float(m.group(2)), self.units)
                py = convert_to_mm(parse_float(m.group(3)), self.units)
                prot = parse_float(m.group(4))
                pmirror = m.group(5)
                net_num = parse_int(m.group(6))
                pad_usage = parse_int(m.group(7))

                current_pads.append({
                    "pin": pin_num,
                    "x": px,
                    "y": py,
                    "rotation": prot,
                    "mirror": pmirror,
                    "net_num": net_num,
                    "pad_usage": pad_usage,
                })
                continue

            # PRP <key> '<value>'
            m = re.match(r"^PRP\s+(\S+)\s+'([^']*)'", line, re.I)
            if m and current_comp:
                current_props[m.group(1)] = m.group(2)
                continue

        # Save last component
        if current_comp:
            self._finalize_component(current_comp, current_pads, current_props, side)

    def _finalize_component(self, comp_data: dict, pads: list, props: dict, side: Side):
        """Create a ComponentInstance from parsed data."""
        ref = comp_data.get("ref", comp_data["name"])
        pkg_name = props.get("COMP_PACKAGE_NAME", comp_data["name"])

        footprint = Footprint(name=pkg_name)

        # Build pads
        for pad_data in pads:
            pin_str = str(pad_data["pin"])
            pad_x = convert_to_mm(pad_data["x"], self.units) if self.units != "MM" else pad_data["x"]
            pad_y = convert_to_mm(pad_data["y"], self.units) if self.units != "MM" else pad_data["y"]

            # Try to find pad shape from symbols
            pad_def = PadDef(shape=PadShape.CIRCLE, width=0.5, height=0.5)

            # Determine net
            net_idx = pad_data.get("net_num", 0)
            net_name = ""
            for nd in self.model.nets:
                if nd.index == net_idx:
                    net_name = nd.name
                    break

            # Determine pad type and layers
            pad_type = "smd"
            if side == Side.TOP:
                layers = ["F.Cu", "F.Paste", "F.Mask"]
            else:
                layers = ["B.Cu", "B.Paste", "B.Mask"]

            fp_pad = FootprintPad(
                number=pin_str,
                pad_def=pad_def,
                pos=Point(pad_x, negate_y(pad_y)),
                rotation=pad_data.get("rotation", 0),
                net_index=net_idx,
                net_name=net_name,
                pad_type=pad_type,
                layers=layers,
            )
            footprint.pads.append(fp_pad)

        comp = ComponentInstance(
            reference=ref,
            footprint_name=pkg_name,
            footprint=footprint,
            pos=Point(comp_data["x"], comp_data["y"]),
            rotation=comp_data["rotation"],
            side=side,
            properties=props,
        )
        self.model.components.append(comp)

    # ── Layer features (copper) ───────────────────────────────────────

    def _parse_layer_features(self):
        """Parse features from each copper layer."""
        for cl in self._copper_layers:
            self._parse_single_layer(cl)

        log.info("Parsed %d traces, %d arcs, %d zones",
                 len(self.model.traces), len(self.model.arcs), len(self.model.zones))

    def _parse_single_layer(self, layer_def: LayerDef):
        """Parse features from a single layer's features file."""
        step = self._step_path()
        if not step:
            return

        layers_dir = self._find_ci(step, "layers")
        if not layers_dir:
            return

        layer_dir = self._find_ci(layers_dir, layer_def.odb_name)
        if not layer_dir:
            return

        features_path = self._find_ci(layer_dir, "features")
        if not features_path:
            return

        try:
            content = features_path.read_text(encoding="utf-8", errors="replace")
        except Exception:
            return

        kicad_layer = layer_def.kicad_name
        net_map = self._layer_net_map.get(layer_def.odb_name, {})

        # Parse symbol table at top of features file
        sym_table = {}  # local_index -> symbol_name
        feature_id = 0
        in_features = False
        in_surface = False
        surface_points = []
        surface_net_idx = 0

        for line in content.splitlines():
            line = line.strip()

            if not line or line.startswith("#"):
                continue

            # Symbol table: $<index> <symbol_name>
            m = re.match(r"^\$(\d+)\s+(\S+)", line)
            if m:
                sym_table[parse_int(m.group(1))] = m.group(2)
                continue

            # Feature records start after symbol table
            # L <xs> <ys> <xe> <ye> <sym_num> <polarity> <dcode> ;...
            m = re.match(
                r"^L\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+(\d+)\s+(\w+)",
                line, re.I
            )
            if m:
                xs = convert_to_mm(parse_float(m.group(1)), self.units)
                ys = convert_to_mm(parse_float(m.group(2)), self.units)
                xe = convert_to_mm(parse_float(m.group(3)), self.units)
                ye = convert_to_mm(parse_float(m.group(4)), self.units)
                sym_idx = parse_int(m.group(5))

                # Get trace width from symbol
                width = 0.25  # default
                sym_name = sym_table.get(sym_idx, "")
                if sym_name:
                    pd = self._symbol_defs.get(sym_name)
                    if pd:
                        width = pd.width
                    else:
                        pd = self._symbol_name_to_pad(sym_name)
                        width = pd.width

                net_idx = net_map.get(feature_id, 0)

                self.model.traces.append(TraceSegment(
                    start=Point(xs, negate_y(ys)),
                    end=Point(xe, negate_y(ye)),
                    width=max(width, 0.01),
                    layer=kicad_layer,
                    net_index=net_idx,
                ))
                feature_id += 1
                continue

            # P <x> <y> <sym_num> <polarity> <dcode> <rotation> ;...
            m = re.match(
                r"^P\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+(\d+)\s+(\w+)",
                line, re.I
            )
            if m:
                # Pads in copper layers - could be via pads or standalone pads
                feature_id += 1
                continue

            # A <xs> <ys> <xe> <ye> <xc> <yc> <sym_num> <polarity> <dcode> <cw/ccw>
            m = re.match(
                r"^A\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+"
                r"([\d.eE+-]+)\s+([\d.eE+-]+)\s+(\d+)\s+(\w+)\s*;?\s*(\w*)",
                line, re.I
            )
            if m:
                xs = convert_to_mm(parse_float(m.group(1)), self.units)
                ys = convert_to_mm(parse_float(m.group(2)), self.units)
                xe = convert_to_mm(parse_float(m.group(3)), self.units)
                ye = convert_to_mm(parse_float(m.group(4)), self.units)
                xc = convert_to_mm(parse_float(m.group(5)), self.units)
                yc = convert_to_mm(parse_float(m.group(6)), self.units)
                sym_idx = parse_int(m.group(7))
                cw_str = m.group(9) if m.group(9) else ""

                width = 0.25
                sym_name = sym_table.get(sym_idx, "")
                if sym_name:
                    pd = self._symbol_defs.get(sym_name) or self._symbol_name_to_pad(sym_name)
                    width = pd.width

                clockwise = cw_str.upper() in ("Y", "CW")
                net_idx = net_map.get(feature_id, 0)

                # Convert center-based arc to midpoint-based
                sy_k = negate_y(ys)
                ey_k = negate_y(ye)
                cy_k = negate_y(yc)
                mid_x, mid_y = arc_center_to_mid(xs, sy_k, xe, ey_k, xc, cy_k, clockwise)

                self.model.arcs.append(TraceArc(
                    start=Point(xs, sy_k),
                    mid=Point(mid_x, mid_y),
                    end=Point(xe, ey_k),
                    width=max(width, 0.01),
                    layer=kicad_layer,
                    net_index=net_idx,
                ))
                feature_id += 1
                continue

            # Surface records (zones/fills)
            if line.upper().startswith("S P"):
                in_surface = True
                surface_points = []
                surface_net_idx = net_map.get(feature_id, 0)
                feature_id += 1
                continue

            if in_surface:
                if line.upper().startswith("SE"):
                    # End of surface - create zone
                    if surface_points:
                        zone_poly = ZonePolygon(
                            outline=[Point(p[0], p[1]) for p in surface_points]
                        )
                        net_name = ""
                        for nd in self.model.nets:
                            if nd.index == surface_net_idx:
                                net_name = nd.name
                                break
                        self.model.zones.append(Zone(
                            net_index=surface_net_idx,
                            net_name=net_name,
                            layer=kicad_layer,
                            polygons=[zone_poly],
                        ))
                    in_surface = False
                    continue

                # OB x y
                m = re.match(r"^OB\s+([\d.eE+-]+)\s+([\d.eE+-]+)", line, re.I)
                if m:
                    x = convert_to_mm(parse_float(m.group(1)), self.units)
                    y = convert_to_mm(parse_float(m.group(2)), self.units)
                    surface_points.append((x, negate_y(y)))
                    continue

                # OS x y
                m = re.match(r"^OS\s+([\d.eE+-]+)\s+([\d.eE+-]+)", line, re.I)
                if m:
                    x = convert_to_mm(parse_float(m.group(1)), self.units)
                    y = convert_to_mm(parse_float(m.group(2)), self.units)
                    surface_points.append((x, negate_y(y)))
                    continue

                # OC x y xc yc cw
                m = re.match(
                    r"^OC\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+(\w+)",
                    line, re.I
                )
                if m:
                    x = convert_to_mm(parse_float(m.group(1)), self.units)
                    y = convert_to_mm(parse_float(m.group(2)), self.units)
                    # For zones, just use the endpoint (simplify arcs to lines)
                    surface_points.append((x, negate_y(y)))
                    continue

    # ── Drill layers ──────────────────────────────────────────────────

    def _parse_drill_layers(self):
        """Parse drill layers for via and hole data."""
        for layer in self.model.layers:
            if layer.layer_type != LayerType.DRILL:
                continue
            self._parse_drill_layer(layer)

        log.info("Parsed %d vias", len(self.model.vias))

    def _parse_drill_layer(self, layer_def: LayerDef):
        """Parse a single drill layer."""
        step = self._step_path()
        if not step:
            return

        layers_dir = self._find_ci(step, "layers")
        if not layers_dir:
            return

        layer_dir = self._find_ci(layers_dir, layer_def.odb_name)
        if not layer_dir:
            return

        # Parse tools file for drill sizes
        tools_path = self._find_ci(layer_dir, "tools")
        drill_tools = {}
        if tools_path:
            try:
                tools_content = tools_path.read_text(encoding="utf-8", errors="replace")
                for tline in tools_content.splitlines():
                    # T<num> <diameter> <unit> ...
                    tm = re.match(r"^T(\d+)\s+([\d.eE+-]+)", tline, re.I)
                    if tm:
                        tool_num = parse_int(tm.group(1))
                        drill_dia = convert_to_mm(parse_float(tm.group(2)), self.units)
                        drill_tools[tool_num] = drill_dia
            except Exception:
                pass

        # Parse features for drill hits
        features_path = self._find_ci(layer_dir, "features")
        if not features_path:
            return

        try:
            content = features_path.read_text(encoding="utf-8", errors="replace")
        except Exception:
            return

        sym_table = {}
        net_map = self._layer_net_map.get(layer_def.odb_name, {})
        feature_id = 0

        # Determine via layer pair from drill span
        via_layers = ("F.Cu", "B.Cu")  # default through-hole

        for line in content.splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue

            m = re.match(r"^\$(\d+)\s+(\S+)", line)
            if m:
                sym_table[parse_int(m.group(1))] = m.group(2)
                continue

            # P <x> <y> <sym_num> <polarity> <dcode> <rotation>
            m = re.match(
                r"^P\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+(\d+)\s+(\w+)",
                line, re.I
            )
            if m:
                x = convert_to_mm(parse_float(m.group(1)), self.units)
                y = convert_to_mm(parse_float(m.group(2)), self.units)
                sym_idx = parse_int(m.group(3))

                # Get drill diameter from symbol
                drill = 0.3  # default
                sym_name = sym_table.get(sym_idx, "")
                if sym_name:
                    pd = self._symbol_defs.get(sym_name) or self._symbol_name_to_pad(sym_name)
                    drill = pd.width

                # Check drill tools
                if sym_idx in drill_tools:
                    drill = drill_tools[sym_idx]

                net_idx = net_map.get(feature_id, 0)

                # Via diameter is typically drill + annular ring
                via_diameter = drill + 0.2  # rough estimate

                self.model.vias.append(Via(
                    pos=Point(x, negate_y(y)),
                    diameter=via_diameter,
                    drill=drill,
                    net_index=net_idx,
                    layers=via_layers,
                ))
                feature_id += 1
                continue
