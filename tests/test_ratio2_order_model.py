#!/usr/bin/env python3
"""Finite checks of 2:1 ordering, independent of the C++ cursor formulas.

The shared mathematical model defines a single global order on an unbalanced
tree. These checks compare its leaf projections with explicit expected order,
exercise restart positions, and establish a finite witness-dependency graph.
They supplement authenticated C++ and live transaction checks.
"""
import collections
import itertools
import random
import unittest

import test_ratio10_order_model as model


UINT64_MAX = 2**64 - 1
INT_MAX = 2**31 - 1


class Ratio2OrderModel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.previous_quota = model.QUOTA
        model.QUOTA = 2

    @classmethod
    def tearDownClass(cls):
        model.QUOTA = cls.previous_quota

    def test_three_layer_child_child_root_sequence(self):
        self.assertEqual(model.leaf_order(23, 3), [
            (1, 3), (2, 3), (1, model.ROOT),
            (3, 3), (4, 3), (2, model.ROOT),
            (5, 3), (6, 3), (3, model.ROOT),
        ])

    def test_four_layer_each_lower_ancestor_gets_two_blocks(self):
        # The model's leaf 21 has three non-root ancestors. Removing its
        # deepest ancestor projects the desired four-layer C,M,Root path.
        order = [event for event in model.leaf_order(21, 2) if event[1] != 30]
        self.assertEqual(order, [
            (1, 7), (1, 5), (2, 7), (2, 5), (1, model.ROOT),
            (3, 7), (3, 5), (4, 7), (4, 5), (2, model.ROOT),
        ])

    def test_unbalanced_leaf_paths_project_one_global_order(self):
        order = model.global_order(16)
        for leaf in model.LEAVES:
            own_ancestors = set(model.ancestors(leaf))
            self.assertEqual(model.leaf_order(leaf, 16),
                             [event for event in order if event[1] in own_ancestors])
        for left, right in itertools.combinations(model.LEAVES, 2):
            common = set(model.ancestors(left)) & set(model.ancestors(right))
            self.assertEqual([event for event in model.leaf_order(left, 16) if event[1] in common],
                             [event for event in model.leaf_order(right, 16) if event[1] in common])

    def test_cursor_bijection_and_restart_at_every_position(self):
        for leaf in model.LEAVES:
            order = model.leaf_order(leaf, 80)
            for cursor, event in enumerate(order, 1):
                self.assertEqual(model.position(leaf, event), cursor)
                self.assertEqual(model.at_position(leaf, cursor), event)
                if cursor < len(order):
                    self.assertEqual(model.at_position(leaf, cursor + 1), order[cursor])

    def test_missing_required_root_or_child_cannot_be_skipped(self):
        order = model.leaf_order(23, 2)
        for missing in [(2, 3), (1, model.ROOT)]:
            cursor = order.index(missing)
            arrived = set(order) - {missing}
            self.assertNotIn(order[cursor], arrived)
            self.assertIn(order[cursor + 1], arrived)
            self.assertEqual(model.at_position(23, cursor + 1), missing)

    def test_32_proof_prefix_restarts_across_multiple_cycles(self):
        for leaf in model.LEAVES:
            order = model.leaf_order(leaf, 80)
            for previous in range(len(order) - 32):
                prefix = order[previous:previous + 32]
                restored = model.position(leaf, prefix[-1])
                self.assertEqual(restored, previous + 32)
                self.assertEqual(model.at_position(leaf, restored + 1), order[previous + 32])

    def test_root_only_path_does_not_depend_on_lower_blocks(self):
        self.assertEqual(model.ancestors(25), [model.ROOT])
        self.assertEqual(model.leaf_order(25, 6), [(block, model.ROOT) for block in range(1, 7)])
        for block in (1, INT_MAX, INT_MAX + 1, UINT64_MAX):
            self.assertEqual(model.position(25, (block, model.ROOT)), block)
            self.assertEqual(model.at_position(25, block), (block, model.ROOT))

    def test_integer_boundary_requires_native_block_limit(self):
        # Cursor arithmetic can represent rounds beyond native int; native
        # proof validation must still reject a block above INT_MAX.
        for leaf in model.LEAVES:
            for co in model.ancestors(leaf):
                for block in (INT_MAX - 1, INT_MAX, INT_MAX + 1):
                    event = (block, co)
                    cursor = model.position(leaf, event)
                    self.assertLessEqual(cursor, UINT64_MAX)
                    self.assertEqual(model.at_position(leaf, cursor), event)
            # At the uint64 edge, inverse coordinates remain representable,
            # but adding one to the durable cursor would overflow.
            event = model.at_position(leaf, UINT64_MAX)
            self.assertEqual(model.position(leaf, event), UINT64_MAX)
            self.assertGreater(model.position(leaf, event) + 1, UINT64_MAX)

    def test_ratios_one_and_ten_keep_their_previous_order(self):
        try:
            for quota in (1, 10):
                model.QUOTA = quota
                expected = ([(block, 3) for block in range(1, quota + 1)] +
                            [(1, model.ROOT)] +
                            [(block, 3) for block in range(quota + 1, 2 * quota + 1)] +
                            [(2, model.ROOT)])
                self.assertEqual(model.leaf_order(23, 2), expected)
                for cursor, event in enumerate(expected, 1):
                    self.assertEqual(model.position(23, event), cursor)
                    self.assertEqual(model.at_position(23, cursor), event)
        finally:
            model.QUOTA = 2

    def test_witness_dependency_graph_is_acyclic(self):
        rng = random.Random(71002)
        participants = {}
        for event in model.global_order(24):
            descendants = [leaf for leaf in model.LEAVES if event[1] in model.ancestors(leaf)]
            participants[event] = (set(rng.sample(descendants, rng.randint(2, len(descendants))))
                                   if len(descendants) >= 2 and rng.random() < 0.65 else set())
        queues = {leaf: collections.deque(event for event in model.leaf_order(leaf, 24)
                                         if leaf in participants[event]) for leaf in model.LEAVES}
        remaining = {event for event, leaves in participants.items() if leaves}
        while remaining:
            ready = [event for event in remaining
                     if all(queues[leaf] and queues[leaf][0] == event for leaf in participants[event])]
            self.assertTrue(ready, "2:1 ancestor order admits a witness dependency cycle")
            event = rng.choice(ready)
            for leaf in participants[event]:
                self.assertEqual(queues[leaf].popleft(), event)
            remaining.remove(event)
        self.assertTrue(all(not queue for queue in queues.values()))


if __name__ == "__main__":
    unittest.main(verbosity=2)
