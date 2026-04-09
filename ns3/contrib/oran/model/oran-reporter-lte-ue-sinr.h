// oran-reporter-lte-ue-sinr.h
#pragma once
#include "oran-reporter.h"
#include "ns3/nstime.h"

namespace ns3 {
class LteUeNetDevice;
class LteUePhy;

class OranReporterLteUeSinr : public OranReporter
{
public:
  static TypeId GetTypeId ();
  OranReporterLteUeSinr ();
  ~OranReporterLteUeSinr () override;

  void Activate () override;
  void Deactivate () override;

protected:
  std::vector<Ptr<OranReport>> GenerateReports () override;

private:
  void ConnectTraces ();
  void DisconnectTraces ();
  void DoDispose() override;

  void NotifyRsrpSinr (uint16_t cellId, uint16_t rnti,
                       double rsrp, double sinr, uint8_t ccId);

  Ptr<LteUeNetDevice> m_ueDev;
  Ptr<LteUePhy>       m_uePhy;

  uint16_t m_cellId {0};
  uint16_t m_rnti   {0};

  double   m_sumSinrLin {0.0};
  uint32_t m_cntSinr    {0};

  double   m_sumRsrpMw  {0.0};
  uint32_t m_cntRsrp    {0};

  Time     m_windowStart {Seconds (0)};
  bool     m_tracesConnected {false};
};
} // ns3
