#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
source "${SCRIPT_DIR}/common.sh"

NS3_DIR="${1:-${REPO_ROOT}/workspace/ns-3.42}"
MODULE_SRC_DIR="${REPO_ROOT}/ns3/contrib/oran"
MODULE_DST_DIR="${NS3_DIR}/contrib/oran"
OVERLAY_SRC_DIR="${REPO_ROOT}/ns3/src"
OVERLAY_DST_DIR="${NS3_DIR}/src"

print_section "Install project module into ns-3"
print_kv "ns-3 directory" "${NS3_DIR}"
print_kv "Module source" "${MODULE_SRC_DIR}"
print_kv "Module destination" "${MODULE_DST_DIR}"

if [[ ! -d "${MODULE_SRC_DIR}" ]]; then
  die "Source module not found: ${MODULE_SRC_DIR}"
fi

if [[ ! -d "${NS3_DIR}" ]]; then
  die "ns-3 directory not found: ${NS3_DIR}"
fi

print_step "Sync contrib/oran"
mkdir -p "${NS3_DIR}/contrib"
rsync -a --delete --exclude '.git' "${MODULE_SRC_DIR}/" "${MODULE_DST_DIR}/"

if [[ -d "${OVERLAY_SRC_DIR}" ]]; then
  print_step "Sync LTE overlay"
  rsync -a "${OVERLAY_SRC_DIR}/" "${OVERLAY_DST_DIR}/"
fi

print_info "Installed project overlays into ${NS3_DIR}."
