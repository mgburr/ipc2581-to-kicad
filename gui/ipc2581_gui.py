#!/usr/bin/env python3
"""
IPC-2581 to KiCad Converter - GUI
A graphical interface for the ipc2581-to-kicad command-line tool.
"""

import json
import os
import sys
import subprocess
import threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext
from pathlib import Path

# Ensure sibling modules are importable regardless of working directory
sys.path.insert(0, str(Path(__file__).parent.resolve()))

from pcb_data import PcbData
from pcb_viewer_2d import PcbViewer2D
from pcb_viewer_3d import PcbViewer3D


class Ipc2581ConverterGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("IPC-2581 / ODB++ / Gerber to KiCad Converter")
        self.root.geometry("700x750")
        self.root.minsize(600, 600)

        # Find the converter executable
        self.converter_path = self._find_converter()

        # Variables
        self.input_file = tk.StringVar()
        self.output_file = tk.StringVar()
        self.kicad_version = tk.StringVar(value="9")
        self.selected_step = tk.StringVar()
        self.verbose = tk.BooleanVar(value=True)
        self.available_steps = []
        self.layer_data = []       # list of dicts from --list-layers --export-json
        self.layer_combos = []     # list of (ipc_name, auto_kicad_name, combobox) tuples

        # Build UI
        self._create_widgets()

        # Check if converter exists
        if not self.converter_path:
            self._log("WARNING: Converter executable not found!", "error")
            self._log("Please build the project first: cd build && cmake .. && make", "error")

    def _find_converter(self):
        """Find the converter executable in common locations."""
        script_dir = Path(__file__).parent.resolve()
        project_dir = script_dir.parent

        possible_paths = [
            # Inside .app bundle: script is in Resources/, binary is also in Resources/
            script_dir / "ipc2581-to-kicad",
            project_dir / "build" / "ipc2581-to-kicad",
            project_dir / "ipc2581-to-kicad",
            Path.cwd() / "ipc2581-to-kicad",
            Path.cwd() / "build" / "ipc2581-to-kicad",
        ]

        for p in possible_paths:
            if p.exists() and os.access(p, os.X_OK):
                return str(p)

        # Try to find in PATH
        import shutil
        found = shutil.which("ipc2581-to-kicad")
        if found:
            return found

        return None

    def _create_widgets(self):
        """Create all GUI widgets."""
        # Main container with padding
        main_frame = ttk.Frame(self.root, padding="10")
        main_frame.grid(row=0, column=0, sticky="nsew")
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        main_frame.columnconfigure(1, weight=1)

        row = 0

        # === Input File Section ===
        ttk.Label(main_frame, text="Input File:", font=("", 10, "bold")).grid(
            row=row, column=0, sticky="w", pady=(0, 5))
        row += 1

        input_frame = ttk.Frame(main_frame)
        input_frame.grid(row=row, column=0, columnspan=3, sticky="ew", pady=(0, 10))
        input_frame.columnconfigure(0, weight=1)

        self.input_entry = ttk.Entry(input_frame, textvariable=self.input_file, width=60)
        self.input_entry.grid(row=0, column=0, sticky="ew", padx=(0, 5))

        ttk.Button(input_frame, text="Browse...", command=self._browse_input).grid(
            row=0, column=1, padx=(0, 5))
        ttk.Button(input_frame, text="Import Folder...",
                   command=self._browse_gerber_folder).grid(row=0, column=2)
        row += 1

        # === Output File Section ===
        ttk.Label(main_frame, text="Output File:", font=("", 10, "bold")).grid(
            row=row, column=0, sticky="w", pady=(0, 5))
        row += 1

        output_frame = ttk.Frame(main_frame)
        output_frame.grid(row=row, column=0, columnspan=3, sticky="ew", pady=(0, 10))
        output_frame.columnconfigure(0, weight=1)

        self.output_entry = ttk.Entry(output_frame, textvariable=self.output_file, width=60)
        self.output_entry.grid(row=0, column=0, sticky="ew", padx=(0, 5))

        ttk.Button(output_frame, text="Browse...", command=self._browse_output).grid(
            row=0, column=1)
        row += 1

        # === Options Section ===
        options_frame = ttk.LabelFrame(main_frame, text="Options", padding="10")
        options_frame.grid(row=row, column=0, columnspan=3, sticky="ew", pady=(0, 10))
        options_frame.columnconfigure(1, weight=1)
        row += 1

        # KiCad Version
        ttk.Label(options_frame, text="KiCad Version:").grid(row=0, column=0, sticky="w", pady=2)
        version_frame = ttk.Frame(options_frame)
        version_frame.grid(row=0, column=1, sticky="w", pady=2)
        ttk.Radiobutton(version_frame, text="KiCad 7", variable=self.kicad_version,
                        value="7").pack(side="left", padx=(0, 10))
        ttk.Radiobutton(version_frame, text="KiCad 8", variable=self.kicad_version,
                        value="8").pack(side="left", padx=(0, 10))
        ttk.Radiobutton(version_frame, text="KiCad 9", variable=self.kicad_version,
                        value="9").pack(side="left")

        # Step Selection
        ttk.Label(options_frame, text="Step:").grid(row=1, column=0, sticky="w", pady=2)
        step_frame = ttk.Frame(options_frame)
        step_frame.grid(row=1, column=1, sticky="ew", pady=2)
        step_frame.columnconfigure(0, weight=1)

        self.step_combo = ttk.Combobox(step_frame, textvariable=self.selected_step,
                                        state="readonly", width=40)
        self.step_combo.grid(row=0, column=0, sticky="w", padx=(0, 5))
        self.step_combo.set("(auto-detect from file)")

        ttk.Button(step_frame, text="Refresh", command=self._load_steps).grid(row=0, column=1)

        # Verbose
        ttk.Checkbutton(options_frame, text="Verbose output",
                        variable=self.verbose).grid(row=2, column=0, columnspan=2, sticky="w", pady=2)

        # === Layer Mapping Section ===
        self.layer_frame = ttk.LabelFrame(main_frame, text="Layer Mapping", padding="5")
        self.layer_frame.grid(row=row, column=0, columnspan=3, sticky="ew", pady=(0, 10))
        self.layer_frame.columnconfigure(0, weight=1)
        row += 1

        # Header row
        layer_header = ttk.Frame(self.layer_frame)
        layer_header.grid(row=0, column=0, sticky="ew")
        layer_header.columnconfigure(0, weight=1)
        layer_header.columnconfigure(1, weight=0)
        layer_header.columnconfigure(2, weight=1)
        ttk.Label(layer_header, text="IPC Layer", font=("", 9, "bold")).grid(
            row=0, column=0, sticky="w", padx=5)
        ttk.Label(layer_header, text="Function", font=("", 9, "bold")).grid(
            row=0, column=1, sticky="w", padx=5)
        ttk.Label(layer_header, text="KiCad Layer", font=("", 9, "bold")).grid(
            row=0, column=2, sticky="w", padx=5)

        # Scrollable layer table
        layer_canvas = tk.Canvas(self.layer_frame, height=130, highlightthickness=0)
        layer_scrollbar = ttk.Scrollbar(self.layer_frame, orient="vertical",
                                         command=layer_canvas.yview)
        self.layer_table_frame = ttk.Frame(layer_canvas)
        self.layer_table_frame.columnconfigure(0, weight=1)
        self.layer_table_frame.columnconfigure(1, weight=0)
        self.layer_table_frame.columnconfigure(2, weight=1)

        self.layer_table_frame.bind(
            "<Configure>",
            lambda e: layer_canvas.configure(scrollregion=layer_canvas.bbox("all")))
        layer_canvas.create_window((0, 0), window=self.layer_table_frame, anchor="nw")
        layer_canvas.configure(yscrollcommand=layer_scrollbar.set)

        layer_canvas.grid(row=1, column=0, sticky="ew")
        layer_scrollbar.grid(row=1, column=1, sticky="ns")

        # Bind mousewheel scrolling on the canvas
        def _on_mousewheel(event):
            layer_canvas.yview_scroll(-1 * (event.delta // 120 or (
                -1 if event.num == 5 else 1)), "units")
        layer_canvas.bind("<MouseWheel>", _on_mousewheel)  # Windows/macOS
        layer_canvas.bind("<Button-4>", _on_mousewheel)    # Linux scroll up
        layer_canvas.bind("<Button-5>", _on_mousewheel)    # Linux scroll down
        self._layer_canvas = layer_canvas

        # Placeholder label
        self.layer_placeholder = ttk.Label(self.layer_table_frame,
                                            text="(load a file to see layer mapping)",
                                            foreground="gray")
        self.layer_placeholder.grid(row=0, column=0, columnspan=3, pady=10)

        # Reset button
        layer_btn_frame = ttk.Frame(self.layer_frame)
        layer_btn_frame.grid(row=2, column=0, columnspan=2, sticky="w", pady=(5, 0))
        self.layer_reset_btn = ttk.Button(layer_btn_frame, text="Reset to Auto",
                                           command=self._reset_layer_mapping,
                                           state="disabled")
        self.layer_reset_btn.pack(side="left", padx=5)

        # === Action Buttons ===
        button_frame = ttk.Frame(main_frame)
        button_frame.grid(row=row, column=0, columnspan=3, pady=10)
        row += 1

        self.convert_btn = ttk.Button(button_frame, text="Convert",
                                       command=self._convert, style="Accent.TButton")
        self.convert_btn.pack(side="left", padx=5)

        ttk.Button(button_frame, text="List Layers",
                   command=self._list_layers).pack(side="left", padx=5)

        self.viewer_2d_btn = ttk.Button(button_frame, text="2D Viewer",
                                         command=self._open_2d_viewer)
        self.viewer_2d_btn.pack(side="left", padx=5)

        self.viewer_3d_btn = ttk.Button(button_frame, text="3D View",
                                         command=self._open_3d_viewer)
        self.viewer_3d_btn.pack(side="left", padx=5)

        ttk.Button(button_frame, text="Clear Log",
                   command=self._clear_log).pack(side="left", padx=5)

        # === Progress Bar ===
        self.progress = ttk.Progressbar(main_frame, mode="indeterminate")
        self.progress.grid(row=row, column=0, columnspan=3, sticky="ew", pady=(0, 10))
        row += 1

        # === Log Output ===
        ttk.Label(main_frame, text="Output Log:", font=("", 10, "bold")).grid(
            row=row, column=0, sticky="w", pady=(0, 5))
        row += 1

        self.log_text = scrolledtext.ScrolledText(main_frame, height=15, width=80,
                                                   font=("Courier", 10))
        self.log_text.grid(row=row, column=0, columnspan=3, sticky="nsew", pady=(0, 10))
        main_frame.rowconfigure(row, weight=1)

        # Configure log text tags
        self.log_text.tag_config("error", foreground="red")
        self.log_text.tag_config("success", foreground="green")
        self.log_text.tag_config("info", foreground="blue")

        row += 1

        # === Status Bar ===
        self.status_var = tk.StringVar(value="Ready")
        status_bar = ttk.Label(main_frame, textvariable=self.status_var, relief="sunken")
        status_bar.grid(row=row, column=0, columnspan=3, sticky="ew")

        # Style configuration
        style = ttk.Style()
        try:
            style.configure("Accent.TButton", font=("", 10, "bold"))
        except:
            pass

    def _browse_input(self):
        """Open file dialog for input file selection."""
        filename = filedialog.askopenfilename(
            title="Select Input File",
            filetypes=[
                ("All supported", "*.xml *.cvg *.tgz *.zip *.gbr *.ger *.gtl *.gbl"),
                ("IPC-2581 files", "*.xml *.cvg"),
                ("ODB++ archives", "*.tgz *.zip"),
                ("Gerber files", "*.gbr *.ger *.gtl *.gbl *.gts *.gbs *.gto *.gbo"),
                ("All files", "*.*")
            ]
        )
        if filename:
            self.input_file.set(filename)
            # Auto-set output filename
            base = os.path.splitext(filename)[0]
            self.output_file.set(base + ".kicad_pcb")
            # Auto-load steps and layers
            self._load_steps()
            self._load_layer_mapping()

    def _browse_gerber_folder(self):
        """Open folder dialog for Gerber directory import."""
        folder = filedialog.askdirectory(title="Select Gerber Folder")
        if folder:
            self.input_file.set(folder)
            # Auto-set output filename from directory name
            dirname = os.path.basename(folder.rstrip("/\\"))
            self.output_file.set(os.path.join(folder, dirname + ".kicad_pcb"))
            # Load steps (will find none, that's fine)
            self._load_steps()
            # Load layer mapping using --gerber-dir
            self._load_layer_mapping()

    def _is_gerber_dir_mode(self):
        """Check if current input is a directory (Gerber folder mode)."""
        input_path = self.input_file.get()
        return input_path and os.path.isdir(input_path)

    def _load_layer_mapping(self):
        """Load layer mapping from the input file via --list-layers --export-json."""
        if not self.converter_path:
            return

        input_path = self.input_file.get()
        if not input_path or not os.path.exists(input_path):
            return

        self._set_status("Loading layer mapping...")

        def run():
            try:
                cmd = [self.converter_path, "--list-layers", "--export-json"]
                if os.path.isdir(input_path):
                    cmd.extend(["--gerber-dir", input_path])
                else:
                    cmd.append(input_path)
                result = subprocess.run(
                    cmd,
                    capture_output=True,
                    text=True,
                    timeout=30
                )
                self.root.after(0, lambda: self._process_layer_mapping(result))
            except Exception as e:
                self.root.after(0, lambda: self._log(f"Layer mapping error: {e}", "error"))
                self.root.after(0, lambda: self._set_status("Ready"))

        threading.Thread(target=run, daemon=True).start()

    def _process_layer_mapping(self, result):
        """Process the JSON layer mapping output and populate the table."""
        self._set_status("Ready")

        if result.returncode != 0:
            self._log(f"Error loading layers: {result.stderr}", "error")
            return

        try:
            self.layer_data = json.loads(result.stdout)
        except json.JSONDecodeError as e:
            self._log(f"Error parsing layer JSON: {e}", "error")
            return

        # KiCad layer choices for comboboxes
        kicad_layers = [
            "(unmapped)",
            "F.Cu", "B.Cu",
            "In1.Cu", "In2.Cu", "In3.Cu", "In4.Cu", "In5.Cu", "In6.Cu",
            "In7.Cu", "In8.Cu", "In9.Cu", "In10.Cu",
            "F.Mask", "B.Mask",
            "F.Paste", "B.Paste",
            "F.SilkS", "B.SilkS",
            "F.Fab", "B.Fab",
            "F.CrtYd", "B.CrtYd",
            "Edge.Cuts",
            "Dwgs.User", "Cmts.User",
            "Eco1.User", "Eco2.User",
            "Margin",
        ]

        # Clear existing table
        for w in self.layer_table_frame.winfo_children():
            w.destroy()
        self.layer_combos = []

        for i, layer in enumerate(self.layer_data):
            ipc_name = layer.get("ipc_name", "")
            ipc_func = layer.get("ipc_function", "")
            auto_kicad = layer.get("kicad_name", "")
            display_kicad = auto_kicad if auto_kicad else "(unmapped)"

            ttk.Label(self.layer_table_frame, text=ipc_name).grid(
                row=i, column=0, sticky="w", padx=5, pady=1)
            ttk.Label(self.layer_table_frame, text=ipc_func,
                      foreground="gray").grid(
                row=i, column=1, sticky="w", padx=5, pady=1)

            combo = ttk.Combobox(self.layer_table_frame, values=kicad_layers,
                                  width=14, state="readonly")
            combo.set(display_kicad)
            combo.grid(row=i, column=2, sticky="w", padx=5, pady=1)

            self.layer_combos.append((ipc_name, auto_kicad, combo))

        self.layer_reset_btn.state(['!disabled'])
        self._log(f"Layer mapping loaded: {len(self.layer_data)} layers", "success")

        # Update scroll region
        self.layer_table_frame.update_idletasks()
        self._layer_canvas.configure(
            scrollregion=self._layer_canvas.bbox("all"))

    def _reset_layer_mapping(self):
        """Reset all layer comboboxes to auto-detected values."""
        for ipc_name, auto_kicad, combo in self.layer_combos:
            combo.set(auto_kicad if auto_kicad else "(unmapped)")
        self._log("Layer mapping reset to auto-detected values", "info")

    def _get_layer_overrides(self):
        """Return list of (ipc_name, kicad_name) for any user-modified layers."""
        overrides = []
        for ipc_name, auto_kicad, combo in self.layer_combos:
            current = combo.get()
            auto_display = auto_kicad if auto_kicad else "(unmapped)"
            if current != auto_display:
                # Convert "(unmapped)" to empty string for CLI
                kicad = "" if current == "(unmapped)" else current
                overrides.append((ipc_name, kicad))
        return overrides

    def _browse_output(self):
        """Open file dialog for output file selection."""
        filename = filedialog.asksaveasfilename(
            title="Save KiCad PCB File",
            defaultextension=".kicad_pcb",
            filetypes=[
                ("KiCad PCB files", "*.kicad_pcb"),
                ("All files", "*.*")
            ]
        )
        if filename:
            self.output_file.set(filename)

    def _load_steps(self):
        """Load available steps from the input file."""
        if not self.converter_path:
            self._log("Converter not found", "error")
            return

        input_path = self.input_file.get()
        if not input_path or not os.path.exists(input_path):
            self._log("Please select a valid input file first", "error")
            return

        self._log("Loading steps from file...", "info")
        self._set_status("Loading steps...")

        def run():
            try:
                result = subprocess.run(
                    [self.converter_path, "--list-steps", input_path],
                    capture_output=True,
                    text=True,
                    timeout=30
                )
                self.root.after(0, lambda: self._process_steps(result))
            except Exception as e:
                self.root.after(0, lambda: self._log(f"Error: {e}", "error"))
                self.root.after(0, lambda: self._set_status("Ready"))

        threading.Thread(target=run, daemon=True).start()

    def _process_steps(self, result):
        """Process the list-steps output."""
        self._set_status("Ready")

        if result.returncode != 0:
            self._log(f"Error loading steps: {result.stderr}", "error")
            return

        # Parse steps from output
        steps = []
        for line in result.stdout.split('\n'):
            line = line.strip()
            if line and not line.startswith("Steps in") and not line.startswith("No steps"):
                steps.append(line)

        self.available_steps = steps

        if steps:
            self.step_combo['values'] = ["(first step - default)"] + steps
            self.step_combo.set("(first step - default)")
            self._log(f"Found {len(steps)} step(s): {', '.join(steps)}", "success")
        else:
            self.step_combo['values'] = ["(no steps found)"]
            self.step_combo.set("(no steps found)")
            self._log("No steps found in file", "error")

    def _list_layers(self):
        """Show layer mapping for the input file."""
        if not self.converter_path:
            self._log("Converter not found", "error")
            return

        input_path = self.input_file.get()
        if not input_path or not os.path.exists(input_path):
            self._log("Please select a valid input file first", "error")
            return

        self._log("Loading layer mapping...", "info")
        self._set_status("Loading layers...")

        def run():
            try:
                result = subprocess.run(
                    [self.converter_path, "--list-layers", "--verbose", input_path],
                    capture_output=True,
                    text=True,
                    timeout=30
                )
                self.root.after(0, lambda: self._show_result(result, "Layer mapping loaded"))
            except Exception as e:
                self.root.after(0, lambda: self._log(f"Error: {e}", "error"))
                self.root.after(0, lambda: self._set_status("Ready"))

        threading.Thread(target=run, daemon=True).start()

    def _convert(self):
        """Run the conversion."""
        if not self.converter_path:
            messagebox.showerror("Error", "Converter executable not found!\n\n"
                               "Please build the project first:\n"
                               "cd build && cmake .. && make")
            return

        input_path = self.input_file.get()
        output_path = self.output_file.get()

        if not input_path:
            messagebox.showerror("Error", "Please select an input file")
            return

        if not os.path.exists(input_path):
            messagebox.showerror("Error", f"Input file not found:\n{input_path}")
            return

        if not output_path:
            messagebox.showerror("Error", "Please specify an output file")
            return

        # Build command
        cmd = [self.converter_path]

        if self.verbose.get():
            cmd.append("--verbose")

        cmd.extend(["-v", self.kicad_version.get()])

        # Add step if selected
        step = self.selected_step.get()
        if step and step not in ["(first step - default)", "(auto-detect from file)", "(no steps found)"]:
            cmd.extend(["-s", step])

        # Add layer mapping overrides
        for ipc_name, kicad_name in self._get_layer_overrides():
            cmd.extend(["--layer-map", f"{ipc_name}={kicad_name}"])

        cmd.extend(["-o", output_path])

        # Use --gerber-dir for directory input, else normal positional arg
        if os.path.isdir(input_path):
            cmd.extend(["--gerber-dir", input_path])
        else:
            cmd.append(input_path)

        self._log(f"Running: {' '.join(cmd)}", "info")
        self._log("-" * 60)
        self._set_status("Converting...")
        self.convert_btn.state(['disabled'])
        self.progress.start(10)

        def run():
            try:
                result = subprocess.run(
                    cmd,
                    capture_output=True,
                    text=True,
                    timeout=300
                )
                self.root.after(0, lambda: self._conversion_complete(result, output_path))
            except subprocess.TimeoutExpired:
                self.root.after(0, lambda: self._conversion_error("Conversion timed out"))
            except Exception as e:
                self.root.after(0, lambda: self._conversion_error(str(e)))

        threading.Thread(target=run, daemon=True).start()

    def _conversion_complete(self, result, output_path):
        """Handle conversion completion."""
        self.progress.stop()
        self.convert_btn.state(['!disabled'])

        # Log output
        if result.stdout:
            for line in result.stdout.split('\n'):
                if line.strip():
                    self._log(line)

        if result.stderr:
            for line in result.stderr.split('\n'):
                if line.strip():
                    self._log(line, "error")

        if result.returncode == 0:
            self._log("-" * 60)
            self._log(f"SUCCESS: Output saved to {output_path}", "success")
            self._set_status("Conversion complete!")

            # Ask if user wants to open the output folder
            if messagebox.askyesno("Success",
                                   f"Conversion complete!\n\n"
                                   f"Output: {output_path}\n\n"
                                   f"Open containing folder?"):
                self._open_folder(os.path.dirname(output_path))
        else:
            self._log("-" * 60)
            self._log("FAILED: Conversion failed", "error")
            self._set_status("Conversion failed")
            messagebox.showerror("Error", "Conversion failed. Check the log for details.")

    def _conversion_error(self, error_msg):
        """Handle conversion error."""
        self.progress.stop()
        self.convert_btn.state(['!disabled'])
        self._log(f"Error: {error_msg}", "error")
        self._set_status("Error")
        messagebox.showerror("Error", f"Conversion error:\n{error_msg}")

    def _get_selected_step(self):
        """Return the selected step name, or None for default."""
        step = self.selected_step.get()
        if step and step not in ["(first step - default)",
                                  "(auto-detect from file)",
                                  "(no steps found)"]:
            return step
        return None

    def _open_2d_viewer(self):
        """Export JSON and open the 2D viewer."""
        self._open_viewer("2D")

    def _open_3d_viewer(self):
        """Export JSON and open the 3D viewer."""
        self._open_viewer("3D")

    def _open_viewer(self, mode):
        """Run --export-json in background, then open the viewer window."""
        if not self.converter_path:
            messagebox.showerror("Error", "Converter executable not found!")
            return

        input_path = self.input_file.get()
        if not input_path or not os.path.exists(input_path):
            messagebox.showerror("Error", "Please select a valid input file first")
            return

        self._log(f"Loading PCB data for {mode} viewer...", "info")
        self._set_status(f"Exporting JSON for {mode} viewer...")
        self.progress.start(10)

        def run():
            try:
                pcb = PcbData()
                pcb.load_from_file(self.converter_path, input_path,
                                   step=self._get_selected_step())
                self.root.after(0, lambda: self._viewer_ready(pcb, mode))
            except Exception as e:
                self.root.after(0, lambda: self._viewer_error(str(e)))

        threading.Thread(target=run, daemon=True).start()

    def _viewer_ready(self, pcb, mode):
        """Called on main thread when JSON data is ready."""
        self.progress.stop()
        self._set_status("Ready")
        self._log(f"{mode} viewer data loaded: {len(pcb.components)} components, "
                  f"{len(pcb.traces)} traces", "success")

        if mode == "2D":
            PcbViewer2D(self.root, pcb)
        else:
            PcbViewer3D(self.root, pcb)

    def _viewer_error(self, error_msg):
        """Called on main thread if JSON export fails."""
        self.progress.stop()
        self._set_status("Ready")
        self._log(f"Viewer error: {error_msg}", "error")
        messagebox.showerror("Viewer Error", error_msg)

    def _show_result(self, result, success_msg):
        """Show command result in log."""
        self._set_status("Ready")

        if result.stdout:
            for line in result.stdout.split('\n'):
                if line.strip():
                    self._log(line)

        if result.stderr:
            for line in result.stderr.split('\n'):
                if line.strip():
                    self._log(line, "error")

        if result.returncode == 0:
            self._log(success_msg, "success")

    def _log(self, message, tag=None):
        """Add a message to the log."""
        self.log_text.insert(tk.END, message + "\n", tag)
        self.log_text.see(tk.END)

    def _clear_log(self):
        """Clear the log text."""
        self.log_text.delete(1.0, tk.END)

    def _set_status(self, status):
        """Set the status bar text."""
        self.status_var.set(status)

    def _open_folder(self, path):
        """Open a folder in the system file manager."""
        import platform
        system = platform.system()

        try:
            if system == "Darwin":  # macOS
                subprocess.run(["open", path])
            elif system == "Windows":
                subprocess.run(["explorer", path])
            else:  # Linux
                subprocess.run(["xdg-open", path])
        except Exception as e:
            self._log(f"Could not open folder: {e}", "error")


def main():
    root = tk.Tk()

    # Set icon if available
    try:
        # Try to set a generic PCB-related icon
        pass
    except:
        pass

    app = Ipc2581ConverterGUI(root)

    def open_file(input_path):
        """Load a file or directory into the GUI input field."""
        if os.path.isfile(input_path):
            app.input_file.set(input_path)
            base = os.path.splitext(input_path)[0]
            app.output_file.set(base + ".kicad_pcb")
            app._log(f"Opened: {input_path}", "info")
            root.after(500, app._load_steps)
            root.after(600, app._load_layer_mapping)
        elif os.path.isdir(input_path):
            app.input_file.set(input_path)
            dirname = os.path.basename(input_path.rstrip("/\\"))
            app.output_file.set(os.path.join(input_path, dirname + ".kicad_pcb"))
            app._log(f"Opened folder: {input_path}", "info")
            root.after(500, app._load_steps)
            root.after(600, app._load_layer_mapping)

    # Handle macOS "Open With" / drag-and-drop via Tk Apple Event handler.
    # When Finder opens a file with this app, macOS sends an odoc Apple Event
    # which Tk receives and dispatches to ::tk::mac::OpenDocument.
    try:
        root.createcommand('::tk::mac::OpenDocument', lambda *args: open_file(args[0]))
    except Exception:
        pass

    # Also handle file path passed as command-line argument (direct invocation)
    if len(sys.argv) > 1:
        open_file(sys.argv[1])

    root.mainloop()


if __name__ == "__main__":
    main()
