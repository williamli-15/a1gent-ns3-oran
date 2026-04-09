#include "oran-reporter-lte-enb-prb-utilization.h"
#include "oran-report-lte-enb-prb-utilization.h"
#include "oran-e2-node-terminator.h"

#include <ns3/lte-enb-net-device.h>
#include <ns3/lte-enb-mac.h>
#include <ns3/lte-amc.h>          // <— for GetTbSizeFromMcs
#include <ns3/simulator.h>
#include <ns3/node.h>
#include <ns3/net-device.h>
#include <ns3/uinteger.h>
#include <ns3/log.h>

#include <algorithm>
#include <cmath>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("OranReporterLteEnbPrbUtilization");
NS_OBJECT_ENSURE_REGISTERED (OranReporterLteEnbPrbUtilization);

TypeId
OranReporterLteEnbPrbUtilization::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::OranReporterLteEnbPrbUtilization")
    .SetParent<OranReporter> ()
    .AddConstructor<OranReporterLteEnbPrbUtilization> ();
  return tid;
}

OranReporterLteEnbPrbUtilization::OranReporterLteEnbPrbUtilization ()
  : OranReporter ()
{
}

OranReporterLteEnbPrbUtilization::~OranReporterLteEnbPrbUtilization ()
{
}

// AMC returns BITS; scheduling traces give TB size in BYTES.
static inline uint16_t
DlTbSizeToPrbs (uint8_t mcs, uint32_t tbBytes, uint16_t bwPrb)
{
  static LteAmc amc;
  const uint32_t tbBits = tbBytes * 8u;
  for (uint16_t prb = 1; prb <= bwPrb; ++prb)
  {
    const uint32_t capBits =
      static_cast<uint32_t>(amc.GetDlTbSizeFromMcs(mcs, prb));
    if (capBits >= tbBits)
    {
      return prb;
    }
  }
  return bwPrb;
}

static inline uint16_t
UlTbSizeToPrbs (uint8_t mcs, uint32_t tbBytes, uint16_t bwPrb)
{
  static LteAmc amc;
  const uint32_t tbBits = tbBytes * 8u;
  for (uint16_t prb = 1; prb <= bwPrb; ++prb)
  {
    const uint32_t capBits =
      static_cast<uint32_t>(amc.GetUlTbSizeFromMcs(mcs, prb));
    if (capBits >= tbBits)
    {
      return prb;
    }
  }
  return bwPrb;
}


void
OranReporterLteEnbPrbUtilization::Activate()
{
  NS_LOG_FUNCTION(this);
  if (m_active)
  {
    return;
  }

  // The reporter must already be attached via its Terminator
  Ptr<OranE2NodeTerminator> term = GetTerminator();
  NS_ABORT_MSG_IF(term == nullptr, "Reporter must be attached to a Terminator before Activate");

  Ptr<Node> node = term->GetNode();
  NS_ABORT_MSG_IF(node == nullptr, "Terminator has no Node");

  // Find the eNB NetDevice on this node
  m_enbDev = nullptr;
  for (uint32_t i = 0; i < node->GetNDevices(); ++i)
  {
    Ptr<LteEnbNetDevice> enb = node->GetDevice(i)->GetObject<LteEnbNetDevice>();
    if (enb)
    {
      m_enbDev = enb;
      break;
    }
  }
  NS_ABORT_MSG_IF(!m_enbDev, "No LteEnbNetDevice found on node for PRB utilization reporter");

  // Cell ID & bandwidths (in PRBs)
  m_cellId = m_enbDev->GetCellId();
  UintegerValue bw;
  m_enbDev->GetAttribute("DlBandwidth", bw);
  m_dlBandwidthPrbs = bw.Get();
  m_enbDev->GetAttribute("UlBandwidth", bw);
  m_ulBandwidthPrbs = bw.Get();

  // eNB MAC
  m_enbMac = m_enbDev->GetMac();
  NS_ABORT_MSG_IF(!m_enbMac, "LteEnbMac not found");

  // Reset the accounting window *before* any trigger can fire
  m_windowStart = Simulator::Now();
  m_dlUsedByTti.clear();
  m_ulUsedByTti.clear();

  // Hook traces first (safe to do before base Activate)
  ConnectTraces();

  // Now let the base class activate (this may schedule triggers immediately)
  OranReporter::Activate();
}


void
OranReporterLteEnbPrbUtilization::Deactivate ()
{
  NS_LOG_FUNCTION (this);
  if (!m_active)
    {
      return;
    }

  DisconnectTraces ();

  OranReporter::Deactivate ();
}

void
OranReporterLteEnbPrbUtilization::ConnectTraces ()
{
  if (m_tracesConnected)
    {
      return;
    }

  // These trace names are standard in ns-3 LTE eNB MAC
  m_enbMac->TraceConnectWithoutContext ("DlScheduling",
    MakeCallback (&OranReporterLteEnbPrbUtilization::NotifyDlScheduling, this));

  m_enbMac->TraceConnectWithoutContext ("UlScheduling",
    MakeCallback (&OranReporterLteEnbPrbUtilization::NotifyUlScheduling, this));

  m_tracesConnected = true;
}

void
OranReporterLteEnbPrbUtilization::DisconnectTraces()
{
  if (!m_tracesConnected || !m_enbMac) return;

  m_enbMac->TraceDisconnectWithoutContext(
      "DlScheduling",
      MakeCallback(&OranReporterLteEnbPrbUtilization::NotifyDlScheduling, this));

  m_enbMac->TraceDisconnectWithoutContext(
      "UlScheduling",
      MakeCallback(&OranReporterLteEnbPrbUtilization::NotifyUlScheduling, this));

  m_tracesConnected = false;
}


void
OranReporterLteEnbPrbUtilization::DoDispose()
{
  DisconnectTraces();
  m_enbMac = nullptr;
  m_enbDev = nullptr;
  m_dlUsedByTti.clear();
  m_ulUsedByTti.clear();
  OranReporter::DoDispose();
}

void
OranReporterLteEnbPrbUtilization::NotifyDlScheduling (DlSchedulingCallbackInfo info)
{
  if (!m_active) return;

  // PRB estimate per TB via AMC inversion (bytes -> PRBs)
  const uint32_t prb0 = (info.sizeTb1 > 0)
                        ? DlTbSizeToPrbs(info.mcsTb1, static_cast<uint32_t>(info.sizeTb1),
                                         m_dlBandwidthPrbs)
                        : 0u;
  const uint32_t prb1 = (info.sizeTb2 > 0)
                        ? DlTbSizeToPrbs(info.mcsTb2, static_cast<uint32_t>(info.sizeTb2),
                                         m_dlBandwidthPrbs)
                        : 0u;
  const uint32_t prbs = std::max(prb0, prb1);

  // bucket by (frame, subframe, CC) and clamp to bandwidth per TTI
  TtiKey k{info.frameNo, info.subframeNo, info.componentCarrierId};
  auto& used = m_dlUsedByTti[k];
  used = std::min<uint32_t>(m_dlBandwidthPrbs, used + prbs);
}




void
OranReporterLteEnbPrbUtilization::NotifyUlScheduling (uint32_t frame,
                                                      uint32_t subframe,
                                                      uint16_t /*rnti*/,
                                                      uint8_t  mcs,
                                                      uint16_t tbsSize,
                                                      uint8_t  ccId)
{
  if (!m_active) return;

  const uint32_t prbs = UlTbSizeToPrbs(mcs, static_cast<uint32_t>(tbsSize), m_ulBandwidthPrbs);

  TtiKey k{frame, subframe, ccId};
  auto& used = m_ulUsedByTti[k];
  used = std::min<uint32_t>(m_ulBandwidthPrbs, used + prbs);
}


std::vector<Ptr<OranReport>>
OranReporterLteEnbPrbUtilization::GenerateReports()
{
  NS_LOG_FUNCTION(this);
  std::vector<Ptr<OranReport>> out;
  if (!m_active) return out;

  const Time now = Simulator::Now();
  const int64_t elapsedMs = std::max<int64_t>(1, (now - m_windowStart).GetMilliSeconds());
  const uint64_t ttis = static_cast<uint64_t>(elapsedMs);

  uint64_t dlUsed = 0, ulUsed = 0;
  for (const auto& kv : m_dlUsedByTti) dlUsed += kv.second;
  for (const auto& kv : m_ulUsedByTti) ulUsed += kv.second;

  const uint64_t dlTotal = static_cast<uint64_t>(m_dlBandwidthPrbs) * ttis;
  const uint64_t ulTotal = static_cast<uint64_t>(m_ulBandwidthPrbs) * ttis;

  const double dlUtil = dlTotal ? std::min(1.0, static_cast<double>(dlUsed) / dlTotal) : 0.0;
  const double ulUtil = ulTotal ? std::min(1.0, static_cast<double>(ulUsed) / ulTotal) : 0.0;

  Ptr<OranReportLteEnbPrbUtilization> rpt = CreateObject<OranReportLteEnbPrbUtilization>();
  rpt->SetAttribute("ReporterE2NodeId", UintegerValue(GetTerminator()->GetE2NodeId()));
  rpt->SetAttribute("Time",             TimeValue(now));
  rpt->SetCellId(m_cellId);
  rpt->SetDlUtilization(dlUtil);
  rpt->SetUlUtilization(ulUtil);

  // NEW
  rpt->SetWindowMs(static_cast<uint32_t>(elapsedMs));
  rpt->SetTtisObserved(static_cast<uint32_t>(ttis));

  NS_LOG_INFO("PRB rpt: cell=" << m_cellId
              << " dl=" << dlUtil
              << " ul=" << ulUtil
              << " winMs=" << elapsedMs
              << " ttis=" << ttis
              << " t=" << now.GetSeconds());

  out.push_back(rpt);

  // Reset window
  m_windowStart = now;
  m_dlUsedByTti.clear();
  m_ulUsedByTti.clear();

  return out;
}


} // namespace ns3