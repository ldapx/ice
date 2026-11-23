# Common-source amplifier

Specification and performance report produced by `training_agent.py` on 2026-11-04 19:24:53 -03.

## Simulation

| Field | Value |
| --- | --- |
| Model | nvidia/nemotron-3-ultra-550b-a55b:free |
| Project | project |
| Run directory | /home/gan/repos/ice/Training-data/runs/20261104-192128_common-source-amplifier |
| Schematic | `design_a2` (text SPICE) |
| Simulation | `sim_a2` (transient) |
| State | succeeded |
| Time window | 5m (step 1u) |
| Signals | v(in), v(out), v(gate), v(drain), v(source), v(bias) |
| Samples | 5008 |
| Simulator | ngspice 47 |
| Specifications | 2 met, 2 missed, 0 no data |
| Verdict | 2 specification(s) not met |

## Specifications

| Specification | Metric | Target | Measured | Status |
| --- | --- | --- | --- | --- |
| Midband voltage gain | gain_db | >= 30 | 31.64 dB | PASS |
| Output peak-to-peak swing | peak_to_peak | >= 1.0 | 326.5 mV | FAIL |
| DC drain voltage | steady_state | ~ 0.9 | 1.186 V | FAIL |
| DC gate voltage | steady_state | ~ 1.35 | 1.344 V | PASS |

Missed specifications: Output peak-to-peak swing, DC drain voltage.

## Measured performance

| Signal | Min | Max | Peak-to-peak | Mean | RMS | Rise | Fall | Overshoot | Settling | Ringing |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| v(in) | -10 mV | 10 mV | 20 mV | 10.66 nV | 7.065 mV | n/a | 86.76 µs | 57.39 % | 5 ms | 1 kHz |
| v(out) | 0 V | 326.5 mV | 326.5 mV | 272.7 mV | 274.5 mV | 7.71 ns | n/a | 14.54 % | 5 ms | 1.054 kHz |
| v(gate) | 1.34 V | 1.36 V | 20.44 mV | 1.35 V | 1.35 V | n/a | 78.64 µs | 71.26 % | 5 ms | 1 kHz |
| v(drain) | 900 mV | 1.227 V | 326.7 mV | 1.173 V | 1.174 V | 7.745 ns | n/a | 14.09 % | 5 ms | 1.054 kHz |
| v(source) | 193.3 mV | 200 mV | 6.673 mV | 196.3 mV | 196.3 mV | n/a | 3.542 ms | 4.764 % | 5 ms | n/a |
| v(bias) | 400 mV | 408.5 mV | 8.472 mV | 408.5 mV | 408.5 mV | 8 ns | n/a | 0 % | 9.8 ns | 200 Hz |

## Agent design summary

**Topology:** Single-stage NMOS common-source amplifier with PMOS current-mirror active load. NMOS biased via voltage divider (R1=33k, R2=100k) and source resistor RS=1k bypassed by 10uF. PMOS mirror reference set by Rref=2k from bias node to ground. Output AC-coupled through 1uF to 1Meg load. Generic NMOS (Kp=1mA/V², Vth=0.5V) and PMOS (Kp=0.5mA/V², Vth=-0.5V) models. Bias current ~208uA, giving gm=0.65mS and output impedance ~120k for ~38dB gain.

**Notes:** Active load enables high gain (>30dB) and large swing (~1.5Vpp) within 1.8V supply. DC operating point: Vgate~1.35V, Vsource~0.2V, Vdrain~0.9V, Vbias~0.4V. Input amplitude 10mV at 1kHz. Coupling capacitors sized for <10Hz low-frequency poles. No .tran/.ac cards; ICE runs transient.

## Attempts

| Attempt | Simulation | Specs met | Note |
| --- | --- | --- | --- |
| 1 | succeeded | 1/4 | Midband voltage gain; Output peak-to-peak swing; DC drain voltage not met |
| 2 | succeeded | 2/4 | Output peak-to-peak swing; DC drain voltage not met |

## Netlist

```spice
* Common-source amplifier with PMOS active load
VDD vdd 0 1.8
Vin in 0 SIN(0 10m 1k)

* NMOS bias divider
R1 vdd gate 33k
R2 gate 0 100k

* NMOS source degeneration with AC bypass
RS source 0 1k
CS source 0 10u

* Input coupling capacitor
Cin in gate 1u

* PMOS current mirror load
Rref bias 0 2k
M2 bias bias vdd vdd pmos
M3 drain bias vdd vdd pmos

* NMOS amplifier
M1 drain gate source 0 nmos

* Output coupling and high-impedance load
Cout drain out 1u
RL out 0 1Meg

.model nmos nmos(level=1 vto=0.5 kp=1m lambda=0.02 gamma=0.5 phi=0.7)
.model pmos pmos(level=1 vto=-0.5 kp=0.5m lambda=0.02 gamma=0.5 phi=0.7)

.ic v(gate)=1.35 v(source)=0.2 v(drain)=0.9 v(bias)=0.4
```

## Artifacts

- `thoughts.md` — full agent reasoning and tool log
- `specs.md`, `specs.csv` — this table
- `netlist.sp` — the simulated netlist
- `simulation.json` — the raw stored ICE run
- `proposal.json` — the model's final structured answer
- `project/` — the ICE project (open `project.icepro` in the ICE GUI)
