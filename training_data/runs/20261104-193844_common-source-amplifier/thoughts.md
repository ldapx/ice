# Agent transcript — Common-source amplifier

- **Circuit**: Common-source amplifier
- **Catalogue**: circuits.txt
- **Model**: nvidia/nemotron-3-ultra-550b-a55b:free
- **Started**: 2026-11-04 19:38:44 -03
- **Run directory**: /home/gan/repos/openvlsi/Training-data/runs/20261104-193844_common-source-amplifier

This file is appended while the agent works, so an interrupted run keeps its reasoning on disk.

## Task

Design **Common-source amplifier** as an OpenVLSI text-SPICE schematic, simulate it, and meet the proposed specifications. The harness builds the schematic, runs the simulation, measures every specification from the waveforms, and gives the agent the results.

### System prompt

```
You are an analogue and mixed-signal circuit design agent.
OpenVLSI (an open-source schematic/layout/simulation tool) will store your netlist as
a text-SPICE schematic and simulate it with ngspice.

Design constraints:
1. Return one complete, self-contained SPICE netlist that ngspice can run.
2. No foundry PDK is installed. Use generic model cards, for example
   `.model nmos nmos(level=1 vto=0.5 kp=100u lambda=0.02)` and
   `.model pnpsub pnp(...)`, or ideal dependent sources (E, G, F, H) for
   behavioural blocks.
3. Node 0 is ground. Every node needs a DC path to ground and a parallel
   capacitance when that is needed for convergence.
4. Do not include .tran, .ac, .dc, .op, .end, .control or .meas cards: OpenVLSI
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
  "netlist": "* title line\nvin in 0 0 SIN(0 1 1k)\n...",
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
(k, meg, m, u, n, p).
```

### User prompt

```
Design the following circuit: Common-source amplifier.

Return the JSON proposal now.
```

## Attempt 1 of 1

### Model

`nvidia/nemotron-3-ultra-550b-a55b:free` in 35.3s

### Reasoning

The user wants a common-source amplifier design. I need to create a SPICE netlist for a common-source amplifier using generic MOSFET models. Let me design a practical common-source amplifier with realistic component values.

Key considerations:
1. Use a generic NMOS model
2. Design for 1.8V supply
3. Include proper biasing (voltage divider bias)
4. Add coupling capacitors for AC analysis
5. Include a load resistor
6. Set up for transient simulation with a sine wave input to measure gain

Let me design:
- VDD = 1.8V
- Target gain around 20dB (10x voltage gain)
- Bias current around 100uA
- RD = 10k, RS = 1k (with bypass cap for AC gain)
- RG1/RG2 for gate bias
- Input coupling cap, output coupling cap
- Source bypass capacitor for AC gain

For a common-source amp with source degeneration bypassed:
Av ≈ -gm * RD
gm = sqrt(2 * kp * (W/L) * ID)
With kp = 100u, ID = 100uA, W/L = 10
gm = sqrt(2 * 100u * 10 * 100u) = sqrt(200e-6) ≈ 447uS
Av = -447u * 10k = -4.47 (13dB) - not enough

Let me increase W/L or RD. With W/L = 50, RD = 20k:
gm = sqrt(2 * 100u * 50 * 100u) = sqrt(1e-3) = 31.6mS
Av = -31.6m * 20k = -632 (56dB) - too much

Let me target more reasonable: ID = 500uA, RD = 5k, W/L = 20
gm = sqrt(2 * 100u * 20 * 500u) = sqrt(2e-3) = 44.7mS
Av = -44.7m * 5k = -223 (47dB) - still high

Actually, with source resistor not bypassed, gain is -gm*RD/(1+gm*RS)
But with bypass cap, it's -gm*RD

Let me use more moderate values: ID = 200uA, RD = 10k, W/L = 10
gm = sqrt(2 * 100u * 10 * 200u) = sqrt(400e-6) = 20mS
Av = -20m * 10k = -200 (46dB)

Hmm, the generic model has kp=100u which is quite high. Let me adjust.
Actually, level=1 kp is in A/V^2, so 100u = 100 μA/V². That's reasonable for a power MOSFET but high for small signal.

Let me just design with practical values and see. I'll use:
- VDD = 1.8V
- RG1 = 1M, RG2 = 1M (gate bias at VDD/2 = 0.9V)
- RD = 10k
- RS = 2k (with bypass cap)
- ID target ~ 200uA
- VGS = VG - VS = 0.9 - 0.4 = 0.5V (Vth=0.5V, so just at threshold)
- Need higher VG. Let me use RG1=1.5M, RG2=1M -> VG = 1.8 * 1/2.5 = 0.72V
- With RS=1k, ID=200uA -> VS=0.2V, VGS=0.52V, VDS = 1.8 - 0.2 - 2 = -0.4V (saturation)

Let me recalculate: VDD=1.8, RD=10k, RS=1k, ID=200uA
VD = 1.8 - 2 = -0.2V (wait: 1.8 - 200u*10k = 1.8 - 2 = -0.2V) - not good!

Need lower RD or lower ID. Let me use RD=5k, ID=200uA -> VD = 1.8 - 1 = 0.8V
VS = 200u * 1k = 0.2V
VDS = 0.8 - 0.2 = 0.6V > VGS-Vth = 0.52-0.5=0.02V - OK, in saturation

Gate bias: VG = VS + VGS = 0.2 + 0.52 = 0.72V
Divider: VG = VDD * RG2/(RG1+RG2) = 0.72
RG2/(RG1+RG2) = 0.72/1.8 = 0.4
So RG1 = 1.5 * RG2. Use RG1=1.5M, RG2=1M.

Input signal: 10mV peak sine at 1kHz
Coupling caps: 1uF (large for low frequency)
Bypass cap: 10uF

Let me write the netlist.

### Response

```json
{
  "circuit": "Common-source amplifier",
  "topology_summary": "Single-stage common-source NMOS amplifier with voltage-divider gate bias, resistive drain load, and source degeneration resistor bypassed by a large capacitor for AC gain. Designed for 1.8 V supply, ~200 µA bias current, targeting ~26 dB midband voltage gain.",
  "design_notes": "Generic level-1 NMOS model (Vth=0.5 V, kp=100 µA/V²) with W/L=20. Gate bias set to 0.72 V via 1.5 MΩ/1 MΩ divider. Drain resistor 5 kΩ, source resistor 1 kΩ bypassed by 10 µF. Input/output coupling caps 1 µF. Input signal 10 mVpk 1 kHz sine. Simulation runs 5 ms to capture steady-state gain.",
  "netlist": "* Common-source amplifier\n.model nmos nmos(level=1 vto=0.5 kp=100u lambda=0.02 gamma=0.5 phi=0.6)\n\nVDD vdd 0 1.8\n\n* Gate bias divider\nRG1 vdd gate 1.5M\nRG2 gate 0 1M\n\n* Input coupling\nVIN in 0 SIN(0 10m 1k)\nCIN in gate 1u\n\n* MOSFET\nM1 drain gate source 0 nmos w=20u l=1u\n\n* Drain load\nRD vdd drain 5k\n\n* Source degeneration with AC bypass\nRS source 0 1k\nCS source 0 10u\n\n* Output coupling\nCOUT drain out 1u\nRL out 0 100k\n\n* Initial condition to help convergence\n.ic v(gate)=0.72 v(source)=0.2 v(drain)=0.8\n",
  "analysis": {"stop": "5m", "step": "1u"},
  "signals": ["v(out)", "v(in)"],
  "specifications": [
    {"name": "Midband voltage gain", "metric": "gain_db", "trace": "v(out)", "reference": "v(in)", "target": ">= 26", "unit": "dB"},
    {"name": "Output swing (peak)", "metric": "peak_to_peak", "trace": "v(out)", "target": ">= 1.0", "unit": "V"},
    {"name": "DC drain voltage", "metric": "steady_state", "trace": "v(drain)", "target": "~ 0.8", "unit": "V"},
    {"name": "DC source voltage", "metric": "steady_state", "trace": "v(source)", "target": "~ 0.2", "unit": "V"}
  ]
}
```

### Token usage

prompt_tokens=956, completion_tokens=1904, total_tokens=2860, cost=0, is_byok=False, prompt_tokens_details={'cached_tokens': 0, 'cache_write_tokens': 0, 'audio_tokens': 0, 'video_tokens': 0}, cost_details={'upstream_inference_cost': 0, 'upstream_inference_prompt_cost': 0, 'upstream_inference_completions_cost': 0}, completion_tokens_details={'reasoning_tokens': 1257, 'image_tokens': 0, 'audio_tokens': 0}

## Proposed design

**Topology:** Single-stage common-source NMOS amplifier with voltage-divider gate bias, resistive drain load, and source degeneration resistor bypassed by a large capacitor for AC gain. Designed for 1.8 V supply, ~200 µA bias current, targeting ~26 dB midband voltage gain.

**Notes:** Generic level-1 NMOS model (Vth=0.5 V, kp=100 µA/V²) with W/L=20. Gate bias set to 0.72 V via 1.5 MΩ/1 MΩ divider. Drain resistor 5 kΩ, source resistor 1 kΩ bypassed by 10 µF. Input/output coupling caps 1 µF. Input signal 10 mVpk 1 kHz sine. Simulation runs 5 ms to capture steady-state gain.

Netlist saved to `netlist_attempt1.sp`.

### Netlist

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

### Proposed specifications

| Specification | Metric | Target | Measured | Status |
| --- | --- | --- | --- | --- |
| Midband voltage gain | gain_db | >= 26 | n/a | measured |
| Output swing (peak) | peak_to_peak | >= 1.0 | n/a | measured |
| DC drain voltage | steady_state | ~ 0.8 | n/a | measured |
| DC source voltage | steady_state | ~ 0.2 | n/a | measured |

### OpenVLSI command

```sh
$ openvlsi-cli project-create project project
```

exit 0 in 0.00s

stderr:
```
[2026-11-04 19:39:19.295] [openvlsi-cli] [info] Saved project project/project.ovpro
```

stdout:
```
{
  "name": "project",
  "path": "project/project.ovpro"
}
```

### OpenVLSI command

```sh
$ openvlsi-cli schematic-add-text project design_a1 generic /home/gan/repos/openvlsi/Training-data/runs/20261104-193844_common-source-amplifier/netlist_attempt1.sp
```

exit 0 in 0.00s

stderr:
```
[2026-11-04 19:39:19.299] [openvlsi-cli] [info] Saved project project/project.ovpro
```

stdout:
```
{
  "id": 1,
  "name": "design_a1",
  "pdk": "generic",
  "text": "* Common-source amplifier\n.model nmos nmos(level=1 vto=0.5 kp=100u lambda=0.02 gamma=0.5 phi=0.6)\n\nVDD vdd 0 1.8\n\n* Gate bias divider\nRG1 vdd gate 1.5M\nRG2 gate 0 1M\n\n* Input coupling\nVIN in 0 SIN(0 10m 1k)\nCIN in gate 1u\n\n* MOSFET\nM1 drain gate source 0 nmos w=20u l=1u\n\n* Drain load\nRD vdd drain 5k\n\n* Source degeneration with AC bypass\nRS source 0 1k\nCS source 0 10u\n\n* Output coupling\nCOUT drain out 1u\nRL out 0 100k\n\n* Initial condition to help convergence\n.ic v(gate)=0.72 v(source)=0.2 v(drain)=0.8\n"
}
```

### OpenVLSI command

```sh
$ openvlsi-cli simulation-add project sim_a1 design_a1 transient 5m 1u 'v(out),v(in),v(drain),v(source)'
```

exit 0 in 0.00s

stderr:
```
[2026-11-04 19:39:19.304] [openvlsi-cli] [info] Saved project project/project.ovpro
```

stdout:
```
{
  "graph": 3,
  "id": 2,
  "name": "sim_a1",
  "schematic": 1,
  "signals": "v(out),v(in),v(drain),v(source)"
}
```

### OpenVLSI command

```sh
$ openvlsi-cli simulation-run project sim_a1 120000
```

exit 0 in 0.03s

stderr:
```
progress: 1 ngspice transient analysis complete
```

stdout:
```
{
  "analysis": "transient",
  "error": "",
  "run_id": "20261104T223919.323Z-490fec3d",
  "state": "succeeded",
  "stored": "project/build/simulations/2-sim_a1/runs/20261104T223919.323Z-490fec3d.json",
  "time_points": 5008,
  "traces": 4,
  "warnings": []
}
```

## Simulation result

State: **succeeded**

Samples: 5008 over 0.005s

### Specifications

| Specification | Metric | Target | Measured | Status |
| --- | --- | --- | --- | --- |
| Midband voltage gain | gain_db | >= 26 | 42.2 dB | PASS |
| Output swing (peak) | peak_to_peak | >= 1.0 | 952.4 mV | FAIL |
| DC drain voltage | steady_state | ~ 0.8 | 1.739 V | FAIL |
| DC source voltage | steady_state | ~ 0.2 | 124.7 mV | FAIL |

### Performance

| Signal | Min | Max | Peak-to-peak | Mean | RMS | Rise | Fall | Overshoot | Settling | Ringing |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| v(out) | 0 V | 952.4 mV | 952.4 mV | 925.6 mV | 925.8 mV | 7.514 ns | n/a | 6.474 % | 3.664 ms | n/a |
| v(in) | -10 mV | 10 mV | 20 mV | 10.66 nV | 7.065 mV | n/a | 86.76 µs | 57.39 % | 5 ms | 1 kHz |
| v(drain) | 800 mV | 1.753 V | 953.2 mV | 1.749 V | 1.749 V | 7.884 ns | n/a | 1.559 % | 9.657 ns | n/a |
| v(source) | 121.7 mV | 200 mV | 78.27 mV | 157.5 mV | 159.1 mV | n/a | 3.766 ms | 3.988 % | 5 ms | n/a |

## Final result

Simulation **succeeded** after 1 attempt(s).

### Specifications

| Specification | Metric | Target | Measured | Status |
| --- | --- | --- | --- | --- |
| Midband voltage gain | gain_db | >= 26 | 42.2 dB | PASS |
| Output swing (peak) | peak_to_peak | >= 1.0 | 952.4 mV | FAIL |
| DC drain voltage | steady_state | ~ 0.8 | 1.739 V | FAIL |
| DC source voltage | steady_state | ~ 0.2 | 124.7 mV | FAIL |

### Performance

| Signal | Min | Max | Peak-to-peak | Mean | RMS | Rise | Fall | Overshoot | Settling | Ringing |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| v(out) | 0 V | 952.4 mV | 952.4 mV | 925.6 mV | 925.8 mV | 7.514 ns | n/a | 6.474 % | 3.664 ms | n/a |
| v(in) | -10 mV | 10 mV | 20 mV | 10.66 nV | 7.065 mV | n/a | 86.76 µs | 57.39 % | 5 ms | 1 kHz |
| v(drain) | 800 mV | 1.753 V | 953.2 mV | 1.749 V | 1.749 V | 7.884 ns | n/a | 1.559 % | 9.657 ns | n/a |
| v(source) | 121.7 mV | 200 mV | 78.27 mV | 157.5 mV | 159.1 mV | n/a | 3.766 ms | 3.988 % | 5 ms | n/a |

