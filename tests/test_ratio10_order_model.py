#!/usr/bin/env python3
"""Independent model of the weighted ancestor stream's order and liveness.

All non-root coordinators receive ten logical blocks per cycle, with the
deepest coordinator first within each lower block. Root receives one block.
Empty blocks occupy the same positions as business blocks. These finite model
checks complement, rather than replace, authenticated C++ and live tests.
"""
import collections
import itertools
import random
import unittest


PARENT = {
    9: None, 5: 9, 7: 5, 30: 7, 3: 9,
    21: 30, 22: 30, 26: 5, 23: 3, 24: 3, 25: 9,
}
LEAVES = (21, 22, 23, 24, 25, 26)
COORDINATORS = tuple(set(PARENT) - set(LEAVES))
ROOT = 9
QUOTA = 10


def ancestors(leaf):
    result = []
    node = PARENT[leaf]
    while node is not None:
        result.append(node)
        node = PARENT[node]
    return result


def depth(node):
    value = 0
    while PARENT[node] is not None:
        value += 1
        node = PARENT[node]
    return value


def key(event):
    block, co = event
    quota = 1 if co == ROOT else QUOTA
    return ((block - 1) // quota, co == ROOT,
            (block - 1) % quota, -depth(co), co)


def global_order(cycles):
    return sorted(((block, co) for co in COORDINATORS
                   for block in range(1, cycles * (1 if co == ROOT else QUOTA) + 1)),
                  key=key)


def leaf_order(leaf, cycles):
    path = ancestors(leaf)
    result = []
    for cycle in range(cycles):
        for step in range(QUOTA):
            result.extend((cycle * QUOTA + step + 1, co) for co in path if co != ROOT)
        result.append((cycle + 1, ROOT))
    return result


def position(leaf, event):
    """Arithmetic cursor mapping, derived independently from the global key."""
    block, co = event
    lower = [ancestor for ancestor in ancestors(leaf) if ancestor != ROOT]
    width = len(lower) * QUOTA + 1
    if co == ROOT:
        return block * width
    cycle, step = divmod(block - 1, QUOTA)
    return cycle * width + step * len(lower) + lower.index(co) + 1


def at_position(leaf, cursor):
    lower = [ancestor for ancestor in ancestors(leaf) if ancestor != ROOT]
    width = len(lower) * QUOTA + 1
    cycle, offset = divmod(cursor - 1, width)
    if offset == width - 1:
        return cycle + 1, ROOT
    step, index = divmod(offset, len(lower))
    return cycle * QUOTA + step + 1, lower[index]


class Ratio10OrderModel(unittest.TestCase):
    def test_three_layer_exact_ten_to_one(self):
        self.assertEqual(leaf_order(23, 2),
                         [(block, 3) for block in range(1, 11)] + [(1, ROOT)] +
                         [(block, 3) for block in range(11, 21)] + [(2, ROOT)])

    def test_four_and_five_layers_keep_each_lower_quota(self):
        order = leaf_order(21, 3)
        self.assertEqual(order[:7], [(1, 30), (1, 7), (1, 5),
                                    (2, 30), (2, 7), (2, 5), (3, 30)])
        self.assertEqual([event for event in order if event[1] == ROOT],
                         [(1, ROOT), (2, ROOT), (3, ROOT)])
        root_positions = [index for index, event in enumerate(order) if event[1] == ROOT]
        self.assertEqual(root_positions, [30, 61, 92])

    def test_unbalanced_paths_are_projections_of_one_global_order(self):
        order = global_order(12)
        self.assertEqual(ancestors(25), [ROOT])
        for leaf in LEAVES:
            relevant = set(ancestors(leaf))
            self.assertEqual(leaf_order(leaf, 12),
                             [event for event in order if event[1] in relevant])

    def test_shared_ancestor_relative_order_is_identical(self):
        for left, right in itertools.combinations(LEAVES, 2):
            common = set(ancestors(left)) & set(ancestors(right))
            self.assertEqual([event for event in leaf_order(left, 12) if event[1] in common],
                             [event for event in leaf_order(right, 12) if event[1] in common])

    def test_cursor_bijection_including_direct_root_leaf(self):
        for leaf in LEAVES:
            for cursor, event in enumerate(leaf_order(leaf, 100), 1):
                self.assertEqual(position(leaf, event), cursor)
                self.assertEqual(at_position(leaf, cursor), event)
            for event in [(10**6, co) for co in ancestors(leaf)]:
                self.assertEqual(at_position(leaf, position(leaf, event)), event)

    def test_missing_root_cannot_be_skipped_for_next_child(self):
        order = leaf_order(23, 2)
        cursor = 10
        arrived = set(order) - {(1, ROOT)}
        self.assertEqual(order[cursor], (1, ROOT))
        self.assertNotIn(order[cursor], arrived)
        self.assertIn((11, 3), arrived)

    def test_full_32_proof_prefix_can_cross_multiple_cycle_boundaries(self):
        for leaf in LEAVES:
            order = leaf_order(leaf, 40)
            for previous in range(len(order) - 32):
                prefix = order[previous:previous + 32]
                self.assertEqual(position(leaf, prefix[-1]), previous + 32)
                self.assertEqual(at_position(leaf, previous + 33), order[previous + 32])

    def test_per_coordinator_projection_is_monotonic_in_weighted_order(self):
        rng = random.Random(71010)
        for leaf in LEAVES:
            previous = collections.Counter()
            projection = collections.Counter()
            for event in leaf_order(leaf, 50):
                block, co = event
                self.assertEqual(block, previous[co] + 1)
                previous[co] = block
                applicable = rng.random() < 0.6
                projected = projection[co] + int(applicable)
                self.assertEqual(projected, projection[co] + (1 if applicable else 0))
                projection[co] = projected

    def test_asynchronous_participant_witness_waits_are_acyclic(self):
        rng = random.Random(71010)
        participants = {}
        for event in global_order(20):
            descendants = [leaf for leaf in LEAVES if event[1] in ancestors(leaf)]
            # Some unary branches have no two-leaf business transaction;
            # treat their logical slots as empty, as the implementation does.
            participants[event] = (set(rng.sample(descendants, rng.randint(2, len(descendants))))
                                   if len(descendants) >= 2 and rng.random() < 0.65 else set())
        queues = {leaf: collections.deque(event for event in leaf_order(leaf, 20)
                                         if leaf in participants[event]) for leaf in LEAVES}
        remaining = {event for event, leaves in participants.items() if leaves}
        while remaining:
            ready = [event for event in remaining
                     if all(queues[leaf] and queues[leaf][0] == event for leaf in participants[event])]
            self.assertTrue(ready, "weighted ancestor order admits a witness dependency cycle")
            event = rng.choice(ready)
            for leaf in participants[event]:
                self.assertEqual(queues[leaf].popleft(), event)
            remaining.remove(event)
        self.assertTrue(all(not queue for queue in queues.values()))


if __name__ == "__main__":
    unittest.main(verbosity=2)
