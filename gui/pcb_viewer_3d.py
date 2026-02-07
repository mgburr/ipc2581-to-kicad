"""
3D Component View — matplotlib mplot3d showing components as colored boxes on the board.
"""

import math
import tkinter as tk
from tkinter import ttk
from pcb_viewer_2d import _arc_points_from_3pt

try:
    import matplotlib
    matplotlib.use("TkAgg")
    from matplotlib.figure import Figure
    from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection
    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False


# Component height and color by refdes prefix
_COMP_HEIGHT = {
    "U": 1.5, "IC": 1.5,
    "R": 1.0, "C": 1.0, "L": 1.0,
    "J": 2.5, "P": 2.5, "CN": 2.5,
    "D": 0.8, "LED": 0.8,
    "Q": 1.0, "T": 1.0,
    "SW": 2.0,
    "F": 1.0,
    "Y": 1.2, "X": 1.2,
}

_COMP_COLOR = {
    "U": "#333333", "IC": "#333333",
    "R": "#8B4513", "C": "#D2B48C", "L": "#556B2F",
    "J": "#696969", "P": "#696969", "CN": "#696969",
    "D": "#DAA520", "LED": "#DAA520",
    "Q": "#2F4F4F", "T": "#2F4F4F",
    "SW": "#808080",
    "F": "#CD853F",
    "Y": "#708090", "X": "#708090",
}


def _refdes_prefix(refdes):
    """Extract letter prefix from refdes (e.g. 'R12' -> 'R', 'LED1' -> 'LED')."""
    prefix = ""
    for ch in refdes:
        if ch.isalpha():
            prefix += ch
        else:
            break
    return prefix.upper()


def _comp_height(refdes):
    prefix = _refdes_prefix(refdes)
    if prefix in _COMP_HEIGHT:
        return _COMP_HEIGHT[prefix]
    # Try shorter prefixes
    for length in range(len(prefix), 0, -1):
        p = prefix[:length]
        if p in _COMP_HEIGHT:
            return _COMP_HEIGHT[p]
    return 1.0


def _comp_color(refdes):
    prefix = _refdes_prefix(refdes)
    if prefix in _COMP_COLOR:
        return _COMP_COLOR[prefix]
    for length in range(len(prefix), 0, -1):
        p = prefix[:length]
        if p in _COMP_COLOR:
            return _COMP_COLOR[p]
    return "#666666"


def _box_faces(x0, y0, z0, x1, y1, z1):
    """Return 6 faces (each a list of 4 vertices) for an axis-aligned box."""
    return [
        [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)],  # bottom
        [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)],  # top
        [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)],  # front
        [(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)],  # back
        [(x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)],  # left
        [(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)],  # right
    ]


def _build_outline_polygon(pcb_data):
    """Build an ordered polygon from outline segments and arcs.

    Chains segments and arcs end-to-end by matching endpoints,
    interpolating arcs into line segments.
    Returns list of (x, y) tuples, or empty list on failure.
    """
    outline = pcb_data.outline
    segments = outline.get("segments", [])
    arcs = outline.get("arcs", [])

    if not segments and not arcs:
        return []

    # Build edge list: each edge is a list of (x,y) points (first, last are endpoints)
    edges = []
    for seg in segments:
        edges.append([(seg["start"][0], seg["start"][1]),
                       (seg["end"][0], seg["end"][1])])
    for arc in arcs:
        pts = _arc_points_from_3pt(
            *arc["start"], *arc["mid"], *arc["end"], n=24)
        edges.append(pts)

    if not edges:
        return []

    # Chain edges into an ordered polygon by matching endpoints
    EPS = 0.05  # tolerance in mm

    def pt_close(a, b):
        return abs(a[0] - b[0]) < EPS and abs(a[1] - b[1]) < EPS

    ordered = list(edges.pop(0))
    max_iters = len(edges) * len(edges) + 1
    iters = 0
    while edges and iters < max_iters:
        iters += 1
        found = False
        for i, edge in enumerate(edges):
            e_start = edge[0]
            e_end = edge[-1]
            tail = ordered[-1]

            if pt_close(tail, e_start):
                ordered.extend(edge[1:])
                edges.pop(i)
                found = True
                break
            elif pt_close(tail, e_end):
                ordered.extend(reversed(edge[:-1]))
                edges.pop(i)
                found = True
                break
        if not found:
            break

    return ordered


def _extrude_polygon(polygon, z_top, z_bot):
    """Extrude a 2D polygon into a 3D prism, returning a list of faces."""
    n = len(polygon)
    if n < 3:
        return []

    faces = []
    # Top face
    faces.append([(x, y, z_top) for x, y in polygon])
    # Bottom face (reversed winding)
    faces.append([(x, y, z_bot) for x, y in reversed(polygon)])
    # Side walls
    for i in range(n):
        j = (i + 1) % n
        x0, y0 = polygon[i]
        x1, y1 = polygon[j]
        faces.append([
            (x0, y0, z_top), (x1, y1, z_top),
            (x1, y1, z_bot), (x0, y0, z_bot),
        ])
    return faces


def _rotate_box_faces(faces, cx, cy, angle_deg):
    """Rotate all face vertices around (cx, cy) by angle_deg."""
    if abs(angle_deg) < 0.01:
        return faces
    rad = math.radians(-angle_deg)
    cos_a = math.cos(rad)
    sin_a = math.sin(rad)
    result = []
    for face in faces:
        new_face = []
        for (x, y, z) in face:
            dx = x - cx
            dy = y - cy
            rx = dx * cos_a - dy * sin_a + cx
            ry = dx * sin_a + dy * cos_a + cy
            new_face.append((rx, ry, z))
        result.append(new_face)
    return result


class PcbViewer3D(tk.Toplevel):
    """3D component viewer window using matplotlib."""

    def __init__(self, parent, pcb_data):
        super().__init__(parent)
        self.title("3D PCB View")
        self.geometry("900x700")
        self.minsize(600, 400)

        self.pcb = pcb_data

        if not HAS_MATPLOTLIB:
            ttk.Label(self, text="matplotlib is required for 3D view.\n"
                      "Install with: pip install matplotlib",
                      font=("", 14)).pack(expand=True)
            return

        self._build_ui()
        self._render()

    def _build_ui(self):
        self.fig = Figure(figsize=(9, 7), dpi=100)
        self.ax = self.fig.add_subplot(111, projection="3d")

        canvas = FigureCanvasTkAgg(self.fig, master=self)
        canvas.draw()
        canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

        toolbar = NavigationToolbar2Tk(canvas, self)
        toolbar.update()

    def _render(self):
        ax = self.ax
        ax.cla()

        bbox = self.pcb.bbox
        if not bbox:
            return
        bx0, by0, bx1, by1 = bbox
        # Add back the margin that compute_bbox adds
        bx0 += 3
        by0 += 3
        bx1 -= 3
        by1 -= 3

        thickness = self.pcb.board_thickness

        # --- Draw board from actual outline ---
        outline_poly = _build_outline_polygon(self.pcb)
        if outline_poly and len(outline_poly) >= 3:
            board_faces = _extrude_polygon(outline_poly, 0, -thickness)
        else:
            board_faces = _box_faces(bx0, by0, 0, bx1, by1, -thickness)
        board = Poly3DCollection(board_faces, alpha=0.3, facecolor="#228B22",
                                 edgecolor="#006400", linewidth=0.5)
        ax.add_collection3d(board)

        # --- Draw copper zones (pours) ---
        for z in self.pcb.zones:
            layer = z.get("layer", "")
            z_height = 0.01 if "F." in layer else -thickness - 0.01
            pts = z.get("outline", [])
            if len(pts) >= 3:
                face = [(x, y, z_height) for x, y in pts]
                color = "#CC0000" if "F." in layer else "#0000CC"
                poly = Poly3DCollection([face], alpha=0.25, facecolor=color,
                                        edgecolor=color, linewidth=0)
                ax.add_collection3d(poly)

        # --- Draw traces ---
        for t in self.pcb.traces:
            layer = t.get("layer", "")
            z_height = 0.02 if "F." in layer else -thickness - 0.02
            sx, sy = t["start"]
            ex, ey = t["end"]
            color = "#FF0000" if "F." in layer else "#0044FF"
            ax.plot([sx, ex], [sy, ey], [z_height, z_height],
                    color=color, linewidth=0.8, alpha=0.8)

        # --- Draw trace arcs ---
        for a in self.pcb.trace_arcs:
            layer = a.get("layer", "")
            z_height = 0.02 if "F." in layer else -thickness - 0.02
            color = "#FF0000" if "F." in layer else "#0044FF"
            pts = _arc_points_from_3pt(
                *a["start"], *a["mid"], *a["end"], n=16)
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            zs = [z_height] * len(pts)
            ax.plot(xs, ys, zs, color=color, linewidth=0.8, alpha=0.8)

        # --- Draw vias ---
        for v in self.pcb.vias:
            vx, vy = v["position"]
            r = v["diameter"] / 2
            angles = [i * 2 * math.pi / 12 for i in range(13)]
            xs = [vx + r * math.cos(a) for a in angles]
            ys = [vy + r * math.sin(a) for a in angles]
            # Draw on both surfaces
            ax.plot(xs, ys, [0.02] * len(angles),
                    color="#C0C0C0", linewidth=0.5, alpha=0.7)
            ax.plot(xs, ys, [-thickness - 0.02] * len(angles),
                    color="#C0C0C0", linewidth=0.5, alpha=0.7)

        # --- Draw components ---
        for comp in self.pcb.components:
            refdes = comp["refdes"]
            prefix = _refdes_prefix(refdes)
            if prefix == "H":  # skip mounting holes
                continue

            fp_name = comp["footprint_ref"]
            fp = self.pcb.footprints.get(fp_name)
            if not fp:
                continue

            cx, cy = comp["position"]
            rot = comp.get("rotation", 0)
            mirror = comp.get("mirror", False)

            # Compute component size from pad bounding box
            pads = fp.get("pads", [])
            if not pads:
                continue

            pad_xs = [p["offset"][0] for p in pads]
            pad_ys = [p["offset"][1] for p in pads]
            # Add pad sizes
            pad_ws = [p.get("width", 0.5) for p in pads]
            pad_hs = [p.get("height", 0.5) for p in pads]

            min_px = min(px - pw / 2 for px, pw in zip(pad_xs, pad_ws))
            max_px = max(px + pw / 2 for px, pw in zip(pad_xs, pad_ws))
            min_py = min(py - ph / 2 for py, ph in zip(pad_ys, pad_hs))
            max_py = max(py + ph / 2 for py, ph in zip(pad_ys, pad_hs))

            # Add margin
            margin = 0.3
            min_px -= margin
            max_px += margin
            min_py -= margin
            max_py += margin

            height = _comp_height(refdes)
            color = _comp_color(refdes)

            if mirror:
                # Bottom side: box hangs below board
                z_base = -thickness
                z_top = -thickness - height
                faces = _box_faces(cx + min_px, cy + min_py, z_base,
                                   cx + max_px, cy + max_py, z_top)
            else:
                # Top side: box sits on board
                z_base = 0
                z_top = height
                faces = _box_faces(cx + min_px, cy + min_py, z_base,
                                   cx + max_px, cy + max_py, z_top)

            faces = _rotate_box_faces(faces, cx, cy, rot)

            comp_poly = Poly3DCollection(faces, alpha=0.7, facecolor=color,
                                         edgecolor="#000000", linewidth=0.3)
            ax.add_collection3d(comp_poly)

            # Label on top
            label_z = z_top + 0.1 if not mirror else z_top - 0.1
            ax.text(cx, cy, label_z, refdes, fontsize=5,
                    ha="center", va="center", color="white",
                    fontweight="bold")

        # --- Set view ---
        ax.set_xlabel("X (mm)")
        ax.set_ylabel("Y (mm)")
        ax.set_zlabel("Z (mm)")

        # Equal aspect ratio
        x_range = bx1 - bx0
        y_range = by1 - by0
        z_range = max(thickness * 4, 5)  # ensure some Z range
        max_range = max(x_range, y_range, z_range)

        mid_x = (bx0 + bx1) / 2
        mid_y = (by0 + by1) / 2
        mid_z = -thickness / 2

        ax.set_xlim(mid_x - max_range / 2, mid_x + max_range / 2)
        ax.set_ylim(mid_y - max_range / 2, mid_y + max_range / 2)
        ax.set_zlim(mid_z - max_range / 2, mid_z + max_range / 2)

        ax.view_init(elev=35, azim=-60)
        self.fig.tight_layout()
        self.fig.canvas.draw()
