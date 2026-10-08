"""Compatibility entry point for the Bluepad adapter regression suite."""
from pathlib import Path
import runpy
runpy.run_path(str(Path(__file__).with_name('bluepad32_host_test.py')),run_name='__main__')
