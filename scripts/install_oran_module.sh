#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

NS3_DIR="${1:-${REPO_ROOT}/workspace/ns-3.42}"
MODULE_SRC_DIR="${REPO_ROOT}/ns3/contrib/oran"
MODULE_DST_DIR="${NS3_DIR}/contrib/oran"
OVERLAY_SRC_DIR="${REPO_ROOT}/ns3/src"
OVERLAY_DST_DIR="${NS3_DIR}/src"

echo
echo "================================================================================"
echo "Install project module into ns-3"
echo "================================================================================"
echo "ns-3 directory: ${NS3_DIR}"
echo "Module source: ${MODULE_SRC_DIR}"
echo "Module destination: ${MODULE_DST_DIR}"

if [[ ! -d "${MODULE_SRC_DIR}" ]]; then
  echo "Error: source module not found: ${MODULE_SRC_DIR}" >&2
  exit 1
fi

if [[ ! -d "${NS3_DIR}" ]]; then
  echo "Error: ns-3 directory not found: ${NS3_DIR}" >&2
  exit 1
fi

echo
echo "Syncing contrib/oran..."
mkdir -p "${NS3_DIR}/contrib"
rsync -a --delete --exclude '.git' "${MODULE_SRC_DIR}/" "${MODULE_DST_DIR}/"

if [[ -d "${OVERLAY_SRC_DIR}" ]]; then
  echo
  echo "Syncing LTE overlay..."
  rsync -a "${OVERLAY_SRC_DIR}/" "${OVERLAY_DST_DIR}/"
fi

echo
echo "Installed project overlays into ${NS3_DIR}."
