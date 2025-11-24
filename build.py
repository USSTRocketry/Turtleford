#!/usr/bin/env python3
import os
import subprocess
import sys

if __name__ == "__main__":
    # Use absolute or normalized path to avoid confusion
    target_script = os.path.normpath("Lib/Bricks/build.py")

    cmd = [sys.executable, target_script] + sys.argv[1:]
    result = subprocess.run(cmd)
    sys.exit(result.returncode)
