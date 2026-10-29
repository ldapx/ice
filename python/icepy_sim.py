"""Load ICE simulation runs and compute reusable waveform metrics.

This module is intentionally pure Python. It reads the JSON files written under
``<project>/build/simulations`` and does not require the optional nanobind
``icepy`` extension.
"""

from __future__ import annotations

from bisect import bisect_left, bisect_right
from dataclasses import asdict, dataclass, field, replace
import json
import math
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


class SimulationDataError(ValueError):
    """Raised when a stored run cannot be interpreted."""


class TraceNotFoundError(SimulationDataError):
    """Raised when a requested signal does not exist in a run."""


def _normalize_signal(name: str) -> str:
    value = "".join(str(name).split()).lower()
    if value.startswith("v(") and value.endswith(")"):
        value = value[2:-1]
    return value


def _unit_for(name: str) -> str:
    normalized = _normalize_signal(name)
    if str(name).strip().lower().startswith("v("):
        return "V"
    if str(name).strip().lower().startswith("i("):
        return "A"
    if normalized in {"time", "time(s)", "t"}:
        return "s"
    return ""


def _as_float_list(values: Any, label: str) -> list[float]:
    if not isinstance(values, list):
        raise SimulationDataError(f"{label} must be a JSON array")
    result: list[float] = []
    for value in values:
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise SimulationDataError(f"{label} contains a non-numeric value")
        number = float(value)
        if not math.isfinite(number):
            raise SimulationDataError(f"{label} contains a non-finite value")
        result.append(number)
    return result


def _mean(values: Sequence[float]) -> float:
    return sum(values) / len(values)


def _crossing(
    time: Sequence[float],
    values: Sequence[float],
    threshold: float,
    direction: str,
    start: int = 0,
) -> float | None:
    """Return the first threshold crossing at or after ``start``."""
    if len(time) < 2:
        return None
    for index in range(max(1, start + 1), len(time)):
        previous = values[index - 1] - threshold
        current = values[index] - threshold
        if direction == "rising":
            crossed = previous <= 0.0 < current
        elif direction == "falling":
            crossed = previous >= 0.0 > current
        else:
            raise ValueError("direction must be 'rising' or 'falling'")
        if not crossed:
            continue
        span = values[index] - values[index - 1]
        if span == 0.0:
            return float(time[index])
        fraction = (threshold - values[index - 1]) / span
        return float(time[index - 1] + fraction * (time[index] - time[index - 1]))
    return None


def _zero_crossings(time: Sequence[float], values: Sequence[float]) -> list[float]:
    crossings: list[float] = []
    for index in range(1, len(time)):
        left, right = values[index - 1], values[index]
        if left == 0.0:
            if index > 1:
                crossings.append(float(time[index - 1]))
            continue
        if (left < 0.0 < right) or (left > 0.0 > right):
            fraction = -left / (right - left)
            crossings.append(
                float(time[index - 1] + fraction * (time[index] - time[index - 1]))
            )
    return crossings


def _local_extrema(
    time: Sequence[float], values: Sequence[float], maxima: bool
) -> list[tuple[float, float]]:
    result: list[tuple[float, float]] = []
    for index in range(1, len(values) - 1):
        if maxima:
            if values[index] >= values[index - 1] and values[index] > values[index + 1]:
                result.append((float(time[index]), float(values[index])))
        elif values[index] <= values[index - 1] and values[index] < values[index + 1]:
            result.append((float(time[index]), float(values[index])))
    return result


@dataclass(frozen=True)
class MetricOptions:
    low: float = 0.10
    high: float = 0.90
    steady_fraction: float = 0.10
    settling_tolerance: float = 0.02
    minimum_peaks: int = 2


@dataclass
class TraceMetrics:
    minimum: float | None
    maximum: float | None
    mean: float | None
    rms: float | None
    peak_to_peak: float | None
    initial: float | None
    final: float | None
    absolute_maximum: float | None
    integral: float | None
    absolute_integral: float | None
    steady_state: float | None = None
    overshoot_percent: float | None = None
    rise_time: float | None = None
    fall_time: float | None = None
    peak_time: float | None = None
    settling_time: float | None = None
    delay: float | None = None
    ringing_frequency: float | None = None
    ringing_period: float | None = None
    damping_ratio: float | None = None
    quality_factor: float | None = None
    zero_crossings: list[float] = field(default_factory=list)

    def as_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class RunInfo:
    path: Path
    run_id: str
    created_at: str
    state: str
    analysis: str
    simulation_name: str
    simulation_id: int
    schematic_name: str
    schematic_id: int
    sample_count: int
    trace_names: tuple[str, ...]


@dataclass(frozen=True)
class SimulationInfo:
    id: int
    name: str
    run_count: int
    latest_run: RunInfo | None


class Trace:
    def __init__(self, name: str, values: Sequence[float], time: Sequence[float]):
        self.name = name
        self.values = list(values)
        self.time = list(time)
        if len(self.values) != len(self.time):
            raise SimulationDataError(
                f"Trace {name!r} has a different length than the time vector"
            )
        self.unit = _unit_for(name)

    @property
    def size(self) -> int:
        return len(self.values)

    def __repr__(self) -> str:
        return f"Trace(name={self.name!r}, size={self.size}, unit={self.unit!r})"

    def at(self, time: float) -> float:
        if not self.time:
            raise SimulationDataError("Cannot interpolate an empty trace")
        if time <= self.time[0]:
            return self.values[0]
        if time >= self.time[-1]:
            return self.values[-1]
        index = bisect_left(self.time, time)
        if self.time[index] == time:
            return self.values[index]
        span = self.time[index] - self.time[index - 1]
        if span == 0:
            return self.values[index]
        fraction = (time - self.time[index - 1]) / span
        return self.values[index - 1] + fraction * (
            self.values[index] - self.values[index - 1]
        )

    def between(self, start: float, end: float) -> "Trace":
        left = bisect_left(self.time, start)
        right = bisect_right(self.time, end)
        return Trace(self.name, self.values[left:right], self.time[left:right])

    def normalized(self) -> "Trace":
        if not self.values:
            return Trace(self.name, [], [])
        minimum = min(self.values)
        maximum = max(self.values)
        span = maximum - minimum
        if span == 0.0:
            return Trace(self.name, [0.0 for _ in self.values], self.time)
        return Trace(
            self.name, [(value - minimum) / span for value in self.values], self.time
        )

    def to_numpy(self):
        try:
            import numpy as np
        except ImportError as error:
            raise RuntimeError("NumPy is not installed") from error
        return np.asarray(self.values, dtype=float)

    def crossing_time(self, level: float, direction: str = "rising") -> float | None:
        return _crossing(self.time, self.values, level, direction)

    def metrics(self, options: MetricOptions | None = None, **overrides) -> TraceMetrics:
        if options is None:
            options = MetricOptions(**overrides)
        elif overrides:
            options = replace(options, **overrides)
        if not self.values:
            return TraceMetrics(
                minimum=None,
                maximum=None,
                mean=None,
                rms=None,
                peak_to_peak=None,
                initial=None,
                final=None,
                absolute_maximum=None,
                integral=None,
                absolute_integral=None,
            )

        values = self.values
        time = self.time
        minimum = min(values)
        maximum = max(values)
        mean = _mean(values)
        rms = math.sqrt(_mean([value * value for value in values]))
        integral = 0.0
        absolute_integral = 0.0
        for index in range(1, len(values)):
            dt = time[index] - time[index - 1]
            integral += 0.5 * (values[index - 1] + values[index]) * dt
            absolute_integral += (
                0.5 * (abs(values[index - 1]) + abs(values[index])) * dt
            )

        fraction = max(0.0, min(1.0, options.steady_fraction))
        if time[-1] > time[0] and fraction > 0.0:
            cutoff = time[-1] - fraction * (time[-1] - time[0])
            steady_values = [
                value for timestamp, value in zip(time, values) if timestamp >= cutoff
            ]
        else:
            steady_values = [values[-1]]
        if not steady_values:
            steady_values = [values[-1]]
        steady = _mean(steady_values)
        initial = values[0]
        amplitude = steady - initial

        overshoot = None
        rise_time = None
        fall_time = None
        peak_time = None
        settling_time = None
        delay = None
        ringing_frequency = None
        ringing_period = None
        damping_ratio = None
        quality_factor = None
        zero_crossings = _zero_crossings(time, values)

        if abs(amplitude) > 1e-15:
            rising = amplitude > 0.0
            low = initial + options.low * amplitude
            high = initial + options.high * amplitude
            low_time = _crossing(time, values, low, "rising" if rising else "falling")
            high_time = _crossing(time, values, high, "rising" if rising else "falling")
            if low_time is not None and high_time is not None and high_time >= low_time:
                if rising:
                    rise_time = high_time - low_time
                else:
                    fall_time = high_time - low_time
            delay = _crossing(
                time,
                values,
                initial + 0.5 * amplitude,
                "rising" if rising else "falling",
            )

            extremum = maximum if rising else minimum
            peak_time = time[values.index(extremum)]
            overshoot = max(0.0, 100.0 * (extremum - steady) / amplitude)

            tolerance = abs(options.settling_tolerance * amplitude)
            outside = [
                index
                for index, value in enumerate(values)
                if abs(value - steady) > tolerance
            ]
            if outside:
                last = outside[-1]
                if last + 1 < len(time):
                    band = steady + (tolerance if values[last] > steady else -tolerance)
                    next_value = values[last + 1]
                    if next_value != values[last]:
                        fraction = (band - values[last]) / (next_value - values[last])
                        settling_time = float(
                            time[last]
                            + max(0.0, min(1.0, fraction))
                            * (time[last + 1] - time[last])
                        )
                    else:
                        settling_time = float(time[last + 1])
                else:
                    settling_time = float(time[last])
            else:
                settling_time = float(time[0])

            peaks = _local_extrema(time, values, maxima=rising)
            if not peaks and not rising:
                peaks = _local_extrema(time, values, maxima=False)
            if len(peaks) >= options.minimum_peaks:
                deviations = [abs(value - steady) for _, value in peaks]
                times = [timestamp for timestamp, _ in peaks]
                periods = [
                    times[index] - times[index - 1] for index in range(1, len(times))
                ]
                periods = [period for period in periods if period > 0.0]
                if periods:
                    ringing_period = _mean(periods)
                    if ringing_period > 0.0:
                        ringing_frequency = 1.0 / ringing_period

                ratios = []
                for index in range(len(deviations) - 1):
                    if deviations[index] > deviations[index + 1] > 0.0:
                        ratios.append(
                            math.log(deviations[index] / deviations[index + 1])
                        )
                if ratios:
                    delta = _mean(ratios)
                    damping_ratio = delta / math.sqrt(
                        4.0 * math.pi * math.pi + delta * delta
                    )
                    if damping_ratio > 0.0:
                        quality_factor = 1.0 / (2.0 * damping_ratio)

        return TraceMetrics(
            minimum=minimum,
            maximum=maximum,
            mean=mean,
            rms=rms,
            peak_to_peak=maximum - minimum,
            initial=initial,
            final=values[-1],
            absolute_maximum=max(abs(value) for value in values),
            integral=integral,
            absolute_integral=absolute_integral,
            steady_state=steady,
            overshoot_percent=overshoot,
            rise_time=rise_time,
            fall_time=fall_time,
            peak_time=peak_time,
            settling_time=settling_time,
            delay=delay,
            ringing_frequency=ringing_frequency,
            ringing_period=ringing_period,
            damping_ratio=damping_ratio,
            quality_factor=quality_factor,
            zero_crossings=zero_crossings,
        )


class _AliasDict(dict):
    """Dictionary with signal-name alias lookup (``n1`` / ``v(n1)``)."""

    def _resolve(self, key):
        if super().__contains__(key):
            return key
        normalized = _normalize_signal(key)
        for name in self.keys():
            if _normalize_signal(name) == normalized:
                return name
        raise KeyError(key)

    def __getitem__(self, key):
        return super().__getitem__(self._resolve(key))

    def __contains__(self, key):
        try:
            self._resolve(key)
        except KeyError:
            return False
        return True

    def get(self, key, default=None):
        try:
            return self[key]
        except KeyError:
            return default


class SimulationRun:
    def __init__(self, document: Mapping[str, Any], path: Path | None = None):
        if not isinstance(document, Mapping):
            raise SimulationDataError("Simulation result must be a JSON object")
        if document.get("format") != "ice-simulation-result":
            raise SimulationDataError("Not an ICE stored simulation result")
        run = document.get("run")
        if not isinstance(run, dict):
            raise SimulationDataError("Stored result is missing the run object")
        if "time" not in document or "traces" not in document:
            raise SimulationDataError("Stored result is missing time or traces")
        if not isinstance(document["traces"], list):
            raise SimulationDataError("traces must be a JSON array")
        self.document = dict(document)
        self.path = Path(path) if path is not None else None
        self.project_root = (
            self.path.parents[4]
            if self.path is not None and len(self.path.parents) >= 5
            else None
        )
        self.metadata = run
        self.run_id = str(run.get("id", ""))
        self.created_at = str(run.get("created_at", ""))
        self.state = str(run.get("state", ""))
        self.analysis = str(document.get("analysis", run.get("analysis", "")))
        self.simulator = str(document.get("simulator", run.get("simulator", "")))
        self.simulator_version = str(
            document.get("simulator_version", run.get("simulator_version", ""))
        )
        project = run.get("project", {})
        self.project_name = (
            str(project.get("name", "")) if isinstance(project, dict) else ""
        )
        simulation = run.get("simulation", {})
        self.simulation_id = (
            int(simulation.get("id", 0)) if isinstance(simulation, dict) else 0
        )
        self.simulation_name = (
            str(simulation.get("name", "")) if isinstance(simulation, dict) else ""
        )
        schematic = run.get("schematic", {})
        self.schematic_id = (
            int(schematic.get("id", 0)) if isinstance(schematic, dict) else 0
        )
        self.schematic_name = (
            str(schematic.get("name", "")) if isinstance(schematic, dict) else ""
        )
        self.time = _as_float_list(document["time"], "time")
        if any(
            self.time[index] < self.time[index - 1]
            for index in range(1, len(self.time))
        ):
            raise SimulationDataError("Simulation time must be non-decreasing")
        self.traces: dict[str, Trace] = _AliasDict()
        self.trace_names: list[str] = []
        for entry in document["traces"]:
            if not isinstance(entry, dict):
                raise SimulationDataError("A trace entry is not an object")
            name = entry.get("name")
            if not isinstance(name, str) or not name:
                raise SimulationDataError("A trace has no name")
            values = _as_float_list(entry.get("values", []), f"trace {name}")
            if len(values) != len(self.time):
                raise SimulationDataError(
                    f"Trace {name!r} length does not match the time vector"
                )
            trace = Trace(name, values, self.time)
            self.traces[_normalize_signal(name)] = trace
            self.trace_names.append(name)
        self.graphs = document.get("graphs", [])

    def __repr__(self) -> str:
        return (
            f"SimulationRun(simulation={self.simulation_name!r}, run_id={self.run_id!r}, "
            f"state={self.state!r}, samples={len(self.time)})"
        )

    @property
    def sample_count(self) -> int:
        return len(self.time)

    def has_signal(self, name: str) -> bool:
        return _normalize_signal(name) in self.traces

    def trace(self, name: str) -> Trace:
        key = _normalize_signal(name)
        if key not in self.traces:
            raise TraceNotFoundError(f"Trace not found: {name}")
        return self.traces[key]

    def metrics(self, options: MetricOptions | None = None, **overrides) -> "RunMetrics":
        metrics = _AliasDict()
        for trace in self.traces.values():
            metrics[trace.name] = trace.metrics(options, **overrides)
        return RunMetrics(metrics, self)

    def to_dict(self) -> dict[str, Any]:
        return dict(self.document)

    def to_numpy(self):
        try:
            import numpy as np
        except ImportError as error:
            raise RuntimeError("NumPy is not installed") from error
        result = {"time": np.asarray(self.time, dtype=float)}
        for trace in self.traces.values():
            result[trace.name] = np.asarray(trace.values, dtype=float)
        return result

    def to_pandas(self):
        try:
            import pandas as pd
        except ImportError as error:
            raise RuntimeError("pandas is not installed") from error
        data = {"time": self.time}
        for trace in self.traces.values():
            data[trace.name] = trace.values
        return pd.DataFrame(data)


@dataclass
class RunMetrics:
    traces: dict[str, TraceMetrics]
    run: "SimulationRun | None" = None

    def summary(self, run: SimulationRun | None = None) -> dict[str, Any]:
        result: dict[str, Any] = {
            "traces": {name: metrics.as_dict() for name, metrics in self.traces.items()}
        }
        run = run or self.run
        if run is not None:
            result.update(
                {
                    "state": run.state,
                    "analysis": run.analysis,
                    "samples": run.sample_count,
                    "simulation": run.simulation_name,
                    "run_id": run.run_id,
                }
            )
        return result


def load(path: str | Path) -> SimulationRun:
    result_path = Path(path)
    try:
        document = json.loads(result_path.read_text())
    except FileNotFoundError as error:
        raise SimulationDataError(
            f"Simulation result not found: {result_path}"
        ) from error
    except json.JSONDecodeError as error:
        raise SimulationDataError(
            f"Invalid simulation result JSON: {result_path}: {error}"
        ) from error
    return SimulationRun(document, result_path)


def _run_paths(project_root: str | Path) -> list[Path]:
    root = Path(project_root)
    base = root / "build" / "simulations"
    if not base.is_dir():
        return []
    return sorted(path for path in base.glob("*/runs/*.json") if path.is_file())


def _run_info(path: Path) -> RunInfo:
    """Read run metadata without constructing Trace objects."""
    try:
        document = json.loads(path.read_text())
    except FileNotFoundError as error:
        raise SimulationDataError(f"Simulation result not found: {path}") from error
    except json.JSONDecodeError as error:
        raise SimulationDataError(
            f"Invalid simulation result JSON: {path}: {error}"
        ) from error
    if (
        not isinstance(document, dict)
        or document.get("format") != "ice-simulation-result"
    ):
        raise SimulationDataError("Not an ICE stored simulation result")
    run = document.get("run")
    if not isinstance(run, dict):
        raise SimulationDataError("Stored result is missing the run object")
    if "time" not in document or "traces" not in document:
        raise SimulationDataError("Stored result is missing time or traces")
    time = _as_float_list(document["time"], "time")
    if any(time[index] < time[index - 1] for index in range(1, len(time))):
        raise SimulationDataError("Simulation time must be non-decreasing")
    traces = document["traces"]
    if not isinstance(traces, list):
        raise SimulationDataError("traces must be a JSON array")
    trace_names: list[str] = []
    for entry in traces:
        if not isinstance(entry, dict):
            raise SimulationDataError("A trace entry is not an object")
        name = entry.get("name")
        if not isinstance(name, str) or not name:
            raise SimulationDataError("A trace has no name")
        values = entry.get("values")
        if not isinstance(values, list) or len(values) != len(time):
            raise SimulationDataError(
                f"Trace {name!r} length does not match the time vector"
            )
        trace_names.append(name)
    simulation = run.get("simulation", {})
    schematic = run.get("schematic", {})
    return RunInfo(
        path=path,
        run_id=str(run.get("id", "")),
        created_at=str(run.get("created_at", "")),
        state=str(run.get("state", "")),
        analysis=str(document.get("analysis", run.get("analysis", ""))),
        simulation_name=str(simulation.get("name", ""))
        if isinstance(simulation, dict)
        else "",
        simulation_id=int(simulation.get("id", 0))
        if isinstance(simulation, dict)
        else 0,
        schematic_name=str(schematic.get("name", ""))
        if isinstance(schematic, dict)
        else "",
        schematic_id=int(schematic.get("id", 0)) if isinstance(schematic, dict) else 0,
        sample_count=len(time),
        trace_names=tuple(trace_names),
    )


def list_runs(
    project_root: str | Path, simulation: str | int | None = None
) -> list[RunInfo]:
    infos = [_run_info(path) for path in _run_paths(project_root)]
    if simulation is not None:
        key = str(simulation).lower()
        infos = [
            info
            for info in infos
            if info.simulation_name.lower() == key or str(info.simulation_id) == key
        ]
    return sorted(infos, key=lambda info: (info.created_at, info.run_id))


def load_latest(
    project_root: str | Path, simulation: str | int | None = None
) -> SimulationRun | None:
    runs = list_runs(project_root, simulation)
    return load(runs[-1].path) if runs else None


def list_simulations(project_root: str | Path) -> list[SimulationInfo]:
    grouped: dict[tuple[int, str], list[RunInfo]] = {}
    for path in _run_paths(project_root):
        info = _run_info(path)
        grouped.setdefault((info.simulation_id, info.simulation_name), []).append(info)
    result: list[SimulationInfo] = []
    for (simulation_id, name), runs in grouped.items():
        runs.sort(key=lambda info: (info.created_at, info.run_id))
        result.append(SimulationInfo(simulation_id, name, len(runs), runs[-1]))
    return sorted(result, key=lambda item: (item.name.lower(), item.id))


def load_latest_many(
    project_root: str | Path,
    simulations: Iterable[str | int] | None = None,
) -> dict[str, SimulationRun]:
    names: list[str | int]
    if simulations is None:
        names = [item.name for item in list_simulations(project_root)]
    else:
        names = list(simulations)
    result: dict[str, SimulationRun] = {}
    for name in names:
        run = load_latest(project_root, name)
        if run is not None:
            result[run.simulation_name] = run
    return result


__all__ = [
    "MetricOptions",
    "RunInfo",
    "RunMetrics",
    "SimulationDataError",
    "SimulationInfo",
    "SimulationRun",
    "Trace",
    "TraceMetrics",
    "TraceNotFoundError",
    "list_runs",
    "list_simulations",
    "load",
    "load_latest",
    "load_latest_many",
]
