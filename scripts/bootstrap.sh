#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

WORKSPACE_DIR="${1:-${REPO_ROOT}/workspace}"
NS3_DIR="${WORKSPACE_DIR}/ns-3.42"

echo
echo "================================================================================"
echo "Bootstrap ns-3 workspace"
echo "================================================================================"
echo "Repository: ${REPO_ROOT}"
echo "Workspace: ${WORKSPACE_DIR}"
echo "Target ns-3: ${NS3_DIR}"

echo
echo "Fetching ns-3 source..."
"${SCRIPT_DIR}/fetch_ns3.sh" "${WORKSPACE_DIR}"

echo
echo "Installing A1gent O-RAN module and LTE overlay..."
"${SCRIPT_DIR}/install_oran_module.sh" "${NS3_DIR}"

echo
echo "Configuring and building ns-3..."
"${SCRIPT_DIR}/build_ns3.sh" "${NS3_DIR}"

echo
echo "================================================================================"
echo "Bootstrap complete"
echo "================================================================================"
echo "ns-3 workspace is ready."
echo "Run ./scripts/run_ns3_scenario.sh and ./scripts/run_orchestrator.sh in separate terminals."
