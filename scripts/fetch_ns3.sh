#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
source "${SCRIPT_DIR}/common.sh"

WORKSPACE_DIR="${1:-${REPO_ROOT}/workspace}"
NS3_DIR="${WORKSPACE_DIR}/ns-3.42"
NS3_REPO_URL="${NS3_REPO_URL:-https://gitlab.com/nsnam/ns-3-dev.git}"
NS3_REF="${NS3_REF:-ns-3.42}"

print_section "Fetch ns-3"
print_kv "Workspace" "${WORKSPACE_DIR}"
print_kv "Repository" "${NS3_REPO_URL}"
print_kv "Reference" "${NS3_REF}"
print_kv "Checkout path" "${NS3_DIR}"

mkdir -p "${WORKSPACE_DIR}"

if [[ -e "${NS3_DIR}" && ! -d "${NS3_DIR}/.git" ]]; then
  die "Refusing to reuse ${NS3_DIR}: not a git checkout"
fi

if [[ -d "${NS3_DIR}/.git" ]]; then
  print_info "Existing ns-3 checkout found. Reusing ${NS3_DIR}."
  exit 0
fi

print_step "Cloning ns-3"
git clone --depth 1 --branch "${NS3_REF}" "${NS3_REPO_URL}" "${NS3_DIR}"
print_info "Fetched ns-3 into ${NS3_DIR}."
