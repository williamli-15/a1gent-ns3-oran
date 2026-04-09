// oran-reporter-lte-ue-prb.h
#pragma once

#include "oran-reporter.h"
#include "ns3/lte-amc.h"
#include <map>

namespace ns3 {

class OranNearRtRic;
struct DlSchedulingCallbackInfo; // fwd from lte-enb-mac.h

class OranReporterLteUePrb : public OranReporter
{
public:
  static TypeId GetTypeId();
  OranReporterLteUePrb();
  ~OranReporterLteUePrb() override;

  void Activate() override;
  void Deactivate() override;
  void DoDispose() override;

  std::vector< Ptr<OranReport> > GenerateReports() override;

private:
  // trace wiring
  void EnsureConnected();
  void Disconnect();
  void SeedExistingUeMappings();
  static void DlSchedCb(OranReporterLteUePrb* self,
                        std::string path,
                        DlSchedulingCallbackInfo info);
  static void UlSchedCb(OranReporterLteUePrb* self,
                        std::string path,
                        uint32_t frameNo, uint32_t subframeNo,
                        uint16_t rnti, uint8_t mcs, uint16_t size, uint8_t ccId);

  // eNB RRC mapping (same as in your PDCP reporter)
  static void NotifyNewUeContextEnb(OranReporterLteUePrb* self,
                                    std::string context, uint16_t cellId, uint16_t rnti);
  void StoreUeManagerPath(const std::string& enbRrcContext, uint16_t cellId, uint16_t rnti);

  // helpers
  uint64_t MapUeToE2Id(uint16_t cellId, uint16_t rnti) const;
  uint16_t GuessCellIdFromRnti(uint16_t rnti) const;
  int      MinPrbForTb(bool dl, uint8_t mcs, uint32_t sizeBytes) const;

  struct Key {
    uint64_t ueid; uint16_t cellId;
    bool operator<(const Key& o) const {
      return (ueid<o.ueid) || (ueid==o.ueid && cellId<o.cellId);
    }
  };
  struct Agg { uint64_t dlPrb=0, ulPrb=0; };

  // (cellId,rnti) -> UeManager path (for bookeeping; also lets us recover cellId later)
  struct CellIdRnti { uint16_t cellId; uint16_t rnti; };
  struct CellIdRntiLess {
    bool operator()(const CellIdRnti& a, const CellIdRnti& b) const {
      return (a.cellId<b.cellId) || (a.cellId==b.cellId && a.rnti<b.rnti);
    }
  };
  std::map<CellIdRnti,std::string,CellIdRntiLess> m_ueManagerPathByCellIdRnti;

  std::map<Key,Agg> m_agg;
  Time              m_windowStart;
  bool              m_tracesConnected = false;

  Ptr<LteAmc>       m_amc;
  Ptr<OranNearRtRic> m_nearRtRic;
};

} // ns3
