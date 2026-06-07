# A1gent-ns3-oran

Agentic ns-3 O-RAN platform: deterministic and auditable intent-driven radio control.

## Quick start

1. Create a Python environment and install dependencies:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r a1gent/requirements.txt
```

2. Prepare an ns-3 workspace:

```bash
./scripts/bootstrap.sh
```

Default workspace target: `workspace/ns-3.42`.

3. Create runtime env files:

```bash
cp experiments/configs/ns3-scenario.env.example experiments/configs/ns3-scenario.env
cp experiments/configs/orchestrator.env.example experiments/configs/orchestrator.env
```

`ns3-scenario.env` controls the ns-3 scenario.
`orchestrator.env` controls A1gent runtime and policy guardrails.

4. Set `OPENROUTER_API_KEY` in `experiments/configs/orchestrator.env`.

5. Run the ns-3 scenario:

```bash
./scripts/run_ns3_scenario.sh
```

6. Run the orchestrator in a second terminal:

```bash
./scripts/run_orchestrator.sh
```

For ns-3-side source changes, see `ns3/README.md`.

## Citation

Please cite:

> H. Li, D. Xu, M. Chen, and Y. Liu, "Agentic Open RAN: A Deterministic and Auditable Framework for Intent-Driven Radio Control," IEEE International Conference on Communications (ICC), to appear, 2026.

## Acknowledgements

This research was supported by NSF through Award CNS-2440756, CNS-2312138,
CNS-2332834, and NVIDIA Academic Grant Program.
