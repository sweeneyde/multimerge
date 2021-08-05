from setuptools import setup, Extension

with open("README.md", "r") as fh:
    long_description = fh.read()

setup(
    name="multimerge",
    version="0.1",
    description="A faster multi-way merge algorithm interchangeable with heapq.merge",
    long_description=long_description,
    long_description_content_type="text/markdown",
    author="Dennis Sweeney",
    author_email="sweeney.427@osu.edu",
    license="MIT",
    ext_modules=[Extension("multimerge", ["multimergemodule.c"])],
    classifiers=[
        "Programming Language :: Python :: 3",
        "License :: OSI Approved :: MIT License",
        "Operating System :: OS Independent",
    ],
)
