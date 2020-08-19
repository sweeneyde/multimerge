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
