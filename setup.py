from setuptools import setup, Extension

setup(
    name="multimerge",
    version="0.2.0",
    author="Dennis Sweeney",
    author_email="sweeney.427@osu.edu",
    description="A faster multi-way merge algorithm interchangeable with heapq.merge",
    ext_modules=[Extension("multimerge", ["multimergemodule.c"])],
)
