#!/usr/bin/env python3
"""Independent topology model of lower-first order and witness liveness.

These checks exercise an unbalanced tree and arbitrary coordinator IDs. The
authenticated C++ and live-cluster tests separately verify certificate handling.
"""
import collections
import itertools
import random
import unittest


PARENT = {
    9: None, 5: 9, 7: 5, 3: 9,
    21: 7, 22: 7, 26: 5, 23: 3, 24: 3, 25: 9,
}
LEAVES = (21, 22, 23, 24, 25, 26)
COORDINATORS = tuple(set(PARENT) - set(LEAVES))


def ancestor_path(leaf):
    """Walking from a leaf already yields nearest/deepest ancestor first."""
    result = []
    node = PARENT[leaf]
    while node is not None:
        result.append(node)
        node = PARENT[node]
    return result


def depth(node):
    distance = 0
    while PARENT[node] is not None:
        distance += 1
        node = PARENT[node]
    return distance


def global_order(rounds):
    return sorted(itertools.product(range(1, rounds + 1), COORDINATORS),
                  key=lambda event: (event[0], -depth(event[1]), event[1]))


def leaf_order(leaf, rounds):
    return [(block, co) for block in range(1, rounds + 1)
            for co in ancestor_path(leaf)]


class LowerFirstOrderModel(unittest.TestCase):
    def test_unbalanced_paths_are_projections_of_one_global_order(self):
        order = global_order(12)
        self.assertEqual(ancestor_path(21), [7, 5, 9])
        self.assertEqual(ancestor_path(23), [3, 9])
        self.assertEqual(ancestor_path(25), [9])
        for leaf in LEAVES:
            relevant = set(ancestor_path(leaf))
            self.assertEqual(leaf_order(leaf, 12),
                             [event for event in order if event[1] in relevant])

    def test_shared_coordinators_keep_the_same_order_between_branches(self):
        for left, right in itertools.combinations(LEAVES, 2):
            common = set(ancestor_path(left)) & set(ancestor_path(right))
            self.assertEqual([event for event in leaf_order(left, 12)
                              if event[1] in common],
                             [event for event in leaf_order(right, 12)
                              if event[1] in common])

    def test_witness_participant_waits_cannot_form_an_order_cycle(self):
        rng = random.Random(7105)
        participants = {}
        for event in global_order(12):
            descendants = [leaf for leaf in LEAVES
                           if event[1] in ancestor_path(leaf)]
            participants[event] = set(rng.sample(descendants, 2))
        queues = {leaf: collections.deque(event for event in leaf_order(leaf, 12)
                                         if leaf in participants[event])
                  for leaf in LEAVES}
        remaining = set(participants)
        while remaining:
            ready = [event for event in remaining
                     if all(queues[leaf] and queues[leaf][0] == event
                            for leaf in participants[event])]
            self.assertTrue(ready, "a participant-order witness deadlock exists")
            # Choose a non-global-minimal ready event to model asynchronous
            # progress of disjoint subtrees rather than a central executor.
            event = rng.choice(ready)
            for leaf in participants[event]:
                self.assertEqual(queues[leaf].popleft(), event)
            remaining.remove(event)
        self.assertTrue(all(not queue for queue in queues.values()))

    def test_a_busy_child_cannot_skip_or_starve_root(self):
        order = leaf_order(21, 100)
        root_positions = [position for position, event in enumerate(order)
                          if event[1] == 9]
        self.assertEqual(root_positions, list(range(2, 300, 3)))
        cursor = 2  # Child and middle block 1 have completed; Root block 1 is next.
        arrived = set(order) - {(1, 9)}
        self.assertNotIn(order[cursor], arrived)
        self.assertIn((2, 7), arrived)
        self.assertEqual(order[cursor], (1, 9))


if __name__ == "__main__":
    unittest.main(verbosity=2)
