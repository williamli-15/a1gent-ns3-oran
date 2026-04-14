#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

WORKSPACE_DIR="${1:-${REPO_ROOT}/workspace}"
NS3_DIR="${WORKSPACE_DIR}/ns-3.42"
NS3_REPO_URL="${NS3_REPO_URL:-https://gitlab.com/nsnam/ns-3-dev.git}"
NS3_REF="${NS3_REF:-ns-3.42}"

echo
echo "================================================================================"
echo "Fetch ns-3"
echo "================================================================================"
echo "Workspace: ${WORKSPACE_DIR}"
echo "Repository: ${NS3_REPO_URL}"
echo "Reference: ${NS3_REF}"
echo "Checkout path: ${NS3_DIR}"

mkdir -p "${WORKSPACE_DIR}"

if [[ -e "${NS3_DIR}" && ! -d "${NS3_DIR}/.git" ]]; then
  echo "Error: refusing to reuse ${NS3_DIR}: not a git checkout" >&2
  exit 1
fi

if [[ -d "${NS3_DIR}/.git" ]]; then
  echo
  echo "Existing ns-3 checkout found. Reusing ${NS3_DIR}."
  exit 0
fi

echo
echo "Cloning ns-3..."
git clone --depth 1 --branch "${NS3_REF}" "${NS3_REPO_URL}" "${NS3_DIR}"
echo
echo "Fetched ns-3 into ${NS3_DIR}."
