#include "oran-reporter-lte-enb-scheduled-throughput.h"
#include "oran-report-lte-enb-scheduled-throughput.h"
#include "oran-e2-node-terminator.h"

#include <ns3/log.h>
#include <ns3/node.h>
#include <ns3/net-device.h>
#include <ns3/simulator.h>
#include <ns3/uinteger.h>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("OranReporterLteEnbScheduledThroughput");
NS_OBJECT_ENSURE_REGISTERED (OranReporterLteEnbScheduledThroughput);

TypeId
OranReporterLteEnbScheduledThroughput::GetTypeId ()
{
  static TypeId tid =
    TypeId ("ns3::OranReporterLteEnbScheduledThroughput")
      .SetParent<OranReporter> ()
      .AddConstructor<OranReporterLteEnbScheduledThroughput> ();
  return tid;
}

OranReporterLteEnbScheduledThroughput::OranReporterLteEnbScheduledThroughput ()
  : OranReporter ()
{
  NS_LOG_FUNCTION (this);
}

OranReporterLteEnbScheduledThroughput::~OranReporterLteEnbScheduledThroughput ()
{
  NS_LOG_FUNCTION (this);
}

void
OranReporterLteEnbScheduledThroughput::DoDispose ()
{
  DisconnectTraces ();
  m_enbMac = nullptr;
  m_enbDev = nullptr;
  OranReporter::DoDispose ();
}

void
OranReporterLteEnbScheduledThroughput::Activate ()
{
  NS_LOG_FUNCTION (this);
  if (m_active) { return; }

  Ptr<OranE2NodeTerminator> term = GetTerminator ();
  NS_ABORT_MSG_IF (term == nullptr, "Reporter must be attached to a Terminator before Activate");

  Ptr<Node> node = term->GetNode ();
  NS_ABORT_MSG_IF (node == nullptr, "Terminator has no Node");

  // find LteEnbNetDevice on this node
  m_enbDev = nullptr;
  for (uint32_t i = 0; i < node->GetNDevices (); ++i)
  {
    Ptr<LteEnbNetDevice> enb = node->GetDevice (i)->GetObject<LteEnbNetDevice> ();
    if (enb) { m_enbDev = enb; break; }
  }
  NS_ABORT_MSG_IF (!m_enbDev, "No LteEnbNetDevice found on node for scheduled throughput reporter");

  m_cellId = m_enbDev->GetCellId ();
  m_enbMac = m_enbDev->GetMac ();
  NS_ABORT_MSG_IF (!m_enbMac, "LteEnbMac not found");

  // reset window counters
  m_windowStart = Simulator::Now ();
  m_dlBytes = 0;
  m_ulBytes = 0;

  // hook traces then activate base (which may schedule the first trigger)
  ConnectTraces ();
  OranReporter::Activate ();
}

void
OranReporterLteEnbScheduledThroughput::Deactivate ()
{
  NS_LOG_FUNCTION (this);
  if (!m_active) { return; }
  DisconnectTraces ();
  OranReporter::Deactivate ();
}

void
OranReporterLteEnbScheduledThroughput::ConnectTraces ()
{
  if (m_tracesConnected) { return; }
  // Same trace names as MacStatsCalculator uses
  m_enbMac->TraceConnectWithoutContext (
      "DlScheduling",
      MakeCallback (&OranReporterLteEnbScheduledThroughput::NotifyDlScheduling, this));
  m_enbMac->TraceConnectWithoutContext (
      "UlScheduling",
      MakeCallback (&OranReporterLteEnbScheduledThroughput::NotifyUlScheduling, this));
  m_tracesConnected = true;
}

void
OranReporterLteEnbScheduledThroughput::DisconnectTraces ()
{
  if (!m_tracesConnected || !m_enbMac) { return; }
  m_enbMac->TraceDisconnectWithoutContext (
      "DlScheduling",
      MakeCallback (&OranReporterLteEnbScheduledThroughput::NotifyDlScheduling, this));
  m_enbMac->TraceDisconnectWithoutContext (
      "UlScheduling",
      MakeCallback (&OranReporterLteEnbScheduledThroughput::NotifyUlScheduling, this));
  m_tracesConnected = false;
}

void
OranReporterLteEnbScheduledThroughput::NotifyDlScheduling (DlSchedulingCallbackInfo info)
{
  if (!m_active) { return; }
  // Each Tb size is already bytes per TTI (scheduler-visible TB)
  m_dlBytes += static_cast<uint64_t> (info.sizeTb1);
  m_dlBytes += static_cast<uint64_t> (info.sizeTb2);
}

void
OranReporterLteEnbScheduledThroughput::NotifyUlScheduling (uint32_t /*frameNo*/,
                                                           uint32_t /*subframeNo*/,
                                                           uint16_t /*rnti*/,
                                                           uint8_t  /*mcs*/,
                                                           uint16_t sizeBytes,
                                                           uint8_t  /*componentCarrierId*/)
{
  if (!m_active) { return; }
  m_ulBytes += static_cast<uint64_t> (sizeBytes);
}

std::vector<Ptr<OranReport>>
OranReporterLteEnbScheduledThroughput::GenerateReports ()
{
  NS_LOG_FUNCTION (this);
  std::vector<Ptr<OranReport>> out;
  if (!m_active) { return out; }

  const Time now = Simulator::Now ();
  const int64_t windowMs = std::max<int64_t> (1, (now - m_windowStart).GetMilliSeconds ());
  const double seconds = static_cast<double> (windowMs) / 1000.0;

  const double dlMbps = seconds > 0.0 ? (static_cast<double>(m_dlBytes) * 8.0) / seconds / 1e6 : 0.0;
  const double ulMbps = seconds > 0.0 ? (static_cast<double>(m_ulBytes) * 8.0) / seconds / 1e6 : 0.0;

  Ptr<OranReportLteEnbScheduledThroughput> rpt = CreateObject<OranReportLteEnbScheduledThroughput> ();
  rpt->SetAttribute ("ReporterE2NodeId", UintegerValue (GetTerminator ()->GetE2NodeId ()));
  rpt->SetAttribute ("Time",             TimeValue (now));
  rpt->SetCellId (m_cellId);
  rpt->SetDlMbps (dlMbps);
  rpt->SetUlMbps (ulMbps);
  rpt->SetWindowMs (static_cast<uint32_t>(windowMs));
  rpt->SetTtisObserved (static_cast<uint32_t>(windowMs)); // LTE: 1 TTI per ms

  out.push_back (rpt);

  // reset window
  m_windowStart = now;
  m_dlBytes = 0;
  m_ulBytes = 0;
  return out;
}

} // namespace ns3
