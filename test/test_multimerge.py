import unittest
import multimerge
from itertools import product, chain
from operator import itemgetter
import random

class TestMerge(unittest.TestCase):
    """mostly copied from test.test_heapq"""

    module = multimerge

    def test_merge_random(self):
        lengths = range(25)
        keys = [None, itemgetter(0), itemgetter(1), itemgetter(1, 0)]
        reverses = [False, True]
        repeats = range(100)
        for n, key, reverse, i, in product(lengths, keys, reverses, repeats):
            inputs = [
                sorted(
                    [(random.choice('ABC'), random.randrange(-500, 500))
                     for _ in range(random.randrange(20))],
                    key=key, reverse=reverse
                )
                for _ in range(n)
            ]
            expected = sorted(chain(*inputs), key=key, reverse=reverse)
            with self.subTest(inputs=inputs, key=key, reverse=reverse):
                m = self.module.merge(*inputs, key=key, reverse=reverse)
                self.assertEqual(list(m), expected)
                self.assertEqual(list(m), [])

    def test_empty_merges(self):
        # Merging two empty lists (with or without a key) should produce
        # another empty list.
        self.assertEqual(list(self.module.merge([], [])), [])
        self.assertEqual(list(self.module.merge([], [], key=lambda: 6)), [])

    def test_merge_does_not_suppress_index_error(self):
        # Issue 19018: Heapq.merge suppresses IndexError from user generator
        def iterable():
            s = list(range(10))
            for i in range(20):
                yield s[i]       # IndexError when i > 10
        with self.assertRaises(IndexError):
            list(self.module.merge(iterable(), iterable()))

    def test_merge_stability(self):
        class Int(int):
            pass
        inputs = [[], [], [], []]
        for i in range(20000):
            stream = random.randrange(4)
            x = random.randrange(500)
            obj = Int(x)
            obj.pair = (x, stream)
            inputs[stream].append(obj)
        for stream in inputs:
            stream.sort()
        result = [i.pair for i in self.module.merge(*inputs)]
        self.assertEqual(result, sorted(result))

    def test_merge_err(self):

        def nexterr_immediate():
            yield from ()
            raise ZeroDivisionError
        def nexterr_delayed():
            yield from range(10)
            raise ZeroDivisionError
        class CmpErr:
            "Dummy element that always raises an error during comparison"
            def __eq__(self, other):
                raise ZeroDivisionError
            __ne__ = __lt__ = __le__ = __gt__ = __ge__ = __eq__

        merge = self.module.merge
        for key in [None, lambda x:x]:
            for reverse in [False, True]:
                # test comparison failure during initialization
                args = [[CmpErr()]] + [range(100)]*10
                mo = merge(*args, key=key, reverse=reverse)
                self.assertRaises(ZeroDivisionError, list, mo)

                for n in range(2,10):
                    # test comparison failure during intermediate steps
                    args = [[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, CmpErr()]]*n
                    mo = merge(*args, key=key, reverse=reverse)
                    self.assertRaises(ZeroDivisionError, list, mo)
                for n in range(1, 10):
                    # test __next__ error during initialization
                    args = [nexterr_immediate() for _ in range(n)]
                    mo = merge(*args, key=key, reverse=reverse)
                    self.assertRaises(ZeroDivisionError, list, mo)
                for n in range(1, 10):
                    # test __next__ error during intermediate steps
                    args = [nexterr_delayed() for _ in range(n)]
                    mo = merge(*args, key=key, reverse=reverse)
                    self.assertRaises(ZeroDivisionError, list, mo)

        for reverse in [False, True]:
            # test error during key computation
            mo = merge(range(10), range(10), key=lambda x: x // 0, reverse=reverse)
            self.assertRaises(ZeroDivisionError, list, mo)
            mo = merge(range(10), key=object(), reverse=reverse)
            self.assertRaises(TypeError, list, merge)

if __name__ == "__main__":
    unittest.main()
