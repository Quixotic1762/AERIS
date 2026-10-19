import sys
from pathlib import Path

# The dashboard modules import each other by plain name (they run as a script).
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "dashboard"))
