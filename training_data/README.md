# Training data

Agent-generated analogue and mixed-signal designs, produced with ICE itself.

## Contents

| Path | Purpose |
| --- | --- |
| `circuits.txt` | The catalogue of analogue and mixed-signal circuits the agent can build. One name per line; `#` lines are headings. |
| `training_agent.py` | Picks one circuit, has an OpenRouter model design it, stores the netlist as an ICE text-SPICE schematic, simulates it, measures the specifications, and writes the reports. |
| `runs/` | One folder per design run, with its artifacts. |

## Running

```sh
export OPENROUTER_API_KEY=sk-or-...
python3 Training-data/training_agent.py                    # numbered picker
python3 Training-data/training_agent.py --circuit 12       # by list number
python3 Training-data/training_agent.py --circuit "Wien-bridge oscillator"
python3 Training-data/training_agent.py --list             # show the catalogue
```

`ice-cli` and `ngspice` must be on `PATH` (set `ICE_CLI` to use another binary).
The API key is taken from `--api-key`, else `$OPENROUTER_API_KEY`, else the
`OPENROUTER_API_KEY` line in `Training-data/.env`. That file is git-ignored and
is the easiest way to run the script without exporting anything:

```sh
# Training-data/.env
OPENROUTER_API_KEY=sk-or-...
```

The key is never written into a run's transcript or report. Without credits on
the account, paid slugs (including `deepseek/...`) return HTTP 402; free slugs
end in `:free`. `--model` selects any OpenRouter slug, the default is the free
`nvidia/nemotron-3-ultra-550b-a55b:free` (a reasoning model, so its thoughts are
captured in `thoughts.md`), and `OPENROUTER_MODEL` overrides it.

The agent gets `--attempts` turns (default 3). Each turn it is told either the
simulator error or the measured specification table, so it can repair a broken
netlist or misspecification. A run stops as soon as every specification is met.

## What a run produces

```text
runs/<timestamp>_<circuit>/
  thoughts.md             full transcript: prompts, model reasoning, replies,
                          every ICE command, simulator output, final verdict
  specs.md                specification and performance table
  specs.csv               the same table, machine-readable
  netlist.sp              the netlist that was simulated
  netlist_attemptN.sp     each attempt (kept for failed/repair turns)
  simulation.json         raw stored ICE run (waveforms, provenance)
  proposal.json           the model's final structured answer
  project/                the ICE project; open project.icepro in the ICE GUI
```

`specs.md`/`specs.csv` compare each proposed specification's target with the
value measured from the simulated waveform (`PASS`, `FAIL`, `NO DATA`) and add
a performance table (min, max, peak-to-peak, mean, RMS, rise/fall, overshoot,
settling, ringing) for every captured signal.

## Design constraints the agent works under

ICE currently runs ngspice **transient** analysis only, and no foundry PDK is
assumed. The prompt therefore requires transient-friendly stimuli, generic
`.model` cards or behavioural sources, and specifications chosen from the
metrics the harness can compute (gain in dB from an input/output pair, rise and
settling times, overshoot, oscillation frequency, average power from a supply
current, and basic waveform statistics). Circuits that need AC, DC-sweep, noise
or distortion analysis are out of scope until ICE grows those analyses.

## Exit codes

| Code | Meaning |
| --- | --- |
| `0` | A simulation succeeded and the reports were written (specs may still be missed). |
| `1` | No attempt produced a successful simulation. |
| `2` | Missing API key, missing `ice-cli`, or an OpenRouter request failed. |
