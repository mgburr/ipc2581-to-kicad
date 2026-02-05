# IPC-2581 to KiCad Converter

A command-line utility and GUI application that converts IPC-2581 monolithic XML files to KiCad `.kicad_pcb` format.

## Features

- Converts IPC-2581 revision B/C XML files to KiCad PCB format
- Supports both **KiCad 7.x** and **KiCad 8.x** output formats
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
- GUI application for easy point-and-click conversion
- Cross-platform (macOS, Linux, Windows)

## Building

### Requirements

- CMake 3.14+
- C++17 compiler (GCC, Clang, MSVC)
- Python 3.x (for GUI only)

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
Usage: ipc2581-to-kicad [options] <input.xml>

Options:
  -o, --output <file>       Output .kicad_pcb file (default: <input>.kicad_pcb)
  -v, --version <7|8>       Target KiCad version (default: 8)
  -s, --step <name>         Step name to convert (default: first step)
  --list-steps              List available steps and exit
  --list-layers             List layer mapping and exit
  --verbose                 Verbose output during conversion
  -h, --help                Show help
```

### Examples

```bash
# Basic conversion (outputs to input.kicad_pcb, KiCad 8 format)
./ipc2581-to-kicad board.xml

# Specify output file and KiCad version
./ipc2581-to-kicad -o output.kicad_pcb -v 7 board.xml

# List available steps in a multi-step file
./ipc2581-to-kicad --list-steps board.xml

# Convert a specific step with verbose output
./ipc2581-to-kicad -s "MainBoard" --verbose board.xml

# Show layer mapping
./ipc2581-to-kicad --list-layers board.xml
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
- File browser for input/output selection
- KiCad version selection (7 or 8)
- Step selection dropdown
- Layer mapping preview
- Progress indication and log output

![GUI Screenshot](gui/screenshot.png)

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

## IPC-2581 Layer Mapping

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

## Project Structure

```
ipc2581-to-kicad/
├── CMakeLists.txt          # Build configuration
├── README.md
├── src/
│   ├── main.cpp            # CLI entry point
│   ├── pcb_model.h         # Intermediate data model
│   ├── geometry.h/.cpp     # Geometry primitives
│   ├── utils.h/.cpp        # Utility functions
│   ├── ipc2581_parser.h/.cpp  # IPC-2581 XML parser
│   └── kicad_writer.h/.cpp    # KiCad output writer
├── third_party/
│   └── pugixml/            # XML parser (vendored)
├── gui/
│   ├── ipc2581_gui.py      # Python/tkinter GUI
│   ├── run_gui.sh          # Linux/macOS launcher
│   ├── run_gui.bat         # Windows launcher
│   └── IPC2581-Converter.command  # macOS double-click launcher
└── samples/
    └── led_power_board.xml # Sample IPC-2581 file
```

## Technical Notes

### Coordinate System
IPC-2581 uses Y+ up, while KiCad uses Y+ down. The converter automatically flips Y coordinates.

### Arc Conversion
IPC-2581 uses center+sweep angle arcs; KiCad uses start/mid/end points. The converter handles this transformation.

### UUID Generation
KiCad 8 requires UUIDs on all elements. The converter generates deterministic UUIDs based on object properties to ensure reproducible output.

## License

MIT License
