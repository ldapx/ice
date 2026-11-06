#!/usr/bin/env python3
"""Generate agent-authored analogue and mixed-signal training data with ICE.

The script reads tasks from ``creation_tasks.csv``, asks a language model served
by OpenRouter for a complete ngspice netlist for the task's circuit and target
PDK, stores that netlist as an ICE text-SPICE schematic, simulates it, and
writes an openable ICE project plus two scores:

- ``result.json`` — specification closeness and tool/thinking efficiency for
  the task;
- ``thoughts.json`` — every model turn, reasoning, ICE tool call, result, and
  the final task result;
- ``netlist.sp``, ``simulation.json``, ``proposal.json`` and ``project/`` —
  the schematic, raw simulated run, final answer, and openable ICE project.

By default the script drives ``ice-mcp`` over stdio and exposes its tools to
the OpenRouter model. The model can create the text schematic, configure and
run the simulation, and inspect the result. If it returns the proposal without
calling any tools, the harness creates and simulates that proposal through
``ice-mcp`` so every task still exercises the MCP server. Pass
``--transport ice-cli`` to use the older direct ``ice-cli`` path.

Usage::

    export OPENROUTER_API_KEY=sk-or-...
    python3 training_data/training_agent.py                 # prompts for key/model/tasks
    python3 training_data/training_agent.py --task 12 --count 3
    python3 training_data/training_agent.py --task "Sallen-Key low-pass filter"
    python3 training_data/training_agent.py --all
    python3 training_data/training_agent.py --transport ice-cli

ICE applies the target PDK setup automatically. The model is instructed to use
the target PDK's devices and transient-testable stimuli.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass, field
import getpass
from datetime import datetime
import json
import math
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from typing import Any, Mapping, Sequence

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent
TASK_LIST = HERE / "creation_tasks.csv"
ENV_FILE = HERE / ".env"
DEFAULT_RUNS_DIR = HERE / "runs"
ICE_CLI = os.environ.get("ICE_CLI", "ice-cli")
ICE_MCP = os.environ.get("ICE_MCP", "ice-mcp")
OPENROUTER_URL = "https://openrouter.ai/api/v1/chat/completions"
DEFAULT_MODEL = os.environ.get(
    "OPENROUTER_MODEL", "nvidia/nemotron-3-ultra-550b-a55b:free"
)
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

_NUMBER = re.compile(r"^([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)\s*([a-zA-Z]*)$")


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


def specification_score(spec: SpecResult) -> float | None:
    """Return a 0-100 closeness score for one measured specification."""
    if spec.target is None or not str(spec.target).strip():
        return None
    if spec.measured is None:
        return 0.0
    if spec.status == "pass":
        return 100.0
    operator, limit = parse_target(str(spec.target))
    measured = spec.measured
    if not math.isfinite(measured):
        return 0.0
    if operator in {">=", ">"} and limit > 0 and measured >= 0:
        return max(0.0, min(100.0, 100.0 * measured / limit))
    if operator in {"<=", "<"} and limit > 0 and measured > 0:
        return max(0.0, min(100.0, 100.0 * limit / measured))
    scale = max(abs(measured), abs(limit), 1e-30)
    return max(0.0, 100.0 * (1.0 - abs(measured - limit) / scale))


def specification_closeness(specifications: Sequence[SpecResult]) -> float | None:
    scores = [
        score
        for spec in specifications
        if (score := specification_score(spec)) is not None
    ]
    if not scores:
        return None
    return sum(scores) / len(scores)


# ---------------------------------------------------------------------------
# Circuit catalogue
# ---------------------------------------------------------------------------


def load_api_key(explicit: str | None = None) -> str:
    """Resolve the key from --api-key, the environment, .env, or a prompt."""
    if explicit and explicit.strip():
        return explicit.strip()
    from_environment = os.environ.get("OPENROUTER_API_KEY", "").strip()
    if not from_environment and ENV_FILE.is_file():
        for line in ENV_FILE.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            name, value = line.split("=", 1)
            if name.strip() in {"OPENROUTER_API_KEY", "OPENROUTER_KEY"}:
                key = value.strip().strip("\"'").strip()
                if key:
                    from_environment = key
                    break
    if sys.stdin.isatty():
        hint = " [Enter to use the configured key]" if from_environment else ""
        entered = getpass.getpass(f"OpenRouter API key{hint}: ").strip()
        if entered:
            return entered
    return from_environment


def choose_model(explicit: str | None = None) -> str:
    """Resolve the OpenRouter model, prompting in interactive sessions."""
    if explicit and explicit.strip():
        return explicit.strip()
    default = DEFAULT_MODEL
    if sys.stdin.isatty():
        entered = input(f"OpenRouter model [{default}]: ").strip()
        if entered:
            return entered
    return default


@dataclass(frozen=True)
class Task:
    task_id: str
    circuit_type: str
    circuit_name: str
    pdk: str
    specifications: str
    prompt: str


def read_tasks(path: Path = TASK_LIST) -> list[Task]:
    """Read the creation task list generated from circuits.csv."""
    tasks: list[Task] = []
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        required = {
            "task_id",
            "circuit_type",
            "circuit_name",
            "pdk",
            "specifications",
            "prompt",
        }
        if not required.issubset(reader.fieldnames or []):
            raise ValueError(
                f"{path} is missing columns: {sorted(required - set(reader.fieldnames or []))}"
            )
        for row in reader:
            if not row.get("task_id", "").strip():
                continue
            tasks.append(
                Task(
                    task_id=row["task_id"].strip(),
                    circuit_type=row["circuit_type"].strip(),
                    circuit_name=row["circuit_name"].strip(),
                    pdk=row["pdk"].strip(),
                    specifications=row["specifications"].strip(),
                    prompt=row["prompt"].strip(),
                )
            )
    return tasks


def print_task_list(tasks: Sequence[Task]) -> None:
    for index, task in enumerate(tasks, start=1):
        print(f"{index:3d}. {task.circuit_name} [{task.pdk}]  {task.task_id}")


def resolve_task_index(tasks: Sequence[Task], selection: str) -> int:
    """Resolve an index, task id, circuit name, or unique substring."""
    text = selection.strip()
    if not text:
        raise ValueError("empty task selection")
    if text.isdigit():
        index = int(text)
        if 1 <= index <= len(tasks):
            return index - 1
        raise ValueError(f"task number {index} is outside 1-{len(tasks)}")
    lowered = text.lower()
    for index, task in enumerate(tasks):
        if task.task_id.lower() == lowered:
            return index
    for index, task in enumerate(tasks):
        if task.circuit_name.lower() == lowered:
            return index
    matches = [
        (index, task)
        for index, task in enumerate(tasks)
        if lowered in task.task_id.lower() or lowered in task.circuit_name.lower()
    ]
    if len(matches) == 1:
        return matches[0][0]
    if not matches:
        raise ValueError(f"no task matches {text!r}")
    listing = "\n".join(
        f"  - {index + 1}: {task.task_id}" for index, task in matches[:20]
    )
    raise ValueError(f"that selection is ambiguous. Candidates:\n{listing}")


def choose_tasks(tasks: Sequence[Task], arguments: argparse.Namespace) -> list[Task]:
    """Resolve an interactive or CLI task range."""
    if arguments.all:
        return list(tasks)
    if arguments.task is not None or arguments.count is not None:
        start = resolve_task_index(tasks, arguments.task or "1")
        count_text = (arguments.count or "1").lower()
        count = len(tasks) - start if count_text == "all" else int(count_text)
        if count < 1:
            raise ValueError("task count must be at least 1")
        return list(tasks[start : start + count])

    if not sys.stdin.isatty():
        raise ValueError(
            "No task selection and stdin is not interactive. "
            "Pass --task <number|task-id|circuit> and --count <N|all>, or --all."
        )

    print("\nAvailable creation tasks:")
    print_task_list(tasks)
    answer = input(f"\nRun all {len(tasks)} tasks? [all/N]: ").strip().lower()
    if answer in {"all", "a", "yes", "y"}:
        return list(tasks)

    count_text = input("How many tasks should be run? [1]: ").strip() or "1"
    if count_text.lower() == "all":
        count = len(tasks)
    else:
        count = int(count_text)
    if count < 1:
        raise ValueError("task count must be at least 1")
    start_text = (
        input(
            "Which task should be run first? [number/task-id/circuit, default 1]: "
        ).strip()
        or "1"
    )
    start = resolve_task_index(tasks, start_text)
    selected = list(tasks[start : start + count])
    print(f"Selected {len(selected)} task(s), starting at {tasks[start].task_id}.")
    return selected


# ---------------------------------------------------------------------------
# Transcript
# ---------------------------------------------------------------------------


class Transcript:
    """JSON record of the agent's reasoning and every tool result."""

    def __init__(self, path: Path, title: str, metadata: Mapping[str, Any]):
        self.path = path
        self.title = title
        self.metadata = {
            key: str(value) if isinstance(value, Path) else value
            for key, value in metadata.items()
            if value not in (None, "")
        }
        self.events: list[dict[str, Any]] = []
        self.final_result: dict[str, Any] | None = None
        self.tool_calls = 0
        self.tool_failures = 0
        self.model_turns = 0
        self.invalid_replies = 0
        self.answer_chars = 0
        self.reasoning_chars = 0
        self._write()

    def _write(self) -> None:
        document: dict[str, Any] = {
            "title": self.title,
            "metadata": self.metadata,
            "counters": {
                "tool_calls": self.tool_calls,
                "tool_failures": self.tool_failures,
                "model_turns": self.model_turns,
                "invalid_replies": self.invalid_replies,
                "answer_chars": self.answer_chars,
                "reasoning_chars": self.reasoning_chars,
            },
            "events": self.events,
        }
        if self.final_result is not None:
            document["result"] = self.final_result
        self.path.write_text(json.dumps(document, indent=2, ensure_ascii=False) + "\n")

    def flush(self) -> None:
        self._write()

    def set_final(self, result: Mapping[str, Any]) -> None:
        self.final_result = dict(result)
        self._write()

    def section(self, heading: str, body: str = "") -> None:
        self.events.append({"type": "section", "title": heading, "body": body})
        self._write()

    def detail(self, heading: str, body: str = "") -> None:
        self.events.append({"type": "detail", "title": heading, "body": body})
        self._write()

    def reasoning(self, text: str) -> None:
        self.events.append({"type": "reasoning", "text": text.strip() or None})
        self._write()

    def assistant(self, text: str) -> None:
        self.events.append({"type": "assistant", "text": text.strip()})
        self._write()

    def ice(
        self,
        command: Sequence[str],
        status: int,
        stdout: str,
        stderr: str,
        elapsed: float,
    ) -> None:
        self.tool_calls += 1
        if status != 0:
            self.tool_failures += 1
        self.events.append(
            {
                "type": "ice",
                "command": [str(part) for part in command],
                "status": status,
                "stdout": stdout[-4000:],
                "stderr": stderr[-4000:],
                "elapsed": elapsed,
            }
        )
        self._write()

    def mcp(
        self,
        tool: str,
        arguments: Mapping[str, Any],
        ok: bool,
        result: str,
        elapsed: float,
        source: str,
    ) -> None:
        self.tool_calls += 1
        if not ok:
            self.tool_failures += 1
        self.events.append(
            {
                "type": "mcp",
                "tool": tool,
                "arguments": dict(arguments),
                "status": "ok" if ok else "error",
                "result": _clip_text(result),
                "elapsed": elapsed,
                "source": source,
            }
        )
        self._write()


# ---------------------------------------------------------------------------
# Efficiency metrics
# ---------------------------------------------------------------------------


@dataclass
class ProcessMetrics:
    tool_calls: int
    tool_failures: int
    model_turns: int
    invalid_replies: int
    tool_efficiency_pct: float
    thinking_efficiency_pct: float
    attempt_efficiency_pct: float
    process_efficiency_pct: float


def process_metrics(transcript: Transcript, completed: bool) -> ProcessMetrics:
    """Score how efficiently the agent used tools and reasoning.

    Tool efficiency rewards successful ICE tool calls; thinking efficiency
    rewards useful answer text over visible reasoning; attempt efficiency
    rewards finishing in fewer model turns. Failed tasks are multiplied by
    0.25 so that a broken run cannot receive a high process score merely for
    making a few successful setup calls.
    """
    tool_efficiency = (
        100.0
        * (transcript.tool_calls - transcript.tool_failures)
        / max(1, transcript.tool_calls)
    )
    reasoning = transcript.reasoning_chars
    answer = transcript.answer_chars
    thinking_efficiency = 100.0 * answer / max(1, answer + reasoning)
    attempt_efficiency = 100.0 / max(1, transcript.model_turns)
    overall = (
        0.4 * tool_efficiency + 0.3 * thinking_efficiency + 0.3 * attempt_efficiency
    )
    if not completed:
        overall *= 0.25
    return ProcessMetrics(
        tool_calls=transcript.tool_calls,
        tool_failures=transcript.tool_failures,
        model_turns=transcript.model_turns,
        invalid_replies=transcript.invalid_replies,
        tool_efficiency_pct=tool_efficiency,
        thinking_efficiency_pct=thinking_efficiency,
        attempt_efficiency_pct=attempt_efficiency,
        process_efficiency_pct=overall,
    )


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
    tool_calls: list[dict[str, Any]] = field(default_factory=list)


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
    messages: Sequence[Mapping[str, Any]],
    timeout: float = 180.0,
    tools: Sequence[Mapping[str, Any]] | None = None,
) -> LlmReply:
    """Send one chat-completion request and return text, reasoning, and tools."""
    payload: dict[str, Any] = {
        "model": model,
        "messages": list(messages),
        "temperature": 0.2,
    }
    if tools:
        payload["tools"] = list(tools)
        payload["tool_choice"] = "auto"
    body = json.dumps(payload).encode()
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
            raise RuntimeError(
                f"OpenRouter rejected the request ({message})"
            ) from error
        except urllib.error.URLError as error:
            last_error = f"connection failed: {error.reason}"
            time.sleep(2.0 * (attempt + 1))
    else:
        raise RuntimeError(f"OpenRouter request failed after 3 attempts ({last_error})")

    choices = document.get("choices")
    if not isinstance(choices, list) or not choices:
        error = document.get("error") or document
        raise RuntimeError("OpenRouter returned no choices: " + json.dumps(error)[:800])
    message = choices[0].get("message") or {}
    reasoning = _content_text(
        message.get("reasoning") or message.get("reasoning_content")
    )
    if not reasoning and isinstance(message.get("reasoning_details"), list):
        reasoning = "\n\n".join(
            item["text"]
            for item in message["reasoning_details"]
            if isinstance(item, dict) and isinstance(item.get("text"), str)
        ).strip()

    tool_calls: list[dict[str, Any]] = []
    raw_tool_calls = message.get("tool_calls")
    if isinstance(raw_tool_calls, list):
        for index, item in enumerate(raw_tool_calls):
            if not isinstance(item, dict):
                continue
            function = item.get("function")
            if not isinstance(function, dict):
                continue
            name = str(function.get("name") or "").strip()
            if not name:
                continue
            arguments = function.get("arguments")
            if isinstance(arguments, dict):
                arguments = json.dumps(arguments, ensure_ascii=False)
            elif not isinstance(arguments, str):
                arguments = "{}"
            tool_calls.append(
                {
                    "id": str(item.get("id") or f"call_{index + 1}"),
                    "type": "function",
                    "function": {"name": name, "arguments": arguments},
                }
            )
    legacy_call = message.get("function_call")
    if not tool_calls and isinstance(legacy_call, dict):
        name = str(legacy_call.get("name") or "").strip()
        arguments = legacy_call.get("arguments")
        if isinstance(arguments, dict):
            arguments = json.dumps(arguments, ensure_ascii=False)
        if name:
            tool_calls.append(
                {
                    "id": "call_1",
                    "type": "function",
                    "function": {
                        "name": name,
                        "arguments": arguments if isinstance(arguments, str) else "{}",
                    },
                }
            )

    return LlmReply(
        content=_content_text(message.get("content")),
        reasoning=reasoning,
        model=str(document.get("model") or model),
        usage=document.get("usage") or {},
        elapsed=time.perf_counter() - started,
        tool_calls=tool_calls,
    )


def extract_json(
    text: str,
    required_keys: set[str] | None = None,
) -> dict[str, Any]:
    """Return the first JSON object embedded in a model reply.

    ``required_keys`` lets callers skip unrelated JSON fragments, such as the
    parameter payload embedded in a plain-text tool call.
    """
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
                        if required_keys is None or required_keys & parsed.keys():
                            return parsed
                    break
        start = text.find("{", start + 1)
    if required_keys is not None:
        raise ProposalError("the reply does not contain a JSON proposal with a netlist")
    raise ProposalError("the reply does not contain a JSON object")


def looks_like_text_tool_call(text: str) -> bool:
    """Detect tool calls written as plain text instead of structured calls."""
    return bool(re.search(r"<tool_call\b|<function\s*=", text, re.IGNORECASE))


def tool_netlist_text(name: str, arguments: Mapping[str, Any]) -> str | None:
    """Return the SPICE source carried by a schematic-writing MCP tool call."""
    kind = str(arguments.get("kind") or "").strip().lower()
    if kind in {"systemverilog", "sv"}:
        return None
    if name in {
        "schematic_create",
        "schematic_create_text",
        "schematic_add_text",
        "schematic_set_text",
    }:
        value = arguments.get("text")
    elif name == "schematic_import_spice":
        value = arguments.get("source")
    else:
        return None
    if isinstance(value, list):
        value = "\n".join(str(line) for line in value)
    return value if isinstance(value, str) and value.strip() else None


def validate_tool_netlist(name: str, arguments: Mapping[str, Any], pdk: str) -> str:
    """Return a designator error for a schematic-writing MCP tool call."""
    text = tool_netlist_text(name, arguments)
    if text is None:
        return ""
    try:
        validate_pdk_designators(text, pdk)
    except ProposalError as error:
        return str(error)
    return ""


# ---------------------------------------------------------------------------
# Proposal parsing
# ---------------------------------------------------------------------------

FORBIDDEN_DIRECTIVES = (
    ".tran",
    ".ac",
    ".dc",
    ".op",
    ".end",
    ".control",
    ".meas",
    ".lib",
    ".include",
)


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
        raise ProposalError('the reply has no "netlist" string')
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
            if directive in {".lib", ".include"}:
                raise ProposalError(
                    f"remove the {directive} directive: ICE applies the target PDK "
                    "model setup automatically"
                )
            raise ProposalError(
                f"remove the {directive} directive: ICE supplies the transient "
                "analysis and computes measurements itself"
            )
    return text + "\n"


def validate_pdk_designators(netlist: str, pdk: str) -> None:
    """Reject M devices that name target-PDK subcircuits.

    PDK transistor names such as sg13_lv_nmos are subcircuits. An M device line
    asks ngspice for a .model with that name, which produces "could not find a
    valid modelname" instead of loading the PDK subcircuit.
    """
    hint = PDK_HINTS.get(pdk)
    if not hint:
        return
    devices = [part.strip() for part in hint[2].split("/") if part.strip()]
    for line in netlist.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith(("*", ".", "X", "x")):
            continue
        if stripped[0] not in {"M", "m"}:
            continue
        for device in devices:
            if re.search(rf"\b{re.escape(device)}\b", stripped, re.IGNORECASE):
                raise ProposalError(
                    f"use the X subcircuit designator for {device}, not M: "
                    f"X1 d g s b {device} w=... l=..."
                )


def parse_proposal(payload: Mapping[str, Any], circuit: str) -> Proposal:
    """Validate a decoded model reply and normalise it into a Proposal."""
    if not isinstance(payload, dict):
        raise ProposalError("the reply must be a JSON object")

    netlist = _netlist_text(payload)

    analysis = payload.get("analysis") or {}
    if not isinstance(analysis, dict):
        raise ProposalError('"analysis" must be an object with stop and step')
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
        raise ProposalError('"signals" must be a non-empty list such as ["v(out)"]')
    signals = [str(item).strip() for item in raw_signals if str(item).strip()]
    if not signals:
        raise ProposalError('"signals" must name at least one signal')
    signals = list(dict.fromkeys(signals))

    raw_specs = payload.get("specifications") or payload.get("specs")
    if not isinstance(raw_specs, list) or len(raw_specs) < 3:
        raise ProposalError(
            '"specifications" must contain at least three measurable specifications'
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
            raise ProposalError(f'specification {metric!r} needs a "trace"')
        reference = entry.get("reference")
        reference = str(reference).strip() if reference else None
        if metric in {"gain", "gain_db"} and not reference:
            raise ProposalError(f'{metric} needs a "reference" trace')
        voltage = None
        if metric == "average_power":
            if entry.get("voltage") is None:
                raise ProposalError('average_power needs a "voltage" value')
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
        return (self.stderr or self.stdout).strip()[
            -4000:
        ] or f"exit status {self.status}"


def _executable_path(name: str) -> str:
    found = shutil.which(name)
    return str(Path(found).resolve()) if found else name


def run_ice(
    arguments: Sequence[Any],
    cwd: Path,
    timeout: float,
    transcript: Transcript,
) -> IceResult:
    command = [_executable_path(ICE_CLI), *[str(argument) for argument in arguments]]
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


# ---------------------------------------------------------------------------
# MCP transport
# ---------------------------------------------------------------------------

MCP_PROTOCOL_VERSION = "2024-11-05"
MCP_RESULT_LIMIT = 12_000


def _clip_text(text: str, limit: int = MCP_RESULT_LIMIT) -> str:
    """Keep the beginning and end of a long tool result."""
    if len(text) <= limit:
        return text
    half = max(1, limit // 2)
    return text[:half] + "\n... truncated ...\n" + text[-half:]


# The model does not need layout, LVS, PEX, or simulation-script tools for the
# circuit-creation tasks. Keeping the exposed set small cuts the tool schema
# cost and makes the workflow easier to follow.
MCP_MODEL_TOOL_ALLOWLIST = {
    "ice_help",
    "ice_doctor",
    "ice_version",
    "pdk_info",
    "project_summary",
    "project_validate",
    "project_undo",
    "project_redo",
    "schematic_create",
    "schematic_create_text",
    "schematic_add_text",
    "schematic_create_systemverilog",
    "schematic_set_text",
    "schematic_delete",
    "schematic_list",
    "schematic_info",
    "schematic_validate",
    "schematic_netlist",
    "schematic_add_component",
    "schematic_add_circuit",
    "schematic_import_spice",
    "schematic_list_symbols",
    "schematic_add_symbol",
    "schematic_add_pin",
    "schematic_add_net",
    "schematic_add_instance",
    "schematic_set_parameter",
    "simulation_create",
    "simulation_update",
    "simulation_run",
    "simulation_result",
    "simulation_trace",
    "simulation_signals",
    "simulation_measure",
}

MCP_REQUIRED_TOOLS = {
    "project_create",
    "schematic_create_text",
    "schematic_info",
    "simulation_create",
    "simulation_run",
}


class MCPError(RuntimeError):
    """Raised when an ice-mcp request cannot be completed."""


@dataclass
class MCPToolResult:
    tool: str
    text: str
    payload: Any | None
    is_error: bool
    elapsed: float

    @property
    def ok(self) -> bool:
        return not self.is_error

    @property
    def error_text(self) -> str:
        if isinstance(self.payload, dict):
            message = str(self.payload.get("error") or "").strip()
            if message:
                return message
        return self.text.strip() or f"{self.tool} failed"


class MCPClient:
    """Minimal newline-delimited JSON-RPC client for the ice-mcp server."""

    def __init__(
        self,
        command: Sequence[str],
        cwd: Path,
        log_path: Path,
        startup_timeout: float = 60.0,
    ):
        self.command = [str(part) for part in command]
        self.cwd = cwd
        self.log_path = log_path
        self._next_id = 0
        self._messages: queue.Queue[dict[str, Any] | None] = queue.Queue()
        self._closed = False
        self._log_handle = log_path.open("w", encoding="utf-8")
        try:
            self.process = subprocess.Popen(
                self.command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=self._log_handle,
                text=True,
                bufsize=1,
                cwd=str(cwd),
            )
        except FileNotFoundError as error:
            self._log_handle.close()
            raise MCPError(
                f"could not start {self.command[0]!r}; build ICE or set ICE_MCP"
            ) from error
        except Exception:
            self._log_handle.close()
            raise
        self._reader = threading.Thread(target=self._read_stdout, daemon=True)
        self._reader.start()
        try:
            self.initialize(timeout=startup_timeout)
        except Exception:
            self.close()
            raise

    def _read_stdout(self) -> None:
        assert self.process.stdout is not None
        while True:
            try:
                line = self.process.stdout.readline()
            except (OSError, ValueError):
                self._messages.put(None)
                return
            if not line:
                self._messages.put(None)
                return
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                self._messages.put({"__invalid__": line[:1000]})
                continue
            if isinstance(message, dict):
                self._messages.put(message)

    def _send(self, message: Mapping[str, Any]) -> None:
        if self.process.stdin is None:
            raise MCPError("ice-mcp stdin is closed")
        try:
            self.process.stdin.write(json.dumps(message, ensure_ascii=False) + "\n")
            self.process.stdin.flush()
        except (BrokenPipeError, OSError, ValueError) as error:
            raise MCPError(self._closed_message()) from error

    def _closed_message(self) -> str:
        tail = ""
        try:
            tail = self.log_path.read_text(encoding="utf-8", errors="replace")[
                -4000:
            ].strip()
        except OSError:
            pass
        message = "ice-mcp closed unexpectedly"
        if tail:
            message += f":\n{tail}"
        return message

    def _request(self, method: str, params: Mapping[str, Any], timeout: float) -> Any:
        self._next_id += 1
        request_id = self._next_id
        self._send(
            {
                "jsonrpc": "2.0",
                "id": request_id,
                "method": method,
                "params": dict(params),
            }
        )
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise MCPError(f"ice-mcp {method} timed out after {timeout:.0f}s")
            try:
                message = self._messages.get(timeout=remaining)
            except queue.Empty as error:
                raise MCPError(
                    f"ice-mcp {method} timed out after {timeout:.0f}s"
                ) from error
            if message is None:
                raise MCPError(self._closed_message())
            if "__invalid__" in message:
                raise MCPError(f"ice-mcp wrote invalid JSON: {message['__invalid__']}")
            if message.get("id") != request_id:
                # Notifications and stale responses are ignored.
                continue
            if "error" in message:
                error = message["error"]
                if isinstance(error, dict):
                    raise MCPError(str(error.get("message") or json.dumps(error)))
                raise MCPError(str(error))
            return message.get("result")

    def initialize(self, timeout: float = 60.0) -> Mapping[str, Any]:
        result = self._request(
            "initialize",
            {
                "protocolVersion": MCP_PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": "ice-training-agent", "version": "1"},
            },
            timeout,
        )
        self._send(
            {
                "jsonrpc": "2.0",
                "method": "notifications/initialized",
                "params": {},
            }
        )
        return result if isinstance(result, dict) else {}

    def list_tools(self, timeout: float = 60.0) -> list[dict[str, Any]]:
        result = self._request("tools/list", {}, timeout)
        tools = result.get("tools") if isinstance(result, dict) else None
        return [tool for tool in tools or [] if isinstance(tool, dict)]

    def call_tool(
        self, name: str, arguments: Mapping[str, Any], timeout: float = 180.0
    ) -> MCPToolResult:
        started = time.perf_counter()
        result = self._request(
            "tools/call",
            {"name": name, "arguments": dict(arguments)},
            timeout,
        )
        elapsed = time.perf_counter() - started
        text = ""
        payload: Any | None = None
        is_error = False
        if isinstance(result, dict):
            is_error = bool(result.get("isError"))
            parts: list[str] = []
            content = result.get("content")
            if isinstance(content, list):
                for item in content:
                    if isinstance(item, str):
                        parts.append(item)
                    elif isinstance(item, dict) and isinstance(item.get("text"), str):
                        parts.append(item["text"])
            text = "\n".join(parts).strip()
            if not text and "structuredContent" in result:
                payload = result["structuredContent"]
                text = json.dumps(payload, ensure_ascii=False)
            else:
                try:
                    payload = json.loads(text) if text else None
                except json.JSONDecodeError:
                    payload = None
        return MCPToolResult(name, text, payload, is_error, elapsed)

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        process = getattr(self, "process", None)
        if process is not None:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
        reader = getattr(self, "_reader", None)
        if reader is not None and reader.is_alive():
            reader.join(timeout=2)
        if not self._log_handle.closed:
            self._log_handle.flush()
            self._log_handle.close()

    def __enter__(self) -> "MCPClient":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()


def _normalise_tool_schema(value: Any) -> Any:
    """Make MCP JSON schemas acceptable to OpenAI-compatible tool calling."""
    if isinstance(value, list):
        return [_normalise_tool_schema(item) for item in value]
    if not isinstance(value, dict):
        return value
    schema = {
        key: _normalise_tool_schema(item)
        for key, item in value.items()
        if key != "type" or not isinstance(item, list)
    }
    types = value.get("type")
    if isinstance(types, list):
        preferred = next(
            (
                kind
                for kind in (
                    "string",
                    "integer",
                    "number",
                    "boolean",
                    "array",
                    "object",
                )
                if kind in types
            ),
            types[0] if types else "string",
        )
        schema["type"] = preferred
    return schema


def openrouter_tools(
    tools: Sequence[Mapping[str, Any]],
    allowlist: set[str] | None = None,
) -> list[dict[str, Any]]:
    """Convert MCP tool definitions to OpenRouter function tools."""
    converted: list[dict[str, Any]] = []
    for tool in tools:
        name = str(tool.get("name") or "").strip()
        if not name or (allowlist is not None and name not in allowlist):
            continue
        schema = tool.get("inputSchema")
        if not isinstance(schema, dict):
            schema = {"type": "object", "properties": {}}
        schema = _normalise_tool_schema(schema)
        if schema.get("type") != "object":
            if isinstance(schema.get("properties"), dict):
                schema["type"] = "object"
            else:
                schema = {"type": "object", "properties": {}}
        schema.setdefault("properties", {})
        schema.setdefault("required", [])
        converted.append(
            {
                "type": "function",
                "function": {
                    "name": name,
                    "description": str(tool.get("description") or ""),
                    "parameters": schema,
                },
            }
        )
    return converted


def _coerce_tool_arguments(name: str, arguments: Mapping[str, Any]) -> dict[str, Any]:
    """Adapt common model mistakes to the MCP tool schemas."""
    converted = dict(arguments)
    if name in {"simulation_create", "simulation_update"}:
        for key in ("stop", "step"):
            value = converted.get(key)
            if isinstance(value, str):
                try:
                    converted[key] = parse_value(value, key)
                except ValueError:
                    pass
        signals = converted.get("signals")
        if isinstance(signals, list):
            converted["signals"] = ",".join(str(item) for item in signals)
    if name in {
        "schematic_create",
        "schematic_create_text",
        "schematic_add_text",
        "schematic_set_text",
    }:
        text = converted.get("text")
        if isinstance(text, list):
            converted["text"] = "\n".join(str(line) for line in text)
    if name == "simulation_run" and "timeout_ms" in converted:
        try:
            converted["timeout_ms"] = int(float(converted["timeout_ms"]))
        except (TypeError, ValueError):
            pass
    return converted


def _mcp_tool_timeout(name: str, simulation_timeout: float) -> float:
    if name == "simulation_run":
        return simulation_timeout + 60.0
    if name == "pdk_build":
        return 600.0
    return 180.0


def call_mcp_tool(
    client: MCPClient,
    name: str,
    arguments: Mapping[str, Any],
    transcript: Transcript,
    *,
    source: str,
    timeout: float = 180.0,
) -> MCPToolResult:
    """Call an MCP tool, record it, and convert protocol failures to results."""
    try:
        result = client.call_tool(name, arguments, timeout=timeout)
    except MCPError as error:
        message = str(error)
        result = MCPToolResult(
            name,
            json.dumps({"error": message}),
            {"error": message},
            True,
            0.0,
        )
    transcript.mcp(
        name,
        arguments,
        result.ok,
        result.text or json.dumps(result.payload, ensure_ascii=False),
        result.elapsed,
        source,
    )
    return result


def _mcp_stored_path(payload: Mapping[str, Any], run_dir: Path) -> Path | None:
    value = payload.get("stored")
    if not value:
        return None
    stored = Path(str(value))
    if not stored.is_absolute():
        stored = run_dir / stored
    return stored.resolve()


def _normalise_netlist(text: str) -> str:
    return "\n".join(
        line.strip() for line in text.replace("\r\n", "\n").splitlines() if line.strip()
    )


@dataclass
class SimulationAttempt:
    ok: bool
    schematic: str
    simulation: str
    stored: Path | None
    error: str
    elapsed: float
    source_netlist: str | None = None
    pdk: str = ""


def simulate_design(
    project_dir: Path,
    netlist_path: Path,
    proposal: Proposal,
    attempt: int,
    transcript: Transcript,
    simulation_timeout: float,
    pdk: str,
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
            pdk,
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


# ---------------------------------------------------------------------------
# Agent loop
# ---------------------------------------------------------------------------

PDK_HINTS = {
    "ihp-sg13g2": ("IHP SG13G2", "1.2", "sg13_lv_nmos / sg13_lv_pmos"),
    "sky130A": (
        "SkyWater 130nm",
        "1.8",
        "sky130_fd_pr__nfet_01v8 / sky130_fd_pr__pfet_01v8",
    ),
    "gf180mcuA": ("GlobalFoundries 180nm MCU", "3.3", "nfet_03v3 / pfet_03v3"),
}


def build_system_prompt(task: Task, mcp: bool = False) -> str:
    display_name, supply, devices = PDK_HINTS.get(
        task.pdk, (task.pdk, "the PDK nominal value", "the target PDK devices")
    )
    tool_workflow = ""
    if mcp:
        tool_workflow = f"""
MCP TOOL WORKFLOW
The ICE project is already open. You may use the ice-mcp tools to create the
text schematic, configure and run the simulation, and inspect the result:
- schematic_create_text(name=..., pdk="{task.pdk}", text=<netlist>)
- simulation_create(name=..., schematic=..., template="tran", stop=<seconds>,
  step=<seconds>, signals="v(out),v(in)")
- simulation_run(simulation=..., timeout_ms=...)
- simulation_result, simulation_trace, simulation_measure, schematic_validate,
  ice_doctor, pdk_info for diagnosis.
Use a new schematic and simulation name for each design attempt. Tool arguments
stop, step, and timeout_ms are numbers, not strings with SPICE suffixes. After a
successful simulation, return the JSON object below with the exact netlist you
simulated. If you do not call the tools, the harness creates and runs the
returned proposal through ice-mcp for you.

"""
    return f"""You are an analogue and mixed-signal circuit design agent.
ICE (an open-source schematic/layout/simulation tool) will store your netlist as
a text-SPICE schematic and simulate it with ngspice.

TASK
Circuit: {task.circuit_name}
Circuit type: {task.circuit_type}
Target PDK: {display_name} ({task.pdk}); nominal supply {supply} V.
PDK devices: {devices}. ICE applies the PDK model library, OSDI setup and
corner selection automatically. Do not add .lib or .include lines yourself.
Desired specifications: {task.specifications}

Design constraints:
1. Return one complete, self-contained SPICE netlist that ngspice can run.
2. Use devices from the target PDK for active devices. Ideal sources, resistors,
   capacitors and dependent sources (E, G, F, H) are allowed for stimuli and
   behavioural blocks. Do not use generic MOS .model cards when a target PDK
   device is appropriate.
3. Node 0 is ground. Every node needs a DC path to ground and a parallel
   capacitance when that is needed for convergence.
4. Do not include .tran, .ac, .dc, .op, .end, .control or .meas cards: ICE
   supplies the transient analysis and computes measurements from the waveforms.
5. Stimuli must be transient-friendly: SIN, PULSE, PWL or EXP. Oscillators need
   a non-zero initial condition (e.g. `.ic v(out)=0.1`) or a deliberate
   mismatch so they start.
6. Keep the run cheap: stop 100u-10m, step 10n-10u, at most 400000 points.
7. List the nodes to observe in `signals` using `v(node)` or `i(device)`.
8. Propose 3-8 measurable specifications. Treat the desired specifications above
   as authoritative: copy the same targets where possible and use the closest
   measurable proxy when a desired metric is not supported. Do not relax or
   silently drop targets; explain any proxy in design_notes.
   The available metrics are:
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
9. Use realistic values for the target PDK, resistors, capacitors and currents,
   and design so that the targets are actually met. Choose a stimulus and time
   window that let the specification be measured.
10. If a target cannot be met with a sensible design, say so in design_notes
    instead of stating an unreachable target.

{tool_workflow}Reply with exactly one JSON object and no other text:
{{
  "circuit": "the requested circuit name",
  "topology_summary": "one or two sentences on the topology and sizing",
  "design_notes": "trade-offs, assumptions, expected behaviour",
  "netlist": "* title line\\nvin in 0 0 SIN(0 1 1k)\\n...",
  "analysis": {{"stop": "2m", "step": "1u"}},
  "signals": ["v(out)", "v(in)"],
  "specifications": [
    {{"name": "Midband gain", "metric": "gain_db", "trace": "v(out)",
     "reference": "v(in)", "target": ">= 20", "unit": "dB"}},
    {{"name": "Settling time", "metric": "settling_time", "trace": "v(out)",
     "target": "<= 500u", "unit": "s"}}
  ]
}}
Targets use >=, <=, = or ~ (5% band) followed by a value with a SPICE suffix
(k, meg, m, u, n, p)."""


@dataclass
class TaskResult:
    task_id: str
    circuit_name: str
    circuit_type: str
    pdk: str
    status: str
    spec_closeness_pct: float
    process_efficiency_pct: float
    tool_efficiency_pct: float
    thinking_efficiency_pct: float
    specs_met: int
    specs_total: int
    model_turns: int
    tool_calls: int
    tool_failures: int
    run_dir: Path
    project: Path
    transport: str = "ice-cli"
    error: str = ""

    def as_document(self) -> dict[str, Any]:
        return {
            "task_id": self.task_id,
            "circuit_name": self.circuit_name,
            "circuit_type": self.circuit_type,
            "pdk": self.pdk,
            "status": self.status,
            "transport": self.transport,
            "spec_closeness_pct": round(self.spec_closeness_pct, 1),
            "process_efficiency_pct": round(self.process_efficiency_pct, 1),
            "tool_efficiency_pct": round(self.tool_efficiency_pct, 1),
            "thinking_efficiency_pct": round(self.thinking_efficiency_pct, 1),
            "specs_met": self.specs_met,
            "specs_total": self.specs_total,
            "model_turns": self.model_turns,
            "tool_calls": self.tool_calls,
            "tool_failures": self.tool_failures,
            "run_dir": str(self.run_dir),
            "project": str(self.project),
            "error": self.error,
        }


def write_task_result(run_dir: Path, result: TaskResult) -> None:
    (run_dir / "result.json").write_text(
        json.dumps(result.as_document(), indent=2) + "\n"
    )


def _mcp_project_names(
    client: MCPClient, transcript: Transcript
) -> tuple[set[str], set[str]]:
    """Return the schematic and simulation names that are already in the project."""
    result = call_mcp_tool(
        client, "project_summary", {}, transcript, source="harness", timeout=60.0
    )
    payload = result.payload if isinstance(result.payload, dict) else {}
    schematics = {
        str(item.get("name"))
        for item in payload.get("schematics") or []
        if isinstance(item, dict) and item.get("name")
    }
    simulations = {
        str(item.get("name"))
        for item in payload.get("simulations") or []
        if isinstance(item, dict) and item.get("name")
    }
    return schematics, simulations


def mcp_simulate_design(
    client: MCPClient,
    proposal: Proposal,
    attempt: int,
    transcript: Transcript,
    simulation_timeout: float,
    pdk: str,
    run_dir: Path,
) -> SimulationAttempt:
    """Create and simulate the proposal through ice-mcp tools."""
    schematics, simulations = _mcp_project_names(client, transcript)
    schematic = f"design_a{attempt}_harness"
    while schematic in schematics:
        schematic += "_x"
    simulation = f"sim_a{attempt}_harness"
    while simulation in simulations:
        simulation += "_x"

    created = call_mcp_tool(
        client,
        "schematic_create_text",
        {"name": schematic, "pdk": pdk, "text": proposal.netlist},
        transcript,
        source="harness",
        timeout=120.0,
    )
    if not created.ok:
        return SimulationAttempt(
            False, schematic, simulation, None, created.error_text, created.elapsed
        )

    try:
        stop = parse_value(proposal.stop, "analysis.stop")
        step = parse_value(proposal.step, "analysis.step")
    except ValueError as error:
        return SimulationAttempt(
            False, schematic, simulation, None, str(error), created.elapsed
        )

    configured = call_mcp_tool(
        client,
        "simulation_create",
        {
            "name": simulation,
            "schematic": schematic,
            "template": "tran",
            "analysis": "transient",
            "stop": stop,
            "step": step,
            "signals": ",".join(proposal.signals),
        },
        transcript,
        source="harness",
        timeout=120.0,
    )
    if not configured.ok:
        return SimulationAttempt(
            False,
            schematic,
            simulation,
            None,
            configured.error_text,
            configured.elapsed,
        )

    executed = call_mcp_tool(
        client,
        "simulation_run",
        {
            "simulation": simulation,
            "timeout_ms": int(simulation_timeout * 1000),
        },
        transcript,
        source="harness",
        timeout=simulation_timeout + 60.0,
    )
    if not executed.ok:
        return SimulationAttempt(
            False, schematic, simulation, None, executed.error_text, executed.elapsed
        )

    payload = executed.payload if isinstance(executed.payload, dict) else {}
    state = str(payload.get("state") or "")
    stored = _mcp_stored_path(payload, run_dir)
    if state != "succeeded" or stored is None:
        error = str(payload.get("error") or executed.error_text)
        return SimulationAttempt(
            False, schematic, simulation, stored, error, executed.elapsed
        )
    if not stored.is_file():
        return SimulationAttempt(
            False,
            schematic,
            simulation,
            stored,
            f"simulation artifact was not written: {stored}",
            executed.elapsed,
        )
    return SimulationAttempt(True, schematic, simulation, stored, "", executed.elapsed)


def _model_run_matches_proposal(
    client: MCPClient,
    model_run: SimulationAttempt,
    proposal: Proposal,
    transcript: Transcript,
    pdk: str,
) -> tuple[bool, str]:
    """Check that the model simulated the returned netlist on the target PDK."""
    if not model_run.ok or not model_run.schematic:
        return False, "the model did not provide a successful MCP simulation"
    cell_pdk = model_run.pdk
    source = model_run.source_netlist
    if source is None:
        info = call_mcp_tool(
            client,
            "schematic_info",
            {"schematic": model_run.schematic},
            transcript,
            source="harness",
            timeout=120.0,
        )
        if not info.ok or not isinstance(info.payload, dict):
            return False, f"could not inspect the model's schematic: {info.error_text}"
        cell_pdk = str(info.payload.get("pdk") or "")
        source = str(info.payload.get("source") or info.payload.get("text") or "")
    if not cell_pdk:
        return False, "could not determine the model simulation's PDK"
    if cell_pdk != pdk:
        return False, (
            f"the model's simulation used PDK {cell_pdk!r} instead of {pdk!r}"
        )
    if _normalise_netlist(source) == _normalise_netlist(proposal.netlist):
        return True, ""
    return False, "the model's simulated netlist differs from the returned proposal"


def _simulation_attempt_from_payload(
    result: MCPToolResult, run_dir: Path
) -> SimulationAttempt | None:
    payload = result.payload if isinstance(result.payload, dict) else {}
    state = str(payload.get("state") or "")
    stored = _mcp_stored_path(payload, run_dir)
    if state != "succeeded" or stored is None or not stored.is_file():
        return None
    return SimulationAttempt(
        True,
        str(payload.get("schematic") or ""),
        str(payload.get("simulation") or ""),
        stored,
        "",
        result.elapsed,
    )


def _tool_result_content(result: MCPToolResult) -> str:
    text = result.text.strip()
    if not text and result.payload is not None:
        text = json.dumps(result.payload, ensure_ascii=False)
    return _clip_text(text) or "{}"


def execute_proposal_via_mcp(
    client: MCPClient,
    proposal: Proposal,
    model_run: SimulationAttempt | None,
    attempt: int,
    transcript: Transcript,
    simulation_timeout: float,
    pdk: str,
    run_dir: Path,
) -> SimulationAttempt:
    """Prefer a matching model-run simulation; otherwise run the proposal."""
    if model_run is not None:
        matches, reason = _model_run_matches_proposal(
            client, model_run, proposal, transcript, pdk
        )
        if matches:
            transcript.section(
                "Simulation source",
                "Using the simulation the model created and ran through ice-mcp.",
            )
            return model_run
        transcript.section(
            "Model simulation ignored",
            f"{reason}. Running the returned proposal through ice-mcp instead.",
        )
    return mcp_simulate_design(
        client,
        proposal,
        attempt,
        transcript,
        simulation_timeout,
        pdk,
        run_dir,
    )


def _new_run_dir(task: Task, arguments: argparse.Namespace) -> Path:
    runs_dir = Path(arguments.runs_dir).expanduser().resolve()
    base = runs_dir / f"{datetime.now().strftime('%Y%m%d-%H%M%S')}_{task.task_id}"
    run_dir = base
    suffix = 2
    while run_dir.exists():
        run_dir = base.with_name(f"{base.name}-{suffix}")
        suffix += 1
    run_dir.mkdir(parents=True)
    return run_dir


def generate(task: Task, arguments: argparse.Namespace) -> TaskResult:
    run_dir = _new_run_dir(task, arguments)
    if arguments.transport == "mcp":
        client = MCPClient(
            [_executable_path(ICE_MCP)],
            cwd=run_dir,
            log_path=run_dir / "mcp.log",
        )
        try:
            return _generate(task, arguments, run_dir, client)
        finally:
            client.close()
    return _generate(task, arguments, run_dir, None)


def _generate(
    task: Task,
    arguments: argparse.Namespace,
    run_dir: Path,
    client: MCPClient | None,
) -> TaskResult:
    project_dir = run_dir / "project"
    project_path = project_dir / "project.icepro"

    transcript = Transcript(
        run_dir / "thoughts.json",
        f"Agent transcript — {task.task_id}",
        {
            "Task": task.task_id,
            "Circuit": task.circuit_name,
            "Circuit type": task.circuit_type,
            "Target PDK": task.pdk,
            "Desired specifications": task.specifications,
            "Catalogue": TASK_LIST.name,
            "Model": arguments.model,
            "Transport": arguments.transport,
            "MCP server": ICE_MCP if client is not None else None,
            "Started": datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S %Z"),
            "Run directory": run_dir,
        },
    )
    transcript.section("Task", task.prompt)
    system_prompt = build_system_prompt(
        task, mcp=client is not None and not arguments.no_tools
    )
    transcript.detail("System prompt", system_prompt)

    tools: list[dict[str, Any]] = []
    available_names: set[str] = set()
    exposed_names: set[str] = set()
    project_error = ""

    if client is not None:
        try:
            mcp_tools = client.list_tools()
        except MCPError as error:
            mcp_tools = []
            project_error = f"could not list ice-mcp tools: {error}"
        available_names = {
            str(tool.get("name"))
            for tool in mcp_tools
            if isinstance(tool, dict) and tool.get("name")
        }
        missing = MCP_REQUIRED_TOOLS - available_names
        if missing and not project_error:
            project_error = "ice-mcp is missing required tools: " + ", ".join(
                sorted(missing)
            )
        if not project_error:
            version = call_mcp_tool(
                client, "ice_version", {}, transcript, source="harness", timeout=60.0
            )
            if version.ok and isinstance(version.payload, dict):
                fingerprint = ", ".join(
                    f"{key}={version.payload.get(key)}"
                    for key in ("version", "commit", "build_time", "build_type")
                    if version.payload.get(key)
                )
                transcript.detail("ICE MCP", fingerprint or "connected")
            if not arguments.no_tools:
                tools = openrouter_tools(mcp_tools, MCP_MODEL_TOOL_ALLOWLIST)
                exposed_names = {tool["function"]["name"] for tool in tools}
                transcript.detail(
                    "MCP tools",
                    f"{len(tools)} tools exposed to the model from "
                    f"{len(available_names)} available: "
                    + ", ".join(sorted(exposed_names)),
                )
            created = call_mcp_tool(
                client,
                "project_create",
                {"path": project_dir.name, "name": task.task_id},
                transcript,
                source="harness",
                timeout=120.0,
            )
            if not created.ok or not isinstance(created.payload, dict):
                project_error = created.error_text
    else:
        project_created = run_ice(
            ["project-create", project_dir.name, task.task_id],
            cwd=project_dir.parent,
            timeout=120.0,
            transcript=transcript,
        )
        if project_created.status != 0:
            project_error = project_created.error

    if project_error:
        process = process_metrics(transcript, completed=False)
        error = f"could not create the ICE project: {project_error}"
        result = TaskResult(
            task_id=task.task_id,
            circuit_name=task.circuit_name,
            circuit_type=task.circuit_type,
            pdk=task.pdk,
            status="failed",
            spec_closeness_pct=0.0,
            process_efficiency_pct=process.process_efficiency_pct,
            tool_efficiency_pct=process.tool_efficiency_pct,
            thinking_efficiency_pct=process.thinking_efficiency_pct,
            specs_met=0,
            specs_total=0,
            model_turns=process.model_turns,
            tool_calls=process.tool_calls,
            tool_failures=process.tool_failures,
            run_dir=run_dir,
            project=project_path,
            transport=arguments.transport,
            error=error,
        )
        write_task_result(run_dir, result)
        transcript.set_final(result.as_document())
        return result

    if client is not None and not arguments.no_tools:
        user_prompt = (
            task.prompt
            + "\n\nUse the ICE MCP tools to create and simulate the design, "
            "then return the JSON proposal."
        )
    else:
        user_prompt = task.prompt + "\n\nReturn the JSON proposal now."
    messages: list[dict[str, Any]] = [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": user_prompt},
    ]
    transcript.detail("User prompt", user_prompt)

    final: dict[str, Any] | None = None
    last_error = ""
    stop_requests = False

    for attempt in range(1, arguments.attempts + 1):
        transcript.section(f"Attempt {attempt} of {arguments.attempts}")
        model_run: SimulationAttempt | None = None
        got_final = False
        retry_attempt = False
        used_tools = False

        for round_index in range(1, arguments.tool_rounds + 2):
            allow_tools = bool(tools) and round_index <= arguments.tool_rounds
            if tools and round_index == arguments.tool_rounds + 1:
                if used_tools:
                    final_message = (
                        "The tool-call limit for this attempt has been reached. "
                        "Return the final JSON proposal now without calling any "
                        "more tools."
                    )
                    final_reason = "Requesting the final JSON proposal without tools."
                else:
                    final_message = (
                        "Return the final JSON proposal now without calling any "
                        "more tools."
                    )
                    final_reason = "Requesting the final JSON proposal."
                messages.append({"role": "user", "content": final_message})
                transcript.section("Final JSON turn", final_reason)
            transcript.model_turns += 1
            try:
                reply = call_openrouter(
                    arguments.api_key,
                    arguments.model,
                    messages,
                    timeout=arguments.request_timeout,
                    tools=tools if allow_tools else None,
                )
            except RuntimeError as error:
                transcript.section("Model request failed", str(error))
                last_error = str(error)
                stop_requests = True
                break

            transcript.answer_chars += len(reply.content)
            transcript.reasoning_chars += len(reply.reasoning)
            transcript.detail("Model", f"`{reply.model}` in {reply.elapsed:.1f}s")
            transcript.reasoning(reply.reasoning)
            if reply.content:
                transcript.assistant(reply.content)
            if reply.usage:
                transcript.detail(
                    "Token usage",
                    ", ".join(f"{key}={value}" for key, value in reply.usage.items()),
                )

            if reply.tool_calls and allow_tools:
                used_tools = True
                messages.append(
                    {
                        "role": "assistant",
                        "content": reply.content or None,
                        "tool_calls": reply.tool_calls,
                    }
                )
                for call in reply.tool_calls:
                    call_id = str(call.get("id") or "")
                    function = call.get("function") if isinstance(call, dict) else None
                    if not isinstance(function, dict):
                        messages.append(
                            {
                                "role": "tool",
                                "tool_call_id": call_id,
                                "content": json.dumps({"error": "malformed tool call"}),
                            }
                        )
                        continue
                    name = str(function.get("name") or "").strip()
                    raw_arguments = function.get("arguments")
                    if isinstance(raw_arguments, str):
                        try:
                            arguments_object = json.loads(raw_arguments or "{}")
                        except json.JSONDecodeError as error:
                            result = MCPToolResult(
                                name or "unknown",
                                json.dumps(
                                    {"error": f"invalid tool arguments: {error}"}
                                ),
                                {"error": f"invalid tool arguments: {error}"},
                                True,
                                0.0,
                            )
                            transcript.mcp(
                                result.tool, {}, False, result.text, 0.0, "model"
                            )
                            messages.append(
                                {
                                    "role": "tool",
                                    "tool_call_id": call_id,
                                    "content": result.text,
                                }
                            )
                            continue
                    elif isinstance(raw_arguments, dict):
                        arguments_object = raw_arguments
                    else:
                        arguments_object = {}
                    if not isinstance(arguments_object, dict):
                        result = MCPToolResult(
                            name or "unknown",
                            json.dumps(
                                {"error": "tool arguments must be a JSON object"}
                            ),
                            {"error": "tool arguments must be a JSON object"},
                            True,
                            0.0,
                        )
                        transcript.mcp(
                            result.tool, {}, False, result.text, 0.0, "model"
                        )
                    elif not client or name not in exposed_names:
                        result = MCPToolResult(
                            name or "unknown",
                            json.dumps({"error": f"unknown tool: {name}"}),
                            {"error": f"unknown tool: {name}"},
                            True,
                            0.0,
                        )
                        transcript.mcp(
                            result.tool,
                            arguments_object,
                            False,
                            result.text,
                            0.0,
                            "model",
                        )
                    else:
                        arguments_object = _coerce_tool_arguments(
                            name, arguments_object
                        )
                        validation_error = validate_tool_netlist(
                            name, arguments_object, task.pdk
                        )
                        if validation_error:
                            result = MCPToolResult(
                                name,
                                json.dumps({"error": validation_error}),
                                {"error": validation_error},
                                True,
                                0.0,
                            )
                            transcript.mcp(
                                name,
                                arguments_object,
                                False,
                                result.text,
                                0.0,
                                "model",
                            )
                        else:
                            result = call_mcp_tool(
                                client,
                                name,
                                arguments_object,
                                transcript,
                                source="model",
                                timeout=_mcp_tool_timeout(name, arguments.sim_timeout),
                            )
                    messages.append(
                        {
                            "role": "tool",
                            "tool_call_id": call_id,
                            "content": _tool_result_content(result),
                        }
                    )
                    if client is not None and name == "simulation_run" and result.ok:
                        candidate = _simulation_attempt_from_payload(result, run_dir)
                        if candidate is not None:
                            info = call_mcp_tool(
                                client,
                                "schematic_info",
                                {"schematic": candidate.schematic},
                                transcript,
                                source="harness",
                                timeout=120.0,
                            )
                            if info.ok and isinstance(info.payload, dict):
                                candidate.source_netlist = str(
                                    info.payload.get("source")
                                    or info.payload.get("text")
                                    or ""
                                )
                                candidate.pdk = str(info.payload.get("pdk") or "")
                            model_run = candidate
                continue

            messages.append({"role": "assistant", "content": reply.content})
            if reply.tool_calls:
                messages[-1]["tool_calls"] = reply.tool_calls
                for call in reply.tool_calls:
                    messages.append(
                        {
                            "role": "tool",
                            "tool_call_id": str(call.get("id") or ""),
                            "content": json.dumps(
                                {
                                    "error": (
                                        "tools are disabled on this turn; "
                                        "return the final JSON proposal"
                                    )
                                }
                            ),
                        }
                    )
                last_error = "the model kept calling tools on the final turn"
                break

            try:
                proposal = parse_proposal(
                    extract_json(
                        reply.content,
                        required_keys={"netlist", "specifications", "specs"},
                    ),
                    task.circuit_name,
                )
                validate_pdk_designators(proposal.netlist, task.pdk)
            except ProposalError as error:
                transcript.invalid_replies += 1
                if looks_like_text_tool_call(reply.content):
                    last_error = "the model wrote a tool call as plain text"
                    transcript.section(
                        "Tool call not executed",
                        "The model wrote a tool call as plain text instead of "
                        "using the structured tool interface, so it was not "
                        "executed.",
                    )
                    messages.append(
                        {
                            "role": "user",
                            "content": (
                                "Your last message contained a tool call in plain "
                                "text. It was not executed. Use the structured "
                                "tool-calling interface, or return the final JSON "
                                "proposal."
                            ),
                        }
                    )
                    continue
                last_error = str(error)
                transcript.section("Reply rejected", str(error))
                messages.append(
                    {
                        "role": "user",
                        "content": (
                            f"Your reply could not be used: {error}.\n"
                            "Return the corrected JSON object only. Use the tools "
                            "first if you need to inspect or change the project."
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
            transcript.detail("Netlist", proposal.netlist)
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

            if client is not None:
                attempt_result = execute_proposal_via_mcp(
                    client,
                    proposal,
                    model_run,
                    attempt,
                    transcript,
                    arguments.sim_timeout,
                    task.pdk,
                    run_dir,
                )
            else:
                attempt_result = simulate_design(
                    project_dir,
                    netlist_path,
                    proposal,
                    attempt,
                    transcript,
                    arguments.sim_timeout,
                    task.pdk,
                )
            if not attempt_result.ok or attempt_result.stored is None:
                last_error = attempt_result.error or "no simulation artifact was stored"
                transcript.section(
                    "Simulation failed",
                    f"{last_error}\n\nElapsed {attempt_result.elapsed:.1f}s.",
                )
                messages.append(
                    {
                        "role": "user",
                        "content": (
                            "ICE/ngspice could not simulate your netlist:\n"
                            f"{last_error}\n\n"
                            "Fix the netlist (check the target PDK devices, node names, "
                            "model cards, DC paths and the requested signals) and return "
                            "the full JSON again."
                        ),
                    }
                )
                retry_attempt = True
                break

            try:
                run = sim.load(attempt_result.stored)
            except Exception as error:
                last_error = f"could not read the stored simulation: {error}"
                transcript.section("Simulation result failed", last_error)
                messages.append(
                    {
                        "role": "user",
                        "content": (
                            f"{last_error}\n\n"
                            "Return the full JSON proposal again so the harness can "
                            "rerun it."
                        ),
                    }
                )
                retry_attempt = True
                break
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
                + (
                    "\n\n### Warnings\n\n" + "\n".join(f"- {w}" for w in warnings)
                    if warnings
                    else ""
                ),
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
            if not missed or attempt == arguments.attempts:
                got_final = True
                break

            transcript.section(
                "Specifications missed", "Feeding the table back to the model."
            )
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
                        "and return the full JSON again. If you use the tools for the "
                        "next attempt, use new schematic and simulation names."
                    ),
                }
            )
            retry_attempt = True
            break

        if stop_requests or got_final:
            break
        if not retry_attempt:
            last_error = last_error or "the model did not return a usable JSON proposal"
            transcript.section("Attempt ended", last_error)

    process = process_metrics(transcript, completed=final is not None)
    if final is None:
        error = last_error or "no attempt produced a successful simulation"
        result = TaskResult(
            task_id=task.task_id,
            circuit_name=task.circuit_name,
            circuit_type=task.circuit_type,
            pdk=task.pdk,
            status="failed",
            spec_closeness_pct=0.0,
            process_efficiency_pct=process.process_efficiency_pct,
            tool_efficiency_pct=process.tool_efficiency_pct,
            thinking_efficiency_pct=process.thinking_efficiency_pct,
            specs_met=0,
            specs_total=0,
            model_turns=process.model_turns,
            tool_calls=process.tool_calls,
            tool_failures=process.tool_failures,
            run_dir=run_dir,
            project=project_path,
            transport=arguments.transport,
            error=error,
        )
        write_task_result(run_dir, result)
        transcript.section("Final result", f"Task failed: {error}")
        transcript.set_final(result.as_document())
        print(f"Failed: {task.task_id} — {error}", file=sys.stderr)
        return result

    proposal = final["proposal"]
    attempt_result = final["result"]
    run = final["run"]
    specifications = final["specifications"]
    spec_closeness_pct = specification_closeness(specifications) or 0.0
    passed = sum(1 for spec in specifications if spec.status == "pass")

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

    result = TaskResult(
        task_id=task.task_id,
        circuit_name=task.circuit_name,
        circuit_type=task.circuit_type,
        pdk=task.pdk,
        status="succeeded",
        spec_closeness_pct=spec_closeness_pct,
        process_efficiency_pct=process.process_efficiency_pct,
        tool_efficiency_pct=process.tool_efficiency_pct,
        thinking_efficiency_pct=process.thinking_efficiency_pct,
        specs_met=passed,
        specs_total=len(specifications),
        model_turns=process.model_turns,
        tool_calls=process.tool_calls,
        tool_failures=process.tool_failures,
        run_dir=run_dir,
        project=project_path,
        transport=arguments.transport,
    )
    write_task_result(run_dir, result)

    transcript.section(
        "Final result",
        f"Simulation **{run.state}** after {final['attempt']} attempt(s).\n\n"
        "### Specifications\n\n"
        + specifications_table(specifications)
        + "\n\n### Performance\n\n"
        + performance_table(final["metrics"])
        + "\n\n### Scores\n\n"
        + f"- Specification closeness: **{spec_closeness_pct:.1f}%**\n"
        + f"- Process efficiency: **{process.process_efficiency_pct:.1f}%**\n"
        + f"- Tool-call efficiency: {process.tool_efficiency_pct:.1f}%\n"
        + f"- Thinking efficiency: {process.thinking_efficiency_pct:.1f}%",
    )
    transcript.set_final(result.as_document())

    print()
    print(f"Circuit : {task.circuit_name} [{task.pdk}]")
    print(f"Run     : {run_dir}")
    print(f"Project : {project_path}")
    print(
        f"State   : {run.state} ({run.sample_count} samples, {len(run.trace_names)} signals)"
    )
    print(
        f"Specs   : {passed}/{len(specifications)} met; closeness {spec_closeness_pct:.1f}%"
    )
    print(f"Process : {process.process_efficiency_pct:.1f}% efficiency")
    print()
    print(specifications_table(specifications))
    return result


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Run ICE circuit-creation tasks with an OpenRouter model. Each task "
            "produces an openable ICE project and two scores: specification "
            "closeness and tool/thinking efficiency."
        ),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--task",
        metavar="NUMBER|TASK_ID|CIRCUIT",
        help="task to start from; omit for an interactive selector",
    )
    parser.add_argument(
        "--count",
        metavar="N|all",
        help="number of tasks to run starting at --task, or 'all'",
    )
    parser.add_argument(
        "--all", action="store_true", help="run every task in creation_tasks.csv"
    )
    parser.add_argument(
        "--list", action="store_true", help="print the creation task list and exit"
    )
    parser.add_argument(
        "--api-key",
        default=None,
        help=(
            "OpenRouter API key (default: $OPENROUTER_API_KEY, then the "
            f"OPENROUTER_API_KEY value in {ENV_FILE.name}, then an interactive prompt)"
        ),
    )
    parser.add_argument(
        "--model",
        default=None,
        help=f"OpenRouter model slug (default: {DEFAULT_MODEL}; prompted interactively if omitted)",
    )
    parser.add_argument(
        "--transport",
        choices=("mcp", "ice-cli"),
        default="mcp",
        help=("execute through ice-mcp (default) or the older direct ice-cli path"),
    )
    parser.add_argument(
        "--tool-rounds",
        type=int,
        default=12,
        help=(
            "maximum tool-enabled model turns per attempt, followed by one "
            "final JSON-only turn"
        ),
    )
    parser.add_argument(
        "--no-tools",
        action="store_true",
        help=(
            "do not expose ice-mcp tools to the model; still create and "
            "simulate the returned proposal through ice-mcp"
        ),
    )
    parser.add_argument(
        "--attempts",
        type=int,
        default=3,
        help="design attempts allowed for simulation repairs and spec fixes",
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
        help="directory that receives one folder per task",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    arguments = parser.parse_args(argv)

    tasks = read_tasks()
    if arguments.list:
        print_task_list(tasks)
        return 0
    if not tasks:
        parser.error(f"{TASK_LIST} contains no tasks")

    arguments.api_key = load_api_key(arguments.api_key)
    if not arguments.api_key:
        print(
            "error: no OpenRouter API key. Pass --api-key, set "
            f"OPENROUTER_API_KEY, or put it in {ENV_FILE}.",
            file=sys.stderr,
        )
        return 2
    arguments.model = choose_model(arguments.model)
    if arguments.transport == "mcp":
        if shutil.which(ICE_MCP) is None:
            print(
                f"error: {ICE_MCP!r} was not found. Build ICE or set ICE_MCP, "
                "or pass --transport ice-cli.",
                file=sys.stderr,
            )
            return 2
    elif shutil.which(ICE_CLI) is None:
        print(
            f"error: {ICE_CLI!r} was not found. Build ICE or set ICE_CLI.",
            file=sys.stderr,
        )
        return 2
    if arguments.attempts < 1:
        parser.error("--attempts must be at least 1")
    if arguments.tool_rounds < 1:
        parser.error("--tool-rounds must be at least 1")
    if arguments.count is not None and arguments.count.lower() != "all":
        try:
            if int(arguments.count) < 1:
                raise ValueError
        except ValueError:
            parser.error("--count must be a positive integer or 'all'")

    try:
        selected = choose_tasks(tasks, arguments)
    except (ValueError, EOFError) as error:
        parser.error(str(error))
    if not selected:
        parser.error("no tasks selected")

    print(f"Running {len(selected)} of {len(tasks)} creation task(s).")
    results: list[TaskResult] = []
    for index, task in enumerate(selected, start=1):
        print(
            f"\n[{index}/{len(selected)}] {task.circuit_name} [{task.pdk}]  {task.task_id}"
        )
        try:
            result = generate(task, arguments)
        except Exception as error:  # keep a batch alive if one task crashes
            print(f"Failed: {task.task_id} — {error}", file=sys.stderr)
            result = TaskResult(
                task_id=task.task_id,
                circuit_name=task.circuit_name,
                circuit_type=task.circuit_type,
                pdk=task.pdk,
                status="failed",
                spec_closeness_pct=0.0,
                process_efficiency_pct=0.0,
                tool_efficiency_pct=0.0,
                thinking_efficiency_pct=0.0,
                specs_met=0,
                specs_total=0,
                model_turns=0,
                tool_calls=0,
                tool_failures=0,
                run_dir=Path(arguments.runs_dir).expanduser().resolve(),
                project=Path(arguments.runs_dir).expanduser().resolve(),
                transport=arguments.transport,
                error=str(error),
            )
        results.append(result)

    runs_dir = Path(arguments.runs_dir).expanduser().resolve()
    runs_dir.mkdir(parents=True, exist_ok=True)
    summary_path = (
        runs_dir / f"task_results_{datetime.now().strftime('%Y%m%d-%H%M%S')}.csv"
    )
    rows = [result.as_document() for result in results]
    with summary_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=list(rows[0].keys()), lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)

    succeeded = sum(1 for result in results if result.status == "succeeded")
    average_spec = sum(result.spec_closeness_pct for result in results) / max(
        1, len(results)
    )
    average_process = sum(result.process_efficiency_pct for result in results) / max(
        1, len(results)
    )
    print(f"\nCompleted {succeeded}/{len(results)} task(s).")
    print(f"Average specification closeness: {average_spec:.1f}%")
    print(f"Average process efficiency:      {average_process:.1f}%")
    print(f"Results CSV: {summary_path}")
    return 0 if succeeded == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
