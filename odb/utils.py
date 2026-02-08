"""Utility functions for ODB++ to KiCad conversion.

Handles unit conversion, coordinate transforms, arc geometry, and UUID generation.
"""

import math
import uuid

# Namespace UUID for deterministic UUID generation
_NAMESPACE = uuid.UUID("a1b2c3d4-e5f6-7890-abcd-ef1234567890")

_counter = 0


def make_uuid(name: str = "") -> str:
    """Generate a deterministic UUID from a name, or a sequential one."""
    global _counter
    if not name:
        _counter += 1
        name = f"odb2kicad-{_counter}"
    return str(uuid.uuid5(_NAMESPACE, name))


def reset_uuid_counter():
    """Reset the UUID counter (for testing)."""
    global _counter
    _counter = 0


def mils_to_mm(mils: float) -> float:
    return mils * 0.0254


def inch_to_mm(inches: float) -> float:
    return inches * 25.4


def convert_to_mm(value: float, units: str) -> float:
    """Convert a value from the given unit system to mm."""
    if units.upper() in ("INCH", "IN"):
        return inch_to_mm(value)
    elif units.upper() in ("MIL", "MILS", "TH"):
        return mils_to_mm(value)
    # Already mm
    return value


def negate_y(y: float) -> float:
    """ODB++ uses Y+ up, KiCad uses Y+ down."""
    return -y


def fmt(value: float) -> str:
    """Format a float for KiCad output: 6 decimal places, strip trailing zeros."""
    s = f"{value:.6f}"
    if "." in s:
        s = s.rstrip("0").rstrip(".")
    return s


def arc_center_to_mid(sx: float, sy: float, ex: float, ey: float,
                       cx: float, cy: float, clockwise: bool = False):
    """Convert arc defined by start/end/center to start/mid/end format.

    Returns (mid_x, mid_y).
    """
    # Angles from center to start and end
    a_start = math.atan2(sy - cy, sx - cx)
    a_end = math.atan2(ey - cy, ex - cx)

    if clockwise:
        # CW: sweep goes from start to end in negative direction
        if a_end >= a_start:
            a_end -= 2 * math.pi
        a_mid = (a_start + a_end) / 2
    else:
        # CCW: sweep goes from start to end in positive direction
        if a_end <= a_start:
            a_end += 2 * math.pi
        a_mid = (a_start + a_end) / 2

    radius = math.sqrt((sx - cx) ** 2 + (sy - cy) ** 2)
    mid_x = cx + radius * math.cos(a_mid)
    mid_y = cy + radius * math.sin(a_mid)

    return mid_x, mid_y


def parse_float(s: str) -> float:
    """Parse a float string, handling edge cases."""
    try:
        return float(s)
    except (ValueError, TypeError):
        return 0.0


def parse_int(s: str) -> int:
    """Parse an int string, handling edge cases."""
    try:
        return int(s)
    except (ValueError, TypeError):
        return 0
