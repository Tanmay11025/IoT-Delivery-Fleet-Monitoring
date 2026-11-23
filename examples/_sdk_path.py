"""Let the examples run from a fresh clone without `pip install -e python_sdk`."""

import os
import sys

try:
    import gateway_client  # noqa: F401
except ImportError:
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python_sdk"))
