#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
source "${SCRIPT_DIR}/common.sh"

WORKSPACE_DIR="${1:-${REPO_ROOT}/workspace}"
NS3_DIR="${WORKSPACE_DIR}/ns-3.42"

print_section "Bootstrap ns-3 workspace"
print_kv "Repository" "${REPO_ROOT}"
print_kv "Workspace" "${WORKSPACE_DIR}"
print_kv "Target ns-3" "${NS3_DIR}"

print_step "Fetch ns-3 source"
"${SCRIPT_DIR}/fetch_ns3.sh" "${WORKSPACE_DIR}"

print_step "Install A1gent O-RAN module and LTE overlay"
"${SCRIPT_DIR}/install_oran_module.sh" "${NS3_DIR}"

print_step "Configure and build ns-3"
"${SCRIPT_DIR}/build_ns3.sh" "${NS3_DIR}"

print_section "Bootstrap complete"
print_info "ns-3 workspace is ready."
print_info "Run ./scripts/run_ns3_scenario.sh and ./scripts/run_orchestrator.sh in separate terminals."
