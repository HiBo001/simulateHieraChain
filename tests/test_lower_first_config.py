"""Lower-first must be an explicit, Arbor-only streaming configuration."""
import copy
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import cluster as c
import benchmark_ordering_windows as w


class Configuration(unittest.TestCase):
    def setUp(self):
        self.raw = c.read(ROOT / "config/three_layer_lower_first.json")

    def test_valid_lower_first_and_legacy_default_preserved(self):
        self.assertEqual(c.validate(self.raw)["consensus"]["ancestor_order"], "lower_first")
        old = c.read(ROOT / "config/three_layer_locality.json")
        self.assertNotIn("ancestor_order", c.validate(old)["consensus"])

    def test_invalid_order_and_disabled_streaming_rejected(self):
        for value in (None, True, 1, [], {}, "arrival", "LOWER_FIRST"):
            raw = copy.deepcopy(self.raw);raw["consensus"]["ancestor_order"] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                c.validate(raw)
        raw = copy.deepcopy(self.raw);raw["consensus"]["ancestor_streaming"] = False
        with self.assertRaisesRegex(ValueError, "ancestor_streaming"):
            c.validate(raw)

    def test_baselines_strip_all_arbor_order_fields(self):
        validated = c.validate(self.raw)
        for method in ("saguaro", "sharper"):
            selected = c.validate_method_config(validated, method)
            self.assertFalse(set(c.ARBOR_WINDOW_FIELDS) & selected["consensus"].keys())
            self.assertEqual(selected, c.validate(w.baseline_config(self.raw)))
        ahl = c.read(ROOT / "config/ahl_two_layer_locality.json")
        ahl["consensus"].update(self.raw["consensus"])
        self.assertFalse(set(c.ARBOR_WINDOW_FIELDS) & w.baseline_config(ahl)["consensus"].keys())

    def test_only_order_flag_distinguishes_streams(self):
        root_first = copy.deepcopy(self.raw);root_first["consensus"].pop("ancestor_order")
        lhs, rhs = c.validate(root_first), c.validate(self.raw)
        self.assertEqual(w.common_cost_config(lhs), w.common_cost_config(rhs))
        self.assertNotEqual(lhs, rhs)


if __name__ == "__main__":
    unittest.main()
