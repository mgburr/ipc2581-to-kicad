"""
PCB data layer for the IPC-2581 / ODB++ viewer tools.
Runs the converter with --export-json and parses the result.
For ODB++ inputs, can also use the Python odb package directly.
"""

import json
import os
import subprocess
import math
import sys

# KiCad layer name -> hex color for rendering
LAYER_COLORS = {
    "F.Cu":     "#CC0000",
    "B.Cu":     "#0000CC",
    "In1.Cu":   "#C2C200",
    "In2.Cu":   "#00C2C2",
    "In3.Cu":   "#C200C2",
    "In4.Cu":   "#008000",
    "F.SilkS":  "#C2C200",
    "B.SilkS":  "#800080",
    "F.Mask":   "#800080",
    "B.Mask":   "#008080",
    "F.Paste":  "#808000",
    "B.Paste":  "#006060",
    "F.Fab":    "#CCCC00",
    "B.Fab":    "#0000AA",
    "F.CrtYd":  "#A0A0A0",
    "B.CrtYd":  "#606060",
    "Edge.Cuts": "#C8C800",
    "Dwgs.User": "#808080",
    "Cmts.User": "#404040",
}


def layer_color(layer_name):
    """Return a hex color for the given KiCad layer name."""
    return LAYER_COLORS.get(layer_name, "#808080")


class PcbData:
    """Holds the full parsed PCB model from JSON export."""

    def __init__(self):
        self.data = {}
        self.bbox = None  # (min_x, min_y, max_x, max_y)

    @staticmethod
    def _is_odb_input(filepath):
        """Check if the input file is an ODB++ format."""
        lower = filepath.lower()
        if lower.endswith((".tgz", ".zip")) or lower.endswith(".tar.gz"):
            return True
        if os.path.isdir(filepath):
            return True
        return False

    def load_from_file(self, converter_path, ipc_file, step=None):
        """Run the converter with --export-json and parse the output.

        For ODB++ inputs, uses the Python odb package directly (faster than
        double subprocess through the C++ binary).
        """
        if self._is_odb_input(ipc_file):
            self._load_odb(ipc_file, step)
            return

        cmd = [converter_path, "--export-json"]
        if step:
            cmd.extend(["-s", step])
        cmd.append(ipc_file)

        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if result.returncode != 0:
            raise RuntimeError(
                f"Converter failed (exit {result.returncode}):\n{result.stderr}"
            )

        self.data = json.loads(result.stdout)
        self.bbox = self.compute_bbox()

    def _load_odb(self, odb_file, step=None):
        """Load ODB++ input directly using the Python odb package (no subprocess)."""
        # Add the project root to sys.path so we can import the odb package
        gui_dir = os.path.dirname(os.path.abspath(__file__))
        project_root = os.path.dirname(gui_dir)
        if project_root not in sys.path:
            sys.path.insert(0, project_root)

        from odb.odb_parser import parse_odb
        from odb.odb_to_json import model_to_json
        from pathlib import Path

        model = parse_odb(Path(odb_file))
        self.data = model_to_json(model)
        self.bbox = self.compute_bbox()

    def load_from_json(self, json_text):
        """Parse JSON text directly (for testing)."""
        self.data = json.loads(json_text)
        self.bbox = self.compute_bbox()

    def compute_bbox(self):
        """Compute bounding box from outline and all geometry."""
        xs, ys = [], []

        # Outline segments
        for seg in self.data.get("outline", {}).get("segments", []):
            xs.extend([seg["start"][0], seg["end"][0]])
            ys.extend([seg["start"][1], seg["end"][1]])

        # Outline arcs
        for arc in self.data.get("outline", {}).get("arcs", []):
            xs.extend([arc["start"][0], arc["mid"][0], arc["end"][0]])
            ys.extend([arc["start"][1], arc["mid"][1], arc["end"][1]])

        # Components
        for comp in self.data.get("components", []):
            xs.append(comp["position"][0])
            ys.append(comp["position"][1])

        # Traces
        for t in self.data.get("traces", []):
            xs.extend([t["start"][0], t["end"][0]])
            ys.extend([t["start"][1], t["end"][1]])

        # Vias
        for v in self.data.get("vias", []):
            xs.append(v["position"][0])
            ys.append(v["position"][1])

        if not xs or not ys:
            return (0, 0, 100, 100)

        margin = 5.0
        return (min(xs) - margin, min(ys) - margin,
                max(xs) + margin, max(ys) + margin)

    @property
    def outline(self):
        return self.data.get("outline", {})

    @property
    def layers(self):
        return self.data.get("layers", [])

    @property
    def nets(self):
        return self.data.get("nets", [])

    @property
    def stackup(self):
        return self.data.get("stackup", {})

    @property
    def footprints(self):
        return self.data.get("footprints", {})

    @property
    def components(self):
        return self.data.get("components", [])

    @property
    def traces(self):
        return self.data.get("traces", [])

    @property
    def trace_arcs(self):
        return self.data.get("trace_arcs", [])

    @property
    def vias(self):
        return self.data.get("vias", [])

    @property
    def zones(self):
        return self.data.get("zones", [])

    @property
    def graphics(self):
        return self.data.get("graphics", [])

    @property
    def board_thickness(self):
        return self.stackup.get("board_thickness", 1.6)
