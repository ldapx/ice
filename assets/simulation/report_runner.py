#!/usr/bin/env python3
"""Execute a project-level ICE simulation report and emit Markdown on stdout."""

from __future__ import annotations

import argparse
from contextlib import redirect_stdout
import importlib.util
import io
from pathlib import Path
import sys
import traceback


def _import_icelib_sim():
    try:
        import icepy_sim as sim
    except ImportError:
        repository_python = Path(__file__).resolve().parents[2] / "python"
        if repository_python.is_dir():
            sys.path.insert(0, str(repository_python))
        import icepy_sim as sim
    return sim


class ReportContext:
    def __init__(self, project_root: Path):
        self.project_root = project_root
        self._sim = _import_icelib_sim()

    def simulations(self):
        return self._sim.list_simulations(self.project_root)

    def runs(self, simulation=None):
        return self._sim.list_runs(self.project_root, simulation)

    def latest(self, simulation=None):
        return self._sim.load_latest(self.project_root, simulation)

    def latest_many(self, simulations=None):
        return self._sim.load_latest_many(self.project_root, simulations)

    def load(self, path):
        return self._sim.load(path)


def _load_report(path: Path):
    spec = importlib.util.spec_from_file_location("ice_simulation_report", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to load report: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--report", required=True)
    args = parser.parse_args()

    project_root = Path(args.project_root).resolve()
    report_path = Path(args.report).resolve()
    if not project_root.is_dir():
        print(f"Project root does not exist: {project_root}", file=sys.stderr)
        return 2
    if not report_path.is_file():
        print(f"Report file does not exist: {report_path}", file=sys.stderr)
        return 2

    captured = io.StringIO()
    try:
        sys.path.insert(0, str(report_path.parent))
        context = ReportContext(project_root)
        with redirect_stdout(captured):
            module = _load_report(report_path)
            builder = getattr(module, "build_report", None)
            result = builder(context) if callable(builder) else None
        if not callable(builder) or result is None:
            result = captured.getvalue()
        elif captured.getvalue():
            print(captured.getvalue(), file=sys.stderr, end="")
        if result is None:
            result = ""
        sys.stdout.write(str(result))
        if not str(result).endswith("\n"):
            sys.stdout.write("\n")
        return 0
    except Exception:
        if captured.getvalue():
            sys.stderr.write(captured.getvalue())
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
