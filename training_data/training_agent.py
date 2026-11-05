#!/usr/bin/env python3
"""Generate agent-authored analogue and mixed-signal training data with ICE.

The script reads one circuit name from ``circuits.txt``, asks a language model
served by OpenRouter for a complete ngspice netlist, stores that netlist as an
ICE text-SPICE schematic, simulates it with ``ice-cli``, evaluates the
specifications the model proposed against the simulated waveforms, and writes:

- ``thoughts.md``  — every model turn, its reasoning, every ICE command and
  result, and the final verdict (the agent's thought process);
- ``specs.md`` / ``specs.csv`` — the specification and performance table;
- ``netlist.sp``, ``simulation.json``, ``proposal.json`` and ``project/`` —
  the schematic, the raw simulated run, the model's final answer, and the
  openable ICE project.

Usage::

    export OPENROUTER_API_KEY=sk-or-...
    python3 Training-data/training_agent.py                 # choose from list
    python3 Training-data/training_agent.py --circuit 12
    python3 Training-data/training_agent.py --circuit "Sallen-Key low-pass filter"

ICE runs ngspice transient analysis only, so the model is instructed to design
transient-testable circuits with generic ``.model`` cards (no foundry PDK is
assumed to be installed).
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from datetime import datetime
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from typing import Any, Mapping, Sequence

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent
CIRCUIT_LIST = HERE / "circuits.txt"
ENV_FILE = HERE / ".env"
DEFAULT_RUNS_DIR = HERE / "runs"
ICE_CLI = os.environ.get("ICE_CLI", "ice-cli")
OPENROUTER_URL = "https://openrouter.ai/api/v1/chat/completions"
DEFAULT_MODEL = os.environ.get("OPENROUTER_MODEL", "nvidia/nemotron-3-ultra-550b-a55b:free")
MAX_TIME_POINTS = 400_000
MAX_NETLIST_CHARS = 20_000


def _import_icepy_sim():
    try:
        import icepy_sim as sim  # type: ignore[import-not-found]
    except ImportError:
        python_dir = REPO_ROOT / "python"
        if python_dir.is_dir():
            sys.path.insert(0, str(python_dir))
        import icepy_sim as sim  # type: ignore[import-not-found]
    return sim


sim = _import_icepy_sim()


class ProposalError(ValueError):
    """Raised when a model reply cannot be turned into a usable design."""


# ---------------------------------------------------------------------------
# Quantities, target parsing, and formatting
# ---------------------------------------------------------------------------

_SUFFIXES = {
    "t": 1e12,
    "g": 1e9,
    "meg": 1e6,
    "k": 1e3,
    "m": 1e-3,
    "u": 1e-6,
    "n": 1e-9,
    "p": 1e-12,
    "f": 1e-15,
}

_NUMBER = re.compile(
    r"^([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)\s*([a-zA-Z]*)$"
)


def parse_value(value: Any, label: str) -> float:
    """Parse ``1.2u``-style quantities (SPICE suffixes) into a float."""
    if isinstance(value, bool):
        raise ValueError(f"{label} must be a number, not a boolean")
    if isinstance(value, (int, float)):
        number = float(value)
    else:
        match = _NUMBER.match(str(value).strip())
        if not match:
            raise ValueError(f"{label}: cannot read the quantity {value!r}")
        number = float(match.group(1))
        suffix = match.group(2).lower()
        if suffix:
            if suffix not in _SUFFIXES:
                raise ValueError(
                    f"{label}: unknown suffix {suffix!r} in {value!r} "
                    "(use k, meg, m, u, n, p)"
                )
            number *= _SUFFIXES[suffix]
    if not math.isfinite(number):
        raise ValueError(f"{label} must be finite")
    return number


_PREFIXES = (
    (1e9, "G"),
    (1e6, "M"),
    (1e3, "k"),
    (1.0, ""),
    (1e-3, "m"),
    (1e-6, "µ"),
    (1e-9, "n"),
    (1e-12, "p"),
)


def format_value(value: float | None, unit: str = "") -> str:
    """Format a measurement with an engineering prefix, e.g. ``1.59 kHz``."""
    if value is None:
        return "n/a"
    if not math.isfinite(value):
        return str(value)
    if unit in {"dB", "%"}:
        if abs(value) < 1e-3:
            return f"0 {unit}"
        return f"{value:.4g} {unit}"
    if unit in {"ratio", ""}:
        return f"{value:.4g} {unit}".strip()
    magnitude = abs(value)
    if magnitude == 0.0:
        return f"0 {unit}".strip()
    for scale, prefix in _PREFIXES:
        if magnitude >= scale * 0.9995:
            return f"{value / scale:.4g} {prefix}{unit}".strip()
    return f"{value / 1e-15:.4g} f{unit}".strip()


_TARGET = re.compile(r"^\s*(>=|<=|==|=|~|>|<)?\s*(.+?)\s*$")


def parse_target(text: str) -> tuple[str, float]:
    match = _TARGET.match(text)
    if not match:
        raise ValueError(f"cannot read the target {text!r}")
    operator = match.group(1) or "="
    limit = parse_value(match.group(2), f"target {text!r}")
    return operator, limit


def compare_to_target(
    measured: float, target: str | None, tolerance: float = 0.05
) -> str:
    """Return ``pass``, ``fail`` or ``info`` for a measured value."""
    if target is None or not str(target).strip():
        return "info"
    operator, limit = parse_target(str(target))
    if operator in {">=", ">"}:
        met = measured >= limit if operator == ">=" else measured > limit
    elif operator in {"<=", "<"}:
        met = measured <= limit if operator == "<=" else measured < limit
    else:
        met = abs(measured - limit) <= tolerance * max(abs(limit), 1e-30)
    return "pass" if met else "fail"


# ---------------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------------

_STATISTICS = {
    "min": "minimum",
    "max": "maximum",
    "peak_to_peak": "peak_to_peak",
    "mean": "mean",
    "rms": "rms",
    "steady_state": "steady_state",
    "final": "final",
    "rise_time": "rise_time",
    "fall_time": "fall_time",
    "delay": "delay",
    "settling_time": "settling_time",
    "overshoot_percent": "overshoot_percent",
    "peak_time": "peak_time",
    "ringing_frequency": "ringing_frequency",
}

_DERIVED = {"gain", "gain_db", "oscillation_frequency", "average_power"}
METRIC_CHOICES = (*_STATISTICS, *_DERIVED)


def default_unit(metric: str, trace: str) -> str:
    if metric == "gain_db":
        return "dB"
    if metric == "gain":
        return "ratio"
    if metric == "overshoot_percent":
        return "%"
    if metric in {"ringing_frequency", "oscillation_frequency"}:
        return "Hz"
    if metric in {"rise_time", "fall_time", "delay", "settling_time", "peak_time"}:
        return "s"
    if metric == "average_power":
        return "W"
    name = trace.strip().lower()
    if name.startswith(("i(", "i[", "@")):
        return "A"
    return "V"


def _rms(values: Sequence[float]) -> float:
    if not values:
        return 0.0
    return math.sqrt(sum(value * value for value in values) / len(values))


def _tail(trace, fraction: float = 0.5):
    """Return the last ``fraction`` of a trace (startup excluded)."""
    if trace.size < 2:
        return trace
    start = trace.time[0] + (1.0 - fraction) * (trace.time[-1] - trace.time[0])
    return trace.between(start, trace.time[-1])


def _oscillation_frequency(trace) -> float | None:
    """Frequency from rising mid-level crossings over the settled half."""
    tail = _tail(trace)
    values = tail.values
    if len(values) < 4:
        return None
    high, low = max(values), min(values)
    if high - low <= 1e-18:
        return None
    level = 0.5 * (high + low)
    crossings: list[float] = []
    for index in range(1, len(values)):
        left = values[index - 1] - level
        right = values[index] - level
        if left < 0.0 <= right:
            span = values[index] - values[index - 1]
            fraction = -left / span if span else 0.0
            crossings.append(
                tail.time[index - 1]
                + fraction * (tail.time[index] - tail.time[index - 1])
            )
    if len(crossings) < 2:
        return None
    period = (crossings[-1] - crossings[0]) / (len(crossings) - 1)
    return 1.0 / period if period > 0.0 else None


@dataclass
class SpecRequest:
    name: str
    metric: str
    trace: str
    target: str | None
    unit: str
    reference: str | None = None
    voltage: float | None = None


@dataclass
class SpecResult:
    name: str
    metric: str
    unit: str
    target: str | None
    measured: float | None
    status: str
    note: str = ""

    @property
    def measured_text(self) -> str:
        return format_value(self.measured, self.unit)

    @property
    def status_text(self) -> str:
        return {
            "pass": "PASS",
            "fail": "FAIL",
            "unmeasured": "NO DATA",
            "info": "measured",
        }.get(self.status, self.status)


def measure_specification(spec: SpecRequest, run) -> SpecResult:
    """Compute one requested specification from a simulated run."""
    def result(value: float | None, note: str = "") -> SpecResult:
        if value is None:
            return SpecResult(
                spec.name, spec.metric, spec.unit, spec.target, None, "unmeasured", note
            )
        status = compare_to_target(value, spec.target)
        return SpecResult(
            spec.name, spec.metric, spec.unit, spec.target, value, status, note
        )

    if not run.has_signal(spec.trace):
        return result(None, f"signal {spec.trace!r} is not in the run")

    trace = run.trace(spec.trace)

    if spec.metric in {"gain", "gain_db"}:
        if not spec.reference or not run.has_signal(spec.reference):
            return result(None, f"reference {spec.reference!r} is not in the run")
        reference = _tail(run.trace(spec.reference))
        output = _tail(trace)
        denominator = _rms(reference.values)
        if denominator <= 0.0:
            return result(None, "reference signal has zero RMS")
        ratio = _rms(output.values) / denominator
        if ratio <= 0.0:
            return result(None, "output signal has zero RMS")
        value = 20.0 * math.log10(ratio) if spec.metric == "gain_db" else ratio
        return result(value, "RMS ratio over the last 50% of the run")

    if spec.metric == "oscillation_frequency":
        return result(_oscillation_frequency(trace), "rising mid-level crossings")

    if spec.metric == "average_power":
        if not spec.voltage:
            return result(None, "no supply voltage given")
        values = trace.values
        if not values:
            return result(None, "empty trace")
        average = sum(values) / len(values)
        return result(abs(average) * spec.voltage, "mean current × supply voltage")

    attribute = _STATISTICS.get(spec.metric)
    if attribute is None:
        return result(None, f"unknown metric {spec.metric!r}")
    metrics = trace.metrics()
    return result(getattr(metrics, attribute, None))


def trace_metrics(run) -> dict[str, dict[str, Any]]:
    return {name: run.trace(name).metrics().as_dict() for name in run.trace_names}


# ---------------------------------------------------------------------------
# Circuit catalogue
# ---------------------------------------------------------------------------


def load_api_key(explicit: str | None = None) -> str:
    """Resolve the key from --api-key, then the environment, then ``.env``."""
    if explicit and explicit.strip():
        return explicit.strip()
    from_environment = os.environ.get("OPENROUTER_API_KEY", "").strip()
    if from_environment:
        return from_environment
    if ENV_FILE.is_file():
        for line in ENV_FILE.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            name, value = line.split("=", 1)
            if name.strip() in {"OPENROUTER_API_KEY", "OPENROUTER_KEY"}:
                key = value.strip().strip("\"'").strip()
                if key:
                    return key
    return ""


def read_circuits(path: Path = CIRCUIT_LIST) -> list[str]:
    entries: list[str] = []
    for line in path.read_text().splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        entries.append(stripped)
    return entries


def choose_circuit(circuits: Sequence[str], selection: str | None) -> str:
    """Resolve ``selection`` (index, name, or unique substring) or prompt."""
    if selection is None:
        if not sys.stdin.isatty():
            raise SystemExit(
                "No circuit selected and stdin is not interactive. "
                "Pass --circuit <number|name>."
            )
        for index, name in enumerate(circuits, start=1):
            print(f"{index:3d}. {name}")
        while True:
            answer = input(f"Select a circuit [1-{len(circuits)}]: ").strip()
            if not answer:
                continue
            try:
                return choose_circuit(circuits, answer)
            except (ValueError, SystemExit) as error:
                print(error)

    text = selection.strip()
    if text.isdigit():
        index = int(text)
        if 1 <= index <= len(circuits):
            return circuits[index - 1]
        raise SystemExit(f"Circuit number {index} is outside 1-{len(circuits)}.")

    lowered = text.lower()
    for name in circuits:
        if name.lower() == lowered:
            return name
    matches = [name for name in circuits if lowered in name.lower()]
    if len(matches) == 1:
        return matches[0]
    if not matches:
        raise SystemExit(f"No circuit in {CIRCUIT_LIST.name} matches {text!r}.")
    listing = "\n".join(f"  - {name}" for name in matches)
    raise SystemExit(f"That name is ambiguous. Candidates:\n{listing}")


# ---------------------------------------------------------------------------
# Transcript
# ---------------------------------------------------------------------------


def _fence(text: str, language: str = "") -> str:
    marker = "````" if "```" in text else "```"
    return f"{marker}{language}\n{text.rstrip()}\n{marker}"


class Transcript:
    """Append-only record of the agent's reasoning and every tool result."""

    def __init__(self, path: Path, title: str, metadata: Mapping[str, Any]):
        self.path = path
        lines = [f"# {title}", ""]
        lines += [
            f"- **{key}**: {value}"
            for key, value in metadata.items()
            if value not in (None, "")
        ]
        lines += [
            "",
            "This file is appended while the agent works, so an interrupted run "
            "keeps its reasoning on disk.",
            "",
        ]
        self.path.write_text("\n".join(lines) + "\n")

    def _append(self, text: str) -> None:
        with self.path.open("a") as handle:
            handle.write(text.rstrip() + "\n\n")

    def section(self, heading: str, body: str = "") -> None:
        self._append(f"## {heading}\n\n{body}" if body else f"## {heading}")

    def detail(self, heading: str, body: str = "") -> None:
        self._append(f"### {heading}\n\n{body}" if body else f"### {heading}")

    def reasoning(self, text: str) -> None:
        self.detail("Reasoning", text.strip() or "_(no separate reasoning returned)_")

    def assistant(self, text: str) -> None:
        self.detail("Response", _fence(text.strip() or "(empty)", "json"))

    def ice(self, command: Sequence[str], status: int, stdout: str, stderr: str,
            elapsed: float) -> None:
        rendered = " ".join(shlex.quote(str(part)) for part in command)
        output = f"exit {status} in {elapsed:.2f}s"
        if stderr.strip():
            output += "\n\nstderr:\n" + _fence(stderr[-4000:])
        if stdout.strip():
            output += "\n\nstdout:\n" + _fence(stdout[-4000:])
        self.detail(f"ICE command\n\n{_fence('$ ' + rendered, 'sh')}", output)


# ---------------------------------------------------------------------------
# OpenRouter client
# ---------------------------------------------------------------------------


@dataclass
class LlmReply:
    content: str
    reasoning: str
    model: str
    usage: Mapping[str, Any] = field(default_factory=dict)
    elapsed: float = 0.0


def _content_text(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, str):
        return value.strip()
    if isinstance(value, list):
        parts = []
        for item in value:
            if isinstance(item, str):
                parts.append(item)
            elif isinstance(item, dict) and isinstance(item.get("text"), str):
                parts.append(item["text"])
        return "\n".join(parts).strip()
    return str(value).strip()


def call_openrouter(
    api_key: str,
    model: str,
    messages: Sequence[Mapping[str, str]],
    timeout: float = 180.0,
) -> LlmReply:
    """Send one chat-completion request and return text, reasoning and usage."""
    body = json.dumps(
        {"model": model, "messages": list(messages), "temperature": 0.2}
    ).encode()
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }
    started = time.perf_counter()
    last_error = "no attempt was made"
    for attempt in range(3):
        request = urllib.request.Request(
            OPENROUTER_URL, data=body, headers=headers, method="POST"
        )
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                document = json.loads(response.read().decode("utf-8"))
            break
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", "replace")
            message = f"HTTP {error.code}: {detail[:800]}"
            if error.code == 429 or error.code >= 500 or error.code == 408:
                last_error = message
                time.sleep(2.0 * (attempt + 1))
                continue
            raise RuntimeError(f"OpenRouter rejected the request ({message})") from error
        except urllib.error.URLError as error:
            last_error = f"connection failed: {error.reason}"
            time.sleep(2.0 * (attempt + 1))
    else:
        raise RuntimeError(f"OpenRouter request failed after 3 attempts ({last_error})")

    choices = document.get("choices")
    if not isinstance(choices, list) or not choices:
        error = document.get("error") or document
        raise RuntimeError(
            "OpenRouter returned no choices: " + json.dumps(error)[:800]
        )
    message = choices[0].get("message") or {}
    reasoning = _content_text(message.get("reasoning") or message.get("reasoning_content"))
    if not reasoning and isinstance(message.get("reasoning_details"), list):
        reasoning = "\n\n".join(
            item["text"]
            for item in message["reasoning_details"]
            if isinstance(item, dict) and isinstance(item.get("text"), str)
        ).strip()
    return LlmReply(
        content=_content_text(message.get("content")),
        reasoning=reasoning,
        model=str(document.get("model") or model),
        usage=document.get("usage") or {},
        elapsed=time.perf_counter() - started,
    )


def extract_json(text: str) -> dict[str, Any]:
    """Return the first JSON object embedded in a model reply."""
    start = text.find("{")
    while start != -1:
        depth = 0
        in_string = False
        escaped = False
        for index in range(start, len(text)):
            character = text[index]
            if in_string:
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == '"':
                    in_string = False
                continue
            if character == '"':
                in_string = True
            elif character == "{":
                depth += 1
            elif character == "}":
                depth -= 1
                if depth == 0:
                    candidate = text[start : index + 1]
                    try:
                        parsed = json.loads(candidate)
                    except json.JSONDecodeError:
                        break
                    if isinstance(parsed, dict):
                        return parsed
                    break
        start = text.find("{", start + 1)
    raise ProposalError("the reply does not contain a JSON object")


# ---------------------------------------------------------------------------
# Proposal parsing
# ---------------------------------------------------------------------------

FORBIDDEN_DIRECTIVES = (".tran", ".ac", ".dc", ".op", ".end", ".control", ".meas")


@dataclass
class Proposal:
    circuit: str
    summary: str
    notes: str
    netlist: str
    stop: str
    step: str
    signals: list[str]
    specifications: list[SpecRequest]


def _netlist_text(payload: Mapping[str, Any]) -> str:
    raw = payload.get("netlist")
    if isinstance(raw, list):
        text = "\n".join(str(line) for line in raw)
    elif isinstance(raw, str):
        text = raw
    else:
        raise ProposalError("the reply has no \"netlist\" string")
    text = text.replace("\r\n", "\n").replace("\r", "\n").strip("\n")
    if not text.strip():
        raise ProposalError("the netlist is empty")
    if len(text) > MAX_NETLIST_CHARS:
        raise ProposalError(
            f"the netlist is too long ({len(text)} characters); keep it under "
            f"{MAX_NETLIST_CHARS}"
        )
    lines = [line for line in text.splitlines() if line.strip()]
    if not any(
        line.strip()[0].isalpha() and not line.lstrip().startswith((".", "*"))
        for line in lines
    ):
        raise ProposalError("the netlist contains no device or source cards")
    lowered = text.lower()
    for directive in FORBIDDEN_DIRECTIVES:
        if re.search(rf"^\s*\{re.escape(directive)}\b", lowered, re.MULTILINE):
            raise ProposalError(
                f"remove the {directive} directive: ICE supplies the transient "
                "analysis and computes measurements itself"
            )
    return text + "\n"


def parse_proposal(payload: Mapping[str, Any], circuit: str) -> Proposal:
    """Validate a decoded model reply and normalise it into a Proposal."""
    if not isinstance(payload, dict):
        raise ProposalError("the reply must be a JSON object")

    netlist = _netlist_text(payload)

    analysis = payload.get("analysis") or {}
    if not isinstance(analysis, dict):
        raise ProposalError("\"analysis\" must be an object with stop and step")
    stop = str(analysis.get("stop") or "1m").strip()
    step = str(analysis.get("step") or "1u").strip()
    try:
        stop_seconds = parse_value(stop, "analysis.stop")
        step_seconds = parse_value(step, "analysis.step")
    except ValueError as error:
        raise ProposalError(str(error)) from error
    if stop_seconds <= 0 or step_seconds <= 0:
        raise ProposalError("analysis.stop and analysis.step must be positive")
    if step_seconds > stop_seconds:
        raise ProposalError("analysis.step must be smaller than analysis.stop")
    points = stop_seconds / step_seconds
    if points > MAX_TIME_POINTS:
        raise ProposalError(
            f"analysis.stop/step asks for {points:.0f} time points; at most "
            f"{MAX_TIME_POINTS} are allowed. Increase step or decrease stop."
        )

    raw_signals = payload.get("signals")
    if isinstance(raw_signals, str):
        raw_signals = re.split(r"[,\s]+", raw_signals)
    if not isinstance(raw_signals, list):
        raise ProposalError("\"signals\" must be a non-empty list such as [\"v(out)\"]")
    signals = [str(item).strip() for item in raw_signals if str(item).strip()]
    if not signals:
        raise ProposalError("\"signals\" must name at least one signal")
    signals = list(dict.fromkeys(signals))

    raw_specs = payload.get("specifications") or payload.get("specs")
    if not isinstance(raw_specs, list) or not raw_specs:
        raise ProposalError(
            "\"specifications\" must contain at least one measurable specification"
        )
    specifications: list[SpecRequest] = []
    for entry in raw_specs:
        if not isinstance(entry, dict):
            raise ProposalError("each specification must be a JSON object")
        metric = str(entry.get("metric") or "").strip().lower()
        if metric not in METRIC_CHOICES:
            raise ProposalError(
                f"unknown metric {metric!r}; choose one of {', '.join(METRIC_CHOICES)}"
            )
        trace = str(entry.get("trace") or "").strip()
        if not trace:
            raise ProposalError(f"specification {metric!r} needs a \"trace\"")
        reference = entry.get("reference")
        reference = str(reference).strip() if reference else None
        if metric in {"gain", "gain_db"} and not reference:
            raise ProposalError(f"{metric} needs a \"reference\" trace")
        voltage = None
        if metric == "average_power":
            if entry.get("voltage") is None:
                raise ProposalError("average_power needs a \"voltage\" value")
            voltage = parse_value(entry["voltage"], "specification voltage")
            if voltage <= 0:
                raise ProposalError("average_power voltage must be positive")
        target = entry.get("target")
        if target is not None:
            target = str(target).strip() or None
        if target is not None:
            try:
                parse_target(target)
            except ValueError as error:
                raise ProposalError(str(error)) from error
        unit = str(entry.get("unit") or "").strip() or default_unit(metric, trace)
        name = str(entry.get("name") or metric).strip() or metric
        specifications.append(
            SpecRequest(
                name=name,
                metric=metric,
                trace=trace,
                target=target,
                unit=unit,
                reference=reference,
                voltage=voltage,
            )
        )
        for signal in (trace, reference):
            if signal and signal not in signals:
                signals.append(signal)

    return Proposal(
        circuit=str(payload.get("circuit") or circuit).strip() or circuit,
        summary=str(payload.get("topology_summary") or "").strip(),
        notes=str(payload.get("design_notes") or "").strip(),
        netlist=netlist,
        stop=stop,
        step=step,
        signals=signals,
        specifications=specifications,
    )


# ---------------------------------------------------------------------------
# ICE orchestration
# ---------------------------------------------------------------------------


@dataclass
class IceResult:
    command: list[str]
    status: int
    stdout: str
    stderr: str
    elapsed: float

    def document(self) -> dict[str, Any] | None:
        text = self.stdout.strip()
        if not text:
            return None
        try:
            parsed = json.loads(text)
        except json.JSONDecodeError:
            try:
                parsed = extract_json(text)
            except ProposalError:
                return None
        return parsed if isinstance(parsed, dict) else None

    @property
    def error(self) -> str:
        document = self.document() or {}
        message = str(document.get("error") or "").strip()
        if message:
            return message
        return (self.stderr or self.stdout).strip()[-4000:] or f"exit status {self.status}"


def run_ice(
    arguments: Sequence[Any],
    cwd: Path,
    timeout: float,
    transcript: Transcript,
) -> IceResult:
    command = [ICE_CLI, *[str(argument) for argument in arguments]]
    started = time.perf_counter()
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except FileNotFoundError:
        raise SystemExit(
            f"{ICE_CLI!r} was not found. Build it or set ICE_CLI to the executable."
        ) from None
    except subprocess.TimeoutExpired as error:
        elapsed = time.perf_counter() - started
        result = IceResult(
            command,
            -1,
            error.stdout or "",
            f"timed out after {timeout:.0f}s",
            elapsed,
        )
        transcript.ice(command, result.status, result.stdout, result.stderr, elapsed)
        return result
    elapsed = time.perf_counter() - started
    result = IceResult(
        command, completed.returncode, completed.stdout, completed.stderr, elapsed
    )
    transcript.ice(command, result.status, result.stdout, result.stderr, elapsed)
    return result


@dataclass
class SimulationAttempt:
    ok: bool
    schematic: str
    simulation: str
    stored: Path | None
    error: str
    elapsed: float


def simulate_design(
    project_dir: Path,
    netlist_path: Path,
    proposal: Proposal,
    attempt: int,
    transcript: Transcript,
    simulation_timeout: float,
) -> SimulationAttempt:
    """Create the schematic in the ICE project and simulate it."""
    schematic = f"design_a{attempt}"
    simulation = f"sim_a{attempt}"
    project_argument = project_dir.name

    if not (project_dir / "project.icepro").exists():
        created = run_ice(
            ["project-create", project_argument, project_dir.name],
            cwd=project_dir.parent,
            timeout=120.0,
            transcript=transcript,
        )
        if created.status != 0:
            return SimulationAttempt(
                False, schematic, simulation, None, created.error, created.elapsed
            )

    added = run_ice(
        [
            "schematic-add-text",
            project_argument,
            schematic,
            "generic",
            str(netlist_path.resolve()),
        ],
        cwd=project_dir.parent,
        timeout=120.0,
        transcript=transcript,
    )
    if added.status != 0:
        return SimulationAttempt(
            False, schematic, simulation, None, added.error, added.elapsed
        )

    configured = run_ice(
        [
            "simulation-add",
            project_argument,
            simulation,
            schematic,
            "transient",
            proposal.stop,
            proposal.step,
            ",".join(proposal.signals),
        ],
        cwd=project_dir.parent,
        timeout=120.0,
        transcript=transcript,
    )
    if configured.status != 0:
        return SimulationAttempt(
            False, schematic, simulation, None, configured.error, configured.elapsed
        )

    executed = run_ice(
        [
            "simulation-run",
            project_argument,
            simulation,
            str(int(simulation_timeout * 1000)),
        ],
        cwd=project_dir.parent,
        timeout=simulation_timeout + 30.0,
        transcript=transcript,
    )
    document = executed.document() or {}
    stored_value = document.get("stored")
    stored = None
    if stored_value:
        stored = Path(str(stored_value))
        if not stored.is_absolute():
            stored = project_dir.parent / stored
        stored = stored.resolve()
    state = str(document.get("state") or "")
    if executed.status != 0 or state != "succeeded" or stored is None:
        message = str(document.get("error") or executed.error)
        return SimulationAttempt(
            False, schematic, simulation, stored, message, executed.elapsed
        )
    return SimulationAttempt(True, schematic, simulation, stored, "", executed.elapsed)


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------


def _cell(value: Any) -> str:
    text = " ".join(str(value).split())
    return text.replace("|", "\\|")


def _row(cells: Sequence[Any]) -> str:
    return "| " + " | ".join(_cell(cell) for cell in cells) + " |"


def specifications_table(specifications: Sequence[SpecResult]) -> str:
    lines = [
        _row(["Specification", "Metric", "Target", "Measured", "Status"]),
        _row(["---"] * 5),
    ]
    for spec in specifications:
        lines.append(
            _row(
                [
                    spec.name,
                    spec.metric,
                    spec.target or "—",
                    spec.measured_text,
                    spec.status_text,
                ]
            )
        )
    return "\n".join(lines)


def performance_table(metrics: Mapping[str, Mapping[str, Any]]) -> str:
    headers = [
        "Signal",
        "Min",
        "Max",
        "Peak-to-peak",
        "Mean",
        "RMS",
        "Rise",
        "Fall",
        "Overshoot",
        "Settling",
        "Ringing",
    ]
    lines = [_row(headers), _row(["---"] * len(headers))]
    for name, values in metrics.items():
        unit = default_unit("mean", name)
        lines.append(
            _row(
                [
                    name,
                    format_value(values.get("minimum"), unit),
                    format_value(values.get("maximum"), unit),
                    format_value(values.get("peak_to_peak"), unit),
                    format_value(values.get("mean"), unit),
                    format_value(values.get("rms"), unit),
                    format_value(values.get("rise_time"), "s"),
                    format_value(values.get("fall_time"), "s"),
                    format_value(values.get("overshoot_percent"), "%"),
                    format_value(values.get("settling_time"), "s"),
                    format_value(values.get("ringing_frequency"), "Hz"),
                ]
            )
        )
    return "\n".join(lines)


def write_report(
    path: Path,
    csv_path: Path,
    circuit: str,
    proposal: Proposal,
    attempt: SimulationAttempt,
    run,
    specifications: Sequence[SpecResult],
    metrics: Mapping[str, Mapping[str, Any]],
    metadata: Mapping[str, Any],
    history: Sequence[Mapping[str, Any]],
) -> None:
    passed = sum(1 for spec in specifications if spec.status == "pass")
    failed = sum(1 for spec in specifications if spec.status == "fail")
    unmeasured = sum(1 for spec in specifications if spec.status == "unmeasured")
    verdict = "all measurable specifications met"
    if failed:
        verdict = f"{failed} specification(s) not met"
    elif unmeasured:
        verdict = f"{unmeasured} specification(s) could not be measured"

    details = [
        _row(["Field", "Value"]),
        _row(["---", "---"]),
    ]
    for key, value in metadata.items():
        if value not in (None, ""):
            details.append(_row([key, value]))
    details += [
        _row(["Schematic", f"`{attempt.schematic}` (text SPICE)"]),
        _row(["Simulation", f"`{attempt.simulation}` (transient)"]),
        _row(["State", "succeeded" if attempt.ok else "failed"]),
        _row(["Time window", f"{proposal.stop} (step {proposal.step})"]),
        _row(["Signals", ", ".join(run.trace_names)]),
        _row(["Samples", run.sample_count]),
        _row(["Simulator", f"{run.simulator} {run.simulator_version}".strip()]),
        _row(["Specifications", f"{passed} met, {failed} missed, {unmeasured} no data"]),
        _row(["Verdict", verdict]),
    ]

    sections = [
        f"# {circuit}",
        "",
        f"Specification and performance report produced by `training_agent.py` "
        f"on {datetime.now().astimezone().strftime('%Y-%m-%d %H:%M:%S %Z')}.",
        "",
        "## Simulation",
        "",
        "\n".join(details),
        "",
        "## Specifications",
        "",
        specifications_table(specifications),
        "",
    ]
    if failed:
        sections += [
            "Missed specifications: "
            + ", ".join(spec.name for spec in specifications if spec.status == "fail")
            + ".",
            "",
        ]
    if unmeasured:
        sections += [
            "Unmeasured specifications: "
            + ", ".join(
                f"{spec.name} ({spec.note})"
                for spec in specifications
                if spec.status == "unmeasured"
            )
            + ".",
            "",
        ]
    sections += [
        "## Measured performance",
        "",
        performance_table(metrics),
        "",
        "## Agent design summary",
        "",
        f"**Topology:** {proposal.summary or 'not provided'}",
        "",
        f"**Notes:** {proposal.notes or 'not provided'}",
        "",
        "## Attempts",
        "",
        _row(["Attempt", "Simulation", "Specs met", "Note"]),
        _row(["---"] * 4),
    ]
    for entry in history:
        sections.append(
            _row(
                [
                    entry.get("attempt", ""),
                    entry.get("state", ""),
                    entry.get("specs", ""),
                    entry.get("note", ""),
                ]
            )
        )
    sections += [
        "",
        "## Netlist",
        "",
        _fence(proposal.netlist, "spice"),
        "",
        "## Artifacts",
        "",
        f"- `{Path('thoughts.md').as_posix()}` — full agent reasoning and tool log",
        "- `specs.md`, `specs.csv` — this table",
        "- `netlist.sp` — the simulated netlist",
        "- `simulation.json` — the raw stored ICE run",
        "- `proposal.json` — the model's final structured answer",
        "- `project/` — the ICE project (open `project.icepro` in the ICE GUI)",
        "",
    ]
    path.write_text("\n".join(sections))

    csv_lines = ["specification,metric,target,measured,unit,status"]
    for spec in specifications:
        measured = "" if spec.measured is None else f"{spec.measured:.6g}"
        target = "" if spec.target is None else spec.target
        csv_lines.append(
            ",".join(
                '"' + str(value).replace('"', '""') + '"'
                for value in (
                    spec.name,
                    spec.metric,
                    target,
                    measured,
                    spec.unit,
                    spec.status,
                )
            )
        )
    csv_path.write_text("\n".join(csv_lines) + "\n")


# ---------------------------------------------------------------------------
# Agent loop
# ---------------------------------------------------------------------------

SYSTEM_PROMPT = """You are an analogue and mixed-signal circuit design agent.
ICE (an open-source schematic/layout/simulation tool) will store your netlist as
a text-SPICE schematic and simulate it with ngspice.

Design constraints:
1. Return one complete, self-contained SPICE netlist that ngspice can run.
2. No foundry PDK is installed. Use generic model cards, for example
   `.model nmos nmos(level=1 vto=0.5 kp=100u lambda=0.02)` and
   `.model pnpsub pnp(...)`, or ideal dependent sources (E, G, F, H) for
   behavioural blocks.
3. Node 0 is ground. Every node needs a DC path to ground and a parallel
   capacitance when that is needed for convergence.
4. Do not include .tran, .ac, .dc, .op, .end, .control or .meas cards: ICE
   supplies the transient analysis and computes measurements from the
   waveforms.
5. Stimuli must be transient-friendly: SIN, PULSE, PWL or EXP. Oscillators need
   a non-zero initial condition (e.g. `.ic v(out)=0.1`) or a deliberate
   mismatch so they start.
6. Keep the run cheap: stop 100u-10m, step 10n-10u, at most 400000 points.
7. List the nodes to observe in `signals` using `v(node)` or `i(device)`.
8. Propose 3-8 specifications with targets. The harness computes them from the
   simulated waveforms, so use only these metrics:
   - min, max, peak_to_peak, mean, rms, steady_state, final (waveform
     statistics of `trace`, unit V or A)
   - gain: RMS(trace)/RMS(reference) over the last 50% of the run
   - gain_db: 20*log10 of that ratio, unit dB
   - rise_time, fall_time: 10-90% crossings, unit s
   - delay: 50% crossing, unit s
   - settling_time: 2% settling band, unit s
   - overshoot_percent: unit %
   - peak_time: unit s
   - ringing_frequency: unit Hz
   - oscillation_frequency: rising mid-level crossings over the last 50%, Hz
   - average_power: |mean(trace)| * `voltage` with a current trace, unit W
9. Use realistic values (1.8 V supply, 1k-100k resistors, pF-nF capacitors,
   uA-mA currents) and design so that the targets are actually met. Choose a
   stimulus and time window that let the specification be measured: a step for
   settling/overshoot, a sine for gain, a free-running run for oscillators.
10. If a target cannot be met with a sensible design, say so in design_notes
    instead of stating an unreachable target.

Reply with exactly one JSON object and no other text:
{
  "circuit": "the requested circuit name",
  "topology_summary": "one or two sentences on the topology and sizing",
  "design_notes": "trade-offs, assumptions, expected behaviour",
  "netlist": "* title line\\nvin in 0 0 SIN(0 1 1k)\\n...",
  "analysis": {"stop": "2m", "step": "1u"},
  "signals": ["v(out)", "v(in)"],
  "specifications": [
    {"name": "Midband gain", "metric": "gain_db", "trace": "v(out)",
     "reference": "v(in)", "target": ">= 20", "unit": "dB"},
    {"name": "Settling time", "metric": "settling_time", "trace": "v(out)",
     "target": "<= 500u", "unit": "s"}
  ]
}
Targets use >=, <=, = or ~ (5% band) followed by a value with a SPICE suffix
(k, meg, m, u, n, p)."""


def _slug(text: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    return slug[:60] or "circuit"


def _history_row(
    attempt: int, state: str, specifications: Sequence[SpecResult], note: str
) -> dict[str, Any]:
    passed = sum(1 for spec in specifications if spec.status == "pass")
    return {
        "attempt": attempt,
        "state": state,
        "specs": f"{passed}/{len(specifications)}" if specifications else "—",
        "note": note,
    }


def generate(circuit: str, arguments: argparse.Namespace) -> int:
    runs_dir = Path(arguments.runs_dir).expanduser().resolve()
    base = runs_dir / f"{datetime.now().strftime('%Y%m%d-%H%M%S')}_{_slug(circuit)}"
    run_dir = base
    suffix = 2
    while run_dir.exists():
        run_dir = base.with_name(f"{base.name}-{suffix}")
        suffix += 1
    run_dir.mkdir(parents=True)
    project_dir = run_dir / "project"

    transcript = Transcript(
        run_dir / "thoughts.md",
        f"Agent transcript — {circuit}",
        {
            "Circuit": circuit,
            "Catalogue": CIRCUIT_LIST.name,
            "Model": arguments.model,
            "Started": datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S %Z"),
            "Run directory": run_dir,
        },
    )
    transcript.section(
        "Task",
        f"Design **{circuit}** as an ICE text-SPICE schematic, simulate it, and "
        "meet the proposed specifications. The harness builds the schematic, "
        "runs the simulation, measures every specification from the waveforms, "
        "and gives the agent the results.",
    )
    transcript.detail("System prompt", _fence(SYSTEM_PROMPT))

    user_prompt = (
        f"Design the following circuit: {circuit}.\n\n"
        "Return the JSON proposal now."
    )
    messages: list[dict[str, str]] = [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": user_prompt},
    ]
    transcript.detail("User prompt", _fence(user_prompt))

    history: list[dict[str, Any]] = []
    final: dict[str, Any] | None = None

    for attempt in range(1, arguments.attempts + 1):
        transcript.section(f"Attempt {attempt} of {arguments.attempts}")
        try:
            reply = call_openrouter(
                arguments.api_key,
                arguments.model,
                messages,
                timeout=arguments.request_timeout,
            )
        except RuntimeError as error:
            transcript.section("Model request failed", str(error))
            print(f"error: {error}", file=sys.stderr)
            return 2

        transcript.detail("Model", f"`{reply.model}` in {reply.elapsed:.1f}s")
        transcript.reasoning(reply.reasoning)
        transcript.assistant(reply.content)
        if reply.usage:
            transcript.detail(
                "Token usage",
                ", ".join(f"{key}={value}" for key, value in reply.usage.items()),
            )
        messages.append({"role": "assistant", "content": reply.content})

        try:
            proposal = parse_proposal(extract_json(reply.content), circuit)
        except ProposalError as error:
            transcript.section("Reply rejected", str(error))
            history.append(_history_row(attempt, "invalid reply", [], str(error)))
            messages.append(
                {
                    "role": "user",
                    "content": (
                        f"Your reply could not be used: {error}.\n"
                        "Return the corrected JSON object only."
                    ),
                }
            )
            continue

        netlist_path = run_dir / f"netlist_attempt{attempt}.sp"
        netlist_path.write_text(proposal.netlist)
        transcript.section(
            "Proposed design",
            f"**Topology:** {proposal.summary or 'not provided'}\n\n"
            f"**Notes:** {proposal.notes or 'not provided'}\n\n"
            f"Netlist saved to `{netlist_path.name}`.",
        )
        transcript.detail("Netlist", _fence(proposal.netlist, "spice"))
        transcript.detail(
            "Proposed specifications",
            specifications_table(
                [
                    SpecResult(
                        spec.name,
                        spec.metric,
                        spec.unit,
                        spec.target,
                        None,
                        "info",
                    )
                    for spec in proposal.specifications
                ]
            ),
        )

        attempt_result = simulate_design(
            project_dir,
            netlist_path,
            proposal,
            attempt,
            transcript,
            arguments.sim_timeout,
        )
        if not attempt_result.ok:
            transcript.section(
                "Simulation failed",
                f"{attempt_result.error}\n\n"
                f"Elapsed {attempt_result.elapsed:.1f}s.",
            )
            history.append(
                _history_row(attempt, "simulation failed", [], attempt_result.error)
            )
            messages.append(
                {
                    "role": "user",
                    "content": (
                        "ICE/ngspice could not simulate your netlist:\n"
                        f"{attempt_result.error}\n\n"
                        "Fix the netlist (check node names, model cards, DC paths "
                        "and the requested signals) and return the full JSON again."
                    ),
                }
            )
            continue

        run = sim.load(attempt_result.stored)
        specifications = [
            measure_specification(spec, run) for spec in proposal.specifications
        ]
        metrics = trace_metrics(run)
        warnings = [str(item) for item in run.document.get("warnings") or []]
        transcript.section(
            "Simulation result",
            f"State: **{run.state}**\n\n"
            f"Samples: {run.sample_count} over {run.time[-1]:.6g}s\n\n"
            "### Specifications\n\n"
            + specifications_table(specifications)
            + "\n\n### Performance\n\n"
            + performance_table(metrics)
            + ("\n\n### Warnings\n\n" + "\n".join(f"- {w}" for w in warnings)
               if warnings else ""),
        )

        missed = [spec for spec in specifications if spec.status == "fail"]
        final = {
            "attempt": attempt,
            "proposal": proposal,
            "result": attempt_result,
            "run": run,
            "specifications": specifications,
            "metrics": metrics,
            "warnings": warnings,
        }
        history.append(
            _history_row(
                attempt,
                "succeeded",
                specifications,
                "; ".join(spec.name for spec in missed) + " not met" if missed else "",
            )
        )
        if not missed or attempt == arguments.attempts:
            break

        transcript.section("Specifications missed", f"Feeding the table back to the model.")
        messages.append(
            {
                "role": "user",
                "content": (
                    "The simulation ran, but these specifications were not met:\n"
                    f"{specifications_table(specifications)}\n\n"
                    + (
                        "The simulator also reported:\n"
                        + "\n".join(f"- {warning}" for warning in warnings)
                        + "\n\n"
                        if warnings
                        else ""
                    )
                    + "Revise the design (keep what works) so the targets are met, "
                    "and return the full JSON again."
                ),
            }
        )

    if final is None:
        failure = run_dir / "specs.md"
        failure.write_text(
            f"# {circuit}\n\nNo attempt produced a successful simulation.\n\n"
            "See `thoughts.md` for the agent transcript and simulator errors.\n"
        )
        print(
            f"error: no attempt produced a successful simulation; see {run_dir / 'thoughts.md'}",
            file=sys.stderr,
        )
        return 1

    proposal = final["proposal"]
    attempt_result = final["result"]
    run = final["run"]
    specifications = final["specifications"]

    (run_dir / "netlist.sp").write_text(proposal.netlist)
    shutil.copyfile(attempt_result.stored, run_dir / "simulation.json")
    (run_dir / "proposal.json").write_text(
        json.dumps(
            {
                "circuit": proposal.circuit,
                "topology_summary": proposal.summary,
                "design_notes": proposal.notes,
                "netlist": proposal.netlist,
                "analysis": {"stop": proposal.stop, "step": proposal.step},
                "signals": proposal.signals,
                "specifications": [
                    {
                        "name": spec.name,
                        "metric": spec.metric,
                        "trace": spec.trace,
                        "reference": spec.reference,
                        "voltage": spec.voltage,
                        "target": spec.target,
                        "unit": spec.unit,
                    }
                    for spec in proposal.specifications
                ],
            },
            indent=2,
        )
        + "\n"
    )

    metadata = {
        "Model": arguments.model,
        "Project": project_dir.name,
        "Run directory": run_dir,
        "Warnings": "; ".join(final["warnings"]) or None,
    }
    write_report(
        run_dir / "specs.md",
        run_dir / "specs.csv",
        circuit,
        proposal,
        attempt_result,
        run,
        specifications,
        final["metrics"],
        metadata,
        history,
    )

    transcript.section(
        "Final result",
        f"Simulation **{run.state}** after {final['attempt']} attempt(s).\n\n"
        "### Specifications\n\n"
        + specifications_table(specifications)
        + "\n\n### Performance\n\n"
        + performance_table(final["metrics"]),
    )

    passed = sum(1 for spec in specifications if spec.status == "pass")
    print()
    print(f"Circuit : {circuit}")
    print(f"Run     : {run_dir}")
    print(
        f"State   : {run.state} ({run.sample_count} samples, "
        f"{len(run.trace_names)} signals)"
    )
    print(f"Specs   : {passed}/{len(specifications)} met")
    print()
    print(specifications_table(specifications))
    print()
    print("Artifacts: thoughts.md, specs.md, specs.csv, netlist.sp, "
          "simulation.json, proposal.json, project/")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Let an OpenRouter model design one circuit from circuits.txt, store "
            "it as an ICE schematic, simulate it, and write the specification "
            "table and the agent transcript."
        ),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--circuit",
        metavar="NUMBER|NAME",
        help="circuit to design; omit to choose from a numbered list",
    )
    parser.add_argument(
        "--list", action="store_true", help="print the circuit catalogue and exit"
    )
    parser.add_argument(
        "--api-key",
        default=None,
        help=(
            "OpenRouter API key (default: $OPENROUTER_API_KEY, then the "
            f"OPENROUTER_API_KEY value in {ENV_FILE.name})"
        ),
    )
    parser.add_argument(
        "--model", default=DEFAULT_MODEL, help="OpenRouter model slug"
    )
    parser.add_argument(
        "--attempts",
        type=int,
        default=3,
        help="model turns allowed for simulation repairs and spec fixes",
    )
    parser.add_argument(
        "--request-timeout",
        type=float,
        default=180.0,
        help="seconds to wait for one OpenRouter request",
    )
    parser.add_argument(
        "--sim-timeout",
        type=float,
        default=120.0,
        help="seconds to wait for one ngspice simulation",
    )
    parser.add_argument(
        "--runs-dir",
        default=str(DEFAULT_RUNS_DIR),
        help="directory that receives one folder per design run",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    arguments = parser.parse_args(argv)

    circuits = read_circuits()
    if arguments.list:
        for index, name in enumerate(circuits, start=1):
            print(f"{index:3d}. {name}")
        return 0
    if not circuits:
        parser.error(f"{CIRCUIT_LIST} contains no circuits")

    arguments.api_key = load_api_key(arguments.api_key)
    if not arguments.api_key:
        print(
            "error: no OpenRouter API key. Put OPENROUTER_API_KEY=... in "
            f"{ENV_FILE}, export OPENROUTER_API_KEY, or pass --api-key.",
            file=sys.stderr,
        )
        return 2
    if shutil.which(ICE_CLI) is None:
        print(
            f"error: {ICE_CLI!r} was not found. Build ICE or set ICE_CLI.",
            file=sys.stderr,
        )
        return 2
    if arguments.attempts < 1:
        parser.error("--attempts must be at least 1")

    circuit = choose_circuit(circuits, arguments.circuit)
    return generate(circuit, arguments)


if __name__ == "__main__":
    raise SystemExit(main())
