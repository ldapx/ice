# Common-source amplifier

Specification and performance report produced by `training_agent.py` on 2026-11-04 19:39:19 -03.

## Simulation

| Field | Value |
| --- | --- |
| Model | nvidia/nemotron-3-ultra-550b-a55b:free |
| Project | project |
| Run directory | /home/gan/repos/openvlsi/Training-data/runs/20261104-193844_common-source-amplifier |
| Schematic | `design_a1` (text SPICE) |
| Simulation | `sim_a1` (transient) |
| State | succeeded |
| Time window | 5m (step 1u) |
| Signals | v(out), v(in), v(drain), v(source) |
| Samples | 5008 |
| Simulator | ngspice 47 |
| Specifications | 1 met, 3 missed, 0 no data |
| Verdict | 3 specification(s) not met |

## Specifications

| Specification | Metric | Target | Measured | Status |
| --- | --- | --- | --- | --- |
| Midband voltage gain | gain_db | >= 26 | 42.2 dB | PASS |
| Output swing (peak) | peak_to_peak | >= 1.0 | 952.4 mV | FAIL |
| DC drain voltage | steady_state | ~ 0.8 | 1.739 V | FAIL |
| DC source voltage | steady_state | ~ 0.2 | 124.7 mV | FAIL |

Missed specifications: Output swing (peak), DC drain voltage, DC source voltage.

## Measured performance

| Signal | Min | Max | Peak-to-peak | Mean | RMS | Rise | Fall | Overshoot | Settling | Ringing |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| v(out) | 0 V | 952.4 mV | 952.4 mV | 925.6 mV | 925.8 mV | 7.514 ns | n/a | 6.474 % | 3.664 ms | n/a |
| v(in) | -10 mV | 10 mV | 20 mV | 10.66 nV | 7.065 mV | n/a | 86.76 µs | 57.39 % | 5 ms | 1 kHz |
| v(drain) | 800 mV | 1.753 V | 953.2 mV | 1.749 V | 1.749 V | 7.884 ns | n/a | 1.559 % | 9.657 ns | n/a |
| v(source) | 121.7 mV | 200 mV | 78.27 mV | 157.5 mV | 159.1 mV | n/a | 3.766 ms | 3.988 % | 5 ms | n/a |

## Agent design summary

**Topology:** Single-stage common-source NMOS amplifier with voltage-divider gate bias, resistive drain load, and source degeneration resistor bypassed by a large capacitor for AC gain. Designed for 1.8 V supply, ~200 µA bias current, targeting ~26 dB midband voltage gain.

**Notes:** Generic level-1 NMOS model (Vth=0.5 V, kp=100 µA/V²) with W/L=20. Gate bias set to 0.72 V via 1.5 MΩ/1 MΩ divider. Drain resistor 5 kΩ, source resistor 1 kΩ bypassed by 10 µF. Input/output coupling caps 1 µF. Input signal 10 mVpk 1 kHz sine. Simulation runs 5 ms to capture steady-state gain.

## Attempts

| Attempt | Simulation | Specs met | Note |
| --- | --- | --- | --- |
| 1 | succeeded | 1/4 | Output swing (peak); DC drain voltage; DC source voltage not met |

## Netlist

```spice
* Common-source amplifier
.model nmos nmos(level=1 vto=0.5 kp=100u lambda=0.02 gamma=0.5 phi=0.6)

VDD vdd 0 1.8

* Gate bias divider
RG1 vdd gate 1.5M
RG2 gate 0 1M

* Input coupling
VIN in 0 SIN(0 10m 1k)
CIN in gate 1u

* MOSFET
M1 drain gate source 0 nmos w=20u l=1u

* Drain load
RD vdd drain 5k

* Source degeneration with AC bypass
RS source 0 1k
CS source 0 10u

* Output coupling
COUT drain out 1u
RL out 0 100k

* Initial condition to help convergence
.ic v(gate)=0.72 v(source)=0.2 v(drain)=0.8
```

## Artifacts

- `thoughts.md` — full agent reasoning and tool log
- `specs.md`, `specs.csv` — this table
- `netlist.sp` — the simulated netlist
- `simulation.json` — the raw stored OpenVLSI run
- `proposal.json` — the model's final structured answer
- `project/` — the OpenVLSI project (open `project.ovpro` in the OpenVLSI GUI)
