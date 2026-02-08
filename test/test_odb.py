#!/usr/bin/env python3
"""Tests for ODB++ parsing and JSON bridge.

Adapted from odb-to-kicad/test/test_convert.py with updated imports
for the ipc2581-to-kicad/odb/ package structure.
"""

import json
import os
import sys
import tempfile
import tarfile
import unittest
from pathlib import Path

# Add project root to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from odb.pcb_model import (
    PcbModel, Point, PadDef, PadShape, LayerDef, LayerType, NetDef, Side,
)
from odb.utils import (
    convert_to_mm, negate_y, fmt, arc_center_to_mid, make_uuid, reset_uuid_counter,
    inch_to_mm, mils_to_mm,
)
from odb.odb_parser import parse_odb, OdbParser
from odb.odb_to_json import model_to_json


class TestUtils(unittest.TestCase):
    """Test utility functions."""

    def test_inch_to_mm(self):
        self.assertAlmostEqual(inch_to_mm(1.0), 25.4)
        self.assertAlmostEqual(inch_to_mm(0.0), 0.0)

    def test_mils_to_mm(self):
        self.assertAlmostEqual(mils_to_mm(1000), 25.4)
        self.assertAlmostEqual(mils_to_mm(100), 2.54)

    def test_convert_to_mm_inch(self):
        self.assertAlmostEqual(convert_to_mm(1.0, "INCH"), 25.4)
        self.assertAlmostEqual(convert_to_mm(1.0, "IN"), 25.4)

    def test_convert_to_mm_mil(self):
        self.assertAlmostEqual(convert_to_mm(100, "MIL"), 2.54)

    def test_convert_to_mm_mm(self):
        self.assertAlmostEqual(convert_to_mm(10.0, "MM"), 10.0)

    def test_negate_y(self):
        self.assertEqual(negate_y(5.0), -5.0)
        self.assertEqual(negate_y(-3.0), 3.0)
        self.assertEqual(negate_y(0.0), 0.0)

    def test_fmt(self):
        self.assertEqual(fmt(1.0), "1")
        self.assertEqual(fmt(1.5), "1.5")
        self.assertEqual(fmt(1.123456), "1.123456")
        self.assertEqual(fmt(0.0), "0")

    def test_arc_center_to_mid(self):
        # Quarter circle: start=(1,0), end=(0,1), center=(0,0), CCW
        mid_x, mid_y = arc_center_to_mid(1, 0, 0, 1, 0, 0, clockwise=False)
        self.assertAlmostEqual(mid_x, 0.7071, places=3)
        self.assertAlmostEqual(mid_y, 0.7071, places=3)

    def test_uuid_deterministic(self):
        reset_uuid_counter()
        u1 = make_uuid("test-name")
        u2 = make_uuid("test-name")
        self.assertEqual(u1, u2)

    def test_uuid_sequential(self):
        reset_uuid_counter()
        u1 = make_uuid()
        u2 = make_uuid()
        self.assertNotEqual(u1, u2)


class TestPcbModel(unittest.TestCase):
    """Test data model creation."""

    def test_point_defaults(self):
        p = Point()
        self.assertEqual(p.x, 0.0)
        self.assertEqual(p.y, 0.0)

    def test_pad_def_circle(self):
        pd = PadDef(shape=PadShape.CIRCLE, width=1.0, height=1.0)
        self.assertEqual(pd.shape, PadShape.CIRCLE)
        self.assertEqual(pd.width, 1.0)

    def test_pcb_model_defaults(self):
        model = PcbModel()
        self.assertEqual(model.units, "MM")
        self.assertEqual(model.board_thickness, 1.6)
        self.assertEqual(len(model.layers), 0)
        self.assertEqual(len(model.nets), 0)


class TestSymbolParsing(unittest.TestCase):
    """Test ODB++ symbol name decoding."""

    def setUp(self):
        self.parser = OdbParser()

    def test_round_pad(self):
        pd = self.parser._symbol_name_to_pad("r100")
        self.assertEqual(pd.shape, PadShape.CIRCLE)
        self.assertAlmostEqual(pd.width, 2.54)  # 100 mils

    def test_square_pad(self):
        pd = self.parser._symbol_name_to_pad("s80")
        self.assertEqual(pd.shape, PadShape.RECT)
        self.assertAlmostEqual(pd.width, 2.032)  # 80 mils

    def test_rect_pad(self):
        pd = self.parser._symbol_name_to_pad("rect100x50")
        self.assertEqual(pd.shape, PadShape.RECT)
        self.assertAlmostEqual(pd.width, 2.54)
        self.assertAlmostEqual(pd.height, 1.27)

    def test_oval_pad(self):
        pd = self.parser._symbol_name_to_pad("oval60x40")
        self.assertEqual(pd.shape, PadShape.OVAL)
        self.assertAlmostEqual(pd.width, 1.524)
        self.assertAlmostEqual(pd.height, 1.016)

    def test_roundrect_pad(self):
        pd = self.parser._symbol_name_to_pad("rc100x50x10")
        self.assertEqual(pd.shape, PadShape.ROUNDRECT)
        self.assertAlmostEqual(pd.width, 2.54)
        self.assertAlmostEqual(pd.height, 1.27)


class TestEndToEnd(unittest.TestCase):
    """End-to-end integration tests with sample ODB++ data."""

    def _create_sample_odb(self, tmpdir: Path):
        """Create a minimal ODB++ directory structure for testing."""
        root = tmpdir / "sample_odb"

        # matrix/matrix
        matrix_dir = root / "matrix"
        matrix_dir.mkdir(parents=True)
        (matrix_dir / "matrix").write_text(
            "STEP {\n"
            "   NAME=pcb\n"
            "}\n"
            "LAYER {\n"
            "   NAME=top\n"
            "   TYPE=SIGNAL\n"
            "   POLARITY=POSITIVE\n"
            "}\n"
            "LAYER {\n"
            "   NAME=bottom\n"
            "   TYPE=SIGNAL\n"
            "   POLARITY=POSITIVE\n"
            "}\n"
            "LAYER {\n"
            "   NAME=drill\n"
            "   TYPE=DRILL\n"
            "   POLARITY=POSITIVE\n"
            "}\n"
        )

        # misc/info
        misc_dir = root / "misc"
        misc_dir.mkdir(parents=True)
        (misc_dir / "info").write_text(
            "UNITS=MM\n"
            "JOB_NAME=test_board\n"
        )

        # steps/pcb/profile
        step_dir = root / "steps" / "pcb"
        step_dir.mkdir(parents=True)
        (step_dir / "profile").write_text(
            "OB 0 0\n"
            "OS 100 0\n"
            "OS 100 80\n"
            "OS 0 80\n"
            "OE\n"
        )

        # steps/pcb/eda/data
        eda_dir = step_dir / "eda"
        eda_dir.mkdir(parents=True)
        (eda_dir / "data").write_text(
            "NET VCC\n"
            "NET GND\n"
        )

        # steps/pcb/layers/top/features
        top_dir = step_dir / "layers" / "top"
        top_dir.mkdir(parents=True)
        (top_dir / "features").write_text(
            "$0 r10\n"
            "L 10 10 50 10 0 P\n"
            "L 50 10 50 40 0 P\n"
        )

        # steps/pcb/layers/bottom/features
        bot_dir = step_dir / "layers" / "bottom"
        bot_dir.mkdir(parents=True)
        (bot_dir / "features").write_text(
            "$0 r10\n"
            "L 20 20 60 20 0 P\n"
        )

        # steps/pcb/layers/drill/features
        drill_dir = step_dir / "layers" / "drill"
        drill_dir.mkdir(parents=True)
        (drill_dir / "features").write_text(
            "$0 r12\n"
            "P 30 30 0 P\n"
        )

        # symbols/r10/features (empty is fine - name-based decode)
        sym_dir = root / "symbols" / "r10"
        sym_dir.mkdir(parents=True)
        (sym_dir / "features").write_text("")

        sym_dir2 = root / "symbols" / "r12"
        sym_dir2.mkdir(parents=True)
        (sym_dir2 / "features").write_text("")

        return root

    def test_parse_sample_directory(self):
        """Test parsing a sample ODB++ directory."""
        with tempfile.TemporaryDirectory() as tmpdir:
            odb_root = self._create_sample_odb(Path(tmpdir))
            model = parse_odb(odb_root)

            # Check layers
            copper = [l for l in model.layers
                      if l.layer_type in (LayerType.SIGNAL, LayerType.POWER, LayerType.MIXED)]
            self.assertEqual(len(copper), 2)
            self.assertEqual(copper[0].kicad_name, "F.Cu")
            self.assertEqual(copper[1].kicad_name, "B.Cu")

            # Check outline
            self.assertEqual(len(model.outline), 4)  # 4 edges of rectangle

            # Check nets
            self.assertTrue(len(model.nets) >= 3)  # "", VCC, GND

            # Check traces
            self.assertTrue(len(model.traces) >= 3)  # 2 on top, 1 on bottom

    def test_parse_tgz_archive(self):
        """Test parsing from a .tgz archive."""
        with tempfile.TemporaryDirectory() as tmpdir:
            odb_root = self._create_sample_odb(Path(tmpdir))
            tgz_path = Path(tmpdir) / "sample.tgz"

            # Create .tgz from the directory
            with tarfile.open(tgz_path, "w:gz") as tf:
                tf.add(odb_root, arcname="sample_odb")

            model = parse_odb(tgz_path)
            self.assertTrue(len(model.layers) > 0)
            self.assertTrue(len(model.outline) > 0)


class TestJsonBridge(unittest.TestCase):
    """Test the ODB++ to JSON bridge (odb_to_json.py)."""

    def _create_sample_odb(self, tmpdir: Path):
        """Create a minimal ODB++ directory structure for testing."""
        root = tmpdir / "sample_odb"

        matrix_dir = root / "matrix"
        matrix_dir.mkdir(parents=True)
        (matrix_dir / "matrix").write_text(
            "STEP {\n   NAME=pcb\n}\n"
            "LAYER {\n   NAME=top\n   TYPE=SIGNAL\n   POLARITY=POSITIVE\n}\n"
            "LAYER {\n   NAME=bottom\n   TYPE=SIGNAL\n   POLARITY=POSITIVE\n}\n"
        )

        misc_dir = root / "misc"
        misc_dir.mkdir(parents=True)
        (misc_dir / "info").write_text("UNITS=MM\nJOB_NAME=test_board\n")

        step_dir = root / "steps" / "pcb"
        step_dir.mkdir(parents=True)
        (step_dir / "profile").write_text(
            "OB 0 0\nOS 50 0\nOS 50 30\nOS 0 30\nOE\n"
        )

        eda_dir = step_dir / "eda"
        eda_dir.mkdir(parents=True)
        (eda_dir / "data").write_text("NET VCC\nNET GND\n")

        top_dir = step_dir / "layers" / "top"
        top_dir.mkdir(parents=True)
        (top_dir / "features").write_text("$0 r10\nL 5 5 25 5 0 P\n")

        bot_dir = step_dir / "layers" / "bottom"
        bot_dir.mkdir(parents=True)
        (bot_dir / "features").write_text("$0 r10\n")

        sym_dir = root / "symbols" / "r10"
        sym_dir.mkdir(parents=True)
        (sym_dir / "features").write_text("")

        return root

    def test_json_has_required_keys(self):
        """Test that model_to_json output has all required top-level keys."""
        with tempfile.TemporaryDirectory() as tmpdir:
            odb_root = self._create_sample_odb(Path(tmpdir))
            model = parse_odb(odb_root)
            result = model_to_json(model)

            required_keys = [
                "outline", "layers", "nets", "stackup", "footprints",
                "components", "traces", "trace_arcs", "vias", "zones",
                "graphics",
            ]
            for key in required_keys:
                self.assertIn(key, result, f"Missing key: {key}")

    def test_json_outline_structure(self):
        """Test that outline has segments and arcs arrays."""
        with tempfile.TemporaryDirectory() as tmpdir:
            odb_root = self._create_sample_odb(Path(tmpdir))
            model = parse_odb(odb_root)
            result = model_to_json(model)

            self.assertIn("segments", result["outline"])
            self.assertIn("arcs", result["outline"])
            self.assertIsInstance(result["outline"]["segments"], list)
            self.assertIsInstance(result["outline"]["arcs"], list)

            # Should have 4 segments for the rectangle
            self.assertEqual(len(result["outline"]["segments"]), 4)

            # Each segment should have start, end, width
            for seg in result["outline"]["segments"]:
                self.assertIn("start", seg)
                self.assertIn("end", seg)
                self.assertIn("width", seg)
                self.assertEqual(len(seg["start"]), 2)
                self.assertEqual(len(seg["end"]), 2)

    def test_json_layers_structure(self):
        """Test that layers have correct schema."""
        with tempfile.TemporaryDirectory() as tmpdir:
            odb_root = self._create_sample_odb(Path(tmpdir))
            model = parse_odb(odb_root)
            result = model_to_json(model)

            self.assertTrue(len(result["layers"]) >= 2)
            for layer in result["layers"]:
                self.assertIn("kicad_id", layer)
                self.assertIn("kicad_name", layer)
                self.assertIn("type", layer)
                self.assertIn("ipc_name", layer)
                self.assertIn("copper_order", layer)

    def test_json_nets_structure(self):
        """Test that nets have id and name."""
        with tempfile.TemporaryDirectory() as tmpdir:
            odb_root = self._create_sample_odb(Path(tmpdir))
            model = parse_odb(odb_root)
            result = model_to_json(model)

            # Should have at least 3 nets: "", VCC, GND
            self.assertTrue(len(result["nets"]) >= 3)
            for net in result["nets"]:
                self.assertIn("id", net)
                self.assertIn("name", net)

    def test_json_traces_structure(self):
        """Test that traces have correct schema."""
        with tempfile.TemporaryDirectory() as tmpdir:
            odb_root = self._create_sample_odb(Path(tmpdir))
            model = parse_odb(odb_root)
            result = model_to_json(model)

            self.assertTrue(len(result["traces"]) >= 1)
            for trace in result["traces"]:
                self.assertIn("start", trace)
                self.assertIn("end", trace)
                self.assertIn("width", trace)
                self.assertIn("layer", trace)
                self.assertIn("net_id", trace)

    def test_json_is_serializable(self):
        """Test that the JSON output is valid JSON."""
        with tempfile.TemporaryDirectory() as tmpdir:
            odb_root = self._create_sample_odb(Path(tmpdir))
            model = parse_odb(odb_root)
            result = model_to_json(model)

            # Should serialize without error
            json_str = json.dumps(result)
            # Should parse back without error
            parsed = json.loads(json_str)
            self.assertEqual(len(parsed["layers"]), len(result["layers"]))

    def test_json_stackup_structure(self):
        """Test stackup has board_thickness and layers array."""
        with tempfile.TemporaryDirectory() as tmpdir:
            odb_root = self._create_sample_odb(Path(tmpdir))
            model = parse_odb(odb_root)
            result = model_to_json(model)

            self.assertIn("board_thickness", result["stackup"])
            self.assertIn("layers", result["stackup"])
            self.assertIsInstance(result["stackup"]["layers"], list)


if __name__ == "__main__":
    unittest.main()
