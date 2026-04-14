#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

show_usage() {
  cat <<'EOF'
Usage: ./scripts/run_orchestrator.sh [ns3-dir] [extra orchestrator args]

Reads experiments/configs/orchestrator.env by default.
Edit that file to change A1gent cadence, backend flags, and policy guardrails.
EOF
}

if [[ $# -gt 0 && ( "${1}" == "-h" || "${1}" == "--help" ) ]]; then
  show_usage
  exit 0
fi

ORCH_ENV_FILE="${A1GENT_ORCH_ENV_FILE:-${REPO_ROOT}/experiments/configs/orchestrator.env}"
if [[ -f "${ORCH_ENV_FILE}" ]]; then
  echo "Loading orchestrator env: ${ORCH_ENV_FILE}"
  set -a
  source "${ORCH_ENV_FILE}"
  set +a
fi

REQUIRE_OPENROUTER_KEY=0
for LLM_FLAG in "${ENERGY_USE_LLM:-1}" "${LOAD_USE_LLM:-1}" "${QOE_USE_LLM:-1}"; do
  case "$(printf '%s' "${LLM_FLAG}" | tr '[:upper:]' '[:lower:]')" in
    0|false|no|off) ;;
    *)
      REQUIRE_OPENROUTER_KEY=1
      ;;
  esac
done

if [[ "${REQUIRE_OPENROUTER_KEY}" -eq 1 && -z "${OPENROUTER_API_KEY:-}" ]]; then
  echo "Error: OPENROUTER_API_KEY is required when any *_USE_LLM flag is enabled." >&2
  echo "Set it in experiments/configs/orchestrator.env or export it before running this script." >&2
  exit 1
fi

NS3_DIR="${A1GENT_NS3_DIR:-${REPO_ROOT}/workspace/ns-3.42}"
if [[ $# -gt 0 && "${1}" != -* ]]; then
  NS3_DIR="$1"
  shift
fi
if [[ "${NS3_DIR}" != /* ]]; then
  NS3_DIR="${REPO_ROOT}/${NS3_DIR}"
fi

ORCH_LOG_FILE="${ORCH_LOG_FILE:-}"

echo
echo "================================================================================"
echo "Run orchestrator"
echo "================================================================================"
echo "Repository: ${REPO_ROOT}"
echo "ns-3 directory: ${NS3_DIR}"
echo "Orchestrator settings come from experiments/configs/orchestrator.env"
if [[ -n "${ORCH_LOG_FILE}" ]]; then
  echo "Log file: ${ORCH_LOG_FILE}"
fi

cd "${REPO_ROOT}"
if [[ -n "${ORCH_LOG_FILE}" ]]; then
  echo
  echo "Launching orchestrator..."
  python3 -m a1gent --ns3-dir "${NS3_DIR}" "$@" 2>&1 | tee "${ORCH_LOG_FILE}"
else
  echo
  echo "Launching orchestrator..."
  exec python3 -m a1gent --ns3-dir "${NS3_DIR}" "$@"
fi
