# IPC-2581 / ODB++ to KiCad Converter

A command-line utility and GUI application that converts IPC-2581 XML and ODB++ files to KiCad `.kicad_pcb` format.

## Features

- Converts **IPC-2581** revision B/C XML files to KiCad PCB format
- Converts **ODB++** archives (.tgz, .zip) and extracted directories to KiCad PCB format
- Supports **KiCad 7.x**, **8.x**, and **9.x** output formats
- Parses complete PCB data:
  - Board outline (with arcs/curves)
  - Layer stackup with materials and dielectric properties
  - Component footprints and placements
  - Copper traces and arcs
  - Vias (through-hole and blind/buried)
  - Copper pours/zones
  - Pad stacks (SMD, through-hole, NPTH)
  - Net connectivity
  - Silkscreen, soldermask, paste layers
- JSON import/export for interoperability
- GUI application for easy point-and-click conversion
- Cross-platform (macOS, Linux, Windows)

## Building

### Requirements

- CMake 3.14+
- C++17 compiler (GCC, Clang, MSVC)
- Python 3.x (required for ODB++ support and GUI)

### Build Steps

```bash
mkdir build && cd build
cmake ..
make
```

The executable `ipc2581-to-kicad` will be created in the `build/` directory.

## Usage

### Command Line

```
Usage: ipc2581-to-kicad [options] <input>

Supported input formats:
  .xml, .cvg          IPC-2581 XML files
  .tgz, .tar.gz, .zip ODB++ archives (requires Python 3)
  directory/           ODB++ extracted directory (requires Python 3)
  .json                JSON (with --import-json)

Options:
  -o, --output <file>       Output .kicad_pcb file (default: <input>.kicad_pcb)
  -v, --version <7|8|9>     Target KiCad version (default: 9)
  -s, --step <name>         Step name to convert (default: first step)
  --list-steps              List available steps and exit
  --list-layers             List layer mapping and exit
  --export-json             Export parsed PCB data as JSON to stdout
  --import-json             Import from JSON file (output of --export-json)
  --verbose                 Verbose output during conversion
  -h, --help                Show help
```

### Examples

```bash
# Convert IPC-2581 XML (auto-detected by .xml extension)
./build/ipc2581-to-kicad board.xml

# Convert ODB++ archive (auto-detected by .tgz extension)
./build/ipc2581-to-kicad board.tgz -o output.kicad_pcb

# Convert ODB++ directory
./build/ipc2581-to-kicad /path/to/odb_directory/ -o output.kicad_pcb

# Specify KiCad version
./build/ipc2581-to-kicad -v 8 board.tgz -o output.kicad_pcb

# List available steps
./build/ipc2581-to-kicad --list-steps board.tgz

# Convert a specific step with verbose output
./build/ipc2581-to-kicad -s "MainBoard" --verbose board.xml

# Export to JSON, then re-import
./build/ipc2581-to-kicad --export-json board.xml > board.json
./build/ipc2581-to-kicad --import-json board.json -o board.kicad_pcb

# Show layer mapping
./build/ipc2581-to-kicad --list-layers board.xml
```

### ODB++ Python Parser (standalone)

The ODB++ parser can also be used directly:

```bash
# Output JSON to stdout
python3 odb/odb_to_json.py board.tgz

# List steps in an ODB++ archive
python3 odb/odb_to_json.py --list-steps board.tgz

# Verbose mode
python3 odb/odb_to_json.py -v board.tgz > board.json
```

### GUI Application

A graphical interface is available in the `gui/` folder:

```bash
# macOS/Linux
./gui/run_gui.sh

# Windows
gui\run_gui.bat

# macOS (double-click)
# Open gui/IPC2581-Converter.command in Finder
```

The GUI provides:
- File browser supporting both IPC-2581 (.xml, .cvg) and ODB++ (.tgz, .zip) files
- KiCad version selection (7, 8, or 9)
- Step selection dropdown
- Layer mapping preview
- 2D and 3D board viewers
- Progress indication and log output

![GUI Screenshot](gui/screenshot.png)

## Architecture

```
IPC-2581 (.xml/.cvg)
  -> [C++: ipc2581_parser.cpp]
  -> PcbModel
  -> [C++: kicad_writer.cpp]
  -> .kicad_pcb

ODB++ (.tgz/.zip/dir)
  -> [Python: odb/odb_to_json.py]
  -> JSON (stdout)
  -> [C++: json_import.cpp]
  -> PcbModel
  -> [C++: kicad_writer.cpp]
  -> .kicad_pcb
```

Both paths share the same C++ `PcbModel` and `KicadWriter`, ensuring consistent output.

## Sample Files

A sample IPC-2581 file is provided in `samples/`:

```bash
./build/ipc2581-to-kicad samples/led_power_board.xml -o samples/led_power_board.kicad_pcb
```

Open the output in KiCad to see:
- 40x25mm board with rounded corner
- Voltage regulator circuit with LED
- SMD and through-hole components
- Top and bottom copper layers
- Ground plane (zone fill)
- Multiple net classes

## Layer Mapping

### IPC-2581

| IPC-2581 layerFunction | KiCad Layer |
|------------------------|-------------|
| SIGNAL, POWER_GROUND (top) | F.Cu |
| SIGNAL, POWER_GROUND (bottom) | B.Cu |
| SIGNAL, POWER_GROUND (inner) | In{N}.Cu |
| SOLDERMASK (top/bottom) | F.Mask / B.Mask |
| PASTEMASK (top/bottom) | F.Paste / B.Paste |
| SILKSCREEN (top/bottom) | F.SilkS / B.SilkS |
| ASSEMBLY (top/bottom) | F.Fab / B.Fab |
| BOARD_OUTLINE | Edge.Cuts |
| DOCUMENT | Cmts.User |

### ODB++

| ODB++ Layer Type | KiCad Layer |
|------------------|-------------|
| SIGNAL (first copper) | F.Cu |
| SIGNAL (last copper) | B.Cu |
| SIGNAL (inner copper) | In{N}.Cu |
| SOLDER_MASK | F.Mask / B.Mask |
| SILK_SCREEN | F.SilkS / B.SilkS |
| SOLDER_PASTE | F.Paste / B.Paste |
| COMPONENT | F.Fab / B.Fab |
| DRILL | (vias/holes) |

## Project Structure

```
ipc2581-to-kicad/
├── CMakeLists.txt          # Build configuration
├── README.md
├── src/
│   ├── main.cpp            # CLI entry point (IPC-2581 + ODB++ + JSON)
│   ├── pcb_model.h         # Intermediate data model
│   ├── geometry.h/.cpp     # Geometry primitives
│   ├── utils.h/.cpp        # Utility functions
│   ├── ipc2581_parser.h/.cpp  # IPC-2581 XML parser
│   ├── json_export.h/.cpp  # PcbModel -> JSON serialization
│   ├── json_import.h/.cpp  # JSON -> PcbModel deserialization
│   └── kicad_writer.h/.cpp # KiCad output writer
├── odb/
│   ├── __init__.py         # Python package
│   ├── pcb_model.py        # Python PCB data model
│   ├── odb_parser.py       # ODB++ format parser
│   ├── utils.py            # Utility functions
│   └── odb_to_json.py      # ODB++ -> JSON bridge
├── third_party/
│   ├── pugixml/            # XML parser (vendored)
│   └── nlohmann/           # JSON parser (vendored, single-header)
├── gui/
│   ├── ipc2581_gui.py      # Python/tkinter GUI
│   ├── pcb_data.py         # PCB data layer (supports IPC-2581 + ODB++)
│   ├── run_gui.sh          # Linux/macOS launcher
│   ├── run_gui.bat         # Windows launcher
│   └── IPC2581-Converter.command  # macOS double-click launcher
├── test/
│   └── test_odb.py         # ODB++ parser and JSON bridge tests
└── samples/
    └── led_power_board.xml # Sample IPC-2581 file
```

## Testing

```bash
# Run ODB++ unit tests
python3 -m pytest test/test_odb.py -v
```

## Technical Notes

### Coordinate System
Both IPC-2581 and ODB++ use Y+ up, while KiCad uses Y+ down. The converter automatically flips Y coordinates.

### Arc Conversion
IPC-2581 uses center+sweep angle arcs; ODB++ uses center+CW/CCW arcs; KiCad uses start/mid/end points. Both parsers handle these transformations.

### UUID Generation
KiCad 8/9 requires UUIDs on all elements. The converter generates deterministic UUIDs based on object properties to ensure reproducible output.

### JSON Schema
The JSON interchange format is documented in the source. Use `--export-json` to see the schema for any input file. The same schema is produced by both the C++ IPC-2581 parser and the Python ODB++ parser.

## License

MIT License
