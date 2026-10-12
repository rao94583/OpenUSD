"""pyside6-uic, PySide6's console script, as a Bazel tool.

CMake runs the pyside6-uic it finds (PYSIDEUICBINARY) to turn usdview's .ui
files into Python (_install_pyside_ui_files in cmake/macros/Private.cmake).
"""

from PySide6.scripts.pyside_tool import uic

uic()
