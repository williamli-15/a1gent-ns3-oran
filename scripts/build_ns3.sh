#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

NS3_DIR="${1:-${REPO_ROOT}/workspace/ns-3.42}"

echo
echo "================================================================================"
echo "Build ns-3"
echo "================================================================================"
echo "ns-3 directory: ${NS3_DIR}"

if [[ ! -x "${NS3_DIR}/ns3" ]]; then
  echo "Error: ns-3 launcher not found at ${NS3_DIR}/ns3" >&2
  exit 1
fi

cd "${NS3_DIR}"
echo
echo "Configuring ns-3..."
./ns3 configure --enable-examples --enable-tests
echo
echo "Building ns-3..."
./ns3 build
echo
echo "ns-3 build completed."
