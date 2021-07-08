#!/usr/bin/python3
import setuptools
from pybind11.setup_helpers import Pybind11Extension, build_ext

ext_modules = [
    Pybind11Extension(
        "udaq_analysis_lib.fletcher_16",
        sources=[
            'src/fletcher_16.cpp',
        ]
    ),
    Pybind11Extension(
        "udaq_analysis_lib.analyze_hitbuffer",
        sources=[
            'src/analyze_hitbuffer.cpp',
        ]
    ),
]

setuptools.setup(
    name="udaq_analysis_lib",
    version="0.0.1",
    author="Martin Pittermann",
    author_email="martin.pittermann@student.kit.edu",
    description="provides fast C++ functions for analyzing uDAQ data files",
    ext_modules=ext_modules,
    extras_require={"test": "pytest"},
    packages=[
        'udaq_analysis_lib',
    ],
    package_dir={
        'udaq_analysis_lib': 'lib'
    },
    # Currently, build_ext only provides an optional "highest supported C++
    # level" feature, but in the future it may provide more features.
    cmdclass={"build_ext": build_ext},
    zip_safe=False,
)
