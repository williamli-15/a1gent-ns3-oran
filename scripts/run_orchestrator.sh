#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
source "${SCRIPT_DIR}/common.sh"

load_env_file() {
  local env_file="${A1GENT_ORCH_ENV_FILE:-${REPO_ROOT}/experiments/configs/orchestrator.env}"
  if [[ -f "${env_file}" ]]; then
    print_info "Loading orchestrator env: ${env_file}"
    set -a
    source "${env_file}"
    set +a
  fi
}

resolve_path() {
  local path="$1"
  resolve_repo_path "${REPO_ROOT}" "${path}"
}

llm_backend_enabled() {
  local value
  value="$(printf '%s' "${1:-1}" | tr '[:upper:]' '[:lower:]')"
  case "${value}" in
    0|false|no|off) return 1 ;;
    *) return 0 ;;
  esac
}

require_openrouter_key() {
  if llm_backend_enabled "${ENERGY_USE_LLM:-1}" || \
     llm_backend_enabled "${LOAD_USE_LLM:-1}" || \
     llm_backend_enabled "${QOE_USE_LLM:-1}"; then
    if [[ -z "${OPENROUTER_API_KEY:-}" ]]; then
      die "OPENROUTER_API_KEY is required when any *_USE_LLM flag is enabled. Set it in experiments/configs/orchestrator.env or export it before running this script."
    fi
  fi
}

main() {
  load_env_file
  require_openrouter_key

  local ns3_dir="${A1GENT_NS3_DIR:-${REPO_ROOT}/workspace/ns-3.42}"
  if [[ $# -gt 0 && "${1}" != -* ]]; then
    ns3_dir="$1"
    shift
  fi
  ns3_dir="$(resolve_path "${ns3_dir}")"

  local orch_log_file="${ORCH_LOG_FILE:-}"

  print_section "Run orchestrator"
  print_kv "Repository" "${REPO_ROOT}"
  print_kv "ns-3 directory" "${ns3_dir}"
  if [[ -n "${orch_log_file}" ]]; then
    print_kv "Log file" "${orch_log_file}"
  fi

  cd "${REPO_ROOT}"
  if [[ -n "${orch_log_file}" ]]; then
    print_step "Launch orchestrator"
    python3 -m a1gent --ns3-dir "${ns3_dir}" "$@" 2>&1 | tee "${orch_log_file}"
  else
    print_step "Launch orchestrator"
    exec python3 -m a1gent --ns3-dir "${ns3_dir}" "$@"
  fi
}

main "$@"
