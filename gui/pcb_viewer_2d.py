"""
2D PCB Layer Viewer — tkinter Canvas with zoom/pan and per-layer toggles.
"""

import math
import tkinter as tk
from tkinter import ttk
from pcb_data import layer_color, LAYER_COLORS


def _arc_points_from_3pt(sx, sy, mx, my, ex, ey, n=32):
    """Interpolate an arc given start/mid/end into n line segments.
    Returns list of (x, y) tuples."""
    # Compute circle through three points
    ax, ay = sx, sy
    bx, by = mx, my
    cx, cy = ex, ey

    D = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
    if abs(D) < 1e-12:
        return [(sx, sy), (mx, my), (ex, ey)]

    ux = ((ax * ax + ay * ay) * (by - cy) +
          (bx * bx + by * by) * (cy - ay) +
          (cx * cx + cy * cy) * (ay - by)) / D
    uy = ((ax * ax + ay * ay) * (cx - bx) +
          (bx * bx + by * by) * (ax - cx) +
          (cx * cx + cy * cy) * (bx - ax)) / D

    r = math.hypot(ax - ux, ay - uy)
    a_start = math.atan2(ay - uy, ax - ux)
    a_mid = math.atan2(by - uy, bx - ux)
    a_end = math.atan2(cy - uy, cx - ux)

    # Determine sweep direction (CW or CCW)
    def norm_angle(a):
        while a < 0:
            a += 2 * math.pi
        while a >= 2 * math.pi:
            a -= 2 * math.pi
        return a

    a_start_n = norm_angle(a_start)
    a_mid_n = norm_angle(a_mid)
    a_end_n = norm_angle(a_end)

    # Check if mid is between start and end going CCW
    def between_ccw(s, m, e):
        if s <= e:
            return s <= m <= e
        return m >= s or m <= e

    if between_ccw(a_start_n, a_mid_n, a_end_n):
        # CCW
        sweep = a_end_n - a_start_n
        if sweep <= 0:
            sweep += 2 * math.pi
    else:
        # CW
        sweep = a_end_n - a_start_n
        if sweep >= 0:
            sweep -= 2 * math.pi

    pts = []
    for i in range(n + 1):
        t = i / n
        a = a_start + sweep * t
        pts.append((ux + r * math.cos(a), uy + r * math.sin(a)))
    return pts


def _all_layer_names(pcb):
    """Collect all unique layer names used in the PCB data."""
    names = set()
    # From layer defs
    for l in pcb.layers:
        if l.get("kicad_name"):
            names.add(l["kicad_name"])
    # From traces
    for t in pcb.traces:
        names.add(t["layer"])
    # From graphics
    for g in pcb.graphics:
        if g.get("layer"):
            names.add(g["layer"])
    # From zones
    for z in pcb.zones:
        names.add(z["layer"])
    # From footprint graphics
    for fp in pcb.footprints.values():
        for g in fp.get("graphics", []):
            if g.get("layer"):
                names.add(g["layer"])
    # Always include Edge.Cuts
    names.add("Edge.Cuts")
    return sorted(names)


class PcbViewer2D(tk.Toplevel):
    """2D PCB viewer window with layer toggles, zoom, and pan."""

    def __init__(self, parent, pcb_data):
        super().__init__(parent)
        self.title("2D PCB Viewer")
        self.geometry("1200x800")
        self.minsize(800, 500)

        self.pcb = pcb_data
        self.scale = 1.0
        self.offset_x = 0.0
        self.offset_y = 0.0
        self.layer_vars = {}  # layer_name -> BooleanVar

        self._build_ui()
        self._draw_all()
        self.after(100, self._fit_view)

    # ------------------------------------------------------------------ UI

    def _build_ui(self):
        # Main panes
        pw = ttk.PanedWindow(self, orient=tk.HORIZONTAL)
        pw.pack(fill=tk.BOTH, expand=True)

        # --- Left: layer panel ---
        left = ttk.Frame(pw, width=200)
        pw.add(left, weight=0)

        ttk.Label(left, text="Layers", font=("", 11, "bold")).pack(pady=(6, 2))

        btn_frame = ttk.Frame(left)
        btn_frame.pack(fill=tk.X, padx=4)
        ttk.Button(btn_frame, text="All On", command=self._layers_all_on).pack(
            side=tk.LEFT, expand=True, fill=tk.X, padx=1)
        ttk.Button(btn_frame, text="All Off", command=self._layers_all_off).pack(
            side=tk.LEFT, expand=True, fill=tk.X, padx=1)

        # Scrollable layer list
        canvas_frame = ttk.Frame(left)
        canvas_frame.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        layer_canvas = tk.Canvas(canvas_frame, highlightthickness=0)
        scrollbar = ttk.Scrollbar(canvas_frame, orient=tk.VERTICAL,
                                  command=layer_canvas.yview)
        self.layer_list_frame = ttk.Frame(layer_canvas)

        self.layer_list_frame.bind(
            "<Configure>",
            lambda e: layer_canvas.configure(scrollregion=layer_canvas.bbox("all"))
        )
        layer_canvas.create_window((0, 0), window=self.layer_list_frame, anchor="nw")
        layer_canvas.configure(yscrollcommand=scrollbar.set)

        layer_canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        layer_names = _all_layer_names(self.pcb)
        for name in layer_names:
            var = tk.BooleanVar(value=True)
            self.layer_vars[name] = var
            color = layer_color(name)
            cb = tk.Checkbutton(
                self.layer_list_frame, text=name, variable=var,
                fg=color, selectcolor="#222222", activeforeground=color,
                anchor="w", command=self._on_layer_toggle
            )
            cb.pack(fill=tk.X, padx=2)

        # --- Right: canvas + toolbar ---
        right = ttk.Frame(pw)
        pw.add(right, weight=1)

        # Toolbar
        tb = ttk.Frame(right)
        tb.pack(fill=tk.X, padx=4, pady=2)

        ttk.Button(tb, text="Fit", command=self._fit_view).pack(side=tk.LEFT, padx=2)
        ttk.Button(tb, text="Zoom +", command=lambda: self._zoom(1.3)).pack(
            side=tk.LEFT, padx=2)
        ttk.Button(tb, text="Zoom -", command=lambda: self._zoom(1 / 1.3)).pack(
            side=tk.LEFT, padx=2)

        self.coord_var = tk.StringVar(value="")
        ttk.Label(tb, textvariable=self.coord_var).pack(side=tk.RIGHT, padx=6)

        # Canvas
        self.canvas = tk.Canvas(right, bg="#1a1a1a", highlightthickness=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)

        # Bindings
        self.canvas.bind("<MouseWheel>", self._on_mousewheel)       # macOS / Windows
        self.canvas.bind("<Button-4>", self._on_mousewheel_linux)   # Linux scroll up
        self.canvas.bind("<Button-5>", self._on_mousewheel_linux)   # Linux scroll down
        self.canvas.bind("<ButtonPress-2>", self._on_pan_start)
        self.canvas.bind("<B2-Motion>", self._on_pan_move)
        self.canvas.bind("<ButtonPress-3>", self._on_pan_start)
        self.canvas.bind("<B3-Motion>", self._on_pan_move)
        self.canvas.bind("<Motion>", self._on_mouse_move)
        self.canvas.bind("<Configure>", lambda e: self.after(50, self._redraw))

    # -------------------------------------------------------------- Layers

    def _layers_all_on(self):
        for v in self.layer_vars.values():
            v.set(True)
        self._on_layer_toggle()

    def _layers_all_off(self):
        for v in self.layer_vars.values():
            v.set(False)
        self._on_layer_toggle()

    def _on_layer_toggle(self):
        for name, var in self.layer_vars.items():
            tag = f"layer_{name}"
            state = tk.NORMAL if var.get() else tk.HIDDEN
            self.canvas.itemconfigure(tag, state=state)

    # -------------------------------------------------------- Coord system

    def _pcb_to_canvas(self, px, py):
        """Convert PCB coords (mm) to canvas pixels."""
        cx = (px - self.offset_x) * self.scale
        cy = (py - self.offset_y) * self.scale
        return cx, cy

    def _canvas_to_pcb(self, cx, cy):
        """Convert canvas pixels to PCB coords (mm)."""
        px = cx / self.scale + self.offset_x
        py = cy / self.scale + self.offset_y
        return px, py

    # ----------------------------------------------------------- Zoom / Pan

    def _fit_view(self):
        if not self.pcb.bbox:
            return
        cw = self.canvas.winfo_width()
        ch = self.canvas.winfo_height()
        if cw < 10 or ch < 10:
            return

        bx0, by0, bx1, by1 = self.pcb.bbox
        bw = bx1 - bx0
        bh = by1 - by0
        if bw < 0.01 or bh < 0.01:
            return

        sx = cw / bw
        sy = ch / bh
        self.scale = min(sx, sy) * 0.92

        self.offset_x = bx0 - (cw / self.scale - bw) / 2
        self.offset_y = by0 - (ch / self.scale - bh) / 2

        self._redraw()

    def _zoom(self, factor, cx=None, cy=None):
        if cx is None:
            cx = self.canvas.winfo_width() / 2
            cy = self.canvas.winfo_height() / 2

        # Zoom centered on cursor
        px, py = self._canvas_to_pcb(cx, cy)
        self.scale *= factor
        self.offset_x = px - cx / self.scale
        self.offset_y = py - cy / self.scale
        self._redraw()

    def _on_mousewheel(self, event):
        factor = 1.15 if event.delta > 0 else 1 / 1.15
        self._zoom(factor, event.x, event.y)

    def _on_mousewheel_linux(self, event):
        factor = 1.15 if event.num == 4 else 1 / 1.15
        self._zoom(factor, event.x, event.y)

    def _on_pan_start(self, event):
        self.canvas.scan_mark(event.x, event.y)
        self._pan_sx = event.x
        self._pan_sy = event.y

    def _on_pan_move(self, event):
        dx = event.x - self._pan_sx
        dy = event.y - self._pan_sy
        self._pan_sx = event.x
        self._pan_sy = event.y
        self.offset_x -= dx / self.scale
        self.offset_y -= dy / self.scale
        self._redraw()

    def _on_mouse_move(self, event):
        px, py = self._canvas_to_pcb(event.x, event.y)
        self.coord_var.set(f"X: {px:.3f} mm   Y: {py:.3f} mm")

    # ------------------------------------------------------------- Drawing

    def _redraw(self):
        self.canvas.delete("all")
        self._draw_all()
        # Reapply layer visibility
        self._on_layer_toggle()

    def _draw_all(self):
        """Draw every PCB element, tagged by layer."""
        self._draw_zones()
        self._draw_outline()
        self._draw_traces()
        self._draw_trace_arcs()
        self._draw_graphics()
        self._draw_footprint_graphics()
        self._draw_pads()
        self._draw_vias()

    def _draw_outline(self):
        tag = "layer_Edge.Cuts"
        color = layer_color("Edge.Cuts")

        for seg in self.pcb.outline.get("segments", []):
            x0, y0 = self._pcb_to_canvas(*seg["start"])
            x1, y1 = self._pcb_to_canvas(*seg["end"])
            w = max(1, seg["width"] * self.scale)
            self.canvas.create_line(x0, y0, x1, y1, fill=color, width=w,
                                    tags=(tag,))

        for arc in self.pcb.outline.get("arcs", []):
            pts = _arc_points_from_3pt(
                *arc["start"], *arc["mid"], *arc["end"])
            coords = []
            for px, py in pts:
                cx, cy = self._pcb_to_canvas(px, py)
                coords.extend([cx, cy])
            if len(coords) >= 4:
                w = max(1, arc["width"] * self.scale)
                self.canvas.create_line(coords, fill=color, width=w,
                                        smooth=False, tags=(tag,))

    def _draw_traces(self):
        for t in self.pcb.traces:
            layer = t["layer"]
            tag = f"layer_{layer}"
            color = layer_color(layer)
            x0, y0 = self._pcb_to_canvas(*t["start"])
            x1, y1 = self._pcb_to_canvas(*t["end"])
            w = max(1, t["width"] * self.scale)
            self.canvas.create_line(x0, y0, x1, y1, fill=color, width=w,
                                    capstyle=tk.ROUND, tags=(tag,))

    def _draw_trace_arcs(self):
        for a in self.pcb.trace_arcs:
            layer = a["layer"]
            tag = f"layer_{layer}"
            color = layer_color(layer)
            pts = _arc_points_from_3pt(
                *a["start"], *a["mid"], *a["end"])
            coords = []
            for px, py in pts:
                cx, cy = self._pcb_to_canvas(px, py)
                coords.extend([cx, cy])
            if len(coords) >= 4:
                w = max(1, a["width"] * self.scale)
                self.canvas.create_line(coords, fill=color, width=w,
                                        capstyle=tk.ROUND, smooth=False,
                                        tags=(tag,))

    def _draw_vias(self):
        via_color = "#C0C0C0"
        drill_color = "#1a1a1a"
        for v in self.pcb.vias:
            px, py = v["position"]
            cx, cy = self._pcb_to_canvas(px, py)
            r_outer = v["diameter"] / 2 * self.scale
            r_drill = v["drill"] / 2 * self.scale
            # Use F.Cu tag so vias show with copper
            tag = "layer_F.Cu"
            if r_outer >= 1:
                self.canvas.create_oval(cx - r_outer, cy - r_outer,
                                        cx + r_outer, cy + r_outer,
                                        fill=via_color, outline=via_color,
                                        tags=(tag,))
            if r_drill >= 0.5:
                self.canvas.create_oval(cx - r_drill, cy - r_drill,
                                        cx + r_drill, cy + r_drill,
                                        fill=drill_color, outline=drill_color,
                                        tags=(tag,))

    def _draw_zones(self):
        for z in self.pcb.zones:
            layer = z["layer"]
            tag = f"layer_{layer}"
            color = layer_color(layer)
            pts = z.get("outline", [])
            if len(pts) < 3:
                continue
            coords = []
            for pt in pts:
                cx, cy = self._pcb_to_canvas(*pt)
                coords.extend([cx, cy])
            self.canvas.create_polygon(coords, fill=color, outline=color,
                                       stipple="gray25", tags=(tag,))

    def _draw_graphics(self):
        for gi in self.pcb.graphics:
            self._draw_graphic_item(gi)

    def _draw_footprint_graphics(self):
        """Draw graphics from each footprint instance at its placed position."""
        for comp in self.pcb.components:
            fp_name = comp["footprint_ref"]
            fp = self.pcb.footprints.get(fp_name)
            if not fp:
                continue
            cx, cy = comp["position"]
            rot = comp.get("rotation", 0)
            mirror = comp.get("mirror", False)
            for gi in fp.get("graphics", []):
                self._draw_graphic_item(gi, cx, cy, rot, mirror)

    def _draw_graphic_item(self, gi, comp_x=0, comp_y=0, comp_rot=0, mirror=False):
        layer = gi.get("layer", "")
        if mirror:
            # Flip layer sides for mirrored components
            layer = layer.replace("F.SilkS", "B.SilkS").replace("F.Fab", "B.Fab").replace("F.CrtYd", "B.CrtYd")
        tag = f"layer_{layer}"
        color = layer_color(layer)
        kind = gi.get("kind", "")

        def transform(px, py):
            """Apply component transform then convert to canvas."""
            if comp_rot != 0:
                rad = math.radians(-comp_rot)
                rx = px * math.cos(rad) - py * math.sin(rad)
                ry = px * math.sin(rad) + py * math.cos(rad)
                px, py = rx, ry
            return self._pcb_to_canvas(px + comp_x, py + comp_y)

        if kind == "line":
            x0, y0 = transform(*gi["start"])
            x1, y1 = transform(*gi["end"])
            w = max(1, gi.get("width", 0.1) * self.scale)
            self.canvas.create_line(x0, y0, x1, y1, fill=color, width=w,
                                    tags=(tag,))
        elif kind == "arc":
            # Use start/center(mid)/end from JSON
            pts = _arc_points_from_3pt(
                *gi["start"], *gi["center"], *gi["end"])
            coords = []
            for px, py in pts:
                tcx, tcy = transform(px, py)
                coords.extend([tcx, tcy])
            if len(coords) >= 4:
                w = max(1, gi.get("width", 0.1) * self.scale)
                self.canvas.create_line(coords, fill=color, width=w,
                                        smooth=False, tags=(tag,))
        elif kind == "circle":
            ccx, ccy = gi["center"]
            r = gi.get("radius", 0)
            # Transform center
            tcx, tcy = transform(ccx, ccy)
            rr = r * self.scale
            if rr >= 0.5:
                fill = color if gi.get("fill") else ""
                self.canvas.create_oval(tcx - rr, tcy - rr,
                                        tcx + rr, tcy + rr,
                                        outline=color, fill=fill,
                                        width=max(1, gi.get("width", 0.1) * self.scale),
                                        tags=(tag,))
        elif kind == "polygon":
            points = gi.get("points", [])
            if len(points) >= 3:
                coords = []
                for pt in points:
                    tcx, tcy = transform(*pt)
                    coords.extend([tcx, tcy])
                fill = color if gi.get("fill") else ""
                self.canvas.create_polygon(coords, outline=color, fill=fill,
                                           width=max(1, gi.get("width", 0.1) * self.scale),
                                           tags=(tag,))

    def _draw_pads(self):
        """Draw pads for each component instance."""
        for comp in self.pcb.components:
            fp_name = comp["footprint_ref"]
            fp = self.pcb.footprints.get(fp_name)
            if not fp:
                continue
            comp_x, comp_y = comp["position"]
            comp_rot = comp.get("rotation", 0)
            mirror = comp.get("mirror", False)

            for pad in fp.get("pads", []):
                pad_x, pad_y = pad["offset"]
                pad_rot = pad.get("rotation", 0)

                # Rotate pad offset by component rotation
                if comp_rot != 0:
                    rad = math.radians(-comp_rot)
                    rx = pad_x * math.cos(rad) - pad_y * math.sin(rad)
                    ry = pad_x * math.sin(rad) + pad_y * math.cos(rad)
                    pad_x, pad_y = rx, ry

                world_x = comp_x + pad_x
                world_y = comp_y + pad_y

                # Determine layer/color
                pad_type = pad.get("type", "smd")
                if pad_type == "thru_hole" or pad_type == "npth":
                    layer = "F.Cu"
                elif mirror:
                    layer = "B.Cu"
                else:
                    layer = "F.Cu"
                tag = f"layer_{layer}"
                color = layer_color(layer)

                cx, cy = self._pcb_to_canvas(world_x, world_y)
                pw = pad["width"] * self.scale / 2
                ph = pad["height"] * self.scale / 2

                shape = pad.get("shape", "rect")
                if shape == "circle":
                    r = pw
                    self.canvas.create_oval(cx - r, cy - r, cx + r, cy + r,
                                            fill=color, outline=color,
                                            tags=(tag,))
                elif shape == "oval":
                    self.canvas.create_oval(cx - pw, cy - ph, cx + pw, cy + ph,
                                            fill=color, outline=color,
                                            tags=(tag,))
                elif shape == "custom":
                    pts = pad.get("custom_shape", [])
                    if len(pts) >= 3:
                        coords = []
                        total_rot = comp_rot + pad_rot
                        for pt in pts:
                            ppx, ppy = pt
                            if total_rot != 0:
                                rad = math.radians(-total_rot)
                                rpx = ppx * math.cos(rad) - ppy * math.sin(rad)
                                rpy = ppx * math.sin(rad) + ppy * math.cos(rad)
                                ppx, ppy = rpx, rpy
                            tcx, tcy = self._pcb_to_canvas(
                                world_x + ppx, world_y + ppy)
                            coords.extend([tcx, tcy])
                        self.canvas.create_polygon(coords, fill=color,
                                                   outline=color, tags=(tag,))
                else:
                    # rect / roundrect / trapezoid -> draw as rectangle
                    self.canvas.create_rectangle(cx - pw, cy - ph,
                                                 cx + pw, cy + ph,
                                                 fill=color, outline=color,
                                                 tags=(tag,))

                # Draw drill hole
                drill = pad.get("drill_diameter", 0)
                if drill > 0:
                    dr = drill * self.scale / 2
                    self.canvas.create_oval(cx - dr, cy - dr, cx + dr, cy + dr,
                                            fill="#1a1a1a", outline="#1a1a1a",
                                            tags=(tag,))
