"""Gym map compiler: builds custom mapsquares out of OSRS cache content."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cache_pipeline"))
