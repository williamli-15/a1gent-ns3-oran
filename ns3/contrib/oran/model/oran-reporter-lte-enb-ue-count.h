#pragma once
#include "oran-reporter.h"
#include <map>
#include <unordered_set>

namespace ns3 {
class OranNearRtRic;

class OranReporterLteEnbUeCount : public OranReporter
{
public:
  static TypeId GetTypeId();
  OranReporterLteEnbUeCount();

  void Activate() override;
  void Deactivate() override;
  void DoDispose() override;
  std::vector< Ptr<OranReport> > GenerateReports() override;

private:
  void EnsureConnected();
  void Disconnect();

  // RRC sinks
  static void ConnEstCb(OranReporterLteEnbUeCount* self, std::string,
                        uint64_t imsi, uint16_t cellId, uint16_t rnti);
  static void HoEndOkCb(OranReporterLteEnbUeCount* self, std::string,
                        uint64_t imsi, uint16_t cellId, uint16_t rnti);

  static void HoStartCb(OranReporterLteEnbUeCount* self, std::string,
                        uint64_t imsi, uint16_t srcCell, uint16_t rnti, uint16_t dstCell);

  // No mapping needed for counts; NearRtRic may remain wired but unused
  bool m_connected = false;
  Ptr<OranNearRtRic> m_nearRtRic;

  // RNTI-based state
  std::map<uint16_t /*cellId*/, std::unordered_set<uint16_t /*rnti*/>> m_cellRntis;
  std::map<uint16_t /*rnti*/,   uint16_t /*cellId*/>                   m_rntiCell;

  uint16_t m_myCellId {0};  // this reporter’s cellId (primary cell of its eNB)
};
}
