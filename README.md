# Multimerge

Multimerge is a Python package that implements an algorithm for lazily
combining several sorted iterables into one longer sorted iterator. It
is a drop-in replacement for `heapq.merge` in the Python standard
library.

## The API (from `heapq.merge()`)

```Python
def merge(*iterables, key=None, reverse=False):
    '''Merge multiple sorted inputs into a single sorted output.

    Similar to sorted(itertools.chain(*iterables)) but returns a generator,
    does not pull the data into memory all at once, and assumes that each of
    the input streams is already sorted (smallest to largest).

    >>> list(merge([1,3,5,7], [0,2,4,8], [5,10,15,20], [], [25]))
    [0, 1, 2, 3, 4, 5, 5, 7, 8, 10, 15, 20, 25]
    
    If *key* is not None, applies a key function to each element to determine
    its sort order.
    
    >>> list(merge(['dog', 'horse'], ['cat', 'fish', 'kangaroo'], key=len))
    ['dog', 'cat', 'fish', 'horse', 'kangaroo']
    
    '''
    ...
```

## Comparing the Algorithms

### `heapq.merge()`

The standard-library implementation of `merge`, as its location suggests,
involves maintaining a heap (priority queue) data structure. In this
algorithm, each node of the heap stores both a unique item from an
iterator as well as the index of the iterator from which the item came.
This is how sort stability is maintained. At each step of the `merge`,
the root of the heap is yielded, then a new item from that root's source
iterator is found, which then replaces the root with a call to `heapreplace`.

### `multimerge.merge()`

Multimerge uses a different data structure: a linked binary tree known 
as a ["tournament tree of winners"](https://en.wikipedia.org/wiki/K-way_merge_algorithm#cite_ref-knuth98_3-0). It works as follows:

* Each leaf node remembers a particular iterator, as well as the most
  recent item yielded from that iterator.

* Each non-leaf node stores the highest-priority value among all of its
  descendent leaves' values, as well as which leaf that highest-priority
  value came from.

* At a typical step of the process, the root of the tree stored the
  highest-priority value that was just yielded. To replace it, look
  at the root's stored reference to the leaf of the item most recently
  yielded. We will replace this leaf's value with a new value from its
  iterator. Finally, to restore the invariant about descendent values,
  we need to re-evaluate several "games of the tournament"; at each
  ancestor of the leaf, re-evaluate which of its two children should
  have the higher priority, then acquire that child's value and leaf
  references. When this is complete, the value at the root is the
  highest-priority, and can be yielded next.

* When sorting values not by direct comparison but by some key function,
  it is cheaper to only bring the keys up the tree, keeping the values
  stored only at the leaves.

* When a leaf's iterator is exhausted, it can be deleted so that its
  sibling can be promoted, shrinking the data structure as the problem
  reduces and maintaining the invariant that each non-leaf
  node has exactly two children.


## Benefits of `multimerge.merge()`

* There are fewer comparisons required on average, especially in the
  case where the input data has long runs where one particular
  iterator should "win".

* In the heap model, during a heapreplace call, the `(it_index, key, value)`
  tuples are compared lexicographically. This involves identifying
  the first place where two tuples differ, which will require evaluating
  `key1 == key2`, followed then by evaluating `key1 < key2`. This is not
  necessary in `multimerge.merge()`'s tournament tree approach, since
  the order of the input iterators part of the tree structure, so
  sort stability comes naturally.

Both of the above are demonstrated below:

```Python
class Int(int):
    lt = eq = 0
    def __lt__(self, other):
        __class__.lt += 1
        return int.__lt__(self, other)

    def __eq__(self, other):
        __class__.eq += 1
        return int.__eq__(self, other)

def comparisons(mergefunc, iterables):
    Int.lt = Int.eq = 0
    for _ in mergefunc(*iterables):
        pass
    return Int.lt, Int.eq

no_overlap = [
    # (0..999), (1_000..1_999), (2_000..2_999), ...
    list(map(Int, range(x, x+1_000)))
    for x in range(0, 16_000, 1_000)
]

interleaved = [
    # (0,16,32,...), (1,17,33,...), (2,18,34,...), ...
    list(map(Int, range(x, 16_000, 16)))
    for x in range(16)
]

def test_merge_func(mergefunc):
    print("No overlap: {:,} lt; {:,} eq".format(
        *comparisons(mergefunc, no_overlap)))
    print("Interleaved: {:,} lt; {:,} eq".format(
        *comparisons(mergefunc, interleaved)))

if __name__ == "__main__":
    import heapq
    print("======= heapq.merge ======")
    test_merge_func(heapq.merge)
    print()

    import multimerge
    print("==== multimerge.merge ====")
    test_merge_func(multimerge.merge)
    print()

```

Result:
```
======= heapq.merge ======
No overlap: 65,004 lt; 65,004 eq
Interleaved: 64,004 lt; 64,004 eq

==== multimerge.merge ====
No overlap: 32,000 lt; 0 eq
Interleaved: 63,968 lt; 0 eq

```


* These theoretical improvements, coupled with a fast C implementation,
  make multimerge up to 5 times faster than heapq for basic benchmarks:

```
py -m pyperf timeit -s "from random import random; from collections import deque; from heapq import merge;  iters = [sorted(random() for j in range(10_000)) for i in range(20)]" "deque(merge(*iters), maxlen=0)"

    Mean +- std dev: 80.8 ms +- 5.6 ms

py -m pyperf timeit -s "from random import random; from collections import deque; from multimerge import merge;  iters = [sorted(random() for j in range(10_000)) for i in range(20)]" "deque(merge(*iters), maxlen=0)"

    Mean +- std dev: 17.1 ms +- 1.4 ms

```
