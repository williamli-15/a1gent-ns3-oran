#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
source "${SCRIPT_DIR}/common.sh"

NS3_DIR="${1:-${REPO_ROOT}/workspace/ns-3.42}"

print_section "Build ns-3"
print_kv "ns-3 directory" "${NS3_DIR}"

if [[ ! -x "${NS3_DIR}/ns3" ]]; then
  die "ns-3 launcher not found at ${NS3_DIR}/ns3"
fi

cd "${NS3_DIR}"
print_step "Configure"
./ns3 configure --enable-examples --enable-tests
print_step "Build"
./ns3 build
print_info "ns-3 build completed."
