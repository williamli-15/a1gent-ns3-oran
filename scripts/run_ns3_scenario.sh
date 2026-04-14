#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

show_usage() {
  cat <<'EOF'
Usage: ./scripts/run_ns3_scenario.sh [ns3-dir] [extra ns-3 args]

Reads experiments/configs/ns3-scenario.env by default.
Edit that file to change topology, traffic, database name, and run duration.
EOF
}

if [[ $# -gt 0 && ( "${1}" == "-h" || "${1}" == "--help" ) ]]; then
  show_usage
  exit 0
fi

SCENARIO_ENV_FILE="${A1GENT_NS3_ENV_FILE:-${REPO_ROOT}/experiments/configs/ns3-scenario.env}"
if [[ -f "${SCENARIO_ENV_FILE}" ]]; then
  echo "Loading scenario env: ${SCENARIO_ENV_FILE}"
  set -a
  source "${SCENARIO_ENV_FILE}"
  set +a
fi

NS3_DIR="${A1GENT_NS3_DIR:-${REPO_ROOT}/workspace/ns-3.42}"
if [[ $# -gt 0 && "${1}" != -* ]]; then
  NS3_DIR="$1"
  shift
fi
if [[ "${NS3_DIR}" != /* ]]; then
  NS3_DIR="${REPO_ROOT}/${NS3_DIR}"
fi

SCENARIO="${A1GENT_NS3_SCENARIO:-oran-lte-2-lte-ml-handover-example}"
USE_ORAN="${NS3_USE_ORAN:-1}"
HANDOVER_ALGORITHM="${NS3_HANDOVER_ALGORITHM:-ns3::A2A4RsrqHandoverAlgorithm}"
USE_HEX="${NS3_USE_HEX:-1}"
NUM_SITES="${NS3_NUM_SITES:-3}"
NUM_UES="${NS3_NUM_UES:-20}"
ISD_METERS="${NS3_ISD:-200}"
LM_QUERY_INTERVAL_S="${NS3_LM_QUERY_INTERVAL:-2}"
DB_FILE="${NS3_DB_FILE:-oran-repository.db}"

EMBB_BASE_RATE="${NS3_EMBB_BASE_RATE:-12Mbps}"
EMBB_BASE_ON="${NS3_EMBB_BASE_ON:-0.6}"
EMBB_BASE_OFF="${NS3_EMBB_BASE_OFF:-1.4}"
EMBB_HOT_RATE="${NS3_EMBB_HOT_RATE:-28Mbps}"
EMBB_HOT_ON="${NS3_EMBB_HOT_ON:-1.4}"
EMBB_HOT_OFF="${NS3_EMBB_HOT_OFF:-0.25}"
EMBB_HOT_MODULO="${NS3_EMBB_HOT_MODULO:-3}"
EMBB_HOT_OFFSET="${NS3_EMBB_HOT_OFFSET:-0}"

EMERG_START_S="${NS3_EMERG_START:-60}"
EMERG_DURATION_S="${NS3_EMERG_DURATION:-90}"
EMERG_HOT_MODULO="${NS3_EMERG_HOT_MODULO:-3}"
EMERG_HOT_OFFSET="${NS3_EMERG_HOT_OFFSET:-0}"
EMERG_BOOST_UL="${NS3_EMERG_BOOST_UL:-1}"

SIM_TIME_S="${NS3_SIM_TIME:-300}"
NS3_LOG_FILE="${NS3_LOG_FILE:-}"
EXTRA_ARGS=("$@")

: "${NS_LOG:=OranLmCommandBridge=level_info|prefix_time}"
export NS_LOG

echo
echo "================================================================================"
echo "Run ns-3 scenario"
echo "================================================================================"
echo "ns-3 directory: ${NS3_DIR}"
echo "Scenario: ${SCENARIO}"
echo "Database: ${DB_FILE}"
echo "Simulation time: ${SIM_TIME_S}s"
echo "Sites / UEs / inter-site distance (m): ${NUM_SITES} / ${NUM_UES} / ${ISD_METERS}"
echo "Command bridge query interval: ${LM_QUERY_INTERVAL_S}s"
echo "Emergency window: ${EMERG_START_S}s + ${EMERG_DURATION_S}s"
echo "Scenario settings come from experiments/configs/ns3-scenario.env"

if [[ ! -x "${NS3_DIR}/ns3" ]]; then
  echo "Error: ns-3 launcher not found at ${NS3_DIR}/ns3" >&2
  exit 1
fi

DB_PATH="${DB_FILE}"
if [[ "${DB_FILE}" != /* ]]; then
  DB_PATH="${NS3_DIR}/${DB_FILE}"
fi

echo
echo "Clearing previous command bridge and database artifacts..."
rm -f "${NS3_DIR}/commands.json" "${DB_PATH}"

RUN_ARGS=(
  "${SCENARIO}"

  # Core scenario settings
  "--use-oran=${USE_ORAN}"
  "--handover-algorithm=${HANDOVER_ALGORITHM}"
  "--use-hex=${USE_HEX}"
  "--num-sites=${NUM_SITES}"
  "--num-ues=${NUM_UES}"
  "--isd=${ISD_METERS}"
  "--lm-query-interval=${LM_QUERY_INTERVAL_S}"
  "--db-file=${DB_FILE}"

  # Baseline eMBB traffic
  "--embb-base-rate=${EMBB_BASE_RATE}"
  "--embb-base-on=${EMBB_BASE_ON}"
  "--embb-base-off=${EMBB_BASE_OFF}"
  "--embb-hot-rate=${EMBB_HOT_RATE}"
  "--embb-hot-on=${EMBB_HOT_ON}"
  "--embb-hot-off=${EMBB_HOT_OFF}"
  "--embb-hot-modulo=${EMBB_HOT_MODULO}"
  "--embb-hot-offset=${EMBB_HOT_OFFSET}"

  # Emergency traffic window
  "--emerg-start=${EMERG_START_S}"
  "--emerg-dur=${EMERG_DURATION_S}"
  "--emerg-hot-modulo=${EMERG_HOT_MODULO}"
  "--emerg-hot-offset=${EMERG_HOT_OFFSET}"
  "--emerg-boost-ul=${EMERG_BOOST_UL}"

  # Run duration
  "--sim-time=${SIM_TIME_S}"
)

if [[ ${#EXTRA_ARGS[@]} -gt 0 ]]; then
  RUN_ARGS+=("${EXTRA_ARGS[@]}")
fi

printf -v RUN_LINE '%s ' "${RUN_ARGS[@]}"
RUN_LINE="${RUN_LINE% }"

cd "${NS3_DIR}"
echo
echo "Launching ns-3..."
echo "./ns3 run \"${RUN_LINE}\""
if [[ -n "${NS3_LOG_FILE}" ]]; then
  echo "Streaming ns-3 output to ${NS3_LOG_FILE}"
  ./ns3 run "${RUN_LINE}" 2>&1 | tee "${NS3_LOG_FILE}"
else
  ./ns3 run "${RUN_LINE}"
fi
