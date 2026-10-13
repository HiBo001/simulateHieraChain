"""Weighted ancestor scheduling is explicit and excluded from baselines."""
import copy
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import cluster as c
import benchmark_ordering_windows as w


class RatioConfiguration(unittest.TestCase):
    def setUp(self):
        self.raw = c.read(ROOT / "config/three_layer_lower_10.json")

    def test_ten_requires_lower_first_streaming(self):
        self.assertEqual(c.validate(self.raw)["consensus"]["ancestor_lower_blocks_per_root"], 10)
        for mutation in ({"ancestor_streaming": False}, {"ancestor_order": "root_first"}):
            raw = copy.deepcopy(self.raw);raw["consensus"].update(mutation)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                c.validate(raw)

    def test_invalid_counts_are_rejected(self):
        for count in (True, None, 0, -1, 33, 4294967297, 1.0, "10", [], {}):
            raw = copy.deepcopy(self.raw);raw["consensus"]["ancestor_lower_blocks_per_root"] = count
            with self.subTest(count=count), self.assertRaises(ValueError):
                c.validate(raw)

    def test_old_default_keeps_original_config_bytes_and_order(self):
        raw = c.read(ROOT / "config/three_layer_lower_first.json")
        self.assertNotIn("ancestor_lower_blocks_per_root", c.validate(raw)["consensus"])
        ratio_one = copy.deepcopy(raw);ratio_one["consensus"]["ancestor_lower_blocks_per_root"] = 1
        self.assertEqual(w.common_cost_config(c.validate(raw)), w.common_cost_config(c.validate(ratio_one)))

    def test_baselines_strip_ratio_and_shared_costs_remain_equal(self):
        validated = c.validate(self.raw)
        raw = w.baseline_config(self.raw)
        for method in ("saguaro", "sharper"):
            selected = c.validate_method_config(validated, method)
            self.assertEqual(selected, c.validate(raw))
            self.assertFalse(set(c.ARBOR_WINDOW_FIELDS) & selected["consensus"].keys())
            self.assertEqual(w.common_cost_config(validated), w.common_cost_config(selected))


if __name__ == "__main__":
    unittest.main()
