# ns-3 O-RAN integration in `a1gent-ns3-oran`

ns-3 O-RAN sources for `a1gent-ns3-oran`.

## Upstream basis

- based on [`usnistgov/ns3-oran`](https://github.com/usnistgov/ns3-oran)
- upstream base commit: `d78da638bd85bc90256cddf927a7a9861c936a3b`

## Included

- `contrib/oran/`: O-RAN contribution module
- `src/lte/`: LTE overlay required by the A1gent-controlled workflow

## Local changes relative to upstream

- Modified upstream files:
  - `CMakeLists.txt`
  - `examples/oran-lte-2-lte-distance-handover-helper-example.cc`
  - `examples/oran-lte-2-lte-ml-handover-example.cc`
  - `model/oran-data-repository-sqlite.cc`
  - `model/oran-data-repository-sqlite.h`
  - `model/oran-data-repository.h`
  - `model/oran-near-rt-ric-e2terminator.cc`
  - `model/oran-reporter-apploss.cc`
  - `model/oran-reporter-apploss.h`
  - `model/oran-reporter-lte-ue-rsrp-rsrq.cc`
- Added bridge files:
  - `model/json.hpp`
  - `model/oran-lm-command-bridge.cc`
  - `model/oran-lm-command-bridge.h`
- Added report files:
  - `model/oran-report-lte-enb-mcs.cc`
  - `model/oran-report-lte-enb-mcs.h`
  - `model/oran-report-lte-enb-prb-utilization.cc`
  - `model/oran-report-lte-enb-prb-utilization.h`
  - `model/oran-report-lte-enb-scheduled-throughput.cc`
  - `model/oran-report-lte-enb-scheduled-throughput.h`
  - `model/oran-report-lte-enb-ue-count.cc`
  - `model/oran-report-lte-enb-ue-count.h`
  - `model/oran-report-lte-enb-ul-interf.cc`
  - `model/oran-report-lte-enb-ul-interf.h`
  - `model/oran-report-lte-ho-event.cc`
  - `model/oran-report-lte-ho-event.h`
  - `model/oran-report-lte-ue-dwell.cc`
  - `model/oran-report-lte-ue-dwell.h`
  - `model/oran-report-lte-ue-pdcp-throughput.cc`
  - `model/oran-report-lte-ue-pdcp-throughput.h`
  - `model/oran-report-lte-ue-prb.cc`
  - `model/oran-report-lte-ue-prb.h`
  - `model/oran-report-lte-ue-sinr.cc`
  - `model/oran-report-lte-ue-sinr.h`
- Added reporter files:
  - `model/oran-reporter-lte-enb-mcs.cc`
  - `model/oran-reporter-lte-enb-mcs.h`
  - `model/oran-reporter-lte-enb-prb-utilization.cc`
  - `model/oran-reporter-lte-enb-prb-utilization.h`
  - `model/oran-reporter-lte-enb-scheduled-throughput.cc`
  - `model/oran-reporter-lte-enb-scheduled-throughput.h`
  - `model/oran-reporter-lte-enb-ue-count.cc`
  - `model/oran-reporter-lte-enb-ue-count.h`
  - `model/oran-reporter-lte-enb-ul-interference.cc`
  - `model/oran-reporter-lte-enb-ul-interference.h`
  - `model/oran-reporter-lte-ho-and-dwell.cc`
  - `model/oran-reporter-lte-ho-and-dwell.h`
  - `model/oran-reporter-lte-ue-pdcp-throughput.cc`
  - `model/oran-reporter-lte-ue-pdcp-throughput.h`
  - `model/oran-reporter-lte-ue-prb.cc`
  - `model/oran-reporter-lte-ue-prb.h`
  - `model/oran-reporter-lte-ue-sinr.cc`
  - `model/oran-reporter-lte-ue-sinr.h`
- No other files under `contrib/oran/` differ from the upstream base commit.

## LTE overlay files

The current `src/lte/` overlay consists of:

- `src/lte/CMakeLists.txt`
- `src/lte/model/lte-ue-rrc.cc`
- `src/lte/model/cio-store.h`
- `src/lte/model/cio-store.cc`

These files add per-neighbor Cell Individual Offset (CIO) support to LTE UE measurement and event triggering.
The overlay is based on `ns-3.42`.
