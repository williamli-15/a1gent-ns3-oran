#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
source "${SCRIPT_DIR}/common.sh"

load_env_file() {
  local env_file="${A1GENT_NS3_ENV_FILE:-${REPO_ROOT}/experiments/configs/ns3-scenario.env}"
  if [[ -f "${env_file}" ]]; then
    print_info "Loading scenario env: ${env_file}"
    set -a
    source "${env_file}"
    set +a
  fi
}

resolve_path() {
  local path="$1"
  resolve_repo_path "${REPO_ROOT}" "${path}"
}

main() {
  load_env_file

  local ns3_dir="${A1GENT_NS3_DIR:-${REPO_ROOT}/workspace/ns-3.42}"
  if [[ $# -gt 0 && "${1}" != -* ]]; then
    ns3_dir="$1"
    shift
  fi
  ns3_dir="$(resolve_path "${ns3_dir}")"

  # Core scenario settings
  local scenario="${A1GENT_NS3_SCENARIO:-oran-lte-2-lte-ml-handover-example}"
  local use_oran="${NS3_USE_ORAN:-1}"
  local handover_algorithm="${NS3_HANDOVER_ALGORITHM:-ns3::A2A4RsrqHandoverAlgorithm}"
  local use_hex="${NS3_USE_HEX:-1}"
  local num_sites="${NS3_NUM_SITES:-3}"
  local num_ues="${NS3_NUM_UES:-20}"
  local isd="${NS3_ISD:-200}"
  local lm_query_interval="${NS3_LM_QUERY_INTERVAL:-2}"
  local db_file="${NS3_DB_FILE:-oran-repository.db}"

  # Baseline traffic
  local embb_base_rate="${NS3_EMBB_BASE_RATE:-12Mbps}"
  local embb_base_on="${NS3_EMBB_BASE_ON:-0.6}"
  local embb_base_off="${NS3_EMBB_BASE_OFF:-1.4}"
  local embb_hot_rate="${NS3_EMBB_HOT_RATE:-28Mbps}"
  local embb_hot_on="${NS3_EMBB_HOT_ON:-1.4}"
  local embb_hot_off="${NS3_EMBB_HOT_OFF:-0.25}"
  local embb_hot_modulo="${NS3_EMBB_HOT_MODULO:-3}"
  local embb_hot_offset="${NS3_EMBB_HOT_OFFSET:-0}"

  # Emergency window
  local emerg_start="${NS3_EMERG_START:-60}"
  local emerg_duration="${NS3_EMERG_DURATION:-90}"
  local emerg_hot_modulo="${NS3_EMERG_HOT_MODULO:-3}"
  local emerg_hot_offset="${NS3_EMERG_HOT_OFFSET:-0}"
  local emerg_boost_ul="${NS3_EMERG_BOOST_UL:-1}"

  # Runtime
  local sim_time="${NS3_SIM_TIME:-300}"
  local ns3_log_file="${NS3_LOG_FILE:-}"
  local db_path
  local -a extra_args=("$@")
  local -a run_args

  : "${NS_LOG:=OranLmCommandBridge=level_info|prefix_time}"
  export NS_LOG

  print_section "Run ns-3 scenario"
  print_kv "ns-3 directory" "${ns3_dir}"
  print_kv "Scenario" "${scenario}"
  print_kv "Database" "${db_file}"
  print_kv "Simulation time" "${sim_time}s"
  print_kv "Sites / UEs / ISD" "${num_sites} / ${num_ues} / ${isd}"
  print_kv "Emergency window" "${emerg_start}s + ${emerg_duration}s"

  if [[ "${db_file}" = /* ]]; then
    db_path="${db_file}"
  else
    db_path="${ns3_dir}/${db_file}"
  fi

  if [[ ! -x "${ns3_dir}/ns3" ]]; then
    die "ns-3 launcher not found at ${ns3_dir}/ns3"
  fi

  print_step "Clear previous command bridge and database artifacts"
  rm -f "${ns3_dir}/commands.json" "${db_path}"

  run_args=(
    "${scenario}"

    # Core switches
    "--use-oran=${use_oran}"
    "--handover-algorithm=${handover_algorithm}"
    "--use-hex=${use_hex}"
    "--num-sites=${num_sites}"
    "--num-ues=${num_ues}"
    "--isd=${isd}"
    "--lm-query-interval=${lm_query_interval}"
    "--db-file=${db_file}"

    # Baseline traffic
    "--embb-base-rate=${embb_base_rate}"
    "--embb-base-on=${embb_base_on}"
    "--embb-base-off=${embb_base_off}"
    "--embb-hot-rate=${embb_hot_rate}"
    "--embb-hot-on=${embb_hot_on}"
    "--embb-hot-off=${embb_hot_off}"
    "--embb-hot-modulo=${embb_hot_modulo}"
    "--embb-hot-offset=${embb_hot_offset}"

    # Emergency traffic
    "--emerg-start=${emerg_start}"
    "--emerg-dur=${emerg_duration}"
    "--emerg-hot-modulo=${emerg_hot_modulo}"
    "--emerg-hot-offset=${emerg_hot_offset}"
    "--emerg-boost-ul=${emerg_boost_ul}"

    # Run duration
    "--sim-time=${sim_time}"
  )
  if [[ ${#extra_args[@]} -gt 0 ]]; then
    run_args+=("${extra_args[@]}")
  fi

  local run_line
  printf -v run_line '%s ' "${run_args[@]}"
  run_line="${run_line% }"

  cd "${ns3_dir}"
  print_step "Launch ns-3"
  print_info "./ns3 run \"${run_line}\""
  if [[ -n "${ns3_log_file}" ]]; then
    print_info "Streaming ns-3 output to ${ns3_log_file}"
    ./ns3 run "${run_line}" 2>&1 | tee "${ns3_log_file}"
  else
    ./ns3 run "${run_line}"
  fi
}

main "$@"
