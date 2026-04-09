#!/usr/bin/env bash

print_rule() {
  printf '%s\n' "================================================================================"
}

print_section() {
  printf '\n'
  print_rule
  printf '%s\n' "$1"
  print_rule
}

print_step() {
  printf '[step] %s\n' "$1"
}

print_info() {
  printf '[info] %s\n' "$1"
}

print_kv() {
  printf '  - %s: %s\n' "$1" "$2"
}

die() {
  printf '[error] %s\n' "$1" >&2
  exit 1
}

resolve_repo_path() {
  local repo_root="$1"
  local path="$2"
  if [[ "${path}" = /* ]]; then
    printf '%s\n' "${path}"
  else
    printf '%s\n' "${repo_root}/${path}"
  fi
}
