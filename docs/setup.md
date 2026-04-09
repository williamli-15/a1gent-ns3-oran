# Setup

## Python

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r a1gent/requirements.txt
```

## Bootstrap

```bash
./scripts/bootstrap.sh
```

Default workspace path: `workspace/ns-3.42`.

## Run

Create runtime env files:

```bash
cp experiments/configs/ns3-scenario.env.example experiments/configs/ns3-scenario.env
cp experiments/configs/orchestrator.env.example experiments/configs/orchestrator.env
```

Set `OPENROUTER_API_KEY` in `experiments/configs/orchestrator.env`.

Terminal 1:

```bash
./scripts/run_ns3_scenario.sh
```

Terminal 2:

```bash
./scripts/run_orchestrator.sh
```
