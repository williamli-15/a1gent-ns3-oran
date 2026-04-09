// oran-reporter-lte-ue-sinr.cc
#include "oran-reporter-lte-ue-sinr.h"
#include "oran-report-lte-ue-sinr.h"
#include "oran-e2-node-terminator.h"

#include <ns3/lte-ue-net-device.h>
#include <ns3/lte-ue-phy.h>
#include <ns3/log.h>
#include <ns3/simulator.h>
#include <ns3/node.h>
#include <ns3/net-device.h>
#include <ns3/double.h>
#include <ns3/uinteger.h>      // for UintegerValue
#include <ns3/nstime.h>        // for TimeValue

#include <cfloat>              // DBL_MAX
#include <limits>
#include <cmath>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("OranReporterLteUeSinr");
NS_OBJECT_ENSURE_REGISTERED (OranReporterLteUeSinr);

TypeId
OranReporterLteUeSinr::GetTypeId ()
{
  static TypeId tid = TypeId ("ns3::OranReporterLteUeSinr")
    .SetParent<OranReporter> ()
    .AddConstructor<OranReporterLteUeSinr> ();
  return tid;
}

OranReporterLteUeSinr::OranReporterLteUeSinr () = default;
OranReporterLteUeSinr::~OranReporterLteUeSinr () = default;

static inline double DbmToMw (double dbm) { return std::pow (10.0, dbm / 10.0); }
static inline double MwToDbm (double mw)  { return 10.0 * std::log10 (mw); }

void
OranReporterLteUeSinr::Activate()
{
  if (m_active) return;

  Ptr<OranE2NodeTerminator> term = GetTerminator();
  NS_ABORT_MSG_IF(term == nullptr, "UE SINR reporter needs a Terminator");

  Ptr<Node> node = term->GetNode();
  NS_ABORT_MSG_IF(!node, "UE SINR reporter needs a node");

  m_ueDev = nullptr;
  for (uint32_t i = 0; i < node->GetNDevices(); ++i)
  {
    Ptr<LteUeNetDevice> ue = node->GetDevice(i)->GetObject<LteUeNetDevice>();
    if (ue) { m_ueDev = ue; break; }
  }
  NS_ABORT_MSG_IF(!m_ueDev, "No LteUeNetDevice found on UE node");

  m_uePhy = m_ueDev->GetPhy();
  NS_ABORT_MSG_IF(!m_uePhy, "UE PHY is null");

  // reset window BEFORE triggers can fire
  m_sumSinrLin = 0.0; m_cntSinr = 0;
  m_sumRsrpMw  = 0.0; m_cntRsrp = 0;
  m_cellId = 0; m_rnti = 0;
  m_windowStart = Simulator::Now();

  ConnectTraces();          // hook first
  OranReporter::Activate(); // then activate (may schedule trigger)
}

void
OranReporterLteUeSinr::Deactivate ()
{
  if (!m_active) return;
  DisconnectTraces ();
  OranReporter::Deactivate ();
}

void
OranReporterLteUeSinr::ConnectTraces ()
{
  if (m_tracesConnected) return;

  m_uePhy->TraceConnectWithoutContext ("ReportCurrentCellRsrpSinr",
    MakeCallback (&OranReporterLteUeSinr::NotifyRsrpSinr, this));

  m_tracesConnected = true;
}

void
OranReporterLteUeSinr::DisconnectTraces ()
{
  if (!m_tracesConnected || !m_uePhy) return;

  m_uePhy->TraceDisconnectWithoutContext(
      "ReportCurrentCellRsrpSinr",
      MakeCallback(&OranReporterLteUeSinr::NotifyRsrpSinr, this));

  m_tracesConnected = false;
}

void
OranReporterLteUeSinr::DoDispose()
{
  DisconnectTraces();
  m_uePhy = nullptr;
  m_ueDev = nullptr;
  OranReporter::DoDispose();
}

void
OranReporterLteUeSinr::NotifyRsrpSinr(uint16_t cellId, uint16_t rnti,
                                      double rsrp, double sinr, uint8_t /*ccId*/)
{
  if (!m_active) return;

  if (rsrp == DBL_MAX || sinr == DBL_MAX) return;
  if (!std::isfinite(rsrp) || !std::isfinite(sinr)) return;
  if (rsrp <= 0.0 || sinr <= 0.0) return;

  // HO / RNTI change inside the same window -> reset accumulators to avoid mixing
  if ((m_cntSinr > 0 || m_cntRsrp > 0) &&
      (m_cellId != 0) &&
      (cellId != m_cellId || rnti != m_rnti))
  {
    NS_LOG_DEBUG("UE SINR reporter: reset window on cell/rnti change "
                 << " old=(" << m_cellId << "," << m_rnti << ")"
                 << " new=(" << cellId << "," << rnti << ")");
    m_sumSinrLin = 0.0; m_cntSinr = 0;
    m_sumRsrpMw  = 0.0; m_cntRsrp = 0;
  }

  m_cellId = cellId;
  m_rnti   = rnti;

  m_sumSinrLin += sinr;
  ++m_cntSinr;

  const double rsrpMw = rsrp * 1000.0;  // ns-3 trace gives RSRP in W
  m_sumRsrpMw += rsrpMw;
  ++m_cntRsrp;
}

std::vector<Ptr<OranReport>>
OranReporterLteUeSinr::GenerateReports()
{
  std::vector<Ptr<OranReport>> out;
  if (!m_active) return out;

  // No samples => no report (avoid writing 0 / -inf into DB)
  if (m_cntSinr == 0 || m_cntRsrp == 0 || m_cellId == 0 || m_rnti == 0)
  {
    m_windowStart = Simulator::Now();
    return out;
  }

  const Time now = Simulator::Now();
  const double avgSinrLin = m_sumSinrLin / m_cntSinr;
  const double avgRsrpMw  = m_sumRsrpMw  / m_cntRsrp;

  const double avgSinrDb  = 10.0 * std::log10(avgSinrLin);
  const double avgRsrpDbm = 10.0 * std::log10(avgRsrpMw); // mW -> dBm

  Ptr<OranReportLteUeSinr> rpt = CreateObject<OranReportLteUeSinr> ();
  rpt->SetAttribute ("ReporterE2NodeId", UintegerValue (m_terminator->GetE2NodeId ()));
  rpt->SetAttribute ("Time",             TimeValue (now));
  rpt->SetCellId (m_cellId);
  rpt->SetRnti   (m_rnti);
  rpt->SetSinrLinear (avgSinrLin);
  rpt->SetSinrDb     (avgSinrDb);
  rpt->SetRsrpMw     (avgRsrpMw);
  rpt->SetRsrpDbm    (avgRsrpDbm);

  NS_LOG_INFO("UE SINR rpt: e2=" << m_terminator->GetE2NodeId()
            << " cell=" << m_cellId
            << " rnti=" << m_rnti
            << " sinrLin=" << avgSinrLin
            << " sinrDb=" << avgSinrDb
            << " rsrpMw=" << avgRsrpMw
            << " rsrpDbm=" << avgRsrpDbm
            << " t=" << now.GetSeconds());

  out.push_back(rpt);

  // reset window
  m_sumSinrLin = 0.0; m_cntSinr = 0;
  m_sumRsrpMw  = 0.0; m_cntRsrp = 0;
  m_cellId = 0; m_rnti = 0;
  m_windowStart = now;
  return out;
}

} // namespace ns3
