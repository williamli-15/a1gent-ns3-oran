#pragma once
#include "oran-reporter.h"
#include "oran-near-rt-ric.h"      // required for MakePointerChecker
#include "oran-data-repository.h"  // for Ptr<OranDataRepository>
#include <unordered_map>
#include <map>
#include <vector>

namespace ns3 {

/**
 * Reporter: UE PDCP delivered throughput (per UE)
 *
 * Hooks PDCP Rx at:
 *  - UE side (DL delivered)
 *  - eNB side (UL delivered)
 * Aggregates per UE over the trigger window and emits Mbps.
 */
class OranReporterLteUePdcpThroughput : public OranReporter
{
public:
  static TypeId GetTypeId ();
  OranReporterLteUePdcpThroughput ();
  ~OranReporterLteUePdcpThroughput () override;

  // Mapper (instance)
  uint64_t MapUeToE2Id(uint16_t cellId, uint16_t rnti) const;

  void Activate () override;
  void Deactivate () override;

protected:
  void DoDispose () override;
  std::vector< Ptr<OranReport> > GenerateReports () override;

private:

  Ptr<OranNearRtRic> m_nearRtRic;  // injected RIC so we can reach Data()

  // --- trace hookup (pattern mirrors RadioBearerStatsConnector) ---
  void EnsureConnected ();
  void Disconnect ();

  static void NotifyNewUeContextEnb (OranReporterLteUePdcpThroughput* self,
                                     std::string context, uint16_t cellId, uint16_t rnti);
  static void CreatedDrbEnb         (OranReporterLteUePdcpThroughput* self,
                                     std::string context, uint64_t imsi,
                                     uint16_t cellId, uint16_t rnti, uint8_t lcid);
  static void CreatedDrbUe          (OranReporterLteUePdcpThroughput* self,
                                     std::string context, uint64_t imsi,
                                     uint16_t cellId, uint16_t rnti, uint8_t lcid);

  void StoreUeManagerPath (const std::string& enbRrcContext, uint16_t cellId, uint16_t rnti);
  void ConnectDrbEnb      (const std::string& enbUeManagerPath, uint16_t cellId, uint8_t lcid);
  void ConnectDrbUe       (const std::string& ueRrcContext,   uint16_t cellId, uint8_t lcid);

  // PDCP Rx callbacks (signatures match PDCP traces)
  static void UeDlRxPduCb(OranReporterLteUePdcpThroughput* self,
                          uint16_t cellId,
                          std::string path,
                          uint16_t rnti, uint8_t lcid,
                          uint32_t packetSize, uint64_t delay);
  static void EnbUlRxPduCb(OranReporterLteUePdcpThroughput* self,
                           uint16_t cellId,
                           std::string path,
                           uint16_t rnti, uint8_t lcid,
                           uint32_t packetSize, uint64_t delay);

  // byte accounting
  struct UeAgg {
    uint64_t dlBytes {0};
    uint64_t ulBytes {0};
    uint16_t cellId {0};
  };

  bool m_tracesConnected {false};
  Time m_windowStart;
  std::unordered_map<uint64_t/*ueid*/, UeAgg> m_agg;

  // Key for UeManager paths
  struct CellIdRnti {
    uint16_t cellId;
    uint16_t rnti;
    bool operator< (const CellIdRnti& other) const {
      return (cellId < other.cellId) || (cellId == other.cellId && rnti < other.rnti);
    }
  };
  std::map<CellIdRnti, std::string> m_ueManagerPathByCellIdRnti;

  uint16_t ResolveCellId(uint16_t rnti);
  std::vector<uint16_t> m_knownCellIds;
};

} // namespace ns3
