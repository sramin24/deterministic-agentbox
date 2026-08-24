"""Custom build step: pyproject.toml alone can't run a shell command, and
this package needs one -- `make build` has to run and its two outputs
(libagentbox.so, agentbox-exec) need to land inside src/agentbox/ (the
same place agentbox._core looks for them first, see _find_library()/
_find_exec_binary()) before setuptools' normal package-data collection
scans the source tree, or it'll find nothing to include.
"""
import shutil
import subprocess
from pathlib import Path

from setuptools import setup
from setuptools.command.build_py import build_py as _build_py

REPO_ROOT = Path(__file__).parent.resolve()
PACKAGE_DIR = REPO_ROOT / "src" / "agentbox"


class build_py(_build_py):
    def run(self):
        subprocess.run(["make", "build"], cwd=REPO_ROOT, check=True)
        for name in ("libagentbox.so", "agentbox-exec"):
            shutil.copy2(REPO_ROOT / "out" / name, PACKAGE_DIR / name)
            (PACKAGE_DIR / name).chmod(0o755)
        super().run()


setup(cmdclass={"build_py": build_py})
