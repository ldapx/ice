#!/usr/bin/env python3
"""Generate one agent creation task per circuit and supported PDK.

The input is the multi-table circuit catalogue (``circuits.csv``). The output
is ``creation_tasks.csv`` with a prompt column that names the circuit, the
target PDK, and a set of desired specifications.

Specification values are taken from the catalogue when a cell is filled in;
blank cells fall back to sensible targets for the circuit family. This keeps
the task list usable while circuits.csv carries only the circuit inventory.
"""

from __future__ import annotations

import csv
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
CIRCUIT_CATALOGUE = HERE / "circuits.csv"
TASK_LIST = HERE / "creation_tasks.csv"

PDKS = {
    "ihp-sg13g2": ("IHP SG13G2", 1.2),
    "sky130A": ("SkyWater 130nm", 1.8),
    "gf180mcuA": ("GlobalFoundries 180nm MCU", 3.3),
}


def fmt_number(value: float) -> str:
    text = f"{value:.3g}"
    return text.replace("e+0", "e").replace("e+", "e")


def supply_text(pdk: str) -> str:
    return f"{fmt_number(PDKS[pdk][1])} V"


def slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def format_spec(column: str, value: str) -> str:
    value = value.strip()
    if value.startswith((">=", "<=", "=", "~")):
        return f"{column} {value}"
    return f"{column} = {value}"


def defaults(kind: str, name: str, pdk: str) -> tuple[dict[str, str], list[str]]:
    supply = supply_text(pdk)
    lower = name.lower()
    values: dict[str, str] = {}
    extras: list[str] = []

    if kind == "amplifier":
        values = {
            "gain_db": ">= 20 dB",
            "bandwidth_hz": ">= 1 MHz",
            "input_impedance_ohm": ">= 1 MOhm",
            "output_impedance_ohm": "<= 10 kOhm",
            "slew_rate_v_per_s": ">= 10 V/us",
            "power_w": "<= 10 mW",
            "supply_v": f"= {supply}",
        }
        if "current mirror" in lower or "current source" in lower:
            values = {
                "output_impedance_ohm": ">= 10 MOhm"
                if "cascode" in lower
                else ">= 1 MOhm",
                "power_w": "<= 1 mW",
                "supply_v": f"= {supply}",
            }
            extras = [
                "output current = 10 uA",
                f"compliance voltage <= {fmt_number(PDKS[pdk][1] - 0.2)} V",
            ]
        elif "transimpedance" in lower:
            values = {
                "bandwidth_hz": ">= 10 MHz",
                "input_impedance_ohm": "<= 100 Ohm",
                "output_impedance_ohm": "<= 100 Ohm",
                "power_w": "<= 10 mW",
                "supply_v": f"= {supply}",
            }
            extras = ["transimpedance gain >= 10 kOhm"]
        elif "low-noise" in lower:
            values = {
                "gain_db": ">= 15 dB",
                "bandwidth_hz": ">= 1 GHz",
                "input_impedance_ohm": "= 50 Ohm",
                "output_impedance_ohm": "= 50 Ohm",
                "power_w": "<= 20 mW",
                "supply_v": f"= {supply}",
            }
            extras = ["noise figure <= 3 dB"]
        elif "mixer" in lower or "multiplier" in lower:
            values = {
                "gain_db": ">= 0 dB",
                "bandwidth_hz": ">= 100 MHz",
                "input_impedance_ohm": ">= 1 kOhm",
                "output_impedance_ohm": "<= 10 kOhm",
                "power_w": "<= 10 mW",
                "supply_v": f"= {supply}",
            }
        elif "instrumentation" in lower:
            values = {
                "gain_db": ">= 20 dB",
                "bandwidth_hz": ">= 100 kHz",
                "input_impedance_ohm": ">= 1 MOhm",
                "output_impedance_ohm": "<= 100 Ohm",
                "power_w": "<= 10 mW",
                "supply_v": f"= {supply}",
            }
            extras = ["CMRR >= 80 dB"]
        elif "op-amp" in lower:
            values = {
                "gain_db": ">= 60 dB",
                "bandwidth_hz": ">= 10 MHz",
                "input_impedance_ohm": ">= 1 MOhm",
                "output_impedance_ohm": "<= 100 Ohm",
                "slew_rate_v_per_s": ">= 10 V/us",
                "power_w": "<= 10 mW",
                "supply_v": f"= {supply}",
            }
            extras = ["phase margin >= 60 deg"]
        elif (
            "source follower" in lower
            or "common-drain" in lower
            or "unity-gain buffer" in lower
        ):
            values = {
                "gain_db": ">= -0.5 dB",
                "bandwidth_hz": ">= 10 MHz",
                "input_impedance_ohm": ">= 1 MOhm",
                "output_impedance_ohm": "<= 1 kOhm",
                "power_w": "<= 5 mW",
                "supply_v": f"= {supply}",
            }
            extras = ["voltage gain >= 0.9 V/V"]
        elif "class-ab" in lower or "class-b" in lower:
            values = {
                "gain_db": ">= 0 dB",
                "bandwidth_hz": ">= 1 MHz",
                "input_impedance_ohm": ">= 1 MOhm",
                "output_impedance_ohm": "<= 10 Ohm",
                "power_w": "<= 20 mW",
                "supply_v": f"= {supply}",
            }
            extras = [
                f"output swing >= {fmt_number(PDKS[pdk][1] - 0.2)} V",
                "drive current >= 10 mA",
            ]
        elif "inverting" in lower or "non-inverting" in lower:
            values = {
                "gain_db": ">= 20 dB",
                "bandwidth_hz": ">= 100 kHz",
                "input_impedance_ohm": ">= 10 kOhm",
                "output_impedance_ohm": "<= 100 Ohm",
                "power_w": "<= 10 mW",
                "supply_v": f"= {supply}",
            }
        elif "integrator" in lower or "differentiator" in lower:
            values = {
                "bandwidth_hz": ">= 100 kHz",
                "input_impedance_ohm": ">= 10 kOhm",
                "output_impedance_ohm": "<= 100 Ohm",
                "power_w": "<= 10 mW",
                "supply_v": f"= {supply}",
            }
            extras = ["unity-gain frequency = 10 kHz"]
        elif "rail-to-rail" in lower:
            values = {
                "gain_db": ">= 20 dB",
                "bandwidth_hz": ">= 1 MHz",
                "input_impedance_ohm": ">= 1 MOhm",
                "output_impedance_ohm": "<= 1 kOhm",
                "power_w": "<= 5 mW",
                "supply_v": f"= {supply}",
            }
            extras = [f"input common-mode range = 0 to {supply}"]
        elif "variable-gain" in lower:
            values = {
                "gain_db": "= -10 to 20 dB",
                "bandwidth_hz": ">= 1 MHz",
                "input_impedance_ohm": ">= 1 kOhm",
                "output_impedance_ohm": "<= 1 kOhm",
                "power_w": "<= 10 mW",
                "supply_v": f"= {supply}",
            }
        elif "differential pair" in lower:
            values = {
                "gain_db": ">= 20 dB",
                "bandwidth_hz": ">= 1 MHz",
                "input_impedance_ohm": ">= 1 MOhm",
                "output_impedance_ohm": "<= 10 kOhm",
                "power_w": "<= 5 mW",
                "supply_v": f"= {supply}",
            }
            extras = ["CMRR >= 60 dB"]

    elif kind == "filter":
        order = 2
        if "first-order" in lower:
            order = 1
        elif "third-order" in lower:
            order = 3
        elif "twin-t" in lower or "sallen-key" in lower or "tow-thomas" in lower:
            order = 2
        values = {
            "cutoff_hz": "= 10 kHz",
            "order": f"= {order}",
            "passband_ripple_db": "<= 1 dB",
            "stopband_attenuation_db": ">= 40 dB",
            "input_impedance_ohm": ">= 10 kOhm",
            "output_impedance_ohm": "<= 1 kOhm",
        }
        if "twin-t" in lower:
            values.update(
                {
                    "cutoff_hz": "= 1 kHz (notch)",
                    "order": "= 2",
                    "passband_ripple_db": "",
                    "stopband_attenuation_db": ">= 40 dB",
                }
            )
        elif "all-pass" in lower:
            values.update(
                {
                    "cutoff_hz": "= 10 kHz",
                    "order": "= 1",
                    "passband_ripple_db": "",
                    "stopband_attenuation_db": "",
                }
            )
            extras = ["phase shift = 180 deg at 10 kHz"]
        elif "switched-capacitor" in lower:
            values.update(
                {
                    "cutoff_hz": "= 100 kHz",
                    "order": "= 2",
                    "passband_ripple_db": "<= 0.5 dB",
                    "stopband_attenuation_db": ">= 40 dB",
                }
            )
            extras = ["clock frequency >= 1 MHz"]
        elif "passive rlc" in lower:
            values.update(
                {
                    "cutoff_hz": "= 1 MHz",
                    "order": "= 2",
                    "passband_ripple_db": "<= 0.1 dB",
                    "input_impedance_ohm": "= 50 Ohm",
                    "output_impedance_ohm": "= 50 Ohm",
                }
            )
        elif "butterworth" in lower:
            values.update(
                {
                    "cutoff_hz": "= 1 MHz",
                    "order": "= 2",
                    "passband_ripple_db": "<= 0.1 dB",
                    "input_impedance_ohm": "= 50 Ohm",
                    "output_impedance_ohm": "= 50 Ohm",
                }
            )
        elif "chebyshev" in lower:
            values.update(
                {
                    "cutoff_hz": "= 1 MHz",
                    "order": "= 3",
                    "passband_ripple_db": "<= 0.5 dB",
                    "stopband_attenuation_db": ">= 50 dB",
                    "input_impedance_ohm": "= 50 Ohm",
                    "output_impedance_ohm": "= 50 Ohm",
                }
            )
        elif "first-order rc" in lower:
            values.update(
                {
                    "cutoff_hz": "= 1 kHz",
                    "order": "= 1",
                    "passband_ripple_db": "<= 0.1 dB",
                    "stopband_attenuation_db": ">= 20 dB",
                }
            )

    elif kind == "comparator":
        values = {
            "propagation_delay_s": "<= 100 ns",
            "input_offset_mv": "<= 5 mV",
            "output_swing_v": f">= {fmt_number(PDKS[pdk][1] - 0.2)} V",
            "power_w": "<= 5 mW",
        }
        if "schmitt" in lower:
            values.update(
                {
                    "propagation_delay_s": "<= 1 us",
                    "hysteresis_mv": ">= 100 mV",
                    "input_offset_mv": "<= 10 mV",
                }
            )
        elif "regenerative latch" in lower:
            values.update({"propagation_delay_s": "<= 10 ns"})
        elif "peak detector" in lower:
            values.update({"propagation_delay_s": "", "input_offset_mv": "<= 10 mV"})
            extras = ["droop <= 10 mV/ms", "tracking error <= 10 mV"]
        elif "bridge" in lower:
            values.update({"propagation_delay_s": "", "input_offset_mv": ""})
            extras = [
                "forward drop <= 1.4 V",
                "output ripple <= 100 mV",
                "load current = 10 mA",
            ]
        elif "rectifier" in lower:
            values.update({"propagation_delay_s": "", "input_offset_mv": "<= 10 mV"})
            extras = ["forward drop <= 10 mV", "load current = 1 mA"]
        elif "clipper" in lower:
            values.update({"propagation_delay_s": "", "input_offset_mv": ""})
            extras = ["clipping levels = +/-0.7 V", f"input range = +/-{supply}"]
        elif "doubler" in lower:
            values.update(
                {
                    "propagation_delay_s": "",
                    "input_offset_mv": "",
                    "output_swing_v": f">= {fmt_number(2 * PDKS[pdk][1] - 0.4)} V",
                }
            )
            extras = [
                "output ripple <= 50 mV",
                "load current = 1 mA",
                "switching frequency = 100 kHz",
            ]

    elif kind == "oscillator":
        values = {
            "oscillation_frequency_hz": "= 1 MHz",
            "output_amplitude_vpp": ">= 1 Vpp",
            "startup_time_s": "<= 1 ms",
            "power_w": "<= 10 mW",
        }
        if "wien" in lower or "rc phase-shift" in lower or "relaxation" in lower:
            values.update(
                {
                    "oscillation_frequency_hz": "= 1 kHz",
                    "startup_time_s": "<= 10 ms",
                }
            )
            extras = ["THD <= 5%"]
        elif "colpitts" in lower or "hartley" in lower or "cross-coupled" in lower:
            values.update(
                {
                    "oscillation_frequency_hz": "= 10 MHz",
                    "phase_noise_dbc_per_hz": "<= -100 dBc/Hz at 1 MHz offset",
                    "startup_time_s": "<= 1 ms",
                    "power_w": "<= 20 mW",
                }
            )
        elif "ring" in lower:
            frequency = "= 5 MHz" if "five-stage" in lower else "= 10 MHz"
            values.update(
                {
                    "oscillation_frequency_hz": frequency,
                    "phase_noise_dbc_per_hz": "<= -80 dBc/Hz at 1 MHz offset",
                    "output_amplitude_vpp": ">= 0.8 Vpp",
                    "startup_time_s": "<= 1 us",
                    "power_w": "<= 5 mW",
                }
            )
        elif "voltage-controlled" in lower or "vco" in lower:
            values.update(
                {
                    "oscillation_frequency_hz": "= 5 MHz",
                    "tuning_range_hz": "= 1 to 10 MHz",
                    "phase_noise_dbc_per_hz": "<= -90 dBc/Hz at 1 MHz offset",
                    "output_amplitude_vpp": ">= 0.8 Vpp",
                    "startup_time_s": "<= 1 ms",
                    "power_w": "<= 5 mW",
                }
            )
        elif "crystal" in lower:
            values.update(
                {
                    "oscillation_frequency_hz": "= 10 MHz",
                    "phase_noise_dbc_per_hz": "<= -120 dBc/Hz at 1 MHz offset",
                    "startup_time_s": "<= 10 ms",
                    "power_w": "<= 5 mW",
                }
            )

    elif kind == "reference":
        values = {
            "output_voltage_v": "= 1.2 V",
            "temperature_coefficient_ppm_per_c": "<= 50 ppm/C",
            "line_regulation_pct_per_v": "<= 1 %/V",
            "load_regulation_pct_per_a": "<= 1 %/A",
            "power_w": "<= 1 mW",
        }
        if "current source" in lower or "ptat" in lower:
            values = {
                "output_current_a": "= 10 uA",
                "temperature_coefficient_ppm_per_c": "<= 100 ppm/C",
                "power_w": "<= 1 mW",
            }
            extras = [
                "output impedance >= 1 MOhm",
                f"compliance voltage <= {fmt_number(PDKS[pdk][1] - 0.2)} V",
            ]
        elif "startup" in lower:
            values = {"power_w": "<= 1 mW"}
            extras = ["startup time <= 1 ms", "quiescent current <= 1 uA"]
        elif "divider" in lower:
            values = {
                "output_voltage_v": f"= {fmt_number(PDKS[pdk][1] / 2)} V",
                "temperature_coefficient_ppm_per_c": "<= 100 ppm/C",
                "power_w": "<= 1 mW",
            }
        elif "supply-independent" in lower:
            values = {
                "output_current_a": "= 10 uA",
                "temperature_coefficient_ppm_per_c": "<= 100 ppm/C",
                "power_w": "<= 1 mW",
            }
            extras = ["PSRR >= 40 dB"]

    elif kind == "power_management":
        values = {
            "input_voltage_v": f"= {supply}",
            "output_voltage_v": f"= {fmt_number(PDKS[pdk][1] * 0.8)} V",
            "output_current_a": "= 10 mA",
            "line_regulation_pct_per_v": "<= 1 %/V",
            "load_regulation_pct_per_a": "<= 1 %/A",
        }
        if "buck" in lower:
            values.update(
                {
                    "output_voltage_v": f"= {fmt_number(PDKS[pdk][1] * 0.6)} V",
                    "output_current_a": "= 100 mA",
                    "efficiency_pct": ">= 85%",
                    "switching_frequency_hz": "= 1 MHz",
                }
            )
            extras = ["output ripple <= 20 mV"]
        elif "boost" in lower:
            values.update(
                {
                    "output_voltage_v": f"= {fmt_number(PDKS[pdk][1] * 1.5)} V",
                    "output_current_a": "= 50 mA",
                    "efficiency_pct": ">= 80%",
                    "switching_frequency_hz": "= 1 MHz",
                }
            )
            extras = ["output ripple <= 20 mV"]
        elif "charge pump" in lower:
            values.update(
                {
                    "output_voltage_v": f"= {fmt_number(2 * PDKS[pdk][1] - 0.4)} V",
                    "output_current_a": "= 1 mA",
                    "efficiency_pct": ">= 70%",
                    "switching_frequency_hz": "= 1 MHz",
                }
            )
            extras = ["output ripple <= 50 mV"]
        elif "current-source load" in lower:
            values = {
                "input_voltage_v": f"= {supply}",
                "output_current_a": "= 1 mA",
            }
            extras = [
                "output impedance >= 1 MOhm",
                f"compliance voltage <= {fmt_number(PDKS[pdk][1] - 0.2)} V",
            ]
        elif "half-bridge" in lower:
            values = {
                "input_voltage_v": f"= {supply}",
                "output_current_a": "= 100 mA",
                "switching_frequency_hz": "= 1 MHz",
            }
            extras = ["propagation delay <= 50 ns", "shoot-through current <= 1 mA"]
        elif "low-dropout" in lower or "linear voltage regulator" in lower:
            extras = [
                "dropout voltage <= 200 mV",
                "PSRR >= 40 dB at 1 kHz",
                "quiescent current <= 100 uA",
            ]

    elif kind == "mixed_signal":
        values = {
            "resolution_bits": "= 8 bits",
            "sample_rate_hz": "= 1 MS/s",
            "input_range_v": f"= 0 to {supply}",
            "output_range_v": f"= 0 to {supply}",
            "power_w": "<= 10 mW",
        }
        if "sample-and-hold" in lower or "track-and-hold" in lower:
            values.update(
                {
                    "resolution_bits": "",
                    "sample_rate_hz": "= 1 MS/s",
                    "power_w": "<= 5 mW",
                }
            )
            extras = ["acquisition time <= 100 ns", "droop <= 1 mV/us"]
        elif "dac" in lower:
            values.update({"resolution_bits": "= 8 bits", "sample_rate_hz": "= 1 MS/s"})
            extras = ["INL <= 1 LSB"]
        elif "flash adc" in lower:
            values.update(
                {
                    "resolution_bits": "= 6 bits",
                    "sample_rate_hz": "= 1 MS/s",
                    "snr_db": ">= 35 dB",
                }
            )
        elif "sar adc" in lower:
            values.update(
                {
                    "resolution_bits": "= 8 bits",
                    "sample_rate_hz": "= 1 MS/s",
                    "snr_db": ">= 45 dB",
                }
            )
        elif "delta-sigma" in lower:
            values.update(
                {
                    "resolution_bits": "= 12 bits",
                    "sample_rate_hz": "= 1 MS/s",
                    "snr_db": ">= 60 dB",
                    "power_w": "<= 5 mW",
                }
            )
        elif "pwm" in lower or "pulse-width" in lower:
            values.update(
                {
                    "resolution_bits": "= 8 bits",
                    "sample_rate_hz": "= 100 kHz",
                }
            )
            extras = ["duty range = 0 to 100%"]
        elif "level shifter" in lower:
            values.update(
                {
                    "resolution_bits": "",
                    "sample_rate_hz": "",
                    "power_w": "<= 1 mW",
                }
            )
            extras = ["propagation delay <= 10 ns", f"voltage range = 0 to {supply}"]
        elif "d flip-flop" in lower:
            values.update(
                {
                    "resolution_bits": "",
                    "sample_rate_hz": ">= 50 MHz",
                    "power_w": "<= 1 mW",
                }
            )
            extras = ["setup time <= 5 ns", "hold time <= 1 ns"]
        elif "phase detector" in lower:
            values.update(
                {
                    "resolution_bits": "",
                    "sample_rate_hz": ">= 10 MHz",
                    "power_w": "<= 1 mW",
                }
            )
            extras = ["phase range = +/-180 deg"]
        elif "pll loop filter" in lower:
            values.update(
                {
                    "resolution_bits": "",
                    "sample_rate_hz": ">= 10 MHz",
                    "power_w": "<= 1 mW",
                }
            )
            extras = ["loop bandwidth = 100 kHz", "phase margin >= 45 deg"]
        elif "quantiser" in lower or "quantizer" in lower:
            values.update(
                {
                    "resolution_bits": "= 1 bit",
                    "sample_rate_hz": "= 10 MS/s",
                    "power_w": "<= 1 mW",
                }
            )

    return values, extras


def read_tables() -> list[tuple[list[str], list[list[str]]]]:
    tables: list[tuple[list[str], list[list[str]]]] = []
    header: list[str] | None = None
    rows: list[list[str]] = []
    with CIRCUIT_CATALOGUE.open(newline="", encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if not row or all(not cell.strip() for cell in row):
                if header is not None:
                    tables.append((header, rows))
                header, rows = None, []
                continue
            lowered = [cell.strip().lower() for cell in row]
            if "name" in lowered:
                if header is not None:
                    tables.append((header, rows))
                header, rows = lowered, []
                continue
            if header is None:
                raise ValueError(f"Row outside a table in {CIRCUIT_CATALOGUE}: {row!r}")
            rows.append(row)
    if header is not None:
        tables.append((header, rows))
    return tables


def main() -> int:
    tasks: list[dict[str, str]] = []
    for header, rows in read_tables():
        type_index = header.index("type")
        name_index = header.index("name")
        for row in rows:
            kind = row[type_index].strip()
            name = row[name_index].strip()
            if not kind or not name:
                continue
            for pdk, (display_name, _) in PDKS.items():
                defaults_for_circuit, extras = defaults(kind, name, pdk)
                specs: list[str] = []
                for index, column in enumerate(header):
                    if column in {"type", "name"}:
                        continue
                    value = row[index].strip() if index < len(row) else ""
                    if not value:
                        value = defaults_for_circuit.get(column, "")
                    if value:
                        specs.append(format_spec(column, value))
                for column, value in defaults_for_circuit.items():
                    if column not in header and value:
                        specs.append(format_spec(column, value))
                specs.extend(extras)
                if not specs:
                    specs = ["meet the circuit's normal functional specifications"]
                supply_note = (
                    ""
                    if kind == "filter"
                    else f" at a nominal supply of {supply_text(pdk)}"
                )
                prompt = (
                    f"Create a {name} in ICE using the {display_name} ({pdk}) PDK{supply_note}. "
                    f"Target specifications: {'; '.join(specs)}. "
                    "Use only devices from the target PDK. Create the circuit as a canvas schematic "
                    "or a text SPICE cell, validate it, simulate it, and report the measured results."
                )
                tasks.append(
                    {
                        "task_id": f"{slug(kind)}-{slug(name)}-{slug(pdk)}",
                        "circuit_type": kind,
                        "circuit_name": name,
                        "pdk": pdk,
                        "specifications": "; ".join(specs),
                        "prompt": prompt,
                    }
                )

    with TASK_LIST.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "task_id",
                "circuit_type",
                "circuit_name",
                "pdk",
                "specifications",
                "prompt",
            ],
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(tasks)

    print(
        f"Wrote {len(tasks)} tasks to {TASK_LIST} ({len(tasks) // len(PDKS)} circuits × {len(PDKS)} PDKs)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
