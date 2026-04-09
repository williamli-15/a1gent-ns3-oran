#pragma once
#include "oran-reporter.h"
#include "ns3/nstime.h"
#include "ns3/lte-enb-mac.h"   // DlSchedulingCallbackInfo

#include <unordered_map>       // ADD

namespace ns3 {

class LteEnbNetDevice;
class LteEnbMac;

class OranReporterLteEnbPrbUtilization : public OranReporter
{
public:
  static TypeId GetTypeId (void);
  OranReporterLteEnbPrbUtilization ();
  ~OranReporterLteEnbPrbUtilization () override;

  void Activate () override;
  void Deactivate () override;

protected:
  std::vector<Ptr<OranReport>> GenerateReports () override;

private:
  // ---- trace sinks (signatures must match ns-3.42 LTE) ----
  // ns-3.42 DL trace uses a struct
  void NotifyDlScheduling (DlSchedulingCallbackInfo info);  // by value
  // ns-3.42 UL trace is positional: frame, subframe, rnti, mcs, tbsSize, ccId
  void NotifyUlScheduling (uint32_t frame, uint32_t subframe, uint16_t rnti,
                           uint8_t mcs, uint16_t tbsSize, uint8_t ccId);

  void ConnectTraces ();
  void DisconnectTraces ();
  void DoDispose() override;   // <— ADD

  Ptr<LteEnbNetDevice> m_enbDev;
  Ptr<LteEnbMac>       m_enbMac;

  uint16_t m_cellId {0};
  uint16_t m_dlBandwidthPrbs {0};
  uint16_t m_ulBandwidthPrbs {0};

  // ---- NEW: per-TTI PRB accounting to avoid over-counting ----
  struct TtiKey
  {
    uint32_t frame;
    uint32_t subframe;
    uint8_t  cc;
  };
  struct TtiKeyHash
  {
    size_t operator()(const TtiKey& k) const noexcept
    {
      // cheap mix; good enough for small maps
      return (static_cast<size_t>(k.frame) * 131u + k.subframe) * 31u + k.cc;
    }
  };
  struct TtiKeyEq
  {
    bool operator()(const TtiKey& a, const TtiKey& b) const noexcept
    {
      return a.frame == b.frame && a.subframe == b.subframe && a.cc == b.cc;
    }
  };

  std::unordered_map<TtiKey, uint32_t, TtiKeyHash, TtiKeyEq> m_dlUsedByTti;
  std::unordered_map<TtiKey, uint32_t, TtiKeyHash, TtiKeyEq> m_ulUsedByTti;

  Time     m_windowStart {Seconds(0)};
  bool     m_tracesConnected {false};
};

} // namespace ns3




