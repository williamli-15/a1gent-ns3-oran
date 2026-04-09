// oran-reporter-lte-ho-and-dwell.h
#pragma once
#include "oran-reporter.h"
#include <ns3/ptr.h>
#include <ns3/simulator.h>
#include <ns3/nstime.h>
#include <map>
#include <vector>

namespace ns3 {

class OranNearRtRic;
class OranDataRepository;

class OranReporterLteHoAndDwell : public OranReporter
{
public:
  static TypeId GetTypeId();
  OranReporterLteHoAndDwell();

  void Activate() override;
  void Deactivate() override;
  void DoDispose() override;
  std::vector< Ptr<OranReport> > GenerateReports() override;

private:
  // Trace wiring
  void EnsureConnected();
  void Disconnect();

  // eNB RRC trace sinks
  static void ConnEstCb(OranReporterLteHoAndDwell* self, std::string ctx,
                        uint64_t imsi, uint16_t cellId, uint16_t rnti);

  static void HoStartCb(OranReporterLteHoAndDwell* self, std::string ctx,
                        uint64_t imsi, uint16_t srcCell, uint16_t rnti, uint16_t targetCell);

  static void HoEndOkCb(OranReporterLteHoAndDwell* self, std::string ctx,
                        uint64_t imsi, uint16_t cellId, uint16_t rnti);

  // Failures
  static void HoFailNoPreamble(OranReporterLteHoAndDwell* s, std::string ctx,
                               uint64_t imsi, uint16_t srcCell, uint16_t rnti);
  static void HoFailMaxRach(OranReporterLteHoAndDwell* s, std::string ctx,
                            uint64_t imsi, uint16_t srcCell, uint16_t rnti);
  static void HoFailLeaving(OranReporterLteHoAndDwell* s, std::string ctx,
                            uint64_t imsi, uint16_t srcCell, uint16_t rnti);
  static void HoFailJoining(OranReporterLteHoAndDwell* s, std::string ctx,
                            uint64_t imsi, uint16_t srcCell, uint16_t rnti);

  uint64_t MapUe(uint16_t cellId, uint16_t rnti, uint64_t imsi) const;

  struct EventRow {
    uint64_t imsi=0, ueid=0;
    uint16_t src=0, dst=0;
    std::string ev;
  };
  struct DwellState {
    uint64_t imsi=0, ueid=0;
    uint16_t currentCell=0;
    Time     lastChange;
  };

  // Pending attach cache used before MapUe becomes ready
  struct PendingAttach {
    uint64_t imsi = 0;
    uint16_t cellId = 0;
    uint16_t rnti = 0;
    Time     t;          // Event time, used to backfill lastChange
  };

  // State
  bool m_connected = false;
  Ptr<OranNearRtRic> m_nearRtRic;

  std::vector<EventRow> m_pendingEvents; // flushed each GenerateReports()
  std::map<uint64_t/*ueid*/, DwellState> m_dwell; // per UE
  std::map<uint64_t/*imsi*/, uint64_t/*ueid*/> m_imsi2ueid; // fallback

  // Pending attach state keyed by IMSI
  std::map<uint64_t/*imsi*/, PendingAttach> m_pendingAttach;

  // NEW: cache last HO Start so EndOk can recover srcCell when st.currentCell==0
  std::map<uint64_t /*imsi*/, std::pair<uint16_t /*src*/, uint16_t /*dst*/>> m_lastHoStart;
};

} // ns3
