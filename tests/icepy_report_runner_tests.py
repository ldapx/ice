"""Headless smoke test for the project-level simulation report runner."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


REPOSITORY = Path(__file__).resolve().parents[1]
RUNNER = REPOSITORY / "assets" / "simulation" / "report_runner.py"


def write_run(
    root: Path,
    simulation_id: int,
    name: str,
    run_id: str,
    created_at: str,
    values: list[float],
) -> None:
    directory = root / "build" / "simulations" / f"{simulation_id}-{name}" / "runs"
    directory.mkdir(parents=True, exist_ok=True)
    document = {
        "format": "ice-simulation-result",
        "schema_version": 1,
        "run": {
            "id": run_id,
            "created_at": created_at,
            "project": {"identity": "test", "name": "test"},
            "simulation": {"id": simulation_id, "name": name},
            "schematic": {"id": simulation_id, "name": "top"},
            "request": {
                "analysis": "transient",
                "stop": 1.0,
                "step": 0.1,
                "use_initial_conditions": False,
                "capture_signals": ["v(n1)"],
                "directives": "",
                "netlist": [],
                "netlist_notes": [],
            },
            "simulator": "ngspice",
            "simulator_version": "test",
            "input_fingerprint": "test",
            "state": "succeeded",
            "error": "",
        },
        "analysis": "transient",
        "time": [index / 10.0 for index in range(11)],
        "traces": [{"name": "v(n1)", "values": values}],
        "graphs": [],
    }
    (directory / f"{run_id}.json").write_text(json.dumps(document))


def run_report(project_root: Path, report: Path) -> subprocess.CompletedProcess[str]:
    environment = dict(os.environ)
    environment["PYTHONPATH"] = str(REPOSITORY / "python")
    return subprocess.run(
        [
            sys.executable,
            str(RUNNER),
            "--project-root",
            str(project_root),
            "--report",
            str(report),
        ],
        check=False,
        capture_output=True,
        text=True,
        env=environment,
    )


def test_report_builder_can_compare_multiple_simulations() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        write_run(
            root,
            1,
            "alpha",
            "20260101T000000.000Z-a",
            "2026-01-01T00:00:00.000Z",
            [0.0, 0.4, 0.8, 1.1, 1.0, 0.95, 1.0, 1.0, 1.0, 1.0, 1.0],
        )
        write_run(
            root,
            2,
            "beta",
            "20260101T000001.000Z-b",
            "2026-01-01T00:00:01.000Z",
            [0.0, 0.7, 1.2, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0],
        )
        report = root / "compare.py"
        report.write_text(
            "def build_report(ctx):\n"
            "    print('computing comparison')\n"
            "    rows = []\n"
            "    for name, run in sorted(ctx.latest_many().items()):\n"
            "        metrics = run.trace('v(n1)').metrics()\n"
            "        rows.append(f'{name}: {run.state} {run.trace(\"v(n1)\").size} '\n"
            "                    f'{metrics.settling_time:.3f}')\n"
            "    return '\\n'.join(rows)\n"
        )
        result = run_report(root, report)
        assert result.returncode == 0, result.stderr
        assert "computing comparison" in result.stderr
        assert "alpha: succeeded 11 0.560" in result.stdout
        assert "beta: succeeded 11 0.290" in result.stdout


def test_module_without_builder_uses_stdout() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        report = root / "script.py"
        report.write_text("print('# Script report')\n")
        result = run_report(root, report)
        assert result.returncode == 0, result.stderr
        assert "# Script report" in result.stdout


if __name__ == "__main__":
    test_report_builder_can_compare_multiple_simulations()
    test_module_without_builder_uses_stdout()
    print("icepy report runner tests passed")
