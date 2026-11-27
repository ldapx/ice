# Common-source amplifier

Specification and performance report produced by `training_agent.py` on 2026-11-04 19:35:27 -03.

## Simulation

| Field | Value |
| --- | --- |
| Model | nvidia/nemotron-3-ultra-550b-a55b:free |
| Project | project |
| Run directory | /home/gan/repos/openvlsi/Training-data/runs/20261104-193505_common-source-amplifier |
| Schematic | `design_a1` (text SPICE) |
| Simulation | `sim_a1` (transient) |
| State | succeeded |
| Time window | 5m (step 1u) |
| Signals | v(in), v(out), v(gate), v(source), v(drain) |
| Samples | 5008 |
| Simulator | ngspice 47 |
| Specifications | 2 met, 1 missed, 0 no data |
| Verdict | 1 specification(s) not met |

## Specifications

| Specification | Metric | Target | Measured | Status |
| --- | --- | --- | --- | --- |
| Midband gain | gain_db | >= 20 | 25.03 dB | PASS |
| Output swing | peak_to_peak | >= 100m | 166.4 mV | PASS |
| DC drain voltage | steady_state | ~ 0.8 | 924.9 mV | FAIL |

Missed specifications: DC drain voltage.

## Measured performance

| Signal | Min | Max | Peak-to-peak | Mean | RMS | Rise | Fall | Overshoot | Settling | Ringing |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| v(in) | -10 mV | 10 mV | 20 mV | 10.66 nV | 7.065 mV | n/a | 86.76 µs | 57.39 % | 5 ms | 1 kHz |
| v(out) | 0 V | 166.4 mV | 166.4 mV | 138.3 mV | 139 mV | 5.688 ns | n/a | 40.65 % | 4.408 ms | n/a |
| v(gate) | 900 mV | 900 mV | 62.92 nV | 900 mV | 900 mV | 9.372 ps | n/a | 8.971e+04 % | 5 ms | 25 kHz |
| v(source) | 93.95 mV | 100 mV | 6.051 mV | 96.53 mV | 96.55 mV | n/a | 3.716 ms | 3.283 % | 5 ms | n/a |
| v(drain) | 800 mV | 966.4 mV | 166.4 mV | 941.9 mV | 942 mV | 6.006 ns | n/a | 33.2 % | 4.318 ms | n/a |

## Agent design summary

**Topology:** Single-stage common-source NMOS amplifier with voltage-divider bias (1M/1M), 10k drain resistor, 1k source resistor with 10uF AC bypass capacitor, and 1uF input/output coupling capacitors. Designed for 1.8V supply, ~100uA bias current, targeting ~26dB midband gain at 1kHz.

**Notes:** Generic level-1 NMOS model (Vth=0.5V, kp=100uA/V^2). Gate biased at 0.9V via equal 1M resistors giving ~0.8V VGS and ~0.1V source voltage. Source bypass capacitor ensures full gm*RD gain at signal frequency. Input amplitude kept small (10mV) for linear operation. Simulation runs 5 cycles at 1kHz to allow gain measurement.

## Attempts

| Attempt | Simulation | Specs met | Note |
| --- | --- | --- | --- |
| 1 | succeeded | 2/3 | DC drain voltage not met |

## Netlist

```spice
* Common-source amplifier
VDD vdd 0 1.8
Vin in 0 SIN(0 10m 1k)

* Bias network
RG1 vdd gate 1M
RG2 gate 0 1M

* Input coupling
Cin in gate 1u

* MOSFET
M1 drain gate source 0 nmos W=22.2u L=1u

* Load and source degeneration
RD vdd drain 10k
RS source 0 1k
CS source 0 10u

* Output coupling
Cout drain out 1u
RL out 0 100k

.model nmos nmos(level=1 vto=0.5 kp=100u lambda=0.02 gamma=0.5 phi=0.6)

.ic v(gate)=0.9 v(source)=0.1 v(drain)=0.8
```

## Artifacts

- `thoughts.md` — full agent reasoning and tool log
- `specs.md`, `specs.csv` — this table
- `netlist.sp` — the simulated netlist
- `simulation.json` — the raw stored OpenVLSI run
- `proposal.json` — the model's final structured answer
- `project/` — the OpenVLSI project (open `project.ovpro` in the OpenVLSI GUI)
