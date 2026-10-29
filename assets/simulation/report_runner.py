#!/usr/bin/env python3
"""Execute a project-level ICE simulation script and emit Markdown on stdout."""

from __future__ import annotations

import argparse
from contextlib import redirect_stdout
import importlib.util
import io
import json
import math
from pathlib import Path
import sys
import traceback
from typing import Any, Mapping, Sequence


def _import_icelib_sim():
    try:
        import icepy_sim as sim
    except ImportError:
        repository_python = Path(__file__).resolve().parents[2] / "python"
        if repository_python.is_dir():
            sys.path.insert(0, str(repository_python))
        import icepy_sim as sim
    return sim


def _as_float_list(values: Sequence[Any], label: str) -> list[float]:
    if isinstance(values, (str, bytes)):
        raise TypeError(f"{label} must be a sequence of numbers")
    try:
        iterator = iter(values)
    except TypeError as error:
        raise TypeError(f"{label} must be a sequence of numbers") from error
    result: list[float] = []
    for value in iterator:
        number = float(value)
        if not math.isfinite(number):
            raise ValueError(f"{label} contains a non-finite value")
        result.append(number)
    return result


def _optional_float(value: Any, label: str) -> float | None:
    if value is None:
        return None
    number = float(value)
    if not math.isfinite(number):
        raise ValueError(f"{label} must be finite")
    return number


def _signal_names(value: Any) -> list[str]:
    if isinstance(value, str):
        return value.replace(",", " ").split()
    return [str(item) for item in value]


class SimulationScriptContext:
    """Helpers passed to ``build_script(ctx)``."""

    def __init__(self, project_root: Path):
        self.project_root = project_root
        self._sim = _import_icelib_sim()
        self._graphs: list[dict[str, Any]] = []

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

    @property
    def graphs(self) -> list[dict[str, Any]]:
        return list(self._graphs)

    def graph(
        self,
        name: str,
        *,
        run=None,
        signals: str | Sequence[str] | None = None,
        x: str | Sequence[float] | None = None,
        y: Mapping[str, Sequence[float]] | Sequence[Sequence[float]] | None = None,
        x_axis: str = "time",
        x_min: float | None = None,
        x_max: float | None = None,
        y_min: float | None = None,
        y_max: float | None = None,
    ) -> dict[str, Any]:
        """Add a graph that the GUI can render after the script runs.

        Use ``run=ctx.latest("RLC")`` and ``signals=["in", "n2"]`` to plot
        traces from a stored run. Pass explicit ``x`` and ``y`` arrays to plot
        values computed by the script.
        """
        graph_name = str(name).strip() or f"Graph {len(self._graphs) + 1}"
        x_values: list[float]
        series: list[dict[str, Any]]
        axis = str(x_axis or "time")

        if run is not None:
            simulation_run = run
            if not (
                hasattr(simulation_run, "time") and hasattr(simulation_run, "trace")
            ):
                path = getattr(simulation_run, "path", simulation_run)
                simulation_run = self._sim.load(path)
            if signals is None:
                names = list(simulation_run.trace_names)
            else:
                names = _signal_names(signals)
            if x is None or (
                isinstance(x, str) and x.strip().lower() in {"time", "t", "time(s)"}
            ):
                x_values = _as_float_list(simulation_run.time, "x")
            elif isinstance(x, str):
                if axis == "time":
                    axis = x
                x_values = _as_float_list(simulation_run.trace(x).values, "x")
            else:
                x_values = _as_float_list(x, "x")
            if y is not None:
                raise TypeError(
                    "Pass either run/signals or explicit x/y data, not both"
                )
            series = [
                {
                    "name": signal,
                    "values": _as_float_list(
                        simulation_run.trace(signal).values, f"signal {signal}"
                    ),
                }
                for signal in names
            ]
        else:
            if x is None or y is None:
                raise TypeError("graph() needs run=... or explicit x= and y= data")
            x_values = _as_float_list(x, "x")
            if isinstance(y, Mapping):
                items = y.items()
            elif (
                isinstance(y, Sequence)
                and y
                and all(
                    isinstance(item, Sequence)
                    and not isinstance(item, (str, bytes))
                    and len(item) == 2
                    for item in y
                )
            ):
                items = y
            else:
                items = [("value", y)]
            series = [
                {
                    "name": str(series_name),
                    "values": _as_float_list(values, f"signal {series_name}"),
                }
                for series_name, values in items
            ]

        if not x_values:
            raise ValueError("Graph x data is empty")
        if not series:
            raise ValueError("Graph has no signals")
        for item in series:
            if len(item["values"]) != len(x_values):
                raise ValueError(
                    f"Graph signal {item['name']!r} length does not match x data"
                )

        graph = {
            "name": graph_name,
            "x_axis": axis,
            "x": x_values,
            "series": series,
            "x_min": _optional_float(x_min, "x_min"),
            "x_max": _optional_float(x_max, "x_max"),
            "y_min": _optional_float(y_min, "y_min"),
            "y_max": _optional_float(y_max, "y_max"),
        }
        self._graphs.append(graph)
        return graph

    add_graph = graph


# Backwards-compatible name for older scripts that import the context type.
ReportContext = SimulationScriptContext


def _load_script(path: Path):
    spec = importlib.util.spec_from_file_location("ice_simulation_script", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to load simulation script: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--report", required=True, help="Simulation script source file")
    parser.add_argument("--graph-output", help="Optional path for generated graph JSON")
    args = parser.parse_args()

    project_root = Path(args.project_root).resolve()
    script_path = Path(args.report).resolve()
    if not project_root.is_dir():
        print(f"Project root does not exist: {project_root}", file=sys.stderr)
        return 2
    if not script_path.is_file():
        print(f"Simulation script file does not exist: {script_path}", file=sys.stderr)
        return 2

    captured = io.StringIO()
    try:
        sys.path.insert(0, str(script_path.parent))
        context = SimulationScriptContext(project_root)
        with redirect_stdout(captured):
            module = _load_script(script_path)
            builder = getattr(module, "build_script", None)
            if not callable(builder):
                builder = getattr(module, "build_report", None)
            result = builder(context) if callable(builder) else None
        if not callable(builder) or result is None:
            result = captured.getvalue()
        elif captured.getvalue():
            print(captured.getvalue(), file=sys.stderr, end="")
        if result is None:
            result = ""

        if args.graph_output:
            payload = {
                "format": "ice-simulation-script-graphs",
                "version": 1,
                "graphs": context.graphs,
            }
            graph_path = Path(args.graph_output)
            graph_path.parent.mkdir(parents=True, exist_ok=True)
            graph_path.write_text(
                json.dumps(payload, allow_nan=False), encoding="utf-8"
            )

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
