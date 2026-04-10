# A1gent-ns3-oran

A1gent on NIST ns-3 O-RAN: deterministic and auditable intent-driven radio control.

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

Default workspace target: `ns-3.42`.

3. Run the ns-3 scenario:

```bash
./scripts/run_ns3_scenario.sh
```

4. Run the orchestrator in a second terminal:

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
