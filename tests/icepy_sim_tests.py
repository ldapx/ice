"""Tests for the pure-Python simulation result and metrics API."""

from __future__ import annotations

import json
import math
from pathlib import Path
import tempfile

import icepy_sim as sim


def write_run(
    root: Path,
    simulation_id: int,
    simulation_name: str,
    run_id: str,
    created_at: str,
    time: list[float],
    values: list[float],
    trace_name: str = "v(out)",
) -> Path:
    directory = (
        root / "build" / "simulations" / f"{simulation_id}-{simulation_name}" / "runs"
    )
    directory.mkdir(parents=True, exist_ok=True)
    document = {
        "format": "ice-simulation-result",
        "schema_version": 1,
        "run": {
            "id": run_id,
            "created_at": created_at,
            "project": {"identity": "test", "name": "test"},
            "simulation": {"id": simulation_id, "name": simulation_name},
            "schematic": {"id": simulation_id, "name": "top"},
            "request": {
                "analysis": "transient",
                "stop": time[-1],
                "step": time[1] - time[0],
                "use_initial_conditions": False,
                "capture_signals": [trace_name],
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
        "time": time,
        "traces": [{"name": trace_name, "values": values}],
        "graphs": [],
    }
    path = directory / f"{run_id}.json"
    path.write_text(json.dumps(document))
    return path


def test_load_and_metric_basics() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        path = write_run(
            root,
            7,
            "step",
            "20260101T000000.000Z-a",
            "2026-01-01T00:00:00.000Z",
            [0.0, 0.25, 0.5, 0.75, 1.0],
            [0.0, 0.5, 1.0, 1.0, 1.0],
        )
        run = sim.load(path)
        assert run.simulation_name == "step"
        assert run.sample_count == 5
        assert run.has_signal("out")
        assert run.has_signal("V(OUT)")
        assert run.trace(" V ( out ) ").values == [0.0, 0.5, 1.0, 1.0, 1.0]
        assert run.trace("v(out)").size == 5
        assert run.traces["V(OUT)"].size == 5
        assert run.metrics().traces["V(OUT)"].minimum == 0.0
        assert run.metrics().summary()["state"] == "succeeded"
        metrics = run.trace("v(out)").metrics()
        assert (
            run.trace("v(out)").metrics(settling_tolerance=0.01).settling_time
            is not None
        )
        assert metrics.minimum == 0.0
        assert metrics.maximum == 1.0
        assert metrics.peak_to_peak == 1.0
        assert metrics.mean == 0.7
        assert metrics.rms > metrics.mean
        assert metrics.rise_time is not None
        assert metrics.settling_time is not None
        assert metrics.zero_crossings == []


def test_constant_trace_has_safe_optional_metrics() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        path = write_run(
            root,
            8,
            "constant",
            "20260101T000001.000Z-b",
            "2026-01-01T00:00:01.000Z",
            [0.0, 0.5, 1.0],
            [2.0, 2.0, 2.0],
        )
        metrics = sim.load(path).trace("v(out)").metrics()
        assert metrics.minimum == metrics.maximum == 2.0
        assert metrics.steady_state == 2.0
        assert metrics.overshoot_percent is None
        assert metrics.rise_time is None
        assert metrics.fall_time is None
        assert metrics.settling_time is None
        assert metrics.ringing_frequency is None

        empty = sim.Trace("v(empty)", [], []).metrics()
        assert empty.minimum is None
        assert empty.maximum is None
        assert empty.zero_crossings == []


def test_multi_simulation_discovery_and_latest() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        write_run(
            root,
            1,
            "first",
            "20260101T000000.000Z-a",
            "2026-01-01T00:00:00.000Z",
            [0.0, 1.0],
            [0.0, 1.0],
        )
        write_run(
            root,
            2,
            "second",
            "20260101T000002.000Z-b",
            "2026-01-01T00:00:02.000Z",
            [0.0, 1.0],
            [1.0, 0.0],
            trace_name="v(in)",
        )
        info = sim.list_simulations(root)
        assert [item.name for item in info] == ["first", "second"]
        assert [item.run_count for item in info] == [1, 1]
        run_info = sim.list_runs(root)
        assert [item.trace_names for item in run_info] == [("v(out)",), ("v(in)",)]
        assert [item.sample_count for item in run_info] == [2, 2]

        runs = sim.load_latest_many(root, ["first", "second"])
        assert set(runs) == {"first", "second"}
        assert runs["first"].trace("v(out)").metrics().initial == 0.0
        assert runs["second"].trace("v(in)").metrics().initial == 1.0

        assert sim.load_latest(root, "missing") is None
        assert sim.load_latest(root, "second").simulation_id == 2


def test_underdamped_ringing_metrics() -> None:
    time = [index / 1000.0 for index in range(2000)]
    zeta = 0.15
    omega_n = 2.0 * math.pi * 10.0
    omega_d = omega_n * math.sqrt(1.0 - zeta * zeta)
    values = [
        1.0
        - math.exp(-zeta * omega_n * timestamp)
        * (
            math.cos(omega_d * timestamp)
            + zeta / math.sqrt(1.0 - zeta * zeta) * math.sin(omega_d * timestamp)
        )
        for timestamp in time
    ]
    metrics = sim.Trace("v(n1)", values, time).metrics()
    assert 50.0 < metrics.overshoot_percent < 75.0
    assert 0.008 < metrics.rise_time < 0.03
    assert 0.3 < metrics.settling_time < 0.6
    assert 8.0 < metrics.ringing_frequency < 12.0
    assert 0.1 < metrics.damping_ratio < 0.2


def test_rejects_legacy_result_shape() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "legacy.json"
        path.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "state": "succeeded",
                    "analysis": "transient",
                    "time": [0.0, 1.0],
                    "traces": [{"name": "v(out)", "values": [0.0, 1.0]}],
                }
            )
        )
        try:
            sim.load(path)
        except sim.SimulationDataError:
            pass
        else:
            raise AssertionError("legacy result shape should be rejected")


def test_missing_and_malformed_results_raise_clear_errors() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        try:
            sim.load(root / "missing.json")
        except sim.SimulationDataError as error:
            assert "not found" in str(error).lower()
        else:
            raise AssertionError("missing result should raise SimulationDataError")

        malformed = root / "malformed.json"
        malformed.write_text("{not json")
        try:
            sim.load(malformed)
        except sim.SimulationDataError as error:
            assert "json" in str(error).lower()
        else:
            raise AssertionError("malformed result should raise SimulationDataError")

        missing_arrays = root / "missing-arrays.json"
        missing_arrays.write_text(
            json.dumps(
                {
                    "format": "ice-simulation-result",
                    "run": {"id": "x", "created_at": "2026-01-01T00:00:00.000Z"},
                }
            )
        )
        try:
            sim.load(missing_arrays)
        except sim.SimulationDataError as error:
            assert "time or traces" in str(error).lower()
        else:
            raise AssertionError(
                "missing result arrays should raise SimulationDataError"
            )


if __name__ == "__main__":
    test_load_and_metric_basics()
    test_constant_trace_has_safe_optional_metrics()
    test_multi_simulation_discovery_and_latest()
    test_underdamped_ringing_metrics()
    test_rejects_legacy_result_shape()
    test_missing_and_malformed_results_raise_clear_errors()
    print("icepy_sim tests passed")
