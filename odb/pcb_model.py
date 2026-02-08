"""Intermediate data model for PCB conversion.

Mirrors the structure of ipc2581-to-kicad's pcb_model.h using Python dataclasses.
All coordinates are in mm with KiCad's Y+ down convention.
"""

from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Optional


class PadShape(Enum):
    CIRCLE = auto()
    RECT = auto()
    OVAL = auto()
    ROUNDRECT = auto()
    CUSTOM = auto()


class LayerType(Enum):
    SIGNAL = auto()
    POWER = auto()
    MIXED = auto()
    SOLDER_MASK = auto()
    SILK_SCREEN = auto()
    SOLDER_PASTE = auto()
    DRILL = auto()
    DOCUMENT = auto()
    COMPONENT = auto()
    OTHER = auto()


class Side(Enum):
    TOP = auto()
    BOTTOM = auto()
    BOTH = auto()


@dataclass
class Point:
    x: float = 0.0
    y: float = 0.0


@dataclass
class PadDef:
    shape: PadShape = PadShape.CIRCLE
    width: float = 0.0
    height: float = 0.0
    # For roundrect
    roundrect_ratio: float = 0.0
    # For custom pads: list of (x, y) outline points
    custom_outline: list = field(default_factory=list)
    # Drill hole diameter (0 = SMD)
    drill: float = 0.0


@dataclass
class FootprintPad:
    number: str = ""
    pad_def: PadDef = field(default_factory=PadDef)
    pos: Point = field(default_factory=Point)
    # Rotation in degrees
    rotation: float = 0.0
    net_index: int = 0
    net_name: str = ""
    # "thru_hole", "smd", "np_thru_hole"
    pad_type: str = "smd"
    # Layers this pad appears on
    layers: list = field(default_factory=list)


@dataclass
class Footprint:
    name: str = ""
    pads: list = field(default_factory=list)  # list of FootprintPad
    # Courtyard / silkscreen outlines as list of (type, data)
    graphics: list = field(default_factory=list)


@dataclass
class ComponentInstance:
    reference: str = ""
    footprint_name: str = ""
    footprint: Optional[Footprint] = None
    pos: Point = field(default_factory=Point)
    rotation: float = 0.0
    side: Side = Side.TOP
    # Properties dict
    properties: dict = field(default_factory=dict)


@dataclass
class TraceSegment:
    start: Point = field(default_factory=Point)
    end: Point = field(default_factory=Point)
    width: float = 0.0
    layer: str = ""
    net_index: int = 0


@dataclass
class TraceArc:
    start: Point = field(default_factory=Point)
    mid: Point = field(default_factory=Point)
    end: Point = field(default_factory=Point)
    width: float = 0.0
    layer: str = ""
    net_index: int = 0


@dataclass
class Via:
    pos: Point = field(default_factory=Point)
    diameter: float = 0.0
    drill: float = 0.0
    net_index: int = 0
    layers: tuple = ("F.Cu", "B.Cu")


@dataclass
class ZonePolygon:
    """A single polygon outline (possibly with holes) in a zone."""
    outline: list = field(default_factory=list)  # list of Point
    holes: list = field(default_factory=list)  # list of list of Point


@dataclass
class Zone:
    net_index: int = 0
    net_name: str = ""
    layer: str = ""
    polygons: list = field(default_factory=list)  # list of ZonePolygon


@dataclass
class GraphicItem:
    """Board-level graphic (outline, text, etc.)."""
    item_type: str = "line"  # "line", "arc", "circle", "rect"
    layer: str = "Edge.Cuts"
    start: Point = field(default_factory=Point)
    end: Point = field(default_factory=Point)
    # For arcs: midpoint
    mid: Optional[Point] = None
    width: float = 0.05
    fill: bool = False


@dataclass
class LayerDef:
    odb_name: str = ""
    kicad_name: str = ""
    layer_type: LayerType = LayerType.SIGNAL
    side: Side = Side.TOP
    layer_id: int = 0
    polarity: str = "positive"
    # Copper layer ordering (0 = top, N-1 = bottom)
    copper_order: int = -1


@dataclass
class NetDef:
    index: int = 0
    name: str = ""


@dataclass
class StackupLayer:
    name: str = ""
    layer_type: str = "copper"  # "copper", "core", "prepreg", "soldermask"
    thickness: float = 0.035  # mm
    material: str = ""
    epsilon_r: float = 4.5


@dataclass
class PcbModel:
    # Board info
    job_name: str = ""
    units: str = "MM"
    board_thickness: float = 1.6

    # Layers
    layers: list = field(default_factory=list)  # list of LayerDef
    stackup: list = field(default_factory=list)  # list of StackupLayer

    # Nets
    nets: list = field(default_factory=list)  # list of NetDef

    # Board outline
    outline: list = field(default_factory=list)  # list of GraphicItem

    # Components
    components: list = field(default_factory=list)  # list of ComponentInstance

    # Routing
    traces: list = field(default_factory=list)  # list of TraceSegment
    arcs: list = field(default_factory=list)  # list of TraceArc
    vias: list = field(default_factory=list)  # list of Via
    zones: list = field(default_factory=list)  # list of Zone

    # Graphics
    graphics: list = field(default_factory=list)  # list of GraphicItem

    # Internal maps used during parsing (not written to output)
    _net_name_to_index: dict = field(default_factory=dict, repr=False)
    _feature_to_net: dict = field(default_factory=dict, repr=False)
    _symbols: dict = field(default_factory=dict, repr=False)
    _pad_usage: dict = field(default_factory=dict, repr=False)
